# PowerPC interpreter fallback: design

Card 37. The game's PowerPC code is recompiled to C++ ahead of time; when the game calls a function
the code generator did not find, the game stops. This document describes how calls work in the
recompiled code today, what an interpreter has to match, how it plugs in, and how it is validated.

## 1. How the recompiled code runs

- **Functions.** Every guest function the code generator finds becomes a C++ function
  `void sub_XXXXXXXX(PPCContext& ctx, uint8_t* base)` (`PPCFunc`, `include/rex/ppc/func.h`). `ctx`
  holds the guest registers of the thread (`include/rex/ppc/context.h`): r0-r31, lr, ctr, xer (so,
  ov, ca as separate bytes), cr0-cr7 (lt, gt, eq, so as separate bytes), the reservation of
  `lwarx`/`stwcx.`, msr, fpscr, f0-f31 and the 128 VMX128 vector registers. `base` is the host
  address of guest address 0: guest memory is one 4 GB host mapping.
- **Finding functions.** The code generator registers entry points from the `.pdata` exception
  table, the targets of `bl`, the slots of vtables with C++ RTTI, a scan of `lis`/`addi` pairs that
  load code addresses (work in progress upstream), and the manifest's `[entrypoint.functions]`
  hints. Pointers to code stored in the data sections (vtables without RTTI, Unreal Engine 3's
  native function tables, callbacks) are not scanned.
- **Function table.** At start-up every generated function is registered with the
  `FunctionDispatcher` (`src/system/function_dispatcher.cpp`), which writes a table of host
  function pointers right after the image in guest memory (`REX_LOOKUP_FUNC`, two bytes of table per
  byte of code).
- **Calls.** A direct `bl` to a known function is a plain C++ call, after `ctx.lr` is set to the
  return address. A `b` to another function is a call followed by `return` (tail call). `blr`, and
  the conditional forms like `beqlr`, are `return;`: the recompiled code returns to its C++ caller
  whatever `lr` holds. A `bl` to the next instruction (the pattern that reads the program counter)
  only sets `lr`. `bctr` is a `switch` when the code generator found a jump table; otherwise, like
  `bctrl` and `blrl`, it is an indirect call.
- **Indirect calls** (`REX_CALL_INDIRECT_FUNC`, in the generated `<project>_pch.h`): the target is
  looked up in the function table; if it is not there, `ctx.last_indirect_target` is set and
  `rex::runtime::ResolveIndirectFunction` asks the dispatcher (every loaded module). If it still
  finds nothing it returns `InvalidFunctionTrap`, which stops the game with "Call to invalid or
  unregistered function at guest address X". The function it returns is called with `(ctx, base)`
  right away.
- **Non-volatile registers as locals** (`non_volatile_as_local` in the manifest, on for this game):
  r14-r31 are C++ locals of each generated function, so a caller's values live in its own stack
  frame, not in `ctx`. A callee never sees them and the guest's register save and restore helpers
  are not called. An interpreted function uses `ctx.r14`-`ctx.r31` for its own registers, saves
  and restores them as the guest code says, and leaves the caller's locals untouched: the calling
  convention holds on both sides.
- **Memory access** (`REX_LOAD_*`, `REX_STORE_*`): big-endian loads and stores at `base + address`,
  plus 0x1000 for physical addresses at 0xE0000000 and above on macOS ARM64 and Windows. Accesses
  through registers the code generator recognises as MMIO bases go through `REX_MM_*`, which call
  the MMIO handler for 0x7F000000-0x7FFFFFFF.
- **Code in memory.** The XEX loader decompresses the whole image into guest memory at its base
  address (`src/system/xex_module.cpp`), so the original instructions of every function, found or
  not, are there to be read.
