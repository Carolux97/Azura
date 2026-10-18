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
#include <rex/rex_app.h>
#include <rex/system/flags.h>

#include "menu/menu_patches.h"
#include "menu/pc_options.h"
#include "prompts/button_prompts.h"
#include "script_hooks.h"

#if defined(__APPLE__)
#include "platform_mac.h"
#endif

class AzuraApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Factory used by REX_DEFINE_APP. "azura" is the application name: it names the user data
  // folder, the log files and the config file (azura.toml).
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
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
#if defined(__APPLE__)
    // Again now that the window exists, so it gets the icon too (see platform_mac.mm).
    AutoConfigureMacAppIcon();
#endif
    azura::menu::InstallMenuPatches(runtime());
    azura::script::SetQuitHandler([this] {
      app_context().CallInUIThread([this] {
        if (window()) {
          window()->RequestClose();
        }
      });
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
    auto exe_dir = paths.config_path.parent_path();
#if defined(__APPLE__)
    AutoConfigureMacAppIcon(exe_dir.string());
#endif
    auto controller_db = exe_dir / "gamecontrollerdb.txt";
    if (std::filesystem::exists(controller_db)) {
      rex::cvar::SetFlagByName("hid_mappings_file", controller_db.string());
      REXLOG_INFO("Configured hid_mappings_file to: {}", controller_db.string());
    }
    return paths;
  }

 private:
  std::unique_ptr<azura::prompts::ButtonPrompts> button_prompts_;
};
