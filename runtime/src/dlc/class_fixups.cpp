// Layout fixes for the script properties of Azura's DLC script patch. See class_fixups.h.

#include "class_fixups.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace azura::dlc {
namespace {

// Guest addresses and object layouts (Unreal Engine 3, big-endian; see docs/reverse-engineering.md).
constexpr uint32_t kGNames = 0x8343D148;       // TArray<FNameEntry*>
constexpr uint32_t kObjectHash = 0x83473754;   // TArray<UObject*>: heads of the by-name hash chains
constexpr uint32_t kObjectHashNext = 4;
constexpr uint32_t kObjectOuter = 40;
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kObjectClass = 52;
constexpr uint32_t kFieldNext = 60;
constexpr uint32_t kStructSuper = 64;
constexpr uint32_t kStructChildren = 68;
constexpr uint32_t kStructPropertiesSize = 72;
constexpr uint32_t kPropertyElementSize = 68;
constexpr uint32_t kPropertyOffset = 84;
constexpr uint32_t kBoolPropertyBitMask = 104;
constexpr uint32_t kArrayPropertyInner = 104;
constexpr uint32_t kClassDefaultObject = 236;
constexpr uint32_t kArraySize = 12;  // TArray: data, count, capacity

// Return addresses of UStruct::Serialize's preloads of the struct's fields (Children and the
// fields that follow), in the disc build.
constexpr uint32_t kStructSerializePreloadChildren = 0x822BF9E4;
constexpr uint32_t kStructSerializePreloadField = 0x822BF564;

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

bool IsGuestPointer(uint32_t address) {
  return address >= 0x40000000u && address < 0xA0000000u;
}

std::string NameText(const uint8_t* base, uint32_t index) {
  const uint32_t data = LoadU32(base, kGNames);
  if (!data || index >= LoadU32(base, kGNames + 4)) return {};
  const uint32_t entry = LoadU32(base, data + index * 4);
  if (!IsGuestPointer(entry)) return {};
  const bool wide = (LoadU32(base, entry) & 1) != 0;
  std::string text;
  for (uint32_t i = 0; i < 128; ++i) {
    const char c = wide ? static_cast<char>(base[entry + 8 + i * 2 + 1]) : static_cast<char>(base[entry + 8 + i]);
    if (c == 0) break;
    text.push_back(c);
  }
  return text;
}

std::string ObjectName(const uint8_t* base, uint32_t object) {
  return object ? NameText(base, LoadU32(base, object + kObjectName)) : std::string();
}

std::string ClassName(const uint8_t* base, uint32_t object) {
  return ObjectName(base, LoadU32(base, object + kObjectClass));
}

// Every object in the by-name hash chains.
std::vector<uint32_t> AllObjects(const uint8_t* base) {
  std::vector<uint32_t> objects;
  const uint32_t heads = LoadU32(base, kObjectHash);
  const uint32_t count = std::min<uint32_t>(LoadU32(base, kObjectHash + 4), 0x100000);
  std::unordered_set<uint32_t> seen;
  for (uint32_t h = 0; IsGuestPointer(heads) && h < count; ++h) {
    for (uint32_t object = LoadU32(base, heads + h * 4); IsGuestPointer(object);
         object = LoadU32(base, object + kObjectHashNext)) {
      if (!seen.insert(object).second) break;  // chains share their tails
      objects.push_back(object);
    }
  }
  return objects;
}

// The objects the fixes need, looked up once.
struct Objects {
  std::unordered_map<std::string, uint32_t> classes;  // GearGame classes by name
  std::vector<uint32_t> all_classes;

