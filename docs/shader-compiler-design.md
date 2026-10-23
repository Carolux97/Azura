# Xenos shader compiler (XSC): design

Card 35. Our own compiler from Xenos shader microcode to the host GPUs: one intermediate
representation with the Xenos semantics and the optimizations, and one emitter per backend
(MSL for Metal on macOS, SPIR-V for Vulkan on Linux and on macOS through MoltenVK). The compiler
and each backend are designed together. This document is written before the code and updated
as each phase is measured.

## 1. Why

The game's shaders are Xenos microcode: the Xbox 360 GPU's own shader machine code, produced by
Microsoft's HLSL compiler. The SDK analyses the microcode and translates it with the Xenia SPIR-V
translator (`spirv_translator*.cpp`, ~11,000 lines); on macOS SPIRV-Cross then turns that SPIR-V
into MSL.

Measured on a base M4 in the room with light shafts (card 29, docs/metal-backend-design.md):

| Measurement | Value |
|---|---|
| Game GPU time per frame | ~17-20 ms (30 FPS cap; 60 FPS needs 16.7 ms for everything) |
| Main scene pass (2_10_10_10_FLOAT color + D24FS8 depth) | ~5 ms |
| Its counters: Fragment Shader Launch / Instruction Throughput / Control Flow / ALU limiters | 81 / 44 / 29 / 26% |
| Its memory bandwidth (read / write) | 10 / 8 GB/s |
| Share of shader time in the game's pixel shaders | ~60% (top 10 shaders ~33%, ~25 for half) |

What the translated shaders look like (MSL dumped with `metal_shader_dump_path`):

- Every one of the 479 game shaders dumped is full of SPIRV-Cross `[[clang::optnone]]` helpers
  (`spvFMul`, `spvFAdd`, `spvFSub`), one per float add and multiply: the translator marks them
  `NoContraction` so that multipass rendering computes the same values in different shaders, and
  SPIRV-Cross can only keep that in MSL by forbidding optimization of each operation. The most
  expensive shader calls them 41 times, a large one ~690 times.
- State known per draw is decided at run time in every pixel: texture signedness (both an
  unsigned and a signed view are bound and sampled under a branch, then a switch picks the
  component conversion), output gamma, alpha test, color exponent bias.
- The Xenos control flow is wrapped in a generic `for (;;)` loop, with a dispatch switch when
  there are jumps or loops.

None of that is needed by the game: it comes from going through SPIR-V and SPIRV-Cross with a
translator written for every Vulkan driver at once. A compiler of our own can emit what each API
compiles best.

## 2. Where it plugs in

```
ucode ──► Shader::AnalyzeUcode (common, unchanged)          parsed instructions
      ──► XscShaderTranslator : ShaderTranslator             builds the IR from the Process* calls
      ──► IR passes                                          structurization, simplification
      ──► emitter: MSL text (Metal) or SPIR-V words (Vulkan)
      ──► Translation binary, then the backend's pipeline cache as today
```

The SDK already separates the analysis of the microcode (`translator.cpp`, `shader.cpp`: control
flow, ALU and fetch instructions parsed into `Parsed*Instruction`, register and constant usage)
from the emission (`ShaderTranslator` has one virtual `Process*` method per instruction kind,
implemented by the DXBC and SPIR-V translators). XSC is a third implementation, so the analysis,
the shader storage per ucode hash and the per-draw modifications are reused as they are.

Code: `src/graphics/pipeline/shader/xsc/` in the SDK, namespace `rex::graphics::xsc`, as our own
patch (`patches/rexglue-0.10.0-xenos-shader-compiler.patch`).

- `xsc_ir.h/.cpp`: the IR.
- `xsc_translator.h/.cpp`: `XscShaderTranslator`, which records the parsed instructions as IR and
  calls the passes and the emitter in `CompleteTranslation`.
