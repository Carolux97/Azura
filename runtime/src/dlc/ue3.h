// Unreal Engine 3 packages and script patches (Gears of War 3, Xbox 360), for Azura's DLC script
// patch. The C++ version of tools/dlc/ue3.py.
//
// Reads a cooked package (file version 828, big-endian) once it is uncompressed: summary, name table,
// imports, exports, and the serialized data of classes and default objects. Writes script patches
// (FLinkerPatchData), the format the engine reads from GearGame/Patches/ScriptPatch_<Package>.bin.
//
// Object references are 32-bit indices: positive for exports (1-based), negative for imports, 0 for
// none. A patch's own exports come after the package's: export N + 1 is the patch's first.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace azura::dlc::ue3 {

using Bytes = std::vector<uint8_t>;

// Object flags (64-bit), as the engine writes them for script objects.
constexpr uint64_t RF_PUBLIC_LOADED = 0x0007000400000000;  // public, loaded on clients, servers and in the editor
constexpr uint64_t RF_STANDALONE = 0x0008000000000000;
constexpr uint64_t RF_CLASS_DEFAULT_OBJECT = 0x0000000000000200;

// Class flags.
constexpr uint32_t CLASS_ABSTRACT = 0x1;
constexpr uint32_t CLASS_COMPILED = 0x2;
constexpr uint32_t CLASS_PARSED = 0x10;
constexpr uint32_t CLASS_NATIVE = 0x80;
constexpr uint32_t CLASS_NOEXPORT = 0x100;
constexpr uint32_t CLASS_NATIVE_REPLICATION = 0x800;
constexpr uint32_t CLASS_EXPORTED = 0x08000000;
constexpr uint32_t CLASS_INTRINSIC = 0x10000000;

// A name reference: index into the name table and number (0 for none, n for name_<n - 1>).
using Name = std::pair<int32_t, int32_t>;

// Thrown when data does not have the layout expected.
struct FormatError {
  std::string what;
};

class Reader {
 public:
  Reader(const Bytes& data, size_t pos = 0) : data_(data), pos_(pos) {}
  uint8_t U8();
  uint16_t U16();
  uint32_t U32();
  int32_t I32() { return static_cast<int32_t>(U32()); }
  uint64_t U64();
  std::string FString();
  Name ReadName();
  Bytes Take(size_t n);
  size_t pos() const { return pos_; }
  void set_pos(size_t pos) { pos_ = pos; }

 private:
  void Need(size_t n) const;
  const Bytes& data_;
  size_t pos_;
};

class Writer {
 public:
  void U8(uint8_t v) { b.push_back(v); }
  void U16(uint16_t v);
  void U32(uint32_t v);
  void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
  void U64(uint64_t v);
  void Raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
  void FString(const std::string& s);
  void WriteName(int32_t index, int32_t number = 0) {
    I32(index);
    I32(number);
  }
  Bytes b;
};

struct Import {
  std::string class_package, class_name;
  int32_t outer = 0;
  std::string name;
};

struct Export {
  int32_t cls = 0, super = 0, outer = 0;
  std::string name;
  int32_t archetype = 0;
  uint64_t flags = 0;
  int32_t size = 0, offset = 0;
};

class Package {
 public:
  explicit Package(Bytes data);

  int32_t net_object_count() const { return net_object_count_; }
  const std::vector<std::string>& names() const { return names_; }
  const std::vector<Import>& imports() const { return imports_; }
  const std::vector<Export>& exports() const { return exports_; }
  // Index of a name in the name table, or -1.
  int32_t NameIndex(const std::string& text) const;
  std::string Text(const Name& name) const;
  // Name of an object reference ("None" for 0).
  std::string ObjName(int32_t index) const;
  int32_t Outer(int32_t index) const;
  // Full path of an object reference, from the package (Outer.Outer.Name).
  std::string Path(int32_t index) const;
  // Class name of an object reference ("Class" for a class export).
  std::string ClassName(int32_t index) const;
  // Serialized data of an export.
  Bytes Data(int32_t index) const;
  // Index of the export or import with this path (Package.Outer.Name for imports), 0 if none.
  int32_t Find(const std::string& path) const;
  // The class export with this name, 0 if none.
  int32_t FindClass(const std::string& name) const;
  // Exports whose outer is `outer`, in order.
  std::vector<int32_t> Children(int32_t outer) const;

 private:
  Bytes data_;
  int32_t net_object_count_ = 0;
  std::vector<std::string> names_;
  std::unordered_map<std::string, int32_t> name_index_;
  std::vector<Import> imports_;
  std::vector<Export> exports_;
  std::unordered_map<std::string, int32_t> paths_;
  std::unordered_map<int32_t, std::vector<int32_t>> children_;
};

// Fields of a serialized UClass (version 828, cooked).
struct ClassData {
  int32_t net_index = 0, next = 0, super = 0, children = 0;
  uint32_t probe_mask = 0;
  uint16_t label_table_offset = 0;
  uint32_t state_flags = 0;
  uint32_t class_flags = 0;
  int32_t within = 0;
  Name config_name;
  std::vector<std::pair<Name, int32_t>> components;
  int32_t default_object = 0;
};
ClassData ParseClass(const Bytes& blob);

// A tagged property: its header bytes (name, type, size, array index and the type's extra), name,
// type and value.
struct Tag {
  Bytes header;
  std::string name, kind;
  Bytes value;
};
// Tagged properties at `pos` (after the object's NetIndex), up to None.
std::vector<Tag> ParseTags(const Package& pkg, const Bytes& blob, size_t pos);

// A script patch (FLinkerPatchData) to write.
struct PatchExport {
  int32_t cls = 0, super = 0, outer = 0;
  std::string name;
  int32_t archetype = 0;
  uint64_t flags = 0;
};

struct Patch {
  std::string package;
  std::vector<std::string> names;
  std::vector<PatchExport> exports;
  std::vector<std::pair<std::string, Bytes>> new_objects;  // (export name, data), one per export, in order
  Bytes Write() const;
};

}  // namespace azura::dlc::ue3
