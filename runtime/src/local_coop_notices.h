// The notices of local co-op, in the look of the Local co-op menu (coop_ui.h):
//   - a player who loses their only device (a controller switched off or unplugged, with no
//     keyboard and mouse) gets a notice to reconnect it, with the lost controller's icon, as the
//     Xbox 360 asked. The same controller coming back takes its place on its own; pressing the
//     confirm button on a controller nobody holds gives that one to the player instead. A player who
//     still holds a device gets no notice;
//   - during co-op, a controller that connects and plays nobody gets a short notice to assign it in
//     Options, Local co-op.
// Nothing shows while the Local co-op menu is open, which shows the same. The dialog lives as long as
// the game and never takes the game's input: it reads the unassigned controllers on its own.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <rex/input/device.h>
#include <rex/ui/imgui_dialog.h>

struct ImFont;

namespace rex::input {
class InputSystem;
}

namespace azura {

namespace input {
class PlayerAssignment;
}

class LocalCoopNotices : public rex::ui::ImGuiDialog {
 public:
  // `language` is the Xbox 360 language ID of the user_language setting; `gamertag` names the
  // players (LocalPlayerName). The fonts may be null (ImGui's default is used).
  LocalCoopNotices(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                   input::PlayerAssignment* assignment, uint32_t language, std::string gamertag,
                   ImFont* title_font, ImFont* body_font);

  // Only while a notice is on screen; otherwise the window repaints for game frames, which also
  // draw this dialog and let it notice a change.
  bool NeedsContinuousPaint() const override { return showing_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  using Clock = std::chrono::steady_clock;

  // Gives a controller nobody holds to `player` when its confirm button is pressed.
  void TakeOverWithUnassigned(int player);

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  std::string gamertag_;
  ImFont* title_font_;
  ImFont* body_font_;
  bool showing_ = false;
  // The unassigned controllers' buttons the frame before, so a press counts once; a controller seen
  // for the first time does nothing that frame.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  // The controllers seen so far, to tell a new one; the newest unassigned one and when it arrived.
  std::set<rex::input::DeviceId> known_;
  bool known_initialized_ = false;
  rex::input::DeviceInfo connected_;
  Clock::time_point connected_at_;
};

}  // namespace azura
