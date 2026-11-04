// When player 1 last did something with what they play with: a controller's buttons, triggers or
// sticks, the keyboard (read as player 1's controller), or the mouse's look. A matchmade match's
// host is removed after the game's idle time with nothing done (script_hooks.cpp, KickIdleHost),
// as the game removes its idle clients; the game's own reckoning (LastActiveTime) only follows
// players whose moves reach the host over the network, never the host's own.
#pragma once

#include <cstdint>

#include <rex/input/input.h>

namespace azura::input {

// Notes player 1's controller state as the game reads it (before any filtering): something pressed,
// or a stick or trigger past its dead zone, counts as activity.
void NotePadActivity(const rex::input::X_INPUT_STATE& state);

// Notes activity that does not show in the controller state (the mouse turning the camera).
void NoteActivity();

// Seconds since player 1's last activity; since the game started when there was none yet.
double SecondsSinceActivity();

// Whether the game's window has the focus. Only what the player does while it has counts: another
// game on the same computer, or another application, may read the same controller and mouse, and a
// host using them elsewhere is not playing.
void SetWindowFocused(bool focused);

}  // namespace azura::input
