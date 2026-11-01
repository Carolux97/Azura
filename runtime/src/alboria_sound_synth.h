// Alboria's sounds, made from code: the sound identity of the Alboria service, the same in every
// game of ours (Azura's docs/alboria-sounds.md, alboria-web's docs/sounds.md). This file only
// synthesises them, with nothing of a runtime or of an audio API; Azura's runtime and alboria-web's
// tools/sounds keep the same copy of it.
//
// Every sound comes from Alboria's call, three notes rising in open fifths (D, A, E, one for each
// of "Al-bo-ria"), answered by an F# above them. The notes are struck by a warm electric piano
// (a soft FM strike, a twin a few cents above it, a sub an octave down) with a halo of pure sines
// an octave and a fifth above, in a large, distant room. The sounds that matter (an invitation,
// the overlay opening and closing, a notice) also hold their chord with a bed below: muted strings
// an octave down, monks far away singing the root and its fifth (organum), and double basses
// bowing the root two octaves down, softly, fading first so the tail never ends on them. The
// short sounds (moving, accepting, going back, typing) keep only the strike and a touch of halo,
// so moving around stays quick.
#pragma once

#include <array>
#include <vector>

namespace azura::alboria_sound_synth {

// The sample rate the sounds are made at.
constexpr int kSampleRate = 48000;

enum class Sound {
  kMove,        // the focus moved to another row or key
  kPage,        // another page
  kType,        // a character typed or erased
  kAccept,      // an action done (join, invite, send)
  kBack,        // going back, or Not now
  kOpen,        // the overlay opened
  kClose,       // the overlay closed
  kInvitation,  // an invitation's notice
  kNotice,      // another notice: a friend request, a friend joining the room
  kError,       // an action failed
  kCount,
};

// Interleaved stereo frames, from -1 to 1.
using Buffer = std::vector<float>;

// Makes `sound`. Slow (the larger ones take a few hundred milliseconds): make them once, off any
// thread that draws or plays.
Buffer Make(Sound sound);

}  // namespace azura::alboria_sound_synth
