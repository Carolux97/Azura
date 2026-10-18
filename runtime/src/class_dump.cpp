// Class dump for reverse engineering the game's UnrealScript (Unreal Engine 3, recompiled).
//
// Script classes are UClass objects. The script hooks report the class of every script function
// that runs and of the object running it (NoteClass), so any class the game uses shows up; the
// super classes of those are followed too. The object table at 0x83473754 (chains linked through
// UObject +4) is also walked: it does not hold every object, but it has classes that have not run
// any script yet. Layouts (guest offsets, big-endian) are documented in
// docs/reverse-engineering.md.
//
// Everything here runs on its own thread and only reads guest memory, through vm_read_overwrite:
// a plain read of an unmapped address would fault, and the runtime's exception handler treats any
// fault in guest memory as the game's and retries it forever. Doing the walk on the script thread
// is not an option either: it takes seconds and stalls the game long enough to break it.

#include "class_dump.h"

#include <mach/mach.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>

namespace azura::script {

namespace {

constexpr uint32_t kGNames = 0x8343D148;   // TArray<FNameEntry*>
constexpr uint32_t kObjectTable = 0x83473754;  // TArray<UObject*>: chain heads
constexpr uint32_t kObjectChainNext = 4;
constexpr uint32_t kObjectOuter = 40;
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kObjectClass = 52;
constexpr uint32_t kFieldNext = 60;
constexpr uint32_t kStructSuper = 64;
constexpr uint32_t kStructChildren = 68;
constexpr uint32_t kPropertyFlags = 72;
constexpr uint32_t kPropertyOffset = 84;
constexpr uint32_t kClassDefaultObject = 236;
constexpr uint32_t kStructScript = 76;  // TArray<BYTE>: the compiled bytecode
constexpr uint64_t kPropertyParm = 0x80;  // CPF_Parm

// Reads guest memory without faulting when it is not mapped.
class GuestReader {
 public:
  explicit GuestReader(const uint8_t* base) : base_(base) {}

  bool Read(uint32_t address, void* out, size_t size) const {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), reinterpret_cast<vm_address_t>(base_ + address), size,
                             reinterpret_cast<vm_address_t>(out), &got) == KERN_SUCCESS &&
           got == size;
  }

  // Big-endian u32 at `address`, or 0 when unreadable.
  uint32_t U32(uint32_t address) const {
    uint32_t raw = 0;
    return Read(address, &raw, 4) ? __builtin_bswap32(raw) : 0;
  }

