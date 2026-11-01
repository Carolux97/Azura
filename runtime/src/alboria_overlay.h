// The Alboria overlay: Alboria's screen over any game, as the Guide was Xbox Live's over the
// console's games. It belongs to the service, not to the game, so it has Alboria's look (the
// launcher's light panels: paper, navy, sky and sun) instead of the game's steel.
//
// Three pages:
// - Friends: Add friend first, which opens a screen to type a Gamertag#tag (with an on-screen
//   keyboard on a controller), then every friend of the account with what they are doing
//   (favourites first, then who plays, who is online, who is not), with Join for a friend in an
//   open room and Invite while the player is in a room.
// - Invitations: the ones received, with Join and Not now.
// - Room: the code of the player's room, to copy, and Join by code.
// The data is the SDK's (rex/system/xam/alboria.h), read again as the overlay opens. Joining goes
// the way a join by code does: the host's party reaches the game as an accepted invite.
//
// It opens anywhere in the game, with the key of bind_alboria_overlay (F1 by default, set from the
// launcher), with RB while holding Back on a controller (input/overlay_input.h), and when the
// game opens the Guide's friends or invite screens (XamShowFriendsUI, XamShowGameInviteUI). Opened
// in a match, the game's pause menu opens under it. While it is open the game reads its controllers with nothing
// pressed and hears that the Guide is open (XN_SYS_UI); the keyboard and the mouse are ImGui's.
//
// Controls: LB/RB or Tab change page, the D-pad or the arrows move, A or Enter does the focused
// row's main action, X its second one, Y copies the room's code, B, Escape or the opening key
// close. The mouse clicks the tabs and the buttons. The controller's glyphs and the on-screen
// keyboard take Alboria's colours too.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <rex/input/device.h>
#include <rex/system/xam/alboria.h>
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

class AlboriaOverlay : public rex::ui::ImGuiDialog {
 public:
  using Page = rex::system::xam::alboria::OverlayPage;

  // `owner` is cleared when the overlay closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. `join_by_code` opens the Join by code screen once the
  // overlay has closed. The fonts may be null (ImGui's default is used).
  AlboriaOverlay(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                 input::PlayerAssignment* assignment, uint32_t language, Page page, ImFont* title_font,
                 ImFont* body_font, std::function<void()> join_by_code, AlboriaOverlay** owner);

  // Closes it, as B does (the opening key pressed again).
  void RequestClose() { closing_ = true; }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // What the controllers and the keyboard asked for this frame.
  struct Actions {
    int page = 0;    // -1 previous, +1 next
    int move = 0;    // -1 up, +1 down
    bool primary = false;
    bool secondary = false;
    bool copy = false;
    bool close = false;
  };
  Actions ReadInput();
  // Does the focused row's main (`secondary` false) or second action on the current page.
  void Act(bool secondary);
  // Starts an action of the SDK, showing `pending` until it reports.
  void Start(const std::string& pending, const std::function<void(rex::system::xam::alboria::ActionDone)>& action,
             const std::string& done_text, bool close_when_done);
  void SetMessage(std::string text, bool error);
  // The Add friend screen: reads its input (returns whether it asked to go back), types a
  // character, erases one, presses the focused key of the on-screen keyboard, sends the request.
  bool AddFriendInput();
  void AddType(char c);
  void AddErase();
  void PressKey();
  void SendRequest();

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  Page page_;
  ImFont* title_font_;
  ImFont* body_font_;
  std::function<void()> join_by_code_;
  AlboriaOverlay** owner_;
  Clock::time_point opened_;
  coop_ui::LastUsedInput last_input_;
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  int focus_ = 0;
  // The Add friend screen, over the Friends page: the Gamertag and the tag being typed, which of
  // the two takes the characters, the on-screen keyboard's capitals and its focused key.
  bool adding_ = false;
  std::string add_name_;
  std::string add_tag_;
  bool add_in_tag_ = false;
  bool add_shift_ = true;
  int key_row_ = 1;
  int key_column_ = 0;
  bool closing_ = false;
  // After closing: open the Join by code screen.
  bool open_join_by_code_ = false;
  // The result of the last action, written by the SDK's worker threads, which may report after the
  // overlay has closed: shared with them, and guarded by its mutex.
  struct Result {
    std::mutex mutex;
    std::string message;
    bool error = false;
    bool busy = false;
    bool close_after = false;
    Clock::time_point since;
  };
  std::shared_ptr<Result> result_ = std::make_shared<Result>();
};

}  // namespace azura
