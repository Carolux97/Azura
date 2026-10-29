// The Alboria menu: the player's account and how they connect to online games, opened from the
// Connection category of the game's Options (menu/pc_options.h). It has the look of the Local co-op
// menu (coop_ui.h): a dark steel panel with cut corners over the dimmed game, imulsion gold for the
// title and the accents.
//
// At the top, the player's identity on Alboria, Gamertag#tag (rex/system/xam/alboria.h: GetIdentity),
// signed in again when the menu opens. Below it, one row per setting, each with a paragraph saying
// what it does:
// - Direct connection (alboria_direct): reach the other players of a room directly when the network
//   allows it, with the relay as fallback; lower latency, but they can learn this computer's public
//   IP address. Takes effect in the next room.
// - Show latency (latency_overlay): the round trip in the performance overlay's corner while in a
//   room.
// No address is ever shown.
//
// Up and down choose a row, A, left or right change it, B or Start close the menu; on the keyboard
// the arrows (or W and S), Enter or Space, and Escape. Each change is saved to azura.toml at once.
// While it is open the game gets no input (PlayerAssignment::SetBlocked).

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

class ConnectionMenu : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the menu closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. The fonts may be null (ImGui's default is used).
  ConnectionMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                 input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                 ImFont* body_font, ConnectionMenu** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // Reads the controllers and the keyboard, moves the focus and changes settings. Returns whether
  // a press asked to close the menu.
  bool HandleInput();
  // Turns the setting of row `row` on or off and saves it.
  void Toggle(int row);

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  ConnectionMenu** owner_;
  Clock::time_point opened_;
  coop_ui::LastUsedInput last_input_;
  // Each controller's buttons the frame before (the left stick's direction as D-pad bits). A
  // controller seen for the first time does nothing that frame: the press that opened the menu
  // does not also act in it.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  int focus_ = 0;
  // A press closed the menu, which closes once it is released (coop_ui::CloseInputHeld).
  bool closing_ = false;
};

}  // namespace azura
