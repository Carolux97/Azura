#include "content_install.h"

#include <cstdio>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

REXCVAR_DEFINE_STRING(install_content, "", "Azura",
                      "Install this downloadable content package (an Xbox 360 LIVE package of Gears of "
                      "War 3) into the saves folder and exit (used by the launcher)");

namespace azura::dlc {

using rex::X_RESULT;  // the X_ERROR_* macros name it unqualified

namespace {

constexpr uint32_t kGearsOfWar3TitleId = 0x4D5308AB;

std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

// Prints the one line the launcher reads, and returns the exit code that goes with it.
int Report(const std::string& json, int exit_code) {
  std::fputs((json + "\n").c_str(), stdout);
  std::fflush(stdout);
  return exit_code;
}

int Fail(const std::string& message) {
  return Report("{\"ok\":false,\"error\":" + JsonString(message) + "}", 1);
}

}  // namespace

bool ContentInstallRequested() { return !REXCVAR_GET(install_content).empty(); }

int InstallContent(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root) {
  const std::filesystem::path package = REXCVAR_GET(install_content);
  if (!std::filesystem::is_regular_file(package)) return Fail("the package is not a file");
  if (user_data_root.empty()) return Fail("no saves folder (user_data_root)");

  // Its STFS header: Marketplace content (type 2) of this game.
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package);
  if (!device->Initialize()) return Fail("the file is not an Xbox 360 content package");
  const auto& metadata = device->header().metadata;
  if (metadata.content_type != rex::system::XContentType::kMarketplaceContent) {
    return Fail("the package is not downloadable content (a title update or another kind of content)");
  }
  if (metadata.execution_info.title_id != kGearsOfWar3TitleId) {
    return Fail("the package is not content of Gears of War 3");
  }
  const std::u16string wide_name = metadata.display_name(rex::system::XLanguage::kEnglish);
  std::string display_name;
  for (char16_t c : wide_name) display_name.push_back(c < 0x80 ? char(c) : '?');
  device.reset();

  // The same headless set-up as the achievements export: the kernel and the file system, with the
  // title loaded so the content manager knows whose content it installs.
  auto runtime = std::make_unique<rex::Runtime>(game_data_root, user_data_root);
  if (XFAILED(runtime->Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      }))) {
    return Fail("the runtime could not start");
  }
  if (XFAILED(runtime->LoadXexImage("game:\\default.xex"))) return Fail("default.xex could not be loaded");
  auto* content = runtime->kernel_state()->content_manager();
  if (!content) return Fail("no content manager");
  const rex::X_RESULT result = content->InstallContent(package);
  if (result != X_ERROR_SUCCESS) {
    return Fail("the package could not be extracted (error " + std::to_string(result) + ")");
  }
  const auto folder = user_data_root / "0000000000000000" / "4D5308AB" / "00000002" / package.filename();
  return Report("{\"ok\":true,\"display_name\":" + JsonString(display_name) + ",\"folder\":" +
                    JsonString(folder.string()) + "}",
                0);
}

}  // namespace azura::dlc
