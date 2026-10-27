// Installs a downloadable content package (an Xbox 360 STFS "LIVE" package the player owns) into the
// player's saves folder, where the game finds it as it would on a console's hard drive.
//
// Started by the launcher for that task alone (--install_content=<package>), the runtime checks that
// the package is Marketplace content of Gears of War 3, extracts it with the SDK's ContentManager into
// <user data>/0000000000000000/4D5308AB/00000002/<package file name>/ with its header next to it,
// prints one JSON line with the result, and exits.

#pragma once

#include <filesystem>

namespace azura::dlc {

// Whether the runtime was started to install a package (the install_content setting).
bool ContentInstallRequested();

// Installs the package of the install_content setting into `user_data_root`, loading default.xex
// from `game_data_root` for the title's identity. Returns the process exit code (0 on success);
// prints {"ok":true,"display_name":"...","folder":"..."} or {"ok":false,"error":"..."}.
int InstallContent(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root);

}  // namespace azura::dlc
