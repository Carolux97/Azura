#!/usr/bin/env python3
"""Checks a player kit before it is published (run by scripts/build-kit.sh, and in CI).

1. What it holds: nothing from a game or derived from one. No file of generated code (by name),
   no default.xex or .xex at all, no file whose SHA-256 is one of the game executables in
   versions.toml, no runtime executable. And on macOS, our MoltenVK: not the unpatched one whose
   SHA-256 versions.toml records.
2. That its commands work on this machine, the way the launcher will run them: a stand-in for
   generated code (the headers the generated precompiled header includes) is precompiled,
   compiled with that header, and linked with our compiled code and the SDK's libraries; the
   staged files are copied into a runtime folder. The symbols the player's generated code would
   define (guest functions, the image description) are stubbed out for the link; any other
   undefined symbol is an error, since the player's link would fail on it too.

Usage: check_kit.py --kit DIR --registry versions.toml --work DIR
"""

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Names of the files codegen writes; none may be in a kit.
GENERATED_NAME = re.compile(r"^(azura|ppc)_(recomp|register|init|funcs|pch|config|func_mapping)[._]")

# Symbols the generated code defines and our code uses: guest functions and their originals
# (sub_XXXXXXXX, __imp__sub_XXXXXXXX), the image description and its function table.
GENERATED_SYMBOL = re.compile(r"^_?(__imp__)?sub_[0-9A-F]{8}$|^_?PPCImageConfig$|^_?PPCFuncMappings$")

# The headers the generated precompiled header includes (the code generator's pch_h.inja).
PCH_HEADER = """#include <rex/chrono/clock.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>
#include <rex/system/mmio_handler.h>
#include <rex/thread/mutex.h>
#include <tracy/Tracy.hpp>
"""
PROBE_SOURCE = """void AzuraKitCheck(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
}
"""


class CheckError(Exception):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check_contents(kit: Path, registry: Path) -> None:
    game_hashes = set(re.findall(r'xex_sha256\s*=\s*"([0-9a-fA-F]{64})"', registry.read_text()))
    game_hashes = {value.lower() for value in game_hashes}
    moltenvk = re.search(r'\[moltenvk\][^\[]*?unpatched_sha256\s*=\s*"([0-9a-fA-F]{64})"', registry.read_text())
    unpatched_moltenvk = moltenvk.group(1).lower() if moltenvk else None
    for path in kit.rglob("*"):
        relative = path.relative_to(kit)
        if "generated" in relative.parts or "game" in relative.parts:
            raise CheckError(f"{relative}: a folder of generated code or game files")
        if not path.is_file():
            continue
        name = path.name
        if GENERATED_NAME.match(name) or name.lower().endswith(".xex"):
            raise CheckError(f"{relative}: generated code or a game executable")
        if name == "azura":
            raise CheckError(f"{relative}: a runtime executable, which holds the game's code")
        digest = sha256(path)
        if digest in game_hashes:
            raise CheckError(f"{relative}: a game executable listed in versions.toml")
        if name == "libMoltenVK.dylib" and digest == unpatched_moltenvk:
            raise CheckError(f"{relative}: the unpatched MoltenVK, without the project's fixes")


def compiler() -> list[str]:
    """The C++ compiler the launcher will use: Apple's through xcrun on macOS, clang++ elsewhere."""
    return ["xcrun", "clang++"] if platform.system() == "Darwin" else ["clang++"]


def fill(template: list[str], values: dict) -> list[str]:
    """A command template with its placeholders filled in; {cxx} and {objects} expand to lists."""
    out = []
    for token in template:
        if token in ("{cxx}", "{objects}"):
            out += values[token]
            continue
        for key, value in values.items():
            if isinstance(value, str):
                token = token.replace(key, value)
        out.append(token)
    return out


def run(command: list[str], what: str) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise CheckError(f"{what} failed:\n{' '.join(command)}\n{result.stdout}{result.stderr}")
    return result


