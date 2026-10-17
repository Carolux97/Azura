// Hooks into the game's UnrealScript interpreter. See script_hooks.cpp for how they work.

#pragma once

#include <functional>

namespace azura::script {

// Called (once, from a game thread) when the player chooses Quit Game in the main menu. The
// handler must close the application.
void SetQuitHandler(std::function<void()> handler);

}  // namespace azura::script
