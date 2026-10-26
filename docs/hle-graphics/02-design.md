# HLE graphics, phase 2: design

How the HLE path is built, on Metal (macOS) and Vulkan (Linux, Windows), from what phases 0 and 1
found. **This design needs review before phase 3 starts**, including one decision that is the
user's: the SDK patch it proposes (ReXGlue is only changed with the user's OK).

## What the earlier phases established

- A recompiled guest function can be replaced with native code, and its original stays callable
  ([00-feasibility.md](00-feasibility.md)).
- The game's D3D library defers state: setters only fill the device structure, and a few internal
  flush functions write it to the command buffer as Xenos registers when a draw runs. Render
  target changes, resolves, clears, frame start and end, and the command buffer kick are explicit
  calls. The command stream also carries recorded command buffers (INDIRECT_BUFFER), inline draws
  whose data the game copies itself, and a few percent of packets written by the game's own code
  ([01-function-map.md](01-function-map.md)).
- The cost to remove is the EDRAM emulation: on an M4 about 24 of 46 ms of GPU time per frame are
  render target transfers, and about 420 render passes per frame cost about 18 ms of fixed overhead
  ([metal-backend-design.md](../metal-backend-design.md)). Draws themselves are about 15 ms.
  The game is GPU-bound; the CPU side of the command processor is not the bottleneck.

## Options, best first

### A. HLE render targets, guest D3D state kept (recommended)

The guest D3D library keeps running: setters, flushes, recorded command buffers, inline draws.
The command stream still reaches the command processor, which keeps tracking registers, loading
shaders and textures and issuing draws through the existing pipeline and texture caches. What is
replaced is **the render target layer**: instead of emulating EDRAM (tiles, ownership, transfers
between formats and sample counts), an HLE render target manager keeps one host texture per guest
surface and turns the frame into host render passes, using what the D3D calls tell us: which
surface is bound, when a clear is a clear, when a resolve ends a pass, where a frame starts and
ends.

- Pros: attacks exactly the measured cost (transfers and pass count); every other part of the
  pipeline is reused and already correct; recorded command buffers, inline draws and game-written
  packets need nothing new; small enough to reach title and menu, then gameplay, in a prototype.
- Cons: draws still go through register state, so CPU savings are small (not the bottleneck);
  depends on the game not relying on EDRAM contents surviving between two different surfaces,
  which must be checked (see risks).

### B. Full API-level HLE

Replace every draw, clear and resolve call and read the state from the device structure instead of
the command stream; no PM4 for draws at all.

- Pros: the most control; no register file, so CPU cost per draw could drop.
- Cons: needs the full layout of the device structure (hundreds of fields, only a few known);
  recorded command buffers and game-written packets still need a PM4 reader, so the command
  processor does not go away; inline draws need their data captured after the game copies it;
  the GPU saving is the same as A, because it comes from the render targets.

B is A plus a large reverse-engineering effort for a CPU gain we have not measured as needed. If
phase 4 shows the CPU becomes the limit once the GPU is relieved, B can be done function by
function on top of A.

### C. Keep improving the EDRAM path

Fast paths inside the existing render target cache for patterns it can prove safe. Rejected as
the plan here: card 29 explored it, and the transfers it measured are not redundant from the
EDRAM point of view; only knowledge of the guest's surfaces (what A adds) can drop them.

## Design of A

### Overview

```
guest D3D library (recompiled, unchanged)
  setters ─▶ device structure ─▶ flush ─▶ command buffer (PM4) ─▶ kick
  hooks on SetRenderTarget, Clear, Resolve, frame start/end ──┐
                                                               ▼
                                   HLE markers (PM4 NOP packets in the same buffer)
                                                               │
command processor (SDK, unchanged except the marker hook) ◀───┘
  registers, shaders, textures, primitives, draws
  IssueDraw / IssueCopy ─▶ render target layer
                              pm4: MetalRenderTargetCache / VulkanRenderTargetCache (EDRAM)
                              hle: HleRenderTargets (Metal | Vulkan), driven by the frame planner
```

The mode (`--gfx=pm4|hle`, default `pm4`) is read once at start: it picks the render target layer
and makes the hooks write markers or pass through. A frame never mixes the two.

### Markers: keeping guest calls and the command stream in order

The command processor consumes the stream later, on another thread, so a hook cannot act at the
time of the call. Instead each hook writes a **marker** into the command buffer at the current
cursor: a type-3 NOP packet (opcode 0x10, which the GPU and the PM4 path skip) carrying a magic
word, an event code and its arguments. The marker lands exactly between the packets before and
after the call, in recorded command buffers too, so ordering is free. Writing it follows the
library's own pattern (check the limit at +56, kick with `82AF58B8` if needed, store at +48).

Events, from the hooked functions in the phase 1 map:

| Event | Hooked call | Carries |
| --- | --- | --- |
| `SetRenderTarget` | `82AED028` (and the depth/stencil setter, to find) | index, guest surface address, its EDRAM base, pitch, format, sample count |
| `Clear` | `82AF1D50` | targets, colour, depth, stencil, rectangle |
| `Resolve` | `82AF34A8` | source surface, destination texture address, rectangle, clear-after flags |
| `FrameBegin` / `FrameEnd` | `82AE9F60` / `82AF65D8` | frame number |

The clear and resolve calls still write their own packets (the clear rectangle draw, the copy
registers). In `hle` mode the command processor sees the marker first and then skips the packets
the marker covers (it carries their length), so they are not drawn or copied twice.

### Frame planner (backend-agnostic, `runtime/src/hle_gfx/`)

Consumes markers and draw notifications and decides render passes:

