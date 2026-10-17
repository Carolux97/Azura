// Reader and writer for Unreal Engine 3 "coalesced" files (Coalesced_<LANG>.bin, Xbox 360).
//
// A coalesced file holds every .ini and localization file of the game in one big-endian blob:
//   int32 file count, then per file: name, int32 section count, then per section: name,
//   int32 key count, then per key: name, value.
// Strings are UE3 FStrings: an int32 length N; N > 0 is N bytes of Latin-1 including the
// terminating zero, N < 0 is -N UTF-16LE code units including the terminating zero, 0 is empty.
//
// Each string keeps the encoding it was read with, so an unedited file is written back byte for
// byte.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azura::menu {

struct CoalescedString {
  std::u16string text;  // without the terminating zero
  bool wide = false;    // stored as UTF-16 (else Latin-1)
  bool empty_marker = false;  // stored with length 0

  static CoalescedString FromAscii(const std::string& ascii);
  // Text that may hold non-ASCII characters; stored as UTF-16 when it does.
  static CoalescedString FromUtf8(const std::string& utf8);
  std::string Ascii() const;  // characters above 0x7F become '?'
};

struct CoalescedKey {
  CoalescedString name;
  CoalescedString value;
};

struct CoalescedSection {
  CoalescedString name;
  std::vector<CoalescedKey> keys;
};

struct CoalescedFile {
  CoalescedString name;  // path as stored, e.g. "..\..\GearGame\Config\Xbox360-GearUI.ini"
  std::vector<CoalescedSection> sections;
};

struct Coalesced {
  std::vector<CoalescedFile> files;
};

// Parses `data`; nullopt when it is not a well-formed coalesced file.
std::optional<Coalesced> ParseCoalesced(const std::vector<uint8_t>& data);
std::vector<uint8_t> SerializeCoalesced(const Coalesced& coalesced);

}  // namespace azura::menu
