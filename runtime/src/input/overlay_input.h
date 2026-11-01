// What the Alboria overlay (alboria_overlay.h) takes from the controllers before the game reads
// them, through the input system's state filter (rex/input/input_system.h):
//
// - RB pressed while Back is held opens it (Share or Create on PlayStation, View on Xbox). Back
//   still reaches the game at once, as always; the RB that opens the overlay never does, and from
//   then on the game sees neither until both are released.
// - While the overlay is open the game reads its controllers with no buttons pressed and the sticks
//   centred: they stay connected, so the game shows no notice of a lost controller, as it would
//   with the Local co-op menu's blocking (PlayerAssignment::SetBlocked).
// - Opened in a match, it first presses Start for the game, which opens its pause menu under the
//   overlay.
#pragma once

#include <functional>

namespace rex::input {
class InputSystem;
}

namespace azura::input {

// Installs the filter on `input` (nullptr removes it). `open` runs on the guest's input thread when
// RB is pressed while Back is held: it posts the opening to the UI thread.
void InstallOverlayInput(rex::input::InputSystem* input, std::function<void()> open);

// While true, the game reads its controllers with nothing pressed (the overlay is open).
void SetOverlayBlocking(bool blocking);

// Presses Start for player 1 for a moment, before the blocking starts: the game opens its pause menu.
void PressStartForGame();

}  // namespace azura::input
