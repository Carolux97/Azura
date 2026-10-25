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

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/image_info.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/pad_remap.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>

#include "achievement_export.h"
#include "achievement_toast.h"
#include "input/mouse_look.h"
#include "input/player_assignment.h"
#include "controls_menu.h"
#include "local_coop_menu.h"
#include "local_coop_notices.h"
#include "gpu_list.h"
#include "menu/menu_patches.h"
#include "menu/pc_options.h"
#include "perf_marker.h"
#include "prompts/button_prompts.h"
#include "relaunch.h"
#include "script_hooks.h"
#include "perf_overlay.h"
#include "shader_compile_overlay.h"
#include "shader_precompile.h"

// Defined by the SDK (core/perf/counter.cpp), which never opens the file itself.
REXCVAR_DECLARE(std::string, perf_log_csv);

#if defined(__APPLE__)
#include "platform_mac.h"
#endif
#if defined(_WIN32)
#include "platform_win.h"
#endif

// The recompiled game image, defined in the code `rexglue codegen` writes from the player's
// default.xex (azura_init.cpp). Declared here rather than taken from the generated headers, so
// this code compiles without any generated code: it is linked with it later, on the machine that
// generates it.
extern const rex::PPCImageInfo PPCImageConfig;

class AzuraApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Factory used by REX_DEFINE_APP. "azura" is the application name: it names the user data
  // folder, the log files and the config file (azura.toml).
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    azura::WaitForRelaunchingProcess();
#if defined(_WIN32)
    // Before any Vulkan instance, including the one --list_gpus creates.
    azura::DisableCrashingVulkanLayers();
#endif
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
    exe_dir_ = paths.config_path.parent_path();
    const auto defaults = paths.config_path.parent_path() / (std::string(GetName()) + ".default.toml");
    if (std::filesystem::exists(defaults)) {
      rex::cvar::LoadConfig(defaults);
    }
    // Settings changed in the game's Options menu (Graphics (PC)) are saved to azura.toml.
    azura::menu::SetPcOptionsConfigPath(paths.config_path);
  }

  // Logging is ready and nothing else has started (no window, no GPU). Started by the launcher for
  // a one-off task, the runtime does only that and exits: exporting the achievements, so the
  // launcher can read them without starting the game, or listing the graphics cards
  // (--list_gpus). std::_Exit skips the static destructors of a runtime that never set up the
  // rest.
  void OnPostInitLogging() override {
#if defined(_WIN32)
    azura::LogDisabledVulkanLayers();
#endif
    if (azura::AchievementExportRequested()) {
      std::_Exit(azura::ExportAchievements(game_data_root()));
    }
    if (azura::GpuListRequested()) {
      std::_Exit(azura::PrintGpuList());
    }
  }

  // azura.toml is loaded and the GPU has not started: the render resolution of the Video menu
  // sets the draw resolution scale and the texture memory, which the GPU reads once. Those
  // settings belong to the GPU plugin, and a setting the runtime changes before its plugin is
  // loaded is dropped, so the plugin is loaded here, as ReXApp would right after this hook (it
  // keeps the one in `config.graphics`).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics && !config.gpu_plugin.empty()) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    }
    azura::menu::ApplyRenderResolution();
    azura::menu::ApplyUpscalingSharpness(false);
  }

  // The runtime has mounted the game files and the game has not started: apply the menu changes
  // and route Quit Game to a normal window close (OnClosing shuts the title down).
  void OnPreLaunchModule() override {
    REXLOG_INFO("[Azura] Active gamertag / user_name: '{}'", REXCVAR_GET(user_name));
    // Before the game polls a controller: which controller plays which player (player_assignment.h).
    azura::input::InstallPlayerAssignment(runtime(), [this](std::function<void()> work) {
      app_context().CallInUIThread(std::move(work));
    });
    // The mouse turns the camera directly while the game reads its input (input/mouse_look.h).
    azura::input::InstallMouseLook(runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                                             : nullptr);
    // The local co-op notices (a lost controller, a new one to assign) for as long as the game runs.
    app_context().CallInUIThread([this] {
      auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system()) : nullptr;
      if (!shutting_down_ && dialog_drawer_ && input && !coop_notices_) {
        coop_notices_ = new azura::LocalCoopNotices(dialog_drawer_, input, azura::input::InstalledPlayerAssignment(),
                                                    REXCVAR_GET(user_language), REXCVAR_GET(user_name),
                                                    title_font_, body_font_);
      }
    });
    // The frame rate limit of the Video menu (azura.toml) sets the guest's vertical blanks.
    azura::menu::ApplyFrameRateLimit();
    // The Video menu's sharpness shows as soon as it changes: the presenter takes it again on the
    // UI thread.
    azura::menu::SetSharpnessChangedHandler([this] {
      app_context().CallInUIThread([this] {
        auto* graphics_system = runtime() ? runtime()->graphics_system() : nullptr;
        if (!shutting_down_ && graphics_system && graphics_system->presenter()) {
          graphics_system->presenter()->RefreshGuestOutputSharpnessFromUIThread();
        }
      });
    });
    // The SDK writes one line per frame (frame time, FPS, draw calls...) to the CSV that
    // perf_log_csv names, once it is opened here; empty, the default, writes nothing.
    if (const std::string& csv = REXCVAR_GET(perf_log_csv); !csv.empty()) {
      rex::perf::SetCsvLogPath(csv);
      REXLOG_INFO("[Azura] Writing per-frame performance counters to {}", csv);
      // While recording, F8 marks the frame in the CSV, so a hitch the player noticed can be found
      // afterwards (controllers mark with Back, see perf_marker.h).
      perf_marker_ = std::make_unique<azura::PerfMarker>();
    }