  uint32_t Class(const std::string& name) const {
    const auto it = classes.find(name);
    return it == classes.end() ? 0 : it->second;
  }
};

Objects FindObjects(const uint8_t* base, const std::vector<uint32_t>& objects) {
  Objects found;
  for (const uint32_t object : objects) {
    if (ClassName(base, object) != "Class") continue;
    found.all_classes.push_back(object);
    if (ObjectName(base, LoadU32(base, object + kObjectOuter)) == "GearGame") {
      found.classes.emplace(ObjectName(base, object), object);
    }
  }
  return found;
}

// The property named `name` whose outer is `owner`, linked into its fields or not.
uint32_t FindProperty(const uint8_t* base, const std::vector<uint32_t>& objects, uint32_t owner,
                      const std::string& name) {
  for (const uint32_t object : objects) {
    if (LoadU32(base, object + kObjectOuter) == owner && ObjectName(base, object) == name) return object;
  }
  return 0;
}

bool IsSubclassOf(const uint8_t* base, uint32_t cls, uint32_t parent) {
  for (uint32_t c = LoadU32(base, cls + kStructSuper); IsGuestPointer(c); c = LoadU32(base, c + kStructSuper)) {
    if (c == parent) return true;
  }
  return false;
}

// Gives bool `property` a bit of a bit field word of `cls` or its parents that no bool uses.
bool PlaceBool(uint8_t* base, uint32_t property, uint32_t cls, const char* what) {
  // Bit field words of the class chain and the bits their bools use.
  std::unordered_map<uint32_t, uint32_t> used;
  for (uint32_t c = cls; IsGuestPointer(c); c = LoadU32(base, c + kStructSuper)) {
    for (uint32_t field = LoadU32(base, c + kStructChildren); IsGuestPointer(field);
         field = LoadU32(base, field + kFieldNext)) {
      if (field == property || ClassName(base, field) != "BoolProperty") continue;
      used[LoadU32(base, field + kPropertyOffset)] |= LoadU32(base, field + kBoolPropertyBitMask);
    }
  }
  // The last word of the chain's bit fields with a free bit: its free bits are the padding of the
  // native bit field it mirrors.
  uint32_t word = 0;
  for (const auto& [offset, mask] : used) {
    if (mask != 0xFFFFFFFFu && offset > word) word = offset;
  }
  if (!word) {
    REXLOG_WARN("[Azura DLC] {}: no bit field word with a free bit; left unplaced", what);
    return false;
  }
  const uint32_t free = ~used[word];
  const uint32_t bit = free & (~free + 1);  // lowest free bit
  StoreU32(base, property + kPropertyOffset, word);
  StoreU32(base, property + kBoolPropertyBitMask, bit);
  StoreU32(base, property + kPropertyElementSize, 4);
  REXLOG_INFO("[Azura DLC] {} placed at +{} bit {:08X}", what, word, bit);
  return true;
}

// Puts `property` (`size` bytes) at the end of `cls`, which grows by it. Only within the 16-byte
// alignment of the class's size: the engine copies a new object's properties from the class default
// object, which was allocated with the old size, so the growth must stay inside that allocation's
// alignment padding (zeroed here, so copies start empty). Refuses when a loaded subclass already has
// fields where the property goes.
bool PlaceAtEnd(uint8_t* base, uint32_t property, uint32_t cls, uint32_t size, const char* what) {
  const uint32_t end = (LoadU32(base, cls + kStructPropertiesSize) + 3) & ~3u;
  if ((end & ~15u) != ((end + size - 1) & ~15u)) {
    REXLOG_WARN("[Azura DLC] {}: {} bytes after +{} leave the class's alignment; left unplaced", what, size, end);
    return false;
  }
  for (const uint32_t other : FindObjects(base, AllObjects(base)).all_classes) {
    if (!IsSubclassOf(base, other, cls)) continue;
    REXLOG_WARN("[Azura DLC] {}: {} extends {} and is already loaded; left unplaced", what, ObjectName(base, other),
                ObjectName(base, cls));
    return false;
  }
  if (const uint32_t defaults = LoadU32(base, cls + kClassDefaultObject); IsGuestPointer(defaults)) {
    for (uint32_t at = end; at < end + size; at += 4) StoreU32(base, defaults + at, 0);
  }
  StoreU32(base, property + kPropertyOffset, end);
  StoreU32(base, property + kPropertyElementSize, size);
  StoreU32(base, cls + kStructPropertiesSize, end + size);
  REXLOG_INFO("[Azura DLC] {} placed at +{}; {} grows to {} bytes", what, end, ObjectName(base, cls), end + size);
  return true;
}

// Gives array `property` its sizes (a TArray, and an inner element of 4 bytes) and no place of its
// own: BindShodTargets points it at the satellite camera's store whenever the camera uses it.
bool PlaceAway(uint8_t* base, uint32_t property, const char* what) {
  const uint32_t inner = LoadU32(base, property + kArrayPropertyInner);
  if (!IsGuestPointer(inner)) {
    REXLOG_WARN("[Azura DLC] {}: no inner property; left unplaced", what);
    return false;
  }
  StoreU32(base, inner + kPropertyElementSize, 4);
  StoreU32(base, property + kPropertyElementSize, kArraySize);
  StoreU32(base, property + kPropertyOffset, 0);
  REXLOG_INFO("[Azura DLC] {} waits for the satellite camera", what);
  return true;
}

// The property named `name` of `cls` or its parents, and the class that declares it, or 0.
uint32_t FindField(const uint8_t* base, uint32_t cls, const std::string& name, const char* owner = nullptr) {
  for (uint32_t c = cls; IsGuestPointer(c); c = LoadU32(base, c + kStructSuper)) {
    if (owner && ObjectName(base, c) != owner) continue;
    for (uint32_t field = LoadU32(base, c + kStructChildren); IsGuestPointer(field);
         field = LoadU32(base, field + kFieldNext)) {
      if (ObjectName(base, field) == name && ClassName(base, field).ends_with("Property")) return field;
    }
  }
  return 0;
}

// A class's linked lists of properties, as UStruct::Link builds them (each list's head in the
// class, and where each property keeps the next one): the properties holding object references
// (RefLink, for the garbage collector), all properties (PropertyLink, for copying and
// serialization) and those that need constructing (ConstructorLink).
constexpr uint32_t kPropertyLists[][2] = {{92, 96}, {96, 88}, {100, 92}};

// Takes GearGRI.SHODTargets out of `cls`'s property lists when `cls` is GearGRI or a subclass. The
// engine copies, constructs and destroys the arrays of those lists at their offset whatever their
// ArrayDim, so the property, which has no place in GearGRI, must be in none of them; script reaches
// it through its offset only. A subclass's lists end in its parent's, as they were when it linked.
void UnlinkShodTargets(uint8_t* base, uint32_t cls) {
  const uint32_t property = FindField(base, cls, "SHODTargets", "GearGRI");
  if (!property) return;
  for (const auto& [head, next] : kPropertyLists) {
    uint32_t link = cls + head;
    for (int n = 0; n < 4096; ++n) {
      const uint32_t p = LoadU32(base, link);
      if (!IsGuestPointer(p)) break;
      if (p == property) {
        StoreU32(base, link, LoadU32(base, property + next));
        REXLOG_INFO("[Azura DLC] GearGRI.SHODTargets taken out of {}'s list at +{}", ObjectName(base, cls), head);
        break;
      }
      link = p + next;
    }
  }
}

// The patch's properties and their classes, as the linker finishes preloading them. A class lays
// itself out at the end of its own preload, after its fields', and again when a new function of
// it is preloaded later; so a property is placed once both it and its class are done, and its
// place (and its class's size) is restored after every later preload of something of that class.
struct Placement {
  uint32_t owner = 0;
  uint32_t offset = 0;
  uint32_t element_size = 0;
  uint32_t bit_mask = 0;     // bools
  uint32_t class_size = 0;   // the owner's size with the property, when it grew
};
struct Pending {
  std::unordered_set<uint32_t> properties;   // preloaded, not placed yet
  std::unordered_set<uint32_t> classes_done;  // owners whose preload has finished
  std::unordered_map<uint32_t, Placement> placed;
};
std::mutex g_pending_mutex;
Pending g_pending;

// Places `property`; returns where it went, or an empty placement (owner 0) when it could not.
Placement Place(uint8_t* base, uint32_t property, uint32_t owner) {
  const std::string name = ObjectName(base, property);
  bool ok = false;
  if (name == "bShieldDispersed") {
    ok = PlaceBool(base, property, owner, "GearPawn_LocustGeneralRaamBase.bShieldDispersed");
  } else if (name == "TaccomMesh") {
    ok = PlaceAtEnd(base, property, owner, 4, "Vehicle_Reaver_Base.TaccomMesh");
  } else if (name == "SHODTargets") {
    ok = PlaceAway(base, property, "GearGRI.SHODTargets");
  }
  if (!ok) return {};
  Placement placement{owner, LoadU32(base, property + kPropertyOffset), LoadU32(base, property + kPropertyElementSize), 0, 0};
  if (name == "bShieldDispersed") placement.bit_mask = LoadU32(base, property + kBoolPropertyBitMask);
  if (name == "TaccomMesh") placement.class_size = LoadU32(base, owner + kStructPropertiesSize);
  return placement;
}

// Puts a placed property back where it was placed, after its class laid itself out again.
void Restore(uint8_t* base, uint32_t property, const Placement& placement) {
  if (LoadU32(base, property + kPropertyOffset) != placement.offset) {
    REXLOG_INFO("[Azura DLC] {}.{} laid out again; put back at +{}", ObjectName(base, placement.owner),
                ObjectName(base, property), placement.offset);
  }
  StoreU32(base, property + kPropertyOffset, placement.offset);
  StoreU32(base, property + kPropertyElementSize, placement.element_size);
  if (placement.bit_mask) StoreU32(base, property + kBoolPropertyBitMask, placement.bit_mask);
  if (placement.class_size && LoadU32(base, placement.owner + kStructPropertiesSize) < placement.class_size) {
    StoreU32(base, placement.owner + kStructPropertiesSize, placement.class_size);
  }
}

// The class each patch property belongs to.
const char* OwnerOf(const std::string& property) {
  if (property == "bShieldDispersed") return "GearPawn_LocustGeneralRaamBase";
  if (property == "TaccomMesh") return "Vehicle_Reaver_Base";
  if (property == "SHODTargets") return "GearGRI";
  return nullptr;
}

bool IsOwner(const std::string& name) {
  return name == "GearPawn_LocustGeneralRaamBase" || name == "Vehicle_Reaver_Base" || name == "GearGRI";
}

// PawnBlockingVolume (the patch's class) and where its objects keep AllowedClasses, once loaded.
std::atomic<uint32_t> g_pawn_blocking_volume{0};
std::atomic<uint32_t> g_allowed_classes_offset{0};

void NotePawnBlockingVolume(uint8_t* base, uint32_t cls) {
  const uint32_t allowed = FindField(base, cls, "AllowedClasses", "PawnBlockingVolume");
  if (!allowed) return;
  g_allowed_classes_offset.store(LoadU32(base, allowed + kPropertyOffset));
  g_pawn_blocking_volume.store(cls);
  REXLOG_INFO("[Azura DLC] PawnBlockingVolume lets its AllowedClasses through (+{})", g_allowed_classes_offset.load());
}

bool IsChildOf(const uint8_t* base, uint32_t cls, uint32_t parent) {
  for (int depth = 0; IsGuestPointer(cls) && depth < 32; cls = LoadU32(base, cls + kStructSuper), ++depth) {
    if (cls == parent) return true;
  }
  return false;
}

}  // namespace

