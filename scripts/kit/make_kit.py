#!/usr/bin/env python3
"""Assembles the player kit from a probe build (run by scripts/build-kit.sh).

The player's machine builds the runtime from their own default.xex with only a C++ compiler: no
CMake, no Ninja, no SDK source (docs/player-runtime-build.md). Everything else ships in the kit,
and so do the commands that build it, which CMake computes here, on a machine that has CMake:

- runtime/CMakeLists.txt, configured with AZURA_KIT_PROBE against the patched SDK as installed,
  has two probes that are never built: azura_kit_recomp, one source configured exactly as the
  generated code is, and azura_kit_probe, an executable configured exactly as azura is. Ninja
  prints the commands it would run for them (`ninja -t commands`).
- Those commands become templates in kit.json, with the paths of this machine turned into
  placeholders the launcher fills in: {cxx} the compiler, {kit} the kit folder, {generated} the
  folder of generated code, {workdir} the folder codegen ran in, {source} and {object} the file
  being compiled, {objects} every object to link, {output} the executable.
- The commands after the link, which stage the runtime folder (the SDK's libraries and GPU
  plugin, MoltenVK, our settings and assets), become a list of files to copy.
- Our code ships compiled: the objects of azura_host.

Of the installed SDK the kit keeps its headers, the code generator, and the libraries and files
the commands use. Any path the templates still hold that is not one of the placeholders stops the
script: a kit that only works on the machine that made it is worse than none.

Usage: make_kit.py --probe-build DIR --config NAME --sdk DIR --runtime DIR --root DIR --out DIR
                   --platform NAME --revision REV
"""

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

# Version of the layout of kit.json; the launcher refuses a kit with a format it does not know.
KIT_FORMAT = 1


class KitError(Exception):
    pass


def ninja_command(build: Path, config: str, target: str) -> str:
    """The last command Ninja would run to build `target`."""
    result = subprocess.run(
        ["ninja", "-C", str(build), "-f", f"build-{config}.ninja", "-t", "commands", "-s", target],
        check=True, capture_output=True, text=True)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        raise KitError(f"Ninja has no command for {target}")
    return lines[-1]


def segments(command: str) -> list[list[str]]:
    """A shell command chained with && as its parts, each as a list of arguments."""
    parts, current = [], []
    for token in shlex.split(command):
        if token == "&&":
            parts.append(current)
            current = []
        else:
            current.append(token)
    parts.append(current)
    return [part for part in parts if part and part != [":"]]


class Paths:
    """Turns the paths of this machine into kit placeholders."""

    def __init__(self, sdk: Path, runtime: Path, probe_dir: Path, forbidden: list[Path]):
        self.sdk = str(sdk.resolve())
        self.runtime = str(runtime.resolve())
        self.probe_dir = str(probe_dir.resolve())
        self.forbidden = [str(path.resolve()) for path in forbidden]

    def token(self, token: str) -> str:
        if token.endswith(f"-prefix-map={self.runtime}=."):
            return token.replace(self.runtime, "{workdir}")
        if token == f"-I{self.probe_dir}":
            return "-I{generated}"
        return token.replace(self.sdk, "{kit}/sdk")

    def check(self, tokens: list[str], what: str) -> None:
        for token in tokens:
            for path in self.forbidden:
                if path in token:
                    raise KitError(f"{what} still refers to {path}: {token}")


def compile_template(command: str, paths: Paths, source: Path) -> list[str]:
    """The compile command of the probe source, without its input and output: the options every
    file of generated code compiles with."""
    (tokens,) = segments(command)
    out = ["{cxx}"]
    skip = 0
    for index, token in enumerate(tokens[1:], start=1):
        if skip:
            skip -= 1
            continue
        if token in ("-MT", "-MF"):
            skip = 1
            continue
        if token == "-MD":
            continue
        if token in ("-o", "-c"):
            skip = 1
            continue
        out.append(paths.token(token))
    if str(source.resolve()) in " ".join(out):
        raise KitError("the probe source is still in the compile command")
    paths.check(out, "The compile command")
    return out


