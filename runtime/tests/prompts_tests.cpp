// Unit tests for the button prompt engine: the Xenos DXT5 codec and tiling, keybind parsing, and
// the glyph designs. They use only synthetic data, never anything from the game's files.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "../src/prompts/glyph_art.h"
#include "../src/prompts/glyph_raster.h"
#include "../src/prompts/xenos_dxt5.h"

using namespace azura::prompts;

namespace {

// Builds a block in standard little-endian DXT5 layout and converts it to the 360 byte order
// (every 16-bit word big-endian), as it would sit in guest memory.
Block XenosBlock(uint8_t a0, uint8_t a1, uint64_t alpha_bits, uint16_t c0, uint16_t c1, uint32_t color_bits) {
  Block le{};
  le[0] = a0;
  le[1] = a1;
  for (int i = 0; i < 6; ++i) le[2 + i] = static_cast<uint8_t>(alpha_bits >> (8 * i));
  le[8] = static_cast<uint8_t>(c0);
  le[9] = static_cast<uint8_t>(c0 >> 8);
  le[10] = static_cast<uint8_t>(c1);
  le[11] = static_cast<uint8_t>(c1 >> 8);
  for (int i = 0; i < 4; ++i) le[12 + i] = static_cast<uint8_t>(color_bits >> (8 * i));
  Block xenos{};
  for (int i = 0; i < 16; i += 2) {
    xenos[i] = le[i + 1];
    xenos[i + 1] = le[i];
  }
  return xenos;
}

int MaxChannelError(const std::array<Rgba, 16>& a, const std::array<Rgba, 16>& b, bool ignore_transparent_rgb) {
  int worst = 0;
  for (int p = 0; p < 16; ++p) {
    for (int c = 0; c < 4; ++c) {
      if (ignore_transparent_rgb && c < 3 && a[p][3] == 0) continue;
      worst = std::max(worst, std::abs(int(a[p][c]) - int(b[p][c])));
    }
  }
  return worst;
}

// Draws a single-key PC prompt.
std::vector<uint8_t> Key(const std::string& name, int w = 26, int h = 26) {
  return DrawPcGlyph({PcPrompt::Layout::kSingle, {name}}, w, h);
}

}  // namespace

// --- Tiling --------------------------------------------------------------------------------------

TEST_CASE("TiledBlockOffset matches the reference Xenos tiling", "[prompts][tiling]") {
  // Reference values from the Python implementation the atlases were first decoded with.
  CHECK(TiledBlockOffset(0, 0, 64) == 0);
  CHECK(TiledBlockOffset(1, 0, 64) == 32);
  CHECK(TiledBlockOffset(0, 1, 64) == 16);
  CHECK(TiledBlockOffset(7, 3, 64) == 1840);
  CHECK(TiledBlockOffset(8, 8, 64) == 8384);
  CHECK(TiledBlockOffset(31, 31, 64) == 16240);
  CHECK(TiledBlockOffset(63, 63, 64) == 65392);
  CHECK(TiledBlockOffset(5, 2, 32) == 1568);
  CHECK(TiledBlockOffset(100, 77, 128) == 193168);
}

TEST_CASE("TiledBlockOffset is a permutation of the texture's blocks", "[prompts][tiling]") {
  // A 256x256 DXT5 texture: 64x64 blocks of 16 bytes fill exactly 64 KB, each block once.
  std::set<uint32_t> seen;
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      const uint32_t off = TiledBlockOffset(x, y, 64);
      CHECK(off % 16 == 0);
      CHECK(off < 65536);
      seen.insert(off);
    }
  }
  CHECK(seen.size() == 64 * 64);
}

// --- DXT5 codec ----------------------------------------------------------------------------------

TEST_CASE("DecodeDxt5 reads the 360 byte order", "[prompts][dxt5]") {
  // Opaque pure red everywhere: alpha endpoint 0 (255) and colour endpoint 0 (RGB565 red) for all
  // 16 pixels.
  const auto px = DecodeDxt5(XenosBlock(255, 0, 0, 0xF800, 0x001F, 0));
  for (const auto& p : px) {
    CHECK(p == Rgba{255, 0, 0, 255});
  }
  // Index 1 everywhere selects the second endpoints: alpha 0 and pure blue.
  const uint64_t alpha_ones = 0x249249249249ull;  // sixteen 3-bit indices of value 1
  const uint32_t color_ones = 0x55555555u;        // sixteen 2-bit indices of value 1
  const auto px1 = DecodeDxt5(XenosBlock(255, 0, alpha_ones, 0xF800, 0x001F, color_ones));
  for (const auto& p : px1) {
    CHECK(p == Rgba{0, 0, 255, 0});
  }
}

TEST_CASE("EncodeDxt5 round-trips solid and two-tone blocks", "[prompts][dxt5]") {
  std::array<Rgba, 16> solid;
  solid.fill(Rgba{255, 168, 40, 255});  // the amber of the PC glyphs
  CHECK(MaxChannelError(solid, DecodeDxt5(EncodeDxt5(solid)), false) <= 8);

  std::array<Rgba, 16> two_tone;
  for (int p = 0; p < 16; ++p) {
    two_tone[p] = p % 2 ? Rgba{9, 6, 4, 255} : Rgba{255, 228, 150, 255};
  }
  CHECK(MaxChannelError(two_tone, DecodeDxt5(EncodeDxt5(two_tone)), false) <= 8);
}

