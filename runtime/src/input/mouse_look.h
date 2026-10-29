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
//
// What the game does to the look input after that is for the stick, and the script hooks keep the
// mouse's share out of it, so a pixel turns the same angle whatever the game is doing: they turn
// mouse smoothing off, take the share out of the profile's look sensitivity, the pitch ratio and
// the slow motion division, carry the fraction of a rotator unit the view rotation drops, and turn
// the view adhesion and the bullet aim assist off for a player on the mouse (PlaysWithMouse).

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
  // The player's view angle as of the last frame (the camera's CameraCache.POV.FOV), 0 when
  // unknown, and the base camera's angle (field_of_view): while aiming, the turn per pixel is
  // scaled by base_fov / view_fov, so that the game's turn scaling by the view angle (FOVScale)
  // leaves aiming turning as the base camera does.
  float view_fov = 0.0f;
  float base_fov = 0.0f;
};

// Sets the input system the mouse motion comes from; nullptr turns mouse look off.
void InstallMouseLook(rex::input::InputSystem* input);

// Writes the mouse motion of guest user `user_index` since the last call into `target`, as the
// angle mnk_sensitivity sets per pixel, and returns true. Leaves it alone and returns false when
// that player has no keyboard and mouse or mouse look is off.
bool ApplyMouseLook(uint8_t* base, uint32_t user_index, const MouseLookTarget& target);

// Whether guest user `user_index` last played with the keyboard and mouse: the aim assist made for
// the stick is off for them.
bool PlaysWithMouse(uint32_t user_index);

}  // namespace azura::input
