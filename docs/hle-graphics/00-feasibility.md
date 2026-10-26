# HLE graphics, phase 0: can a recompiled guest function be replaced with native code?

Part of the HLE graphics prototype: replacing the game's linked Xbox 360 D3D library, which writes
PM4 packets that the runtime's command processor emulates (Xenos registers, EDRAM), with native
implementations that drive Metal (macOS) and Vulkan (Linux, Windows) directly. Before identifying
those functions (phase 1) we need to know whether the runtime can replace one at all.

**Answer: yes, with what ReXGlue 0.10.0 already provides. No SDK change is needed.**

## How the code generator emits a function

Every recompiled function is defined through `DEFINE_REX_FUNC(name)`, which the code generator
writes into the generated `azura_pch.h` (the macro's source is the code generator's template; the
generated file itself is never committed). The function's body is `__imp__<name>`, and `<name>`
(`sub_XXXXXXXX`, the guest address) is a **weak** symbol that reaches it:

| Platform | What `<name>` is | Why |
| --- | --- | --- |
| macOS | a weak, `noinline` thunk whose body is a `musttail` call to `__imp__<name>` (one branch, no frame) | Mach-O has no overridable alias: `__attribute__((alias))` is rejected on Darwin |
| Linux, Windows | a weak alias of `__imp__<name>` | ELF and COFF have weak aliases |

A strong definition of `<name>` anywhere else in the link therefore wins over the generated one,
and `__imp__<name>` keeps the original body, callable from the replacement.

Every way into a recompiled function goes through `<name>`:

- direct calls in the generated code are `sub_XXXXXXXX(ctx, base)`;
- the indirect call table `PPCFuncMappings` (generated `azura_init.cpp`) maps each guest address
  to `<name>`, so virtual calls, function pointers and `rex::runtime::ResolveIndirectFunction`
  (used by the PowerPC interpreter to call back into recompiled code) reach the replacement too.

## The SDK's API for it

`sdk/rexglue-0.10.0-src/include/rex/hook.h` is the SDK's hook authoring API:

- `REX_HOOK_RAW(name) { ... }` defines a replacement with direct access to the guest registers
  (`ctx`) and guest memory (`base`), the form the HLE layer will use;
- `REX_HOOK(name, fn)` marshals guest registers into the arguments of a plain C++ function;
- `REX_STUB`, `REX_STUB_RETURN` replace a function with a logged no-op;
- `REX_IMPORT(symbol, callable, sig)` gives a typed C++ callable for a recompiled function, to
  call the original (`__imp__<name>`) or any other guest function from native code.

The SDK uses the same mechanism for its own C runtime replacements (`src/kernel/crt/`, enabled
per game under `[rexcrt]` in the manifest; ours leaves them off). Manifest `[functions]` entries
also accept a `name`, so a function can be renamed in the generated code
(`src/codegen/config.cpp`), and `[[entrypoint.midasm_hook]]` (already used by
`runtime/src/render_queue.cpp`) inserts a call at a single instruction.

## Probe (verified on macOS)

A temporary `REX_HOOK_RAW(sub_822B2140)` in `runtime/src` (the space reservation of the engine's
render command queue, see [reverse-engineering.md](../reverse-engineering.md#render-command-queue))
counted its calls, logged the count every 10 seconds and then called `__imp__sub_822B2140`. It
was built with `scripts/build.sh` (RelWithDebInfo, Apple M4) and removed afterwards.

- The link took the replacement: `nm` shows `_sub_822B2140` as a strong text symbol (`T`) next to
  `___imp__sub_822B2140`.
- A 60-second run from start reached the game's own menu setup (the menu patches applied) and
  logged the replacement being called, rising to about 1.1 million calls (some 38,000 per second
  once the engine was rendering), with no guest access violation and no invalid call in the log.
- The screen capture of that run showed another window in front, so the picture itself was not
  checked: the probe proves the call path, not the visual result (a pass-through cannot change
  it anyway).

**Not verified by a build:** Linux and Windows. No Linux machine was available, and the Windows
build (native or cross from Linux) was not run. The mechanism there is the standard one (a
strong definition overrides a weak alias in ld, lld and lld-link), but that is an inference until
phase 3 builds the replacement on those platforms.

## How `--gfx=pm4|hle` fits

Each replaced function is always linked in, and decides at run time:

```cpp
REX_HOOK_RAW(sub_XXXXXXXX) {
  if (!hle_gfx::Enabled()) {        // --gfx=pm4 (default): the original, unchanged behaviour
    __imp__sub_XXXXXXXX(ctx, base);
    return;
  }
  hle_gfx::SetRenderTarget(ctx, base);  // --gfx=hle: native implementation
}
```

The mode is read once at start and never changes, so a frame never mixes the two paths. In
`pm4` mode the cost is one predictable branch per call; nothing else changes.

## Metal and Vulkan

The replacement mechanism is the same on every platform, so the HLE path is not Metal only:
Linux and Windows get it on Vulkan. Planned shape (detailed in phase 2's design):

- `runtime/src/hle_gfx/` holds a backend-agnostic core: the state tracker that mirrors the guest
  API, resources (Xenos tiling and endianness conversion, format mapping), and frame and render
  pass planning;
- two backends behind one interface, `metal/` (macOS) and `vulkan/` (Linux, Windows);
- shaders reuse the SDK's existing translators and pipeline caches: the Xenos shader compiler's
  Metal path (`src/graphics/metal/`) and the SPIR-V translator
  (`src/graphics/pipeline/shader/spirv*.cpp`) for Vulkan;
- render pass planning aims at few pass breaks for tile GPUs (Apple) and maps to Vulkan render
  passes on desktop GPUs, where avoiding the EDRAM transfers helps too.

The prototype is built on Metal first (the development machine), then Vulkan.

## Limits and risks for phase 1

- **Inlined D3D code.** Only functions can be replaced. Whatever part of the D3D library the
  original compiler inlined into the game (writes to the command buffer, register shadow
  updates) has no function to hook; phase 1 has to measure how much of the PM4 stream comes from
  real functions and how much from inlined code. If a lot is inlined, the hooks move up a level
  (the engine's own render functions) or use mid-function hooks.
- **Undiscovered functions.** A function the code generator did not find runs in the PowerPC
  interpreter and cannot be replaced until it is listed under `[entrypoint.functions]` in the
  manifest, which needs a codegen run.
- **Shared state with the emulated GPU.** Kernel exports (`VdSwap`, interrupt callbacks, GPU
  fences the game waits on) and memory the game reads back from the GPU must keep working in
  `hle` mode; phase 1 lists them.
- **Both editions.** World (58122AD6) and España (6EF14DF9) share the same code, so one function
  map serves both; another build would need its own.