#if defined(__APPLE__)
    // Again now that the window exists, so it gets the icon too (see platform_mac.mm).
    AutoConfigureMacAppIcon();
#endif
    // The upscaling row lists the effects the presenter draws on this GPU and backend.
    azura::menu::SetUpscalingSupport([this](const std::string& name) {
      auto* graphics_system = runtime() ? runtime()->graphics_system() : nullptr;
      auto* presenter = graphics_system ? graphics_system->presenter() : nullptr;
      if (!presenter) return true;
      rex::ui::Presenter::GuestOutputPaintConfig::Effect effect;
      return rex::ui::Presenter::GuestOutputPaintConfig::EffectFromName(name, effect) &&
             presenter->SupportsGuestOutputEffect(effect);
    });
    azura::menu::InstallMenuPatches(runtime());
    // A notice while the GPU backend compiles shaders in the background, from the shader storage
    // preload that runs right after this hook to the shaders of new areas while playing.
    if (auto* graphics_system = runtime()->graphics_system()) {
      graphics_system->SetShaderCompilationStartedCallback([this, graphics_system] {
        app_context().CallInUIThread([this, graphics_system] {
          if (!shutting_down_ && dialog_drawer_ && !shader_compile_overlay_) {
            shader_compile_overlay_ = new azura::ShaderCompileOverlay(
                dialog_drawer_, graphics_system, REXCVAR_GET(user_language), title_font_,
                body_font_, &shader_compile_overlay_);
          }
        });
      });
    }
    // Choosing Local co-op in the Options opens the Local co-op menu, on the UI thread.
    azura::script::SetLocalCoopHandler([this] {
      app_context().CallInUIThread([this] {
        auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system()) : nullptr;
        if (!shutting_down_ && dialog_drawer_ && input && !local_coop_menu_) {
          local_coop_menu_ = new azura::LocalCoopMenu(
              dialog_drawer_, input, azura::input::InstalledPlayerAssignment(), REXCVAR_GET(user_language),
              REXCVAR_GET(user_name), title_font_, body_font_, &local_coop_menu_);
        }
      });
    });
    // In the game's menus (not the title screen) the keyboard's Return and Escape accept and go back,
    // in every scene: the Options, their message boxes, the multiplayer preferences, the pause menu.
    rex::input::mnk::SetMenuKeysQuery(
        [] { return azura::script::WhereInMenus() == azura::script::Menus::kMenu; });
    // A controller's custom layout changes what its buttons do in play only; the menus and the
    // title screen keep its own buttons.
    rex::input::SetPadLayoutQuery(
        [] { return azura::script::WhereInMenus() == azura::script::Menus::kPlaying; });
    // Keyboard and mouse, and Controller, in the Options: their controls menus.
    azura::script::SetControlsMenuHandler([this](azura::script::ControlsMenu menu) {
      app_context().CallInUIThread([this, menu] {
        auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system()) : nullptr;
        if (!shutting_down_ && dialog_drawer_ && input && !controls_menu_) {
          controls_menu_ = new azura::ControlsMenu(
              dialog_drawer_, input, azura::input::InstalledPlayerAssignment(),
              menu == azura::script::ControlsMenu::kController ? azura::ControlsMenu::Kind::kController
                                                               : azura::ControlsMenu::Kind::kKeyboardMouse,
              REXCVAR_GET(user_language), title_font_, body_font_, &controls_menu_);
        }
      });
    });
    azura::script::SetQuitHandler([this] {
      app_context().CallInUIThread([this] {
        if (window()) {
          window()->RequestClose();
        }
      });
    });
    // Accepting the restart message box (Video settings that apply at start-up): close the game
    // like Quit Game and have it start again. A launcher that started it does so when it exits with
    // kRestartExitCode, so it keeps following the game; otherwise the game starts itself again.
    azura::script::SetRestartHandler([this] {
      app_context().CallInUIThread([this] {
        if (azura::StartedByLauncher()) {
          app_context().SetExitCode(azura::kRestartExitCode);
        } else {
          azura::RelaunchAfterExit();
        }
        if (window()) {
          window()->RequestClose();
        }
      });
    });
  }

  // The game module is running and guest memory holds its textures: start keeping the button
  // prompts in step with the player's device and bindings.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    // The launcher's Prepare shaders: the stored shaders are made with the game suspended, and the
    // runtime closes when they are done (shader_precompile.h).
    if (azura::ShaderPrecompile::Requested()) {
      shader_precompile_ = std::make_unique<azura::ShaderPrecompile>(runtime(), thread, [this] {
        app_context().CallInUIThread([this] {
          if (window()) {
            window()->RequestClose();
          }
        });
      });
      return;
    }
    button_prompts_ = std::make_unique<azura::prompts::ButtonPrompts>(
        runtime(), exe_dir_ / "assets" / "fonts" / "Barlow-SemiBold.ttf");
  }

  // The ImGui drawer the game's own dialogs are added to.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_drawer_ = drawer;
    // Draws nothing until the Video options turn the performance overlay on.
    perf_overlay_ = std::make_unique<azura::PerfOverlay>(drawer, &title_font_, &body_font_);
  }

  // Barlow (installed in assets/fonts) for the shader compilation notice and the performance
  // overlay, rasterized large and drawn smaller so it stays sharp on high-DPI displays. ImGui's default font stays the first one, so
  // the SDK's own overlays are unchanged.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    auto load = [&](const char* name) -> ImFont* {
      std::filesystem::path path = exe_dir_ / "assets" / "fonts" / name;
      std::error_code error_code;
      return std::filesystem::exists(path, error_code)
                 ? atlas->AddFontFromFileTTF(path.string().c_str(), 48.0f)
                 : nullptr;
    };
    title_font_ = load("Barlow-SemiBold.ttf");
    body_font_ = load("Barlow-Medium.ttf");
  }

  // The notice for unlocked achievements, in place of the SDK's toast.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    if (!imgui_drawer() || !immediate_drawer() || !runtime()) {
      return nullptr;
    }
    return std::make_unique<azura::AchievementToast>(imgui_drawer(), immediate_drawer(), runtime(),
                                                     REXCVAR_GET(user_language), title_font_,
                                                     body_font_);
  }

  // Stop the prompt thread before the runtime and its memory go away, and remove the shader
  // compilation notice before the ImGui drawer does.
  void OnShutdown() override {
    perf_overlay_.reset();
    button_prompts_.reset();
    perf_marker_.reset();
    shutting_down_ = true;
    azura::menu::SetSharpnessChangedHandler(nullptr);
    azura::menu::SetUpscalingSupport(nullptr);
    azura::script::SetLocalCoopHandler(nullptr);
    azura::script::SetControlsMenuHandler(nullptr);
    rex::input::mnk::SetMenuKeysQuery(nullptr);
    rex::input::SetPadLayoutQuery(nullptr);
    azura::input::InstallMouseLook(nullptr);
    if (auto* graphics_system = runtime()->graphics_system()) {
      graphics_system->SetShaderCompilationStartedCallback(nullptr);
    }
    delete shader_compile_overlay_;
    shader_compile_overlay_ = nullptr;
    delete local_coop_menu_;
    local_coop_menu_ = nullptr;
    delete controls_menu_;
    controls_menu_ = nullptr;
    delete coop_notices_;
    coop_notices_ = nullptr;
    shader_precompile_.reset();
  }


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
#if defined(__linux__)
    // The Linux build relaxes these guest memory protections; macOS keeps the SDK defaults.
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("writable_executable_memory", "true");
    rex::cvar::SetFlagByName("protect_on_release", "false");
