# Native Metal backend and sample-space EDRAM: design

Card 29. This document collects what was measured, what it rules out and what is built, in
order. All numbers are from a base M4 (10-core GPU, 24 GB) at the campaign start of Gears of
War 3, unless they come from a microbenchmark (marked as such).

## 1. Where the GPU time goes today

The runtime emulates the Xenos GPU with the ReXGlue SDK (a Xenia derivative) and its Vulkan
backend, which runs on Metal through MoltenVK. The game is GPU-bound: the command processor
waits ~48 ms per frame for the GPU and the guest render thread spin-waits on GPU fences.

Measured with the profiling tools of PR #13 (perf_log_csv, vulkan_debug_labels, Metal System
Trace exports), ~46 ms of GPU time per frame:

| Work | ms per frame |
|---|---|
| Render target transfers (render passes) | ~22 |
| Render target transfers (compute) | ~2 |
| Game draws | ~15 |
| Texture loads | ~2.6 |
| Presentation and unlabelled | ~2.8 |
| Resolves | ~0.8 |
| Guest memory uploads | ~0.4 |

Per frame there are ~420 render passes, ~780 barriers, ~320 dispatches, ~1,230 guest draws and
~55 resolves. Most transfers move the same EDRAM regions between 1x and 4x MSAA (for example
D24S8 at tile 1440 with a pitch of 14 tiles, D24FS8 at tile 0, a 16_16_16_16 target at tile
1624), and none of them is redundant: something writes the copy between every two transfers.

## 2. What was ruled out, with data

| Idea | Result |
|---|---|
| Guest memory fences | ~5% |
| Vertical blank pacing | frames take ~55 ms with vsync on or off |
| Present effect (FSR) | not available on macOS at all (card 26) |
| Smaller render area per pass | 40% less area, no change in fragment time |
| Merging a draw's uploads | saves almost no render passes |
| MSAA emulation mode (native 2x) | no change |
| EDRAM in a buffer with interlock (fsi path) | 0 transfers but slower: 66 ms frames, draws ~49 ms |

## 3. Microbenchmarks (native Metal, no MoltenVK, no game)

Sources: `edram_bench.mm`, `pass_overhead.mm`, `msaa_bench.mm` (kept with the card notes).

| Measurement | Cost |
|---|---|
| Empty render pass, any size from 256x144 to 2560x1440 | ~44 us |
| Empty compute encoder | ~0.7 us |
| Render pass with 0, 4 or 32 small draws | 41-45 us |
| Full 1280x720 transfer between 4x and 1x | 115-166 us (matches the ~154 us seen in the game) |
| EDRAM in a buffer loaded and stored per pass, 1x / 4x | 125 / 610 us |
| 8 blended layers: fixed function / tile memory read / ordered memory | 41 / 72 / 172 us |
| Lit surface, 4 layers: 1x / 4x MSAA / sample space (2560x1440 1x) | 45 / 92 / 124 us |
| Game pattern, draw 4x then read as 1x: MSAA + transfer / sample space | 540 / 433 us |

Conclusions:

1. On Apple GPUs a render pass has a fixed cost of ~44 us that does not depend on its size,
   while a compute encoder costs ~0.7 us. ~420 passes per frame are on the order of 18 ms of
   fixed cost (extrapolation). The number of render passes is a first-order cost, also with
   native Metal.
2. Render targets must stay textures. A plain buffer loses the hardware's lossless render
   target compression: loading a 4x region from a buffer costs 4-5 times a texture transfer.
3. Fixed-function blending and depth are the cheapest; reading the pixel from tile memory costs
   1.75x, and ordered device memory access (what the interlock path does) 4.2x. Software
   output merging only where the hardware cannot do the format.
4. Storing 4x surfaces in sample space costs ~35% more fragment work than real 4x MSAA but
   needs no transfer when the same EDRAM region is used as 1x, which wins in the game's
   pattern.

## 4. EDRAM design: sample-space render targets

The Xenos EDRAM stores a multisampled surface exactly like a single-sampled one with twice the
height (2x MSAA) and twice the width (4x MSAA): 2048 tiles of 80x16 samples (xenos.h). The
host keeps each render target in that same space: one texel per EDRAM sample.

- Host render targets are keyed by EDRAM base, pitch in tiles, format and depth or color, not
  by MSAA. A 4x target of 560x360 pixels with a pitch of 14 tiles is the same host texture
  (1120x720 texels) as the 1x target of 1120x720 pixels at the same base and pitch.
- Switching an EDRAM region between 1x, 2x and 4x is free: no transfer, no new texture.
  Transfers remain only for format changes (depth to color, 32 to 64 bpp), which are a
  minority of the transfers measured.
