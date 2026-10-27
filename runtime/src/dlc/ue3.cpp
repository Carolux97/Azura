// Unreal Engine 3 packages and script patches. See ue3.h.

#include "ue3.h"

#include "lzx.h"

namespace azura::dlc::ue3 {

// --- Reader --------------------------------------------------------------------------------------

void Reader::Need(size_t n) const {
  if (pos_ + n > data_.size()) throw FormatError{"data ends early"};
}

uint8_t Reader::U8() {
  Need(1);
  return data_[pos_++];
}

uint16_t Reader::U16() {
  Need(2);
  const uint16_t v = uint16_t((data_[pos_] << 8) | data_[pos_ + 1]);
  pos_ += 2;
  return v;
}

uint32_t Reader::U32() {
  Need(4);
  const uint32_t v = (uint32_t(data_[pos_]) << 24) | (uint32_t(data_[pos_ + 1]) << 16) |
                     (uint32_t(data_[pos_ + 2]) << 8) | data_[pos_ + 3];
  pos_ += 4;
  return v;
}

uint64_t Reader::U64() {
  const uint64_t high = U32();
  return (high << 32) | U32();
}

std::string Reader::FString() {
  const int32_t n = I32();
  std::string s;
  if (n < 0) {
    // UTF-16, big-endian: only the characters the engine's names use (Latin-1).
    const size_t count = size_t(-int64_t(n));
    Need(count * 2);
    for (size_t i = 0; i < count; ++i) {
      const uint16_t c = uint16_t((data_[pos_ + 2 * i] << 8) | data_[pos_ + 2 * i + 1]);
      s.push_back(c < 0x100 ? char(c) : '?');
    }
    pos_ += count * 2;
  } else {
    Need(size_t(n));
    s.assign(reinterpret_cast<const char*>(data_.data() + pos_), size_t(n));
    pos_ += size_t(n);
  }
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

Name Reader::ReadName() {
  const int32_t index = I32();
  return {index, I32()};
}

Bytes Reader::Take(size_t n) {
  Need(n);
  Bytes v(data_.begin() + pos_, data_.begin() + pos_ + n);
  pos_ += n;
  return v;
}

// --- Writer --------------------------------------------------------------------------------------

void Writer::U16(uint16_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}

void Writer::U32(uint32_t v) {
  U16(uint16_t(v >> 16));
  U16(uint16_t(v));
}

void Writer::U64(uint64_t v) {
  U32(uint32_t(v >> 32));
  U32(uint32_t(v));
}

void Writer::FString(const std::string& s) {
  I32(int32_t(s.size() + 1));
  b.insert(b.end(), s.begin(), s.end());
  b.push_back(0);
}

// --- Package -------------------------------------------------------------------------------------

Package::Package(Bytes data) : data_(std::move(data)) {
  Reader r(data_);
  if (r.U32() != kPackageTag) throw FormatError{"not a package"};
  r.U32();      // file version
  r.I32();      // header size
  r.FString();  // folder name
  r.U32();      // package flags
  const int32_t name_count = r.I32(), name_offset = r.I32();
  const int32_t export_count = r.I32(), export_offset = r.I32();
  const int32_t import_count = r.I32(), import_offset = r.I32();
  r.I32();    // depends offset
  r.Take(16);  // import/export guids and thumbnail table
  r.Take(16);  // package guid
  const int32_t generations = r.I32();
  if (generations <= 0) throw FormatError{"no generations"};
  for (int32_t g = 0; g < generations; ++g) {
    r.I32();
    r.I32();
    net_object_count_ = r.I32();  // the last generation's
  }

  r.set_pos(size_t(name_offset));
  for (int32_t i = 0; i < name_count; ++i) {
    names_.push_back(r.FString());
    r.U64();
    name_index_.emplace(names_.back(), i);
  }

  r.set_pos(size_t(import_offset));
  for (int32_t i = 0; i < import_count; ++i) {
    const Name class_package = r.ReadName(), class_name = r.ReadName();
    const int32_t outer = r.I32();
    const Name name = r.ReadName();
    imports_.push_back({names_.at(size_t(class_package.first)), names_.at(size_t(class_name.first)), outer, Text(name)});
  }

  r.set_pos(size_t(export_offset));
  for (int32_t i = 0; i < export_count; ++i) {
    Export e;
    e.cls = r.I32();
    e.super = r.I32();
    e.outer = r.I32();
    e.name = Text(r.ReadName());
    e.archetype = r.I32();
    e.flags = r.U64();
    e.size = r.I32();
    e.offset = r.I32();
    r.U32();  // export flags
    for (int32_t n = r.I32(); n > 0; --n) r.I32();  // generation net object counts
    r.Take(16);                                    // package guid
    r.U32();                                       // package flags
    exports_.push_back(std::move(e));
  }
  for (int32_t i = 1; i <= export_count; ++i) {
    paths_.emplace(Path(i), i);
    children_[exports_[size_t(i - 1)].outer].push_back(i);
  }
  for (int32_t k = 0; k < import_count; ++k) paths_.emplace(Path(-k - 1), -k - 1);
}

int32_t Package::NameIndex(const std::string& text) const {
  const auto it = name_index_.find(text);
  return it == name_index_.end() ? -1 : it->second;
}

std::string Package::Text(const Name& name) const {
  const std::string& base = names_.at(size_t(name.first));
  return name.second == 0 ? base : base + "_" + std::to_string(name.second - 1);
}

std::string Package::ObjName(int32_t index) const {
  if (index == 0) return "None";
  return index > 0 ? exports_.at(size_t(index - 1)).name : imports_.at(size_t(-index - 1)).name;
}

int32_t Package::Outer(int32_t index) const {
  return index > 0 ? exports_.at(size_t(index - 1)).outer : imports_.at(size_t(-index - 1)).outer;
}

std::string Package::Path(int32_t index) const {
  std::string path;
  for (int depth = 0; index && depth < 64; ++depth) {
    path = path.empty() ? ObjName(index) : ObjName(index) + "." + path;
    index = Outer(index);
  }
  return path;
}

std::string Package::ClassName(int32_t index) const {
  if (index < 0) return imports_.at(size_t(-index - 1)).class_name;
  const int32_t cls = exports_.at(size_t(index - 1)).cls;
  return cls == 0 ? "Class" : ObjName(cls);
}

Bytes Package::Data(int32_t index) const {
  const Export& e = exports_.at(size_t(index - 1));
  if (e.offset < 0 || e.size < 0 || size_t(e.offset) + size_t(e.size) > data_.size()) {
    throw FormatError{"export data outside the package"};
  }
  return Bytes(data_.begin() + e.offset, data_.begin() + e.offset + e.size);
}

int32_t Package::Find(const std::string& path) const {
  const auto it = paths_.find(path);
  return it == paths_.end() ? 0 : it->second;
}

int32_t Package::FindClass(const std::string& name) const {
  for (size_t i = 0; i < exports_.size(); ++i) {
    if (exports_[i].cls == 0 && exports_[i].name == name) return int32_t(i + 1);
  }
  return 0;
}

std::vector<int32_t> Package::Children(int32_t outer) const {
  const auto it = children_.find(outer);
  return it == children_.end() ? std::vector<int32_t>{} : it->second;
}

// --- Classes and tagged properties ---------------------------------------------------------------

ClassData ParseClass(const Bytes& blob) {
  Reader r(blob);
  ClassData c;
  c.net_index = r.I32();
  c.next = r.I32();
  c.super = r.I32();
  c.children = r.I32();
  r.I32();  // bytecode size in memory
  r.Take(size_t(r.I32()));
  c.probe_mask = r.U32();
  c.label_table_offset = r.U16();
  c.state_flags = r.U32();
  for (int32_t n = r.I32(); n > 0; --n) {  // function map
    r.ReadName();
    r.I32();
  }
  c.class_flags = r.U32();
  c.within = r.I32();
  c.config_name = r.ReadName();
  for (int32_t n = r.I32(); n > 0; --n) {
    const Name name = r.ReadName();
    c.components.emplace_back(name, r.I32());
  }
  for (int32_t n = r.I32(); n > 0; --n) {  // interfaces
    r.I32();
    r.I32();
  }
  r.ReadName();  // DLL bind name
  c.default_object = r.I32();
  if (r.pos() != blob.size()) throw FormatError{"unexpected class data"};
  return c;
}

std::vector<Tag> ParseTags(const Package& pkg, const Bytes& blob, size_t pos) {
  Reader r(blob, pos);
  std::vector<Tag> tags;
  while (true) {
    const size_t start = r.pos();
    const std::string name = pkg.Text(r.ReadName());
    if (name == "None") break;
    const std::string kind = pkg.Text(r.ReadName());
    const int32_t size = r.I32();
    r.I32();  // array index
    if (kind == "BoolProperty") {
      r.U8();
    } else if (kind == "StructProperty" || kind == "ByteProperty") {
      r.ReadName();
    }
    Tag tag;
    tag.header.assign(blob.begin() + start, blob.begin() + r.pos());
    tag.name = name;
    tag.kind = kind;
    tag.value = r.Take(size_t(size));
    tags.push_back(std::move(tag));
  }
  return tags;
}

// --- Patches -------------------------------------------------------------------------------------

Bytes Patch::Write() const {
  Writer w;
  w.FString(package);
  w.U32(uint32_t(names.size()));
  for (const auto& n : names) w.FString(n);
  w.U32(uint32_t(exports.size()));
  for (const auto& e : exports) {
    w.I32(e.cls);
    w.I32(e.super);
    w.I32(e.outer);
    w.FString(e.name);
    w.I32(e.archetype);
    w.U64(e.flags);
    w.Raw(Bytes(36));  // serial size and offset, export flags, net counts, guid, package flags
  }
  w.U32(0);  // imports
  w.U32(uint32_t(new_objects.size()));
  for (const auto& [name, data] : new_objects) {
    w.FString(name);
    w.U32(uint32_t(data.size()));
    w.Raw(data);
  }
  w.U32(0);  // modified default objects
  w.U32(0);  // enums
  w.U32(0);  // script patches
  return w.b;
}

}  // namespace azura::dlc::ue3
