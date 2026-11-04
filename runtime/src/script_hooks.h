// Hooks into the game's UnrealScript interpreter. See script_hooks.cpp for how they work.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace azura::script {

// Called (once, from a game thread) when the player chooses Quit Game in the main menu. The
// handler must close the application.
void SetQuitHandler(std::function<void()> handler);

// Called (from a game thread) when the player accepts restarting the game to apply Video settings
// that only take effect at start-up. The handler must start the game again and close this one.
void SetRestartHandler(std::function<void()> handler);
// What choosing Local co-op in the Options runs (from the game's script thread): it opens
// the Local co-op menu. nullptr to stop.
void SetLocalCoopHandler(std::function<void()> handler);
// What choosing Connection in the Options runs (from the game's script thread): it opens the
// Alboria menu (connection_menu.h). nullptr to stop.
void SetConnectionHandler(std::function<void()> handler);
// What choosing Versus online, Horde online or Beast online in their menus runs (from the game's
// script thread), with the game (0 Versus, 1 Horde, 2 Beast): it opens the online menu
// (versus_online_menu.h) for that game. nullptr to stop.
void SetVersusOnlineHandler(std::function<void(int game)> handler);
// What the online menu opening now is to say, once (it clears it): "join_failed" (the matchmade match
// Alboria sent this player to could not be joined; the runtime left its room and created an online
// party again), "room_closed" (the matchmade lobby or match closed under the player: its host went),
// or nullptr.
const char* TakeOnlineMenuNotice();
// What the Versus online menu asks the game for once Alboria decided, done on player 1's next input
// frame in the front end: open the matchmade pre-game lobby of `playlist` (this player hosts), or
// take the match's invite (Alboria sent this player into another's match). Any thread.
void RequestMatchmadeLobby(int32_t playlist);
void RequestMatchInvite();
// What the Versus online menu's stats and awards button asks for: the War Journal, opened on player
// 1's next input frame in the front end as the main menu's Journal opens it. Any thread.
void RequestWarJournal();

// The two controls menus (controls_menu.h), opened from the Options' Keyboard and mouse and
// Controller categories.
enum class ControlsMenu { kKeyboardMouse, kController };

// What choosing one of those categories runs (from the game's script thread): it opens that menu.
// nullptr to stop.
void SetControlsMenuHandler(std::function<void(ControlsMenu)> handler);

// What Manage/Invite's RB runs with online play on (from the game's script thread): it opens the
// Join by code screen (join_by_code_menu.h) over Manage/Invite, which stays open under it until the
// code joins a room. nullptr to stop.
void SetJoinByCodeHandler(std::function<void()> handler);

// Why the game stays in Local when the player wants a room on Alboria: the refusal Alboria gave the
// sign-in (alboria::SignInRefusal, such as "no_account"), handed out once, at the moment the
// start-up party falls back to Local or the player chooses Joinable or Invite only; empty
// otherwise. The room overlay shows it then. Any thread.
std::string TakeRefusalNotice();

// Once the player is in a friend's room on Alboria: opens the game's System Link browser, which
// finds the host's party in the room, and joins the first party it finds (searching again a few
// times while there is none). Any thread; it happens on the game's script thread soon after.
void JoinSystemLinkPartyInRoom();

// A sound of the game's menus, for our menus that stand in for the game's (Local co-op, the
// controls menus, Join by code, Connection), so they sound as the game's own do: the events of its
// UISoundTheme (Options_StepUp, ValueChangeLeft, Options_AcceptButton...). Left and right change a
// list's value; the slider's move a bar one step, as the Options' volume bars do.
enum class MenuSound { kUp, kDown, kLeft, kRight, kAccept, kBack, kOpen, kClose, kSliderLeft, kSliderRight };
// Plays `sound` through the game on player 1's next input frame. Any thread. Dropped while the game
// has not played a menu sound yet (before the title screen) or no input runs (a loading screen).
void PlayMenuSound(MenuSound sound);

// Where player 1 is, as of their last input: the title screen (where Return is Start), a menu (the
// front end and what it opens, or the game paused), or playing. Any thread.
enum class Menus { kPlaying, kTitleScreen, kMenu };
Menus WhereInMenus();

// The value of profile setting `id` of guest user `user` (Value1 of its SettingsData: a value ID
// for a list, a float's bits for a slider), as the game keeps it in memory. False while it cannot
// be found (the player's controller has not run its input yet). Any thread.
bool ReadProfileSetting(uint32_t user, int id, uint32_t& value);

// Changes that value. The game writes the profile, and picks the change up, when its Options menu
// closes.
bool WriteProfileSetting(uint32_t user, int id, uint32_t value);

}  // namespace azura::script
