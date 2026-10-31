#include "window_icon.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/ui/image_decode.h>

namespace azura {

namespace {

// Side of the icon given to the windows: big enough for any taskbar, small enough for an X11
// window property.
constexpr int kWindowIconSize = 256;

// Folder of the running executable (SDL's base path), or empty.
std::filesystem::path ExecutableFolder() {
  const char* base = SDL_GetBasePath();
  return base ? std::filesystem::path(base) : std::filesystem::path();
}

// The Azura icon beside the executable (the build installs it there), or empty.
std::filesystem::path IconPath() {
  const std::filesystem::path folder = ExecutableFolder();
  if (folder.empty()) return {};
  const std::filesystem::path icon = folder / "assets" / "azura-icon.png";
  std::error_code ec;
  return std::filesystem::is_regular_file(icon, ec) ? icon : std::filesystem::path();
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Writes `data` to `path` unless it already holds exactly that. Returns false on failure.
bool WriteIfChanged(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && ReadAll(path) == data) return true;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

#if defined(__linux__)
// The user's data folder ($XDG_DATA_HOME, else ~/.local/share), or empty.
std::filesystem::path DataHome() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return xdg;
  if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".local" / "share";
  return {};
}

// Installs <data>/applications/<kAppId>.desktop, hidden from the application menus, with a copy of
// the icon in <data>/icons, so the desktop names and draws the game window as Gears of War 3. The
// icon is a copy rather than the one beside the executable so the entry stays the same whichever
// build of the runtime runs. Nothing is written when both are already as they should be.
void InstallDesktopEntry() {
  const std::filesystem::path data_home = DataHome();
  const std::filesystem::path icon = IconPath();
  if (data_home.empty() || icon.empty()) return;
  const std::filesystem::path icon_copy = data_home / "icons" / (std::string(kAppId) + ".png");
  const std::filesystem::path entry = data_home / "applications" / (std::string(kAppId) + ".desktop");
  const std::filesystem::path executable = ExecutableFolder() / "azura";
  const std::string text = std::string("[Desktop Entry]\n") +
                           "Type=Application\n"
                           "Name=Gears of War 3\n"
                           "Comment=Gears of War 3 on Azura\n"
                           "Exec=\"" + executable.string() + "\"\n"
                           "Icon=" + icon_copy.string() + "\n"
                           "StartupWMClass=" + kAppId + "\n"
                           "Categories=Game;\n"
                           "NoDisplay=true\n";
  if (!WriteIfChanged(icon_copy, ReadAll(icon)) ||
      !WriteIfChanged(entry, std::vector<uint8_t>(text.begin(), text.end()))) {
    REXLOG_WARN("[Azura] Cannot write the desktop entry {}; the taskbar may show a generic icon", entry.string());
  }
}
#endif

}  // namespace

void SetAppIdentity() {
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, kAppId);
#if defined(__linux__)
  InstallDesktopEntry();
#endif
}

void SetWindowIcon() {
  const std::filesystem::path path = IconPath();
  if (path.empty()) return;
  const std::vector<uint8_t> png = ReadAll(path);
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels = rex::ui::DecodeImageRGBA(png.data(), png.size(), width, height);
  if (pixels.empty()) {
    REXLOG_WARN("[Azura] Cannot decode {}; the window keeps a generic icon", path.string());
    return;
  }
  SDL_Surface* full = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4);
  SDL_Surface* icon = full ? SDL_ScaleSurface(full, kWindowIconSize, kWindowIconSize, SDL_SCALEMODE_LINEAR) : nullptr;
  int count = 0;
  SDL_Window** windows = icon ? SDL_GetWindows(&count) : nullptr;
  for (int i = 0; i < count; ++i) SDL_SetWindowIcon(windows[i], icon);
  SDL_free(windows);
  SDL_DestroySurface(icon);
  SDL_DestroySurface(full);
}

}  // namespace azura