- a **surface table**: guest surface address → host texture description (size from pitch and
  window, format, sample count), created on first bind, kept across frames, dropped when unused
  for a while;
- a pass is the run of draws with the same bound surfaces; it ends at a resolve, a surface change,
  or the end of the frame. Clears before the first draw become the pass's load action (clear
  colour and depth on tile memory, no draw); a clear in the middle splits the pass or becomes a
  clear draw;
- **resolves end the pass**: the store action keeps the surface if it is used again, the MSAA
  resolve happens in the store action when the destination is single-sampled, and the copy to the
  destination texture is a blit;
- **content carry**: when a newly bound surface shares EDRAM with another one and its first
  operation is not a clear, the planner asks the backend to copy the old surface's contents in
  (the EDRAM path's transfer, but only in that case). Counting these is how phase 4 checks the
  main risk.

### Backends (in the SDK patch, `src/graphics/metal/` and `src/graphics/vulkan/`)

`HleRenderTargets` implements what the command processors use from their render target cache
(`Update` returns the render pass for the next draw, `Resolve`, format mapping, clear cache), so
`IssueDraw` and `IssueCopy` stay as they are:

- **Metal**: one `MTLTexture` per surface, native 2x/4x MSAA (no sample space: the pipeline cache
  is told so, and translates shaders without the sample-space adjustment); render passes with
  load and store actions from the planner; colour formats from the existing mapping
  (`GetColorMetalFormat`, `GetDepthMetalFormat`), including the 7e3 float and gamma formats.
- **Vulkan**: the same with Vulkan render passes, as the existing Vulkan backend uses (its
  deferred command buffer records `vkCmdBeginRenderPass`), one image per surface, the same planner
  decisions.
- **Resolve destinations**: the texture cache loads textures from guest memory. First version:
  write the resolved image to guest memory in the Xenos tiled layout with a compute pass (what
  the EDRAM path does from EDRAM, here from a texture), so the texture cache keeps working
  unchanged. Later: register the host texture with the texture cache for that address range and
  skip the memory round trip.
- **Present**: the front buffer is the last resolve destination; the presenter keeps reading it
  as today (later: present the host texture directly).

Shaders, pipelines and textures are the existing ones: the pipeline cache (`MetalPipelineCache`,
the Vulkan pipeline cache) with the Xenos shader compiler and SPIR-V translator, the texture cache
with its tiling and endianness conversion, the primitive processor and shared memory.

### Code layout

| Where | What |
| --- | --- |
| `runtime/src/hle_gfx/hooks.cpp` | replacements of the D3D calls above: in `pm4` mode they call the original; in `hle` mode they write the marker and call the original |
| `runtime/src/hle_gfx/markers.h` | marker format, shared with the SDK side |
| `runtime/src/hle_gfx/planner.{h,cpp}` | surface table and pass planning, no GPU API |
| `runtime/src/hle_gfx/unimplemented.cpp` | logs once per address and role any D3D path the prototype does not handle yet, never crashes |
| `patches/rexglue-0.10.0-hle-gfx.patch` | `gfx` option; marker hook in the command processor's NOP handling; `HleRenderTargets` for Metal and Vulkan; the planner interface they call |

### SDK patch to approve

The render target layer and the marker hook live inside the SDK's graphics backends, so phase 3
needs a new patch, `patches/rexglue-0.10.0-hle-gfx.patch`, applied like the others:

1. a `gfx` option (`pm4` default, `hle`) read when the command processor is created;
2. in `ExecutePacketType3_NOP`, a callback for NOP packets that start with the marker magic
   (in `pm4` mode markers are never written, and unknown NOPs are skipped as today);
3. `HleRenderTargets` for Metal and Vulkan behind the interface their command processors already
   use, chosen by the option;
4. the pipeline caches accept "no sample space" from the render target layer (they already ask
   it through `IsSampleSpaceRequired` and `GetSampleSpaceXLog2/YLog2`).

`pm4` mode does not change: no markers, the existing render target caches, the same pipelines.

## Phase 3 plan

1. Hooks and markers in `pm4` mode only: markers written and logged by the command processor,
   nothing else changes. Checks ordering and the marker hook on Metal and Vulkan builds.
2. Metal `HleRenderTargets` with the planner: binds, clears as load actions, resolves through the
   tiled write, present. **Milestone A**: title screen and main menu in `hle` mode.
3. Content carry and the remaining surface cases; **milestone B**: the first campaign chapter,
   or its blockers documented.
4. Vulkan `HleRenderTargets` (built and checked by the runtime kit workflow on Linux and
   Windows, run by hand on the branch).

## Risks

| Risk | Effect | Mitigation |
| --- | --- | --- |
| The game expects EDRAM contents to survive between two surfaces (depth reused at another sample count, restore passes) | wrong pixels | content carry when the first operation is not a clear; phase 4 frame diff shows any miss |
| Predicated tiling (SET_BIN_MASK/SELECT, only a reset per frame seen so far) | draws repeated per tile | full-size surfaces make tiles redundant; check in gameplay |
| Unknown D3D paths in gameplay (the trace covered the title only) | unhandled calls | log-once fallback; gameplay trace before milestone B |
| Pipelines differ from `pm4` mode (no sample space) | more shader compiles the first time | separate shader storage per mode; counted in phase 4 |
| Resolves read back by the CPU | stale data | the tiled write keeps guest memory current |
| Markers inside recorded command buffers replayed many times | events repeated | events are idempotent (bind, clear, resolve describe state, not deltas) |

## Phase 4 measurements

`pm4` against `hle` on the same runs: frame diff with a tolerance (captures local, ignored by git),
CPU and GPU frame time, render passes per frame, pipeline compiles, frames over 50 ms, and the
number of content carries.
