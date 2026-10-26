#!/usr/bin/env python3
"""Finds the recompiled guest functions that build PM4 command packets or address GPU registers.

Reads the C++ that `rexglue codegen` generated from the player's own default.xex
(runtime/generated/), where each PowerPC instruction is kept as a comment above its translation,
and reconstructs the 32-bit constants each function materialises (`lis` followed by `ori`,
`addi` or `oris`, or a lone `li`). A function is reported when such a constant is:

- a PM4 type-3 packet header: bits 31-30 = 3, an opcode from the public PM4 opcode list
  (rex/graphics/xenos.h) in bits 14-8, a plausible word count in bits 29-16, low byte 0;
- the high half of the GPU register window (0x7FC8xxxx), which the guest maps for direct register
  access.

The analysis is linear within each function (it ignores control flow), so a constant counts even
if only one path builds it: good enough to shortlist candidates that the run-time trace then
confirms. The output (addresses, counts, opcodes) is derived from the game and stays local: write
it under captures/, which git ignores, or anywhere outside the repository.

Usage: scripts/hle/scan_pm4_writers.py [generated_dir] > captures/pm4_writers.tsv
"""

import collections
import glob
import os
import re
import sys

# PM4 type-3 opcodes, from the public list in rex/graphics/xenos.h.
PM4_OPCODES = {
    0x48: "ME_INIT", 0x10: "NOP", 0x3F: "INDIRECT_BUFFER", 0x37: "INDIRECT_BUFFER_PFD",
    0x26: "WAIT_FOR_IDLE", 0x3C: "WAIT_REG_MEM", 0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE",
    0x5C: "WAIT_UNTIL_READ", 0x5D: "WAIT_IB_PFD_COMPLETE", 0x21: "REG_RMW", 0x3E: "REG_TO_MEM",
    0x3D: "MEM_WRITE", 0x4F: "MEM_WRITE_CNTR", 0x44: "COND_EXEC", 0x45: "COND_WRITE",
    0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL",
    0x5A: "EVENT_WRITE_EXT", 0x5B: "EVENT_WRITE_ZPD", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2",
    0x34: "DRAW_INDX_BIN", 0x35: "DRAW_INDX_2_BIN", 0x23: "VIZ_QUERY", 0x25: "SET_STATE",
    0x2D: "SET_CONSTANT", 0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS",
    0x2F: "LOAD_ALU_CONSTANT", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE",
    0x2E: "LOAD_CONSTANT_CONTEXT", 0x3B: "INVALIDATE_STATE", 0x4A: "SET_SHADER_BASES",
    0x4B: "SET_BIN_BASE_OFFSET", 0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT",
    0x5E: "CONTEXT_UPDATE", 0x54: "INTERRUPT", 0x2C: "IM_STORE", 0x60: "SET_BIN_MASK_LO",
    0x61: "SET_BIN_MASK_HI", 0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI",
}

# Largest word count accepted in a type-3 header; real packets in a D3D runtime are small, and
# a bound keeps unrelated large constants out.
MAX_TYPE3_COUNT = 0x400
GPU_REGISTER_HIGH_HALF = 0x7FC8

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
INSN_RE = re.compile(r"^\s*// (\w+\.?) (.*)$")


def imm(text):
    text = text.strip()
    return int(text, 16) if text.lower().startswith(("0x", "-0x")) else int(text)


def type3_opcode(value):
    """Returns the opcode name when value looks like a PM4 type-3 header, else None."""
    if (value >> 30) != 3 or (value & 0xFF) != 0:
        return None
    count = (value >> 16) & 0x3FFF
    opcode = (value >> 8) & 0x7F
    if count >= MAX_TYPE3_COUNT or opcode not in PM4_OPCODES:
        return None
    return PM4_OPCODES[opcode]


def scan_file(path, results):
    func = None
    regs = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = FUNC_RE.match(line)
        if m:
            func = m.group(1)
            regs = {}
            continue
        if func is None:
            continue
        m = INSN_RE.match(line)
        if not m:
            continue
        op, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
        value = None
        try:
            if op == "lis" and len(args) == 2:
                high = imm(args[1]) & 0xFFFF
                regs[args[0]] = (high << 16) & 0xFFFFFFFF
                if high == GPU_REGISTER_HIGH_HALF:
                    results[func]["gpu_mmio"] += 1
                continue
            if op == "li" and len(args) == 2:
                regs[args[0]] = imm(args[1]) & 0xFFFFFFFF
                continue
            if op in ("ori", "addi", "oris") and len(args) == 3 and args[1] in regs:
                base_value, k = regs[args[1]], imm(args[2])
                if op == "ori":
                    value = base_value | (k & 0xFFFF)
                elif op == "oris":
                    value = base_value | ((k & 0xFFFF) << 16)
                else:
                    value = (base_value + k) & 0xFFFFFFFF
                regs[args[0]] = value
            else:
                # Any other instruction writing a register invalidates what it held.
                if args and args[0] in regs and op not in ("stw", "stwx", "stwu", "cmpw", "cmplw", "cmpwi", "cmplwi"):
                    del regs[args[0]]
                continue
        except ValueError:
            continue
        name = type3_opcode(value)
        if name:
            results[func]["type3"][name] += 1


def main():
    generated = sys.argv[1] if len(sys.argv) > 1 else "runtime/generated/default"
    results = collections.defaultdict(lambda: {"type3": collections.Counter(), "gpu_mmio": 0})
    files = sorted(glob.glob(os.path.join(generated, "azura_recomp.*.cpp")))
    if not files:
        sys.exit(f"No generated code in {generated}: build the runtime first")
    for path in files:
        scan_file(path, results)
    print("function\ttype3_headers\tgpu_mmio\topcodes")
    rows = [(f, r) for f, r in results.items() if r["type3"] or r["gpu_mmio"]]
    rows.sort(key=lambda fr: (-sum(fr[1]["type3"].values()), fr[0]))
    for func, r in rows:
        opcodes = ",".join(f"{k}x{v}" for k, v in sorted(r["type3"].items()))
        print(f"{func}\t{sum(r['type3'].values())}\t{r['gpu_mmio']}\t{opcodes}")


if __name__ == "__main__":
    main()
