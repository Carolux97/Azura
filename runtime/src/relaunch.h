// Starting the game again (to apply settings that the runtime only reads at start-up).

#pragma once

namespace azura {

// Starts this executable again, with the same arguments, working folder and environment, as soon
// as the current process has exited. Call right before closing the game. macOS and Linux.
void RelaunchAfterExit();

}  // namespace azura
