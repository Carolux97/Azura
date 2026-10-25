# Building the runtime on the player's machine: proposal

Card 33. How a player who downloads the launcher ends up with the game installed and running with
every fix the project makes, and keeps getting the new ones, when the recompiled game cannot be
distributed. This is the proposal; implementing it is one card, in the phases of section 11.

## 1. The constraint

The runtime is the game's PowerPC code translated to C++ (`rexglue codegen`), compiled and linked
with the ReXGlue SDK and our own code. The translated code is derived from the player's
`default.xex`, so it is generated on the player's machine and never published. Everything else is
ours or open source and can be shipped.

Today the launcher only copies a runtime someone already built with `scripts/build.sh`
(`launcher/src-tauri/src/install.rs`, which looks for `AZURA_RUNTIME_BUILD` or the project's
`runtime/out/build` folder). A launcher downloaded from a release has nothing to copy.

## 2. Where each fix lives

What a fix changes decides whether it can ship prebuilt or has to be redone on the player's machine:

| Fix | Where it ends up | Holds game code | A new one needs |
| --- | --- | --- | --- |
| SDK patches (Metal, presenter, kernel...) | `librexruntime`, `librexgpu-xenos`, SDK headers | No | new libraries; recompiling the game only if a header the generated code includes changed |
| Patched MoltenVK | `libMoltenVK.dylib` | No | the new library |
| Our code (hooks, menus, achievements, prompts...) | `runtime/src`, linked into `azura` | No | relinking |
| Code generator patches | the C++ generated from the `.xex` | The output does | regenerating and recompiling |
| The build's manifest (`runtime/azura_<media ID>.toml`) | which functions are generated | The output does | regenerating and recompiling |
| Linux fixes to the generated code (`runtime/scripts/patch_game_code.py`) | the generated C++ | Yes | applying them again and recompiling |
| Settings and assets (`azura.default.toml`, `assets/`) | files beside the executable | No | the new files |

## 3. Measurements (macOS)

MacBook Pro, base M4 (10 cores), the Spain copy (World build, media ID 58122AD6), Apple clang 21.

From `scripts/build.sh` today, from scratch but with MoltenVK already built (Ninja log of the build):
the SDK compiles in 79 s (1322 s of CPU), codegen takes 93 s in parallel with it, the generated code
compiles in 92 s, our code in 6 s and the link under 1 s. The build folder takes 1.4 GB, the SDK
source tree 912 MB.

The proposal (section 5) was prototyped: a kit of only distributable pieces was packed from that
build, and a script did what the player's machine would do, with the kit, the `.xex` and the
compiler alone (no CMake, Ninja or SDK source):

| Phase | Time | Disk |
| --- | --- | --- |
| Codegen | 29.1 s | 234 MB of generated C++ |
| Split the function registration (`split_register.py`) | 0.1 s | |
| Precompiled header | 1.4 s | 41 MB |
| Compile the 226 generated files (10 in parallel) | 91.5 s | 216 MB of objects (with the header) |
| Link and stage the runtime folder | 0.3 s | 119 MB |
| **Total** | **about 2 min** | **about 570 MB at the peak** |

The kit took 81 MB (RelWithDebInfo libraries, not stripped). The executable it produced is
91.2 MB, the size of the normal build's. Checked with it: `--export_achievements` read the 50
achievements, and the game started and loaded for 50 seconds with the same log as a normal start
(the picture was not looked at). Compiling and linking work with the Command Line Tools alone
(`DEVELOPER_DIR=/Library/Developer/CommandLineTools`), without Xcode. The Command Line Tools take
1.9 GB installed; their download is about 700 MB according to a third-party guide (not an Apple
figure).

Not measured: Linux (section 10), and machines with fewer cores, where the compile phase should
grow roughly with the core count (an extrapolation).

## 4. Options

1. **Ship a prebuilt kit and compile only the generated code on the player's machine**
   (recommended, section 5). About 2 minutes on the M4; needs a C++ compiler (installed by the
   launcher when missing, section 6); updates of our code only relink.
2. **The same with CMake and Ninja shipped**, reusing the current build files. More tools to ship
   and keep working on every machine, for nothing the launcher cannot do itself: the build is one
   command per file and one link.
