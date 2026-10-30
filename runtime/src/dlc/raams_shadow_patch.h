// Azura's GearGame script patch for RAAM's Shadow, written on the player's machine from their own
// game files (the C++ version of tools/dlc/make_patch.py; see docs/raams-shadow-dlc.md).
//
// RAAM's Shadow's packages import a few classes, properties and functions of the GearGame script
// package that only the game's Title Update has. The disc build reads script patches from the update
// volume (GearGame/Patches/ScriptPatch_GearGame.bin), so Azura serves one that declares them. Its
// content is Azura's own, kept to what the DLC needs to load and play; every index, name and layout
// comes from the player's GearGame and Engine packages, so the patch is made here, for their copy of
// the game, and nothing derived from the game is distributed.
//
// Started by the launcher for that task alone (--make_dlc_patch=<folder>), the runtime writes
// <folder>/GearGame/Patches/ScriptPatch_GearGame.bin and its _us size file, prints one JSON line
// ({"ok":true,"patch_size":N} or {"ok":false,"error":"..."}) and exits.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace azura::dlc {

// Whether the runtime was started to write the patch (the make_dlc_patch setting).
bool DlcPatchRequested();

// Writes the patch for the game in `game_data_root` into the make_dlc_patch folder. Returns the
// process exit code (0 on success).
int MakeDlcPatch(const std::filesystem::path& game_data_root);

// The patch for the given uncompressed GearGame and Engine packages, before packing (for tests).
// Throws ue3::FormatError when a package lacks something the patch needs.
std::vector<uint8_t> BuildRaamsShadowPatch(std::vector<uint8_t> gear_game, std::vector<uint8_t> engine);

}  // namespace azura::dlc
