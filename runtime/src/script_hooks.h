// Hooks into the game's UnrealScript interpreter. See script_hooks.cpp for how they work.

#pragma once

#include <cstdint>
#include <functional>

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

// The two controls menus (controls_menu.h), opened from the Options' Keyboard and mouse and
// Controller categories.
enum class ControlsMenu { kKeyboardMouse, kController };

// What choosing one of those categories runs (from the game's script thread): it opens that menu.
// nullptr to stop.
void SetControlsMenuHandler(std::function<void(ControlsMenu)> handler);

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
