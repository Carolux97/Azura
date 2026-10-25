// The controls menus: the keyboard and mouse's, and the controller's. They take the place of the
// Options' Keyboard and mouse and Controller lists (menu/pc_options.h) and look like the Local co-op
// menu (coop_ui.h): a dark steel panel with cut corners over the dimmed game, imulsion gold for the
// title and the accents, Barlow text.
//
// Each has two tabs. Assignment:
//   - keyboard and mouse: every bindable action with two keys or mouse buttons. Enter waits for
//     the next key, mouse button or wheel notch and binds it (a key another action had swaps with
//     this one's), Delete clears it, F1 brings the defaults back. Escape, Enter, Backspace and the
//     arrow keys are fixed and cannot be bound (the SDK's keyboard and mouse driver);
//   - controller: the game's control schemes (Default, Alternate) and the player's own (Custom),
//     changed with the bumpers, with what each button does around a drawing of the player's
//     controller. In Custom, A waits for the button to give the chosen action to (the two swap);
//     the layout is rex/input/pad_remap.h's.
// Settings:
//   - keyboard and mouse: mouse sensitivity for looking, aiming and zooming, inverted Y, mouse
//     acceleration, and pressing instead of holding to aim, zoom and roadie run (cvars of the
//     SDK's driver and of input/mouse_look.h, saved to azura.toml);
//   - controller: the game's own Controller rows (inverted axes, evade, the three sensitivities,
//     vibration, sticks, triggers), read from and written to the player's profile
//     (script::ReadProfileSetting); the game saves them as its Options menu closes.
// Up and down choose a row, left and right change its value. Tab (keyboard) or the triggers
// (controller) switch tabs; Esc or B close the menu. The controller's menu works from the keyboard
// too (Q and E change the scheme), all but giving a button an action, which needs a controller to
// press it on. While it is open the game gets no input.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <rex/input/device.h>
#include <rex/input/pad_remap.h>
#include <rex/ui/imgui_dialog.h>

#include "coop_ui.h"
#include "menu/pc_options.h"

struct ImFont;
struct ImDrawList;

namespace rex::input {
class InputSystem;
}

namespace azura {

namespace input {
class PlayerAssignment;
}

class ControlsMenu : public rex::ui::ImGuiDialog {
 public:
  enum class Kind { kKeyboardMouse, kController };

  // `owner` is cleared when the menu closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. The fonts may be null (ImGui's default is used).
  ControlsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input, input::PlayerAssignment* assignment,
               Kind kind, uint32_t language, ImFont* title_font, ImFont* body_font, ControlsMenu** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // What one frame's input asks for, from the keyboard or any controller.
  struct Actions {
    int vertical = 0;    // -1 up, 1 down
    int horizontal = 0;  // -1 left, 1 right
    int tab = 0;         // -1 previous, 1 next
    int scheme = 0;      // -1 previous, 1 next (controller, bumpers)
    bool accept = false;
    bool clear = false;     // Delete (keyboard)
    bool defaults = false;  // F1, or Y
    bool back = false;
  };

  Actions ReadActions();
  // The physical control pressed on any controller this frame, or kCount.
  rex::input::PadControl ReadPadPress();
  // The bind token pressed on the keyboard or the mouse this frame ("Q", "LMB", "WheelUp"), "" for
  // none; `cancel` is set when Escape was pressed.
  std::string ReadKeyPress(bool& cancel);

  void HandleKeyboardAssignment(const Actions& actions);
  void HandleKeyboardSettings(const Actions& actions);
  void HandleControllerAssignment(const Actions& actions);
  void HandleControllerSettings(const Actions& actions);

  void DrawKeyboardAssignment(ImDrawList* draw_list, float left, float top, float width, float height);
  void DrawSettings(ImDrawList* draw_list, float left, float top, float width, float height);
  void DrawControllerAssignment(ImDrawList* draw_list, float left, float top, float width, float height);
  void DrawHints(ImDrawList* draw_list, float left, float right, float y);

  // The controller whose glyphs and drawing the menu shows: the one last pressed in the menu, else
  // player 1's, else the first one; with none connected the menu shows a generic grey controller.
  const rex::input::DeviceInfo* ShownController() const;

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  Kind kind_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  ControlsMenu** owner_;
  menu::GameControlsTexts game_texts_;
  Clock::time_point opened_;
  // The keyboard or the controller the player last used: the hints' glyphs and the controller drawn.
  coop_ui::LastUsedInput last_input_;
  float unit_ = 1.0f;

  int tab_ = 0;  // 0 Assignment, 1 Settings
  int row_ = 0;
  int slot_ = 0;  // keyboard: which of a row's two keys
  bool waiting_ = false;
  // Esc or B closed the menu, which closes once it is released (coop_ui::CloseInputHeld).
  bool closing_ = false;
  Clock::time_point waiting_since_;

  // Controller: the scheme shown (0 Default, 1 Alternate, 2 Custom) and the custom layout.
  int scheme_ = 0;
  rex::input::PadLayout layout_;

  // Each controller's buttons the frame before (the left stick's direction as D-pad bits, the
  // triggers as their bits below), so an action happens once per press. A controller seen for the
  // first time does nothing that frame: the press that opened the menu does not also act here.
  std::map<rex::input::DeviceId, uint32_t> previous_buttons_;
  // The keyboard's state the frame before, for the keys ImGui reports while held.
  bool keyboard_ready_ = false;
};

}  // namespace azura
