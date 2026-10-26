#!/usr/bin/env python3
"""Summarizes a PM4 writer trace (runtime/src/hle_gfx/pm4_trace.h) per guest function.

For each traced function that ran: calls, calls from outside the traced range (from the game's
own code), its own and inclusive PM4 bytes, its call sites, and the packets its calls produced:
registers (named from the SDK's public register table, with numbered banks such as shader
constants folded into one name), type-3 opcodes and SET_CONSTANT kinds. The output describes the
player's own game; keep it local.

Usage: scripts/hle/summarize_pm4_trace.py TRACE.tsv [FIRST LAST] > captures/summary.txt
  (FIRST and LAST: the traced range in hex, default 82AE0000 82B10000)
"""

import collections
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REGISTER_TABLE = os.path.join(REPO, "sdk", "rexglue-0.10.0-src", "include", "rex", "graphics", "register_table.inc")
OPCODES = {
    0x48: "ME_INIT", 0x10: "NOP", 0x3F: "INDIRECT_BUFFER", 0x37: "INDIRECT_BUFFER_PFD",
    0x26: "WAIT_FOR_IDLE", 0x3C: "WAIT_REG_MEM", 0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE",
    0x5C: "WAIT_UNTIL_READ", 0x5D: "WAIT_IB_PFD_COMPLETE", 0x21: "REG_RMW", 0x3E: "REG_TO_MEM",
    0x3D: "MEM_WRITE", 0x4F: "MEM_WRITE_CNTR", 0x44: "COND_EXEC", 0x45: "COND_WRITE",
    0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5A: "EVENT_WRITE_EXT",
    0x5B: "EVENT_WRITE_ZPD", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x34: "DRAW_INDX_BIN",
    0x35: "DRAW_INDX_2_BIN", 0x23: "VIZ_QUERY", 0x25: "SET_STATE", 0x2D: "SET_CONSTANT",
    0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x2F: "LOAD_ALU_CONSTANT", 0x27: "IM_LOAD",
    0x2B: "IM_LOAD_IMMEDIATE", 0x2E: "LOAD_CONSTANT_CONTEXT", 0x3B: "INVALIDATE_STATE",
    0x4A: "SET_SHADER_BASES", 0x4B: "SET_BIN_BASE_OFFSET", 0x50: "SET_BIN_MASK",
    0x51: "SET_BIN_SELECT", 0x5E: "CONTEXT_UPDATE", 0x54: "INTERRUPT", 0x2C: "IM_STORE",
    0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI", 0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI",
}
CONSTANT_KINDS = {0: "ALU", 1: "FETCH", 2: "BOOL", 3: "LOOP", 4: "REGISTERS"}


def register_names():
    names = {}
    for m in re.finditer(r"^XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)", open(REGISTER_TABLE).read(), re.M):
        # Numbered banks (SHADER_CONSTANT_012_X, SHADER_CONSTANT_FETCH_05_3, ...) fold into one name.
        names[int(m.group(1), 16)] = re.sub(r"(_\d+)+(_[XYZW])?$", "_*", m.group(2))
    return names


def main():
    path = sys.argv[1]
    lo, hi = (int(sys.argv[2], 16), int(sys.argv[3], 16)) if len(sys.argv) > 3 else (0x82AE0000, 0x82B10000)
    names = register_names()
    funcs = {}
    callers = collections.defaultdict(collections.Counter)
    packets = collections.defaultdict(collections.Counter)
    section = None
    for line in open(path):
        if line.startswith("#"):
            section = line.strip()
            continue
        p = line.rstrip("\n").split("\t")
        if p[0] == "function":
            continue
        if section == "# functions":
            funcs[p[0]] = tuple(int(x) for x in p[1:5])
        elif section == "# callers":
            callers[p[0]][int(p[1], 16)] += int(p[3])
        elif section == "# packets":
            kind, value, n = p[1], int(p[2], 16), int(p[3])
            if kind == "register":
                label = "reg " + names.get(value, f"0x{value:04X}")
            elif kind == "opcode":
                label = "op " + OPCODES.get(value, f"0x{value:02X}")
            else:
                label = "const " + CONSTANT_KINDS.get(value, str(value))
            packets[p[0]][label] += n

    def external(f):
        return sum(n for ra, n in callers[f].items() if not lo <= ra < hi)

    rows = sorted(funcs, key=lambda f: (-funcs[f][3], -funcs[f][0]))
    for f in rows:
        calls, self_bytes, wraps, inclusive = funcs[f]
        sites = sorted(callers[f].items(), key=lambda kv: -kv[1])
        print(f"{f}  calls={calls} external={external(f)} self_bytes={self_bytes} "
              f"inclusive_bytes={inclusive} per_call={inclusive / calls:.1f} wraps={wraps}")
        print("   callers: " + ", ".join(f"{ra - 4:08X}x{n}" + ("" if lo <= ra < hi else "*") for ra, n in sites[:6]))
        top = packets[f].most_common(14)
        if top:
            print("   packets: " + ", ".join(f"{k} {n}" for k, n in top))


if __name__ == "__main__":
    main()
