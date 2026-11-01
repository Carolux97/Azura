// Alboria's sounds over the game (the overlay and its notices), as alboria_sound_synth.h makes them
// (the sound identity of the service, docs/alboria-sounds.md).
//
// They play through SDL's audio, beside the game's, at the game's effects volume (the Options'
// effects slider, from player 1's profile) and not at all with audio_mute. They are made once, on a
// thread of their own, when the app starts (Prepare); a sound asked for before it is ready is not
// played.
#pragma once

#include "alboria_sound_synth.h"

namespace azura::alboria_sounds {

using Sound = alboria_sound_synth::Sound;

// Starts making the sounds on a thread of their own. Call once, at start-up.
void Prepare();

// Plays `sound`. Any thread; the audio device opens on the first call.
void Play(Sound sound);

// Closes the audio device (the app is closing).
void Shutdown();

}  // namespace azura::alboria_sounds
