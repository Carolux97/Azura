#include "coalesced.h"

namespace azura::menu {

namespace {

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

  bool I32(int32_t& out) {
    if (pos_ + 4 > data_.size()) return false;
    out = static_cast<int32_t>(uint32_t(data_[pos_]) << 24 | uint32_t(data_[pos_ + 1]) << 16 |
                               uint32_t(data_[pos_ + 2]) << 8 | uint32_t(data_[pos_ + 3]));
    pos_ += 4;
    return true;
  }

  bool String(CoalescedString& out) {
    int32_t n = 0;
    if (!I32(n)) return false;
    out = {};
    if (n == 0) {
      out.empty_marker = true;
      return true;
    }
    if (n < 0) {
      const size_t units = static_cast<size_t>(-int64_t(n));
      if (pos_ + units * 2 > data_.size()) return false;
      out.wide = true;
      for (size_t i = 0; i < units; ++i) {
        out.text.push_back(static_cast<char16_t>(data_[pos_ + i * 2] | data_[pos_ + i * 2 + 1] << 8));
      }
      pos_ += units * 2;
    } else {
      if (pos_ + size_t(n) > data_.size()) return false;
      for (int32_t i = 0; i < n; ++i) out.text.push_back(static_cast<char16_t>(data_[pos_ + i]));
      pos_ += size_t(n);
    }
    if (!out.text.empty() && out.text.back() == u'\0') out.text.pop_back();
    return true;
  }

  bool AtEnd() const { return pos_ == data_.size(); }

 private:
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
};

void PutI32(std::vector<uint8_t>& out, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  out.insert(out.end(), {uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u)});
}

void PutString(std::vector<uint8_t>& out, const CoalescedString& s) {
  if (s.empty_marker && s.text.empty()) {
    PutI32(out, 0);
    return;
  }
  const size_t units = s.text.size() + 1;  // with the terminating zero
  if (s.wide) {
    PutI32(out, -static_cast<int32_t>(units));
    for (char16_t c : s.text) out.insert(out.end(), {uint8_t(c), uint8_t(c >> 8)});
    out.insert(out.end(), {0, 0});
  } else {
    PutI32(out, static_cast<int32_t>(units));
    for (char16_t c : s.text) out.push_back(static_cast<uint8_t>(c));
    out.push_back(0);
  }
}

}  // namespace

CoalescedString CoalescedString::FromAscii(const std::string& ascii) {
  CoalescedString s;
  for (unsigned char c : ascii) s.text.push_back(c);
  return s;
}

CoalescedString CoalescedString::FromUtf8(const std::string& utf8) {
  CoalescedString s;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = utf8[i];
    char32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c >> 5) == 6 && i + 1 < utf8.size()) {
      cp = (c & 0x1F) << 6 | (utf8[i + 1] & 0x3F);
      len = 2;
    } else if ((c >> 4) == 14 && i + 2 < utf8.size()) {
      cp = (c & 0x0F) << 12 | (utf8[i + 1] & 0x3F) << 6 | (utf8[i + 2] & 0x3F);
      len = 3;
    } else {
      cp = u'?';
      len = 1;
    }
    s.text.push_back(static_cast<char16_t>(cp));  // the game's strings stay in the BMP
    if (cp > 0x7F) s.wide = true;  // non-ASCII is stored as UTF-16, which the game always reads right
    i += len;
  }
  return s;
}

std::string CoalescedString::Ascii() const {
  std::string out;
  for (char16_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

std::optional<Coalesced> ParseCoalesced(const std::vector<uint8_t>& data) {
  Reader r(data);
  Coalesced result;
  int32_t file_count = 0;
  if (!r.I32(file_count) || file_count < 0) return std::nullopt;
  for (int32_t f = 0; f < file_count; ++f) {
    CoalescedFile file;
    int32_t section_count = 0;
    if (!r.String(file.name) || !r.I32(section_count) || section_count < 0) return std::nullopt;
    for (int32_t s = 0; s < section_count; ++s) {
      CoalescedSection section;
      int32_t key_count = 0;
      if (!r.String(section.name) || !r.I32(key_count) || key_count < 0) return std::nullopt;
      section.keys.resize(static_cast<size_t>(key_count));
      for (auto& key : section.keys) {
        if (!r.String(key.name) || !r.String(key.value)) return std::nullopt;
      }
      file.sections.push_back(std::move(section));
    }
    result.files.push_back(std::move(file));
  }
  if (!r.AtEnd()) return std::nullopt;
  return result;
}

std::vector<uint8_t> SerializeCoalesced(const Coalesced& coalesced) {
  std::vector<uint8_t> out;
  PutI32(out, static_cast<int32_t>(coalesced.files.size()));
  for (const auto& file : coalesced.files) {
    PutString(out, file.name);
    PutI32(out, static_cast<int32_t>(file.sections.size()));
    for (const auto& section : file.sections) {
      PutString(out, section.name);
      PutI32(out, static_cast<int32_t>(section.keys.size()));
      for (const auto& key : section.keys) {
        PutString(out, key.name);
        PutString(out, key.value);
      }
    }
  }
  return out;
}

}  // namespace azura::menu
