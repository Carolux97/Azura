# Rational draw resolution scales: what assumes a whole number (map, for review)

Status: implemented on the Metal backend (card 101), with 1920x1080 in the Video menu and the
launcher on macOS. It needs gfx = hle (a rational scale falls back to its whole part with pm4).
Differences from the proposals below: a resolve textures sample at the scale writes a scaled
surface of its own (laid out as a guest texture of the host size, which the unscaled load shaders
read) instead of copying into the textures; resolves the native path does not handle are dumped
at the guest resolution (each guest sample from the host pixel at its center) to an EDRAM buffer
of that size and copied by the unscaled shaders, and their textures load at the guest resolution.

Phase (a) of option 4 in [04-full-hle-design.md](04-full-hle-design.md): every place on the macOS
path (the shared render target, texture and draw code, the Metal backend, and the XSC and SPIR-V
shader translators) that uses the draw resolution scale, what it assumes, and what a scale of 3/2
(1920x1080 from the console's 1280x720) needs there. D3D12 is not used by Azura; the Vulkan backend
follows the same shared code and is left for its own port.

About 250 uses, in two kinds:

- **Per EDRAM tile** (80x16 samples, 40x16 at 64 bits per pixel): at 3/2 a tile is 120x24 host
  pixels (60x24 at 64 bits), whole numbers, and so are the depth layout's 40-sample halves (60) and
  the 8-pixel resolve alignment (12). These need rational arithmetic, not new behaviour.
- **Per guest pixel**: at 3/2 a guest pixel is 1.5 host pixels. Each of these needs a decision.

## How the scale is represented today

| Where | What | At 3/2 |
| --- | --- | --- |
| `draw_resolution_scale_x/y`, `resolution_scale` options (`src/graphics/pipeline/texture/cache.cpp`), `TextureCache::GetConfigDrawResolutionScale` | integers 1 to 7 per axis | a ratio per axis: numerator and denominator, the denominator dividing 8 (so a tile, a 64-bit tile and an 8-pixel block stay whole); 3/2 first |
| `render_resolution` (`runtime/src/menu/pc_options.cpp`) | 1280x720, 2560x1440, 3840x2160 set the scale 1, 2, 3 | add 1920x1080 as 3/2 |
| 3-bit scale fields: `ResolveCoordinateInfo` (`include/rex/graphics/util/draw.h`), the transfer constants (`render_target/cache.h`), the texture load constants (`metal_texture_cache.mm`, scale << 4 and << 7) | integer scales packed into shader constants | numerator and denominator fields, or the scaled tile size directly |
| `RenderTargetCache` and `TextureCache` accessors `draw_resolution_scale_x/y()`, `IsDrawResolutionScaled()` | integers | ratio accessors; every caller below moves to them |

## Per EDRAM tile: rational arithmetic only

| Where | Use | At 3/2 |
| --- | --- | --- |
| `MetalRenderTargetCache::CreateRenderTarget` (`metal_render_target_cache.mm`, width and height) | host size = guest size times the scale | `pitch_tiles * 80 * 3/2`, `rows * 16 * 3/2`: whole |
| Transfer, dump and host depth store shaders (`spirv_translator_rb.cpp` tile sizes; `GetDumpPipeline`; the dump dispatch size) | tile size in host samples = 80 or 16 times the scale | 120 and 24 (60 at 64 bits); the thread group counts from those |
| Transfer rectangles (`RenderTargetCache::GetTransferRectangles`, the transfer pass viewport in `metal_render_target_cache.mm`) | rectangles in guest pixels, multiples of 8, times the scale | multiples of 8 times 3/2: whole |
| Native resolves (`resolve_from_render_target_*`, `GetResolveFromRenderTargetConstants`) | read through EDRAM tiles; tile size times the scale | whole tile sizes; the read side works as is |
| Resolve clears, depth halves, sample space (2x and 4x MSAA as more host samples) | per tile or per sample | whole at 3/2 |

## Per guest pixel: decisions

| Where | Today | At 3/2 | Proposal |
| --- | --- | --- | --- |
| Viewport (`draw_util::GetHostViewportInfo`, `src/graphics/util/draw.cpp`) | "a guest pixel is always treated as a whole": offsets and extents in whole guest pixels times the scale | an odd guest coordinate is a half host pixel | round edges to the nearest host pixel; the NDC transform stays exact (floats); count odd edges per frame before deciding (likely rare: full-screen and power-of-two targets) |
| Scissor (`MetalCommandProcessor`, scissor rectangle) | guest pixels times the scale | same as the viewport | same rounding as the viewport, so a viewport and its scissor agree |
| Clear cutouts (`RenderTargetCache::GetClearCutout`, the transfer scissor) | any guest rectangle times the scale | odd edges are half pixels | shrink the cutout to whole host pixels inside it (keeping a sliver of old contents is safe; overwriting kept data is not) |
| Pixel position in shaders (`xsc_translator_ps.cpp` guest pixel size, `spirv_translator.cpp` param gen and memexport) | host position divided by an integer to get the guest position | divide by 1.5 (floats); a host pixel straddles two guest pixels | divide by the ratio; integer guest pixel indices (memexport by pixel) use the host pixel's centre |
| Point sprites and polygon offset (`metal_command_processor.mm`) | float factors from the integer scale | floats already | the ratio as a float |
| Texture fetches from resolved textures (`xsc_translator_fetch.cpp`: unnormalised coordinates, sizes and offsets reverted by the scale) | multiply or divide by the integer scale (floats) | the same with 1.5 | the ratio as a float |
| Half-pixel fill (`fill_half_pixel_offset`, `scale / 2` host pixels; `resolve_resolution_scale_fill_half_pixel_offset`) | whole host pixels | 0.75 | round to 1 host pixel; check against the 1x and 2x images |
| Data resolves reduced to guest memory (`resolve_reduce`, the centre sample `scale / 2`) | one host pixel per guest pixel taken | no host pixel is a guest pixel's centre | sample the host pixel containing the guest pixel's centre |

## The scaled resolve buffer: replaced

| Where | Today | At 3/2 |
| --- | --- | --- |
| `TextureCache` scaled resolve range (`GetScaledResolveRange`), `MetalTextureCache` scaled resolve buffer, the native resolve's scaled address (`resolve_dest_address`), the scaled resolve copy shaders | each 16 bytes of guest memory are scale_x * scale_y blocks of 16 bytes | 2.25 blocks: the layout does not exist |
| Scaled textures (`MetalTextureCache` host sizes, the load shader's scale area and offsets, `texture_resolution_scale_*`) | guest size times the integer scale, loaded from that buffer | a guest texture of odd size is a fractional host size |

**Proposal: host-texture resolves.** At a rational scale, a resolve copies the render target's
rectangle into a host texture of the texture cache for the destination's key, at the scale
(rounding the host size up), as C1 does at 1x, for every format (the 64-bit conversions with a
compute pass), and writes the guest memory at the guest resolution (the data resolve reduction,
for the CPU and other readers). The scaled resolve buffer is not used at rational scales. Integer
scales keep today's path, so 1440p and 4K are untouched.

## Order of work (phase b), each step verified

1. The ratio in the options, accessors and constants; 1x, 2x and 3x must give the same images and
   counters as before (no behaviour change for integer scales).
2. Per-tile code at 3/2: render targets, transfers, dumps, native resolve reads. Check: transfers
   and resolve reads at 3/2 against 1x on the same frames (`hle_verify_resolves` compares the
   guest-resolution memory).
3. Host-texture resolves and the guest-memory reduction at 3/2.
4. The per-pixel decisions above (viewport, scissor, cutouts, shaders, half-pixel fill), with
   counters of odd edges.
5. The 1920x1080 option in the launcher and the runtime menu.

Not verified anywhere yet: that the game's viewports and scissors are mostly at even guest
coordinates (step 4 counts them), and how the presenter scales a 1920x1080 front buffer (it
handles 2x and 3x today).
