// PC settings in the game's own Options menu.
//
// The Options menu (UIScene_Options) is a list of categories whose rows each edit a profile
// setting. Azura changes it so that:
//   - Video, which only opened the brightness calibration screen, becomes a list: a "Calibrate
//     brightness" row that opens that screen with Accept, then full screen, vsync, anti-aliasing
//     and motion blur;
//   - "Storage location", which has no use on a PC, becomes "Keyboard and mouse", with the mouse
//     sensitivity (and the place for key bindings later).
// How:
//   - the rows are defined in the game's localization files, which the engine imports in full
//     (PatchOptionsMenu);
//   - list rows are tied to profile settings the game never uses (the engine's racing game
//     preferences), whose definitions get our value names (PatchValueNames);
//   - the script hooks (script_hooks.cpp) put each row's current value in the list as the menu
//     builds it, hand back what the player chose when the menu saves, follow the selected row to
//     open the calibration screen, and patch two spots of the menu's bytecode.
// Values are kept in azura.toml next to the runtime. Details: docs/reverse-engineering.md,
// "Options menu".

#pragma once

#include <filesystem>
#include <string>

namespace azura::menu {

struct CoalescedSection;

// Positions in UIScene_Options.OptionCategories (the game's EOptionsMenuMode).
constexpr int kVideoCategory = 1;
constexpr int kKeyboardMouseCategory = 5;  // the game's storage location position
// Row of the Video category that opens the brightness calibration screen.
constexpr int kCalibrateRow = 0;

// Where chosen values are saved (azura.toml).
void SetPcOptionsConfigPath(std::filesystem::path config_path);

// Identifier of the PC option at row `row` of category `category`, or -1 when the row is one of the
// game's own.
int FindPcOption(int category, int row);

// Whether PC option `option` is a slider (else a list).
bool PcOptionIsSlider(int option);

// Whether PC option `option` only triggers an action (the calibration row) and has no value.
bool PcOptionIsAction(int option);

// Value the menu shows for `option`: the slider value, or the index in the list.
float PcOptionDisplayValue(int option);

// Applies and saves the value the player chose for `option` (slider value or list index).
void PcOptionStoreValue(int option, float value);

// Puts the options of `category` back to their default values (the category's Defaults button).
void PcOptionsResetDefaults(int category);

// Rewrites Video and puts Keyboard and mouse in place of the storage location, in a
// [UIScene_Options] section of a GearGame localization file. New texts are in `language` (the file
// extension: "esn", "int", ...); the game's own texts are kept from the file.
void PatchOptionsMenu(CoalescedSection& section, const std::string& language);

// Rewrites the definitions (value names) of the profile settings the list rows use, in a
// [GearProfileSettings] section of a GearGame localization file.
void PatchValueNames(CoalescedSection& section, const std::string& language);

// Applies the settings the engine reads at start-up to the [SystemSettings] section of the
// engine configuration (Xbox360-GearEngine.ini).
void PatchSystemSettings(CoalescedSection& section);

}  // namespace azura::menu

