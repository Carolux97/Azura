// The Join by code screen: the player types the code of a friend's room on Alboria
// (rex/system/xam/alboria.h) and joins it. It has the look of the Local co-op menu (coop_ui.h): a
// dark steel panel with cut corners over the dimmed game, imulsion gold for the title and the
// accents.
//
// The code is eight characters, shown as XXXX-XXXX in eight boxes that fill from the left. With a
// controller, an on-screen keyboard like the launcher's (launcher/src/virtualKeyboard.js) types
// it: the keys are the codes' alphabet and Delete, Back and Join, the D-pad or the left stick moves
// over them, A presses one, X deletes, B goes back and Start joins. With the keyboard, the keys are
// not shown: the characters are typed, Backspace deletes, Ctrl or Cmd+V pastes a code, Enter joins
// and Escape goes back. While it is open the game gets no input (PlayerAssignment::SetBlocked).
//
// Joining signs in and enters the room; once the relay answers, the screen closes and the game's
// System Link browser opens on its own, which finds the host's party in the room and joins it
// (script_hooks.cpp). A failure shows in the screen, which stays open to try again.
//
// Opened from Manage/Invite's RB (script::SetJoinByCodeHandler), in a party on Alboria, over
// Manage/Invite: going back returns to it, and joining closes it before the browser opens. Joining
// leaves the room the player was in.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

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

class JoinByCodeMenu : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the menu closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. The fonts may be null (ImGui's default is used).
  JoinByCodeMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                 input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                 ImFont* body_font, JoinByCodeMenu** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr size_t kCodeLength = 8;

  // Reads the controllers and the keyboard and edits the code. Returns whether a press asked to
  // close the menu.
  bool HandleInput();
  // Moves the on-screen keyboard's focus by a row and a column.
  void MoveFocus(int rows, int columns);
  // Presses the focused key.
  void PressFocused();
  // Adds `c` to the code when it is one of its characters and there is room.
  void Type(char c);
  // Puts a pasted code in the boxes.
  void Paste(const char* text);
  void Join();

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  JoinByCodeMenu** owner_;
  Clock::time_point opened_;
  coop_ui::LastUsedInput last_input_;
  // Each controller's buttons the frame before (the left stick's direction as D-pad bits). A
  // controller seen for the first time does nothing that frame: the press that opened the menu
  // does not also act in it.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  // The code typed so far, and the on-screen keyboard's focused key (row and column).
  std::string code_;
  int focus_row_ = 0;
  int focus_column_ = 0;
  // Waiting for the room after Join, and the error of the last attempt.
  bool joining_ = false;
  std::string error_;
  // A press closed the menu, which closes once it is released (coop_ui::CloseInputHeld).
  bool closing_ = false;
  // Whether the close came from a press of B or Escape (going back) rather than joining.
  bool going_back_ = false;
};

}  // namespace azura