- **Floating point modes.** The generated code switches the host's flush-to-zero mode
  (`REX_SET_FLUSH_MODE`): off for scalar floating point (the Xenon's FPU keeps denormals), on for
  VMX (which flushes them), tracked per function as the instructions go.

## 2. Design

- **Where.** `rex::runtime::ResolveIndirectFunction` and `FunctionDispatcher::Execute` return an
  interpreter entry instead of `InvalidFunctionTrap` (behind a setting while it is incomplete). The
  entry has the `PPCFunc` signature and reads its guest address from `ctx.last_indirect_target`,
  which the caller set right before.
- **Execution.** The interpreter fetches, decodes and runs instructions from guest memory, with
  its own program counter, until a `blr` (or a taken conditional return) at its own level, then
  returns to its C++ caller: the same convention as the generated code.
  - `bl` to an address in the function table: C++ call, as the generated code does. `bl` to an
    unknown address: interpreted recursively (C++ recursion). `bl` to the next instruction: only
    `lr`.
  - `b` (and conditional branches) to an address in the function table other than the one being
    interpreted: tail call. Otherwise a jump.
  - `bctr`: jump, or tail call if the target is in the function table. `bctrl`/`blrl`: call, through
    the table or interpreted.
- **Semantics by construction.** Each instruction is written from the code generator's builder for
  it (`src/codegen/builders/`), with the same `PPCContext` fields and helpers (CR and XER updates,
  record forms, the reservation, `REX_QUERY_TIMEBASE`, traps, flush mode), so both paths compute
  the same thing. The decoder is the code generator's own (`ppc_insn`, operands as it decodes them).
- **Log.** Each interpreted function is logged once with its address, so it can be added to the
  manifest as a hint and go back to the fast path.

## 3. The differential bench

Every instruction form runs through the interpreter and through the recompiled code, from the same
state, and the results must match bit for bit. The bench lives in the SDK patch
`patches/rexglue-0.10.0-ppc-interpreter.patch` (`tests/interp_bench/`) and builds only when asked
for:

```
cmake --build --preset mac-arm64-sdk-source-relwithdebinfo --target ppc_interp_bench
sdk/rexglue-0.10.0-src/out/mac-arm64/RelWithDebInfo/ppc_interp_bench [--seeds N] [--filter TEXT]
    [--report FILE] [--verbose] [--self-check]
```

- **Cases.** `rexglue interp-bench-gen` walks the decoder's opcode table. Each entry is a form
  (`add` and `add.` are two), and it counts when it belongs to the Xenon dialect and the code
  generator has a builder for it. For each form it draws words with random bits outside the form's
  mask and keeps those that the decoder decodes to that same entry (the decoder takes the first
  entry that matches, so aliases like `tdlnli` never get words of their own). Six words per form;
  each case is the word followed by `blr`, at its own address from 0x82010000. No assembler is
  needed.
- **Left out of the random cases:** branches (they move the program counter and get layouts of
  their own in phase 1), `mftb`/`mftbu` (the time base differs between two runs), `mtmsrd` (it
  takes and releases the global lock), and the service traps 20 and 26 (they log a string read at
  r3).
- **Recompiled side.** The cases go through the real code generator as a test binary
  (`TestModule` and `AnalyzeTestBinary`, as `rexglue recompile-tests` does), with every register
  in `ctx`, so all effects are visible. The emitted C++ is compiled like the game's generated code:
  `-O2` and the compiler's default floating point model, not the SDK's `-ffp-model=strict`, so any
  multiply and add the compiler fuses in the game is fused in the bench too. Where a builder emits a
  trap for operand values it does not handle (some `vpkd3d128` and `vupkd3d128` types), the
  interpreter must stop rather than run the instruction.
- **Interpreted side.** The bench writes the same words into guest memory and interprets them.
- **Guest memory.** A 4 GB reservation that is inaccessible except for the cases' code and two
  8 KB windows: one at 0x40000000 and one at 0xE0010000, in the physical range the recompiled code
  maps 0x1000 bytes further on the host. Memory cases get their base and index registers set so
  that the access lands in the middle of a window (8-byte aligned for the atomic forms). A stray
  access faults, and the fault is reported for that case instead of stopping the bench.
- **State.** Random registers from a seed per run: GPRs with small, sign- and zero-extended,
  full 64-bit and edge values; floating point values including zeros, infinities, quiet and
  signaling NaNs, denormals, singles and integers near the conversion limits; vector lanes with
  float specials, small integers and random bits; CR and XER bits; the reservation (half the time
  equal to memory, so `stwcx.` and `stdcx.` store); the rounding mode and flush-to-zero bits of the
  host register; random window contents.
- **Comparison.** Every field of `PPCContext`, the host floating point control register and both
  windows, after each side, bit for bit; a mismatch is reported with the instruction, the seed and
  the first differing field. `--self-check` runs the recompiled code on both sides, which must
  always match: it shows the bench itself is deterministic.

## 4. Phases

0. This document and the differential bench, running with the interpreter returning "not
   supported" for every form. Measured: 566 forms with cases, 3,357 cases (6 per form; 9 forms
   are aliases without words of their own, 100 branch forms left for phase 1); the interpreter
   reports all 566 as unsupported and nothing fails; `--self-check` with 64 seeds gives 214,720
   identical runs in 2.6 s.
1. Integer core (arithmetic, logical, shifts and rotates, compares, branches, condition and
   special registers, loads and stores, `lwarx`/`stwcx.`) and the fallback in the dispatcher behind
   a setting.
2. Floating point.
3. VMX and VMX128.
4. In the game, without the 0x824B8CF0 hint: the point where the game stopped is passed through the
   interpreter, and the cost is measured.
5. On by default, documentation, and the decision on a data pointer scan in the code generator.

## 5. Risks

- Instructions whose recompiled form depends on what the code generator learned about the
  function (jump tables, MMIO base registers, CSR state): the interpreter checks at run time what
  the code generator decided ahead of time (the MMIO range on every access, the target of every
  `bctr`), which gives the same result for the same inputs.
- `setjmp`/`longjmp` in guest code, which the generated code maps to host `setjmp`: kept out of the
  interpreter until a game needs it.
- Speed: interpreted functions are tens of times slower than recompiled ones; the log lets each
  one be added as a hint.