TEST_CASE("EncodeDxt5 keeps an alpha ramp and ignores transparent colours", "[prompts][dxt5]") {
  std::array<Rgba, 16> ramp;
  for (int p = 0; p < 16; ++p) {
    // Transparent pixels carry a wild colour that must not pull the endpoints.
    ramp[p] = p < 4 ? Rgba{0, 255, 0, 0} : Rgba{255, 168, 40, static_cast<uint8_t>(p * 17)};
  }
  const auto out = DecodeDxt5(EncodeDxt5(ramp));
  CHECK(MaxChannelError(ramp, out, true) <= 20);
  for (int p = 0; p < 4; ++p) {
    CHECK(out[p][3] == 0);
  }
}

// --- Keybind parsing -----------------------------------------------------------------------------

TEST_CASE("PrimaryKeyName picks the key the prompt shows", "[prompts][bindings]") {
  CHECK(PrimaryKeyName("Space") == "Space");
  CHECK(PrimaryKeyName("F,Backspace") == "F");
  CHECK(PrimaryKeyName(" RMB , Q ") == "RMB");
  CHECK(PrimaryKeyName("Shift+W,Q") == "W");
  CHECK(PrimaryKeyName("Shift+Ctrl+Up") == "Up");
  CHECK(PrimaryKeyName("Shift") == "Shift");
  CHECK(PrimaryKeyName("NumpadPlus") == "NumpadPlus");
  CHECK(PrimaryKeyName("") == "");
}

// --- Glyph designs -------------------------------------------------------------------------------

TEST_CASE("Every bindable key has its own PC glyph", "[prompts][glyphs]") {
  // Every name rex::ui::ParseVirtualKey accepts (src/ui/keybinds.cpp in the SDK). A name the
  // glyphs do not know is drawn as "?", so each must differ from an unknown name's glyph.
  const char* names[] = {
      "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15",
      "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "A", "B", "C", "D", "E", "F", "G",
      "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "0",
      "1", "2", "3", "4", "5", "6", "7", "8", "9", "Backtick", "Minus", "Plus", "Comma", "Period",
      "Semicolon", "Slash", "Backslash", "LBracket", "RBracket", "Quote", "Escape", "Return", "Space",
      "Tab", "Backspace", "Delete", "Insert", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up",
      "Down", "Shift", "Control", "Ctrl", "Alt", "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
      "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9", "NumpadEnter", "Enter", "NumpadPlus",
      "NumpadMinus", "NumpadStar", "NumpadSlash", "PrintScreen", "Pause", "CapsLock", "NumLock",
      "ScrollLock", "LMB", "RMB", "MMB", "MiddleMouse", "MB4", "Mouse4", "XButton1", "MB5", "Mouse5",
      "XButton2"};
  const auto unknown = Key("NotAKey");
  for (const char* name : names) {
    INFO("key " << name);
    CHECK(Key(name) != unknown);
  }
}

TEST_CASE("Key names are matched case-insensitively", "[prompts][glyphs]") {
  CHECK(Key("space") == Key("Space"));
  CHECK(Key("lmb") == Key("LMB"));
  CHECK(Key("f12") == Key("F12"));
}

TEST_CASE("Distinct keys draw distinct glyphs", "[prompts][glyphs]") {
  CHECK(Key("MB4") != Key("MB5"));
  CHECK(Key("LMB") != Key("RMB"));
  CHECK(Key("Numpad1") != Key("1"));
  CHECK(Key("Up") != Key("Down"));
}

TEST_CASE("Glyphs fill their slot at every in-game size", "[prompts][glyphs]") {
  const std::array<std::pair<int, int>, 5> sizes = {{{18, 26}, {22, 26}, {24, 24}, {29, 29}, {32, 32}}};
  auto visible = [](const std::vector<uint8_t>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4)
      if (rgba[i] > 0) return true;
    return false;
  };
  const std::vector<PcPrompt> pc = {
      {PcPrompt::Layout::kSingle, {"Q"}},
      {PcPrompt::Layout::kSingle, {"LMB"}},
      {PcPrompt::Layout::kMouseLook, {}},
      {PcPrompt::Layout::kCross, {"W", "A", "S", "D"}},
      {PcPrompt::Layout::kPair, {"Up", "Down"}},
  };
  for (auto [w, h] : sizes) {
    for (PsModel model : {PsModel::kDualSense, PsModel::kDualShock4}) {
      for (int b = 0; b <= static_cast<int>(PadButton::kDpadLeftRight); ++b) {
        const auto glyph = DrawPlayStationGlyph(static_cast<PadButton>(b), w, h, model);
        CHECK(glyph.size() == static_cast<size_t>(w * h * 4));
        CHECK(visible(glyph));
      }
    }
    for (const auto& prompt : pc) {
      const auto glyph = DrawPcGlyph(prompt, w, h);
      CHECK(glyph.size() == static_cast<size_t>(w * h * 4));
      CHECK(visible(glyph));
    }
  }
}

TEST_CASE("Letters and digits are drawn smooth with the legend font", "[prompts][glyphs]") {
  // Loaded last: the cases above check the pixel fonts used without it.
  REQUIRE(LoadLegendFont(AZURA_LEGEND_FONT));
  const Coverage w = TextCoverage("W", 9.0f);
  REQUIRE_FALSE(w.empty());
  CHECK(w.height == 9);
  // Antialiased: edge pixels are partly covered, not only on or off.
  CHECK(std::any_of(w.alpha.begin(), w.alpha.end(), [](float a) { return a > 0.1f && a < 0.9f; }));
  // The narrowest WASD slot still gets the four keys, and the glyph fills its slot.
  const auto cross = DrawPcGlyph({PcPrompt::Layout::kCross, {"W", "A", "S", "D"}}, 18, 26);
  CHECK(cross.size() == static_cast<size_t>(18 * 26 * 4));
}
