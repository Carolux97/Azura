// PM4 writer trace: a debug build option (AZURA_PM4_TRACE) that replaces every guest function in
// a chosen address range with a pass-through which records, per function, how often it runs, how
// many bytes of PM4 commands it writes itself and who calls it. It maps the game's linked D3D
// library for the HLE graphics work (docs/hle-graphics/01-function-map.md).
//
// The pass-throughs are generated (scripts/hle/gen_pm4_trace_hooks.py writes
// pm4_trace_hooks.gen.cpp, which git ignores: its list of addresses comes from the player's own
// game). Each one calls Enter, the original function (__imp__sub_XXXXXXXX), then Exit.

#pragma once

#include <cstddef>
#include <cstdint>

struct PPCContext;

namespace azura::pm4_trace {

// Guest addresses of the traced functions, in the generated file; Enter and Exit take an index
// into it.
extern const uint32_t kTracedFunctions[];
extern const size_t kTracedFunctionCount;

void Enter(uint32_t index, PPCContext& ctx, uint8_t* base);
void Exit(uint32_t index, uint8_t* base);

}  // namespace azura::pm4_trace
