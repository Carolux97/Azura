#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#include <filesystem>
#include <string>
#include <vector>
#include "platform_mac.h"
#include <rex/logging.h>

void SetMacAppIcon(const std::string& icon_path) {
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:icon_path.c_str()];
    NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
    if (image) {
      [NSApplication sharedApplication].applicationIconImage = image;
      REXLOG_INFO("[Azura] Application dock icon successfully updated from: {}", icon_path);
    } else {
      REXLOG_WARN("[Azura] Failed to load icon image from: {}", icon_path);
    }
  }
}

void AutoConfigureMacAppIcon(const std::string& exe_dir_str) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  if (!exe_dir_str.empty()) {
    fs::path exe_dir(exe_dir_str);
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
