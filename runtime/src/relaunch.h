// Starting the game again (to apply settings that the runtime only reads at start-up).

#pragma once

namespace azura {

// Exit code with which the game asks the launcher to start it again (EX_TEMPFAIL in sysexits.h).
// The launcher (launcher/src-tauri/src/runtime.rs) uses the same value.
constexpr int kRestartExitCode = 75;

// Whether the launcher started this process and starts it again when it exits with
// kRestartExitCode: it sets the AZURA_LAUNCHER environment variable to 1.
bool StartedByLauncher();

// Starts this executable again, with the same arguments, working folder and environment, as soon
// as the current process has exited. Call right before closing the game. macOS and Linux.
void RelaunchAfterExit();

}  // namespace azura
