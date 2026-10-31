// The game window's identity and icon on Linux and Windows (macOS has its own: platform_mac.mm).
//
// A taskbar or dock does not take a window's icon the same way everywhere:
//   - GNOME on Wayland ignores the icon a window sets: it shows the icon of the desktop entry whose
//     name matches the window's application ID. The runtime gives its window the ID kAppId and, on
//     Linux, writes a hidden desktop entry of that name (with a copy of the icon) to the user's
//     applications folder, so the game shows its own icon whichever executable started it.
//   - X11 desktops (StartupWMClass is the same ID) and KDE also take the window's own icon, which
//     SetWindowIcon sets; so does Windows for the title bar and the taskbar.
//   - Windows Explorer shows the icon resource of azura.exe (src/azura.rc).

#pragma once

namespace azura {

// The application ID of the game window, and the name of its desktop entry on Linux.
inline constexpr const char* kAppId = "com.gears3azura.game";

// Sets the application ID and, on Linux, installs the desktop entry. Call before the window is
// created: the ID is read when it is.
void SetAppIdentity();

// Gives every window of the game the Azura icon (assets/azura-icon.png beside the executable).
// Call once the window exists.
void SetWindowIcon();

}  // namespace azura
