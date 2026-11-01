// Alboria's sounds over the game (the overlay and its notices), as alboria_sound_synth.h makes them
// (the sound identity of the service, docs/alboria-sounds.md).
//
// They play through SDL's audio, beside the game's, at the game's effects volume (the Options'
// effects slider, from player 1's profile) times the player's own volume for each group (the
// overlay's Settings page: its menu's sounds, and the notices), and not at all with audio_mute.
// They are made once, on a thread of their own, when the app starts (Prepare); a sound asked for
// before it is ready is not played.
#pragma once

#include "alboria_sound_synth.h"

namespace azura::alboria_sounds {

using Sound = alboria_sound_synth::Sound;

// Starts making the sounds on a thread of their own. Call once, at start-up.
void Prepare();

// The two groups of sounds, each with its own volume: the overlay's menu (moving, choosing, going
// back, opening and closing it) and the notices over the game.
enum class Group { kMenu, kNotices };
Group GroupOf(Sound sound);
// A group's volume, from 0 (silent) to kMaxVolume (the game's effects volume), and setting it: it
// takes effect at once and is saved to azura.toml (alboria_menu_volume, alboria_notice_volume).
constexpr int kMaxVolume = 10;
int Volume(Group group);
void SetVolume(Group group, int volume);

// Plays `sound`. Any thread; the audio device opens on the first call.
void Play(Sound sound);

// Closes the audio device (the app is closing).
void Shutdown();

}  // namespace azura::alboria_sounds
