#include "player_assignment.h"

#include <algorithm>
#include <memory>

namespace azura::input {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

PlayerAssignment::PlayerAssignment(uint32_t players)
    : players_(std::clamp<uint32_t>(players, 1, kMaxGuestUsers)) {}

void PlayerAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  devices_ = devices;
  // A player whose device went away is free again.
  for (auto& owner : owner_) {
    if (std::none_of(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return d.id == owner; })) {
      owner = DeviceId::kInvalid;
    }
  }
}

bool PlayerAssignment::IsSynthetic(DeviceId id) const {
  auto it = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return d.id == id; });
  return it != devices_.end() && it->synthetic;
}

uint32_t PlayerAssignment::OwnerOf(DeviceId id) const {
  for (uint32_t user = 0; user < players_; ++user) {
    if (owner_[user] == id && id != DeviceId::kInvalid) {
      return user;
    }
  }
  return players_;
}

uint32_t PlayerAssignment::FirstFree() const {
  for (uint32_t user = 0; user < players_; ++user) {
    if (owner_[user] == DeviceId::kInvalid) {
      return user;
    }
  }
  return players_;
}

void PlayerAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index >= players_) {
    return;
  }
  if (players_ == 1) {
    // One player: everything plays player 1.
    for (const auto& device : devices_) {
      out.push_back(device.id);
    }
    return;
  }
  const uint32_t free = FirstFree();
  for (const auto& device : devices_) {
    const uint32_t owner = OwnerOf(device.id);
    // The keyboard and mouse are player 1's; a device nobody owns is offered to the first free
    // player, so its first press is that player's and claims it (OnActive).
    const bool mine = owner == user_index || (owner == players_ && (device.synthetic ? user_index == 0
                                                                                      : user_index == free));
    if (mine) {
      out.push_back(device.id);
    }
  }
}

void PlayerAssignment::OnActive(uint32_t user_index, DeviceId id) {
  if (players_ == 1 || user_index >= players_ || id == DeviceId::kInvalid) {
    return;
  }
  if (owner_[user_index] != DeviceId::kInvalid || OwnerOf(id) != players_) {
    return;
  }
  // The keyboard and mouse claim player 1 only; a controller, the player it was offered to.
  if (IsSynthetic(id) ? user_index == 0 : user_index == FirstFree()) {
    owner_[user_index] = id;
  }
}

}  // namespace azura::input
