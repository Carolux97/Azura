// The game's achievements for the launcher's Achievements section (--export_achievements).
//
// The achievements (names, descriptions, gamerscore, flags and artwork) live in the XDBF resource
// of default.xex, which is encrypted and compressed on disk. The runtime already knows how to load
// it, so the launcher asks the runtime instead of reading the executable itself: started with
// --export_achievements=<folder>, the runtime loads default.xex without a window or a GPU, writes
// the achievements in every language the game has, and exits.

#pragma once

#include <filesystem>

namespace azura {

// Whether the runtime was started to export the achievements (export_achievements), not to run the
// game.
bool AchievementExportRequested();

// Loads default.xex from `game_data_root` (a folder or a disc image) and writes into the
// export_achievements folder:
//   achievements.json  {"titleId":"4D5308AB","languages":["en","es",...],"achievements":[
//                        {"id":1,"gamerscore":5,"flags":14,"imageId":1,
//                         "label":{"en":"..."},"description":{...},"unachieved":{...}}]}
//   icons/<imageId>.png  the artwork of each achievement, as the game stores it
// achievements.json is written last, so its presence means the export is complete. Prints one line
// of JSON on standard output ({"achievements":50} or {"error":"..."}) and returns the exit code:
// 0, or 1 when the achievements cannot be read.
int ExportAchievements(const std::filesystem::path& game_data_root);

}  // namespace azura
