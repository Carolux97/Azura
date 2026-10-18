#!/usr/bin/env python3
"""
Applies essential engine fixes to Gears of War 3 recompiled C++ code.
Corresponds to official community patches for Title ID 4D5308AB:
  1. Disable Ambient Occlusion (0x82762c54 & 0x827c9628)
     Prevents the UE3 SSAO pass from generating a solid black multiplier across the screen.
  2. Occlusion Query Fix (0x827b2bd0 & 0x827b6c58)
     Forces occlusion testing against full depth, preventing UE3 from culling static environment meshes.
"""

import sys
from pathlib import Path

def apply_patches(generated_dir: Path):
    patches = [
        {
            "file": generated_dir / "azura_recomp.181.cpp",
            "name": "Disable Ambient Occlusion (Part 1, 0x82762C54)",
            "old": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
            "new": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
        },
        {
            "file": generated_dir / "azura_recomp.122.cpp",
            "name": "Disable Ambient Occlusion (Part 2, 0x827C9628)",
            "old": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
            "new": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
        },
        {
            "file": generated_dir / "azura_recomp.198.cpp",
            "name": "Occlusion Query Fix (Part 1, 0x827B2BD0)",
            "old": "\t// li r11,1\n\tctx.r11.s64 = 1;",
            "new": "\t// li r11,1\n\tctx.r11.s64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        },
        {
            "file": generated_dir / "azura_recomp.129.cpp",
            "name": "Occlusion Query Fix (Part 2, 0x827B6C58)",
            "old": "loc_827B6C58:\n\t// lwz r11,2700(r14)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r14.u32 + 2700);",
            "new": "loc_827B6C58:\n\t// lwz r11,2700(r14)\n\tctx.r11.u64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        },
        {
            "file": generated_dir / "azura_recomp.99.cpp",
            "name": "Disable Bloom and Depth of Field (0x82593DC0 & 0x82593DC4)",
            "old": "\t// lwz r10,72(r11)\n\tctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 72);\n\t// lwz r11,64(r11)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 64);",
            "new": "\t// lwz r10,72(r11)\n\tctx.r10.u64 = 0; // Patch: Disable Bloom (li r10, 0)\n\t// lwz r11,64(r11)\n\tctx.r11.u64 = 0; // Patch: Disable Depth of Field (li r11, 0)",
        },
        {
            "file": generated_dir / "azura_recomp.187.cpp",
            "name": "Disable Dynamic Shadows for NVIDIA (0x827CE810)",
            "old": "\t// lwz r11,1044(r21)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r21.u32 + 1044);",
            "new": "\t// lwz r11,1044(r21)\n\tctx.r11.u64 = 0; // Patch: Disable Dynamic Shadows (li r11, 0)",
        },
    ]

    for p in patches:
        path = p["file"]
        if not path.is_file():
            print(f"[Patch] Warning: File {path.name} not found, skipping {p['name']}.")
            continue

        content = path.read_text(encoding="utf-8")
        if p["new"] in content:
            print(f"[Patch] Already applied: {p['name']} in {path.name}")
            continue

        if p["old"] in content:
            updated = content.replace(p["old"], p["new"], 1)
            path.write_text(updated, encoding="utf-8")
            print(f"[Patch] Applied: {p['name']} in {path.name}")
        else:
            print(f"[Patch] Target signature not found in {path.name} for {p['name']}!")

if __name__ == "__main__":
    target = Path(__file__).resolve().parent.parent / "generated" / "default"
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    apply_patches(target)
