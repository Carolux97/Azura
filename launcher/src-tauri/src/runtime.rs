//! Starts the game runtime and relays its output to the interface.
//!
//! The runtime is the recompiled game executable, `azura`. It lives in the `runtime` folder of the
//! app data folder, next to the `azura.toml` the launcher writes, because the runtime reads its
//! settings from beside its own executable (`install.rs` puts it there).
//!
//! The folders for saves and for the shader cache are passed on the command line
//! (`--user_data_root`, `--cache_root`): the runtime picks them before it reads `azura.toml`, so
//! the config file cannot set them. The game folder goes on the command line too
//! (`--game_data_root`), so starting does not depend on `azura.toml` having been written.
//!
//! While the game runs, every line it prints is sent to the interface as a `game-output` event,
//! and its end as a `game-exit` event, so the interface can show a live log and explain a failed
//! start.

use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;

use serde::Serialize;
use tauri::{AppHandle, Emitter};

/// Name of the runtime executable inside the runtime folder.
pub const RUNTIME_EXECUTABLE: &str = "azura";

/// Event carrying one line of the game's output.
pub const OUTPUT_EVENT: &str = "game-output";

/// Event sent once when the game process ends.
pub const EXIT_EVENT: &str = "game-exit";

/// One line of output; `stream` is "stdout" or "stderr".
#[derive(Debug, Clone, Serialize)]
pub struct OutputLine {
    pub stream: &'static str,
    pub line: String,
}

/// How the game ended. `code` is missing when the process was killed by a signal.
#[derive(Debug, Clone, Serialize)]
pub struct ExitInfo {
    pub code: Option<i32>,
    pub success: bool,
}

/// Why the game could not start. Serialized as `{"kind": "...", "detail": "..."}` so the
/// interface can show a translated message for each kind.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", content = "detail", rename_all = "camelCase")]
pub enum LaunchError {
    /// The runtime executable is not in place yet.
    RuntimeMissing,
    /// A game started by this launcher is still running.
    AlreadyRunning,
    /// The saves folder could not be prepared (see `saves.rs`).
    SavesUnavailable(String),
    /// The operating system refused to start the process.
    StartFailed(String),
}

/// Tracks whether a game started by this launcher is running. Shared with the threads that
/// watch the process.
#[derive(Default)]
pub struct GameProcess {
    running: Arc<AtomicBool>,
}

/// Path of the runtime executable inside a runtime folder.
pub fn executable_path(runtime_dir: &Path) -> PathBuf {
    runtime_dir.join(RUNTIME_EXECUTABLE)
}

/// Folders the runtime is started with.
pub struct LaunchFolders<'a> {
    /// Holds the runtime executable and its `azura.toml`.
    pub runtime: &'a Path,
    /// The game files (`game_data_root`), when one is chosen.
    pub game: Option<&'a Path>,
    /// The player's saves (`user_data_root`).
    pub saves: &'a Path,
    /// The shader cache (`cache_root`).
    pub cache: &'a Path,
}

/// Command-line arguments that point the runtime at the saves and cache folders.
pub fn folder_arguments(folders: &LaunchFolders) -> Vec<String> {
    let mut args = Vec::new();
    if let Some(game) = folders.game {
        args.push(format!("--game_data_root={}", game.display()));
    }
    args.push(format!("--user_data_root={}", folders.saves.display()));
    args.push(format!("--cache_root={}", folders.cache.display()));
    args
}

/// Reads `source` line by line on its own thread and emits each line as a `game-output` event.
fn relay_lines(app: AppHandle, stream: &'static str, source: impl Read + Send + 'static) {
    thread::spawn(move || {
        for line in BufReader::new(source).lines().map_while(Result::ok) {
            let _ = app.emit(OUTPUT_EVENT, OutputLine { stream, line });
        }
    });
}

impl GameProcess {
    /// Whether a game started by this launcher is running.
    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }

    /// Starts the runtime from its folder, so it finds `azura.toml` and writes its logs there.
    /// Returns once the process has started; output and exit arrive later as events.
    pub fn launch(&self, app: AppHandle, folders: &LaunchFolders) -> Result<(), LaunchError> {
        let runtime_dir = folders.runtime;
        let executable = executable_path(runtime_dir);
        if !executable.is_file() {
            return Err(LaunchError::RuntimeMissing);
        }
        // Claim the slot atomically, so two quick clicks cannot start two games.
        if self.running.swap(true, Ordering::SeqCst) {
            return Err(LaunchError::AlreadyRunning);
        }

        let mut cmd = Command::new(&executable);
        cmd.args(folder_arguments(folders))
            .current_dir(runtime_dir)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());

        #[cfg(target_os = "linux")]
        {
            cmd.env("__NV_PRIME_RENDER_OFFLOAD", "1");
            cmd.env("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
            cmd.env("__VK_LAYER_NV_optimus", "NVIDIA_only");
            if std::path::Path::new("/usr/share/vulkan/icd.d/nvidia_icd.json").is_file() {
                cmd.env("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/nvidia_icd.json");
            }
        }

        let spawned = cmd.spawn();
        let mut child = match spawned {
            Ok(child) => child,
            Err(error) => {
                self.running.store(false, Ordering::SeqCst);
                return Err(LaunchError::StartFailed(error.to_string()));
            }
        };

        if let Some(stdout) = child.stdout.take() {
            relay_lines(app.clone(), "stdout", stdout);
        }
        if let Some(stderr) = child.stderr.take() {
            relay_lines(app.clone(), "stderr", stderr);
        }

        let running = Arc::clone(&self.running);
        thread::spawn(move || {
            let exit = match child.wait() {
                Ok(status) => ExitInfo { code: status.code(), success: status.success() },
                Err(_) => ExitInfo { code: None, success: false },
            };
            running.store(false, Ordering::SeqCst);
            let _ = app.emit(EXIT_EVENT, exit);
        });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TempFolder;

    #[test]
    fn passes_the_saves_and_cache_folders() {
        let runtime = TempFolder::new("runtime-args");
        let folders = LaunchFolders {
            runtime: &runtime,
            game: Some(Path::new("/Games/Gears of War 3")),
            saves: Path::new("/Users/me/Documents/Games Saves/Gears3-Azura"),
            cache: Path::new("/tmp/cache"),
        };
        assert_eq!(
            folder_arguments(&folders),
            [
                "--game_data_root=/Games/Gears of War 3",
                "--user_data_root=/Users/me/Documents/Games Saves/Gears3-Azura",
                "--cache_root=/tmp/cache"
            ]
        );
    }

    #[test]
    fn launch_errors_serialize_with_their_kind() {
        let json = |error: LaunchError| serde_json::to_string(&error).unwrap();
        assert_eq!(json(LaunchError::RuntimeMissing), r#"{"kind":"runtimeMissing"}"#);
        assert_eq!(
            json(LaunchError::StartFailed("denied".into())),
            r#"{"kind":"startFailed","detail":"denied"}"#
        );
    }
}
