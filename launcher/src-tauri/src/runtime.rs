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
//!
//! Some settings only apply when the game starts, and the game offers to restart when they change.
//! The runtime is started with `AZURA_LAUNCHER=1`, which tells it to leave the restart to the
//! launcher: it exits with `RESTART_EXIT_CODE`, and the launcher starts it again with the same
//! command, sending a `game-restart` event instead of `game-exit`. The game stays a child of the
//! launcher, so its output and its end keep being followed.

use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
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

/// Event sent when the game exits asking to be started again, right before it is.
pub const RESTART_EVENT: &str = "game-restart";

/// Environment variable that tells the runtime the launcher restarts it (`runtime/src/relaunch.h`).
pub const LAUNCHER_ENV: &str = "AZURA_LAUNCHER";

/// Exit code with which the runtime asks to be started again (`kRestartExitCode` in
/// `runtime/src/relaunch.h`; EX_TEMPFAIL in sysexits.h).
pub const RESTART_EXIT_CODE: i32 = 75;

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

/// Whether a game that ended with `exit` asks to be started again.
pub fn asks_for_restart(exit: &ExitInfo) -> bool {
    exit.code == Some(RESTART_EXIT_CODE)
}

/// Where the game's output, restarts and end go: the interface (as events) in the app, a recorder
/// in the tests.
pub trait GameEvents: Clone + Send + 'static {
    fn output(&self, line: OutputLine);
    fn restart(&self);
    fn exit(&self, exit: ExitInfo);
}

impl GameEvents for AppHandle {
    fn output(&self, line: OutputLine) {
        let _ = self.emit(OUTPUT_EVENT, line);
    }

    fn restart(&self) {
        let _ = self.emit(RESTART_EVENT, ());
    }

    fn exit(&self, exit: ExitInfo) {
        let _ = self.emit(EXIT_EVENT, exit);
    }
}

/// Reads `source` line by line on its own thread and sends each line to `events`.
fn relay_lines<E: GameEvents>(events: E, stream: &'static str, source: impl Read + Send + 'static) {
    thread::spawn(move || {
        for line in BufReader::new(source).lines().map_while(Result::ok) {
            events.output(OutputLine { stream, line });
        }
    });
}

