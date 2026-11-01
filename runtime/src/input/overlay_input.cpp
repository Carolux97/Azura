#include "overlay_input.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include <rex/input/input.h>
#include <rex/input/input_system.h>

namespace azura::input {

namespace {

using Clock = std::chrono::steady_clock;

// How long a press the filter makes for the game lasts: a few of its frames.
constexpr auto kPressLength = std::chrono::milliseconds(120);
// How far a trigger goes before it counts as pressed: XInput's XINPUT_GAMEPAD_TRIGGER_THRESHOLD.
constexpr uint8_t kTriggerPress = 30;

std::atomic<bool> g_blocking{false};
std::atomic<bool> g_accepting{false};
// Until when the game sees Start held (the pause menu), as steady clock ticks; 0 when not.
std::atomic<int64_t> g_start_until{0};

// Player 1 pressed RB while holding Back, which opened the overlay: the game sees neither until both
// are up. Only touched by the filter, on the guest's input thread.
bool g_chord_opened = false;
// RB was down at the last poll, to see it go down.
bool g_shoulder_down = false;
// Player 1 pressed RT while holding Back, which accepted a notice's invitation: the game sees
// neither until both are up. RT was down at the last poll. Only touched by the filter.
bool g_accept_chord = false;
bool g_trigger_down = false;

int64_t Ticks(Clock::time_point when) {
  return when.time_since_epoch().count();
}

// Changes player `user_index`'s state as the overlay needs, before the game reads it.
void Filter(uint32_t user_index, rex::input::X_INPUT_STATE& state, const std::function<void()>& open,
            const std::function<void()>& accept) {
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

    const bool trigger = pad.right_trigger > kTriggerPress;
    if (!back && !trigger) {
      g_accept_chord = false;
    } else if (back && trigger && !g_trigger_down && !g_accept_chord && g_accepting.load() && !g_blocking.load()) {
      // RT went down with Back held, while an invitation's notice is on screen.
      g_accept_chord = true;
      if (accept) accept();
    }
    g_trigger_down = trigger;
    if (g_accept_chord) {
      buttons = static_cast<uint16_t>(buttons & ~rex::input::X_INPUT_GAMEPAD_BACK);
      pad.right_trigger = 0;
    }
  }
  pad.buttons = buttons;
  const bool starting = Ticks(now) < g_start_until.load();
  if (g_blocking.load() || starting) {
    pad = {};
  }
  if (user_index == 0 && starting) pad.buttons = rex::input::X_INPUT_GAMEPAD_START;
}

}  // namespace

void InstallOverlayInput(rex::input::InputSystem* input, std::function<void()> open, std::function<void()> accept) {
  if (!input) return;
  if (!open) {
    input->SetStateFilter(nullptr);
    return;
  }
  input->SetStateFilter([open = std::move(open), accept = std::move(accept)](uint32_t user_index,
                                                                             rex::input::X_INPUT_STATE& state) {
    Filter(user_index, state, open, accept);
  });
}

void SetOverlayBlocking(bool blocking) {
  g_blocking.store(blocking);
}

void SetNoticeAccepting(bool accepting) {
  g_accepting.store(accepting);
}

void PressStartForGame() {
  g_start_until.store(Ticks(Clock::now() + kPressLength));
}

}  // namespace azura::input
