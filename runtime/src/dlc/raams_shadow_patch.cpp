// Azura's GearGame script patch for RAAM's Shadow. See raams_shadow_patch.h.
//
// The content and the way each object is written follow tools/dlc/make_patch.py one for one (the same
// exports, NetIndexes and names in the same order), so both write the same bytes; the Python tool is
// where the format was worked out, and docs/raams-shadow-dlc.md explains each choice.

#include "raams_shadow_patch.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

#include "bytecode.h"
#include "lzx.h"
#include "ue3.h"

REXCVAR_DEFINE_STRING(make_dlc_patch, "", "Azura",
                      "Write Azura's script patch for RAAM's Shadow, made from the game files, into this "
                      "folder and exit (used by the launcher)");

namespace azura::dlc {
namespace {

namespace bc = bytecode;
using ue3::Bytes;

// --- Content -------------------------------------------------------------------------------------

// Property flags: CPF_Edit; component properties also CPF_ExportObject, CPF_Component and
// CPF_EditInline, as the disc's own component properties have them.
constexpr uint64_t EDIT = 0x1;
constexpr uint64_t TRANSIENT = 0x2000;
constexpr uint64_t COMPONENT_FLAGS = 0x0000000004080009;
constexpr uint64_t CPF_PARM = 0x80;
constexpr uint64_t CPF_NEED_CTOR_LINK = 0x400000;
constexpr uint32_t FUNC_FINAL = 0x1;
constexpr uint32_t FUNC_NATIVE = 0x400;

// A property: name, type, flags, the classes it names (an object or component property's class; a
// class property's own class and its meta class), and an array's inner property (which takes the
// array's name).
struct Prop {
  std::string name, kind;
  uint64_t flags = 0;
  std::vector<std::string> extra;
  std::vector<Prop> inner;  // one, for an array
};

Prop Object(std::string name, uint64_t flags, std::string cls) { return {std::move(name), "ObjectProperty", flags, {std::move(cls)}, {}}; }
Prop Component(std::string name, std::string cls) { return {std::move(name), "ComponentProperty", COMPONENT_FLAGS, {std::move(cls)}, {}}; }
Prop Array(std::string name, uint64_t flags, Prop inner) { return {std::move(name), "ArrayProperty", flags, {}, {std::move(inner)}}; }

// A function's parameter or local variable: name, type, the class an object property names, flags.
struct Param {
  std::string name, kind, cls;
  uint64_t flags = CPF_PARM;
};

class Builder;
using Fields = std::map<std::string, int32_t>;
using ScriptBody = std::function<bc::Code(Builder&, const Fields&)>;

// A function's body: empty (return;), a copy of the bytecode of the disc function the flags come from
// with the references it names replaced (by a GearGame path or a parameter's name), or a script.
struct Body {
  enum Kind { kEmpty, kCopy, kScript } kind = kEmpty;
  std::vector<std::pair<std::string, std::string>> replace;  // kCopy
  ScriptBody script;                                         // kScript
};

struct FunctionSpec {
  std::string owner, name, like;  // like: the disc function whose flags it takes; owner empty in a new class
  std::vector<Param> params;      // in the function's field chain order (last declared first, then locals)
  Body body;
  std::string params_from = {};   // a disc function whose parameters (all of them) it takes instead
};

// A state's code (from its 'Begin' label).
using StateScript = std::function<bc::Code(Builder&)>;

// A tagged property of a new class's default object: a float, a bool or a struct; in a struct also an
// enum value, a name and an array of structs.
struct DefaultTag {
  enum Kind { kFloat, kBool, kStruct, kEnum, kName, kStructs } kind;
  std::string name;
  float number = 0.0f;                    // kFloat
  bool flag = false;                      // kBool
  std::string type = {}, value = {};      // kStruct: its struct; kEnum: its enum and value; kName: value
  std::vector<std::vector<DefaultTag>> structs = {};  // kStruct: one, its properties; kStructs: each element's
};

DefaultTag FloatTag(std::string name, float v) { return {DefaultTag::kFloat, std::move(name), v}; }
DefaultTag BoolTag(std::string name, bool v) { return {DefaultTag::kBool, std::move(name), 0.0f, v}; }
DefaultTag StructTag(std::string name, std::string type, std::vector<DefaultTag> properties) {
  return {DefaultTag::kStruct, std::move(name), 0.0f, false, std::move(type), {}, {std::move(properties)}};
}
DefaultTag EnumTag(std::string name, std::string type, std::string value) {
  return {DefaultTag::kEnum, std::move(name), 0.0f, false, std::move(type), std::move(value)};
}
DefaultTag NameTag(std::string name, std::string value) { return {DefaultTag::kName, std::move(name), 0.0f, false, {}, std::move(value)}; }
DefaultTag StructsTag(std::string name, std::vector<std::vector<DefaultTag>> elements) {
  return {DefaultTag::kStructs, std::move(name), 0.0f, false, {}, {}, std::move(elements)};
}

// A BodyStance of one full body animation.
DefaultTag Stance(std::string name, std::string anim) {
  return StructTag(std::move(name), "BodyStance",
                   {StructsTag("Stance", {{EnumTag("Slot", "EBodyStance", "BS_FullBody"), NameTag("AnimName", std::move(anim))}})});
}

struct ClassSpec {
  std::string name, parent;  // parent: a GearGame class name, or 'Engine.Class'
  std::vector<Prop> properties;
  std::string within = {};                                    // its ClassWithin (a GearGame class), when not the parent's
  std::vector<FunctionSpec> functions = {};                   // in their declaration order
  std::vector<std::pair<std::string, StateScript>> states = {};
  std::vector<DefaultTag> defaults = {};  // the default object's own tagged properties
};

bc::Code GrapplingDropStarted(Builder& b, const Fields& f);
bc::Code BrumakEvaluateActivation(Builder& b, const Fields& f);
bc::Code BrumakPushed(Builder& b, const Fields& f);
bc::Code BrumakPopped(Builder& b, const Fields& f);
bc::Code BrumakAttackTruck(Builder& b);

// New classes, as script subclasses of the disc classes they extend.
const std::vector<ClassSpec> kClasses = {
    // The drop of a Locust from a grappling hook: its parent's fall, started with the burst of an
    // emergence (its animations) and a downward speed, as the Title Update has it.
    {"GSM_GrapplingHook_DynamicClimbDrop",
     "GSM_BaseVariableFall",
     {Prop{"FallStartSpeedZ", "FloatProperty", EDIT, {}, {}}},
     {},
     {{"", "SpecialMoveStarted", "GSM_BaseVariableFall.SpecialMoveStarted", {}, {Body::kScript, {}, GrapplingDropStarted},
       "GSM_BaseVariableFall.SpecialMoveStarted"}},
     {},
     {FloatTag("FallStartSpeedZ", -400.0f), Stance("BS_Jump", "Emerge_Burst_fall"), Stance("BS_Fall", "Emerge_Burst_fall"),
      Stance("BS_Land", "Emerge_Burst_land"), BoolTag("bLockPawnRotation", true)}},
    {"GearPawn_LocustEliteBoomerFlailBase", "GearPawn_LocustBoomerFlailBase", {}},
    {"Turret_SatelliteHODBase", "Turret_TroikaBase", {}},
    {"GearPawn_SatelliteHODCamera_Base",
     "GearPawn",
     {Object("SatelliteTurret", EDIT, "GearGame.Turret_SatelliteHODBase"),
      Object("HODPostProcessEffect", EDIT, "Engine.PostProcessEffect"),
      // Azura's own: where GearGRI.SHODTargets keeps the camera's targets (see kMembers).
      Array("SHODTargetStore", TRANSIENT, Object("", 0, "Engine.Actor"))}},
    {"PawnBlockingVolume",
     "Engine.DynamicBlockingVolume",
     {Array("AllowedClasses", EDIT, Prop{"", "ClassProperty", EDIT, {"Core.Class", "Engine.Pawn"}, {}})}},
    {"SeqEvent_BerserkerVulnerable", "Engine.SequenceEvent", {}},
    {"ThumperSpikeBase",
     "Engine.Actor",
     {Component("LightEnvironment", "Engine.DynamicLightEnvironmentComponent"),
      Component("MeshComp", "Engine.StaticMeshComponent")}},
    // The DLC's Brumak (Caravan) attacks its scripted targets, the convoy's vehicles, every so often:
    // the AI command its utility-based AI picks for that, as the Title Update has it.
    {"AICmd_Brumak_Action_AttackScriptedTargets",
     "AICmd_Base_UtilitySelectedCmd",
     {},
     "GearAI_Brumak",
     {{"", "EvaluateActivation", "AICmd_Base_UtilitySelectedCmd.EvaluateActivation", {},
       {Body::kScript, {}, BrumakEvaluateActivation}, "AICmd_Base_UtilitySelectedCmd.EvaluateActivation"},
      {"", "Pushed", "AICommand.Popped", {}, {Body::kScript, {}, BrumakPushed}},
      {"", "Popped", "AICommand.Popped", {}, {Body::kScript, {}, BrumakPopped}}},
     {{"AttackTruck", BrumakAttackTruck}}},
    {"GearDLCCampaignActData", "GearCampaignActData", {}},
};

// Properties on disc classes. The runtime gives each its place in the class (class_fixups.h).
const std::vector<std::pair<std::string, Prop>> kMembers = {
    {"GearPawn_LocustGeneralRaamBase", Prop{"bShieldDispersed", "BoolProperty", 0, {}, {}}},
    {"Vehicle_Reaver_Base", Component("TaccomMesh", "Engine.StaticMeshComponent")},
    // The satellite camera's enemies (GearPawn_SatelliteHODCamera.AddEnemyLocators, the only script
    // that uses it, through GearGRI.AddSHODTarget). GearGRI has no room for it, so the runtime points
    // it at the camera's SHODTargetStore while the camera runs that function; its inner property is
    // an int, of the same size as an object reference, so that GearGRI's garbage collection token
    // stream gets nothing for it (the store's own property is the one the collector follows).
    {"GearGRI", Array("SHODTargets", 0, Prop{"", "IntProperty", 0, {}, {}})},
};

bc::Code EnablePostProcessEffects(Builder& b, const Fields& f);
bc::Code DisablePostProcessEffects(Builder& b, const Fields& f);

const std::vector<FunctionSpec> kFunctions = {
    {"GearPawn", "NonFinalActivateRoadieRunComet", "GearPawn.ActivateRoadieRunComet", {}, {}},
    // if (SHODTargets.Find(Target) == INDEX_NONE) SHODTargets.AddItem(Target);, as AddTaccomTurret
    // does with its own array.
    {"GearGRI",
     "AddSHODTarget",
     "GearGRI.AddTaccomTurret",
     {{"Target", "ObjectProperty", "Engine.Actor"}},
     {Body::kCopy,
      {{"GearGRI.TaccomTurrets", "GearGame.GearGRI.SHODTargets"}, {"GearGRI.AddTaccomTurret.Turret", "Target"}},
      {}}},
    // Myrrah's transmissions (SeqAct_MyrrahTalk) and the satellite camera turn a post process effect of
    // their own on and off for each player. Reliable client functions, as the disc's other GearPC.Client*
    // functions.
    {"GearPC",
     "EnablePostProcessEffects",
     "GearPC.ClientColorFade",
     {{"EffectToEnable", "ObjectProperty", "Engine.MaterialEffect"},
      {"Chain", "ObjectProperty", "Engine.PostProcessChain"},
      {"ChainName", "NameProperty", ""},
      {"LP", "ObjectProperty", "Engine.LocalPlayer", 0},
      {"MIC", "ObjectProperty", "Engine.MaterialInstanceConstant", 0}},
     {Body::kScript, {}, EnablePostProcessEffects}},
    {"GearPC",
     "DisablePostProcessEffects",
     "GearPC.ClientColorFade",
     {{"EffectToDisable", "ObjectProperty", "Engine.MaterialEffect"},
      {"ChainName", "NameProperty", ""},
      {"I", "IntProperty", "", 0},
      {"LP", "ObjectProperty", "Engine.LocalPlayer", 0}},
     {Body::kScript, {}, DisablePostProcessEffects}},
};
// Not Vehicle_Reaver_Base's GetHitAnimName_Damage, PlayHitReactionAnimation and
// ShouldTurnCursorRedFor, which the DLC's Raam Reaver overrides: the disc build's loader breaks on a
// class that receives a new property (TaccomMesh) and new functions. The DLC's overrides keep no
// parent function, and the runtime takes their Super calls (missing_super.h).

// Bytecode tokens of an empty body.
constexpr uint8_t EX_RETURN = 0x04;
constexpr uint8_t EX_NOTHING = 0x0B;
constexpr uint8_t EX_END_OF_SCRIPT = 0x53;

int32_t LoadI32(const Bytes& b, size_t at) {
  return int32_t((uint32_t(b.at(at)) << 24) | (uint32_t(b.at(at + 1)) << 16) | (uint32_t(b.at(at + 2)) << 8) |
                 b.at(at + 3));
}

void StoreI32(Bytes& b, size_t at, int32_t v) {
  for (int k = 0; k < 4; ++k) b.at(at + k) = uint8_t(uint32_t(v) >> (24 - 8 * k));
}

uint64_t LoadU64(const Bytes& b, size_t at) { return (uint64_t(uint32_t(LoadI32(b, at))) << 32) | uint32_t(LoadI32(b, at + 4)); }

void StoreU64(Bytes& b, size_t at, uint64_t v) {
  StoreI32(b, at, int32_t(v >> 32));
  StoreI32(b, at + 4, int32_t(v));
}

bool StartsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The part of an Engine parent ("Engine.Actor") after the package.
std::string AfterPackage(const std::string& path) { return path.substr(path.find('.') + 1); }

// --- Building ------------------------------------------------------------------------------------

class Builder {
 public:
  Builder(const ue3::Package& pkg, const ue3::Package& engine)
      : pkg_(pkg), engine_(engine), next_net_index_(pkg.net_object_count()), none_(pkg.NameIndex("None")) {
    patch_.package = "GearGame";
    if (none_ < 0) throw ue3::FormatError{"GearGame has no None name"};
  }

