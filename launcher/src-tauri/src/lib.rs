//! Azura launcher: the native side of the Tauri app.
//!
//! The interface lives in `launcher/src` as plain HTML, CSS and JavaScript. This crate opens the
//! window that shows it and exposes the commands the interface calls through `invoke`.

mod game_folder;
mod install;
#[cfg(test)]
mod test_support;
mod runtime;
mod saves;
mod settings;
mod system;
mod updater;
mod versions;

use std::path::Path;

use game_folder::FolderCheck;
use install::RuntimeStatus;
use runtime::{GameProcess, LaunchError, LaunchFolders};
use saves::SavesError;
use settings::{Settings, SettingsPaths};
use system::SystemInfo;
use tauri::Manager;
use tauri_plugin_dialog::DialogExt;
use updater::UpdateInfo;
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

/// Folder for the runtime's shader cache, under the app data folder.
fn cache_dir(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    Ok(app.path().app_data_dir().map_err(|e| e.to_string())?.join("cache"))
}

/// Reports whether the runtime is installed and whether a newer build can be installed.
#[tauri::command]
async fn get_runtime_status(app: tauri::AppHandle) -> Result<RuntimeStatus, String> {
    Ok(install::status(&runtime_dir(&app)?, install::find_build().as_deref()))
}

/// Installs (or updates) the runtime from the project's build (see `install.rs`). `async`: it
/// copies about a hundred megabytes.
#[tauri::command]
async fn install_runtime(app: tauri::AppHandle) -> Result<RuntimeStatus, String> {
    let build = install::find_build().ok_or_else(|| "no runtime build found".to_string())?;
    let dir = runtime_dir(&app)?;
    install::install(&build, &dir)?;
    Ok(install::status(&dir, Some(&build)))
}

/// Starts the game with the chosen saves folder. Its output and its end arrive as `game-output`
/// and `game-exit` events.
#[tauri::command]
async fn launch_game(
    app: tauri::AppHandle,
    process: tauri::State<'_, GameProcess>,
) -> Result<(), LaunchError> {
    let runtime = runtime_dir(&app).map_err(LaunchError::StartFailed)?;
    let cache = cache_dir(&app).map_err(LaunchError::StartFailed)?;
    let paths = settings_paths(&app).map_err(LaunchError::StartFailed)?;
    let game = paths.load().map_err(LaunchError::StartFailed)?.game_folder.map(std::path::PathBuf::from);
    let saves = paths.saves_folder().map_err(LaunchError::SavesUnavailable)?;
    saves::prepare(&saves, &cache).map_err(|e| LaunchError::SavesUnavailable(format!("{e:?}")))?;
    let folders = LaunchFolders { runtime: &runtime, game: game.as_deref(), saves: &saves, cache: &cache };
    process.launch(app, &folders)
}

/// Opens the folder picker for a new saves folder. Returns `None` when the user cancels; nothing
/// moves until `change_saves_folder`.
#[tauri::command]
async fn pick_saves_folder(app: tauri::AppHandle) -> Option<String> {
    let picked = app.dialog().file().blocking_pick_folder()?;
    Some(picked.into_path().ok()?.to_string_lossy().into_owned())
}

/// Moves the saves to `folder` and makes it the saves folder. With saves already in `folder` it
/// fails with `destinationHasSaves` unless `replace` is set (the interface asks first).
#[tauri::command]
async fn change_saves_folder(
    app: tauri::AppHandle,
    process: tauri::State<'_, GameProcess>,
    folder: String,
    replace: bool,
) -> Result<String, SavesError> {
    if process.is_running() {
        return Err(SavesError::Io("the game is running".into()));
    }
    let paths = settings_paths(&app).map_err(SavesError::Io)?;
    let current = paths.saves_folder().map_err(SavesError::Io)?;
    let target = std::path::PathBuf::from(&folder);
    // Saves still in the runtime's old folder count as the current ones.
    let source = if saves::has_saves(&current) { current } else { saves::legacy_folder() };
    saves::move_saves(&source, &target, replace)?;
    std::fs::create_dir_all(&target).map_err(|e| SavesError::Io(e.to_string()))?;
    paths.set_saves_folder(&target).map_err(SavesError::Io)?;
    Ok(folder)
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

/// Checks whether a newer launcher version is published (see `updater.rs`).
#[tauri::command]
async fn check_for_update(app: tauri::AppHandle) -> Result<Option<UpdateInfo>, String> {
    updater::check(&app).await
}

/// Installs the newer launcher version and restarts into it.
#[tauri::command]
async fn install_update(app: tauri::AppHandle) -> Result<(), String> {
    updater::install(app).await
}

/// Builds and runs the Tauri application. Blocks until the last window closes.
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .manage(GameProcess::default())
        .invoke_handler(tauri::generate_handler![
            pick_game_folder,
            check_game_folder,
            load_settings,
            save_settings,
            get_system_info,
            get_runtime_status,
            install_runtime,
            launch_game,
            pick_saves_folder,
            change_saves_folder,
            check_for_update,
            install_update
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
