// AzuraApp: our application class on top of the ReXGlue runtime.
//
// rex::ReXApp owns the window, the runtime and the built-in overlays (F3 debug, F4 settings,
// backtick console). It exposes virtual hooks (OnPreSetup, OnPostLaunchModule, OnCreateDialogs,
// ...) that run at fixed points of start-up; this class overrides them to add the game-specific
// behaviour (keyboard and mouse, graphics settings, networking) as the project grows.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>

#include <rex/cvar.h>
#include <rex/platform/exceptions.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>

#include <rex/ui/keybinds.h>

#include "menu/menu_patches.h"
#include "menu/pc_options.h"
#include "perf_overlay.h"
#include "prompts/button_prompts.h"
#include "script_hooks.h"

#if defined(__APPLE__)
#include "platform_mac.h"
#endif

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

class AzuraApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static void SanitizePresentEffectInConfigs() {
    std::vector<std::filesystem::path> config_candidates = {
      std::filesystem::current_path() / "azura.toml",
    };
    const char* home = std::getenv("HOME");
    if (home) {
      config_candidates.push_back(std::filesystem::path(home) / ".local/share/com.gears3azura.launcher/runtime/azura.toml");
    }
    for (const auto& path : config_candidates) {
      std::error_code ec;
      if (std::filesystem::exists(path, ec)) {
        try {
          std::ifstream in(path);
          if (in.is_open()) {
            std::stringstream buffer;
            buffer << in.rdbuf();
            std::string content = buffer.str();
            in.close();

            bool modified = false;
            std::vector<std::string> invalid_vals = {"\"cas\"", "\"fsr\"", "\"fsr2\"", "\"fsr3\""};
            for (const auto& inv : invalid_vals) {
              std::string target = "present_effect = " + inv;
              size_t pos = 0;
              while ((pos = content.find(target, pos)) != std::string::npos) {
                content.replace(pos, target.length(), "present_effect = \"bilinear\"");
                pos += 26;
                modified = true;
              }
            }
            if (!std::filesystem::exists("/dev/nvidiactl")) {
              size_t vpos = content.find("vulkan_device = 0");
              if (vpos != std::string::npos) {
                content.replace(vpos, 17, "vulkan_device = -1");
                modified = true;
              }
            }
            size_t cmps_pos = content.find("clear_memory_page_state = false");
            if (cmps_pos != std::string::npos) {
              content.replace(cmps_pos, 31, "clear_memory_page_state = true");
              modified = true;
            }
            size_t asc_pos = content.find("async_shader_compilation = false");
            if (asc_pos != std::string::npos) {
              content.replace(asc_pos, 32, "async_shader_compilation = true");
              modified = true;
            }
            size_t rr_pos = content.find("readback_resolve = \"fast\"");
            if (rr_pos != std::string::npos) {
              content.replace(rr_pos, 25, "readback_resolve = \"none\"");
              modified = true;
            }
            if (modified) {
              std::ofstream out(path, std::ios::trunc);
              if (out.is_open()) {
                out << content;
              }
            }
          }
        } catch (...) {
        }
      }
    }
  }

  // Factory used by REX_DEFINE_APP. "azura" is the application name: it names the user data
  // folder, the log files and the config file (azura.toml).
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    rex::initialize_seh();
    SanitizePresentEffectInConfigs();