3. **Run `scripts/build.sh` from the launcher.** Needs git, CMake, Ninja, the SDK source (912 MB),
   applying the patches and building MoltenVK; 1.4 GB of build folder; every step is one more thing
   that can fail on a machine we do not control.
4. **Ship a compiler with the launcher.** Possible later on Linux, for machines without clang
   (section 6). On macOS linking needs the macOS SDK, which comes with Apple's Command Line Tools
   or Xcode (not checked whether it may be redistributed; the Command Line Tools are free, so there
   is no reason to try).

Rejected:

- **The game as a library loaded by a shipped executable.** Our hooks replace game functions (for
  example `sub_822DF4D0` in `runtime/src/script_hooks.cpp`, which still calls the original through
  `__imp__sub_822DF4D0`), and the generated code calls our mid-function hooks. Inside one executable
  the linker resolves both ways; across a library boundary on macOS the library's calls to its own
  functions do not go through our replacements.
- **Running the whole game in the PowerPC interpreter.** It is a fallback for functions codegen
  misses (`docs/ppc-interpreter-design.md`); running everything interpreted would be far too slow
  (an inference, not measured).
- **Shipping a compiled game.** It is derived from the game.

## 5. The proposal

### What ships with the launcher

A kit per platform (`scripts/build-kit.sh`, built by the Runtime kit workflow), bundled with the
launcher package, so the launcher's updater brings new fixes. About 123 MB on macOS:

- the patched SDK **as installed**: built on its own with its own presets, as the SDK builds its
  releases (macOS 13.3 as the oldest system; Linux in Ubuntu 22.04 with clang 20). From it the kit
  keeps the headers, the code generator, and the libraries and files the commands below use (on
  macOS the patched MoltenVK and the Vulkan loader among them);
- our code compiled: the objects of `azura_host` (our `runtime/src` and the SDK's host sources), an
  object library so hooks that replace weak generated functions are always linked;
- the manifests (`runtime/azura_*.toml`), our settings and assets, and the licenses;
- `kit.json`: the kit's format, platform and revision, the two keys of section 7, and the commands
  that build the runtime, with placeholders the launcher fills in (`{cxx}`, `{kit}`, `{generated}`,
  `{workdir}`, `{header}`, `{pch}`, `{source}`, `{object}`, `{objects}`, `{output}`): precompile
  the generated header, compile a generated file, link; and the files to stage beside the
  executable.

The commands are not written by hand. `runtime/CMakeLists.txt`, configured with `AZURA_KIT_PROBE`
against the installed SDK, has two probes that are never built: one source configured exactly as
generated code is (through the SDK's `rexglue_add_recomp_library`, which codegen's CMake uses too)
and an executable configured exactly as `azura` is. `scripts/kit/make_kit.py` records the commands
Ninja would run for them and turns the machine's paths into placeholders; any path it cannot turn
stops it. `scripts/kit/check_kit.py` then checks the kit (section 9).

Our code compiles without generated code: `azura_app.h` declares `PPCImageConfig` instead of taking
it from a generated header, which was the only use of one in `runtime/src`.

### What the launcher does

After the game folder is checked (`game_folder.rs` already identifies the build by its SHA-256),
`src-tauri/src/builder.rs`:

1. **Preparing.** The compiler (section 6), the kit (its format and platform), the game build's
   manifest in the kit, and 1 GB of free space.
2. **Translating.** The kit's code generator, in a work folder under the app data folder
   (`build/<game build>/`). The generator itself writes the function registration in small
   functions, and the Linux engine fix is a pair of manifest hooks, so nothing edits the generated
   code afterwards and the player needs no Python.
3. **Compiling.** The precompiled header, then the generated files, one compiler process per core.
4. **Linking** with our objects and the SDK libraries, and staging the files, into a new folder.
5. **Installing.** The new folder replaces the runtime folder by renames, keeping the player's
   `azura.toml` and logs. An install that was interrupted is finished when the launcher starts
   again.

The work folder keeps the objects (216 MB) for updates; the generated C++ (234 MB) is deleted after
each build, since codegen writes it again when needed.

### The game: a folder or a disc image

The player picks either the folder of their dump or its disc image (`.iso`), or drops either on
the Installation view. A disc image is used in place, never extracted:

- **The check.** `disc_image.rs` reads the image's file system (XDVDFS: the volume descriptor at
  sector 32 of the game partition, then directory tables that are binary trees of entries), at the
  same partition offsets the SDK's `DiscImageDevice` tries, so it takes both an image of the game
  partition alone and a full dump of the disc. `game_folder.rs` then runs the same checks as for
  a folder on the files read from it: the XEX2 header, the title ID, the SHA-256 of `default.xex`
  against `versions.toml`, and the languages. `versions.toml` needs nothing new, since the hash is
  the executable's.
