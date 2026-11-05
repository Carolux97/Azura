// Alboria's notices over the game, as the console's Guide showed Xbox Live's: an invitation to a
// friend's room, a friend request, or a friend joining the player's room, each for a few seconds at
// the bottom of the screen, in Alboria's look (alboria_style.h).
//
// The SDK's friends reader finds them (rex::system::xam::alboria::TakeNotices); this dialog takes
// them every frame and shows them one at a time. Each one comes in with Alboria's planet growing
// from nothing to its size with a bounce, then the panel unfolding to its right; while it shows, a
// moon travels the planet's ring, a bar runs down the time left and the hint pulses; it leaves the
// same way backwards.
//
// An invitation's notice can be answered from it: Back + RT on a controller (Share + R2) or Shift
// with the overlay's key accepts it and joins the friend's room, and Back + RB or the overlay's key
// open the overlay on the Invitations page. The hints show the glyphs of the input player 1 is
// playing with, as the game's own prompts do: the keys while on the keyboard and mouse, the
// controller's buttons while on a controller, never both. While
// the overlay is open no notice shows; they wait for it to close.
//
// The app creates it at start-up when network_mode is online and it lives as long as the app. It
// draws on ImGui's foreground draw list, with no window, so it never takes input: the controllers'
// Back + RT comes through the input filter (input/overlay_input.h) and the keyboard's through the
// overlay's bind.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/input/device.h>
#include <rex/system/xam/alboria.h>
#include <rex/ui/imgui_dialog.h>

#include "coop_ui.h"
#include "input/player_assignment.h"

struct ImFont;

namespace rex::input {
class InputSystem;
}

namespace azura {

namespace input {
class PlayerAssignment;
}

class AlboriaNotices : public rex::ui::ImGuiDialog {
 public:
  using Page = rex::system::xam::alboria::OverlayPage;

  // `owner` is cleared when the dialog closes and deletes itself. `language` is MenuLanguage()
  // (an Xbox 360 language ID, or Czech or Hungarian). The fonts may be null (ImGui's default is used).
  AlboriaNotices(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input, input::PlayerAssignment* assignment,
                 uint32_t language, ImFont* title_font, ImFont* body_font, AlboriaNotices** owner);

  // Accepts the invitation whose notice is on screen, if there is one. On the UI thread.
  bool AcceptShown();
  // The overlay's page for the notice on screen: Invitations for an invitation, else Friends.
  Page OverlayPage() const;
  // The notice on screen goes (the overlay opened over it).
  void Dismiss();

  // Only while a notice is on screen (its animation needs every frame).
  bool NeedsContinuousPaint() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;
  using Notice = rex::system::xam::alboria::Notice;

  // The notice on screen.
  struct Shown {
    Notice notice;
    Clock::time_point since;
    // How long it stays before it leaves on its own.
    float seconds = 0.0f;
    // When it started to leave.
    std::optional<Clock::time_point> leaving;
  };
  // What accepting reports, written by the SDK's worker thread, which may report after the dialog
  // has closed: shared with it, and guarded by its mutex.
  struct Accepting {
    std::mutex mutex;
    bool busy = false;
    bool done = false;
    std::string error;
  };

  // Notes a press on the controllers, to tell which of player 1's controllers is in use.
  void WatchPads();
  // The controller whose glyphs the hints show, or null for the keys: player 1's active input.
  const rex::input::DeviceInfo* HintPad(const std::vector<input::PlayerAssignment::Slot>& slots) const;
  void Leave(Clock::time_point now);

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  AlboriaNotices** owner_;
  // The controller last pressed, while player 1 has several.
  std::optional<rex::input::DeviceId> last_pad_;
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  // UI thread only: the notices waiting, and the one on screen.
  std::deque<Notice> queue_;
  std::optional<Shown> shown_;
  // The pause after a notice leaves, before the next may come.
  Clock::time_point gap_until_;
  // The accept of the notice on screen, while there is one.
  std::shared_ptr<Accepting> accepting_;
};

}  // namespace azura