  float Float(uint32_t address) const {
    const uint32_t bits = U32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

  uint32_t NameCount() const { return U32(kGNames + 4); }

  std::string Name(uint32_t index) const {
    if (index >= NameCount()) return {};
    const uint32_t entry = U32(U32(kGNames) + index * 4);
    if (!entry) return {};
    const bool wide = (U32(entry) & 1) != 0;
    uint8_t raw[256] = {};
    if (!Read(entry + 8, raw, wide ? 256 : 128)) return {};
    std::string text;
    for (uint32_t i = 0; i < 128; ++i) {
      const uint16_t c = wide ? static_cast<uint16_t>(raw[i * 2] << 8 | raw[i * 2 + 1]) : raw[i];
      if (c == 0) break;
      text.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return text;
  }

  // FString (TArray<TCHAR>: data, num, max; UTF-16BE) at `address`, as ASCII.
  std::string String(uint32_t address) const {
    const uint32_t data = U32(address);
    const uint32_t count = std::min<uint32_t>(U32(address + 4), 200);
    std::string text;
    for (uint32_t i = 0; data && i + 1 < count; ++i) {
      uint16_t raw = 0;
      Read(data + i * 2, &raw, 2);
      const uint16_t c = __builtin_bswap16(raw);
      text.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return text;
  }

  std::string ObjectName(uint32_t object) const { return object ? Name(U32(object + kObjectName)) : "None"; }
  std::string ClassName(uint32_t object) const { return ObjectName(U32(object + kObjectClass)); }

  bool PlausibleObject(uint32_t object, uint32_t name_count) const {
    if (object < 0x40000000 || object >= 0xA0000000) return false;
    const uint32_t cls = U32(object + kObjectClass);
    return U32(object + kObjectName) < name_count && cls >= 0x40000000 && cls < 0xA0000000 &&
           U32(cls + kObjectName) < name_count;
  }

 private:
  const uint8_t* base_;
};

// Whether `name` is in the comma-separated `list`. An entry ending in '*' matches by prefix
// (those classes are only listed by name, not dumped: see TryDump).
bool InList(const std::string& list, const std::string& name, bool* by_prefix = nullptr) {
  size_t start = 0;
  while (start <= list.size()) {
    const size_t end = std::min(list.find(',', start), list.size());
    const std::string entry = list.substr(start, end - start);
    if (!entry.empty() && entry.back() == '*') {
      if (name.compare(0, entry.size() - 1, entry, 0, entry.size() - 1) == 0) {
        if (by_prefix) *by_prefix = true;
        return true;
      }
    } else if (entry == name) {
      if (by_prefix) *by_prefix = false;
      return true;
    }
    start = end + 1;
  }
  return false;
}

// Enum value names: UEnum keeps them as a TArray<FName> (8 bytes each: index, number) right after
// UField.
std::string EnumValues(const GuestReader& mem, uint32_t enumeration) {
  const uint32_t data = mem.U32(enumeration + kStructSuper);
  const uint32_t num = mem.U32(enumeration + kStructSuper + 4);
  std::string values;
  for (uint32_t i = 0; data && i < num && i < 64; ++i) {
    values += (values.empty() ? "" : ", ") + std::to_string(i) + "=" + mem.Name(mem.U32(data + i * 8));
  }
  return values;
}

bool g_dump_code = false;

// Rough listing of a function's bytecode: the references that can be recognised, in order. UE3
// tokens used (cooked for Xbox 360, big-endian; object references take 8 bytes whose high half is
// zero, names take 8 bytes: index and number): 0x00/0x01/0x02 local/instance/default variable +
// property, 0x1B virtual call + name, 0x1C final call + function, 0x1D int, 0x1E float,
// 0x1F string (zero-terminated), 0x21 name, 0x24 byte (enum values), 0x25/0x26 int 0/1,
// 0x27/0x28 true/false, 0x2C byte, 0x06 jump, 0x07 jump-if-not. Other bytes are skipped one by
// one, so a constant can occasionally be misread as a token. The raw bytes of short functions are
// logged too, to check by hand.
void DumpCode(const GuestReader& mem, uint32_t function, const std::string& indent) {
  const uint32_t data = mem.U32(function + kStructScript);
  const uint32_t size = std::min<uint32_t>(mem.U32(function + kStructScript + 4), 0x10000);
  if (!data || !size) return;
  std::vector<uint8_t> code(size + 16);
  if (!mem.Read(data, code.data(), size)) return;
  const uint32_t name_count = mem.NameCount();
  // Raw bytes too for short functions, to check the listing by hand.
  if (size <= 400) {
    std::string hex;
    for (uint32_t i = 0; i < size; ++i) {
      char byte[4];
      std::snprintf(byte, sizeof(byte), "%02X ", code[i]);
      hex += byte;
    }
    REXLOG_INFO("[Azura dump] {}  bytes: {}", indent, hex);
  }
  auto u32 = [&](uint32_t at) { return uint32_t(code[at]) << 24 | code[at + 1] << 16 | code[at + 2] << 8 | code[at + 3]; };
  // An 8-byte object reference at `at`: its guest address, or 0 when the high half is not zero.
  auto ref = [&](uint32_t at) { return u32(at) == 0 ? u32(at + 4) : 0; };
  auto is_type = [&](uint32_t object, const char* suffix) {
    if (!mem.PlausibleObject(object, name_count)) return false;
    const std::string type = mem.ClassName(object);
    const size_t n = std::strlen(suffix);
    return type.size() >= n && type.compare(type.size() - n, n, suffix) == 0;
  };
  std::string line;
  auto emit = [&](const std::string& token) {
    line += token + " ";
    if (line.size() > 1500) {
      REXLOG_INFO("[Azura dump] {}  code: {}", indent, line);
      line.clear();
    }
  };
  for (uint32_t i = 0; i < size;) {
    const uint8_t op = code[i];
    if ((op <= 0x02) && i + 9 <= size && is_type(ref(i + 1), "Property")) {
      emit(mem.ObjectName(ref(i + 1)));
      i += 9;
    } else if (op == 0x1B && i + 9 <= size && u32(i + 1) < name_count && u32(i + 5) == 0) {
      emit(mem.Name(u32(i + 1)) + "()");
      i += 9;
    } else if (op == 0x1C && i + 9 <= size && is_type(ref(i + 1), "Function")) {
      const uint32_t target = ref(i + 1);
      emit(mem.ObjectName(mem.U32(target + kObjectOuter)) + "." + mem.ObjectName(target) + "()");
      i += 9;
    } else if (op == 0x1D && i + 5 <= size) {
      emit("#" + std::to_string(static_cast<int32_t>(u32(i + 1))));
      i += 5;
    } else if (op == 0x1E && i + 5 <= size) {
      const uint32_t bits = u32(i + 1);
      float f;
      std::memcpy(&f, &bits, 4);
      emit("#" + std::to_string(f));
      i += 5;
    } else if (op == 0x24 && i + 2 <= size) {  // byte constant (enum values)
      emit("#b" + std::to_string(code[i + 1]));
      i += 2;
    } else if (op == 0x25 || op == 0x26) {  // int zero, int one
      emit(op == 0x25 ? "#0" : "#1");
      ++i;
    } else if (op == 0x27 || op == 0x28) {  // true, false
      emit(op == 0x27 ? "#true" : "#false");
      ++i;
    } else if (op == 0x2C && i + 2 <= size) {
      emit("#" + std::to_string(code[i + 1]));
      i += 2;
    } else if (op == 0x1F) {
      std::string text;
      uint32_t j = i + 1;
      while (j < size && code[j] && text.size() < 80) text.push_back(static_cast<char>(code[j++]));
      emit("\"" + text + "\"");
      i = j + 1;
    } else if (op == 0x21 && i + 9 <= size && u32(i + 1) < name_count && u32(i + 5) == 0) {
      emit("'" + mem.Name(u32(i + 1)) + "'");
      i += 9;
    } else if ((op == 0x06 || op == 0x07) && i + 3 <= size) {
      emit(op == 0x06 ? "|jump|" : "|if-not|");
      i += 3;
    } else {
      ++i;
    }
  }
  if (!line.empty()) REXLOG_INFO("[Azura dump] {}  code: {}", indent, line);
}

void DumpFields(const GuestReader& mem, uint32_t owner, const std::string& indent) {
  for (uint32_t field = mem.U32(owner + kStructChildren); field; field = mem.U32(field + kFieldNext)) {
    const std::string type = mem.ClassName(field);
    if (type == "Function") {
      std::string params;
      for (uint32_t prop = mem.U32(field + kStructChildren); prop; prop = mem.U32(prop + kFieldNext)) {
        const uint64_t flags = uint64_t(mem.U32(prop + kPropertyFlags)) << 32 | mem.U32(prop + kPropertyFlags + 4);
        if (!(flags & kPropertyParm)) break;
        params += (params.empty() ? "" : ", ") + mem.ClassName(prop) + " " + mem.ObjectName(prop);
      }
      REXLOG_INFO("[Azura dump] {}function {}({})", indent, mem.ObjectName(field), params);
      if (g_dump_code) {
        // Locals follow the parameters among the function's children.
        std::string locals;
        for (uint32_t prop = mem.U32(field + kStructChildren); prop; prop = mem.U32(prop + kFieldNext)) {
          locals += " " + mem.ObjectName(prop) + "@" + std::to_string(mem.U32(prop + kPropertyOffset));
        }
        REXLOG_INFO("[Azura dump] {}  locals:{}", indent, locals);
        DumpCode(mem, field, indent);
      }
    } else if (type == "ScriptStruct") {
      REXLOG_INFO("[Azura dump] {}struct {}", indent, mem.ObjectName(field));
      DumpFields(mem, field, indent + "  ");
    } else if (type == "Enum") {
      REXLOG_INFO("[Azura dump] {}enum {} {{{}}}", indent, mem.ObjectName(field), EnumValues(mem, field));
    } else if (type.size() > 8 && type.compare(type.size() - 8, 8, "Property") == 0) {
      REXLOG_INFO("[Azura dump] {}{} {} @{}", indent, type, mem.ObjectName(field), mem.U32(field + kPropertyOffset));
    } else {
      REXLOG_INFO("[Azura dump] {}{} {}", indent, type, mem.ObjectName(field));
    }
  }
}

// The option menu's categories and rows as the class defaults hold them (UIScene_Options:
// OptionCategories at +1016, OptionCategoryInfo 64 bytes with CategoryOptions at +40, OptionInfo
// 48 bytes; offsets from the dump of those structs).
void DumpOptionDefaults(const GuestReader& mem, uint32_t defaults) {
  const uint32_t categories = mem.U32(defaults + 1016);
  const uint32_t category_count = mem.U32(defaults + 1020);
  for (uint32_t c = 0; categories && c < category_count && c < 16; ++c) {
    const uint32_t category = categories + c * 64;
    REXLOG_INFO("[Azura dump] category {} '{}' callback '{}' global {}", c, mem.String(category),
                mem.String(category + 24), mem.U32(category + 36));
    const uint32_t buttons = mem.U32(category + 52);
    const uint32_t button_count = mem.U32(category + 56);
    for (uint32_t b = 0; buttons && b < button_count && b < 8; ++b) {
      const uint32_t button = buttons + b * 16;  // ButtonInfo: string +0, ButtonType +12, ButtonHandler +13
      uint8_t kinds[2] = {};
      mem.Read(button + 12, kinds, 2);
      REXLOG_INFO("[Azura dump]   button {} '{}' type {} handler {}", b, mem.String(button), kinds[0], kinds[1]);
    }
    const uint32_t options = mem.U32(category + 40);
    const uint32_t option_count = mem.U32(category + 44);
    for (uint32_t o = 0; options && o < option_count && o < 32; ++o) {
      const uint32_t option = options + o * 48;
      REXLOG_INFO("[Azura dump]   option {} psid {} type {} flags 0x{:08X} slider {}..{} step {} '{}'", o,
                  static_cast<int32_t>(mem.U32(option)), mem.U32(option + 28) >> 24, mem.U32(option + 32),
                  mem.Float(option + 36), mem.Float(option + 40), mem.Float(option + 44), mem.String(option + 4));
    }
  }
}

// A profile settings class's setting definitions as its defaults hold them (OnlinePlayerStorage:
// ProfileMappings at +84, SettingsPropertyPropertyMetaData 64 bytes: Id +0, Name +4,
// MappingType +24, ValueMappings +28 with IdToStringMapping 12 bytes: Id +0, Name +4).
void DumpProfileDefaults(const GuestReader& mem, uint32_t defaults) {
  const uint32_t mappings = mem.U32(defaults + 84);
  const uint32_t count = mem.U32(defaults + 88);
  REXLOG_INFO("[Azura dump] {} profile mappings, {} current settings", count, mem.U32(defaults + 76));
  for (uint32_t m = 0; mappings && m < count && m < 512; ++m) {
    const uint32_t mapping = mappings + m * 64;
    std::string values;
    const uint32_t value_data = mem.U32(mapping + 28);
    const uint32_t value_count = mem.U32(mapping + 32);
    for (uint32_t v = 0; value_data && v < value_count && v < 16; ++v) {
      values += " " + std::to_string(mem.U32(value_data + v * 12)) + ":" + mem.Name(mem.U32(value_data + v * 12 + 4));
    }
    REXLOG_INFO("[Azura dump]   mapping {} id {} '{}' type {} range {}..{} step {} values{}", m,
                static_cast<int32_t>(mem.U32(mapping)), mem.Name(mem.U32(mapping + 4)), mem.U32(mapping + 24) >> 24,
                mem.Float(mapping + 52), mem.Float(mapping + 56), mem.Float(mapping + 60), values);
  }
}

void DumpClass(const GuestReader& mem, uint32_t cls) {
  REXLOG_INFO("[Azura dump] class {} extends {} (outer {})", mem.ObjectName(cls),
              mem.ObjectName(mem.U32(cls + kStructSuper)), mem.ObjectName(mem.U32(cls + kObjectOuter)));
  DumpFields(mem, cls, "  ");
  if (mem.ObjectName(cls) == "UIScene_Options") DumpOptionDefaults(mem, mem.U32(cls + kClassDefaultObject));
  if (mem.ObjectName(cls) == "GearProfileSettings") DumpProfileDefaults(mem, mem.U32(cls + kClassDefaultObject));
}

// Classes in the object table (see the top of this file).
std::vector<uint32_t> TableClasses(const GuestReader& mem) {
  const uint32_t heads = mem.U32(kObjectTable);
  const uint32_t head_count = std::min<uint32_t>(mem.U32(kObjectTable + 4), 0x100000);
  const uint32_t name_count = mem.NameCount();
  std::unordered_set<uint32_t> seen;
  std::vector<uint32_t> classes;
  for (uint32_t h = 0; heads && h < head_count; ++h) {
    for (uint32_t object = mem.U32(heads + h * 4); mem.PlausibleObject(object, name_count);
         object = mem.U32(object + kObjectChainNext)) {
      if (!seen.insert(object).second) break;  // chains share tails
      if (mem.ClassName(object) == "Class") classes.push_back(object);
    }
  }
  return classes;
}

std::mutex g_notes_mutex;
std::unordered_set<uint32_t> g_noted;   // every class reported so far
std::vector<uint32_t> g_pending;        // reported but not yet looked at by the dump thread
std::atomic<bool> g_active{false};

// One pass over the newly reported classes; true when finished (all dumped, or given up).
bool TryDump(const GuestReader& mem, const std::string& wanted, std::unordered_set<std::string>& dumped,
             std::string& listed, int attempt) {
  std::vector<uint32_t> pending = TableClasses(mem);
  {
    std::lock_guard lock(g_notes_mutex);
    pending.insert(pending.end(), g_pending.begin(), g_pending.end());
    g_pending.clear();
  }
  const uint32_t name_count = mem.NameCount();
  std::unordered_set<uint32_t> visited;
  for (size_t i = 0; i < pending.size(); ++i) {
    const uint32_t cls = pending[i];
    if (!visited.insert(cls).second || !mem.PlausibleObject(cls, name_count)) continue;
    const uint32_t super = mem.U32(cls + kStructSuper);
    if (super) pending.push_back(super);  // super classes are classes the game uses too
    const std::string name = mem.ObjectName(cls);
    bool by_prefix = false;
    if (!InList(wanted, name, &by_prefix)) continue;
    if (by_prefix) {
      if (listed.find(" " + name + " ") == std::string::npos) listed += name + " ";
    } else if (dumped.insert(name).second) {
      DumpClass(mem, cls);
    }
  }
  const size_t wanted_count = std::count(wanted.begin(), wanted.end(), ',') + 1 -
                              std::count(wanted.begin(), wanted.end(), '*');
  // Gives up after about ten minutes: some classes load only after the title screen, so the wait
  // must leave time to get past it.
  const bool done = dumped.size() >= wanted_count || attempt >= 120;
  if (done && listed.size() > 1) REXLOG_INFO("[Azura dump] classes matching prefixes: {}", listed);
  if (done && dumped.size() < wanted_count) {
    REXLOG_INFO("[Azura dump] gave up: {} of {} classes seen", dumped.size(), wanted_count);
  }
  return done;
}

}  // namespace

void NoteClass(uint32_t cls) {
  if (!cls || !g_active.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_notes_mutex);
  if (g_noted.insert(cls).second) g_pending.push_back(cls);
}

void StartClassDump(const uint8_t* base, std::string class_names, bool with_code) {
  static std::once_flag started;
  std::call_once(started, [base, names = std::move(class_names), with_code] {
    g_dump_code = with_code;
    g_active.store(true);
    std::thread([base, names] {
      const GuestReader mem(base);
      std::unordered_set<std::string> dumped;
      std::string listed = " ";
      for (int attempt = 0; !TryDump(mem, names, dumped, listed, attempt); ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
      g_active.store(false);
    }).detach();
  });
}

}  // namespace azura::script
