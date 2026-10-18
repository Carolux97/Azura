#!/usr/bin/env python3
"""
Applies essential engine fixes to Gears of War 3 recompiled C++ code.
Corresponds to official community patches for Title ID 4D5308AB:
  1. Disable Ambient Occlusion (0x82762c54 & 0x827c9628)
     Prevents the UE3 SSAO pass from generating a solid black multiplier across the screen.
  2. Occlusion Query Fix (0x827b2bd0 & 0x827b6c58)
     Forces occlusion testing against full depth, preventing UE3 from culling static environment meshes.
  3. Disable Bloom and Depth of Field (0x82593DC0 & 0x82593DC4)
  4. Disable Dynamic Shadows for NVIDIA (0x827CE810)
"""

import sys
from pathlib import Path

def apply_patches(generated_dir: Path):
    patches = [
        {
            "name": "Disable Ambient Occlusion (Part 1, 0x82762C54)",
            "context": "loc_82762D60",
            "old": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
            "new": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
        },
        {
            "name": "Disable Ambient Occlusion (Part 2, 0x827C9628)",
            "context": "loc_827C985C",
            "old": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 1040);",
            "new": "\t// lwz r11,1040(r10)\n\tctx.r11.u64 = 0; // Patch: Disable Ambient Occlusion (li r11, 0)",
        },
        {
            "name": "Occlusion Query Fix (Part 1, 0x827B2BD0)",
            "context": "sub_827B2BA0",
            "old": "\t// li r21,0\n\tctx.r21.s64 = 0;\n\t// li r11,1\n\tctx.r11.s64 = 1;",
            "new": "\t// li r21,0\n\tctx.r21.s64 = 0;\n\t// li r11,1\n\tctx.r11.s64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        },
        {
            "name": "Occlusion Query Fix (Part 2, 0x827B6C58)",
            "context": "loc_827B6C58:",
            "old": "loc_827B6C58:\n\t// lwz r11,2700(r14)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r14.u32 + 2700);",
            "new": "loc_827B6C58:\n\t// lwz r11,2700(r14)\n\tctx.r11.u64 = 0; // Patch: Occlusion Query Fix (li r11, 0)",
        },
        {
            "name": "Disable Bloom and Depth of Field (0x82593DC0 & 0x82593DC4)",
            "context": "82593D",
            "old": "\t// lwz r10,72(r11)\n\tctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 72);\n\t// lwz r11,64(r11)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 64);",
            "new": "\t// lwz r10,72(r11)\n\tctx.r10.u64 = 0; // Patch: Disable Bloom (li r10, 0)\n\t// lwz r11,64(r11)\n\tctx.r11.u64 = 0; // Patch: Disable Depth of Field (li r11, 0)",
        },
        {
            "name": "Disable Dynamic Shadows for NVIDIA (0x827CE810)",
            "context": "827CE8",
            "old": "\t// lwz r11,1044(r21)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r21.u32 + 1044);",
            "new": "\t// lwz r11,1044(r21)\n\tctx.r11.u64 = 0; // Patch: Disable Dynamic Shadows (li r11, 0)",
        },
    ]

    all_files = sorted(list(generated_dir.glob("azura_recomp.*.cpp")))
    if not all_files:
        print(f"[Patch] No azura_recomp.*.cpp files found in {generated_dir}!")
        return

    for p in patches:
        applied = False
        already = False

        for f in all_files:
            content = f.read_text(encoding="utf-8")
            if p["new"] in content:
                print(f"[Patch] Already applied: {p['name']} in {f.name}")
                already = True
                break

            if p["old"] in content and (not p.get("context") or p["context"] in content):
                updated = content.replace(p["old"], p["new"], 1)
                f.write_text(updated, encoding="utf-8")
                print(f"[Patch] Applied: {p['name']} in {f.name}")
                applied = True
                break

        if not applied and not already:
            print(f"[Patch] Warning: Target signature not found anywhere for {p['name']}!")

if __name__ == "__main__":
    target = Path(__file__).resolve().parent.parent / "generated" / "default"
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    apply_patches(target)