void OnPreloaded(uint8_t* base, uint32_t object) {
  if (!IsGuestPointer(object)) return;
  // GearGRI or a subclass linked (or GearGRI linked again for a new function of it).
  if (ClassName(base, object) == "Class") {
    if (ObjectName(base, object) == "PawnBlockingVolume") NotePawnBlockingVolume(base, object);
    UnlinkShodTargets(base, object);
  } else if (ObjectName(base, LoadU32(base, object + kObjectOuter)) == "GearGRI") {
    UnlinkShodTargets(base, LoadU32(base, object + kObjectOuter));
  }
  const std::string name = ObjectName(base, object);
  const uint32_t outer = LoadU32(base, object + kObjectOuter);
  const bool is_owner = IsOwner(name) && ClassName(base, object) == "Class";
  const bool in_owner = IsGuestPointer(outer) && IsOwner(ObjectName(base, outer));
  if (!is_owner && !in_owner) return;
  std::lock_guard lock(g_pending_mutex);
  Pending& p = g_pending;
  if (is_owner) p.classes_done.insert(object);
  const char* owner_name = OwnerOf(name);
  if (owner_name && ObjectName(base, outer) == owner_name && !p.placed.count(object)) p.properties.insert(object);
  for (auto it = p.properties.begin(); it != p.properties.end();) {
    const uint32_t owner = LoadU32(base, *it + kObjectOuter);
    if (!p.classes_done.count(owner)) {
      ++it;
      continue;
    }
    const Placement placement = Place(base, *it, owner);
    if (placement.owner) p.placed[*it] = placement;
    it = p.properties.erase(it);
  }
  // Something of a class with a placed property was preloaded: the class may have laid itself out.
  for (const auto& [property, placement] : p.placed) {
    if (placement.owner == object || placement.owner == outer) Restore(base, property, placement);
  }
}