def undefined_symbols(output: str) -> set[str]:
    """Undefined symbols from a failed link: Apple's ld ("_sym", referenced from) or GNU ld and
    lld (undefined reference to `sym', or undefined symbol: sym)."""
    found = set(re.findall(r'^\s+"([^"]+)", referenced from:', output, re.MULTILINE))
    found |= set(re.findall(r"undefined reference to `([^']+)'", output))
    found |= set(re.findall(r"undefined symbol: (\S+)", output))
    return found


def check_commands(kit: Path, work: Path) -> None:
    description = json.loads((kit / "kit.json").read_text())
    if work.exists():
        shutil.rmtree(work)
    generated = work / "generated"
    generated.mkdir(parents=True)
    (generated / "check_pch.h").write_text(PCH_HEADER)
    (generated / "check.cpp").write_text(PROBE_SOURCE)
    values = {"{cxx}": compiler(), "{kit}": str(kit.resolve()), "{generated}": str(generated),
              "{workdir}": str(work)}

    pch = work / "check_pch.pch"
    run(fill(description["compile"]["precompile"],
             {**values, "{header}": str(generated / "check_pch.h"), "{pch}": str(pch)}),
        "Precompiling the header with the kit's command")
    obj = work / "check.o"
    run(fill(description["compile"]["command"],
             {**values, "{pch}": str(pch), "{source}": str(generated / "check.cpp"), "{object}": str(obj)}),
        "Compiling with the kit's command")

    host = [str(kit / path) for path in description["link"]["host_objects"]]
    output = work / "out" / "azura_check"
    output.parent.mkdir()
    link = description["link"]["command"]
    first = subprocess.run(fill(link, {**values, "{objects}": [str(obj)] + host, "{output}": str(output)}),
                           capture_output=True, text=True)
    objects = [str(obj)] + host
    if first.returncode != 0:
        missing = undefined_symbols(first.stdout + first.stderr)
        unexpected = sorted(symbol for symbol in missing if not GENERATED_SYMBOL.match(symbol))
        if not missing or unexpected:
            raise CheckError("Linking with the kit's command failed on more than the generated code's "
                             f"symbols ({', '.join(unexpected) or 'no undefined symbols found'}):\n"
                             f"{first.stdout}{first.stderr}")
        stubs = work / "stubs.c"
        stubs.write_text("".join(
            f'void azura_kit_stub_{index}(void) __asm__("{symbol}");\n'
            f"void azura_kit_stub_{index}(void) {{}}\n" for index, symbol in enumerate(sorted(missing))))
        c_compiler = ["xcrun", "clang"] if platform.system() == "Darwin" else ["clang"]
        run(c_compiler + ["-c", str(stubs), "-o", str(work / "stubs.o")] +
            (["-arch", "arm64"] if platform.system() == "Darwin" else []), "Compiling the symbol stubs")
        objects.append(str(work / "stubs.o"))
        run(fill(link, {**values, "{objects}": objects, "{output}": str(output)}),
            "Linking with the generated code's symbols stubbed out")
    if not output.is_file():
        raise CheckError("the link produced no executable")

    for entry in description["stage"]:
        source = kit / entry["from"]
        target = output.parent / entry["to"]
        if not source.exists():
            raise CheckError(f"staged file missing from the kit: {entry['from']}")
        target.parent.mkdir(parents=True, exist_ok=True)
        if entry["kind"] == "directory":
            shutil.copytree(source, target, dirs_exist_ok=True)
        else:
            shutil.copy2(source, target)
    print(f"Kit commands work: compiled, linked ({len(host)} host objects) and staged "
          f"{len(description['stage'])} entries")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kit", required=True, type=Path)
    parser.add_argument("--registry", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    check_contents(args.kit, args.registry)
    print("Kit contents: nothing from a game")
    check_commands(args.kit, args.work.resolve())
    shutil.rmtree(args.work)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except CheckError as error:
        print(f"check_kit.py: {error}", file=sys.stderr)
        sys.exit(1)
