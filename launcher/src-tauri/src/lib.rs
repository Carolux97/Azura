//! Azura launcher: the native side of the Tauri app.
//!
//! The interface lives in `launcher/src` as plain HTML, CSS and JavaScript. This crate opens the
//! window that shows it and exposes the commands the interface calls through `invoke`.

mod game_folder;
#[cfg(test)]
mod test_support;
mod runtime;
mod settings;
mod system;
mod versions;

use std::path::Path;

use game_folder::FolderCheck;
use runtime::{GameProcess, LaunchError, RuntimeStatus};
use settings::{Settings, SettingsPaths};
use system::SystemInfo;
use tauri::Manager;
use tauri_plugin_dialog::DialogExt;
use versions::Registry;

/// Opens the native folder picker and checks the chosen folder.
///
/// Returns `None` when the user cancels. It is `async` so Tauri runs it off the main thread: the
/// blocking picker would otherwise freeze the window it belongs to.
#[tauri::command]
async fn pick_game_folder(app: tauri::AppHandle) -> Option<FolderCheck> {
    let picked = app.dialog().file().blocking_pick_folder()?;
    let path = picked.into_path().ok()?;
    Some(game_folder::check_folder(&path, &Registry::bundled()))
}

/// Checks a folder chosen earlier, so the launcher can confirm on start-up that it is still there
/// and still holds the game.
///
/// `async` for the same reason as `pick_game_folder`: hashing the executable reads ~20 MB, which
/// must not stall the window.
#[tauri::command]
async fn check_game_folder(path: String) -> FolderCheck {
    game_folder::check_folder(Path::new(&path), &Registry::bundled())
}

/// Paths of the config files under this app's data folder.
fn settings_paths(app: &tauri::AppHandle) -> Result<SettingsPaths, String> {
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    Ok(SettingsPaths::new(&dir))
}

/// Folder that holds the runtime executable and its `azura.toml` (see `runtime.rs`).
fn runtime_dir(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    Ok(app.path().app_data_dir().map_err(|e| e.to_string())?.join("runtime"))
}

/// Reports whether the runtime executable is installed.
#[tauri::command]
async fn get_runtime_status(app: tauri::AppHandle) -> Result<RuntimeStatus, String> {
    Ok(runtime::status(&runtime_dir(&app)?))
}

/// Starts the game. Its output and its end arrive as `game-output` and `game-exit` events.
#[tauri::command]
async fn launch_game(
    app: tauri::AppHandle,
    process: tauri::State<'_, GameProcess>,
) -> Result<(), LaunchError> {
    let dir = runtime_dir(&app).map_err(LaunchError::StartFailed)?;
    process.launch(app, &dir)
}

/// Loads the launcher and runtime settings (see `settings.rs`).
#[tauri::command]
async fn load_settings(app: tauri::AppHandle) -> Result<Settings, String> {
    settings_paths(&app)?.load()
}

/// Saves the launcher and runtime settings, keeping whatever else `azura.toml` holds.
#[tauri::command]
async fn save_settings(app: tauri::AppHandle, settings: Settings) -> Result<(), String> {
    settings_paths(&app)?.save(&settings)
}

/// Describes this Mac (chip, memory), for the status lines and the "Auto" preset.
#[tauri::command]
async fn get_system_info() -> SystemInfo {
    system::system_info()
}

/// Builds and runs the Tauri application. Blocks until the last window closes.
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(GameProcess::default())
        .invoke_handler(tauri::generate_handler![
            pick_game_folder,
            check_game_folder,
            load_settings,
            save_settings,
            get_system_info,
            get_runtime_status,
            launch_game
        ])
        .setup(|app| {
            // Debug builds log to the terminal that started them; release builds stay quiet.
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the Azura launcher");
}
