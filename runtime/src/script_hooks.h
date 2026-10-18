// Hooks into the game's UnrealScript interpreter. See script_hooks.cpp for how they work.

#pragma once

#include <functional>

namespace azura::script {

// Called (once, from a game thread) when the player chooses Quit Game in the main menu. The
// handler must close the application.
void SetQuitHandler(std::function<void()> handler);

// Called (from a game thread) when the player accepts restarting the game to apply Video settings
// that only take effect at start-up. The handler must start the game again and close this one.
void SetRestartHandler(std::function<void()> handler);

}  // namespace azura::script
