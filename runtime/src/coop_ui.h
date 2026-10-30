// The look of Azura's own screens over the game (the Local co-op menu and its notices, the
// performance overlay): the launcher's colours (launcher/src/styles.css), its panels with cut
// corners, and icons of the input devices drawn after the real ones. Everything draws on an ImGui
// draw list; sizes are in pixels.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/input/device.h>
#include <rex/input/pad_remap.h>

namespace rex::input {
class InputSystem;
}

namespace azura::coop_ui {

// The launcher's colours (launcher/src/styles.css).
constexpr ImU32 Panel(int alpha) { return IM_COL32(20, 22, 25, alpha); }
constexpr ImU32 PanelSolid(int alpha) { return IM_COL32(22, 24, 27, alpha); }
constexpr ImU32 Steel(int alpha) { return IM_COL32(38, 42, 47, alpha); }
constexpr ImU32 SteelLine(int alpha) { return IM_COL32(54, 59, 66, alpha); }
constexpr ImU32 Imulsion(int alpha) { return IM_COL32(255, 184, 28, alpha); }
constexpr ImU32 Text(int alpha) { return IM_COL32(236, 230, 218, alpha); }
constexpr ImU32 TextDim(int alpha) { return IM_COL32(162, 156, 144, alpha); }
constexpr ImU32 TextFaint(int alpha) { return IM_COL32(109, 105, 96, alpha); }
// The launcher's state colours: --ok, and the text of its error badges (on --crimson).
constexpr ImU32 Ok(int alpha) { return IM_COL32(143, 191, 106, alpha); }
constexpr ImU32 ErrorText(int alpha) { return IM_COL32(255, 136, 136, alpha); }

// `text` cut with an ellipsis to fit `max_width`.
std::string Fit(ImFont* font, float size, const std::string& text, float max_width);

// Draws `text` with the middle of its ink (the glyphs' drawn pixels, not the font's line box) at
// `center`, so a short label sits in the middle of a key cap or a button.
void AddTextCentered(ImDrawList* draw_list, ImFont* font, float size, ImVec2 center, ImU32 color, const char* text);

// A panel with the launcher's cut corners (top left and bottom right).
void DrawCutPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float cut, ImU32 fill, ImU32 line,
                  float thickness);

// A keyboard with a mouse beside it, `size` tall.
void DrawKeyboardIcon(ImDrawList* draw_list, ImVec2 center, float size, ImU32 body, ImU32 detail);

// The look of a controller family, as the real controllers: body colours and layout.
enum class PadLook { kDualSense, kDualShock4, kXbox, kSwitchPro, kGeneric };
PadLook LookOf(const rex::input::DeviceInfo& device);

// A controller, `size` tall, drawn after the real one of its family.
void DrawControllerIcon(ImDrawList* draw_list, ImVec2 center, float size, PadLook look, int alpha);

// The controls of a controller drawn by DrawControllerIcon, for pointing at them.
enum class PadPoint {
  kA, kB, kX, kY, kLeftShoulder, kRightShoulder, kLeftTrigger, kRightTrigger,
  kLeftStick, kRightStick, kDpad, kBack, kStart,
};
// Where `point` is on the controller DrawControllerIcon draws at `center`, `size` tall. The
// shoulders and the triggers, not drawn, are on its top edge; Back and Start where they sit on the
// real controller.
ImVec2 PadPointPosition(ImVec2 center, float size, PadLook look, PadPoint point);

// A small left or right arrow, for the button hints.
void DrawArrow(ImDrawList* draw_list, ImVec2 center, float size, bool left, ImU32 color);

// The name of a shoulder, trigger, stick or menu button on the family `look` ("LB", "L1", "Options").
const char* ControlName(rex::input::PadControl control, PadLook look);

// A control's glyph as a controller of the family `look` shows it (the face buttons by their shapes
// or letters and colours, the others by their names on a key cap), centred on `center`, `height`
// tall. Returns its width.
float DrawPadGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 center, float height, PadLook look,
                   rex::input::PadControl control, int alpha, float unit);

// The button that confirms, as `pad` shows it (the cross on PlayStation, A on the others), or the
// Enter key named `enter` when `pad` is null; its top left at `top_left`, 16 * `unit` tall. Returns
// its width.
float DrawConfirmGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 top_left, float unit,
                       const rex::input::DeviceInfo* pad, const char* enter, int alpha);

// The button that goes back, as `pad` shows it (the circle on PlayStation, B on the others), or the
// key named `escape` when `pad` is null; drawn as DrawConfirmGlyph. Returns its width.
float DrawCancelGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 top_left, float unit,
                      const rex::input::DeviceInfo* pad, const char* escape, int alpha);

// Whether a key or button that could have closed a menu is still held: Enter, Escape or Backspace
// on the keyboard, or any button or trigger of the controllers `pads`. A menu that blocks the
// game's input waits for them to be released before it closes, so the game does not take the
// same press as its own (Escape would go back in the Options too).
bool CloseInputHeld(rex::input::InputSystem* input, const std::vector<rex::input::DeviceId>& pads);

// The input device the player last used in one of our menus: the keyboard and mouse, or a
// controller. It starts as player 1's active device and follows every key, mouse button and
// controller press, so a menu's glyphs follow the device in the player's hands as it changes.
class LastUsedInput {
 public:
  explicit LastUsedInput(rex::input::InputSystem* input);
  // Notes a key or mouse button pressed this frame (ImGui's), if any.
  void NoteKeyboard();
  // Notes a press on controller `id`.
  void NotePad(rex::input::DeviceId id) {
    keyboard_ = false;
    pad_ = id;
  }
  bool keyboard() const { return keyboard_; }
  // The controller last pressed, kInvalid until one is (or when the menu opened from the keyboard).
  rex::input::DeviceId pad() const { return pad_; }

 private:
  bool keyboard_ = false;
  rex::input::DeviceId pad_ = rex::input::DeviceId::kInvalid;
};

}  // namespace azura::coop_ui