namespace {

// What BindShodTargets needs of a camera class, found the first time the class runs it.
struct CameraLayout {
  uint32_t store = 0;       // offset of SHODTargetStore in the camera
  uint32_t world_info = 0;  // offset of Actor.WorldInfo
  bool valid = false;
};
std::mutex g_bind_mutex;
std::unordered_map<uint32_t, CameraLayout> g_camera_layouts;
std::unordered_map<uint32_t, uint32_t> g_gri_offsets;  // WorldInfo class -> offset of GRI
uint32_t g_shod_targets = 0;                            // the GearGRI.SHODTargets property

}  // namespace

void BindShodTargets(uint8_t* base, uint32_t camera) {
  if (!IsGuestPointer(camera)) return;
  std::lock_guard lock(g_bind_mutex);
  const uint32_t camera_class = LoadU32(base, camera + kObjectClass);
  auto it = g_camera_layouts.find(camera_class);
  if (it == g_camera_layouts.end()) {
    CameraLayout layout;
    const uint32_t store = FindField(base, camera_class, "SHODTargetStore");
    const uint32_t world_info = FindField(base, camera_class, "WorldInfo");
    layout.valid = store && world_info;
    if (layout.valid) {
      layout.store = LoadU32(base, store + kPropertyOffset);
      layout.world_info = LoadU32(base, world_info + kPropertyOffset);
    }
    REXLOG_INFO("[Azura DLC] {}: SHODTargetStore at +{}{}", ObjectName(base, camera_class), layout.store,
                layout.valid ? "" : " (not found: the satellite camera keeps no targets)");
    it = g_camera_layouts.emplace(camera_class, layout).first;
  }
  if (!it->second.valid) return;
  const uint32_t world = LoadU32(base, camera + it->second.world_info);
  if (!IsGuestPointer(world)) return;
  const uint32_t world_class = LoadU32(base, world + kObjectClass);
  auto gri_at = g_gri_offsets.find(world_class);
  if (gri_at == g_gri_offsets.end()) {
    const uint32_t gri = FindField(base, world_class, "GRI");
    gri_at = g_gri_offsets.emplace(world_class, gri ? LoadU32(base, gri + kPropertyOffset) : 0).first;
  }
  const uint32_t gri = gri_at->second ? LoadU32(base, world + gri_at->second) : 0;
  if (!IsGuestPointer(gri)) return;
  if (!g_shod_targets) g_shod_targets = FindField(base, LoadU32(base, gri + kObjectClass), "SHODTargets", "GearGRI");
  if (!g_shod_targets) return;
  // Script reaches the array at GRI + offset, in 32-bit guest arithmetic: this offset makes that the
  // camera's store, wherever the two objects are.
  StoreU32(base, g_shod_targets + kPropertyOffset, camera + it->second.store - gri);
}

