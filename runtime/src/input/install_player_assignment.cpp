// The installation of PlayerAssignment (player_assignment.h) on the runtime's input system, and the
// signing in and out of player 2 it reports. Apart from the policy itself, which the unit
// tests build on its own.

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include "player_assignment.h"

namespace azura::input {

namespace {

// Owned by the input system, which lives as long as the runtime.
PlayerAssignment* g_installed = nullptr;
// The players to sign in, as last reported (bit N for player N + 1).
std::atomic<uint32_t> g_signed_in{1};

// Signs player 2 in and out to match g_signed_in. Runs outside the input system's lock: the
// sign-in notification takes the kernel's locks, which guest threads hold while they read input.
void ApplySignedIn(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel) {
    return;
  }
  const uint32_t wanted = g_signed_in.load();
  const uint32_t now = kernel->signed_in_users();
  for (uint32_t player = 1; player < rex::input::kMaxGuestUsers; ++player) {
    const uint32_t bit = 1u << player;
    if ((wanted & bit) && !(now & bit)) {
      kernel->SignInLocalUser(player, LocalPlayerName(REXCVAR_GET(user_name), player));
    } else if (!(wanted & bit) && (now & bit)) {
      kernel->SignOutLocalUser(player);
    }
  }
}

}  // namespace

void InstallPlayerAssignment(rex::Runtime* runtime, std::function<void(std::function<void()>)> post) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {
    return;
  }
  auto assignment = std::make_unique<PlayerAssignment>(
      [](const std::string& line) { REXLOG_INFO("[Azura co-op] {}", line); },
      [runtime, post](uint32_t players) {
        g_signed_in.store(players);
        if (post) {
          post([runtime] { ApplySignedIn(runtime); });
        }
      },
      // A player connected or disconnected: XN_SYS_INPUTDEVICESCHANGED, as the console sent when a
      // controller came or went, so the game pauses for a player left with nothing and resumes.
      [runtime, post](uint32_t players) {
        REXLOG_INFO("[Azura co-op] Connected players: {:02b}", players & 0x3);
        if (post) {
          post([runtime] {
            if (auto* kernel = runtime ? runtime->kernel_state() : nullptr) {
              kernel->BroadcastNotification(0x00000012, 0);
            }
          });
        }
      });
  g_installed = assignment.get();
  input->SetDeviceAssignment(std::move(assignment));
}

PlayerAssignment* InstalledPlayerAssignment() { return g_installed; }

}  // namespace azura::input