  Bytes Build() {
    for (const auto& c : kClasses) NewClass(c);
    for (const auto& [owner, prop] : kMembers) Member(owner, prop);
    for (const auto& f : kFunctions) Function(f);
    for (size_t k = 0; k < patch_.exports.size(); ++k) patch_.new_objects.emplace_back(patch_.exports[k].name, blobs_[k]);
    return patch_.Write();
  }

  // Names: the package's, then the patch's own.
  int32_t NameIndex(const std::string& text) {
    if (const int32_t index = pkg_.NameIndex(text); index >= 0) return index;
    for (size_t k = 0; k < patch_.names.size(); ++k) {
      if (patch_.names[k] == text) return int32_t(pkg_.names().size() + k);
    }
    patch_.names.push_back(text);
    return int32_t(pkg_.names().size() + patch_.names.size() - 1);
  }

  // Reference to an object of the package (or one of the patch's own, by path).
  int32_t Ref(const std::string& path) const {
    int32_t index = 0;
    if (StartsWith(path, "GearGame.")) {
      const std::string local = path.substr(9);
      for (size_t k = 0; k < patch_.exports.size(); ++k) {
        if (ExportPath(k) == local) return PatchIndex(k);
      }
      index = pkg_.Find(local);
    } else {
      index = pkg_.Find(path);
    }
    if (!index) throw ue3::FormatError{path + " is not in GearGame"};
    return index;
  }

