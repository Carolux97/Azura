// RAAM's Shadow's achievements on the disc build.
//
// The add-on's achievements (IDs 56 to 65) are not in default.xex: the package brings its own
// achievement database (spa.bin, an XDBF file like the executable's resource). The game asks for
// them by ID as for any other (GearPC.ClientUnlockAchievement passes its EGearAchievement value as
// the ID), but the SDK only records achievements it knows, so RegisterDlcAchievements adds the
// add-on's to the ones the executable defines, with their texts in the player's language and their
// artwork.
//
// Three of them are unlocked by Title Update script the disc build does not have, so the runtime
// unlocks them itself, from the calls the disc's script still makes (see script_hooks.cpp):
//   - 56 "Welcome To Zeta" and 57 "Some Friendly Competition": the add-on's five chapters
//     (31 to 35) completed in a standard or an arcade campaign. The Title Update checks this in
//     GearProfileSettings.CompleteChapterByDifficulty; the runtime keeps the chapters each kind of
//     campaign completed and unlocks the achievement when it has all five.
//   - 61 "Death from Above": 50 enemies killed with the satellite's Hammer of Dawn. The add-on's
//     script reports each kill to GearProfileSettings.UpdateAchievementProgression, which on the
//     disc only counts the main game's achievements; the runtime keeps the count.
// What they need is kept in <saves folder>/achievements/4D5308AB-raams-shadow.txt.
//
// 58 needs four players in co-op, and the others belong to the multiplayer modes; the runtime does
// not unlock them.

#pragma once

#include <cstdint>
#include <filesystem>

namespace azura::dlc {

// First and last achievement IDs of RAAM's Shadow in its achievement database.
constexpr uint32_t kRaamsShadowFirstAchievement = 56;
constexpr uint32_t kRaamsShadowLastAchievement = 65;

// Registers RAAM's Shadow's achievements when the add-on is installed in `user_data_root`, with the
// icons extracted into `cache_root`. Call once default.xex is loaded.
void RegisterDlcAchievements(const std::filesystem::path& user_data_root, const std::filesystem::path& cache_root);

// GearProfileSettings.CompleteChapterByDifficulty ran for `chapter`.
void OnChapterCompleted(int32_t chapter, bool arcade);

// GearProfileSettings.UpdateAchievementProgression ran for `achievement`, by `count`.
void OnAchievementProgress(uint32_t achievement, int32_t count);

}  // namespace azura::dlc
