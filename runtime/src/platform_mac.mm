#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "platform_mac.h"
#include <rex/logging.h>

// The runtime is a bare executable, not an app bundle, so macOS has no icon for it: the Dock,
// Stage Manager and minimised windows would show a generic one. The icon is set on the running
// application and on its windows here. The executable file gets its icon when it is built
// (CMakeLists.txt) and when the launcher installs it, never at run time: setting a file's icon
// rewrites its metadata and modification time, which the launcher compares to tell whether the
// installed runtime is up to date, and adds a resource fork that a code signature does not allow.

static NSImage* s_icon_image = nil;
static id s_window_observer = nil;

void SetMacAppIcon(const std::string& icon_path) {
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:icon_path.c_str()];
    NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
    if (!image) {
      REXLOG_WARN("[Azura] Failed to load icon image from: {}", icon_path);
      return;
    }
    if (s_icon_image) {
      [s_icon_image release];
    }
    s_icon_image = image;  // owned: initWithContentsOfFile returned it retained

    [NSApplication sharedApplication].applicationIconImage = s_icon_image;

    // Minimised windows and Stage Manager show a window's miniwindowImage: set it on the windows
    // that exist now and on every window when it becomes key, which covers those created later.
    for (NSWindow* window in [NSApplication sharedApplication].windows) {
      [window setMiniwindowImage:s_icon_image];
    }
    if (!s_window_observer) {
      s_window_observer = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidBecomeKeyNotification
                      object:nil
                       queue:[NSOperationQueue mainQueue]
                  usingBlock:^(NSNotification* note) {
                    NSWindow* window = (NSWindow*)note.object;
                    if (window && s_icon_image) {
                      [window setMiniwindowImage:s_icon_image];
                    }
                  }];
    }

    REXLOG_INFO("[Azura] Application and window icons set from: {}", icon_path);
  }
}

void GetMacGpuMemoryStats(uint64_t& used_bytes, uint64_t& budget_bytes) {
  used_bytes = 0;
  budget_bytes = 0;
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    used_bytes = static_cast<uint64_t>(device.currentAllocatedSize);
    budget_bytes = static_cast<uint64_t>(device.recommendedMaxWorkingSetSize);
#if !__has_feature(objc_arc)
    [device release];
#endif
  }
}

namespace {

// Folder of the running executable, with links resolved, or empty when it cannot be found.
std::filesystem::path ExecutableFolder() {
  char path[PATH_MAX] = {0};
  uint32_t size = sizeof(path);
  char resolved[PATH_MAX] = {0};
  if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, resolved)) {
    return {};
  }
  return std::filesystem::path(resolved).parent_path();
}

}  // namespace

void AutoConfigureMacAppIcon(const std::string& exe_dir_str) {
  namespace fs = std::filesystem;
  const fs::path exe_dir = exe_dir_str.empty() ? ExecutableFolder() : fs::path(exe_dir_str);
  std::vector<fs::path> candidates;
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir / "assets" / "azura-icon.png");
    candidates.push_back(exe_dir / "assets" / "azura-icon.icns");
    candidates.push_back(exe_dir / "azura-icon.png");
    candidates.push_back(exe_dir / "azura-icon.icns");
  }
  candidates.push_back("runtime/assets/azura-icon.png");
  candidates.push_back("runtime/assets/azura-icon.icns");
  candidates.push_back("assets/azura-icon.png");
  candidates.push_back("assets/azura-icon.icns");

  for (const auto& candidate : candidates) {
    if (fs::exists(candidate)) {
      SetMacAppIcon(candidate.string());
      return;
    }
  }
}
#endif
