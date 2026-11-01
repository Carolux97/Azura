# Alboria's sounds

Alboria's sound identity: the sounds the service makes over every game of ours (its overlay, its
notices), and how our own menus that stand in for a game's menus sound. They are made from code,
with no recordings of anyone's, and they are the same in every game, so that players come to
recognise them: what Marty O'Donnell, Halo's composer, called emotional equity, a sound that
gathers meaning each time it is heard.

The canonical description, with a tool that renders every sound to a file to listen to, is in
alboria-web (`docs/sounds.md`, `tools/sounds/`). This document is Azura's: where the sounds play and
how they are wired. The synthesis (`runtime/src/alboria_sound_synth.cpp`) must stay the same as the
tool's; change both together.

## 1. The identity

- **The call.** Three notes rising in open fifths, D, A and E, one for each of "Al-bo-ria",
  answered by an F# above them. Every sound is the call or a piece of it.
- **The strike.** A warm electric piano: a soft FM strike whose brightness fades fast, a twin 4
  cents above it, a sub an octave down, and a felt-soft attack (12 ms).
- **The halo.** Pure sines an octave and a fifth above each note, swelling in slowly and breathing
  with a slow tremolo, as a shimmer reverb leaves behind.
- **The bed**, under the sounds that matter (an invitation, the overlay opening and closing, a
  notice), holding their chord:
  - muted strings an octave down (six bowed voices a few cents apart, a slow bow in, a gentle
    vibrato);
  - monks far away singing the chord's first note and the fifth above it, an octave down
    (organum): three voices in near unison on a round "oh", a straight tone, barely any vibrato;
  - double basses bowing the first note two octaves down, softly (piano), fading before the
    strings so the tail never ends on them.
- **The room.** Large and distant: a Schroeder reverberator with a long tail and a 30 ms
  predelay. The short sounds use less of it.
- **The field.** Each note of a phrase sits a little further across the stereo field than the one
  before, so the call also moves.

Vibrato is always computed as the integral of the frequency (`VibratoPhase`): multiplying a
wobbling frequency by the time instead makes the pitch drift further as a note goes on, which
sounds like distortion.

## 2. The sounds

| Sound | What it is | Notes |
|---|---|---|
| Invitation | The whole call, answered | D4, A4, E5, then F#5, with the bed |
| Notice (a friend request, a friend joining) | The call's first two notes | D4, A4, with the bed |
| Overlay opens | The call's last two notes rising | A4, E5, with the bed |
| Overlay closes | The same, falling | E5, A4, with the bed |
| Accept | Rising a fourth | A4, D5 |
| Back, Not now | One soft note | A4 |
| Error | A semitone falling | A4, G#4 |
| Move | A short tick | E6 |
| Page | Two ticks | A5, E6 |
| Type | A softer tick | D6 |

## 3. In Azura

- `runtime/src/alboria_sound_synth.{h,cpp}`: the synthesis, with nothing of the runtime (the
  tool in alboria-web is a copy of it).
- `runtime/src/alboria_sounds.{h,cpp}`: makes the sounds on a thread of their own when the app
  starts (they take a moment: the bed has many voices) and plays them through SDL's audio beside
  the game's, at the game's effects volume (the Options' effects slider, profile setting 42) times
  0.55, times the player's volume for its group, and not at all with `audio_mute`. A sound asked
  for before they are made is not played.
- Two groups have their own volume, 0 to 10, in the overlay's Settings page and in `azura.toml`:
  the menu (`alboria_menu_volume`: moving, choosing, going back, the overlay opening and closing)
  and the notices (`alboria_notice_volume`: an invitation and the other notices). Changing one plays
  a sample of it at the new volume.
- The overlay (`alboria_overlay.cpp`) plays them as it opens and closes and for each action; the
  notices (`alboria_notices.cpp`) as each notice comes in, and for accepting and failing.

## 4. Our menus that stand in for the game's

Local co-op, the controls menus, Join by code and Connection are part of the game, so they sound
like it: they play the game's own menu sounds, not Alboria's. Every sound of Gears of War 3's menus
goes through `UISoundTheme.ProcessSoundEvent(SoundEventName, SoundOwner)` (traced with
`script_trace`): the theme maps the event's name to a SoundCue and the owner, the player's
controller, plays it. `script::PlayMenuSound` queues an event, and the script hooks run
`ProcessSoundEvent` with it on player 1's next input frame, on the theme the game last used
(`runtime/src/script_hooks.cpp`, "Menu sounds").

| Our menu does | The game's event |
|---|---|
| Up, down | `Options_StepUp`, `Options_StepDown` |
| A list's value left, right | `ValueChangeLeft`, `ValueChangeRight` |
| A bar (a sensitivity) left, right | `Options_SliderLeft`, `Options_SliderRight` (the volume bars') |
| Accept | `Options_AcceptButton` |
| Back | `Options_BackButton` |
| Opens, closes | `Options_DetailsEnter`, `Options_DetailsLeave` |

Other events seen in the menus, not used yet: `Host_SelectionChanged` and `Host_SelectItem` (the
main menu), `Alert_RectanglePopUp` and `Alert_RectangleLeave` (a dialog), `CloseOptions`,
`PartyPanel_PlayerJoin`, `Startup_Enter`, `Startup_HitStart`.