def link_template(command: str, paths: Paths, output: Path) -> tuple[list[str], list[list[str]]]:
    """The link command of the probe executable as a template, and the commands chained after it."""
    parts = segments(command)
    link, after = parts[0], parts[1:]
    out = ["{cxx}"]
    objects_at = None
    skip = 0
    for token in link[1:]:
        if skip:
            skip -= 1
            continue
        if token == "-o":
            out += ["-o", "{output}"]
            skip = 1
            continue
        if token.endswith(".o"):
            if objects_at is None:
                objects_at = len(out)
                out.append("{objects}")
            continue
        out.append(paths.token(token))
    if objects_at is None:
        raise KitError("the link command has no objects")
    paths.check(out, "The link command")
    return out, after


def stage_entries(after: list[list[str]], kit: Path, paths: Paths, output_dir: Path) -> list[tuple[str, str, str]]:
    """The files the commands after the link copy beside the executable, as (kind, from, to):
    `from` relative to the kit, `to` relative to the runtime folder. Our own files (settings and
    assets) are copied into the kit's files/ folder."""
    output_dir = str(output_dir.resolve())
    entries = []

    def source(path: str) -> str:
        resolved = str(Path(path).resolve())
        if resolved.startswith(paths.sdk + "/"):
            return "sdk/" + resolved[len(paths.sdk) + 1:]
        if resolved.startswith(paths.runtime + "/"):
            relative = resolved[len(paths.runtime) + 1:]
            target = kit / "files" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if Path(resolved).is_dir():
                shutil.copytree(resolved, target, dirs_exist_ok=True)
            else:
                shutil.copy2(resolved, target)
            return "files/" + relative
        raise KitError(f"a staged file comes from outside the SDK and the runtime: {path}")

    def destination(path: str) -> str:
        resolved = str(Path(path).resolve()) if path.startswith("/") else path
        if resolved == output_dir:
            return "."
        if not resolved.startswith(output_dir + "/"):
            raise KitError(f"a file is staged outside the runtime folder: {path}")
        return resolved[len(output_dir) + 1:]

    for part in after:
        program = Path(part[0]).name
        if program == "cd" or program == "osascript":
            # cd only sets the folder of the next commands; osascript sets the Finder icon of the
            # executable file, which the runtime does not need.
            continue
        if program != "cmake" or len(part) < 3 or part[1] != "-E":
            raise KitError(f"unknown command after the link: {' '.join(part)}")
        action = part[2]
        if action == "make_directory":
            continue
        if action in ("copy_if_different", "copy"):
            # As cmake -E: with several sources, or a folder as the target, each file keeps its
            # name inside the folder; otherwise the target is the file's new path.
            *sources, target = part[3:]
            to = destination(target)
            into_folder = len(sources) > 1 or to == "." or Path(target).is_dir()
            for item in sources:
                name = Path(item).name
                path = (name if to == "." else f"{to}/{name}") if into_folder else to
                entries.append(("file", source(item), path))
            continue
        if action == "copy_directory":
            entries.append(("directory", source(part[3]), destination(part[4])))
            continue
        raise KitError(f"unknown cmake -E command after the link: {' '.join(part)}")
    return entries


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_hash(paths: list[Path], base: Path) -> str:
    """One hash over files and their paths relative to `base`, in a fixed order."""
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda p: str(p.relative_to(base))):
        digest.update(str(path.relative_to(base)).encode())
        digest.update(file_hash(path).encode())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    for name in ("probe-build", "sdk", "runtime", "root", "out"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    for name in ("config", "platform", "revision"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()

    kit = args.out
    if kit.exists():
        shutil.rmtree(kit)
    kit.mkdir(parents=True)
    build = args.probe_build
    probe_source = args.runtime / "tools/kit/recomp_probe.cpp"
    paths = Paths(args.sdk, args.runtime, args.runtime / "tools/kit",
                  forbidden=[args.root, build, args.runtime])

    # Compile and link commands, and what is staged beside the executable.
    recomp_object = f"CMakeFiles/azura_kit_recomp.dir/{args.config}/tools/kit/recomp_probe.cpp.o"
    compile_options = compile_template(ninja_command(build, args.config, recomp_object), paths, probe_source)
    # Generated code is compiled with its precompiled header, as CMake builds it; the header is
    # precompiled with the same options.
    precompile_command = compile_options + ["-x", "c++-header", "{header}", "-o", "{pch}"]
    compile_command = compile_options + ["-include-pch", "{pch}", "-c", "{source}", "-o", "{object}"]
    output = build / args.config / "azura_kit_probe"
    link_command, after = link_template(ninja_command(build, args.config, f"{args.config}/azura_kit_probe"),
                                        paths, output)
    stage = stage_entries(after, kit, paths, output.parent)
    # The MoltenVK staging (macOS) is a custom target; its command is the output of that name.
    moltenvk_target = f"CMakeFiles/azura_kit_probe_stage_moltenvk-{args.config}"
    targets = subprocess.run(["ninja", "-C", str(build), "-f", f"build-{args.config}.ninja", "-t", "targets", "all"],
                             check=True, capture_output=True, text=True).stdout
    if f"\n{moltenvk_target}:" in "\n" + targets:
        stage += stage_entries(segments(ninja_command(build, args.config, moltenvk_target)), kit, paths,
                               output.parent)

    # Our code, compiled.
    host_dir = build / "CMakeFiles/azura_host.dir" / args.config
    host_objects = sorted(host_dir.rglob("*.o"))
    if not host_objects:
        raise KitError(f"no objects of azura_host in {host_dir}: build the azura_host target first")
    for obj in host_objects:
        target = kit / "host" / obj.relative_to(host_dir)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(obj, target)
    host_paths = ["host/" + str(obj.relative_to(host_dir)) for obj in host_objects]

    # The SDK: headers, the code generator, and what the commands use.
    sdk = kit / "sdk"
    shutil.copytree(args.sdk / "include", sdk / "include")
    generator = next((p for p in (args.sdk / "bin").iterdir() if p.name.startswith("rexglue")), None)
    if generator is None:
        raise KitError(f"no code generator in {args.sdk / 'bin'}")
    (sdk / "bin").mkdir()
    shutil.copy2(generator, sdk / "bin" / generator.name)
    used = {token[len("{kit}/sdk/"):] for token in link_command if token.startswith("{kit}/sdk/")}
    used |= {entry[1][len("sdk/"):] for entry in stage if entry[1].startswith("sdk/")}
    for relative in sorted(used):
        source = args.sdk / relative
        target = sdk / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, dirs_exist_ok=True)
        else:
            shutil.copy2(source, target, follow_symlinks=True)
    if (args.sdk / "share/licenses").is_dir():
        shutil.copytree(args.sdk / "share/licenses", sdk / "share/licenses")

    # The manifests of the supported builds, and the licenses of what the kit holds.
    (kit / "manifests").mkdir()
    for manifest in sorted(args.runtime.glob("azura_*.toml")):
        if manifest.is_file() and not manifest.is_symlink():
            shutil.copy2(manifest, kit / "manifests" / manifest.name)
    (kit / "licenses").mkdir()
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(args.root / name, kit / "licenses" / name)

    # Keys for updates (phase 6): what the generated code depends on, and what only the link does.
    build_inputs = [sdk / "bin" / generator.name] + sorted((sdk / "include").rglob("*")) + \
        sorted((kit / "manifests").glob("*.toml"))
    build_key = hashlib.sha256((tree_hash([p for p in build_inputs if p.is_file()], kit) +
                                "\n".join(precompile_command + compile_command)).encode()).hexdigest()
    link_inputs = [kit / path for path in host_paths] + [p for p in sdk.rglob("*")
                                                         if p.is_file() and "include" not in p.relative_to(sdk).parts[:1]
                                                         and p.parent.name != "bin"]
    link_inputs += [p for p in (kit / "files").rglob("*") if p.is_file()]
    link_key = hashlib.sha256((tree_hash(link_inputs, kit) + "\n".join(link_command)).encode()).hexdigest()

    description = {
        "format": KIT_FORMAT,
        "platform": args.platform,
        "revision": args.revision,
        "config": args.config,
        "keys": {"build": build_key, "link": link_key},
        "codegen": {"generator": "sdk/bin/" + generator.name, "manifests": "manifests"},
        "compile": {"precompile": precompile_command, "command": compile_command},
        "link": {"command": link_command, "host_objects": host_paths},
        "stage": [{"kind": kind, "from": source, "to": target} for kind, source, target in stage],
    }
    (kit / "kit.json").write_text(json.dumps(description, indent=2) + "\n")
    print(f"Kit for {args.platform} in {kit}: {len(host_paths)} host objects, {len(stage)} staged entries")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KitError, subprocess.CalledProcessError) as error:
        print(f"make_kit.py: {error}", file=sys.stderr)
        sys.exit(1)
