#include "overlay_input.h"

#include <atomic>
#include <chrono>
#include <utility>

#include <rex/input/input.h>
#include <rex/input/input_system.h>

namespace azura::input {

namespace {

using Clock = std::chrono::steady_clock;

// How long a press the filter makes for the game lasts: a few of its frames.
constexpr auto kPressLength = std::chrono::milliseconds(120);

std::atomic<bool> g_blocking{false};
// Until when the game sees Start held (the pause menu), as steady clock ticks; 0 when not.
std::atomic<int64_t> g_start_until{0};

// Player 1 pressed RB while holding Back, which opened the overlay: the game sees neither until both
// are up. Only touched by the filter, on the guest's input thread.
bool g_chord_opened = false;
// RB was down at the last poll, to see it go down.
bool g_shoulder_down = false;

int64_t Ticks(Clock::time_point when) {
  return when.time_since_epoch().count();
}

// Changes player `user_index`'s state as the overlay needs, before the game reads it.
void Filter(uint32_t user_index, rex::input::X_INPUT_STATE& state, const std::function<void()>& open) {
  constexpr uint16_t kChordButtons = rex::input::X_INPUT_GAMEPAD_BACK | rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  const Clock::time_point now = Clock::now();
  auto& pad = state.gamepad;
  uint16_t buttons = pad.buttons;
  if (user_index == 0) {
    const bool back = buttons & rex::input::X_INPUT_GAMEPAD_BACK;
    const bool shoulder = buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (!back && !shoulder) {
      g_chord_opened = false;
    } else if (back && shoulder && !g_shoulder_down && !g_chord_opened) {
      // RB went down with Back held.
      g_chord_opened = true;
      if (open && !g_blocking.load()) open();
    }
    g_shoulder_down = shoulder;
    if (g_chord_opened) buttons = static_cast<uint16_t>(buttons & ~kChordButtons);
  }
  pad.buttons = buttons;
  const bool starting = Ticks(now) < g_start_until.load();
  if (g_blocking.load() || starting) {
    pad = {};
  }
  if (user_index == 0 && starting) pad.buttons = rex::input::X_INPUT_GAMEPAD_START;
}

}  // namespace

void InstallOverlayInput(rex::input::InputSystem* input, std::function<void()> open) {
  if (!input) return;
  if (!open) {
    input->SetStateFilter(nullptr);
    return;
  }
  input->SetStateFilter([open = std::move(open)](uint32_t user_index, rex::input::X_INPUT_STATE& state) {
    Filter(user_index, state, open);
  });
}

void SetOverlayBlocking(bool blocking) {
  g_blocking.store(blocking);
}

void PressStartForGame() {
  g_start_until.store(Ticks(Clock::now() + kPressLength));
}

}  // namespace azura::input
