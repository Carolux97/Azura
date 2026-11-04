#include "player_activity.h"

#include <atomic>
#include <chrono>
#include <cstdlib>

namespace azura::input {

namespace {

using Clock = std::chrono::steady_clock;

// XInput's own dead zones: a resting stick or trigger reads a little off zero.
constexpr int kLeftStickDeadZone = 7849;
constexpr int kRightStickDeadZone = 8689;
constexpr int kTriggerThreshold = 30;

// Whether the game's window has the focus (SetWindowFocused).
std::atomic<bool> g_focused{true};

// Player 1's last activity, as steady clock ticks.
std::atomic<int64_t> g_last_activity{Clock::now().time_since_epoch().count()};

bool Past(int16_t value, int dead_zone) {
  return std::abs(static_cast<int>(value)) > dead_zone;
}

}  // namespace

void NotePadActivity(const rex::input::X_INPUT_STATE& state) {
  if (!g_focused.load(std::memory_order_relaxed)) return;
  const auto& pad = state.gamepad;
  if (pad.buttons != 0 || pad.left_trigger > kTriggerThreshold || pad.right_trigger > kTriggerThreshold ||
      Past(pad.thumb_lx, kLeftStickDeadZone) || Past(pad.thumb_ly, kLeftStickDeadZone) ||
      Past(pad.thumb_rx, kRightStickDeadZone) || Past(pad.thumb_ry, kRightStickDeadZone)) {
    NoteActivity();
  }
}

void NoteActivity() {
  if (!g_focused.load(std::memory_order_relaxed)) return;
  g_last_activity.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

void SetWindowFocused(bool focused) {
  g_focused.store(focused, std::memory_order_relaxed);
}

double SecondsSinceActivity() {
  const Clock::duration since = Clock::now().time_since_epoch() - Clock::duration(g_last_activity.load(std::memory_order_relaxed));
  return std::chrono::duration<double>(since).count();
}

}  // namespace azura::input