#endif
    return paths;
  }

 private:
  std::unique_ptr<azura::prompts::ButtonPrompts> button_prompts_;
  std::filesystem::path exe_dir_;
  rex::ui::ImGuiDrawer* dialog_drawer_ = nullptr;
  std::unique_ptr<azura::PerfOverlay> perf_overlay_;
  ImFont* title_font_ = nullptr;
  ImFont* body_font_ = nullptr;
  // Deletes itself when it closes, clearing this pointer. Accessed on the UI thread only.
  azura::ShaderCompileOverlay* shader_compile_overlay_ = nullptr;
  // The Local co-op menu while it is open; deletes itself when it closes, clearing this pointer.
  // Accessed on the UI thread only.
  azura::LocalCoopMenu* local_coop_menu_ = nullptr;
  azura::ControlsMenu* controls_menu_ = nullptr;
  // The local co-op notices, from the game's start to its end. Accessed on the UI thread only.
  azura::LocalCoopNotices* coop_notices_ = nullptr;
  // Only with shader_precompile_only: follows the stored shaders being made, then closes the runtime.
  std::unique_ptr<azura::ShaderPrecompile> shader_precompile_;
  bool shutting_down_ = false;
  // Marks frames in the perf_log_csv CSV; only while it records.
  std::unique_ptr<azura::PerfMarker> perf_marker_;
};