- Drawing to a multisampled guest target rasterizes on the host at 1x over the sample-space
  texture: the viewport and scissor are scaled by 2 (4x: both axes; 2x: vertical), and the
  pixel shader receives the guest pixel position (host position divided by the same factor).
  Every sample is shaded (supersampling). Depth, stencil and blending stay fixed function.
- Resolves read samples from the sample-space texture (2x2 or 1x2 texels per guest pixel)
  instead of from a multisampled texture.
- Known approximations: sample positions become the texel centres of the 2x2 grid instead of
  the Xenos rotated pattern; alpha to coverage cannot use real coverage (dithered or
  alpha-test fallback). To check visually.

This lives in the common render target cache as a new path, selected by a setting
(`edram_sample_space`), so the Vulkan backend and Linux can use it too and both paths can be
compared on the same build.

## 5. Native Metal backend

A third backend next to Vulkan and D3D12, built only on macOS, selected with
`gpu_backend = "metal"` (the GPU plugin already chooses the backend by name). It implements the
same interfaces as the Vulkan backend: `GraphicsSystem`, `CommandProcessor`, `SharedMemory`,
`RenderTargetCache` (host render target and sample-space paths), `TextureCache`,
`PrimitiveProcessor`, `PipelineCache` and the presenter.

- Shaders: the existing Xenos to SPIR-V translator, then SPIRV-Cross to MSL when a pipeline is
  created (not per frame), cached on disk. A direct Xenos to MSL translator is a later option
  if the data asks for it. Apple's Metal Shader Converter, used by xenia-mac, is closed source
  and adds DXBC and DXIL steps, so it is not used.
- Memory: the guest's 512 MiB shared memory is one `MTLStorageModeShared` buffer over the
  guest memory, with no copies (unified memory), as xenia-mac does. Uploads disappear.
- Passes: one render encoder per run of draws to the same targets. Work that ends a pass in
  Vulkan (barriers, uploads, texture loads) is either gone (uploads, Metal tracks hazards
  inside a command buffer) or moved before the pass (texture loads run as compute first).