- `xsc_passes.cpp`: IR passes.
- `xsc_msl.cpp`: MSL emitter.
- `xsc_spirv.cpp`: SPIR-V emitter (on the SDK's `SpirvBuilder`).

## 3. Xenos semantics to keep

The reference is the current SPIR-V translator: every rule below is in its code, and the
offline bench (section 8) compares the two translators on the game's own shaders.

- **ALU pairs.** An ALU instruction is a vector operation (4 components) and a scalar operation
  issued together; both read their operands before either result is written. The scalar result
  is also kept as the "previous scalar" (`ps`) operand of later instructions.
- **Multiplication.** Shader Model 3 rules: `±0 × anything = +0`, also for infinity and NaN, in
  `mul`, `mad`, the dot products and their scalar forms. Translated as a select on
  `min(|a|, |b|) == 0`, except when both operands are the same register components (`x × x`), or
  when one operand is a literal that is neither zero nor infinite (a pass decides it).
- **Other special values.** `rcp`, `rsq`, `log` and their clamped forms (`rcpc`, `rcpf`, `rsqc`,
  `rsqf`, `logc`) have defined results for zero and infinity; `max`/`min` and the comparisons
  (`sge`, `sgt`, `seq`, `sne`, `cnd*`) have defined NaN behaviour; `frc`, `trunc`, `floor`.
- **Predication and constants.** `p0` set by `setp*`, instructions and exec blocks predicated on
  it; boolean and loop constants from the bool/loop constant buffer; `a0` (`mova`, `movas`,
  rounding) and the loop register `aL` index float constants and registers.
- **Control flow.** `exec` blocks (unconditional, on a boolean constant, predicated), `loop_start`
  and `loop_end` with the loop constant's count, start and step, `jmp`, `call` and `return`
  (conditional or predicated), `alloc` for exports.
- **Fetch.** Vertex fetch: formats, endianness swaps, normalization, signed and integer forms,
  the index rounding and the mini fetches. Texture fetch: dimensions (1D, 2D, 3D, stacked, cube
  with the Xenos cube coordinates), offsets, LOD (computed, register, bias, the exponent
  adjustment of the fetch constant), gradients, filters from the fetch constant or the
  instruction, signedness per component (unsigned, signed, biased, gamma), resolution scaling,
  and the getters (`getWeights`, `getGradients`, `getCubeMapFaceIndex`...).
- **Exports.** Color with the exponent bias, the gamma conversion of render targets with gamma,
  alpha test and alpha to coverage; depth (and the float24 conversion modes; without them,
  oDepth halved for float24 targets as the viewport does for rasterized depth); interpolators,
  position with the NDC scale and offset, point size, clip planes and vertex kill; memexport.
- **Pixel position.** `param_gen` inputs (position, point coordinates, front facing) and the
  sample-space position of the Metal backend (host pixel divided by the samples per guest pixel).

## 4. The IR

A structured register IR, close to the microcode but explicit:

- **Values** are the Xenos storage: 64 temporary vec4 registers (`r0`-`r63`), `a0`, `aL`, `p0`,
  the previous scalar, the exports. Temporaries become local variables of the host shader; the
  host compilers (Metal's LLVM, the Vulkan drivers' NIR/ACO or NVIDIA's) do SSA, common
  subexpressions and register allocation better than we would, as long as nothing stops them.
- **Statements** are assignments `dest.mask = expression`, where expressions are trees of
  operations with explicit Xenos semantics (`mul_ieee`, `mul_sm3`, `rcp_clamped`...), operand
  reads (register, float constant absolute or relative to `a0`/`aL`, literal 0 and 1 from swizzles),
  negation and absolute value, swizzles. The vector and scalar halves of an ALU instruction read
  into temporaries first when they depend on each other's results, as the hardware does.
- **Control flow** is a tree: `if` on a boolean constant or on `p0`, loops with the loop
  constant's count, and `kill`. The microcode's control flow is structured by a pass: exec
  blocks, predication and loops map to `if` and loops directly; jumps and calls that form
  reducible regions become `if`s and inlined subroutines; anything left falls back to a dispatch
  loop over labels (what the SPIR-V translator always does today).
- **Texture and vertex fetches** are statements with their full set of parameters, so each
  emitter writes the best sampling call for its API.

## 5. Passes

In the order they run; each one is kept only if the bench and a trace show it helps.

1. Control flow structurization (section 4).
2. Write-mask and dead code removal: components never read later are not computed.
3. Literal folding: swizzled 0 and 1, `mul` by a literal 1 or 0, negations.
4. Multiplication rule elision where an operand is a non-zero finite literal.
5. Specialization (phase 4): per-draw state that is today read from the system constants in every
   pixel becomes a function constant (Metal) or a specialization constant (Vulkan): texture
   signedness per fetch, output gamma per render target, alpha test function, color exponent bias
   when it is 1. The pipeline cache keys then include those values; the number of pipelines this
   creates is measured before it is kept.

## 6. Emitters

Both emitters start with the exact interface of the current translator, so the backends and their
pipeline caches take the new shaders without changes and the two translators can be compared draw
by draw; the interface is changed afterwards, together with the backend, in phase 4.

- **MSL (Metal).** Buffers 0 (shared memory) and 1-5 (system, vertex float, pixel float, bool and
  loop, fetch constants) as `metal_pipeline_cache.mm` binds them; textures and samplers in the
  order the shader's texture and sampler bindings list them; the same interpolator locations,
  outputs and entry point names. Plain float operations (no `optnone`): with `MTLMathModeSafe` the
  compiler may not reassociate them or change the signed zero and infinity behaviour, and it does
  not contract a multiply and an add written as separate expressions into one fused operation
  (to verify with the bench: the comparison has to be bit-exact against the current translator
  for shaders without texture filtering differences).
- **SPIR-V (Vulkan).** The descriptor sets, bindings, push constants and interface variables of
  `SpirvShaderTranslator`, written with the SDK's `SpirvBuilder`. `NoContraction` is kept where
  the multipass rule needs it (vertex positions and everything that feeds them) and dropped
  elsewhere: Vulkan drivers take it as a hint without the cost it has through SPIRV-Cross, and
  the pass that finds the position's dependencies is shared with the MSL emitter.

## 7. Selection and fallback

- `shader_compiler = "translator" | "xsc"` (default `xsc` since phase 5), read by the
  Metal and Vulkan pipeline caches.
- The choice is made per shader (ucode hash) at its first translation and kept for all its
  variants, because the texture and sampler bindings are stored per shader.
- XSC reports what it does not support yet (an instruction, a fetch mode, a control flow shape)
  as a non-fatal translation failure; that shader then uses the current translator. A log line
  lists those shaders once, so coverage can be followed.
- D3D12 keeps the DXBC translator.

## 8. Validation without playing: the offline bench

`xsc_bench` (macOS command-line tool built with the SDK, `src/graphics/pipeline/shader/xsc/
bench/`), fed with the game's real shaders:

- **Input.** The ucode the game uses, dumped once with `dump_shaders` (`shader_<hash>.ucode.bin.
  frag/vert`), and the variants actually used (shader hash and modification), taken from the names
  of the MSL dumped with `metal_shader_dump_path` (`ps_<hash>_<modification>_<flags>.metal`).
- **translate.** Every variant through both translators: SPIR-V translator plus SPIRV-Cross, and
  XSC to MSL and to SPIR-V (the latter through SPIRV-Cross too, plus the SPIR-V validator).
  Reports translation and Metal compilation failures, unsupported features and compile times.
- **compare.** Every pixel shader variant drawn into a small float render target with both
  translations and the same inputs: interpolators, float, bool, loop and fetch constants and
  textures generated from a fixed seed (with zeros, infinities and NaNs among the values on
  purpose), several seeds per variant so that branches on constants take both ways. The outputs
  must match bit for bit, or within a stated number of ULPs where a texture filter or a
  transcendental function is involved; every mismatch is reported with the variant and the
  pixel. Vertex shaders the same way in phase 3, drawing points with their outputs written to
  float render targets.
- **perf.** The same draw repeated many times per variant with GPU timestamps, as a first
  comparison of the cost of each translation before a trace in the game.

In the game: side by side window captures of the same scenes with each translator (menus, Bink
intro, campaign, room with light shafts) and Metal System Traces.

## 9. Measurement

- **macOS.** perf_log_csv, Metal System Trace with the "Azura GPU Limiters" template (GPU counters
  and shader profiler) in the room with light shafts, as in card 29. Baseline: section 1.
- **Linux.** A new counter, `host_gpu_time_us`, with the GPU execution time of the submissions
  that completed during the frame, in both backends (Metal: the command buffers' `GPUStartTime` and
  `GPUEndTime`; Vulkan: timestamp queries at the start and end of each submission). With the
  30 FPS cap the frame time hides the GPU cost; this counter does not, and it needs no vendor
  tool. `scripts/linux_gpu_baseline.sh` and docs/linux-gpu-measurement.md tell Juan Llorca
  Martínez how to record the same route.

## 10. Phases

0. This document, the bench, `host_gpu_time_us` and the baselines on macOS and Linux.
1. IR and MSL emitter for pixel shaders, behind `shader_compiler`, falling back to the current
   translator for what is not supported yet; the most expensive shaders first; bench and
   captures.
2. SPIR-V emitter for pixel shaders, checked with the bench and the Vulkan backend on macOS,
   measured on Linux.
3. Vertex shaders on both emitters, with positions computed exactly as the depth prepass and the
   main pass need.
4. Passes and backend co-design (specialization, constants laid out per shader, render target
   formats), each measured on macOS and Linux.
5. On by default on macOS and Linux when every shader of the game compiles with no visual
   difference; the current translator stays behind the setting.

## 11. Phase 0 results

- **Corpus.** One play session on 21 October 2026, from the menus to well past the room with light
  shafts: 956 pixel and 519 vertex shaders, 958 and 521 variants (with modifications) used. Kept
  outside the repository (game data) in `~/Library/Application Support/com.gears3azura.launcher/
  xsc_corpus/`.
- **Bench.** `cmake --build --preset mac-arm64-sdk-source-relwithdebinfo --target xsc-bench` (from
  `runtime/`), then `xsc-bench <corpus> --mode translate|compare|perf`. With the current
  translator alone:
  - all 1,479 variants translate and compile, with no failure;
  - translation (SPIR-V and SPIRV-Cross) takes ~2.6 ms per variant, and Metal's compilation of
    that MSL ~37 ms per variant (55 s for the corpus on the bench's first run; later runs hit
    Metal's shader cache and take ~0.1 s);
  - the comparison of the translator with itself, 4 kept seeds per pixel shader variant: 3,826
    comparisons, 0 differences, so the draws are deterministic; only 13 draws per translator
    write no color value (kill instructions discard every pixel for every seed tried), against
    ~2,850 that write at least half of the values.
- **macOS baseline** (perf_log_csv with `host_gpu_time_us`, same session, 9.1 minutes of
  gameplay): GPU time per frame median 18.9 ms, p90 23.1 ms, p99 30.2 ms; the heaviest minutes
  22-24 ms median. Frame time median 33.3 ms (the cap) and p99 104.6 ms: those hitches are not GPU
  time; Metal compiling shaders while playing is one candidate (37 ms per variant above), to be
  checked.
- **Linux baseline.** Pending: docs/linux-gpu-measurement.md and `scripts/gpu_baseline.sh`, for
  Juan Llorca Martínez.
- **Patches.** `scripts/regen_sdk_patches.sh` regenerates the Metal backend patch and the shader
  compiler patch from the working SDK tree, from a base rebuilt from the SDK commit.

## 12. Phase 1 results: pixel shaders to MSL

- **Code.** `xsc_ir.h` and `xsc_builder.h` (the IR and its builder), `xsc_translator*.cpp` (the
  lowering of the Xenos microcode: control flow, ALU, texture fetch, pixel prologue and
  epilogue, ported from the SPIR-V translator rule by rule), `xsc_msl.cpp` (the MSL emitter).
  The Metal pipeline cache uses it for pixel shaders with `shader_compiler = "xsc"`; vertex
  shaders, memory export and float24 depth conversion go to the SPIR-V translator.
- **Bit-exact with the SPIR-V translator.** The bench compares every pixel shader variant of the
  corpus (958) drawn with both, 4 seeds each: 3,825 comparisons, no difference. Two things the
  bench had to account for:
  - SPIRV-Cross writes a multiplication as `fma(a, b, 0)`, which turns a -0 product into +0,
    while the Xenos rules (and XSC) keep -0: +0 and -0 compare equal.
  - Derivatives inside control flow that differs between the pixels of a 2x2 quad (a predicated
    texture fetch with computed LOD, where the predicate differs per pixel) are undefined, on
    Metal as on any GPU, and each compilation resolves them its own way: the compare mode
    replaces `dfdx` and `dfdy` by a fixed function in both translations (`--derivatives fixed`,
    the default); `--derivatives hardware` keeps the real ones.
- **Contraction, measured.** With `MTLMathModeSafe`, the Metal compiler fuses `a * b + c` written
  in one expression but not across statements, and `fma(a, b, 0)` stays exact. The MSL emitter
  writes every operation as its own statement, so no `[[clang::optnone]]` is needed.
- **Kill.** A Xenos kill keeps the pixel running for its neighbours' derivatives. Phase 1 marked
  the pixel and called `discard_fragment()` at the end of the shader; phase 4 discards at once,
  since `discard_fragment()` has demote semantics from MSL 2.3 (section 15).
- **Inherited rules to check later in the game** (kept as they are for parity, they look like
  errors of the SPIR-V translator):
  - linear filtering between the layers of a stacked texture computes
    `layer1 + (layer1 - layer0) * f` instead of `layer0 + (layer1 - layer0) * f` (the DXBC
    translator interpolates from layer 0); only one shader of the corpus can take that path, the
    color grading lookup at the end of the post-processing, and only if its texture is a stacked
    one rather than 3D;
  - with register gradients on 2D textures, the horizontal and the vertical ones are swapped.
- **Compile time.** Translation of the corpus 0.46 s against 4.2 s (SPIR-V translator and
  SPIRV-Cross), Metal compilation of the generated MSL 44 s against 64 s.
- **In the game** (base M4, the campaign start up to the room with light shafts, 22 October
  2026): no visual difference noticed (the white flashes in some effects are older and also
  happen with Vulkan), no shader fell back to the SPIR-V translator. GPU time per frame from
  `host_gpu_time_us`, 3.1 minutes of gameplay: median 12.5 ms, p90 17.1 ms, p99 21.7 ms, against
  18.9, 23.1 and 30.2 ms for the baseline (9.1 minutes, a longer route: the first minutes,
  comparable, went from 17-20 ms to 12-15 ms). Metal System Trace with the shader profiler in the
  room with light shafts: the main scene pass's fragment time 5.1 to 1.7 ms per frame, the most
  expensive pixel shader 1.73 to 0.14 ms, large shaders 2 to 4 times cheaper, small ones the
  same.

## 13. Phase 2 results: pixel shaders to SPIR-V

- **Code.** `xsc_spirv.cpp`, the SPIR-V emitter: one instruction per IR operation, `NoContraction`
  on the float arithmetic (an option, for phase 4), the IR's if, loop and switch as SPIR-V
  structured control flow (OpPhi in the merge blocks, a continue block per loop, fallthrough as a
  branch to the next case), the interface of the SPIR-V translator (descriptor sets, bindings and
  locations; the constant buffers declared as arrays of 4-component vectors with the same layout).
  `XscShaderTranslator` takes the target (MSL or SPIR-V) and the same host configuration as the
  SPIR-V translator of the backend: the device features (demote, sample rate shading, float
  controls, view swizzles), native 2x MSAA and the draw resolution scale.
- **Vulkan backend.** `VulkanPipelineCache` translates pixel shaders with XSC first when
  `shader_compiler = "xsc"`, on the host render target path only (`fsi` keeps the SPIR-V
  translator, which does the render target logic in the shader); what XSC does not support goes to
  the SPIR-V translator as on Metal.
- **Bench.** A third translator, `xsc-spirv`: XSC's SPIR-V converted by SPIRV-Cross like the SPIR-V
  translator's. All 958 pixel shader variants translate and compile, the 958 modules pass
  `spirv-val --target-env vulkan1.2` (built from the SDK's SPIRV-Tools), and the comparison gives
  3,825 comparisons with no difference, at scale 1 and with `XSC_BENCH_RESOLUTION_SCALE=3`.
- **Fixed on the way, for both targets:**
  - The draw resolution scale. XSC ignored it in the pixel position (`ps_param_gen`) and in the
    texture offsets and weights of textures resolved at the scaled resolution, which the SPIR-V
    translator takes into account. With the scale simulated in the bench, XSC before the fix
    differed in 2,472 of 3,825 comparisons (mostly one step of 8 bits); after it, in none. It only
    matters on Vulkan: the Metal backend did not support resolution scaling then and ran at 1x
    whatever `draw_resolution_scale` said, so phase 1 on Metal was not affected (it does now, see
    section 13 of metal-backend-design.md).
  - 7e3 render targets. The Xbox 360 converts the color written to a `k_2_10_10_10_FLOAT` render
    target to 7e3 (RGB in [0, 31.875], NaN as 0, alpha fixed-point); the host render target is
    float16 and keeps negative and larger values. XSC clamps the color when the render target is
    7e3 (the backends now fill `edram_rt_format_flags` on the host render target path too). The
    bench checks it with `XSC_BENCH_RT_7E3` (the reference clamped the same way: no difference).
    It does not change the white flashes below.
- **White flashes (fixed later, section 17).** Measured with
  `xsc_debug_color_range` (markers and per-shader counters of out-of-range colors): 221 pixel
  shaders write RGB above 31.875 to 7e3 render targets, some in every pixel (lights, sky), and the
  markers appear where the flashes are; but clamping the color as the Xbox 360 does leaves the
  flashes, so the out-of-range values mark the bright surfaces rather than cause the flashes.
  Ruled out by play tests: the draw resolution scale (they also happen at 720p) and the occlusion
  queries (`query_occlusion_fake_sample_count = 1` leaves them; Linux uses fake results too). The
  cause was the texture exponent bias (section 17).
- **Pending, needs playing:** a Vulkan (MoltenVK) session on macOS with captures (the title screen
  renders and no XSC shader falls back, checked without input), and the Linux sessions of
  docs/linux-gpu-measurement.md (section 3), with and without XSC.

## 14. Phase 3 results: vertex shaders

- **Code.** `xsc_translator_vs.cpp` (the prologue: outputs, the guest vertex index in r0.x from the
  host index, the line loop closing vertex, the guest index buffer for point sprites or when the
  host can't take 32-bit indices, the endian swap, the base index, the mask and the clamp; the
  epilogue: the W, XY and Z formats of the guest position, user clip planes, the guest to host
  viewport transform, vertex kill as a NaN W, the point size output and the point sprite
  expansion with its clip distances), vertex fetch in `xsc_translator_fetch.cpp` (every vertex
  format, the endian swap, normalization, the exponent bias), and both emitters: MSL with the
  outputs named as SPIRV-Cross names them, the Y flip, and a second entry point `<name>_norast`
  for pipelines without rasterization; SPIR-V with `gl_PerVertex` and the shared memory storage
  buffer. Both backends send every shader to XSC first.
- **Not supported yet (the SPIR-V translator takes them):** rectangle lists (3 variants of 2
  shaders in the corpus), memory export (1 shader), tessellation, cull distances, and the shared
  memory split into several storage buffers on Vulkan devices with a small storage buffer range.
- **Bench.** Vertex shaders are compared too: each translation's function without rasterization
  gets a capture inserted (the outputs written to a buffer, one record per vertex) and is drawn
  as 1,024 points with the same inputs (random guest memory, vertex fetch constants pointing into
  it, random position formats and viewport constants). 1,474 of 1,478 variants (516 vertex and
  958 pixel) translate with XSC to MSL and to SPIR-V, all the SPIR-V passes `spirv-val`, and the
  comparison gives 5,889 comparisons (2,064 of vertex shaders) with no difference for both. The
  corpus has no point sprite variant XSC supports, so `XSC_BENCH_VS_SYNTHETIC` changes every
  vertex variant to cover point sprites, the point size, 1 to 6 user clip planes and indices
  loaded from the guest memory (16 and 32 bits, every endianness): 5,881 comparisons, no
  difference.
- **Positions in the depth prepass and the main pass.** Every operation rounds the same way in
  both translators (the bench compares positions bit for bit, +0 and -0 equal), so a draw whose
  vertex shader falls back to the SPIR-V translator still gets the same positions as one compiled
  by XSC.
- **In the game,** checked without input (the screen was locked): Metal and Vulkan start with
  XSC for both stages, no GPU error, only the rectangle list and memory export shaders fall back.
  Pending: a play session on each backend, and the measurements.

## 15. Phase 4: measured optimizations

- **Method.** `xsc-bench --mode perf`: every pixel shader variant drawn 20 times at 512x512 with
  the same inputs, the GPU time per draw from the command buffer. One draw per translation was
  too noisy (the same shader varied by 24% from run to run, median); each translation is now drawn
  in 5 rounds alternating the translations, keeping the minimum: the SPIR-V translator, the
  control, then varies by 0.8% from run to run (median), 2.6% (p90). The bench only measures
  pixel shaders in isolation: the game's GPU time also has render passes, transfers and vertex
  work, and is measured in the game (perf_log_csv and traces) and on Linux.
- **Baseline of phase 4** (958 pixel shader variants, sum of the GPU time per draw): SPIR-V
  translator 251 ms, XSC 93 ms.
- **Kept: discard at once.** `discard_fragment()` has demote semantics since MSL 2.3 (the pixel
  stops writing but keeps running for its neighbours' derivatives; SPIRV-Cross writes the SPIR-V
  translator's demote that way), so the kill no longer waits for the end of the shader: the 279
  kill variants 22.1 to 20.4 ms (-8%), up to 3 times faster each (0.15 to 0.05 ms), the others
  unchanged; still bit for bit the SPIR-V translator in the comparison.
- **Removed, no gain:** the constant buffers as structures of a known size instead of pointers
  (89.2 to 89.4 ms), and declaring only the buffers a shader reads (no change on the shaders it
  should have helped).
- **Found, left as is:** 85 shaders are slower with XSC, in total 1.8 ms of 89 ms, almost all of
  them tiny shaders whose output depends only on constants (0.07 to 0.19 ms each here). Bisected
  with `XSC_BENCH_MSL_OVERRIDE` (hand-edited MSL in place of XSC's): the SPIR-V translator's MSL
  is fast because its `[[clang::optnone]]` multiplication hides that the output is the same for
  every pixel; written as a plain multiplication, even in the SPIR-V translator's MSL, the same
  shader takes 2.6 times longer. How Metal handles such shaders is not known (no documentation
  found); the cost is small in total, so XSC is not changed for it.
- **Not done yet, need the game to be measured:**
  - Control flow structurization (section 4): 375 of the 958 pixel shaders and 120 of the 516
    vertex shaders run in the dispatch loop over labels (guest loops and jumps); the others are
    straight code already. Its gain can only be told in the game's own scenes.
  - Specialization (section 5) of the per-draw state read from the system constants (texture
    signs, output gamma, alpha test, 7e3 clamp): it would multiply the pipelines and compile
    shaders while playing, which already causes hitches; to be measured with a trace first.
  - Dropping `NoContraction` in the SPIR-V for Vulkan: measurable only on Linux, and it changes
    the results (fused multiply-add instead of the Xenos rounding), so not without a reason.

## 16. Phase 5: on by default

- `shader_compiler` defaults to `xsc` (the cvar and `runtime/config/azura.default.toml`);
  `shader_compiler = "translator"` in `azura.toml` goes back to the SPIR-V translator.
- **Checked in the game** (22 October 2026, base M4):
  - Metal: 11.8 minutes from the menus past the room with light shafts, no visual difference
    besides the known white flashes; GPU time per frame median 12.3 ms, p90 16.3 ms, p99 22.4 ms
    (baseline 18.9, 23.1 and 30.2 ms). Only the expected shaders fell back (rectangle lists and
    memory export).
  - Vulkan (MoltenVK), windowed: no visual difference. At the 3x3 draw resolution scale the title
    screen takes ~91 ms of GPU per frame with either compiler (~12.6 ms at 1x): the scale, not XSC
    (MoltenVK turns both SPIR-V modules into MSL with SPIRV-Cross, `NoContraction` included).
  - Vulkan in fullscreen on macOS shows a black screen with either compiler: a separate issue.
- **Linux:** pending, docs/linux-gpu-measurement.md (section 3).
- **White flashes, more ruled out:** blending in the fixed-point 16-bit render targets (stored
  divided by 32: a float16 mode with their real values left the flashes, and was removed), and the
  depth precision (`depth_float24_convert_in_pixel_shader` left the flashes; the option also
  breaks the picture on Metal, with the SPIR-V translator, which takes those shaders).

## 17. White flashes and dark sky cards: the texture exponent bias

Two artifacts on macOS (Metal and Vulkan) had the same cause: white flashes on some objects
(sprites and smoke in the intro, dust, glows, Jack cloaking) and dark rectangles over the sky on
the rooftop with the fountain, where the background behind them showed four times darker.

- **Cause.** The Xenos multiplies the result of every texture fetch by 2^`exp_adjust`, bits 13:18
  of the word 3 of the texture fetch constant (`xe_gpu_texture_fetch_t`), as the DXBC translator
  reads it. The SPIR-V translator took bits 13:18 of the word 4 instead, which are part of the LOD
  bias, and XSC did the same to match it bit for bit. So the real exponent was ignored and one
  taken from the LOD bias was applied.
- **How it was found.** A frame history (`metal_frame_history_frames`) gave the images of the
  frames with the artifacts, and a per-draw pixel history (`metal_pixel_history`, a grid of render
  target 0 sampled after every draw, with the shaders and fetch constants of each draw) the draws
  that changed those pixels (docs/metal-backend-design.md, section 10). The dark rectangles are
  distortion cards (pixel shader 28ECCE34FC872029) that sample a copy of the scene
  (`k_2_10_10_10_AS_16_16_16_16`, 1280x720) fetched with `exp_adjust` 2: the shaders applied x1, and
  the card darkened the sky x0.24 where it should be almost invisible. The intro flash is a draw
  (pixel shader 4D71F55D9299E7D4) sampling a DXT1 texture with `exp_adjust` 0 and a LOD bias whose
  bits made x256. With a temporary field logging the bits the shaders used, 12 pixel histories
  (gameplay and intro) had the two exponents differ in 628 of 26,367 texture fetch bindings, in
  about 64 pixel shaders: `exp_adjust` 2 read as 0 (the scene
  copies of the distortion cards and of the bloom), and `exp_adjust` 0 read as anything from -27
  to 30, depending on the LOD bias of the material.
- **Fix.** Both translators read the exponent from the word 3. The bench still finds no difference
  between XSC (MSL and SPIR-V) and the SPIR-V translator (5,889 comparisons). In a play session
  of the intro and the rooftop no artifact was left, and the user saw the game look as on the
  Xbox 360; the bloom, whose source was read four times too dark, is stronger now.
- **Not explained.** Kabu did not see the flashes on Linux, where the SPIR-V translator had the same
  bug; not verified why.

## 18. Risks

- Visual differences in shaders the bench does not reach with random constants (branches the
  game never takes in the bench, special texture formats): the in-game captures cover the main
  scenes, and the per-shader fallback keeps the game working while something is missing.
- Pipeline variant growth from specialization: measured in phase 4 before it is kept.
- Divergence from the upstream SDK: everything is in our own patches, as with the Metal backend.
