//! Azura launcher: the native side of the Tauri app.
//!
//! The interface lives in `launcher/src` as plain HTML, CSS and JavaScript. This crate opens the
//! window that shows it and exposes the commands the interface calls through `invoke`.

mod achievements;
mod builder;
mod disc_image;
mod game_folder;
mod gamepad;
mod install;
mod release_notes;
#[cfg(test)]
mod test_support;
mod runtime;
mod saves;
mod settings;
mod system;
mod toolchain;
mod updater;
mod versions;

use std::path::Path;

use achievements::AchievementsState;
use game_folder::FolderCheck;
use install::RuntimeStatus;
use runtime::{GameProcess, LaunchError, LaunchFolders};
use saves::{BackupInfo, SaveStatus, SavesError};
use settings::{Settings, SettingsPaths};
use system::SystemInfo;
use toolchain::CompilerStatus;
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

/// Opens the native file picker for a disc image (`.iso`) of the game and checks it. Returns
/// `None` when the user cancels. `async` for the same reason as `pick_game_folder`.
#[tauri::command]
async fn pick_game_image(app: tauri::AppHandle) -> Option<FolderCheck> {
    let picked = app.dialog().file().add_filter("Xbox 360 disc image", &["iso"]).blocking_pick_file()?;
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

/// Folder that keeps the achievements read from each game build (see `achievements.rs`).
fn achievements_dir(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    Ok(app.path().app_data_dir().map_err(|e| e.to_string())?.join("achievements"))
}

/// The player's achievements for the Achievements section, for the game in `game_folder`, with
/// their texts in `game_language` (an engine code such as "ESN"), else in the launcher's language
/// (`ui_locale`), else in English. The interface passes the folder and the language it shows,
/// because a change it has just made may not be saved yet. The first call for a game build has the
/// runtime read the achievements from the game (about a second); later calls read what it kept.
///
/// `async`: it hashes the game executable to know its build, and may run the runtime.
#[tauri::command]
async fn get_achievements(
    app: tauri::AppHandle,
    game_folder: Option<String>,
    game_language: Option<String>,
    ui_locale: String,
) -> Result<AchievementsState, String> {
    let Some(folder) = game_folder else {
        return Ok(AchievementsState::NoGame);
    };
    let FolderCheck::Ok { folder, version_id, .. } = game_folder::check_folder(Path::new(&folder), &Registry::bundled())
    else {
        return Ok(AchievementsState::NoGame);
    };
    let runtime = runtime_dir(&app)?;
    let export_dir = achievements_dir(&app)?.join(&version_id);
    let export = match achievements::read_export(&export_dir) {
        Some(export) => export,
        // No runtime yet, or one older than the build: an older one may not know
        // --export_achievements and would start the game instead. The section asks for it to be
        // installed, as Play would do first.
        None if !runtime::executable_path(&runtime).is_file()
            || install::status(&runtime, install::find_build().as_deref()).update_available =>
        {
            return Ok(AchievementsState::NoRuntime)
        }
        None => match achievements::export(&runtime, Path::new(&folder), &export_dir) {
            Ok(export) => export,
            Err(detail) => return Ok(AchievementsState::Unavailable { detail }),
        },
    };
    let saves = settings_paths(&app)?.saves_folder()?;
    let unlocks = achievements::read_unlocks(&achievements::unlocks_path(&saves, &export.title_id));
    let languages: Vec<&str> = game_language
        .as_deref()
        .and_then(achievements::text_language)
        .into_iter()
        .chain(std::iter::once(ui_locale.as_str()))
        .collect();
    Ok(achievements::build_state(&export, &export_dir, &unlocks, &languages))
}

/// Where the runtime comes from. A developer's explicit choice wins (`AZURA_RUNTIME_BUILD`, then
/// `AZURA_KIT`); then a project build, when the launcher runs from a checkout that has one; then
/// the player kit (bundled with the launcher, or the project's kit build), which is how players
/// get it.
enum RuntimeSource {
    Build(std::path::PathBuf),
    Kit(std::path::PathBuf),
    None,
}

fn runtime_source(app: &tauri::AppHandle) -> RuntimeSource {
    if std::env::var_os("AZURA_RUNTIME_BUILD").is_some() {
        if let Some(build) = install::find_build() {
            return RuntimeSource::Build(build);
        }
    }
    if std::env::var_os(builder::KIT_ENV).is_none() {
        if let Some(build) = install::find_build() {
            return RuntimeSource::Build(build);
        }
    }
    let resources = app.path().resource_dir().ok();
    builder::find_kit(resources.as_deref()).map(RuntimeSource::Kit).unwrap_or(RuntimeSource::None)
}

/// The game build in the saved game folder, for building the runtime from it: its folder and its
/// entry in the registry.
fn game_build(app: &tauri::AppHandle) -> Result<(std::path::PathBuf, versions::KnownVersion), builder::BuildError> {
    let settings = settings_paths(app).and_then(|paths| paths.load()).map_err(builder::BuildError::Io)?;
    let folder = settings.game_folder.ok_or(builder::BuildError::UnsupportedGame)?;
    let registry = Registry::bundled();
    let FolderCheck::Ok { folder, version_id, .. } = game_folder::check_folder(Path::new(&folder), &registry) else {
        return Err(builder::BuildError::UnsupportedGame);
    };
    let version = registry.versions.into_iter().find(|version| version.id == version_id).ok_or(builder::BuildError::UnsupportedGame)?;
    Ok((std::path::PathBuf::from(folder), version))
}

/// The work folder the runtime of a game build is built in.
fn work_dir(app: &tauri::AppHandle, version: &versions::KnownVersion) -> Result<std::path::PathBuf, String> {
    Ok(app.path().app_data_dir().map_err(|e| e.to_string())?.join("build").join(&version.id))
}

/// Reports whether the runtime is installed and whether it has to be installed again: from a
/// project build, when the build is newer; from the kit, when the installed runtime was not built
/// from this kit for the game in the saved folder.
#[tauri::command]
async fn get_runtime_status(app: tauri::AppHandle) -> Result<RuntimeStatus, String> {
    let dir = runtime_dir(&app)?;
    match runtime_source(&app) {
        RuntimeSource::Build(build) => Ok(install::status(&dir, Some(&build))),
        RuntimeSource::None => Ok(install::status(&dir, None)),
        RuntimeSource::Kit(kit) => {
            let mut status = install::status(&dir, None);
            status.source = "kit";
            status.build_found = true;
            let kind = match (builder::read_kit(&kit), game_build(&app)) {
                (Ok(kit), Ok((_, version))) => {
                    let work = work_dir(&app, &version)?;
                    let state = builder::read_state(&work);
                    // Installed means built here from the kit for this game: a runtime folder
                    // that holds an executable from elsewhere (a copied project build) is not.
                    status.installed &= state.as_ref().is_some_and(|state| state.xex_sha256 == version.xex_sha256);
                    let kept = state.as_ref().is_some_and(|state| builder::objects_kept(&work, state));
                    let compiler = toolchain::compiler_version().unwrap_or_default();
                    builder::update_kind(state.as_ref(), &kit.keys, &version.xex_sha256, &compiler, status.installed, kept)
                }
                _ => builder::UpdateKind::Build,
            };
            status.update_available = kind != builder::UpdateKind::None;
            status.update_kind = match kind {
                builder::UpdateKind::None => None,
                builder::UpdateKind::Link => Some("link"),
                builder::UpdateKind::Build => Some("build"),
            };
            Ok(status)
        }
    }
}

/// Only one runtime build runs at a time.
static BUILDING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Installs (or updates) the runtime: copies a project build (see `install.rs`), or builds it here
/// from the player kit and the player's default.xex (see `builder.rs`), sending its progress as
/// `runtime-build-progress` events. `async`: a build takes minutes.
#[tauri::command]
async fn install_runtime(
    app: tauri::AppHandle,
    process: tauri::State<'_, GameProcess>,
) -> Result<RuntimeStatus, builder::BuildError> {
    if process.is_running() {
        return Err(builder::BuildError::AlreadyRunning);
    }
    let dir = runtime_dir(&app).map_err(builder::BuildError::Io)?;
    match runtime_source(&app) {
        RuntimeSource::Build(build) => {
            install::install(&build, &dir).map_err(builder::BuildError::Io)?;
        }
        RuntimeSource::Kit(kit) => {
            let (game, version) = game_build(&app)?;
            let work = work_dir(&app, &version).map_err(builder::BuildError::Io)?;
            let events = app.clone();
            tauri::async_runtime::spawn_blocking(move || {
                let folders = builder::BuildFolders { kit: &kit, game: &game, work: &work, runtime: &dir };
                builder::build(&folders, &version.manifest, &version.xex_sha256, &events, &BUILDING)
            })
            .await
            .map_err(|e| builder::BuildError::Io(e.to_string()))??;
        }
        RuntimeSource::None => return Err(builder::BuildError::NoKit),
    }
    get_runtime_status(app).await.map_err(builder::BuildError::Io)
}

/// Shows a failed build's log in the system's file manager. Only a `build.log` inside the
/// launcher's build folder is shown: the path comes from the interface.
#[tauri::command]
async fn show_build_log(app: tauri::AppHandle, path: String) -> Result<(), String> {
    let builds = app.path().app_data_dir().map_err(|e| e.to_string())?.join("build");
    let log = std::path::PathBuf::from(&path).canonicalize().map_err(|e| e.to_string())?;
    let builds = builds.canonicalize().map_err(|e| e.to_string())?;
    if !log.starts_with(&builds) || log.file_name().and_then(|name| name.to_str()) != Some("build.log") {
        return Err(format!("not a build log: {path}"));
    }
    #[cfg(target_os = "macos")]
    let status = std::process::Command::new("open").arg("-R").arg(&log).status();
    #[cfg(not(target_os = "macos"))]
    let status = std::process::Command::new("xdg-open").arg(log.parent().unwrap_or(&log)).status();
    status.map(|_| ()).map_err(|e| e.to_string())
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
    // The runtime reads the render resolution from render_resolution only.
    paths.migrate_render_resolution().map_err(LaunchError::StartFailed)?;
    let game = paths.load().map_err(LaunchError::StartFailed)?.game_folder.map(std::path::PathBuf::from);
    let saves = paths.saves_folder().map_err(LaunchError::SavesUnavailable)?;
    saves::prepare(&saves, &cache).map_err(|e| LaunchError::SavesUnavailable(format!("{e:?}")))?;
    let folders = LaunchFolders { runtime: &runtime, game: game.as_deref(), saves: &saves, cache: &cache };
    process.launch(app, &folders)
}

/// Returns the current state of saved games.
#[tauri::command]
async fn get_save_status(app: tauri::AppHandle) -> Result<SaveStatus, String> {
    let paths = settings_paths(&app)?;
    let saves = paths.saves_folder()?;
    Ok(saves::inspect_saves(&app, &saves))
}

/// The saves folder, with a failure to find it as a saves error for the interface.
fn saves_folder(app: &tauri::AppHandle) -> Result<std::path::PathBuf, SavesError> {
    settings_paths(app).and_then(|paths| paths.saves_folder()).map_err(SavesError::Io)
}

/// Removes orphaned checkpoint headers, after backing up the saves; returns how many it removed.
/// Errors are `{"kind": "...", "detail": "..."}` (`SavesError`), worded by the interface.
#[tauri::command]
async fn repair_saves(app: tauri::AppHandle) -> Result<usize, SavesError> {
    saves::repair_saves_internal(&app, &saves_folder(&app)?)
}

/// Resets saved games so the player can start a clean campaign, backing up first.
#[tauri::command]
async fn reset_saves(app: tauri::AppHandle) -> Result<(), SavesError> {
    saves::reset_saves_internal(&app, &saves_folder(&app)?)
}

/// Creates a backup of the current saves (keeping the newest `saves::MAX_BACKUPS`).
#[tauri::command]
async fn create_save_backup(app: tauri::AppHandle) -> Result<BackupInfo, SavesError> {
    saves::create_backup_internal(&app, &saves_folder(&app)?)
}

/// Restores a chosen backup.
#[tauri::command]
async fn restore_save_backup(app: tauri::AppHandle, backup_id: String) -> Result<(), SavesError> {
    saves::restore_backup_internal(&app, &saves_folder(&app)?, &backup_id)
}

/// Opens the save games folder in the system file manager.
#[tauri::command]
async fn open_saves_folder(app: tauri::AppHandle) -> Result<(), String> {
    let paths = settings_paths(&app)?;
    let saves = paths.saves_folder()?;
    saves::open_saves_folder_internal(&saves)
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

/// Whether there is a working C++ compiler to build the runtime with, and how to install one
/// otherwise (see `toolchain.rs`). `async`: it compiles a small test program.
#[tauri::command]
async fn get_compiler_status() -> CompilerStatus {
    toolchain::status()
}

/// Installs the compiler: opens Apple's Command Line Tools dialog on macOS (the interface then
/// checks the status until they are there), or runs the package manager on Linux and waits.
#[tauri::command]
async fn install_compiler() -> Result<(), String> {
    toolchain::install()
}

/// Describes this Mac (chip, memory), for the status lines and the "Auto" preset.
#[tauri::command]
async fn get_system_info() -> SystemInfo {
    system::system_info()
}

/// What this launcher version brings, for the "What's new" panel (see `release_notes.rs`).
/// `None` when the changelog has no section for it.
#[tauri::command]
async fn get_release_notes(app: tauri::AppHandle) -> Option<release_notes::ReleaseNotes> {
    release_notes::current(&app.package_info().version.to_string())
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
            pick_game_image,
            check_game_folder,
            load_settings,
            save_settings,
            get_system_info,
            get_release_notes,
            get_runtime_status,
            install_runtime,
            launch_game,
            pick_saves_folder,
            change_saves_folder,
            check_for_update,
            install_update,
            get_save_status,
            repair_saves,
            reset_saves,
            create_save_backup,
            restore_save_backup,
            open_saves_folder,
            get_achievements,
            get_compiler_status,
            install_compiler,
            show_build_log
        ])
        .setup(|app| {
            // Controllers navigate the interface (gamepad.rs).
            gamepad::spawn(app.handle().clone());
            // A runtime install the launcher was closed in the middle of is finished first.
            if let Ok(dir) = runtime_dir(app.handle()) {
                builder::recover(&dir);
            }
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