 private:
  int32_t PatchIndex(size_t k) const { return int32_t(pkg_.exports().size() + 1 + k); }
  size_t PatchSlot(int32_t index) const { return size_t(index) - pkg_.exports().size() - 1; }

  std::string ExportPath(size_t k) const {
    const auto* e = &patch_.exports[k];
    std::string path = e->name;
    int32_t outer = e->outer;
    while (outer) {
      if (size_t(outer) > pkg_.exports().size()) {
        e = &patch_.exports[PatchSlot(outer)];
        path = e->name + "." + path;
        outer = e->outer;
      } else {
        path = pkg_.Path(outer) + "." + path;
        break;
      }
    }
    return path;
  }

  int32_t Add(const std::string& name, int32_t cls, int32_t outer, uint64_t flags, int32_t super = 0,
              int32_t archetype = 0) {
    patch_.exports.push_back({cls, super, outer, name, archetype, flags});
    blobs_.emplace_back();
    NameIndex(name);
    return PatchIndex(patch_.exports.size() - 1);
  }

  int32_t NetIndex() { return next_net_index_++; }
  Bytes& Blob(int32_t index) { return blobs_.at(PatchSlot(index)); }

  // Properties: NetIndex, no tagged properties (None), Next, ArrayDim, PropertyFlags, then the type's
  // references.
  int32_t Property(int32_t outer, const std::string& name, const Prop& p) {
    if (p.kind == "ArrayProperty") {
      // The inner property comes first, with the array (the next export) as its outer, as the
      // script compiler exports them; arrays need constructor linking (CPF_NeedCtorLink).
      const int32_t array = PatchIndex(patch_.exports.size() + 1);
      const int32_t inner = Property(array, name, p.inner.at(0));
      const int32_t index = Add(name, Ref("Core." + p.kind), outer, ue3::RF_PUBLIC_LOADED);
      if (index != array) throw ue3::FormatError{"array property out of order"};
      ue3::Writer w;
      w.I32(NetIndex());
      w.WriteName(none_);
      w.I32(0);
      w.I32(1);
      w.U64(p.flags | CPF_NEED_CTOR_LINK);
      w.I32(inner);
      Blob(index) = w.b;
      return index;
    }
    const int32_t index = Add(name, Ref("Core." + p.kind), outer, ue3::RF_PUBLIC_LOADED);
    ue3::Writer w;
    w.I32(NetIndex());
    w.WriteName(none_);
    w.I32(0);
    w.I32(1);
    w.U64(p.flags);
    if (p.kind == "ObjectProperty" || p.kind == "ComponentProperty") {
      w.I32(Ref(p.extra.at(0)));
    } else if (p.kind == "ClassProperty") {
      w.I32(Ref(p.extra.at(0)));
      w.I32(Ref(p.extra.at(1)));
    } else if (p.kind != "BoolProperty" && p.kind != "IntProperty" && p.kind != "FloatProperty") {
      throw ue3::FormatError{"unknown property kind " + p.kind};
    }
    Blob(index) = w.b;
    return index;
  }

  // Chains fields through UField::Next (at +12 of their data).
  void Link(const std::vector<int32_t>& fields) {
    for (size_t k = 0; k + 1 < fields.size(); ++k) StoreI32(Blob(fields[k]), 12, fields[k + 1]);
  }

  // Class data to inherit from the parent: a GearGame class, or a GearGame class that extends the
  // same Engine class (for flags, probe mask and state flags). Components by name.
  std::pair<int32_t, ue3::ClassData> ParentInfo(const std::string& parent,
                                                std::vector<std::pair<std::string, int32_t>>& components) {
    if (StartsWith(parent, "Engine.")) {
      const int32_t super = Ref(parent);
      int32_t sibling = 0;
      for (size_t i = 0; i < pkg_.exports().size() && !sibling; ++i) {
        if (pkg_.exports()[i].cls == 0 && pkg_.exports()[i].super == super) sibling = int32_t(i + 1);
      }
      if (!sibling) throw ue3::FormatError{"no GearGame class extends " + parent};
      components = ImportComponents(parent);
      return {super, ue3::ParseClass(pkg_.Data(sibling))};
    }
    const int32_t super = pkg_.FindClass(parent);
    if (!super) throw ue3::FormatError{"no class " + parent};
    ue3::ClassData info = ue3::ParseClass(pkg_.Data(super));
    components.clear();
    for (const auto& [name, object] : info.components) components.emplace_back(pkg_.Text(name), object);
    return {super, info};
  }

  // (name, export index) of the patch's subobjects of `default_object`.
  std::map<std::string, int32_t> SubobjectExports(int32_t default_object) const {
    std::map<std::string, int32_t> out;
    for (size_t k = 0; k < patch_.exports.size(); ++k) {
      if (patch_.exports[k].outer == default_object) out.emplace(patch_.exports[k].name, PatchIndex(k));
    }
    return out;
  }

