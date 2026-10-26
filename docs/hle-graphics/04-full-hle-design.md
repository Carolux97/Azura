# HLE graphics: render targets without emulated EDRAM (design, for review)

Status: proposal, revised after the phase 1 audit (below). No code until it is approved. Builds on option A of
[02-design.md](02-design.md) and on what the native resolves found ([03-results.md](03-results.md)).

## Where we are

`gfx = hle` today keeps the EDRAM model and makes it cheaper:

- The render target cache still keys host render targets by **EDRAM ranges**: a base tile, a pitch
  and a format, in sample space, with an ownership map of which render target last wrote each tile.
  Binding a surface over tiles another one owns makes a **transfer** (14 per frame at the title and
  menu, 28 at the campaign start), unless the markers say a clear overwrites them.
- Clears become load actions or cut transfers short. Every resolve of the measured scenes is
  written natively from the render target (no EDRAM dump), reading each pixel through the EDRAM
  tile that holds it, so even resolves of EDRAM another format wrote are exact.
- The draw resolution scale is an integer (1, 2 or 3): each EDRAM sample is a whole number of host
  pixels, and the scaled resolve buffer the texture cache reads has a layout built on that.

What is left of the EDRAM emulation is the ownership map, the transfers it triggers, the sample
space render targets and the integer-scale layout. Removing it is what makes non-integer
resolutions such as 1080p (1.5 times the console's 720p) possible.

## What "without EDRAM" means here

Each **guest surface** gets its own host texture, keyed by the surface the game binds (the
SetRenderTarget and SetDepthStencilSurface hooks already see the surface object, whose RB_*_INFO
word at +28 gives its EDRAM base, pitch, format and sample count), at any host resolution. Binding
a surface never copies anything by default. EDRAM addresses become metadata used only to answer
one question: **when the game reads memory another surface wrote** (a resolve, or a surface
bound without a clear over data it expects to find), which surface's pixels does it mean.

The game does rely on that in places. Measured so far:

| Case | Where seen | Handling without EDRAM |
| --- | --- | --- |
| A resolve as another format of what was drawn (2_10_10_10 of a 2_10_10_10_FLOAT surface; 8_8_8_8 of a D24S8 surface) | title, menu, campaign start | already native: the resolve packs the writer's pixels as the EDRAM would hold them |
| A resolve whose source several surfaces wrote (640x360 at base 1624) | title, menu | already native: per-tile owners |
| A surface bound over another one's data without a clear first (what the 14 to 28 transfers per frame are) | every scene | unknown how many are needed: the audit (phase 1) finds out |

## Options, best first

### 1. Surface render targets, carries only where the data is read (recommended)

Host render targets per surface, at any scale. A logical ownership map (which surface last wrote
which EDRAM tiles, no pixels) replaces the physical one. When a surface is bound over tiles another
surface wrote and its first operation is not a clear, its contents are **carried** from that
surface with a conversion shader (the existing transfer shaders already convert between formats;
here they also resample between scales). Resolves read the writer through the same map, as the
native resolves do today.

- Pros: correct by construction where the game relies on EDRAM (the carries are the transfers,
  only without the integer layout); any resolution; reuses the native resolve kernels, the
  pipeline and texture caches and the markers.
- Cons: the carries stay where the game really needs them (the audit says how many); a carry
  between different scales resamples instead of copying exact samples, which can change pixels
  where the game reinterprets data (depth read as colour, for instance), so those cases need the
  surfaces' own scale.

### 2. Surface render targets, no carries at all

As 1, but a surface bound without a clear starts empty.

- Pros: the simplest and the fastest.
- Cons: wrong pixels wherever the game expects the data (every needed transfer of the audit becomes
  a bug). Only acceptable if the audit shows no transfer is needed, which is unlikely.

### 3. Keep EDRAM and add fractional scales

Resample the EDRAM model at 1.5.

- Cons: the model's exactness comes from whole samples per tile; at 1.5 every transfer and resolve
  would resample, and the scaled resolve buffer's layout does not exist. Rejected.

API-level HLE (option B of 02-design.md) is still not needed: the CPU is not the limit (phase D).

## What 1080p needs besides the render targets

| Piece | Today (integer scale) | At 1.5 |
| --- | --- | --- |
| Render target size | guest size times the scale | guest size times 1.5, rounded per surface |
| Viewport, scissor, pixel position in shaders, half-pixel offsets | scaled by an integer in the draw setup and the shader translators | the same with a float factor; half-pixel and `fill_half_pixel_offset` rules re-derived |
| Resolved textures the game samples | the scaled resolve buffer (integer layout) | **host-texture resolves**: the texture cache takes the resolved host texture for that address (C1 generalised to every format and scale); memory keeps a guest-resolution copy for the CPU and other readers |
| MSAA | sample space (each sample a host pixel) | native MSAA textures per surface, resolved by the host |
| Presentation | front buffer at the scale | the same, at 1920x1080 |

## Phases, each verified before the next

1. **Audit, no behaviour change.** Count, per transfer the current cache makes, whether the
   destination surface reads those tiles before overwriting them (a later draw with blending or
   depth test, or a resolve): needed carries against wasted copies, per scene. Log the surface
   objects per bind to check that the surface identity is stable across frames. One short
   measurement per scene; this decides between options 1 and 2 and sizes the carries.
2. **Surface render targets at integer scales** (option 1), behind an option, with the native
   resolves reading through the logical map. Verified against `pm4` with `hle_verify_resolves` and
   image comparisons at 720p, then 1440p and 4K; counters: transfers replaced by carries.
3. **Host-texture resolves**: the texture cache samples the resolved host texture at its own scale
   for the formats the game samples, memory still written (C1 for every format and scale; the
   64-bit conversions need texture-side formats). Verified with a per-texel version of the C1 check.
4. **Fractional scales**: 1080p (1.5) as the first one; float scale factors in the draw setup and
   the shader translators, native MSAA, presentation. Verified visually against 720p and 1440p,
   and with the resolve checks adapted to compare at guest resolution.
5. **Vulkan handoff document** for Juan, from what phases 1 to 4 built.

## Risks

| Risk | Effect | Mitigation |
| --- | --- | --- |
| Surface objects reused for different render targets, or recreated each frame | carries or cache misses every frame | phase 1 logs surface identity; fall back to keying by EDRAM base, pitch and format (today's key) |
| Carries between scales change reinterpreted data | wrong pixels in effects | keep reinterpreted pairs at one scale; phase 1 lists them |
| Shaders using pixel positions in ways the scale does not cover | misplaced effects at 1.5 | the shader translators already scale by an integer; audit their uses of the position before phase 4 |
| Heavy gameplay paths not seen in the scripted scenes | unhandled cases | log-once fallbacks, as today; a played run before each milestone |
| Size of the change | weeks, not days (an estimate, not measured) | phases 1 and 2 are useful alone (fewer transfers); 1080p comes at phase 4 |

## Phase 1 results: the transfer audit

`hle_audit_transfers` follows each transfer the cache makes until its destination gets the next
one, and counts whether its data was read (a draw blending, testing depth or stencil or keeping
channels, a resolve, or a transfer from it), only written over, or untouched; it also counts the
transfers between different formats and logs each pair of render targets once. One run at 1280x720
through the campaign start (`PAD_SCRIPT` recorded by hand):

| Per frame | Transfers | Data read | Only written over | Untouched | Between different formats |
| --- | --- | --- | --- | --- | --- |
| Title screen, main menu | 14 | 14 | 0 | 0 | 11 |
| Campaign start | 28 | 26 | 2 | 0 | 24 |

"Read" is generous (a later blending or depth-tested draw on the target counts even if it does not
touch the copied tiles), so the transfers really needed may be fewer, but not few. The pairs logged
in that run (34) show how the game uses EDRAM:

- **depth and colour share EDRAM both ways**: D24S8 to 8_8_8_8, 16_16_FLOAT, 16_16_16_16,
  2_10_10_10_FLOAT and back, and D24S8 to D24FS8;
- **64-bit and 32-bit targets share it**: 16_16_16_16 to and from 8_8_8_8 and 16_16_FLOAT;
- **one EDRAM area is reused at several pitches**: base 1624 as pitch 1, 8, 10 and 16 tiles (the
  post-processing chain), and base 1440 as pitch 7, 8 and 14 (depth).

**What this means for the design.** Option 2 (no carries) is ruled out. And option 1's carries
would have to reproduce, tile by tile, exactly what the EDRAM does when the game reinterprets it,
pitch changes included: that is the EDRAM model again, keyed differently. Without EDRAM emulation
this game renders wrong; with it, the surface model brings no gain at integer scales.

**What is left of the motivation: other resolutions.** At a scale of 1.5 (1080p), an EDRAM tile of
80x16 samples is **120x24 host pixels, a whole number**, and so are the 40-sample halves the depth
layout swaps (60 pixels) and the 8-pixel alignment of resolves (12 pixels). The EDRAM model works
on tiles, so a rational scale (3/2) may fit it, unlike a per-pixel one. Not verified; what breaks
at 3/2 is what assumes a whole number of host pixels per guest pixel:

- the scaled resolve buffer's layout (4 guest pixels as scale_x * scale_y blocks of 16 bytes) and
  the texture cache's scaled textures that read it;
- the per-pixel scale factors in the draw setup and the shader translators (integers today);
- the half-pixel offset fill (scale / 2 host pixels).

## Revised recommendation (option 4): rational scales in the EDRAM model

Keep the EDRAM model (with everything the HLE path added) and let the draw resolution scale be a
ratio, 3/2 first, with tile-granular mappings: render targets, transfers and resolves at the tile
level, and resolved textures handed to the texture cache as host textures at their own scale
(C1 generalised) instead of the integer-only scaled resolve buffer. Phases: (a) map every place
that assumes an integer scale; (b) make the draw setup, shaders and render targets work at 3/2
with resolves through host textures; (c) verify against 720p and 1440p. Same verification rules as
before.

## What to decide

1. Option 1 was approved before the audit; the audit shows it would rebuild the EDRAM model. The
   proposal now is option 4 (rational scales in the EDRAM model) for 1080p.
2. Whether to start option 4's phase (a), the map of everything that assumes an integer scale.
