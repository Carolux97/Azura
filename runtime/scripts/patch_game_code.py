#!/usr/bin/env python3
"""
Applies engine fixes to the recompiled Gears of War 3 C++ code (runtime/generated/default).

They are the Xenia Canary community patches for title 4D5308AB (game-patches repository,
"4D5308AB - Gears of War 3.patch.toml"), each one turning a `lwz r11,...` into `li r11,0`:

  1. Disable Ambient Occlusion: 0x82762C54 and 0x827C9628.
  2. Occlusion Query Fix: 0x827B2BD0 and 0x827B6C58. Makes the occlusion pass test against the
     full-resolution scene depth instead of the downsampled one.

Every patch is located by its exact guest address: the enclosing function (`DEFINE_REX_FUNC`) is
found, its instructions are counted (the code generator writes one `// <instruction>` comment per
guest instruction), and the instruction at the address must match the expected one before the C
statement under it is replaced. A patch that does not find what it expects is not applied.

An earlier version of this script searched for the first text match in a file, and for Occlusion
Query Fix part 1 it changed `li r11,1` at 0x827B2BE0 instead of `lwz r11,2700(r28)` at 0x827B2BD0.
In sub_827B2BA0 that binds the downsampled occlusion depth surface as render target but clears the
flag that makes the function restore the scene render targets at its end (bl 0x827C5BE8), so the
rest of the frame was drawn into the wrong targets. REVERTS undoes that change in generated code
that still carries it.
"""

import re
import sys
from pathlib import Path

PATCHES = [
    {
        "name": "Disable Ambient Occlusion (Part 1)",
        "function": 0x82762C30,
        "address": 0x82762C54,
        "asm": "lwz r11,1040(r10)",
        "old": "ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
        "new": "ctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
    },
    {
        "name": "Disable Ambient Occlusion (Part 2)",
        "function": 0x827C9610,
        "address": 0x827C9628,
        "asm": "lwz r11,1040(r10)",
        "old": "ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
        "new": "ctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
    },
    {
        "name": "Disable Depth of Field",
        "function": 0x82593DB8,
        "address": 0x82593DC4,
        "asm": "lwz r11,64(r11)",
        "old": "ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 64);",
        "new": "ctx.r11.u64 = 0; // Patch: Disable Depth of Field (li r11, 0)",
    },
    {
        "name": "16x Anisotropic Filtering",
        "function": 0x82969558,
        "address": 0x82969614,
        "asm": "lwz r5,836(r30)",
        "old": "ctx.r5.u64 = REX_LOAD_U32(ctx.r30.u32 + 836);",
        "new": "ctx.r5.u64 = 16; // Patch: 16x Anisotropic Filtering (li r5, 16)",
    },
    {
        "name": "Disable Motion Blur (Part 1)",
        "function": 0x824305E8,
        "address": 0x82430640,
        "asm": "lwz r11,56(r10)",
        "old": "ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 56);",
        "new": "ctx.r11.u64 = 0; // Patch: Disable Motion Blur (li r11, 0)",
    },
    {
        "name": "Disable Motion Blur (Part 2)",
        "function": 0x827A2B98,
        "address": 0x827A2BCC,
        "asm": "lwz r11,128(r10)",
        "old": "ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 128);",
        "new": "ctx.r11.u64 = 0; // Patch: Disable Motion Blur (li r11, 0)",
    },
]

# Changes made by earlier versions of this script that must be undone.
REVERTS = [
    {
        "name": "Misplaced Occlusion Query Fix (li r11,1 at 0x827B2BE0)",
        "function": 0x827B2BA0,
        "address": 0x827B2BE0,
        "asm": "li r11,1",
        "old": "ctx.r11.s64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        "new": "ctx.r11.s64 = 1;",
    },
    {
        "name": "Harmful Occlusion Query Fix Part 1 (0x827B2BD0)",
        "function": 0x827B2BA0,
        "address": 0x827B2BD0,
        "asm": "lwz r11,2700(r28)",
        "old": "ctx.r11.u64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        "new": "ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 2700);",
    },
    {
        "name": "Harmful Occlusion Query Fix Part 2 (0x827B6C58)",
        "function": 0x827B6BA8,
        "address": 0x827B6C58,
        "asm": "lwz r11,2700(r14)",
        "old": "ctx.r11.u64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        "new": "ctx.r11.u64 = REX_LOAD_U32(ctx.r14.u32 + 2700);",
    },
]

INSTRUCTION_RE = re.compile(r"^\s*// [a-z]")


def find_function(files, function):
    header = f"DEFINE_REX_FUNC(sub_{function:08X}) {{"
    for f in files:
        content = f.read_text(encoding="utf-8")
        if header in content:
            return f, content, header
    return None, None, header


def locate_statement(lines, start, index, asm):
    """Returns the line number of the C statement for instruction `index` of the function whose
    header is at line `start`, or an error string."""
    count = -1
    for i in range(start + 1, len(lines)):
        line = lines[i]
        if line.startswith("DEFINE_REX_FUNC("):
            return f"function ended before instruction #{index}"
        if INSTRUCTION_RE.match(line):
            count += 1
            if count == index:
                found = line.strip()[3:]
                if found != asm:
                    return f"expected '{asm}' but found '{found}'"
                for j in range(i + 1, len(lines)):
                    if lines[j].strip():
                        return j
                return "no statement after instruction"
    return "end of file"


def apply(files, p):
    f, content, header = find_function(files, p["function"])
    if f is None:
        print(f"[Patch] Warning: {header} not found for {p['name']}")
        return
    lines = content.split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith(header))
    index = (p["address"] - p["function"]) // 4
    where = locate_statement(lines, start, index, p["asm"])
    if isinstance(where, str):
        print(f"[Patch] Warning: {p['name']} at 0x{p['address']:08X}: {where}; not applied")
        return
    statement = lines[where].strip()
    indent = lines[where][: len(lines[where]) - len(lines[where].lstrip())]
    if statement == p["new"]:
        print(f"[Patch] Already applied: {p['name']} ({f.name})")
        return
    if statement != p["old"]:
        print(f"[Patch] Warning: {p['name']} at 0x{p['address']:08X}: unexpected code "
              f"'{statement}'; not applied")
        return
    lines[where] = indent + p["new"]
    f.write_text("\n".join(lines), encoding="utf-8")
    print(f"[Patch] Applied: {p['name']} at 0x{p['address']:08X} ({f.name})")


def revert(files, r):
    f, content, header = find_function(files, r["function"])
    if f is None:
        return
    lines = content.split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith(header))
    where = locate_statement(lines, start, (r["address"] - r["function"]) // 4, r["asm"])
    if isinstance(where, str) or lines[where].strip() != r["old"]:
        return
    indent = lines[where][: len(lines[where]) - len(lines[where].lstrip())]
    lines[where] = indent + r["new"]
    f.write_text("\n".join(lines), encoding="utf-8")
    print(f"[Patch] Reverted: {r['name']} ({f.name})")


def main():
    target = Path(__file__).resolve().parent.parent / "generated" / "default"
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    files = sorted(target.glob("azura_recomp.*.cpp"))
    if not files:
        print(f"[Patch] No azura_recomp.*.cpp files found in {target}!")
        return
    for r in REVERTS:
        revert(files, r)
    for p in PATCHES:
        apply(files, p)


if __name__ == "__main__":
    main()
