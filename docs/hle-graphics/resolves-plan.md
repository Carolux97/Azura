# HLE graphics, phase C plan: native resolves

**Status: plan for review. No code yet.**

Resolves are the largest piece of GPU work and render pass breaks left in the HLE path. At the
campaign start (M4, uncapped, both modes at the maximum GPU state):

- 38 resolves per frame cause 37 of the 83 render pass ends.
- The "Resolve" compute encoders (the EDRAM dump plus the copy to memory) take about 2 ms of
  GPU time per frame, and the "Texture load" encoders (untiling textures from memory, including
  resolved ones) about 1.7 ms, out of about 9.2 ms.

This plan says what a native resolve does, case by case, the rules that keep it correct, the order
of the work and how each step is checked.

## How a resolve works today

1. `draw_util::GetResolveInfo` reads the copy registers (RB_COPY_CONTROL, RB_COPY_DEST_INFO,
   RB_COPY_DEST_PITCH, the source EDRAM info): the source rectangle, the destination address,
   texture format, number format, endianness, exponent bias, and the clear values.
2. `DumpRenderTargets` copies the host render targets that own the source EDRAM range into the
   EDRAM buffer (compute), ending the render pass.
3. A resolve copy shader (compute) converts and writes the rectangle from the EDRAM buffer into the
   shared memory (the GPU copy of guest memory), in the Xenos tiled layout.
4. `MarkRangeAsResolved` invalidates the textures over that range; when the game samples them, the
   texture cache loads them from the shared memory, untiling them (compute).
5. If the resolve also clears, `PerformTransfersAndResolveClears` clears the render targets.

Guest memory as the CPU sees it is not updated by resolves (`readback_resolve = none`, the
default), so the CPU never reads resolved data today; only the GPU-side shared memory holds it.

## What the game resolves

Every distinct resolve from the title screen to the first playable moment of the campaign (a
survey run with temporary logging, not committed):

| Source | Size | Destination format | Read later as a texture |
| --- | --- | --- | --- |
| color 8_8_8_8 | 1280x720 | 8_8_8_8, endianness 0 (the front buffer) | same format and size |
| color 8_8_8_8 | 1280x720, 640x360 | 8_8_8_8, 8-in-32 | same format and size |
| depth (format not recorded) | 1280x720 | 24_8_FLOAT (depth as a texture) | same format and size |
| color 16_16_FLOAT | 640x360 | 16_16_FLOAT | same format and size |
| color 2_10_10_10_FLOAT_AS_16_16_16_16 | 1280x720 | 2_10_10_10, exponent bias -5 | same format and size |
| color 2_10_10_10 | 1280x720 | 2_10_10_10 | same format and size |
| color 16_16_16_16 | 640x360 | 16_16_16_16_FLOAT (float number format) | same format and size |
| color 16_16_16_16 | 640x360, 328x184, 192x96 | 16_16_16_16, exponent bias -5 | same format; the 192x96 one is read as part of a 322x182 texture |
| color 8_8_8_8 | 16x512 | 8_8_8_8 | read as a 16x16 texture (a lookup table) |

No resolve used MSAA in these scenes. Every resolved texture is tiled, single-level and read at
offset 0 of the resolve.

## Cases

| Case | Example | Native handling |
| --- | --- | --- |
| A. Same format, same size, read as a texture | most rows | snapshot the render target region into a texture the texture cache uses directly |
| B. Format or number format changes, exponent bias | 16_16_16_16 to FLOAT, bias -5 | snapshot only if the host formats match and the conversion can be a view or a simple shader; otherwise case E |
| C. Partial resolves into a larger texture | 192x96 into 322x182, 16x512 read as 16x16 | case E (memory path) |
| D. Front buffer | the 8_8_8_8 endianness-0 resolve | case A, and the presenter can take the snapshot directly |
| E. Anything else (depth, MSAA, mips, arrays and cube faces, draw resolution scale, unknown formats) | depth as 24_8_FLOAT | today's path, unchanged |

