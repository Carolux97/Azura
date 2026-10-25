// The Local co-op menu: hands the keyboard and mouse and the controllers to players 1 and 2. The
// launcher stands in for the Xbox 360's own system screens, so the menu has the launcher's look and
// the achievement notice's: a dark steel panel with cut corners over the dimmed game, imulsion gold
// for the title and the accents, Barlow text.
//
// One column for the devices nobody holds and one per player the menu offers
// (PlayerAssignment::AvailablePlayers), each with the player's name and their devices, every
// device with an icon of its kind and its name. Each device moves itself: left and right on the
// D-pad or the left stick of a controller, and A/D or the arrow keys on the keyboard, pass it to the
// previous or next column. A, B or Start on a controller, and Enter or Esc on the keyboard, close
// the menu. While it is open the game gets no input (PlayerAssignment::SetBlocked), and every
// player stays connected.
//
// Opened from the Local co-op entry of the game's Options; see menu/pc_options.h.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
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

class LocalCoopMenu : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the menu closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting; `gamertag` names the players (LocalPlayerName). The
  // fonts may be null (ImGui's default is used).
  LocalCoopMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                input::PlayerAssignment* assignment, uint32_t language, std::string gamertag,
                ImFont* title_font, ImFont* body_font, LocalCoopMenu** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // Reads each controller's buttons and the keyboard and moves devices. Returns whether a press
  // asked to close the menu.
  bool HandleInput();

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  std::string gamertag_;
  ImFont* title_font_;
  ImFont* body_font_;
  LocalCoopMenu** owner_;
  Clock::time_point opened_;
  // Each controller's buttons the frame before (the left stick's direction as D-pad bits), so a
  // move happens once per press. A controller seen for the first time does nothing that frame: the
  // press that opened the menu does not also close it.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  // The device moved last and when, which glows for a moment.
  rex::input::DeviceId moved_ = rex::input::DeviceId::kInvalid;
  Clock::time_point moved_at_;
};

}  // namespace azura
