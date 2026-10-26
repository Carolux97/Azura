# HLE graphics, phase 4: gfx = pm4 against gfx = hle

What the HLE path built in phase 3 ([02-design.md, as built](02-design.md#as-built-in-phase-3))
changes, measured on the same scripted runs in both modes. Everything here was measured on one
machine, a MacBook Pro with an Apple M4, on Metal; the Vulkan check is noted where it applies.

**In short:** the HLE path renders the same images as the PM4 path (within the run-to-run noise),
makes 25 to 40% fewer render target transfers and a few fewer render passes, and costs nothing
extra in pipelines or hitches. It does **not** make the GPU faster yet: at the campaign start it is
about 1.4 ms per frame slower, consistently, for a reason not found yet.

## Method

- `scripts/hle/compare_modes.sh 3 240`: three rounds, each running `gfx = pm4` and `gfx = hle`,
  alternating which goes first, one game at a time. Every run starts with fresh saves and an empty
  shader cache, and the scripted controller (`AZURA_SCRIPTED_PAD`) presses Start at the title,
  then Campaign, Standard, New, and accepts the screens up to the first chapter, which it reaches
  about 140 seconds in; the run is stopped at 240 seconds, during the first playable moment.
- Each run records the per-frame counters (`perf_log_csv`) and a capture of the game's output
  every 4 seconds (`AZURA_CAPTURE_DIR`). Both stay local (`captures/`, ignored by git).
- `scripts/hle/summarize_compare.py --images` groups frames into scenes by what the game draws
  (guest draws and resolves per frame), so runs that reach a scene a few seconds earlier or later
  are still compared on the same content, and reports medians per mode.
- Images: runs drift by a few seconds (loading time varies), so a capture is compared with the
  `pm4` captures at the same second and 4 seconds before and after, keeping the closest. Comparing
  `pm4` runs with each other the same way gives the noise floor: film grain, animated lights and
  menu backgrounds, and that drift.
- The game is capped at 30 frames per second in all scenes measured, so the frame time is the cap
  (33.3 ms) in both modes; the CPU side of the command processor is not measured separately.

One `pm4` run of the three stayed in the menus (a scripted press arrived before the game took
input), so the campaign figures have three `hle` runs and two `pm4` runs. An earlier set of runs,
before a correctness fix described below, is not used for the figures.

## Results

Medians of the steady frames of each scene.

| Scene | Mode | Render target transfers | Skipped | Render passes | Pass ends by transfer | Pass ends by resolve |
| --- | --- | --- | --- | --- | --- | --- |
| Title screen (357 draws, 30 resolves) | pm4 | 18 | 0 | 58 | 7 | 29 |
| | hle | 11 | 6 | 57 | 2 | 28 |
| Main menu (392 draws, 30 resolves) | pm4 | 18 | 0 | 60 | 7 | 29 |
| | hle | 11 | 6 | 59 | 2 | 29 |
| Campaign start (719 draws, 38 resolves) | pm4 | 32 | 0 | 84 | 11 | 37 |
| | hle | 24 | 6 | 82 | 7 | 37 |

GPU time per frame, median of each run (ms):

| Scene | pm4 runs | hle runs |
| --- | --- | --- |
| Title screen | 13.8, 13.4, 11.3 | 14.9, 11.3, 10.8 |
| Main menu | 11.0, 13.2, 11.5 | 15.0, 10.4, 11.8 |
| Campaign start | 9.7, 9.7 | 11.1, 11.4, 11.1 |

Whole runs (mean per run):

| | pm4 | hle |
| --- | --- | --- |
| Pipeline creations | 344 | 353 |
| Shader translations | 431 | 440 |
| Frames over 50 ms (loading and first shader compiles) | 4.3 | 4.3 |

Images, share of pixels differing by more than 16 (of 255) from the closest `pm4` capture,
median / worst capture of the scene:

| Scene | pm4 against other pm4 runs (noise floor) | hle against pm4 |
| --- | --- | --- |
| Title screen (logo movies, animated title) | 54.8 / 90.0 | 25.2 / 52.9 |
| Main menu (animated background) | 58.5 / 89.9 | 43.6 / 58.0 |
| Campaign cutscene | 7.1 / 92.5 | 5.8 / 8.9 |
| Campaign start | 7.4 / 63.3 | 5.6 / 8.4 |

The HLE runs are as close to the PM4 runs as PM4 runs are to each other in every scene (the high
title and menu figures are animation and timing, as the side-by-side previews show). Earlier in
phase 3, the title screen, the main menu and the first playable moment were also compared by eye
side by side, with the same result, on Metal and on Vulkan through MoltenVK.

## What the measurements found

- **A correctness bug, fixed.** The first set of runs showed a dimmer light shaft at the
  campaign start in `hle`. Investigating it found a real hole in the transfer skipping: a clear
  covers the viewport, while EDRAM ownership changes whole rows the surface pitch wide, and the
  game clears targets whose viewport is narrower than the pitch, so skipping their transfers left
  columns stale. The skip now requires the scissor and viewport to cover every row and column
  that changes owner (commit "skip clear transfers only when the clear covers the whole range").
  The dimmer shaft itself turned out to be timing, not that bug: the shaft and parts of the HUD
  change over time, and a `pm4` run launched the same way showed the same frame; the images above
  are from the fixed build.
- **Less work, not less GPU time.** The HLE path removes 7 of 18 transfers at the title and menu
  and 8 of 32 at the campaign start, and 1 to 2 render passes, with every other host counter equal
  or lower (draws, dispatches, copies, tiles transferred). Yet at the campaign start the GPU time is
  higher in all three `hle` runs (11.1 to 11.4 ms) than in both `pm4` runs (9.7 ms), a gap the
  earlier set of runs also showed. In the title and menu the spread between runs (10.8 to 15 ms) is
  larger than any difference between modes. The cause is not known; measuring per pass with Metal
  System Trace in both modes is the way to find it (the counters say what work is issued, not what
  each piece costs).
- **No cost elsewhere:** the same pipelines and shader translations (the HLE path changes no
  shader or pipeline), the same number of hitches over 50 ms, and no errors in any run log.
- **The scene that matters most was not measured.** These scenes run at the 30 fps cap with 10 to
  14 ms of GPU work. The 46 ms frames of
  [metal-backend-design.md](../metal-backend-design.md) are deeper in the campaign, which the
  scripted controller cannot reach (it does not steer).

## Success criteria

| Criterion | Status |
| --- | --- |
| A: title screen and main menu with `gfx = hle` | Met on Metal and on Vulkan (MoltenVK) |
| B: first campaign chapter | Its start is met (chapter loads, cutscene and first playable moment render as in `pm4`, no errors); playing it through needs someone at the controls |
| Measured comparison | This document |
| CI green on macOS and Linux | Runtime kit workflow green on macOS, Linux and Windows for the phase 3 commits; Content check green on every push |
| Content check active | `scripts/check-content.sh` in the Content check workflow, on every push and pull request |

## Limits of these results

- One machine (M4), Metal; Vulkan only through MoltenVK on the same machine, and only for the
  title and menu. Linux and Windows were built by CI, not run.
- Scenes at the frame rate cap; heavier scenes need a person playing with the comparison build.
- GPU time on this machine varies by up to 40% between runs of the same scene, so only
  differences that repeat across rounds (the campaign start gap) mean something.

## Next steps

1. Find why the campaign start costs about 1.4 ms more in `hle` (Metal System Trace in both
   modes on the same scene).
2. Measure a heavy scene with a person playing both modes.
3. The larger HLE piece left: resolves still end the render pass and go through EDRAM dumps (37
   of 82 pass ends at the campaign start); resolving straight from the host texture and planning
   passes across resolves is where the remaining render pass count is.
