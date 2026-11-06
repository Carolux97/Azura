// The graphics menu: the Video settings in a menu of ours, opened from the Video category of the
// game's Options (menu/pc_options.h: UseGraphicsMenu) instead of the game's list of rows. It has the
// look of the Alboria menu (connection_menu.h): a dark steel panel with cut corners over the dimmed
// game, imulsion gold for the title and the accents.
//
// The rows are grouped in four sections, in a list that scrolls:
// - Image: brightness (opens the game's calibration screen), full screen, upscaling preset, render
//   resolution, image scaling and its sharpness (only with an effect that sharpens);
// - Performance: frame rate limit, dynamic shadows, depth of field, anisotropic filtering;
// - Effects: anti-aliasing, motion blur, film grain, ambient occlusion, bloom, lens flares, field
//   of view;
// - Overlay: the performance overlay and its corner.
// The focused row's description is shown under the list. While an upscaling preset other than
// custom is chosen, the render resolution and the image scaling show, dimmed, what the preset picks
// for this screen; changing either turns the preset to custom, starting from those values. Rows that
// only apply at the next start show a tag once changed, and closing the menu in the main menu
// offers to restart the game, as leaving the game's Video list did.
//
// Each change is applied and saved to azura.toml at once (the same keys as the game's list). Up and
// down choose a row, left and right change it, A changes it forward (or opens the calibration), X
// puts every Video setting back to its default (pressed twice: the first press asks), B or Start
// close the menu; on the keyboard the arrows (or W, A, S, D), Enter or Space, R and Escape; with
// the mouse, pointing at a row chooses it, a click changes it (on its left arrow, backwards), a
// click on the defaults hint stands for X, and the wheel scrolls. While it is open the game gets no input (PlayerAssignment::SetBlocked).

#pragma once

#include <chrono>
#include <cstdint>
#include <map>

#include <rex/input/device.h>
#include <rex/ui/imgui_dialog.h>

#include "coop_ui.h"

struct ImFont;

namespace rex::input {
class InputSystem;
}

namespace azura {

namespace input {
class PlayerAssignment;
}

class GraphicsMenu : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the menu closes and deletes itself. `language` is MenuLanguage()
  // (an Xbox 360 language ID, or Czech or Hungarian). The fonts may be null (ImGui's default is
  // used). Opened again when the brightness calibration it opened closes, it starts on that row.
  GraphicsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input, input::PlayerAssignment* assignment,
               uint32_t language, ImFont* title_font, ImFont* body_font, GraphicsMenu** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // What the controllers and the keyboard asked for this frame.
  struct Presses {
    int move = 0;    // -1 up, 1 down
    int change = 0;  // -1 left, 1 right
    bool accept = false;
    bool defaults = false;
    bool close = false;
  };
  Presses ReadInput();
  // Changes row `row` (an index in the menu's rows) by `direction` (-1 or 1), or forward and
  // wrapping around with 0 (A, Enter, a click); the brightness row opens the calibration screen.
  void Change(int row, int direction);
  // Moves the focus by `step` rows among the ones shown.
  void MoveFocus(int step);
  // The defaults button: asks first, then puts every Video setting back to its default.
  void Defaults();

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  GraphicsMenu** owner_;
  Clock::time_point opened_;
  coop_ui::LastUsedInput last_input_;
  // Each controller's buttons the frame before (the left stick's direction as D-pad bits). A
  // controller seen for the first time does nothing that frame: the press that opened the menu
  // does not also act in it.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  // A direction held on a controller repeats: the directions held and when they next repeat.
  uint16_t repeat_buttons_ = 0;
  Clock::time_point repeat_at_{};
  int focus_ = 0;
  // How far the list is scrolled, in pixels, and whether it must scroll to the focused row (the
  // focus moved with a controller or the keyboard).
  float scroll_ = 0.0f;
  bool scroll_to_focus_ = true;
  // The defaults button was pressed once: until then a second press puts the defaults back.
  Clock::time_point confirm_defaults_until_{};
  // A press closed the menu, which closes once it is released (coop_ui::CloseInputHeld).
  bool closing_ = false;
  // The menu closed to open the brightness calibration: it opens again after it.
  bool to_calibration_ = false;
};

}  // namespace azura
