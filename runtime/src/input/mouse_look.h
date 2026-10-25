// Mouse look the way a PC game does it: the mouse turns the camera directly, by a fixed angle per
// pixel, instead of posing as the right stick.
//
// Gears of War 3 keeps Unreal Engine 3's PC mouse axes. Every frame, PlayerInput.PlayerInput adds
// aMouseX and aMouseY (scaled by MouseSensitivity and the field of view) to aTurn and aLookUp,
// after the right stick has been through the game's curve, acceleration and deceleration, and
// GearPC.UpdateRotation turns the view by them. On the console they are always zero. The script
// hooks call ApplyMouseLook as that function starts, and it writes the mouse motion since the last
// frame there (docs/reverse-engineering.md, Mouse look). While it does, the SDK's keyboard and
// mouse driver stops turning the mouse into right stick (rex::input::InputSystem::TakeMouseLook).

#pragma once

#include <cstdint>

namespace rex::input {
class InputSystem;
}

namespace azura::input {

// Where ApplyMouseLook writes, in guest memory: one PlayerInput object's properties.
struct MouseLookTarget {
  uint32_t mouse_x;      // PlayerInput.aMouseX (float)
  uint32_t mouse_y;      // PlayerInput.aMouseY (float)
  uint32_t sensitivity;  // PlayerInput.MouseSensitivity (float)
};

// Sets the input system the mouse motion comes from; nullptr turns mouse look off.
void InstallMouseLook(rex::input::InputSystem* input);

// Writes the mouse motion of guest user `user_index` since the last call into `target`, as the
// angle mnk_sensitivity sets per pixel. Leaves it alone when that player has no keyboard and mouse
// or mouse look is off.
void ApplyMouseLook(uint8_t* base, uint32_t user_index, const MouseLookTarget& target);

}  // namespace azura::input