  // The parent default object's properties that point at its subobjects, pointing at ours. A default
  // object's component properties (Mesh, CylinderComponent, Components...) name the subobjects it
  // owns; the new class's default object must name its own copies, or every object of the class
  // would share its parent's templates.
  void DefaultTags(ue3::Writer& w, const std::string& parent, const std::map<std::string, int32_t>& ours) {
    const bool from_engine = StartsWith(parent, "Engine.");
    const ue3::Package& pkg = from_engine ? engine_ : pkg_;
    const int32_t default_object = pkg.Find("Default__" + (from_engine ? AfterPackage(parent) : parent));
    if (!default_object) throw ue3::FormatError{"no default object for " + parent};
    std::map<int32_t, std::string> owned;
    for (int32_t i : pkg.Children(default_object)) owned.emplace(i, pkg.exports()[size_t(i - 1)].name);
    auto ours_of = [&](int32_t object) -> std::optional<int32_t> {
      const auto o = owned.find(object);
      if (o == owned.end()) return std::nullopt;
      const auto it = ours.find(o->second);
      if (it == ours.end()) return std::nullopt;
      return it->second;
    };
    for (const auto& tag : ue3::ParseTags(pkg, pkg.Data(default_object), 4)) {
      if ((tag.kind == "ObjectProperty" || tag.kind == "ComponentProperty") && tag.value.size() == 4) {
        if (const auto ours_object = ours_of(LoadI32(tag.value, 0))) {
          Bytes value(4);
          StoreI32(value, 0, *ours_object);
          Tag(w, tag, value);
        }
      } else if (tag.kind == "ArrayProperty" && tag.value.size() >= 4) {
        const int32_t count = LoadI32(tag.value, 0);
        if (count < 0 || tag.value.size() != 4 + 4 * size_t(count)) continue;
        std::vector<int32_t> items;
        for (int32_t k = 0; k < count; ++k) items.push_back(LoadI32(tag.value, 4 + 4 * size_t(k)));
        bool any = false, all_owned = true;
        for (int32_t i : items) {
          any = any || ours_of(i).has_value();
          all_owned = all_owned && owned.count(i);
        }
        if (!any) continue;
        if (from_engine && !all_owned) continue;  // other Engine objects: not referable from GearGame
        // The parent's own subobjects become ours; anything else stays as it is.
        Bytes value(4 + 4 * items.size());
        StoreI32(value, 0, count);
        for (size_t k = 0; k < items.size(); ++k) StoreI32(value, 4 + 4 * k, ours_of(items[k]).value_or(items[k]));
        Tag(w, tag, value);
      }
    }
  }

  // A tagged property of ours: name, type, size, array index 0, the type's extra and the value.
  Bytes Tagged(const DefaultTag& tag) {
    ue3::Writer value;
    const char* type = nullptr;
    switch (tag.kind) {
      case DefaultTag::kFloat: {
        type = "FloatProperty";
        uint32_t bits;
        std::memcpy(&bits, &tag.number, 4);
        value.U32(bits);
        break;
      }
      case DefaultTag::kBool:
        type = "BoolProperty";
        break;
      case DefaultTag::kName:
        type = "NameProperty";
        value.WriteName(NameIndex(tag.value));
        break;
      case DefaultTag::kEnum:
        type = "ByteProperty";
        value.WriteName(NameIndex(tag.value));
        break;
      case DefaultTag::kStruct:
        type = "StructProperty";
        for (const DefaultTag& inner : tag.structs.at(0)) value.Raw(Tagged(inner));
        value.WriteName(none_);
        break;
      case DefaultTag::kStructs:
        type = "ArrayProperty";
        value.I32(int32_t(tag.structs.size()));
        for (const auto& element : tag.structs) {
          for (const DefaultTag& inner : element) value.Raw(Tagged(inner));
          value.WriteName(none_);
        }
        break;
    }
    ue3::Writer w;
    w.WriteName(NameIndex(tag.name));
    w.WriteName(NameIndex(type));
    w.I32(int32_t(value.b.size()));
    w.I32(0);
    if (tag.kind == DefaultTag::kBool) w.U8(tag.flag ? 1 : 0);
    if (tag.kind == DefaultTag::kStruct || tag.kind == DefaultTag::kEnum) w.WriteName(NameIndex(tag.type));
    w.Raw(value.b);
    return w.b;
  }

  // Writes a tagged property: name, type, size, array index, value (names in this patch).
  void Tag(ue3::Writer& w, const ue3::Tag& tag, const Bytes& value) {
    const int32_t array_index = LoadI32(tag.header, 20);
    w.WriteName(NameIndex(tag.name));
    w.WriteName(NameIndex(tag.kind));
    w.I32(int32_t(value.size()));
    w.I32(array_index);
    w.Raw(value);
  }

  // (name, object) of every object the parent class's default object owns.
  std::vector<std::pair<std::string, int32_t>> Subobjects(const std::string& parent) const {
    if (StartsWith(parent, "Engine.")) return ImportComponents(parent);
    std::vector<std::pair<std::string, int32_t>> out;
    for (int32_t i : pkg_.Children(pkg_.Find("Default__" + parent))) out.emplace_back(pkg_.exports()[size_t(i - 1)].name, i);
    return out;
  }

  // Component templates of an Engine class's default object, as GearGame imports them.
  std::vector<std::pair<std::string, int32_t>> ImportComponents(const std::string& parent) const {
    const int32_t default_object = pkg_.Find("Engine.Default__" + AfterPackage(parent));
    std::vector<std::pair<std::string, int32_t>> out;
    if (!default_object) return out;
    for (size_t k = 0; k < pkg_.imports().size(); ++k) {
      if (pkg_.imports()[k].outer == default_object) out.emplace_back(pkg_.imports()[k].name, -int32_t(k) - 1);
    }
    return out;
  }

  // Whether the component `template_object` is a `class_name` or a subclass of it.
  bool IsA(int32_t template_object, const std::string& class_name) const {
    if (template_object < 0) {
      const std::string& cls = pkg_.imports()[size_t(-template_object - 1)].class_name;
      return cls == class_name || EndsWith(cls, class_name);
    }
    return pkg_.ObjName(pkg_.exports()[size_t(template_object - 1)].cls) == class_name;
  }

  int32_t ComponentClass(int32_t template_object) const {
    if (template_object < 0) {
      const auto& imp = pkg_.imports()[size_t(-template_object - 1)];
      return Ref(imp.class_package + "." + imp.class_name);
    }
    return pkg_.exports()[size_t(template_object - 1)].cls;
  }