#if defined(__APPLE__)
    AutoConfigureMacAppIcon();
    // MoltenVK warns "Metal does not support disabling primitive restart" for nearly every
    // pipeline the game creates (hundreds a second while shaders compile), which buries the log
    // and floods the launcher's log panel. Only its errors are kept. Set before Vulkan starts;
    // an MVK_CONFIG_LOG_LEVEL already in the environment wins.
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
    return std::unique_ptr<AzuraApp>(new AzuraApp(ctx, "azura", PPCImageConfig));
  }

 protected:
  // Settings are layered: azura.default.toml (the project's defaults, installed beside the
  // executable by every build), then azura.toml (the player's settings, written by the launcher
  // and the F4 menu), then the command line. The SDK loads azura.toml right after this hook, so
  // loading the defaults here lets any key missing from azura.toml keep the project's default
  // (keyboard and mouse on, the key bindings, button prompts) instead of the SDK's.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto defaults = paths.config_path.parent_path() / (std::string(GetName()) + ".default.toml");
    if (std::filesystem::exists(defaults)) {
      rex::cvar::LoadConfig(defaults);
    }
    // Settings changed in the game's Options menu (Graphics (PC)) are saved to azura.toml.
    azura::menu::SetPcOptionsConfigPath(paths.config_path);
  }

  // The runtime has mounted the game files and the game has not started: apply the menu changes
  // and route Quit Game to a normal window close (OnClosing shuts the title down).
  void OnPreLaunchModule() override {
    REXLOG_INFO("[Azura] Active gamertag / user_name: '{}'", REXCVAR_GET(user_name));
    azura::menu::InstallMenuPatches(runtime());
    azura::script::SetQuitHandler([this] {
      app_context().CallInUIThread([this] {
        if (window()) {
          window()->RequestClose();
        }
      });
    });
    SetGuestFrameStats([]() -> rex::ui::FrameStats {
      float fps = 0.0f;
      float ms = 0.0f;
      uint64_t count = 0;
      azura::GetGuestFrameStats(fps, ms, count);
      rex::ui::FrameStats stats;
      stats.fps = fps;
      stats.frame_time_ms = ms;
      stats.frame_count = count;
      return stats;
    });
  }

  // The game module is running and guest memory holds its textures: start keeping the button
  // prompts in step with the player's device and bindings.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    button_prompts_ = std::make_unique<azura::prompts::ButtonPrompts>(runtime());
  }

  // Stop the prompt thread before the runtime and its memory go away.
  void OnShutdown() override { button_prompts_.reset(); }


  // ReXApp computes path_config BEFORE LoadConfig(config_path_) is executed, so
  // game_data_root defined in azura.toml is only available in REXCVAR_GET after LoadConfig.
  // OnFinalizePaths runs after LoadConfig and before ConstructRuntime, so we update it here.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig paths = defaults;
    if (paths.game_data_root.empty()) {
      std::string cvar_root = REXCVAR_GET(game_data_root);
      if (!cvar_root.empty()) {
        paths.game_data_root = cvar_root;
      }
    }

    auto exe_dir = paths.config_path.empty() ? std::filesystem::current_path() : paths.config_path.parent_path();

    // Universal resolution: if game_data_root is relative or does not exist, try local fallbacks
    if (!paths.game_data_root.empty() && paths.game_data_root.is_relative() &&
        std::filesystem::exists(exe_dir / paths.game_data_root)) {
      paths.game_data_root = exe_dir / paths.game_data_root;
    }

    if (paths.game_data_root.empty() || !std::filesystem::exists(paths.game_data_root)) {
      if (std::filesystem::exists(exe_dir / "game")) {
        paths.game_data_root = exe_dir / "game";
      } else if (std::filesystem::exists(exe_dir / "data")) {
        paths.game_data_root = exe_dir / "data";
      } else if (std::filesystem::exists(exe_dir.parent_path() / "runtime" / "game")) {
        paths.game_data_root = exe_dir.parent_path() / "runtime" / "game";
      }
      if (!paths.game_data_root.empty()) {
        rex::cvar::SetFlagByName("game_data_root", paths.game_data_root.string());
        REXLOG_INFO("Resolved universal game_data_root to: {}", paths.game_data_root.string());
      }
    }

#if defined(__APPLE__)
    AutoConfigureMacAppIcon(exe_dir.string());
#endif
    auto controller_db = exe_dir / "gamecontrollerdb.txt";
    if (!std::filesystem::exists(controller_db)) {
      controller_db = exe_dir.parent_path() / "gamecontrollerdb.txt";
    }
    if (std::filesystem::exists(controller_db)) {
      rex::cvar::SetFlagByName("hid_mappings_file", controller_db.string());
      REXLOG_INFO("Configured hid_mappings_file to: {}", controller_db.string());
    }
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("writable_executable_memory", "true");
    rex::cvar::SetFlagByName("protect_on_release", "false");
    return paths;
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_perf_overlay", "F6", "Toggle performance overlay", [this, drawer] {
      if (perf_overlay_) {
        perf_overlay_.reset();
      } else {
        perf_overlay_ = std::make_unique<azura::PerfOverlayDialog>(drawer);
      }
    });
  }

 private:
  std::unique_ptr<azura::prompts::ButtonPrompts> button_prompts_;
  std::unique_ptr<azura::PerfOverlayDialog> perf_overlay_;
};