bool DeferPreload(const uint8_t* base, uint32_t object, uint32_t return_address) {
  if (return_address != kStructSerializePreloadChildren && return_address != kStructSerializePreloadField) {
    return false;
  }
  if (!IsGuestPointer(object)) return false;
  // Only the patch's first field of a class that had none (see class_fixups.h).
  return ObjectName(base, object) == "bShieldDispersed" &&
         ObjectName(base, LoadU32(base, object + kObjectOuter)) == "GearPawn_LocustGeneralRaamBase";
}

}  // namespace azura::dlc

// ULinkerLoad::Preload (guest 0x822ED198: r3 = linker, r4 = object), which DeferPreload may skip.
extern "C" void __imp__sub_822ED198(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822ED198(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t object = ctx.r4.u32;
  if (azura::dlc::DeferPreload(base, object, static_cast<uint32_t>(ctx.lr))) return;
  __imp__sub_822ED198(ctx, base);
  azura::dlc::OnPreloaded(base, object);
}

// ABlockingVolume::IgnoreBlockingBy (guest 0x8243CB08: r3 = the volume, r4 = the other actor; returns
// whether the actor goes through), the version BlockingVolume, DynamicBlockingVolume and the patch's
// PawnBlockingVolume share. The Title Update's PawnBlockingVolume is a native class that lets the
// pawns of its AllowedClasses through; the patch's is a script class, so the runtime does that here.
extern "C" void __imp__sub_8243CB08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8243CB08(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t pawn_blocking_volume = azura::dlc::g_pawn_blocking_volume.load(std::memory_order_relaxed);
  const uint32_t volume = ctx.r3.u32, other = ctx.r4.u32;
  if (pawn_blocking_volume && azura::dlc::IsGuestPointer(volume) && azura::dlc::IsGuestPointer(other) &&
      azura::dlc::IsChildOf(base, azura::dlc::LoadU32(base, volume + azura::dlc::kObjectClass), pawn_blocking_volume)) {
    const uint32_t array = volume + azura::dlc::g_allowed_classes_offset.load(std::memory_order_relaxed);
    const uint32_t data = azura::dlc::LoadU32(base, array), count = azura::dlc::LoadU32(base, array + 4);
    const uint32_t other_class = azura::dlc::LoadU32(base, other + azura::dlc::kObjectClass);
    for (uint32_t i = 0; azura::dlc::IsGuestPointer(data) && i < count && i < 64; ++i) {
      if (azura::dlc::IsChildOf(base, other_class, azura::dlc::LoadU32(base, data + i * 4))) {
        ctx.r3.u64 = 1;
        return;
      }
    }
  }
  __imp__sub_8243CB08(ctx, base);
}