  void NewClass(const ClassSpec& spec) {
    std::vector<std::pair<std::string, int32_t>> parent_components;
    const auto [super, info] = ParentInfo(spec.parent, parent_components);
    const bool engine_parent = StartsWith(spec.parent, "Engine.");
    const int32_t cls = Add(spec.name, 0, 0, ue3::RF_PUBLIC_LOADED | ue3::RF_STANDALONE, super);
    const std::string parent_name = engine_parent ? AfterPackage(spec.parent) : spec.parent;
    const int32_t default_object =
        Add("Default__" + spec.name, cls, 0, ue3::RF_PUBLIC_LOADED | ue3::RF_CLASS_DEFAULT_OBJECT, 0,
            Ref(std::string(engine_parent ? "Engine" : "GearGame") + ".Default__" + parent_name));
    // Subobjects: a copy of each of the parent default object's (components, and other objects it owns
    // such as camera shakes), which they take as archetype. The DLC's packages import them by path.
    std::unordered_set<std::string> component_names;
    for (const auto& [name, object] : parent_components) component_names.insert(name);
    std::vector<std::pair<std::string, int32_t>> components;
    for (const auto& [sub_name, template_object] : Subobjects(spec.parent)) {
      const int32_t c = Add(sub_name, ComponentClass(template_object), default_object, ue3::RF_PUBLIC_LOADED, 0,
                            template_object);
      ue3::Writer w;
      const bool is_component = component_names.count(sub_name) > 0;
      if (is_component) {
        // A component: its template's owner class (none) and name, then its properties.
        w.I32(0);
        w.WriteName(NameIndex(sub_name));
      }
      w.I32(NetIndex());
      w.WriteName(none_);
      if (IsA(template_object, "StaticMeshComponent") || IsA(template_object, "BrushComponent")) {
        w.I32(0);  // the component's own data after its properties: an empty array
      }
      Blob(c) = w.b;
      if (is_component) components.emplace_back(sub_name, c);
    }
    {
      ue3::Writer w;
      w.I32(NetIndex());
      DefaultTags(w, spec.parent, SubobjectExports(default_object));
      for (const DefaultTag& tag : spec.defaults) w.Raw(Tagged(tag));
      w.WriteName(none_);
      Blob(default_object) = w.b;
    }
    std::vector<int32_t> properties;
    for (const Prop& p : spec.properties) properties.push_back(Property(cls, p.name, p));
    // Functions and states, chained as the Title Update's classes have them: the states, then the
    // functions from the last declared to the first, then the properties.
    std::vector<int32_t> functions;
    for (const FunctionSpec& f : spec.functions) functions.push_back(Function(f, cls));
    std::vector<int32_t> fields;
    for (const auto& [state_name, script] : spec.states) fields.push_back(State(cls, state_name, script));
    fields.insert(fields.end(), functions.rbegin(), functions.rend());
    fields.insert(fields.end(), properties.begin(), properties.end());
    Link(fields);
    // Class data: no script; flags of a script class.
    uint32_t flags = info.class_flags & ~(ue3::CLASS_NATIVE | ue3::CLASS_NATIVE_REPLICATION | ue3::CLASS_EXPORTED |
                                          ue3::CLASS_ABSTRACT | ue3::CLASS_NOEXPORT | ue3::CLASS_INTRINSIC);
    flags |= ue3::CLASS_COMPILED | ue3::CLASS_PARSED;
    ue3::Writer w;
    w.I32(NetIndex());
    w.I32(0);
    w.I32(super);
    w.I32(fields.empty() ? 0 : fields[0]);
    w.I32(0);
    w.I32(0);
    w.U32(info.probe_mask);
    w.U16(0xFFFF);
    w.U32(info.state_flags);
    // Function map: each function by name, as the engine finds them.
    w.U32(uint32_t(functions.size()));
    for (auto it = functions.rbegin(); it != functions.rend(); ++it) {
      w.WriteName(NameIndex(patch_.exports[PatchSlot(*it)].name));
      w.I32(*it);
    }
    w.U32(flags);
    int32_t within = info.within;
    if (!spec.within.empty() && !(within = pkg_.FindClass(spec.within))) throw ue3::FormatError{"no class " + spec.within};
    w.I32(within);
    w.WriteName(info.config_name.first, info.config_name.second);
    w.U32(uint32_t(components.size()));
    for (const auto& [component_name, c] : components) {
      w.WriteName(NameIndex(component_name));
      w.I32(c);
    }
    w.U32(0);  // interfaces
    w.WriteName(none_);
    w.I32(default_object);
    Blob(cls) = w.b;
  }

  void Member(const std::string& owner, const Prop& prop) {
    const int32_t cls = pkg_.FindClass(owner);
    if (!cls) throw ue3::FormatError{"no class " + owner};
    const int32_t index = Property(cls, prop.name, prop);
    // The disc build's loader links a patch property into its existing class (as its first field,
    // before the one its Next names) and lays the class out again. So that no field the executable
    // knows moves, the property has no elements (ArrayDim 0): it takes no room, and the engine's
    // copying and serialization, which go through ArrayDim elements, leave it alone. An array also
    // leaves the constructor link (no CPF_NeedCtorLink). The runtime gives the property a place
    // afterwards (class_fixups.h); script reaches it by its offset only. Next names the class's first
    // field, so the class keeps all of them.
    Bytes& data = Blob(index);
    StoreI32(data, 12, ue3::ParseClass(pkg_.Data(cls)).children);
    StoreI32(data, 16, 0);
    StoreU64(data, 20, LoadU64(data, 20) & ~CPF_NEED_CTOR_LINK);
  }

  int32_t NewParam(int32_t function, const Param& p) {
    const int32_t index = Add(p.name, Ref("Core." + p.kind), function, ue3::RF_PUBLIC_LOADED);
    ue3::Writer w;
    w.I32(NetIndex());
    w.WriteName(none_);
    w.I32(0);
    w.I32(1);
    w.U64(p.flags);
    if (p.kind == "ObjectProperty") w.I32(Ref(p.cls));
    Blob(index) = w.b;
    return index;
  }

  // A state of the new class `owner`: its code (from its 'Begin' label), label table, no functions of
  // its own.
  int32_t State(int32_t owner, const std::string& name, const StateScript& script) {
    const int32_t state = Add(name, Ref("Core.State"), owner, ue3::RF_PUBLIC_LOADED);
    const bc::AssembledState code = bc::AssembleState(script(*this), {{"Begin", NameIndex("Begin")}}, none_);
    ue3::Writer w;
    w.I32(NetIndex());
    w.WriteName(none_);
    w.I32(0);
    w.I32(0);
    w.I32(0);
    w.I32(int32_t(code.memory_size));
    w.I32(int32_t(code.code.size()));
    w.Raw(code.code);
    w.U32(0);  // probe mask
    w.U16(code.label_table);
    w.U32(0);  // state flags
    w.U32(0);  // function map
    Blob(state) = w.b;
    return state;
  }

  // Fields of a disc function, in order (Children, then each field's Next).
  std::vector<int32_t> Chain(int32_t function) const {
    std::vector<int32_t> fields;
    for (int32_t field = LoadI32(pkg_.Data(function), 20); field; field = LoadI32(pkg_.Data(field), 12)) {
      fields.push_back(field);
    }
    return fields;
  }

  // A copy of the disc parameter `model` in `function`.
  int32_t CopiedParam(int32_t function, int32_t model) {
    const ue3::Export& e = pkg_.exports()[size_t(model - 1)];
    const int32_t index = Add(e.name, e.cls, function, ue3::RF_PUBLIC_LOADED);
    Bytes copy = pkg_.Data(model);
    StoreI32(copy, 0, NetIndex());
    StoreI32(copy, 12, 0);
    Blob(index) = copy;
    return index;
  }