- **The build.** Codegen reads only `game/default.xex`: for a disc image, `game/` in the work
  folder holds that file copied out of the image (about 20 MB) instead of being a link to the
  game folder.
- **The runtime.** `game_data_root` (the launcher passes it as `--game_data_root`) may name the
  image. An SDK patch makes `Runtime::SetupVfs` mount a file with the SDK's `DiscImageDevice`,
  which maps the image and reads the game's files from it, and a folder with `HostPathDevice` as
  before; `ReXApp::ConstructRuntime` accepts either. The achievement export takes the image too.

Extracting the image to a folder was set aside: it costs the game's size again on disk (7.3 GB for
this game) and minutes of copying, for a runtime that can read the image as the console read the
disc. Reading a disc image breaks no protection: it is the game's own file system, and
`default.xex` is processed exactly as from a folder.

### Progress

While the runtime is built, a card under the button shows the step, a bar with the percentage of
the whole build (each step takes the share of the bar it takes of a first build) and the time
left, from the pace of the files compiled. On failure it says why, with Try again and Show the log.
The button says Install, Installing or Update, and turns to Play when the build is done, without
starting the game. A build that is interrupted resumes with the objects already built.

## 6. Installing the compiler when it is missing

The launcher checks before building and, when the compiler is missing, installs it itself, asking
the player only what the operating system requires:

- **macOS.** Present when `xcode-select -p` succeeds and `xcrun --find clang++` returns a compiler.
  Otherwise the launcher runs `xcode-select --install`, which opens Apple's own dialog: the player
  clicks Install and accepts Apple's license there (the launcher cannot do that for them). The
  launcher checks every few seconds until the compiler is there, or until Apple's installer has
  closed without installing it, and then goes on by itself or offers to open it again. `xcrun`
  runs only once `xcode-select -p` names a folder, since on a Mac without the tools it would open
  Apple's dialog by itself.