## Plan, in steps, each behind a flag and measured

Options (`gfx = hle` only):

- `hle_native_resolves`: master switch, off by default until the step is verified.
- `hle_resolve_legacy`: a comma-separated list of resolve indices within a frame, or destination
  addresses, forced through today's path. This is the per-resolve bisection toggle.

**C1. Resolve to a snapshot texture (case A, and D for presentation).**
- At the resolve, when the case is A: copy the source rectangle of the owning render target into
  a host texture of the texture cache's host format for the destination key, with a blit or a
  small render pass.
- Register it with the texture cache for that key: base address, format, size, tiling, endianness.
- Keep writing the shared memory as today (steps 2 and 3), so anything else that reads the range
  still finds the data.
- Gain: the texture load (untiling) of resolved textures.
- Check: `hle_verify_resolves` compares the snapshot with the texture loaded from memory, texel by
  texel (the same approach as `hle_verify_clears`).

**C2. Resolve straight from the render target texture.**
- A copy shader that reads the host render target texture instead of the EDRAM buffer, for the
  1x color formats above, and writes the same tiled guest format to the shared memory.
- Drops the dump (step 2) for those resolves.
- Check: `hle_verify_resolves` compares the bytes written to the shared memory by both paths.

**C3. Skip the memory write when only the texture cache reads it.**
- After C1 and C2 are verified, the shared memory write of a case A resolve is deferred.
- The range is marked as held by a snapshot. Anything that needs the bytes makes them from the
  snapshot with the C2 shader before going on: a texture load with a different key, another
  resolve over part of it, memexport, a CPU readback, a partial texture.
- Gain: the copy (step 3) for the common case.

**C4. Fewer pass breaks around resolves (research spike first).**
- Today any compute ends the render pass.
- On Apple GPUs a tile shader dispatched inside the render pass (`dispatchThreadsPerTile`) can
  read the attachments from tile memory and write a buffer or a texture, so the snapshot or copy
  of the resolve could happen without ending the pass. That is the only way to cut the 37 pass
  ends a frame; it is Apple-specific.
- First a spike measures it on one resolve kind before committing to it.

## Correctness rules

- A snapshot is valid only for its exact texture key. Any other key over its range uses memory,
  which must then hold current data: always until C3, materialized on demand after C3.
- Invalidated, with the texture cache's existing invalidation:
  - another resolve overlapping its range;
  - a CPU write to the range (the shared memory's write watches, which already invalidate
    textures);
  - memexport writes;
  - a texture cache clear.
- Taken in submission order, in the same command buffer as the draws, so it holds the render target
  as it was at the resolve, not as later draws leave it.
- Resolve clears run after the snapshot, as today after the copy.
- Unsure means memory: any field the plan does not recognise (format, number format, swap,
  exponent bias not handled, MSAA, scale, arrays) takes today's path, logged once with the reason.

## Risks

| Risk | Mitigation |
| --- | --- |
| Number format and exponent bias conversions differ from the copy shaders | only cases verified texel by texel take the native path; the rest stay on memory |
| Endianness: the snapshot is host-order, the texture cache expects the key's swap | the key's endianness only selects the untile swap; a snapshot replaces the untiled result, verified by C1's check |
| Partial updates of a texture by several resolves | case C stays on memory; C3 materializes on any overlap |
| Memory: a snapshot per resolve, up to 1280x720x8 bytes | released with the texture cache's own eviction; measure the peak |
| Draw resolution scale | out of scope (case E); the default scale is 1 |
| Tile shaders (C4) | a separate spike with its own go or no-go |

## Measurement

The same scripted scenes with the per-frame counters, uncapped and alternated, as phases A and B:

- texture loads and their GPU time;
- dumps and copies per frame;
- "Resolve" and "Texture load" GPU time from a Metal System Trace;
- render pass ends by resolve;
- images against `pm4` with the noise floor.

Each step is committed separately, and each starts only after the previous one is verified.
