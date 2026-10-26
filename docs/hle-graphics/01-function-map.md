# HLE graphics, phase 1: the guest D3D library, function by function

Which guest functions build the PM4 command stream, what each one asks the GPU to do, and how
sure we are. Everything here comes from observing the player's own retail `default.xex` (World
and España editions share the code) at run time, plus the public PM4 opcode and register lists in
the ReXGlue SDK (`rex/graphics/xenos.h`, `register_table.inc`). No leaked or proprietary material
was used. Names below are ours, chosen to describe behaviour; they are not the library's symbols.

Confidence: **high** = packets and code both agree and the count matches the role (for example
one per frame); **medium** = the packets or the code point to it, not both; **low** = a guess
from where it sits.

## How it was measured

Two tools, both in `scripts/hle/` and `runtime/src/hle_gfx/`, with all their output kept local
(it is derived from the game; `captures/` is ignored by git and blocked by the content check):

1. **Static scan** (`scan_pm4_writers.py`): reads the generated C++, where each PowerPC
   instruction is kept as a comment, rebuilds the 32-bit constants every function materialises
   and lists the functions that build PM4 type-3 headers or address the GPU register window
   (`0x7FC8xxxx`). 55 functions, almost all in one contiguous block, **0x82AE6000-0x82B10000**:
   the statically linked D3D library.
2. **Run-time trace** (build option `AZURA_PM4_TRACE`, `pm4_trace.h`): every one of the 788
   recompiled functions in that block (all but `sub_82AF6B10`, which the frame rate overlay
   already replaces) is replaced with a pass-through that records calls, the caller (link
   register, plus one level up through the stack back chain), the PM4 bytes written by the call
   itself and by everything it calls, and a profile of those packets: registers written by type-0
   packets, type-3 opcodes, SET_CONSTANT kinds.