  // A function of a disc class (spec.owner), or of the new class `new_owner` (then its Next is set when
  // the class's fields are chained).
  int32_t Function(const FunctionSpec& spec, int32_t new_owner = 0) {
    const int32_t model = pkg_.Find(spec.like);
    if (!model) throw ue3::FormatError{spec.like + " is not in GearGame"};
    const Bytes data = pkg_.Data(model);
    ue3::Reader r(data);
    r.I32();
    r.ReadName();
    r.I32();
    r.I32();
    r.I32();  // NetIndex, None, Next, Super, Children
    const int32_t model_memory_size = r.I32();
    const Bytes model_code = r.Take(size_t(r.I32()));
    Bytes tail(data.begin() + r.pos(), data.end());
    // Native index (16 bits), operator precedence (8), function flags (32) and, for replicated
    // functions, the replication offset. Not native and not final: the DLC overrides them.
    tail.at(0) = tail.at(1) = 0;
    StoreI32(tail, 3, int32_t(uint32_t(LoadI32(tail, 3)) & ~(FUNC_FINAL | FUNC_NATIVE)));
    const int32_t owner = new_owner ? new_owner : pkg_.FindClass(spec.owner);
    if (!owner) throw ue3::FormatError{"no class " + spec.owner};
    const int32_t function = Add(spec.name, Ref("Core.Function"), owner, ue3::RF_PUBLIC_LOADED);
    std::vector<int32_t> fields;
    Fields by_name;
    if (!spec.params_from.empty()) {
      const int32_t from = pkg_.Find(spec.params_from);
      if (!from) throw ue3::FormatError{spec.params_from + " is not in GearGame"};
      for (const int32_t field : Chain(from)) {
        if (!(LoadU64(pkg_.Data(field), 20) & CPF_PARM)) continue;
        fields.push_back(CopiedParam(function, field));
        by_name.emplace(pkg_.exports()[size_t(field - 1)].name, fields.back());
      }
    }
    for (const Param& p : spec.params) {
      fields.push_back(NewParam(function, p));
      by_name.emplace(p.name, fields.back());
    }
    Link(fields);
    Bytes code;
    int32_t memory_size = 0;
    switch (spec.body.kind) {
      case Body::kEmpty:
        code = {EX_RETURN, EX_NOTHING, EX_END_OF_SCRIPT};
        memory_size = int32_t(code.size());
        break;
      case Body::kCopy: {
        // The model's bytecode, references replaced: the same layout, so the same size in memory.
        code = model_code;
        for (const auto& [path, replacement] : spec.body.replace) {
          const int32_t old_ref = pkg_.Find(path);
          const auto param = by_name.find(replacement);
          const int32_t new_ref = param != by_name.end() ? param->second : Ref(replacement);
          Bytes old_bytes(4), new_bytes(4);
          StoreI32(old_bytes, 0, old_ref);
          StoreI32(new_bytes, 0, new_ref);
          bool found = false;
          for (size_t at = 0; at + 4 <= code.size();) {
            if (std::equal(old_bytes.begin(), old_bytes.end(), code.begin() + at)) {
              std::copy(new_bytes.begin(), new_bytes.end(), code.begin() + at);
              found = true;
              at += 4;
            } else {
              ++at;
            }
          }
          if (!old_ref || !found) throw ue3::FormatError{spec.like + " does not name " + path};
        }
        memory_size = model_memory_size;
        break;
      }
      case Body::kScript: {
        auto [assembled, size] = bc::Assemble(spec.body.script(*this, by_name));
        code = std::move(assembled);
        memory_size = int32_t(size);
        break;
      }
    }
    ue3::Writer w;
    // Next names the class's first field: the disc build's loader may link the function into the class
    // along with the class's new properties (see Member), and the class keeps its fields.
    const int32_t first = new_owner ? 0 : ue3::ParseClass(pkg_.Data(owner)).children;
    w.I32(NetIndex());
    w.WriteName(none_);
    w.I32(first);
    w.I32(0);
    w.I32(fields.empty() ? 0 : fields[0]);
    // Bytecode size in memory, where each object reference takes 8 bytes (4 in the file), then in
    // the file.
    w.I32(memory_size);
    w.I32(int32_t(code.size()));
    w.Raw(code);
    w.Raw(tail);
    Blob(function) = w.b;
    return function;
  }