/// Starts the runtime with `args` from `runtime_dir`, relaying its output.
fn spawn_game<E: GameEvents>(
    events: &E,
    executable: &Path,
    args: &[String],
    runtime_dir: &Path,
) -> std::io::Result<Child> {
    let mut command = Command::new(executable);
    command
        .args(args)
        .current_dir(runtime_dir)
        .env(LAUNCHER_ENV, "1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    // On Linux laptops with an NVIDIA GPU next to an integrated one, run the game on the NVIDIA
    // one (PRIME render offload). Only when its Vulkan driver is installed, so other GPUs are left
    // alone.
    #[cfg(target_os = "linux")]
    {
        const NVIDIA_ICD: &str = "/usr/share/vulkan/icd.d/nvidia_icd.json";
        if Path::new(NVIDIA_ICD).is_file() {
            command
                .env("__NV_PRIME_RENDER_OFFLOAD", "1")
                .env("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
                .env("__VK_LAYER_NV_optimus", "NVIDIA_only")
                .env("VK_DRIVER_FILES", NVIDIA_ICD);
        }
    }
    let mut child = command.spawn()?;
    if let Some(stdout) = child.stdout.take() {
        relay_lines(events.clone(), "stdout", stdout);
    }
    if let Some(stderr) = child.stderr.take() {
        relay_lines(events.clone(), "stderr", stderr);
    }
    Ok(child)
}

/// How a process waited on with `child.wait()` ended.
fn exit_info(child: &mut Child) -> ExitInfo {
    match child.wait() {
        Ok(status) => ExitInfo { code: status.code(), success: status.success() },
        Err(_) => ExitInfo { code: None, success: false },
    }
}

/// Waits for the game started as `child` to end, and starts it again with the same command for as
/// long as it asks to be. Returns how the last run ended.
fn watch_game<E: GameEvents>(
    events: &E,
    mut child: Child,
    executable: &Path,
    args: &[String],
    runtime_dir: &Path,
) -> ExitInfo {
    loop {
        let exit = exit_info(&mut child);
        if !asks_for_restart(&exit) {
            return exit;
        }
        events.restart();
        match spawn_game(events, executable, args, runtime_dir) {
            Ok(next) => child = next,
            Err(error) => {
                let line = format!("Cannot restart the game: {error}");
                events.output(OutputLine { stream: "stderr", line });
                return ExitInfo { code: None, success: false };
            }
        }
    }
}

impl GameProcess {
    /// Whether a game started by this launcher is running.
    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }

    /// Starts the runtime from its folder, so it finds `azura.toml` and writes its logs there.
    /// Returns once the process has started; output, restarts and exit arrive later as events.
    pub fn launch<E: GameEvents>(
        &self,
        events: E,
        folders: &LaunchFolders,
    ) -> Result<(), LaunchError> {
        let runtime_dir = folders.runtime;
        let executable = executable_path(runtime_dir);
        if !executable.is_file() {
            return Err(LaunchError::RuntimeMissing);
        }
        // Claim the slot atomically, so two quick clicks cannot start two games.
        if self.running.swap(true, Ordering::SeqCst) {
            return Err(LaunchError::AlreadyRunning);
        }

        let args = folder_arguments(folders);
        let runtime_dir = runtime_dir.to_path_buf();
        let child = match spawn_game(&events, &executable, &args, &runtime_dir) {
            Ok(child) => child,
            Err(error) => {
                self.running.store(false, Ordering::SeqCst);
                return Err(LaunchError::StartFailed(error.to_string()));
            }
        };

        // The slot stays claimed while the game restarts, so Play cannot start a second game.
        let running = Arc::clone(&self.running);
        thread::spawn(move || {
            let exit = watch_game(&events, child, &executable, &args, &runtime_dir);
            running.store(false, Ordering::SeqCst);
            events.exit(exit);
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

    /// Everything a launch sent, in order.
    #[derive(Clone, Default)]
    struct Recorder(Arc<std::sync::Mutex<Vec<String>>>);

    impl GameEvents for Recorder {
        fn output(&self, line: OutputLine) {
            self.0.lock().unwrap().push(format!("{}: {}", line.stream, line.line));
        }

        fn restart(&self) {
            self.0.lock().unwrap().push("restart".into());
        }

        fn exit(&self, exit: ExitInfo) {
            self.0.lock().unwrap().push(format!("exit {:?}", exit.code));
        }
    }

    impl Recorder {
        /// Waits up to five seconds for the `exit` event, then returns everything recorded.
        fn until_exit(&self) -> Vec<String> {
            for _ in 0..500 {
                if self.0.lock().unwrap().iter().any(|event| event.starts_with("exit")) {
                    break;
                }
                thread::sleep(std::time::Duration::from_millis(10));
            }
            // The output relays may still be on their last lines.
            thread::sleep(std::time::Duration::from_millis(100));
            self.0.lock().unwrap().clone()
        }
    }

    /// Writes a stand-in runtime: a shell script run as `azura` from `runtime`.
    #[cfg(unix)]
    fn fake_runtime(runtime: &Path, script: &str) {
        use std::os::unix::fs::PermissionsExt;
        let executable = executable_path(runtime);
        std::fs::write(&executable, format!("#!/bin/sh\n{script}\n")).unwrap();
        std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o755)).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn starts_the_game_again_while_it_asks_for_a_restart() {
        let runtime = TempFolder::new("runtime-restart");
        // The first run asks for a restart, the second one ends normally. Each prints the variable
        // that tells it the launcher restarts it.
        fake_runtime(
            &runtime,
            &format!(
                "if [ -f started ]; then echo \"second $AZURA_LAUNCHER\"; exit 0; fi\n\
                 touch started; echo \"first $AZURA_LAUNCHER\"; exit {RESTART_EXIT_CODE}"
            ),
        );
        let folders =
            LaunchFolders { runtime: &runtime, game: None, saves: &runtime, cache: &runtime };
        let process = GameProcess::default();
        let recorder = Recorder::default();
        process.launch(recorder.clone(), &folders).unwrap();

        let events = recorder.until_exit();
        let position = |event: &str| events.iter().position(|e| e == event);
        assert_eq!(events.iter().filter(|e| *e == "restart").count(), 1, "{events:?}");
        assert_eq!(events.iter().filter(|e| e.starts_with("exit")).count(), 1, "{events:?}");
        assert_eq!(events.last().map(String::as_str), Some("exit Some(0)"), "{events:?}");
        assert!(position("stdout: first 1") < position("restart"), "{events:?}");
        assert!(position("restart") < position("stdout: second 1"), "{events:?}");
        assert!(!process.is_running());
    }

    #[cfg(unix)]
    #[test]
    fn reports_any_other_exit_without_restarting() {
        let runtime = TempFolder::new("runtime-exit");
        fake_runtime(&runtime, "echo crashed >&2; exit 3");
        let folders =
            LaunchFolders { runtime: &runtime, game: None, saves: &runtime, cache: &runtime };
        let process = GameProcess::default();
        let recorder = Recorder::default();
        process.launch(recorder.clone(), &folders).unwrap();

        let events = recorder.until_exit();
        assert!(!events.contains(&"restart".to_string()), "{events:?}");
        assert!(events.contains(&"stderr: crashed".to_string()), "{events:?}");
        assert!(events.contains(&"exit Some(3)".to_string()), "{events:?}");
        assert!(!process.is_running());
    }

    #[test]
    fn only_the_restart_exit_code_asks_for_a_restart() {
        let exit = |code| ExitInfo { code, success: code == Some(0) };
        assert!(asks_for_restart(&exit(Some(RESTART_EXIT_CODE))));
        assert!(!asks_for_restart(&exit(Some(0))));
        assert!(!asks_for_restart(&exit(Some(1))));
        assert!(!asks_for_restart(&exit(None)));
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