The trace reads the write cursor of the command buffer from the device structure (below). Runs:
three of 90-100 seconds each from start, through logos, intro movies and the title screen with
its attract movie (about 3,000 frames per run at the title's frame rate). **The main menu and the
campaign were not traced**: the game window could not be driven from the session (see Gaps).

## The device structure

Every D3D call receives the device in `r3`; the game reaches it through the global word at
**0x83416C5C** (429 load sites in the game's code). Offsets observed:

| Offset | What | Evidence |
| --- | --- | --- |
| +16 | 64-bit dirty mask: bits set by setters, consumed by the flush functions | `or`/`std` of single bits in every setter |
| +48 | command buffer write cursor (address of the last word written; packets are stored with pre-incrementing stores) | every writer; the game also stores it after inline draws |
| +56 | command buffer limit; past it the writer calls the kick function | compare + call of `82AF58B8` |
| +1152 + 24·n | texture fetch constants, 6 words each (fetch constant n = sampler n) | `82AEF370`, `82AEBF48`, `82AEC0F0` index `(n + 48) · 24` |
| ~+10370 to +10600 | shadow copies of Xenos registers (RB_SURFACE_INFO region, etc.) | setters build register values there; the flush functions write them out |
| +12152, +12156 | render-state flag words | boolean setters `82AEA700`, `82AEAA90` |
| +12704 | current render target (surface pointer) | `82AED028` |
| +12940, +12944 | current vertex and pixel shader | `82AF2750`, `82AF2AF8` |
| +13716 | end of the space an inline draw reserved | the game sets +48 from it after copying vertices |

## The key finding: state is deferred

The library follows the classic pattern of a deferred-state driver. **Of the 108 functions the
game calls directly, only 27 write any PM4 at all.** The most called ones (resource reference
counting, render state, sampler state, texture, shader and render target setters, hundreds of
thousands of calls per run) only update the device structure and set dirty bits. The packets are
written later, by a small group of internal **flush** functions that the draw calls run, and by
the few calls that act at once (resolve, clear, frame start and end, command buffer kick).

For HLE this is good news: the state the game sets is all in one structure, in Xenos register
form, and the moments it becomes GPU work are a handful of functions.

## Function map

Per-frame rates are per frame of the title screen (about 2,960 frames per run).

### Draws

| Address | Role (inferred) | Confidence | Evidence |
| --- | --- | --- | --- |
| `82AF0318` | indexed draw from vertex and index buffers | high | one DRAW_INDX per call (10,453 of 10,453), preceded by the state flush (shaders, constants, render state) |
| `82AEFF00` | indexed draw, second variant | medium | one DRAW_INDX per call (753), same flush |
| `82AEF9C0` | **inline draw** (vertices and indices copied into the command buffer by the caller) | high | reserves space and returns two pointers; the game copies vertex and index data there (`82CC9970`, memcpy) and moves the cursor itself; 212,000 calls, about 72 per frame (the UI) |
| `82AEF500` | draw setup whose draw packet the caller writes | medium | full flush (IM_LOAD, LOAD_ALU_CONSTANT, render state), no draw opcode inside |

### Render targets, resolves, clears

| Address | Role (inferred) | Confidence | Evidence |
| --- | --- | --- | --- |
| `82AED028` | set render target | medium | stores the surface at +12704, copies its fields into the register shadow, rewrites window and scissor (`82AEA088`) |
| `82AF34A8` | **resolve** (EDRAM to memory copy) | high | RB_COPY_CONTROL, RB_COPY_DEST_INFO, COHER_DEST_BASE, a DRAW_INDX_2 rectangle and EVENT_WRITE per call; about 27 per frame |
| `82AF1D50` | clear (draws a rectangle with built-in shaders) | medium | IM_LOAD_IMMEDIATE (embedded shader), DRAW_INDX_2, RB_DEPTHCONTROL, RB_SURFACE_INFO, RB_COLOR_INFO; about 12 per frame |
| `82AECC98`, `82AECA70` | set scissor or viewport window | low | write PA_SC_WINDOW_OFFSET and PA_SC_WINDOW_SCISSOR_TL/BR at once |

### State setters (no packets)

| Address | Role (inferred) | Confidence | Evidence |
| --- | --- | --- | --- |
| `82AE9358` / `82AE93D0` | resource AddRef / Release | high | atomic increment / decrement at +4, recurse to the parent resource, free on zero |
| `82AF2750` / `82AF2AF8` | set vertex shader / set pixel shader | medium | store the argument at +12940 / +12944, mark shader dirty bits |
| `82AEF370` | set texture (sampler n, texture) | medium | builds the 6-word texture fetch constant for sampler n from the texture's header |
| `82AEBF48`, `82AEC0F0` | set sampler state (two state kinds) | medium | rewrite bit fields of fetch constant n, set its dirty bit |
| `82AED4A0` | set a per-stage pointer (vertex stream or similar) | low | stores the argument in a per-index array, sets an index dirty bit |
| `82AEA700`, `82AEAA90`, `82AEB928` | render state setters (boolean and register-backed) | medium | update the flag words or a register shadow, set dirty bits |
| `82AEF290`, `82AEE118` | get a surface or texture description | medium | fill a caller structure with width, height and format from the resource header |

### Internal flush (run by the draws)

| Address | Role (inferred) | Confidence | Evidence |
| --- | --- | --- | --- |
| `82AF7CA8` | write ALU (shader) constants | high | almost only SHADER_CONSTANT_* registers, 229 bytes per call |
| `82AF7B50` | write fetch constants (textures, vertex fetch) | high | SHADER_CONSTANT_FETCH_* and the fetch flush registers |
| `82AF7910` | write render state registers | high | RB_* (blend, depth, color, surface), SQ_*, PA_* registers |
| `82AF8CF8`, `82AF8A78` | load shaders and constant blocks | high | IM_LOAD and LOAD_ALU_CONSTANT packets |
| `82AF7578` | write hierarchical Z control | high | only RB_HIZCONTROL |
| `82AF9C28` | synchronisation | medium | WAIT_UNTIL |

### Frame and command buffer

| Address | Role (inferred) | Confidence | Evidence |
| --- | --- | --- | --- |
| `82AF6B10` | present (calls VdSwap) | high | already replaced by the frame rate overlay (`runtime/src/perf_overlay.cpp`) |
| `82AF65D8` | end of frame: fence and GPU interrupt | high | WAIT_REG_MEM, CALLBACK_ADDRESS, SCRATCH_REG, INTERRUPT; exactly one per frame |
| `82AE9F60` | start of frame: tiling bins reset | medium | SET_BIN_MASK and SET_BIN_SELECT (all four), RB_BC_CONTROL; one per frame |
| `82AF58B8` | kick: hand the command buffer to the GPU | high | called when the cursor passes the limit, and by the game to flush |
| `82AF4C80` | kick worker (segment hand-off) | medium | only called from the kick; about 53 KB per call |
| `82AF2DB8` | **run a recorded command buffer** | high | three INDIRECT_BUFFER packets per call, 26,000 calls from five game sites |
| `82AFA708` | resource or fence busy query | medium | reads resource and fence state, may kick; 37 million calls, the render thread's spin-wait |
| `82AEE798`, `82AE9E08`, `82AE8E40` | memory coherency (CPU-written data made visible to the GPU) | medium | COHER_BASE_HOST, COHER_SIZE_HOST, COHER_STATUS_HOST with WAIT_REG_MEM |
| `82AF5D00` | GPU interrupt callback | high | registered by the game (`SetInterruptCallback(82AF5D00, ...)` in the log) |

## How much the game writes by itself

Bytes that land in the command buffer between two traced calls on the same thread, with no other
thread's call in between, were written outside the library. At the title screen that is
**about 15.5 MB against 487 MB written by the library, around 3%**, and it is an upper bound:
it includes the untraced present function. On top of that, the vertex and index data of inline
draws (`82AEF9C0`) is copied by the game into space the library reserved.

## Consequences for the design (phase 2)

- **Replace at the flush, not at every setter.** The setters can stay as they are: the device
  structure then holds the full state in Xenos register form when a draw, clear or resolve runs.
  The HLE layer replaces the draw, clear, resolve, frame and kick functions and reads that
  state, instead of replacing dozens of setters.
- **Render targets and resolves are explicit calls** (`82AED028`, `82AF34A8`, `82AF1D50`), so
  they can become render passes and pass ends without emulating EDRAM.
- **Inline draws** need the data the game copies after the call returns: process them lazily
  (at the next library call or the kick), or with a mid-function hook after the copy.
- **Recorded command buffers** (`82AF2DB8`) hold PM4 recorded earlier; the HLE path must either
  interpret their packets (a small PM4 reader limited to what they contain) or replace their
  recording. Their contents were not profiled yet.
- **Shaders reach the GPU as IM_LOAD of Xenos microcode**, so the existing translators (MSL,
  SPIR-V) stay the shader path for both backends.

## Gaps and risks

- **Only the start of the game was traced**: logos, movies, title screen. The main menu, the
  campaign and split screen were not, because the game window (full screen, on its own Space)
  could not be driven or captured from the session. Gameplay will add draw paths (more
  `82AF0318`-style draws, shadow and post-processing resolves). Next run: a person plays the
  menu and the first chapter with the traced build.
- **The block may extend past 0x82B10000**: functions near the end of the range ran
  (`82B0B2D0`, `82B0FF00`); the next run should trace up to 0x82B20000 to find the end.
- **Game-side writes are an upper bound** and, for inline draws, depend on the game's copy.
- **Packet profiles of inline draws and the kick worker include data words** (vertices, segment
  copies) read as if they were packets; their register columns are noise and were ignored.
- **Roles are inferences** from packets and code shape: each must be confirmed when its HLE
  replacement is written (phase 3), by comparing against the PM4 path.
