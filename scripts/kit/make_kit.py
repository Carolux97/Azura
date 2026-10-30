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
import platform
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

# On Windows Ninja runs its commands without a shell, chains them through cmd.exe, and CMake writes
# paths with either slash; the kit's commands and paths are kept with forward slashes there.
WINDOWS = platform.system() == "Windows"

# File names of objects and executables on this platform.
OBJECT_SUFFIX = ".obj" if WINDOWS else ".o"
EXECUTABLE_SUFFIX = ".exe" if WINDOWS else ""

# Version of the layout of kit.json; the launcher refuses a kit with a format it does not know.
KIT_FORMAT = 1


class KitError(Exception):
    pass


def response_file_contents(build: Path, config: str, target: str, rsp: str) -> str:
    """What the response file `rsp` of `target`'s command holds, on one line.

    Ninja writes a response file only while it runs the command, so the command is run once, with
    `-d keeprsp` so the file stays. The probe is not meant to link, and whether it does is not
    looked at: the file is written before the command runs."""
    subprocess.run(["ninja", "-C", str(build), "-f", f"build-{config}.ninja", "-d", "keeprsp", target],
                   capture_output=True, text=True)
    path = build / rsp
    if not path.is_file():
        raise KitError(f"Ninja did not write the response file {rsp} of {target}")
    return " ".join(path.read_text().split("\n"))


def ninja_command(build: Path, config: str, target: str) -> str:
    """The last command Ninja would run to build `target`, with any response file it uses replaced
    by what the file holds.

    CMake's Ninja generator moves a command's objects and libraries into a response file
    (`@file.rsp`) when the command would be longer than cmd.exe takes on Windows (8191 characters),
    whatever CMAKE_<LANG>_USE_RESPONSE_FILE_FOR_* say. The kit records the arguments themselves:
    the launcher runs the link without cmd.exe, and passes the objects in a response file of its
    own (launcher/src-tauri/src/builder.rs)."""
    result = subprocess.run(
        ["ninja", "-C", str(build), "-f", f"build-{config}.ninja", "-t", "commands", "-s", target],
        check=True, capture_output=True, text=True)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        raise KitError(f"Ninja has no command for {target}")
    command = lines[-1]
    for token in command.split():
        if token.startswith("@") and token.lower().endswith(".rsp"):
            command = command.replace(token, response_file_contents(build, config, target, token[1:]))
    return command


