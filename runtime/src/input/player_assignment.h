// Which controller plays which guest user (player).
//
// The SDK's default gives controller N, by connection order, to user N, and only user 0 has a
// signed-in profile: with two controllers on, the second one is a player 2 without a profile, and
// the game refuses to save when Start is pressed on it. PlayerAssignment instead follows what the
// players hold:
//   - one local player (the default): every controller, and the keyboard and mouse, drive user 0.
//     The SDK makes the one pressed last the active one, so the button prompts (ActiveKind(0)) and
//     the vibration follow the controller in the player's hands, whatever the connection order;
//   - several local players (co-op): a controller nobody owns yet is offered to the first player
//     without one, and becomes theirs when it presses something; the keyboard and mouse belong to
//     player 1. A controller that disconnects frees its player for the next one pressed.
// `local_players` (azura.toml) sets how many. Signing in profiles for players 2 to 4 belongs to the
// co-op work: this only decides whose input reaches whom.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <rex/input/device_assignment.h>

namespace rex {
class Runtime;
}

namespace azura::input {

class PlayerAssignment final : public rex::input::DeviceAssignment {
 public:
  // `players` is clamped to 1..kMaxGuestUsers.
  explicit PlayerAssignment(uint32_t players);

  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override;
  void OnActive(uint32_t user_index, rex::input::DeviceId id) override;

  // The players this assignment serves, after clamping.
  uint32_t players() const { return players_; }

 private:
  // The first player with no device of their own, or players_ when every player has one.
  uint32_t FirstFree() const;
  // The player who owns `id`, or players_ when nobody does.
  uint32_t OwnerOf(rex::input::DeviceId id) const;
  bool IsSynthetic(rex::input::DeviceId id) const;

  uint32_t players_;
  std::vector<rex::input::DeviceInfo> devices_;
  // With several players, the device each one claimed (kInvalid while they have none).
  std::array<rex::input::DeviceId, rex::input::kMaxGuestUsers> owner_ = {};
};

// Installs PlayerAssignment on the runtime's input system, for the `local_players` setting.
void InstallPlayerAssignment(rex::Runtime* runtime);

}  // namespace azura::input
