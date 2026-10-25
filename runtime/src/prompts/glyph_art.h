// The button prompt designs: PlayStation (DualSense) glyphs and the PC keyboard and mouse glyphs.
//
// The PC glyphs follow the Gears of War 3 key art: black silhouettes with a slightly ragged edge,
// rimmed by amber light, with amber legends. The rim is opaque and crisp, not a soft glow, so the
// glyphs sit in the game's text as cleanly as its own. Every key a binding can name has a face here,
// so the prompts follow whatever the player binds.
//
// Each Draw* function returns a straight-alpha RGBA8 image of exactly width x height pixels.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace azura::prompts {

// A controller button as the game's prompts show it. Some prompts stand for a group (the whole
// D-pad, or two of its directions).
enum class PadButton : uint8_t {
  kA,
  kB,
  kX,
  kY,
  kLeftTrigger,
  kRightTrigger,
  kLeftShoulder,
  kRightShoulder,
  kStart,
  kBack,
  kLeftStick,
  kRightStick,
  kLeftStickPress,
  kRightStickPress,
  kDpadAll,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kDpadUpDown,
  kDpadLeftRight,
};

// The PlayStation controller the glyphs are drawn for: the DualSense's action button symbols are
// grey, the DualShock 4's coloured.
enum class PsModel : uint8_t { kDualSense, kDualShock4 };

std::vector<uint8_t> DrawPlayStationGlyph(PadButton button, int width, int height, PsModel model);

// What a PC prompt shows.
struct PcPrompt {
  enum class Layout : uint8_t {
    kSingle,     // one key or mouse button: keys[0]
    kMouseLook,  // the mouse with movement arrows
    kCross,      // four keys in a WASD-style group: keys = {up, left, down, right}
    kPair,       // two keys side by side: keys = {first, second}
  };
  Layout layout = Layout::kSingle;
  std::vector<std::string> keys;  // key names as written in the keybind_* settings
};

std::vector<uint8_t> DrawPcGlyph(const PcPrompt& prompt, int width, int height);

// Reduces a keybind value to the key its prompt shows: the first alternative of a comma-separated
// list, without modifier prefixes ("Shift+W,Q" -> "W"). A bare modifier ("Shift") stays as is.
std::string PrimaryKeyName(std::string_view bind_value);

}  // namespace azura::prompts