- **Linux.** Present when the newest `clang++` or `clang++-N` on the PATH is version 18 or newer
  (C++23; the SDK's own builds use 20) and compiles and links a small C++23 program. Otherwise the
  launcher reads `/etc/os-release` and installs the packages with the distribution's package
  manager through `pkexec`, so the system asks for the password in its own dialog:
  - Debian, Ubuntu and derivatives: `apt-get install -y clang build-essential`;
  - Fedora: `dnf install -y clang gcc-c++`;
  - Arch and derivatives: `pacman -S --needed --noconfirm clang base-devel`;
  - openSUSE: `zypper install -y clang gcc-c++`.

  Image-based systems (ostree ones such as Bazzite, and SteamOS) cannot install packages that way
  and get manual instructions instead (`rpm-ostree`, or a distrobox container). A compiler
  downloaded by the launcher (option 4) waits until it is known what those systems have
  (section 10).
- **Windows.** Present when `clang++.exe` (on the PATH, or in `Program Files\LLVM\bin`, where
  LLVM's installer puts it) is version 18 or newer and compiles and links the same program, which
  needs Microsoft's C++ headers and libraries: clang finds those of Visual Studio or its Build
  Tools by itself. Otherwise the launcher runs `winget install --id LLVM.LLVM --exact` and
  `winget install --id Microsoft.VisualStudio.2022.BuildTools --exact` with the C++ workload, each
  in its own console window, where the player accepts the packages' agreements and Windows asks
  for permission to install.

The player can also install the compiler by hand; the launcher shows the exact command for their
system and a "Check again" button.

## 7. Updates

Each object of the generated code has a key beside it: a hash of its source, the compile command,
the compiler's version (through the precompiled header's key, since a compiler rejects a
precompiled header another version wrote) and every generated header the compiler reported it
used (`-MD`, as Ninja tracks them). A build compiles only the files whose key changed.

`build/<game build>/state.json` records the kit's two keys from `kit.json`, the game executable's
hash, the compiler and the objects the installed runtime was linked from:

- **Build key:** the code generator, the headers, the manifests and the compile commands. When it
  changes, or the game or the compiler changes, the runtime is built again: codegen, then only the
  files whose key changed (about 2 minutes for a full build on the M4).
- **Link key:** our objects, the SDK libraries, the staged files and the link command. When only it
  changes and the kept objects are there, they are linked again: 0.4 s on the M4. Most fixes are of
  this kind: our code and the SDK patches that do not touch the headers of the generated code.

The launcher compares both at every start. The Installation view says which kind an update is
(seconds or about two minutes), and Play says Update until it is installed. A game folder with
another supported build gets its own work folder.

## 8. Failures

| Failure | What the launcher does |
| --- | --- |
| No compiler | Installs it (section 6); without it, explains and offers the manual command. |
| Unsupported game build | Already shown by the folder check (`unsupportedVersion`); no build starts. |
| Not enough disk | Checks for about 1 GB free before starting and says how much is needed. |
| Build interrupted (closed, crashed, power off) | The old runtime stays installed; the next start resumes with the objects already built. |
| A file fails to compile or the link fails | Keeps the full log in the work folder, shows the first error, offers to retry and to open the log for a bug report. |
| Codegen fails | Same; the most likely cause is a damaged `.xex`, which the folder check catches first by its hash. |

## 9. What is published, and checking it

Published: the launcher and the kit (the open source SDK as installed, our compiled code, the
manifests and the build commands). The manifests hold addresses and options for the
code generator, not game code. Never published: the generated C++, the objects compiled from it,
the linked executable, and any file from the game.

CI checks every kit before keeping it (`scripts/kit/check_kit.py`): no generated file by name, no
`.xex`, no runtime executable, none of the game executables' hashes from `versions.toml`, and not
the unpatched MoltenVK. It also checks that the commands work: a stand-in for generated code is
precompiled, compiled and linked with our objects and the SDK's libraries, with only the generated
code's own symbols stubbed, and the staged files are copied.

## 10. Still to check

- **Linux** (Juan Llorca Martínez): the same measurements on a Linux machine, the minimum clang
  version the generated code needs (18 is assumed from C++23), whether the packages of section 6
  are enough on each distribution, and whether Bazzite and SteamOS can compile with a downloaded
  LLVM and the system headers.
- **Slower machines:** the build time on a Mac with fewer cores.
- **Release libraries:** the kit size with Release builds stripped of debug information (the kit is
  123 MB in RelWithDebInfo, the configuration the project tests).

## 11. Implementation

Card 72 built phases 1 to 6 in one pull request
([#49](https://github.com/Carolux97/Gears3-Azura/pull/49)), each phase in its own commits:

1. **Our code without generated headers.** `azura_app.h` declares `PPCImageConfig`, and our code
   is the `azura_host` object library.
2. **Post-codegen steps without Python.** The function registration split in the code generator
   (an SDK patch) and the Linux engine fix as manifest hooks.
3. **The player kit per platform in CI.** `scripts/build-kit.sh`, the CMake probes, `make_kit.py`
   and `check_kit.py`, and the Runtime kit workflow. The kit feeds the GitHub CD card.
4. **The compiler.** `toolchain.rs`: detecting and installing it on macOS and Linux, with the
   manual instructions (section 6).
5. **Building the runtime in the launcher.** `builder.rs` and the progress card (sections 5
   and 8).
6. **Updates by build and link keys,** with the per-object keys (section 7).
7. **Linux** is still open: the measurements and checks of section 10, by Juan Llorca Martínez,
   before phases 4 and 5 are closed on Linux.

Card 33 added the game as a disc image (section 5): `disc_image.rs`, the check and the build in
the launcher, and the SDK patch that mounts the image in the runtime.