- Reference: xenia-mac (BSD, https://github.com/wmarti/xenia-mac) for device, heap, texture
  and presenter plumbing. Its EDRAM is the host render target path with transfers.

## 6. Order of work

1. Sample-space EDRAM in the common render target cache, tested on the current Vulkan backend
   first: it is the part with the measured gain, it is independent of the API, and the
   existing tools measure it (transfers per frame, GPU time per label, frame time).
2. Metal backend bring-up behind `gpu_backend = "metal"`: presenter, shared memory, command
   processor, pipelines from SPIR-V through SPIRV-Cross, textures, render targets in sample
   space, resolves, up to the menus and then the campaign.
3. Visual parity with Vulkan (menus, Bink intro, campaign) and measurements against the
   baseline.

The Vulkan backend and Linux keep working throughout: every change to the common layer is
behind a setting whose default keeps today's behaviour until it is measured.

## 7. First results (Metal bring-up)

Measured on the same machine and build, in a window, from the start of the campaign (prologue
cutscene, then the first rooms). Medians over 300-frame windows of perf_log_csv. The game caps
at 30 FPS, so 33.3 ms is the floor.

| Gameplay | Vulkan, host MSAA (19 Oct) | Vulkan, sample space | Metal, sample space |
|---|---|---|---|
| Frame time, first rooms | 50.6 ms | 33.6 ms | 33.4 ms |
| Frame time, room with light shafts | - | 41.8 ms | 36-39 ms |
| Render passes per frame | 515 | 225-360 | 112-216 |

- Bink intro, title screen, main menu and campaign render the same as with Vulkan (side by
  side window captures: title screen 40-41 dB PSNR, gameplay pairs at the same moment differ
  only in character poses and camera). The black patches on Anya's face and occasional white
  flashes in some effects happen with both backends: open issues, not Metal regressions.
- In the room with light shafts the command processor waits ~32 ms per frame for the GPU, with
  ~60 render target transfers per frame: the next target for render pass reduction.
- Not yet in the Metal backend: tessellation (domain shaders), resolution scaling, host
  occlusion queries and the Dear ImGui overlays.
- FXAA (the Antialiasing option of the game's Video menu) is a Metal shader written for Apple
  GPUs (half precision colors, quality as a function constant) after the gamma ramp pass with
  luma: ~0.18 ms more GPU time per frame at 1280x720 (0.32 vs 0.14 ms for the swap pass).

## 8. Guest fences written when the GPU is done

With the shared memory over the guest memory, looking at one spot of the first rooms broke the
whole picture (quadrants of misread render target data). It did not happen with Vulkan, nor
with Metal copying guest memory into a separate buffer as Vulkan does
(`metal_shared_memory_copy`, a diagnostic setting), so it was a race on guest memory.

The cause: the command processor wrote the fences the guest waits on (EVENT_WRITE_SHD) as soon as
it read them, while the GPU runs the frame later, up to a few frames behind. The guest then
reused memory the GPU had not read or written yet. Measured at the campaign start: 14-28 fences
per frame (68-70 in the heaviest scenes), half of them with draws since the previous one, and 0
other memory writes.

`metal_gpu_accurate_fences` (on by default) sends the work before each such fence to the GPU and
writes the fence from the command buffer's completion handler. Results on the same route:

| Gameplay | Fences when read | Fences when the GPU is done |
|---|---|---|
| Frame time median / p90 / p99 | 33.4 / 35.1 / 40.6 ms | 33.4 / 34.7 / 40.0 ms |
| Submissions per frame | 1 | 8-35 |
| Render passes per frame (median / p90) | 112 / 194 | 124 / 146 |
| Command processor waiting for the GPU (p90 / p99) | 6.1 / 34.9 ms | 0 / 0 ms |

The full-screen glitch and the black patches on Anya's face are gone, and every gameplay window
stays at the 30 FPS cap, the heaviest scenes included. The white flashes in some effects remain
and happen with Vulkan too.

## 9. Render target transfers: draws without clipping

`log_render_target_transfers` logs, every 4096 transfers, the totals by reason (depth and color,
other EDRAM base, pitch, format) and the source and destination pairs that copy the most tiles;
the `render_target_transfers` and `render_target_transfer_tiles` counters go to perf_log_csv.

In the room with light shafts, a Metal System Trace gave ~10 ms of GPU time per frame to
transfers, as much as the game's draws. Almost half of the copied tiles were between render
targets at different bases: the main depth buffer at tile 0 and a depth buffer at tile 1440 whose
range wrapped around the end of the EDRAM over it, back and forth about 7 times per frame, and
copies of 1024 tiles out of a 720-tile (1280x720) depth buffer. The game clears with draws, not
with resolves (no resolve clears in a minute of gameplay), so turning transfers of cleared data
into clears does not apply.

The cause was the height estimated for draws without clipping and with the default 8192 scissor
(screen-space quads such as clears): with nothing to clamp it, they claimed up to the whole EDRAM.
`execute_unclipped_draw_vs_on_cpu` runs the vertex shader of those draws on the CPU to get their
real extent (on by default in Xenia, off in the SDK), and azura.default.toml now turns it on.
Same room, 15 s traces:

| Light-shaft room | Before | With the setting |
|---|---|---|
| Game GPU time per frame | 26.4 ms | 20.5 ms |
| Render target transfers (GPU) | 10.2 ms | 1.9 ms |
| Transfers per frame (median / p90) | 95 / 181 | 32 / 35 |
| Tiles copied per frame (median) | 66,700 | 8,600 |
| Frame time p90 / p99 | 39.3 / 50.0 ms | 34.5 / 35.6 ms |

The transfers left are almost all the 1280x720 region at tile 720 used in turn as 8_8_8_8 color,
D24S8 depth and 2_10_10_10 (fixed and float) color, which the game does reinterpret. The CPU
cost was ~0.3 ms more per frame on the GPU command thread (median). No new visual artefacts.

## 10. Debugging tools for visual artifacts

Three settings in `azura.toml`, all off by default and with no cost when off. They need a restart,
and they write into `metal_gpu_capture_path`. The keys are bound by physical position.

- **GPU capture** (`metal_gpu_capture_frames = N`): the key `bind_metal_gpu_capture` (RBracket, two
  keys right of P) saves a Metal capture of the next N frames as a `.gputrace` document for Xcode.
  Each capture copies the shared memory many times and takes 12 to 26 GB; replaying one in Xcode
  takes minutes.
- **Frame history** (`metal_frame_history_frames = N`): the last N guest front buffers are kept in
  a ring of textures, and the key `bind_metal_frame_history` (LBracket, the key right of P) saves
  them as PPM images in a `frames_<time>` folder. The key can be pressed after an artifact was
  seen, even one that lasted a few frames. 90 frames (~3 s) take ~330 MB of memory.
- **Pixel history** (`metal_pixel_history = true`, with the frame history): the frame after
  `bind_metal_frame_history` samples a 64x36 grid of render target 0 after every draw (the 16:9
  top of the host render target, where the screen-sized targets are), in a compute dispatch that
  ends the render pass, and logs per draw the render target, blend and depth state, the shader
  hashes and the pixel shader's texture fetch constants (format, address, size, exponent bias, LOD
  bias). It saves `pixels_<time>.txt` and `pixels_<time>.half4`;
  `scripts/pixel_history.py` lists the draws that change each part of the screen, the history of
  one grid point, and the grid after any draw as an image. That frame is slow.