  const ue3::Package& pkg_;
  const ue3::Package& engine_;  // for the default objects of Engine parents
  ue3::Patch patch_;
  std::vector<Bytes> blobs_;
  int32_t next_net_index_;
  int32_t none_;
};

// LP = LocalPlayer(Player);
// if (LP != None) {
//     LP.InsertPostProcessingChain(Chain, -1, true);
//     EffectToEnable = MaterialEffect(LP.PlayerPostProcess.FindPostProcessEffect(ChainName));
//     if (EffectToEnable != None) {
//         MIC = new class'MaterialInstanceConstant';
//         MIC.SetParent(EffectToEnable.Material);
//         EffectToEnable.Material = MIC;
//         EffectToEnable.bShowInGame = true;
//     }
// }
bc::Code EnablePostProcessEffects(Builder& b, const Fields& f) {
  const bc::Code lp = bc::Local(f.at("LP")), effect = bc::Local(f.at("EffectToEnable")), mic = bc::Local(f.at("MIC"));
  const int32_t material = b.Ref("Engine.MaterialEffect.Material");
  const int32_t show = b.Ref("Engine.PostProcessEffect.bShowInGame");
  const int32_t post_process = b.Ref("Engine.LocalPlayer.PlayerPostProcess");
  const int32_t find = b.Ref("Engine.PostProcessChain.FindPostProcessEffect");
  return {
      bc::Let(lp, bc::Cast(b.Ref("Engine.LocalPlayer"), bc::Instance(b.Ref("Engine.PlayerController.Player")))),
      bc::JumpIfNot("end", bc::Native(bc::NOT_EQUAL_OBJECT_OBJECT, {lp, bc::NoObject()})),
      bc::Context(lp,
                  bc::Call(b.NameIndex("InsertPostProcessingChain"),
                           {bc::Local(f.at("Chain")), bc::IntConst(-1), bc::TrueConst()}),
                  b.Ref("Engine.LocalPlayer.InsertPostProcessingChain.ReturnValue")),
      bc::Let(effect, bc::Cast(b.Ref("Engine.MaterialEffect"),
                               bc::Context(bc::Context(lp, bc::Instance(post_process), post_process),
                                           bc::CallFinal(find, {bc::Local(f.at("ChainName"))}),
                                           b.Ref("Engine.PostProcessChain.FindPostProcessEffect.ReturnValue")))),
      bc::JumpIfNot("end", bc::Native(bc::NOT_EQUAL_OBJECT_OBJECT, {effect, bc::NoObject()})),
      bc::Let(mic, bc::New(b.Ref("Engine.MaterialInstanceConstant"))),
      bc::Context(mic, bc::Call(b.NameIndex("SetParent"), {bc::Context(effect, bc::Instance(material), material)})),
      bc::Let(bc::Context(effect, bc::Instance(material), material), mic),
      bc::LetBool(bc::Context(effect, bc::BoolVariable(bc::Instance(show)), show), bc::TrueConst()),
      bc::Label("end"),
      bc::Return(),
      bc::End(),
  };
}

// if (EffectToDisable != None) EffectToDisable.bShowInGame = false;
// LP = LocalPlayer(Player);
// if (LP != None) {
//     LP.ClearPostProcessSettingsOverride();
//     for (I = 1; I < LP.PlayerPostProcessChains.Length; I++)
//         if (LP.PlayerPostProcessChains[I].FindPostProcessEffect(ChainName) != None)
//             LP.RemovePostProcessingChain(I--);
// }
bc::Code DisablePostProcessEffects(Builder& b, const Fields& f) {
  const bc::Code lp = bc::Local(f.at("LP")), effect = bc::Local(f.at("EffectToDisable")), i = bc::Local(f.at("I"));
  const int32_t show = b.Ref("Engine.PostProcessEffect.bShowInGame");
  const int32_t chains = b.Ref("Engine.LocalPlayer.PlayerPostProcessChains");
  return {
      bc::JumpIfNot("player", bc::Native(bc::NOT_EQUAL_OBJECT_OBJECT, {effect, bc::NoObject()})),
      bc::LetBool(bc::Context(effect, bc::BoolVariable(bc::Instance(show)), show), bc::FalseConst()),
      bc::Label("player"),
      bc::Let(lp, bc::Cast(b.Ref("Engine.LocalPlayer"), bc::Instance(b.Ref("Engine.PlayerController.Player")))),
      bc::JumpIfNot("end", bc::Native(bc::NOT_EQUAL_OBJECT_OBJECT, {lp, bc::NoObject()})),
      bc::Context(lp, bc::Call(b.NameIndex("ClearPostProcessSettingsOverride"), {bc::EmptyParm()})),
      bc::Let(i, bc::IntOne()),
      bc::Label("loop"),
      bc::JumpIfNot("end", bc::Native(bc::LESS_INT_INT, {i, bc::ArrayLength(bc::Context(lp, bc::Instance(chains), chains))})),
      bc::JumpIfNot("next", bc::Native(bc::NOT_EQUAL_OBJECT_OBJECT,
                                       {bc::Context(bc::ArrayElement(i, bc::Context(lp, bc::Instance(chains), chains)),
                                                    bc::CallFinal(b.Ref("Engine.PostProcessChain.FindPostProcessEffect"),
                                                                  {bc::Local(f.at("ChainName"))}),
                                                    b.Ref("Engine.PostProcessChain.FindPostProcessEffect.ReturnValue")),
                                        bc::NoObject()})),
      bc::Context(lp, bc::Call(b.NameIndex("RemovePostProcessingChain"), {i}),
                  b.Ref("Engine.LocalPlayer.RemovePostProcessingChain.ReturnValue")),
      bc::Native(bc::SUBTRACT_SUBTRACT_INT, {i}),
      bc::Label("next"),
      bc::Native(bc::ADD_ADD_INT, {i}),
      bc::Jump("loop"),
      bc::Label("end"),
      bc::Return(),
      bc::End(),
  };
}

// Super.SpecialMoveStarted(bForced, PrevMove); PlayFall(0.0); PawnOwner.Velocity.Z = FallStartSpeedZ;
bc::Code GrapplingDropStarted(Builder& b, const Fields& f) {
  const int32_t velocity = b.Ref("Engine.Actor.Velocity");
  return {
      bc::CallFinal(b.Ref("GearGame.GSM_BaseVariableFall.SpecialMoveStarted"),
                    {bc::BoolVariable(bc::Local(f.at("bForced"))), bc::Local(f.at("PrevMove"))}),
      bc::Call(b.NameIndex("PlayFall"), {bc::FloatConst(0.0f)}),
      bc::Let(bc::StructMember(b.Ref("Core.Object.Vector.Z"), b.Ref("Core.Object.Vector"),
                               bc::Context(bc::Instance(b.Ref("GearGame.GearSpecialMove.PawnOwner")), bc::Instance(velocity), velocity)),
              bc::Instance(b.Ref("GearGame.GSM_GrapplingHook_DynamicClimbDrop.FallStartSpeedZ"))),
      bc::Return(),
      bc::End(),
  };
}

// Seconds between the Brumak's attacks on its scripted targets: the DLC's GearAI_Brumak_DLC sets it
// (AttackScriptedTargetInterval), in a property the Title Update adds to GearAI_Brumak, which the
// patch cannot add (a native class with a subclass the disc build loads with it).
constexpr float kBrumakAttackInterval = 13.0f;

// static function float EvaluateActivation(GearAI AI, UtilBasedAICmdMetaDataPacket ActivationData):
// return 1.2 when the AI has targets, the Brumak's main gun is not destroyed, and the interval has
// passed since the AI was made and since this command last ran; -1 otherwise.
bc::Code BrumakEvaluateActivation(Builder& b, const Fields& f) {
  const bc::Code ai = bc::Local(f.at("AI"));
  const int32_t world = b.Ref("Engine.Actor.WorldInfo"), now = b.Ref("Engine.WorldInfo.TimeSeconds");
  const int32_t created = b.Ref("Engine.Actor.CreationTime"), targets = b.Ref("GearGame.GearAI.TargetList");
  const int32_t pawn = b.Ref("Engine.Controller.Pawn");
  const int32_t last = b.Ref("GearGame.UtilBasedAICmdMetaDataPacket.LastActivationTime");
  auto since = [&](const bc::Code& start) {
    return bc::Native(bc::SUBTRACT_FLOAT_FLOAT,
                      {bc::Context(bc::Context(ai, bc::Instance(world), world), bc::Instance(now), now), start});
  };
  const bc::Code interval = bc::FloatConst(kBrumakAttackInterval);
  return {
      bc::JumpIfNot(
          "no",
          bc::AndAlso(
              bc::AndAlso(
                  bc::AndAlso(
                      bc::Native(bc::GREATER_INT_INT,
                                 {bc::ArrayLength(bc::Context(ai, bc::Instance(targets), targets)), bc::IntOne()}),
                      bc::Native(bc::NOT_PRE_BOOL,
                                 {bc::Context(bc::Cast(b.Ref("GearGame.GearPawn_LocustBrumakBase"),
                                                       bc::Context(ai, bc::Instance(pawn), pawn)),
                                              bc::CallFinal(b.Ref("GearGame.GearPawn_LocustBrumakBase.IsMainGunDestroyed"), {}),
                                              b.Ref("GearGame.GearPawn_LocustBrumakBase.IsMainGunDestroyed.ReturnValue"))})),
                  bc::Native(bc::GREATER_FLOAT_FLOAT,
                             {since(bc::Context(ai, bc::Instance(created), created)), interval})),
              bc::Native(bc::GREATER_FLOAT_FLOAT,
                         {since(bc::Context(bc::Local(f.at("ActivationData")), bc::Instance(last), last)), interval}))),
      bc::Return(bc::FloatConst(1.2f)),
      bc::Label("no"),
      bc::Return(bc::FloatConst(-1.0f)),
      bc::ReturnNothing(f.at("ReturnValue")),
      bc::End(),
  };
}

// Super.Pushed(); GotoState('AttackTruck');
bc::Code BrumakPushed(Builder& b, const Fields&) {
  return {
      bc::CallFinal(b.Ref("GameFramework.GameAICommand.Pushed"), {}),
      bc::Native(bc::GOTO_STATE, {bc::NameConst(b.NameIndex("AttackTruck")), bc::EmptyParm(), bc::EmptyParm(), bc::EmptyParm()}),
      bc::Return(),
      bc::End(),
  };
}

// Outer.<prop> (the command's outer is its AI, a GearAI_Brumak).
bc::Code BrumakOuter(Builder& b, const std::string& prop) {
  const int32_t member = b.Ref(prop);
  return bc::Context(bc::Instance(b.Ref("Core.Object.Outer")), bc::Instance(member), member);
}

// Super.Popped(); each gun's AI selects its enemy again; Outer.Focus = Outer.FireTarget;
bc::Code BrumakPopped(Builder& b, const Fields&) {
  auto select = [&](const char* gun) {
    return bc::Context(BrumakOuter(b, gun), bc::Call(b.NameIndex("SelectEnemy"), {}),
                       b.Ref("GearGame.GearAI_Brumak_Slave.SelectEnemy.ReturnValue"));
  };
  return {
      bc::CallFinal(b.Ref("GearGame.AICommand.Popped"), {}),
      select("GearGame.GearAI_Brumak.LeftGunAI"),
      select("GearGame.GearAI_Brumak.RightGunAI"),
      select("GearGame.GearAI_Brumak.DriverAI"),
      bc::Let(BrumakOuter(b, "Engine.Controller.Focus"), BrumakOuter(b, "GearGame.GearAI.FireTarget")),
      bc::Return(),
      bc::End(),
  };
}

// Begin: if (Outer.SelectScriptedTarget()) { Outer.DriverAI.SetScriptedFireTarget(Outer.FireTarget,
// false, 3.0); Outer.DriverAI.FireRockets(Outer); } Outer.PopCommand(self); Stop;
bc::Code BrumakAttackTruck(Builder& b) {
  const bc::Code outer = bc::Instance(b.Ref("Core.Object.Outer"));
  const bc::Code driver = BrumakOuter(b, "GearGame.GearAI_Brumak.DriverAI");
  const bc::Code fire_target = BrumakOuter(b, "GearGame.GearAI.FireTarget");
  return {
      bc::Label("Begin"),
      bc::JumpIfNot("pop", bc::Context(outer, bc::Call(b.NameIndex("SelectScriptedTarget"), {}),
                                       b.Ref("GearGame.GearAI.SelectScriptedTarget.ReturnValue"), true)),
      bc::Context(driver, bc::Call(b.NameIndex("SetScriptedFireTarget"), {fire_target, bc::FalseConst(), bc::FloatConst(3.0f)})),
      bc::Context(driver, bc::Call(b.NameIndex("FireRockets"), {outer})),
      bc::Label("pop"),
      bc::Context(outer, bc::Call(b.NameIndex("PopCommand"), {bc::Self()}), 0, true),
      bc::Stop(),
  };
}

// --- The tool ------------------------------------------------------------------------------------

std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

// Prints the one line the launcher reads, and returns the exit code that goes with it.
int Report(const std::string& json, int exit_code) {
  std::fputs((json + "\n").c_str(), stdout);
  std::fflush(stdout);
  return exit_code;
}

int Fail(const std::string& message) { return Report("{\"ok\":false,\"error\":" + JsonString(message) + "}", 1); }

std::optional<Bytes> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A file of the disc image (.iso) mounted as device, by its path inside the image (backslashes).
std::optional<Bytes> ReadDiscFile(rex::filesystem::DiscImageDevice& device, std::string_view path) {
  using rex::X_STATUS;
  rex::filesystem::Entry* entry = device.ResolvePath(path);
  if (!entry) return std::nullopt;
  rex::filesystem::File* file = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS || !file) {
    return std::nullopt;
  }
  Bytes data(entry->size());
  size_t bytes_read = 0;
  X_STATUS status = file->ReadSync(std::span<uint8_t>(data.data(), data.size()), 0, &bytes_read);
  file->Destroy();
  if (status != X_STATUS_SUCCESS || bytes_read != data.size()) return std::nullopt;
  return data;
}

bool WriteFile(const std::filesystem::path& path, const Bytes& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

}  // namespace

std::vector<uint8_t> BuildRaamsShadowPatch(std::vector<uint8_t> gear_game, std::vector<uint8_t> engine) {
  const ue3::Package pkg(std::move(gear_game));
  const ue3::Package engine_pkg(std::move(engine));
  return Builder(pkg, engine_pkg).Build();
}

bool DlcPatchRequested() { return !REXCVAR_GET(make_dlc_patch).empty(); }

int MakeDlcPatch(const std::filesystem::path& game_data_root) {
  const std::filesystem::path out = REXCVAR_GET(make_dlc_patch);
  // The game data root is a folder, or a disc image (.iso) read in place as the runtime mounts it
  // (Runtime::SetupVfs).
  std::unique_ptr<rex::filesystem::DiscImageDevice> disc;
  if (std::filesystem::is_regular_file(game_data_root)) {
    disc = std::make_unique<rex::filesystem::DiscImageDevice>("", game_data_root);
    if (!disc->Initialize()) return Fail("the game's disc image could not be read");
  }
  const std::filesystem::path cooked = game_data_root / "GearGame" / "CookedXbox360";
  auto read_package = [&](const char* name) -> std::optional<Bytes> {
    const auto file = disc ? ReadDiscFile(*disc, std::string("GearGame\\CookedXbox360\\") + name)
                           : ReadFile(cooked / name);
    return file ? DecompressChunk(*file) : std::nullopt;
  };
  const auto gear_game = read_package("GearGame.xxx");
  if (!gear_game) return Fail("GearGame.xxx is missing or could not be read");
  const auto engine = read_package("Engine.xxx");
  if (!engine) return Fail("Engine.xxx is missing or could not be read");
  Bytes patch;
  try {
    patch = BuildRaamsShadowPatch(*gear_game, *engine);
  } catch (const ue3::FormatError& e) {
    return Fail("these game files do not have what the patch needs: " + e.what);
  }
  const std::filesystem::path folder = out / "GearGame" / "Patches";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) return Fail("cannot create " + folder.string());
  const std::string size_text = std::to_string(patch.size()) + "\r\n";
  if (!WriteFile(folder / "ScriptPatch_GearGame.bin", PackStored(patch)) ||
      !WriteFile(folder / "ScriptPatch_GearGame.bin_us", Bytes(size_text.begin(), size_text.end()))) {
    return Fail("cannot write the patch into " + folder.string());
  }
  return Report("{\"ok\":true,\"patch_size\":" + std::to_string(patch.size()) + "}", 0);
}

}  // namespace azura::dlc
