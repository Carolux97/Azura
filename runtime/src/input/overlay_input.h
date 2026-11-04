// What the Alboria overlay (alboria_overlay.h) takes from the controllers before the game reads
// them, through the input system's state filter (rex/input/input_system.h):
//
// - RB pressed while Back is held opens it (Share or Create on PlayStation, View on Xbox). Back
//   still reaches the game at once, as always; the RB that opens the overlay never does, and from
//   then on the game sees neither until both are released.
// - RT pressed while Back is held accepts the invitation of an Alboria notice
//   (alboria_notices.h), only while one is on screen (SetNoticeAccepting); its RT never reaches the
//   game either. Without such a notice both reach the game as always.
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

// Installs the filter on `input` (`open` null removes it). `open` runs on the guest's input thread
// when RB is pressed while Back is held, and `accept` when RT is while a notice is accepting: they
// post their work to the UI thread.
void InstallOverlayInput(rex::input::InputSystem* input, std::function<void()> open, std::function<void()> accept);

// While true, an invitation's notice is on screen and Back + RT accepts it.
void SetNoticeAccepting(bool accepting);

// While true, the game reads its controllers with nothing pressed (the overlay is open).
void SetOverlayBlocking(bool blocking);

// Presses Start for player 1 for a moment, before the blocking starts: the game opens its pause menu.
void PressStartForGame();

}  // namespace azura::input
