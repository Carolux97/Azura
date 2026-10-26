# HLE graphics, phase 4: gfx = pm4 against gfx = hle

What the HLE path built in phase 3 ([02-design.md, as built](02-design.md#as-built-in-phase-3))
changes, measured on the same scripted runs in both modes. Everything here was measured on one
machine, a MacBook Pro with an Apple M4, on Metal; the Vulkan check is noted where it applies.

**In short:** the HLE path renders the same images as the PM4 path (within the run-to-run noise),
makes 25 to 40% fewer render target transfers and a few fewer render passes, and costs nothing
extra in pipelines or hitches. At the 30 FPS cap the GPU time it reports at the campaign start
looked about 1.4 ms higher; Metal System Traces showed that was the GPU clock, not the work (the
system ran the GPU at its minimum performance state under the lighter HLE load). With the GPU at
the same clock in both modes, the HLE path takes about 3 to 10% less GPU time per frame: its
transfers cost 30% less, and the draw passes, once traces label them correctly, cost about the same. See
[the campaign start gap](#the-campaign-start-gap-explained).

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
  (33.3 ms) in both modes; the CPU side of the command processor is not measured separately. At
  the cap the GPU clock also changes with the load, which makes GPU time a poor measure of work
  (see the gap below); uncapped runs fix the clock.

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
- **Less work; at the cap, a lower GPU clock.** The HLE path removes 7 of 18 transfers at the
  title and menu and 8 of 32 at the campaign start, and 1 to 2 render passes, with every other host
  counter equal or lower (draws, dispatches, copies, tiles transferred). Yet at the campaign start,
  at the 30 FPS cap, the GPU time was higher in all three `hle` runs (11.1 to 11.4 ms) than in both
  `pm4` runs (9.7 ms). The next section explains it. In the title and menu the spread between runs
  (10.8 to 15 ms) is larger than any difference between modes.
- **No cost elsewhere:** the same pipelines and shader translations (the HLE path changes no
  shader or pipeline), the same number of hitches over 50 ms, and no errors in any run log.
- **The scene that matters most was not measured.** These scenes run at the 30 fps cap with 10 to
  14 ms of GPU work. The 46 ms frames of
  [metal-backend-design.md](../metal-backend-design.md) are deeper in the campaign, which the
  scripted controller cannot reach (it does not steer).

## The campaign start gap, explained

Metal System Traces (Instruments' Metal System Trace template, 15 seconds attached to the game at
the campaign start, one run per mode, one game at a time) gave the answer:

| At the 30 FPS cap | pm4 | hle |
| --- | --- | --- |
| GPU performance state over the trace | Medium 61%, Maximum 38%, Minimum 1% | Minimum 100% |
| Thermal state | Nominal | Nominal |

The machine was not hot; the system chose the GPU's minimum performance state for the HLE run,
whose load is lighter, and medium and maximum for the PM4 run. At a lower clock the same work takes
longer, so the GPU time the counters report (the time the GPU is busy) went up while the work went
down. That the lighter load is what lowered the clock is an inference from these two traces: the
system does not say why it picks a state.

Measured again without the frame rate cap (`frame_rate_limit = 150`), so the GPU is busy all the
time in both modes:

| Without the cap | pm4 | hle |
| --- | --- | --- |
| GPU performance state over the trace | Maximum 99.8% | Maximum 99.8% |
| Campaign start, GPU ms per frame (traced runs) | 10.20 | 9.90 |
| Campaign start, GPU ms per frame (two rounds of compare_modes.sh) | 10.29, 9.66 | 9.27, 9.58 |
| Title screen, GPU ms per frame (two rounds) | 6.85, 6.72 | 5.89, 6.57 |

GPU time by kind of work at the same clock (campaign start, ms of GPU time per second, from the
encoder labels; the two modes ran at the same frame rate):

| Work | pm4 | hle |
| --- | --- | --- |
| Render target transfers (fragment, vertex and compute) | 161 | 113 |
| Guest draw passes (fragment and vertex) | 447 | 511 |
| Resolves | 186 | 187 |
| Texture loads | 153 | 149 |

The transfers cost 30% less, as the counters predicted. The guest draw passes looked longer in
the HLE run; the next section shows that was mostly how the trace labels the work.

### Why the draw passes looked longer

A render target transfer opens a render pass on its destination, labelled "Render target
transfer". When the game's next draw uses the same targets (often the clear right after it), the
Metal backend keeps that pass open, so in a trace the draw's GPU time counts as a transfer. The
`draws_in_transfer_passes` counter, which only the Vulkan backend had, now counts those draws on
Metal too: at the campaign start about 120 draws per frame ran inside transfer passes in `pm4`
and 63 in `hle` (77 and 134 at the title screen). Skipping a transfer moves those draws into a pass
labelled "Guest draws" without changing their cost.

With `gpu_debug_markers = true` the Metal backend now gives such a draw a pass of its own, as the
Vulkan backend does with `vulkan_debug_labels`, so traces count it as a draw (this adds render
passes, more of them in `pm4`). Traced again that way, uncapped, both at the maximum performance
state: run to run the GPU speed differed by several percent even for the work that is the same in
both modes (resolves and texture loads were 6 to 7% slower per frame in the run recorded second),
so each kind of work is compared relative to resolves plus texture loads in the same run:

| Campaign start, relative to resolves + texture loads | pm4 | hle |
| --- | --- | --- |
| Guest draw passes, labels as before (earlier pair) | 1.32 | 1.52 (+15%) |
| Guest draw passes, draws in passes of their own | 1.54 | 1.59 (+4%) |
| Render target transfers, draws in passes of their own | 0.38 | 0.27 (-29%) |

So most of the apparent growth was labelling; the 4% left is within the run-to-run variation these
pairs show, and was not pursued further. Which run of a pair goes first also changes its speed by
several percent here, which is why the comparisons above alternate the modes and why single pairs
of traces need this normalisation.

## Pass reduction, phase A: clears as load actions

With `hle_clear_load_actions` (on by default with `gfx = hle`), a game clear that writes every
target it uses completely is not drawn: it becomes a pending clear of those textures, and the
next render pass on them starts with `MTLLoadActionClear` (colour, depth and stencil separately).
Pending clears are applied in a pass of their own before anything reads render targets outside a
render pass (compute and blit work, the backend's own passes, the end of a submission). A clear in
the middle of a pass on the same targets, a target format whose host storage needs a conversion
(gamma, the 2_10_10_10 formats, 16-bit fixed point with a non-zero value) or a 24-bit float depth
other than 0 or 1 is drawn as before.

**What the values are.** `hle_verify_clears = true` draws the clears as before and compares two
texels of each target (a corner and one inside) with what the load action would have written,
byte for byte. That found two things before the feature was right:

- The depth a clear writes is not ClearF's depth argument: Direct3D draws the clear with the
  viewport's depth transform, so an argument of 0 writes 0.9961 and 1 writes 0.0000036 in some
  passes. The depth is now taken from the clear's vertices, run on the CPU.
- Many clears do not cover their surface. With the viewport transform off (as Direct3D draws
  clears) the viewport registers can belong to another draw; the clear's real rectangle, from its
  vertices run on the CPU (`DrawExtentEstimator::EstimateVertexRectangle`, which also handles the
  immediate indices Direct3D uses), is often narrower than the pitch (0 to 793 of 1280) or offset
  (800 to 818). Phase 3's transfer skipping used the viewport and so also skipped transfers for
  some of those partial clears; it now uses the real rectangle, which skips fewer (4 per frame
  instead of 6 to 9) but only where the clear writes everything.

With both fixed, every check matches (colour, depth and stencil, corner and inside).

**Measured** (two rounds each, alternating, M4):

| Campaign start | pm4 | hle without load actions | hle |
| --- | --- | --- | --- |
| Clears as load actions per frame | 0 | 0 | 4 |
| Transfers / skipped per frame | 32 / 0 | 28 / 4 | 28 / 4 |
| Render passes per frame | 84 | 83 | 83 |
| Images, % of pixels over 16 from the closest pm4 capture (median / worst) | 7.5 / 11.0 (noise floor) | 5.7 / 7.7 | 6.0 / 7.7 |

| Uncapped, frame ms / GPU ms per frame, each run | hle without load actions | hle |
| --- | --- | --- |
| Title screen | 8.17 / 6.97, 8.05 / 6.94 | 8.39 / 7.22, 8.15 / 7.03 |
| Main menu | 6.82 / 6.25, 6.81 / 6.31 | 6.81 / 6.27, 6.83 / 6.28 |
| Campaign start | 9.34 / 9.30, 11.17 / 8.98 | 9.17 / 9.19, 9.21 / 9.21 |

The load actions are correct but make no measurable difference: only 3 or 4 clears per frame
qualify (most are partial or in the middle of a pass), they did not end passes of their own (the
clear was drawn in the pass that follows it), and a full-screen rectangle without textures is
cheap on a tile-based GPU. The rectangle and depth fixes are the useful part: they make the
transfer skipping safe.

## Pass reduction, phase B: clears that keep part of the target

**Partial clears.** A clear whose rectangle (its vertices run on the CPU) covers only part of the
range that changes owner still needs the old contents outside it. With
`hle_clear_transfer_cutouts` (on by default with `gfx = hle`), when the clear writes every target
it uses completely within its rectangle, the whole pixels of that rectangle (within the scissor
and the pitch) are cut out of the transfers into them, with the cutout mechanism the SDK already
uses for resolve clears: transfers entirely inside it are dropped, the others copy only the parts
outside it. One rectangle serves every target of the draw, so it is used only when the clear
writes all of them; a clear draw normally uses one target. On Vulkan the dropped transfers are
dropped too, and the partial ones run whole (correct, without the saving), since only the Metal
execution passes the rectangle on.

**Depth-only and stencil-only clears are not handled.** They keep the other aspect everywhere,
and the SDK's transfers copy depth and stencil together; transferring one aspect alone needs new
transfer shaders. They keep their full transfers, and the render target cache logs them once.

**Measured** (two rounds each, alternating, M4):

| | pm4 | hle without cutouts | hle |
| --- | --- | --- | --- |
| Transfers cut out per frame (title / menu / campaign start) | 0 | 0 | 4 / 4 / 5 |
| Transfers per frame (campaign start) | 32 | 28 | 28 |

| Uncapped, frame ms / GPU ms per frame, each run | hle without cutouts | hle |
| --- | --- | --- |
| Title screen | 7.22 / 6.42, 7.58 / 6.47 | 7.03 / 6.36, 7.16 / 6.37 |
| Main menu | 6.89 / 6.33, 6.81 / 6.33 | 6.82 / 6.33, 6.83 / 6.32 |
| Campaign start | 11.24 / 9.70, 9.30 / 9.30 | 11.12 / 9.34, 9.22 / 9.23 |

About 1 to 2% less GPU time, within the noise of two rounds. Images: at the title and menu the
HLE runs are 0.4 to 1.7% (median) from the closest PM4 capture; at the campaign start the one
cutout run that reached it matched PM4 at the noise level until second 216 and then differed by
27 to 32%, which the previews show is a different camera position and HUD state (that run reached
the chapter later), not a rendering difference.

## Is the CPU the bottleneck now? (phase D: the case for option B)

Option B of the design (replacing the guest D3D calls and reading the device structure instead of
the command stream) saves CPU work: register tracking and packet parsing. It is worth doing only
if the CPU limits the frame rate. Without the frame rate cap, so that the game runs as fast as it
can, after phases A and B (two rounds each of `hle`, M4, Metal):

| Uncapped, median per frame | Title screen | Main menu | Campaign start |
| --- | --- | --- | --- |
| Frame time | 7.0 to 7.6 ms | 6.8 to 6.9 ms | 9.2 to 11.2 ms |
| GPU busy time | 6.4 to 6.5 ms | 6.3 ms | 9.2 to 9.7 ms |
| Command processor waits for the GPU | 0 | 0 | 0 |
| CPU time requesting textures in draws (`draw_texture_load_us`) | 0.17 ms | 0.17 to 0.20 ms | 0.25 to 0.27 ms |

The GPU is busy for 90 to 100% of every frame and the command processor never waits for it, so
these scenes are limited by the GPU, not the CPU: a faster command processor would not shorten
the frame. The gaps of up to 1.8 ms between frame and GPU time in some campaign runs are pacing
(the 90th percentile frame is 10 to 13.6 ms), not a CPU-bound steady state.

What this does not cover, and is not measured: the CPU time of the command processor thread
itself (there is no counter for it; the counters above are a part of it), and heavy gameplay
scenes, where far more draws per frame raise the CPU work per frame faster than the GPU work.

**Conclusion: option B is not justified by these measurements.** The GPU-side work (resolves,
texture loads, transfers, render passes; phase C) is where frame time can still be saved.
Revisit with a heavy scene played uncapped: if its frame time then exceeds its GPU time by a wide
margin with the GPU idle between submissions, the CPU has become the limit.

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

1. Measure a heavy scene with a person playing both modes, uncapped, where a GPU-time saving
   becomes frame rate.
2. The larger HLE piece left: resolves still end the render pass and go through EDRAM dumps (37
   of 82 pass ends at the campaign start); resolving straight from the host texture and planning
   passes across resolves is where the remaining render pass count is.
