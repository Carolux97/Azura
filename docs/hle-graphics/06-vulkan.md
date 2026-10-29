# HLE graphics on Vulkan (Linux, Windows)

What of the HLE path ([02-design.md](02-design.md), [03-results.md](03-results.md)) works on the
Vulkan backend, how it was ported from Metal, how it was checked, and what is left. Linux and
Windows draw with Vulkan; `gfx = hle` is not the default there (`pm4` is).

## What runs on Vulkan

| Piece | Option | Vulkan |
| --- | --- | --- |
| Markers, `gfx = hle` | `gfx` | shared code |
| Clear cutouts in transfers | `hle_clear_transfer_cutouts` | shared code |
| Resolves written from the render targets (C2) | `hle_resolve_from_render_targets` | **this document**, off by default (`vulkan_hle_resolve_from_render_targets`) |
| Resolve checks | `hle_verify_resolves` | **this document** |
| Resolved textures from the render target (C1) | `hle_native_resolves` | not yet |
| Clears as load actions | `hle_clear_load_actions`, `hle_verify_clears` | not yet |
| Rational scales | `vulkan_rational_draw_resolution_scale` | **this document**, off by default: resolves to the guest memory only |

## Resolves written from the render targets (C2)

As on Metal (`MetalRenderTargetCache::ResolveFromRenderTarget`): a resolve whose bits one or more
render targets hold is written to the shared memory straight from those render targets, one compute
dispatch per render target owning part of the source (`RenderTargetCache::GetRangeOwners`), with no
EDRAM dump. Each pixel finds its EDRAM tile, the render target owning it and its pixel there, is
packed as the EDRAM holds it, swapped as the resolve asks and stored at its tiled address.

`VulkanRenderTargetCache` (in the SDK patch):

- `GetNativeResolveOwners`: the same cases as Metal (1x MSAA, sample 0, no arrays, endianness up to
  16-in-32; 8_8_8_8, 2_10_10_10, 16_16_FLOAT and 32_FLOAT copied bitwise, 2_10_10_10_FLOAT and
  64bpp converted; depth bitwise; a bitwise copy of a surface written as another 32bpp format), plus
  what is Vulkan's own: the guest resolution or a rational scale (not a whole one, which has the
  scaled resolve buffer), and an integer transfer
  view (`GetColorOwnershipTransferVulkanFormat`) for the formats read as their 16-bit or 32-bit bits.
  Anything else takes the usual path.
- `ResolveFromRenderTarget`: binds the render target's transfer descriptor set (the one the dump
  reads), the destination storage buffer bound from the resolve's destination base, and the check
  result; the constants go in push constants.
- The shaders are GLSL (`kNativeResolveShaderSource`), compiled at start-up with the command
  processor's glslang (`CompileGlslToSpirv`) in four variants: float, uint, 64bpp and depth. They
  pack each format as the Vulkan EDRAM dump does (`GetDumpPipeline`), which differs from Metal in
  places (2_10_10_10 render targets are A8B8G8R8 on Vulkan; D24S8 may be D24 or D32 float), so the
  dump is the reference, not the Metal kernels.
- Only with `gfx = hle` and host render targets; created at start-up, and without them (a failed
  compile) every resolve takes the dump.
- Off by default on Vulkan: `vulkan_hle_resolve_from_render_targets` (false) has to be on as well as
  `hle_resolve_from_render_targets`, which stays on by default for Metal. See "Measured" below.

## Checks

`hle_verify_resolves` runs the usual path (dump and copy), then the native shader compares what it
would write with the memory, counting mismatches into a host-readable buffer; the result is read
when the submission completes (`ReadNativeResolveChecks`, from `CompletedSubmissionUpdated`) and
logged once per kind ("HLE resolve check: ... matches the memory").

Linux, RTX 3050 Laptop, 1280x720, title screen and main menu: every resolve kind matches, no
mismatch:

| Source | Destination |
| --- | --- |
| 8_8_8_8 | 8_8_8_8, 1280x720 (endianness 0 and 8-in-32, red and blue swapped) and the 16x512 table |
| D24FS8 depth | 24_8_FLOAT (depth as a texture) |
| 2_10_10_10_FLOAT | 2_10_10_10, exponent bias 0 and -5 (the 7e3 conversion) |
| 16_16_16_16 | 16_16_16_16 (bias -5) and 16_16_16_16_FLOAT, at 640x360, 328x184 and 192x96 |

