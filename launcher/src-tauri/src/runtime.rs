//! Starts the game runtime and relays its output to the interface.
//!
//! The runtime is the recompiled game executable, `azura`. It lives in the `runtime` folder of the
//! app data folder, next to the `azura.toml` the launcher writes, because the runtime reads its
//! settings from beside its own executable. Building and placing it there is the installation
//! step; until then the launcher reports it as missing.
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

/// Whether the runtime is installed, sent to the interface as JSON.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeStatus {
    pub installed: bool,
    pub path: String,
}

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

/// Reports whether the runtime executable is present.
pub fn status(runtime_dir: &Path) -> RuntimeStatus {
    let path = executable_path(runtime_dir);
    RuntimeStatus {
        installed: path.is_file(),
        path: path.to_string_lossy().into_owned(),
    }
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
    /// Starts the runtime from its folder, so it finds `azura.toml` and writes its logs there.
    /// Returns once the process has started; output and exit arrive later as events.
    pub fn launch(&self, app: AppHandle, runtime_dir: &Path) -> Result<(), LaunchError> {
        let executable = executable_path(runtime_dir);
        if !executable.is_file() {
            return Err(LaunchError::RuntimeMissing);
        }
        // Claim the slot atomically, so two quick clicks cannot start two games.
        if self.running.swap(true, Ordering::SeqCst) {
            return Err(LaunchError::AlreadyRunning);
        }

        let spawned = Command::new(&executable)
            .current_dir(runtime_dir)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn();
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
    fn reports_a_missing_runtime() {
        let dir = TempFolder::new("runtime");
        assert!(!status(&dir).installed);

        std::fs::write(executable_path(&dir), b"").unwrap();
        let found = status(&dir);
        assert!(found.installed);
        assert!(found.path.ends_with("/azura"));
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
