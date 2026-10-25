#include "mouse_look.h"

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

// Defined by the SDK's keyboard and mouse driver; the Options menu's Mouse sensitivity sets it.
REXCVAR_DECLARE(double, mnk_sensitivity);

namespace azura::input {

namespace {

// The camera's turn for one pixel with mnk_sensitivity 1, in Unreal rotator units (65536 a full
// turn): about 0.044 degrees, a full turn every 8192 pixels, as PC shooters commonly start. The
// game then scales it by the field of view (less turn while zoomed) and by its own look
// sensitivity for normal, aiming and zoomed views.
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

void ApplyMouseLook(uint8_t* base, uint32_t user_index, const MouseLookTarget& target) {
  auto* input = g_input.load(std::memory_order_acquire);
  float dx = 0.0f;
  float dy = 0.0f;
  if (!input || !input->TakeMouseLook(user_index, dx, dy)) {
    return;
  }
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
  const double per_pixel = kRotatorUnitsPerPixel * REXCVAR_GET(mnk_sensitivity) / engine_sensitivity;
  StoreFloat(base, target.mouse_x, static_cast<float>(dx * per_pixel));
  // The window's y grows downwards; aMouseY looks up when positive.
  StoreFloat(base, target.mouse_y, static_cast<float>(-dy * per_pixel));
}

}  // namespace azura::input
