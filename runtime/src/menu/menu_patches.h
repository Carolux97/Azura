// Changes to the game's front-end menus, applied to its configuration as it loads.
//
// The main menu and its sub-menus are defined in the game's coalesced configuration
// (Coalesced_<LANG>.bin): [GearGame.UIWidget_MainMenu] lists every screen as an AllOptions entry
// with a List of options (Label, Next screen, flags), and the visible text of each Label lives in
// the [MainMenu] section of the GearGameUI localization file. Azura changes them so that:
//   - options that will never work (Xbox LIVE event calendar, Marketplace) are removed;
//   - online options that may work once peer-to-peer play exists are greyed out
//     (bIsDisabled=true) and marked "(coming soon)" in the player's language;
//   - a "Quit Game" option is added at the end of the main menu.
// Only options at the end of a list are removed, so no other option changes position: the game's
// script identifies a choice by its index (UIScene_FrontEnd.HandleMenuSelection).
//
// The same files also get the changes to the Options menu and the engine settings described in
// pc_options.h.
//
// The player's files are never modified. InstallMenuPatches writes patched copies to Azura's cache
// folder and maps the game's paths to them in the virtual file system.

#pragma once

#include <string>

namespace rex {
class Runtime;
}

namespace azura::menu {

struct Coalesced;

// Screen index of the main menu in the game's main-menu screen enum (MAINMENUOP_MainMenu).
constexpr int kMainMenuScreen = 1;

// Applies the menu changes to `coalesced`. Texts are chosen by the extension of each GearGameUI
// localization file it holds ("esn", "int", ...). Returns the index of the Quit Game option in the
// main menu, or -1 when the file does not hold the main menu definition.
int PatchMenus(Coalesced& coalesced);

// Patches every Coalesced_*.bin of the game and serves the patched copies to it. Call after the
// runtime has mounted the game files and before the game module starts.
void InstallMenuPatches(rex::Runtime* runtime);

// Index of the Quit Game option in the main menu, or -1 if the menus were not patched.
int MainMenuQuitOption();

}  // namespace azura::menu