Act 1, Chapter 1, played for a few minutes through the opening, the walkway and combat, with the
check on: **600 resolve kinds, no mismatch** (536 from 8_8_8_8 render targets, 17 depth, 1
2_10_10_10, 42 2_10_10_10_FLOAT, 4 16_16_16_16), and the picture looked right. 16_16_FLOAT and
32_FLOAT resolves did not appear in that stretch.

## Measured

Main menu, `gfx = hle`, frame rate not limited, one run each (the first third of each run dropped):

| | Dump (`hle_resolve_from_render_targets = false`) | From the render targets |
| --- | --- | --- |
| Frame | 8.79 ms | 6.94 ms |
| GPU | 4.42 ms | 4.27 ms |
| Resolves per frame | 21, none native | 21, all native |

Gameplay is the other way round. Act 1, Chapter 1 with combat, one run each (332 and 204 s, not the
same route, so frames compared by draws per frame); 94% of the resolves were native in run A:

| Draws per frame | From the render targets: frame / GPU | Dump: frame / GPU |
| --- | --- | --- |
| 300–500 | 8.4 / 5.25 ms | 8.3 / 4.86 ms |
| 700–900 | 15.6 / 11.39 ms | 12.0 / 10.38 ms |
| 1100–1300 | 15.1 / 11.35 ms | 11.9 / 10.48 ms |
| 1500–1700 | 15.6 / 11.69 ms | 12.4 / 10.57 ms |

The native path records fewer barriers (242 against 276 per frame) and dispatches (96 against
131), yet frames take about 3 ms longer and the GPU about 1 ms. An inference, not measured per pass:
the shader writes one dword per invocation at a scattered tiled address, where the resolve copy
shaders write 16-byte blocks of consecutive pixels per thread; with 35 resolves per frame in combat
that costs more on NVIDIA than the dump it saves. Hence off by default until it is rewritten.

## Rational scales

1920x1080 (3/2) is drawn at 2x2 by default, as 2560x1440 is, and the presenter scales the image down
to the window: the Video menu and the launcher offer it on every platform. Drawing 3/2 as it is
(`vulkan_rational_draw_resolution_scale`, with `gfx = hle` and host render targets) follows Metal's
phase (b) of [05-rational-scale-map.md](05-rational-scale-map.md), steps 1 and 2:

- the render targets, the framebuffers, the transfer, dump and clear rectangles and the transfer
  shaders' tiles at the ratio (120x24 host samples per tile at 3/2), the transfer rectangles and the
  stencil clears rounded to host pixels (`RoundX`), the viewport and the scissor through the shared
  rational `GetHostViewportInfo`, and the shader translator given the ratio;
- a resolve (`ResolveCopyAtRationalScale`) written to the guest memory at the guest resolution,
  each guest pixel from the host pixel at its centre: by the native path when it handles the
  resolve, otherwise from an EDRAM dump at the guest resolution (`edram_buffer_guest_`) copied by
  the unscaled shaders; `hle_verify_resolves` compares the two.

Checked on Linux (RTX 3050 Laptop) at the title screen and the main menu: the twelve resolve kinds
seen there match the memory (`hle_verify_resolves`), and 1280x720, 2560x1440 and 3840x2160 are
unchanged. Not done: the resolves' textures and the front buffer at the scale (step 3), so the
picture still reaches the screen at 1280x720.

## Left

1. Rewrite the shader to write as the copy shaders do (several pixels per thread, a 16-byte block
   per store), measure the GPU time per pass against the dump, and turn it on by default if it
   wins in gameplay.
2. C1: resolved textures taken from the render target (`MetalTextureCache::UpdateTexturesFromResolve`).
3. Clears as load actions (`hle_clear_load_actions`) with `hle_verify_clears`.
4. Rational scales, steps 3 to 5 of [05-rational-scale-map.md](05-rational-scale-map.md): resolves
   to textures of their own at the scale, the per-pixel decisions, then drawing 1920x1080 at 3/2 by
   default.
