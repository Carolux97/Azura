// Renders every button prompt glyph into one PNG, to review the designs without running the game.
//
// Rows: the PlayStation glyphs, the PC glyphs for the default bindings, then one PC glyph for every
// key name a binding can use. Each glyph is drawn at an in-game slot size and then magnified with
// nearest-neighbour so single pixels stay visible.
//
// Build: cmake --build --preset mac-arm64-sdk-source-relwithdebinfo --target azura_glyph_sheet
// Usage: azura_glyph_sheet <output.png> [scale]   (scale defaults to 4)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/prompts/glyph_art.h"

using namespace azura::prompts;

namespace {

// --- Minimal PNG writer (stored deflate blocks, no compression) ---------------------------------

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutU32(png, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png.insert(png.end(), body.begin(), body.end());
  PutU32(png, Crc32(body.data(), body.size()));
}

bool WritePng(const char* path, int w, int h, const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size(); pos += 65535) {
    const size_t len = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + len == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(len));
    z.push_back(static_cast<uint8_t>(len >> 8));
    z.push_back(static_cast<uint8_t>(~len));
    z.push_back(static_cast<uint8_t>(~len >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
  }
  PutU32(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutU32(ihdr, static_cast<uint32_t>(w));
  PutU32(ihdr, static_cast<uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

// --- Sheet layout -------------------------------------------------------------------------------

struct Item {
  int w, h;
  std::vector<uint8_t> rgba;
};

constexpr int kCell = 32;
constexpr uint8_t kBackground[3] = {14, 15, 19};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <output.png> [scale]\n", argv[0]);
    return 1;
  }
  const int scale = argc > 2 ? std::max(1, std::atoi(argv[2])) : 4;

  std::vector<std::vector<Item>> rows;
  using B = PadButton;
  const B buttons[] = {B::kA, B::kB, B::kX, B::kY, B::kLeftTrigger, B::kRightTrigger, B::kLeftShoulder,
                   B::kRightShoulder, B::kStart, B::kBack, B::kLeftStick, B::kRightStick,
                   B::kLeftStickPress, B::kRightStickPress, B::kDpadAll, B::kDpadUp, B::kDpadUpDown};
  const int pad_w[] = {24, 26, 24, 24, 22, 22, 27, 27, 28, 28, 18, 18, 29, 29, 24, 20, 24};
  std::vector<Item> ps;
  for (size_t i = 0; i < std::size(buttons); ++i) ps.push_back({pad_w[i], 26, DrawPlayStationGlyph(buttons[i], pad_w[i], 26)});
  rows.push_back(std::move(ps));

  // PC prompts for the default bindings, in the same order and sizes.
  const std::vector<PcPrompt> defaults = {
      {PcPrompt::Layout::kSingle, {"Space"}},  {PcPrompt::Layout::kSingle, {"F"}},
      {PcPrompt::Layout::kSingle, {"E"}},      {PcPrompt::Layout::kSingle, {"V"}},
      {PcPrompt::Layout::kSingle, {"RMB"}},    {PcPrompt::Layout::kSingle, {"LMB"}},
      {PcPrompt::Layout::kSingle, {"Q"}},      {PcPrompt::Layout::kSingle, {"R"}},
      {PcPrompt::Layout::kSingle, {"Escape"}}, {PcPrompt::Layout::kSingle, {"Tab"}},
      {PcPrompt::Layout::kCross, {"W", "A", "S", "D"}}, {PcPrompt::Layout::kMouseLook, {}},
      {PcPrompt::Layout::kSingle, {"Shift"}},  {PcPrompt::Layout::kSingle, {"MMB"}},
      {PcPrompt::Layout::kCross, {"Up", "Left", "Down", "Right"}}, {PcPrompt::Layout::kSingle, {"Up"}},
      {PcPrompt::Layout::kPair, {"Up", "Down"}},
  };
  std::vector<Item> pc;
  for (size_t i = 0; i < defaults.size(); ++i) {
    pc.push_back({pad_w[i], 26, DrawPcGlyph(defaults[i], pad_w[i], 26, static_cast<uint32_t>(i * 7 + 11))});
  }
  rows.push_back(std::move(pc));

  // Every bindable key, at a typical slot size.
  const char* keys[] = {
      "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
      "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F1", "F5", "F10",
      "F12", "F24", "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash",
      "LBracket", "RBracket", "Quote", "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert",
      "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down", "Shift", "Control", "Alt",
      "Numpad0", "Numpad7", "NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash",
      "PrintScreen", "Pause", "CapsLock", "NumLock", "ScrollLock", "LMB", "RMB", "MMB", "MB4", "MB5",
      "Shift+W", "Nonsense"};
  std::vector<Item> all;
  for (size_t i = 0; i < std::size(keys); ++i) {
    const std::string name = PrimaryKeyName(keys[i]);
    all.push_back({26, 26, DrawPcGlyph({PcPrompt::Layout::kSingle, {name}}, 26, 26, static_cast<uint32_t>(i * 13 + 5))});
    if (all.size() == 20) {
      rows.push_back(std::move(all));
      all.clear();
    }
  }
  if (!all.empty()) rows.push_back(std::move(all));

  size_t cols = 0;
  for (const auto& r : rows) cols = std::max(cols, r.size());
  const int pad = 2;
  const int W = static_cast<int>(cols) * (kCell + pad) * scale + pad * scale;
  const int H = static_cast<int>(rows.size()) * (kCell + pad) * scale + pad * scale;
  std::vector<uint8_t> rgb(static_cast<size_t>(W * H * 3));
  for (size_t i = 0; i < rgb.size(); i += 3) std::copy(kBackground, kBackground + 3, rgb.begin() + i);
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t c = 0; c < rows[r].size(); ++c) {
      const Item& it = rows[r][c];
      const int ox = (pad + static_cast<int>(c) * (kCell + pad) + (kCell - it.w) / 2) * scale;
      const int oy = (pad + static_cast<int>(r) * (kCell + pad) + (kCell - it.h) / 2) * scale;
      for (int y = 0; y < it.h * scale; ++y) {
        for (int x = 0; x < it.w * scale; ++x) {
          const uint8_t* p = &it.rgba[static_cast<size_t>(((y / scale) * it.w + x / scale) * 4)];
          uint8_t* d = &rgb[static_cast<size_t>(((oy + y) * W + ox + x) * 3)];
          const float a = p[3] / 255.0f;
          for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(p[k] * a + d[k] * (1 - a));
        }
      }
    }
  }
  if (!WritePng(argv[1], W, H, rgb)) {
    std::fprintf(stderr, "Could not write %s\n", argv[1]);
    return 1;
  }
  return 0;
}
