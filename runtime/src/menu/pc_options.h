// PC settings in the game's own Options menu.
//
// The Options menu (UIScene_Options) is a list of categories whose rows each edit a profile
// setting. Azura changes it so that:
//   - Video, which only opened the brightness calibration screen, becomes a list: a "Calibrate
//     brightness" row that opens that screen with Accept, then full screen, frame rate limit, anti-aliasing
//     and motion blur;
//   - "Storage location", which has no use on a PC, becomes "Keyboard and mouse", and it and
//     "Controller" open the controls menus (controls_menu.h) instead of a list.
// How:
//   - the rows are defined in the game's localization files, which the engine imports in full
//     (PatchOptionsMenu);
//   - list rows are tied to profile settings the game never uses (the engine's racing game
//     preferences), whose definitions get our value names (PatchValueNames), or are lists of our
//     own (frame rate limit, render resolution, upscaling), whose value names come from PcOptionValueNames;
//   - the script hooks (script_hooks.cpp) put each row's current value in the list as the menu
//     builds it, hand back what the player chose when the menu saves, follow the selected row to
//     open the calibration screen, and patch two spots of the menu's bytecode.
// Values are kept in azura.toml next to the runtime. Details: docs/reverse-engineering.md,
// "Options menu".

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace azura::menu {

struct CoalescedSection;

// Positions in UIScene_Options.OptionCategories (the game's EOptionsMenuMode).
constexpr int kGameCategory = 0;
constexpr int kVideoCategory = 1;
// Controller: choosing it opens the controller's controls menu (controls_menu.h) through
// Callback_ViewController, the function of the list's "View controller" button, which has no list
// to be in any more.
constexpr int kControllerCategory = 4;
// Keyboard and mouse, at the game's storage location position: choosing it opens the keyboard and
// mouse's controls menu through Callback_ResetTutorials, which only Game's list runs otherwise.
constexpr int kKeyboardMouseCategory = 5;
// Local co-op, at the game's mature content position (that category's two rows move to the end of
// Game): choosing it opens the Local co-op menu (local_coop_menu.h) instead of a list.
constexpr int kLocalCoopCategory = 6;
// Connection, at the game's credits position: choosing it runs Callback_Credits, where
// script_hooks.cpp opens the Alboria menu (connection_menu.h). The credits move to a Y button of
// the list of categories (kNoCategory's buttons), which runs the same function for them.
constexpr int kConnectionCategory = 7;
// OptionCategories' last entry, the game's OMI_MAX: no category, only the buttons of the list of
// categories (and UIScene_Options.CurrentMode while that list is shown).
constexpr int kCategoryListButtons = 8;
// Row of the Video category that opens the brightness calibration screen.
constexpr int kCalibrateRow = 0;

// The value of frame_rate_limit that removes the limit (the last entry of its list).
constexpr float kFrameRateUnlimited = 150.0f;

// The value names of a list row of our own (one without a profile setting definition) whose
// profile setting ID is `psid`, in the player's language. False when `psid` is not one of them.
bool PcOptionValueNames(int psid, std::vector<std::u16string>& names);

// Where chosen values are saved (azura.toml).
void SetPcOptionsConfigPath(std::filesystem::path config_path);

// The game's own texts the controls menus show, in the language of the game's menus.
struct GameControlsTexts {
  // The controller view's descriptions of the buttons (UIScene_Options: "AButtonString",
  // "AButtonAltString", ... "LeftStickClickString"), by key.
  std::map<std::string, std::string> buttons;
  // The Controller category's title and its rows' labels, by profile setting ID.
  std::string controller_title;
  std::map<int, std::string> setting_labels;
  // The value names of the profile settings that are lists, by ID, in the order of their values
  // (GearProfileSettings' ProfileMappings).
  std::map<int, std::vector<std::string>> value_names;
  // The menus' Back and Defaults buttons.
  std::string back;
  std::string defaults;
};
// The texts of the player's language (user_language), or English when the game has none in it.
GameControlsTexts ControlsTextsFromGame();

// A setting of the controls menus: sets cvar `name` to `value` and writes it to azura.toml, quoted
// when `quoted` (a string setting).
void StoreControlsSetting(const std::string& name, const std::string& value, bool quoted);

// The project's default of cvar `name` (azura.default.toml), or "" when it sets none (the SDK's
// default applies).
std::string ControlsSettingDefault(const std::string& name);

// Identifier of the PC option at row `row` of category `category`, or -1 when the row is one of the
// game's own. Rows count only the options shown now: without an image scaling effect that sharpens,
// the sharpness row is hidden and the rows below it move up.
int FindPcOption(int category, int row);

// Number of rows category `category` shows now (some depend on other rows' values).
int PcOptionsShownRows(int category);

// Profile setting IDs of category `category`'s options in the order its CategoryOptions array must
// hold them: the rows shown now, in menu order, then the hidden ones, which the menu leaves out by
// counting fewer rows.
std::vector<int> PcOptionsArrangedPsids(int category);

// For a row of ours whose changes the script must hear of while the menu is open, the profile
// setting ID of the game row whose change callback it borrows (UIScene_Options.
// GetCallbackForOptionPSID); -1 for any other `psid`.
int PcOptionLiveCallbackPsid(int psid);

// The borrowed callback of `borrowed_psid` ran with `value` in category `category`: applies and
// saves it for the row of ours that borrows it, and returns true (false when no shown row of ours
// borrows it, so the game's own callback must run). `row` is that row; `rows_changed` tells whether
// the rows the category shows changed, so the list must be built again.
bool PcOptionLiveChange(int category, int borrowed_psid, float value, int& row, bool& rows_changed);

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

// Some options (anti-aliasing, render resolution, image scaling, motion blur) only take effect when the game starts.
// Called when the player leaves `category` of the Options menu, once its values are saved: returns
// true when a new value of one of those options is not in effect yet and the player has not been
// asked about these values before, so the game can offer to restart.
bool PcOptionsCategoryClosed(int category);

// Keys of the restart message box texts in [MessageBoxStrings] of GearGameUI. Each is exactly as
// long as the game's key it stands in for (ResetOptions_Title and ResetOptions_Message), since
// the script hooks swap them in the bytecode of the Defaults message box (see script_hooks.cpp).
constexpr const char* kRestartTitleKey = "AzuraRestart_Title";
constexpr const char* kRestartMessageKey = "AzuraRestart_Message";

// Adds the restart message box texts, in `language`, to a [MessageBoxStrings] section of a
// GearGameUI localization file.
void PatchMessageBoxTexts(CoalescedSection& section, const std::string& language);

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
// [Engine.Engine] of Xbox360-GearEngine.ini: the frame rate smoothing ceiling.
void PatchEngineSettings(CoalescedSection& section);
// Applies render_resolution (the Video menu's render resolution, "auto" for this computer) to the
// draw resolution scale and the texture cache limits. Called at start-up, once the GPU plugin that
// defines them is loaded and before the GPU starts: they cannot change while the game runs.
void ApplyRenderResolution();
// Applies upscaling_sharpness (the Video menu's sharpness) to the sharpening settings of the image
// scaling effects. Called at start-up with `show_now` false, before the presenter reads them, and
// when the menu changes it with `show_now` true, which also runs the handler of
// SetSharpnessChangedHandler so the next frame shows it.
void ApplyUpscalingSharpness(bool show_now);
// Whether the running presenter draws a present_effect value ("cas", "metalfx"...), so the
// upscaling row lists only those (from any thread); nullptr lists every value the build accepts.
void SetUpscalingSupport(std::function<bool(const std::string&)> supported);
// What ApplyUpscalingSharpness runs to show a change while the game runs (from any thread);
// nullptr to stop.
void SetSharpnessChangedHandler(std::function<void()> handler);
// Applies frame_rate_limit to the guest's vertical blanks (vsync, guest_vblank_rate). Called at
// start-up and when the Video menu changes it.
void ApplyFrameRateLimit();
// On Windows, allows the mailbox presentation mode unless the player's azura.toml sets
// vulkan_allow_present_mode_mailbox. Elsewhere it does nothing. Called at start-up, before the
// presenter creates its swapchain.
void ApplyPresentMode();

}  // namespace azura::menu
