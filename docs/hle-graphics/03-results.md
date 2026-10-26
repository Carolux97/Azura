# HLE graphics, phase 4: gfx = pm4 against gfx = hle

What the HLE path built in phase 3 ([02-design.md, as built](02-design.md#as-built-in-phase-3))
changes, measured on the same scripted runs in both modes. Everything here was measured on one
machine, a MacBook Pro with an Apple M4, on Metal; the Vulkan check is noted where it applies.

**In short:** the HLE path renders the same images as the PM4 path (within the run-to-run noise),
makes fewer render target transfers (4 fewer per frame, and 4 or 5 more cut down, with the final
rules) and costs nothing extra in pipelines or hitches. At the 30 FPS cap the GPU time it reports
at the campaign start looked about 1.4 ms higher; Metal System Traces showed that was the GPU
clock, not the work (the system ran the GPU at its minimum performance state under the lighter HLE
load). With the GPU at the same clock in both modes, its transfers cost 30% less and the draw
passes, once traces label them correctly, about the same; with the final rules for which clears
qualify, it takes 1 to 4% less GPU time per frame in the menu and at the campaign start. See
[the campaign start gap](#the-campaign-start-gap-explained) and
[after phases A and B](#after-phases-a-and-b-pm4-against-hle-uncapped).

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

## After phases A and B: pm4 against hle, uncapped

Two alternating rounds with the build of phase B, uncapped (one `hle` run reached the campaign):

| Median per frame, each run | pm4 | hle |
| --- | --- | --- |
| Transfers (title and menu / campaign start) | 18 / 32 | 14 / 28 |
| Title screen, frame ms / GPU ms | 7.37 / 6.74, 7.76 / 6.88 | 7.15 / 6.13, 8.31 / 7.18 |
| Main menu, frame ms / GPU ms | 7.36 / 6.57, 7.02 / 6.51 | 6.84 / 6.28, 6.76 / 6.25 |
| Campaign start, frame ms / GPU ms | 9.51 / 9.43, 9.38 / 9.30 | 10.98 / 9.26 |

The HLE path takes 1 to 4% less GPU time in the menu and at the campaign start, and is within the
noise at the title. These are smaller savings than the earlier figures in this document, which
came before the clear coverage used the clear's real rectangle (phase A): some of the transfers
skipped then were not safe to skip.

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

## Native resolves, step C1: resolved textures copied from the render target

With `hle_native_resolves` (off by default, `gfx = hle` only), a resolve whose destination the
texture cache already holds with exactly the resolve's key copies the render target's rectangle
into that texture with a blit, and the texture is not loaded back from memory. The shared memory
is still written as before, so anything else reading the range finds the same bytes
(`resolves-plan.md`, C1).

Which resolves qualify: 1x, 32 bits per pixel, no exponent bias, no destination offsets, formats
8_8_8_8, 16_16_FLOAT and 32_FLOAT, one render target owning the whole source range with the same
base, pitch and format, and a destination texture with one mip level and the same host pixel
format. The 8_8_8_8 resolves swap red and blue (`copy_dest_swap`); the copy keeps the render
target's order and the texture's views swap the two channels instead. Everything else goes
through the usual path. The per-resolve bisection list of the plan (`hle_resolve_legacy`) is not
built: no resolve needed it.

**What the values are.** `hle_verify_resolves = true` loads the textures from memory as before
and compares three texels of each with a copy made the C1 way. Every eligible resolve in the
scripted scenes matches: 16_16_FLOAT 640x360 (endianness 2), and 8_8_8_8 1280x720 (endianness 0
and 2, red and blue swapped).

**Measured** (two rounds each, alternating, M4, muted):

| Per frame, at the cap | pm4 | hle | hle with native resolves |
| --- | --- | --- | --- |
| Resolves to textures (title, menu / campaign start) | 0 / 0 | 0 / 0 | 3 / 2 |
| Pass ends by a texture load (title, menu / campaign start) | 10 / 13 | 11 / 13 | 10 / 13 |
| Render passes (title / menu / campaign start) | 58 / 60 / 84 | 58 / 61 / 83 | 58 / 61 / 83 |
| Host compute dispatches (title / campaign start) | 113 / 142 | 109 / 139 | 106 / 137 |

| Uncapped, frame ms / GPU ms per frame, each run | hle | hle with native resolves |
| --- | --- | --- |
| Title screen | 6.77 / 6.43, 8.06 / 6.91 | 6.66 / 6.05, 6.43 / 6.41 |
| Main menu | 6.84 / 6.37, 6.76 / 6.30 | 6.73 / 6.35, 6.49 / 6.45 |
| Campaign start | 11.10 / 9.53, 11.02 / 9.42 | 11.03 / 9.73, 9.12 / 9.13 |

Images match `pm4` like `hle` does at the title and the campaign start (median 0.3 and 5.8% of
pixels over 16, against `pm4`'s own 10.7 and 5.5). One `hle` run and one `hle` with native
resolves run left the main menu up to a minute earlier than the `pm4` runs, so their menu
captures show the loading screen and the opening cutscene. Those captures match `pm4`'s captures
of the same screens (the cutscene at a slightly different camera position), so the difference is
pacing, not rendering.

C1 is correct but saves little: 2 or 3 texture loads (3 compute dispatches) per frame, and no
render pass, because the pass the load ended now ends at the next transfer or resolve. The GPU
time differences are within the run-to-run noise. Its value is as the base for C2 and C3, which
remove the EDRAM dump and the memory copy of the same resolves.

## Native resolves, step C2: resolves written to memory from the render target

With `hle_native_resolves`, a resolve that copies one render target's bits unchanged (the
conditions of C1 except that the destination may be anywhere in the texture, and the fast 32bpp
copy shader, not an array destination) is written to the shared memory by a compute shader that
reads the render target (`resolve_from_render_target_float` and `_uint` in the render target
cache's Metal source). It packs each pixel as the EDRAM dump does, swaps red and blue and the
bytes as the fast resolve copy does, and stores it at its 2D tiled address. The EDRAM dump and the
copy from it are skipped for those resolves. `hle_resolve_from_render_targets = false` keeps C1
alone, to compare.

**What the values are.** With `hle_verify_resolves`, resolves work as before and then the new
shader runs in a checking mode: for every pixel it compares what it would write with what the
usual path wrote at that address, and counts the mismatches. Every kind of eligible resolve in
the scripted scenes matches, every pixel: 8_8_8_8 1280x720 (endianness 0 and 2), 640x360 and
16x512 (endianness 2), all with red and blue swapped, and 16_16_FLOAT 640x360 (endianness 2).

**Measured** (one run each, M4, muted; the 10-minute budget per measurement leaves no rounds):

| Per frame, at the cap | C1 only | C1 and C2 |
| --- | --- | --- |
| Resolves written from the render target (title, menu / campaign start) | 0 / 0 | 8 / 10 |
| Host compute dispatches (title, menu / campaign start) | 106 / 137 | 98 / 127 |
| Render passes (title / menu / campaign start) | 58 / 61 / 83 | 58 / 61 / 83 |

| Uncapped, one run each, frame ms / GPU ms | C1 only | C1 and C2 |
| --- | --- | --- |
| Title screen | 6.5 / 6.5 | 6.3 / 6.2 |
| Main menu | 6.7 / 6.6 | 6.4 / 6.3 |

The image comparison of these runs says nothing: the only `pm4` run stayed in the intro video
the title plays when left alone, so it never showed the title and menu the other runs captured,
and the campaign captures differ in the camera's position (the input timing of the scripted
controller). The captures of C1 and C2 look right, and the per-pixel check above is the proof of
correctness.

C2 removes 8 to 10 compute dispatches per frame (the dumps of those resolves), and with them the
EDRAM buffer traffic. The 0.3 ms less GPU time in both scenes is in the direction expected, but
one run cannot tell it from the noise. No render pass is saved: the resolve's compute still ends
the pass.

## Native resolves, step C4: how many pass breaks a resolve inside the pass could save

C4 (a tile shader doing the resolve inside the render pass, Apple GPUs only) only saves a pass
break when the pass after the resolve draws to the same attachments again. Two counters measure
that before any tile shader is written: `render_passes_resumed_after_resolve` (a render pass
begun on exactly the attachments of the pass the last resolve ended) and
`render_passes_resumed_after_native_resolve` (the same, when that resolve was written from the
render target, C2).

**Measured** (one 100-second run with C1 and C2, title and menu; per frame):

| | Title screen | Main menu |
| --- | --- | --- |
| Render passes | 58 | 61 |
| Pass ends by a resolve | 28 | 29 |
| Passes resumed on the same attachments after a resolve | 19 | 20 |
| Of them after a resolve written from the render target (C2) | 4 | 4 |

So a third of the render passes start again on the attachments a resolve just interrupted, but
only 4 per frame follow a resolve C2 handles. Which resolves the others are (a temporary log
during one run, by render target format, destination format, exponent bias, size; about 2,240
frames):

| Resolve | Per frame | Native today |
| --- | --- | --- |
| 16_16_16_16 (fixed) 328x184 to 16_16_16_16, exponent bias -5, endianness 8 in 16 | 12.6 | No: 64 bits per pixel, and the bias scales the values |
| 16_16_FLOAT 640x360 to 16_16_FLOAT, endianness 8 in 32 | 4 | Yes (C2) |
| 16_16_16_16 (fixed) 192x96, as the first row | 1 | No |
| 2_10_10_10 1280x720 to 2_10_10_10, no bias | 1 | No: its EDRAM belongs to a 2_10_10_10_FLOAT target (see below) |
| 2_10_10_10_FLOAT_AS_16_16_16_16 1280x720 to 2_10_10_10, exponent bias -5 | 1 | No: converts |

The repeated small 16_16_16_16 resolves (a chain of passes on a 328x184 target, likely a blur or
a luminance reduction; not verified) are most of the breaks C4 could remove. Before a tile shader
can do them, the native resolve has to handle 64-bit pixels and the exponent bias conversion.

What this does not say: what a pass break on a 328x184 target costs. Its attachments are small
(about 470 KB at 64 bits per pixel), so storing and loading them is cheap; the fixed cost of a
render pass on this GPU was not measured.

## Native resolves, C2 extended: 64-bit conversions

The conversions of the full 64bpp resolve copy were read from its SPIR-V (decompiled with
SPIRV-Cross; the shader is the SDK's own) and done in `resolve_from_render_target_64bpp`: each
16-bit component of a 16_16_16_16 target is the EDRAM's -32...32 value (`max(-1, x * 32 / 32767)`,
the copy's own clamp), a 16_16_16_16_FLOAT one is a half; both are scaled by 2 to the exponent
bias, swapped, and packed as 16_16_16_16 (unsigned normalized) or 16_16_16_16_FLOAT. 2_10_10_10 was
added to the bitwise copies. Only sample 0 is handled (at 1x, the other sample selects read a
neighbouring pixel), and the EDRAM pitch of 64-bit targets is compared in 64-bit tiles.

**Checked** with `hle_verify_resolves`, every pixel: 16_16_16_16 to 16_16_16_16 with bias -5 at
192x96, 328x184 and 640x360, and 16_16_16_16 to 16_16_16_16_FLOAT at 640x360, besides the earlier
kinds.

The 2_10_10_10 resolve of these scenes is still not native: the EDRAM it reads belongs to a
2_10_10_10_FLOAT render target, so the game reinterprets float bits as fixed-point ones, which
needs the float 7e3 packing (as does the 2_10_10_10_FLOAT_AS_16_16_16_16 resolve). One of each per
frame.

| Per frame (title / menu, one 100-second run each) | C1 only | C1 and C2 extended |
| --- | --- | --- |
| Resolves written from the render target | 0 / 0 | 26 / 26 |
| Passes resumed after a resolve, of them after a native one | 19 / 20, 0 | 19 / 20, 17 / 18 |
| Host compute dispatches | 106 / 106 | 80 / 80 |

## Native resolves, step C4 spike: the resolve inside the render pass

With `hle_resolve_in_render_pass` (experimental, off), a native resolve whose render target is a
color attachment of the open render pass is written by a tile function inside that pass
(`dispatchThreadsPerTile`, reading the attachment through an implicit imageblock), and the pass
goes on. The textures it updates are not copied from the render target (a copy would end the
pass): they load from memory as before.

**Checked** with `hle_verify_resolves`: the tile function writes a scratch copy while the pass is
open, and a compute shader compares it with what the usual path wrote, dword by dword. Every kind
in the scenes matches (8_8_8_8 1280x720 and 16x512, 16_16_FLOAT 640x360, and the three
16_16_16_16 kinds).

**Measured** (uncapped, one 100-second run each):

| Per frame (title / menu) | C2 extended | C4 spike |
| --- | --- | --- |
| Pass ends by a resolve | 28 / 29 | 4 / 4 |
| Pass ends by a texture load | 10 / 10 | 27 / 28 |
| Render passes | 58 / 61 | 54 / 57 |
| Host compute dispatches | 80 / 80 | 58 / 57 |
| GPU ms | 5.8 / 6.2 | 12.9 / 13.2 |
| Images, % of pixels over 16 from the `pm4` capture (median / worst) | 1.4 / 6.3, 1.5 / 2.5 | 4.2 / 7.1, 4.9 / 5.7 |

The spike removes the resolve breaks, but not the passes: the draws after a resolve sample the
textures it wrote, which now load from memory in compute and end the pass instead (only 4 passes
per frame fewer). And the GPU time doubles. Why was not measured; a likely cause (not verified) is
that `dispatchThreadsPerTile` runs over every tile of the pass, not only the resolve's rectangle,
26 times per frame, and waits for the pass's fragment work each time.

For C4 to pay, the tile function would also have to write the textures the next draws sample (the
texture cache's textures are not writable today, and the draws would need a barrier after the
tile stage), and its cost per dispatch has to be found and cut. Until then the option stays off.

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
