#include "mouse_look.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

// Defined by the SDK's keyboard and mouse driver; the Options menu's Mouse sensitivity sets it.
REXCVAR_DECLARE(double, mnk_sensitivity);

REXCVAR_DEFINE_DOUBLE(mnk_aim_sensitivity, 1.0, "Input",
                      "Mouse look while aiming, as mnk_sensitivity is the rest of the time "
                      "(Options, Keyboard and mouse, Settings)")
    .range(0.01, 10.0);
REXCVAR_DEFINE_DOUBLE(mnk_zoom_sensitivity, 1.0, "Input",
                      "Mouse look while zoomed in, as mnk_sensitivity is the rest of the time")
    .range(0.01, 10.0);
REXCVAR_DEFINE_DOUBLE(mnk_run_sensitivity, 1.0, "Input",
                      "Mouse look while roadie running, as a share of the rest of the time: the "
                      "game turns the stick slower then")
    .range(0.1, 1.0);
REXCVAR_DEFINE_DOUBLE(mnk_run_turn_limit, 2.0, "Input",
                      "While roadie running, the fastest the mouse turns sideways, as a multiple of "
                      "a full A or D steer: above 1 it steers better than the keyboard, without "
                      "flicking round")
    .range(1.0, 10.0);
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "Input", "Moving the mouse forward looks down");
REXCVAR_DEFINE_DOUBLE(mnk_mouse_acceleration, 0.0, "Input",
                      "Mouse acceleration: 0 turns the same angle per pixel at any speed (as PC "
                      "shooters do by default); up to 1, fast motion turns up to three times "
                      "further")
    .range(0.0, 1.0);

namespace azura::input {

namespace {

// Mouse speed, in pixels per frame, at which acceleration reaches its full effect.
constexpr double kAccelerationFullSpeed = 80.0;

// The camera's turn for one pixel with mnk_sensitivity 1, in Unreal rotator units (65536 a full
// turn): about 0.044 degrees, a full turn every 8192 pixels, as PC shooters commonly start. The
// game then scales it by the field of view (less turn while zoomed); its own look sensitivity is
// kept off the mouse.
constexpr double kRotatorUnitsPerPixel = 8.0;

std::atomic<rex::input::InputSystem*> g_input{nullptr};
std::atomic<bool> g_logged{false};

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(bits);
}

}  // namespace

void InstallMouseLook(rex::input::InputSystem* input) {
  g_input.store(input, std::memory_order_release);
}

bool PlaysWithMouse(uint32_t user_index) {
  auto* input = g_input.load(std::memory_order_acquire);
  return input && input->ActiveKind(user_index) == rex::input::ActiveDeviceKind::kKeyboardMouse;
}

bool ApplyMouseLook(uint8_t* base, uint32_t user_index, const MouseLookTarget& target) {
  auto* input = g_input.load(std::memory_order_acquire);
  rex::input::InputDriver::MouseLook look;
  if (!input || !input->TakeMouseLook(user_index, look)) {
    return false;
  }
  const float dx = look.dx;
  const float dy = REXCVAR_GET(mnk_invert_y) ? -look.dy : look.dy;
  // PlayerInput multiplies aMouseX and aMouseY by MouseSensitivity, whose value a console build
  // never needed: it is divided out here, so the turn per pixel is ours whatever it holds.
  float engine_sensitivity = LoadFloat(base, target.sensitivity);
  if (!(engine_sensitivity > 0.0f)) {
    engine_sensitivity = 1.0f;
    StoreFloat(base, target.sensitivity, engine_sensitivity);
  }
  if (!g_logged.exchange(true)) {
    REXLOG_INFO("[Azura mouse] Mouse look turns the camera directly (player {}, engine MouseSensitivity {})",
                user_index + 1, LoadFloat(base, target.sensitivity));
  }
  // The sensitivity of what the player is doing: zoomed in, aiming, or neither.
  const double sensitivity = look.zoomed  ? REXCVAR_GET(mnk_zoom_sensitivity)
                             : look.aiming ? REXCVAR_GET(mnk_aim_sensitivity)
                                           : REXCVAR_GET(mnk_sensitivity);
  // Acceleration: the faster the mouse moves, the further each pixel turns.
  const double speed = std::hypot(double(dx), double(dy));
  const double acceleration =
      1.0 + 2.0 * REXCVAR_GET(mnk_mouse_acceleration) * std::min(speed / kAccelerationFullSpeed, 1.0);
  double per_pixel = kRotatorUnitsPerPixel * sensitivity * acceleration / engine_sensitivity;
  // The game turns less the narrower the view (FOVScale): aiming, with its narrower angle, would
  // turn slower than looking around. Scaled back here so a pixel turns as far as without aiming;
  // zoomed in, the slower turn is kept for precision.
  if (look.aiming && !look.zoomed && target.view_fov > 0.0f && target.base_fov > 0.0f) {
    per_pixel *= target.base_fov / target.view_fov;
  }
  StoreFloat(base, target.mouse_x, static_cast<float>(dx * per_pixel));
  // The window's y grows downwards; aMouseY looks up when positive.
  StoreFloat(base, target.mouse_y, static_cast<float>(-dy * per_pixel));
  return true;
}

}  // namespace azura::input
