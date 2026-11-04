// Azura's own online matchmaking menu, for Versus, Horde and Beast: the player chooses Standard
// (Ranked is shown, coming soon) and one or more of its modes, and the menu searches for a match through Alboria, showing how the
// search goes (the players found so far) until the game opens or joins the match's pre-game lobby.
// Opened from Versus', Horde's and Beast's menus (their online option, menu_patches.cpp;
// script_hooks.cpp opens it).
//
// Nothing it shows tells who will host the match (alboria-notes:alboria-server/online-rules.md).

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>

#include "coop_ui.h"
#include "input/player_assignment.h"

struct ImFont;

namespace azura {

// The game whose public matches the menu searches for: Versus (its Standard modes), Horde or Beast
// (one mode each for now; more may come as rows of their own).
enum class OnlineGame { kVersus, kHorde, kBeast };

// A party member follows its leader's search: about once a second, while the player is a member
// (not the leader) of a party with others on Alboria and not playing, it asks Alboria how the
// leader's search goes, and calls `open_view` (from its own thread) while the leader searches,
// unless the player hid that search. `open_view` opens the menu with `watching`.
void StartPartySearchFollower(std::function<void()> open_view);
void StopPartySearchFollower();

class VersusOnlineMenu : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the menu closes and deletes itself. `language` is the Xbox 360 language
  // ID of the user_language setting. The fonts may be null (ImGui's default is used). `game` is the
  // menu's game; a party member's view (`watching`) takes its leader's instead.
  VersusOnlineMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input, input::PlayerAssignment* assignment,
                   uint32_t language, ImFont* title_font, ImFont* body_font, VersusOnlineMenu** owner,
                   bool watching = false, OnlineGame game = OnlineGame::kVersus);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  // The menu's screens: choosing the modes, searching, the match found (until the game takes over),
  // and a search that could not be made.
  enum class Screen { kChoose, kSearching, kReady, kError };

  // What the player pressed this frame, from the controllers and the keyboard.
  struct Presses {
    int move = 0;
    bool accept = false;
    bool mark = false;
    bool journal = false;
    bool back = false;
  };
  Presses ReadInput();
  // Starts a search for the modes marked, or says that none is.
  void Search();
  // Follows the search and acts on the player's presses.
  void Update(const Presses& presses);

  rex::input::InputSystem* input_;
  input::PlayerAssignment* assignment_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  VersusOnlineMenu** owner_;
  Clock::time_point opened_;
  coop_ui::LastUsedInput last_input_;
  // A party member's view of its leader's search (StartPartySearchFollower), not a search of its own.
  bool watching_ = false;
  // The game the menu searches for, and the modes it offers (indexes in the menu's list of modes).
  OnlineGame game_ = OnlineGame::kVersus;
  std::vector<int> rows_;
  // Each controller's buttons the frame before (the left stick's direction as D-pad bits). A
  // controller seen for the first time does nothing that frame: the press that opened the menu does
  // not also act in it.
  std::map<rex::input::DeviceId, uint16_t> previous_buttons_;
  Screen screen_ = Screen::kChoose;
  Clock::time_point screen_since_;
  // The mode list's focused row.
  int focus_ = 0;
  // Since when "choose at least one mode" shows, after a search with none marked.
  Clock::time_point no_mode_since_{};
  // Why the last search failed (Alboria's error), for the error screen.
  std::string error_;
  // A press closed the menu, which closes once it is released (coop_ui::CloseInputHeld).
  bool closing_ = false;
};

}  // namespace azura