def split_windows(command: str) -> list[str]:
    """A Windows command line as its arguments, the way the C runtime splits it (CommandLineToArgvW):
    spaces separate them, double quotes group, and backslashes are literal unless they precede a
    double quote."""
    tokens, current, quoted, any_text = [], [], False, False
    index = 0
    while index < len(command):
        char = command[index]
        if char == "\\":
            run = len(command) - index - len(command[index:].lstrip("\\"))
            if index + run < len(command) and command[index + run] == '"':
                current.append("\\" * (run // 2))
                index += run
                if run % 2:
                    current.append('"')
                    index += 1
                any_text = True
                continue
            current.append("\\" * run)
            index += run
            any_text = True
            continue
        if char == '"':
            quoted = not quoted
            any_text = True
        elif char in " \t" and not quoted:
            if any_text:
                tokens.append("".join(current))
                current, any_text = [], False
        else:
            current.append(char)
            any_text = True
        index += 1
    if any_text:
        tokens.append("".join(current))
    return tokens


def command_tokens(command: str) -> list[str]:
    """The arguments of a command Ninja would run. On Windows a chain comes wrapped in
    `cmd.exe /C "..."`, whose inner command is split again, and every backslash becomes a slash
    (clang, lld-link and CMake take either)."""
    if not WINDOWS:
        return shlex.split(command)
    # As cmd.exe reads /C "...": the rest of the line, without its first and last double quote.
    wrapped = re.match(r'^\s*"?[^"\s]*cmd\.exe"?\s+/[cC]\s+(.*)$', command, re.DOTALL | re.IGNORECASE)
    if wrapped:
        inner = wrapped.group(1).strip()
        if len(inner) >= 2 and inner[0] == '"' and inner[-1] == '"':
            inner = inner[1:-1]
        command = inner
    return [token.replace("\\", "/") for token in split_windows(command)]


def program_name(path: str) -> str:
    """The name of the program `path` runs, without .exe."""
    name = Path(path).name
    return name[:-4] if name.lower().endswith(".exe") else name


def segments(command: str) -> list[list[str]]:
    """A shell command chained with && as its parts, each as a list of arguments."""
    parts, current = [], []
    for token in command_tokens(command):
        # CMake writes "\$ORIGIN" (the rpath) for the shell, which drops the backslash inside
        # double quotes; shlex keeps it. The kit's commands run without a shell, so drop it here.
        token = token.replace("\\$", "$")
        if token == "&&":
            parts.append(current)
            current = []
        else:
            current.append(token)
    parts.append(current)
    parts = [part for part in parts if part and part != [":"]]
    if WINDOWS:
        parts = [sub for part in parts for sub in nested_cmd(part)]
    return parts


def nested_cmd(part: list[str]) -> list[list[str]]:
    """On Windows the commands after a link come as one more `cmd.exe /C "..."` inside the link's
    chain, whose inner double quotes (around "C:/Program Files/CMake/bin/cmake.exe") the outer
    split has already taken apart. Its words, joined again, are split at each `&&`, and each command
    into its program (a path that may hold spaces, up to cmake.exe) and its arguments."""
    if len(part) < 3 or program_name(part[0]).lower() != "cmd" or part[1].lower() != "/c":
        return [part]
    commands = []
    for command in " ".join(part[2:]).split(" && "):
        command = command.strip().strip('"').strip()
        if not command:
            continue
        program = re.match(r"^(.*?cmake(?:\.exe)?)\s+(-E\s.*)$", command, re.IGNORECASE)
        if program:
            commands.append([program.group(1).strip('"')] + program.group(2).split())
        else:
            commands.append(command.split())
    return [command for command in commands if command != [":"]]


class Paths:
    """Turns the paths of this machine into kit placeholders."""

    def __init__(self, sdk: Path, runtime: Path, probe_dir: Path, forbidden: list[Path]):
        self.sdk = sdk.resolve().as_posix()
        self.runtime = runtime.resolve().as_posix()
        self.probe_dir = probe_dir.resolve().as_posix()
        self.forbidden = [path.resolve().as_posix() for path in forbidden]

    @staticmethod
    def same(a: str, b: str) -> bool:
        """Whether two paths are the same; on Windows whatever their case (the drive letter, above
        all, which CMake and Python may write differently)."""
        return a.lower() == b.lower() if WINDOWS else a == b

    @staticmethod
    def replace(token: str, path: str, placeholder: str) -> str:
        """`token` with every `path` in it replaced, ignoring case on Windows."""
        return re.sub(re.escape(path), lambda _: placeholder, token, flags=re.IGNORECASE if WINDOWS else 0)

    def inside(self, path: str, folder: str) -> "str | None":
        """`path` relative to `folder` when it is inside it, else None."""
        prefix = folder + "/"
        if (path.lower().startswith(prefix.lower()) if WINDOWS else path.startswith(prefix)):
            return path[len(prefix):]
        return None

    def token(self, token: str) -> str:
        suffix = f"-prefix-map={self.runtime}=."
        if self.same(token[-len(suffix):], suffix):
            return self.replace(token, self.runtime, "{workdir}")
        if self.same(token, f"-I{self.probe_dir}"):
            return "-I{generated}"
        return self.replace(token, self.sdk, "{kit}/sdk")

    def check(self, tokens: list[str], what: str) -> None:
        for token in tokens:
            for path in self.forbidden:
                if (path.lower() in token.lower()) if WINDOWS else (path in token):
                    raise KitError(f"{what} still refers to {path}: {token}")


def without_missing_search_paths(tokens: list[str]) -> list[str]:
    """The arguments without search paths (-L, -I, -isystem) to folders that do not exist on this
    machine. A preset may add one for an optional folder (the Linux presets add sdk/compat/lib for
    libraries a distribution lacks); where it is missing it adds nothing, and it would only carry
    this machine's path into the kit."""
    out, skip = [], False
    for index, token in enumerate(tokens):
        if skip:
            skip = False
            continue
        if token == "-isystem" and index + 1 < len(tokens) and not Path(tokens[index + 1]).is_dir():
            skip = True
            continue
        if (token.startswith("-L") or token.startswith("-I")) and len(token) > 2 and not Path(token[2:]).is_dir():
            continue
        out.append(token)
    return out


def compile_template(command: str, paths: Paths, source: Path) -> list[str]:
    """The compile command of the probe source, without its input and output: the options every
    file of generated code compiles with."""
    (tokens,) = segments(command)
    tokens = without_missing_search_paths(tokens)
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
    if source.resolve().as_posix() in " ".join(out):
        raise KitError("the probe source is still in the compile command")
    paths.check(out, "The compile command")
    return out


def link_template(command: str, paths: Paths, output: Path) -> tuple[list[str], list[list[str]]]:
    """The link command of the probe executable as a template, and the commands chained after it."""
    parts = segments(command)
    # On Windows the chain starts in the build folder ("cd ."), before the link itself.
    while parts and program_name(parts[0][0]) == "cd":
        parts = parts[1:]
    link, after = without_missing_search_paths(parts[0]), parts[1:]
    out = ["{cxx}"]
    objects_at = None
    skip = 0
    for index, token in enumerate(link[1:], start=1):
        if skip:
            skip -= 1
            continue
        if token == "-o":
            out += ["-o", "{output}"]
            skip = 1
            continue
        # lld-link options for the probe's own files: its import library is not needed (nothing
        # links against the runtime), and its debug information goes beside the player's
        # executable.
        if token == "-Xlinker" and index + 1 < len(link):
            option = link[index + 1]
            if option.lower().startswith("/implib:"):
                skip = 1
                continue
            if option.lower().startswith("/pdb:"):
                out += ["-Xlinker", "/pdb:{output}.pdb"]
                skip = 1
                continue
        if token.endswith(OBJECT_SUFFIX):
            if objects_at is None:
                objects_at = len(out)
                out.append("{objects}")
            continue
        out.append(paths.token(token))
    if objects_at is None:
        raise KitError(f"the link command has no objects:\n{command}")
    paths.check(out, "The link command")
    return out, after


def stage_entries(after: list[list[str]], kit: Path, paths: Paths, output_dir: Path) -> list[tuple[str, str, str]]:
    """The files the commands after the link copy beside the executable, as (kind, from, to):
    `from` relative to the kit, `to` relative to the runtime folder. Our own files (settings and
    assets) are copied into the kit's files/ folder."""
    output_dir = output_dir.resolve().as_posix()
    entries = []

    def source(path: str) -> str:
        resolved = Path(path).resolve().as_posix()
        if (relative := paths.inside(resolved, paths.sdk)) is not None:
            return "sdk/" + relative
        if (relative := paths.inside(resolved, paths.runtime)) is not None:
            target = kit / "files" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if Path(resolved).is_dir():
                shutil.copytree(resolved, target, dirs_exist_ok=True)
            else:
                shutil.copy2(resolved, target)
            return "files/" + relative
        raise KitError(f"a staged file comes from outside the SDK and the runtime: {path}")

    def destination(path: str) -> str:
        resolved = Path(path).resolve().as_posix() if Path(path).is_absolute() else path
        if paths.same(resolved, output_dir):
            return "."
        relative = paths.inside(resolved, output_dir)
        if relative is None:
            raise KitError(f"a file is staged outside the runtime folder: {path}")
        return relative

    for part in after:
        program = program_name(part[0])
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
    recomp_object = f"CMakeFiles/azura_kit_recomp.dir/{args.config}/tools/kit/recomp_probe.cpp{OBJECT_SUFFIX}"
    compile_options = compile_template(ninja_command(build, args.config, recomp_object), paths, probe_source)
    # Generated code is compiled with its precompiled header, as CMake builds it; the header is
    # precompiled with the same options.
    precompile_command = compile_options + ["-x", "c++-header", "{header}", "-o", "{pch}"]
    compile_command = compile_options + ["-include-pch", "{pch}", "-c", "{source}", "-o", "{object}"]
    output = build / args.config / f"azura_kit_probe{EXECUTABLE_SUFFIX}"
    link_command, after = link_template(ninja_command(build, args.config, f"{args.config}/azura_kit_probe{EXECUTABLE_SUFFIX}"),
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
    host_objects = sorted(host_dir.rglob(f"*{OBJECT_SUFFIX}"))
    if not host_objects:
        raise KitError(f"no objects of azura_host in {host_dir}: build the azura_host target first")
    for obj in host_objects:
        target = kit / "host" / obj.relative_to(host_dir)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(obj, target)
    host_paths = ["host/" + obj.relative_to(host_dir).as_posix() for obj in host_objects]

    # The SDK: headers, the code generator, and what the commands use.
    sdk = kit / "sdk"
    shutil.copytree(args.sdk / "include", sdk / "include")
    # The executable itself: on Windows its debug information (rexglue.pdb) is beside it.
    generator = next((p for p in sorted((args.sdk / "bin").iterdir())
                      if p.name.startswith("rexglue") and p.suffix.lower() in ("", ".exe")), None)
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
