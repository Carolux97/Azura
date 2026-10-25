#include "player_assignment.h"

#include <algorithm>
#include <utility>

namespace azura::input {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

namespace {

// The SDK's keyboard and mouse device (kMnkDevice in mnk_input_driver.cpp, "MNK").
constexpr DeviceId kKeyboardDevice = static_cast<DeviceId>(0x4D4E4B00);

// "player 2" or "nobody", for the log.
std::string PlayerName(int player) {
  return player == PlayerAssignment::kUnassigned ? "nobody" : "player " + std::to_string(player + 1);
}

}  // namespace

PlayerAssignment::PlayerAssignment(LogFn log, SignedInFn signed_in, ConnectedFn connected)
    : log_(std::move(log)), signed_in_(std::move(signed_in)), connected_(std::move(connected)) {}

bool PlayerAssignment::Connected(int player) const {
  if (!coop_) {
    return player == 0;
  }
  return HoldsAnything(player) || (player == 0 && !lost_[0].has_value());
}

std::string LocalPlayerName(const std::string& gamertag, uint32_t player) {
  constexpr size_t kMaxName = 15;
  if (player == 0) {
    return gamertag.substr(0, kMaxName);
  }
  const std::string suffix = " (" + std::to_string(player) + ")";
  return gamertag.substr(0, kMaxName - suffix.size()) + suffix;
}

void PlayerAssignment::ReportSignedIn() {
  uint32_t players = 1;
  if (coop_) {
    for (int player = 1; player < static_cast<int>(kMaxGuestUsers); ++player) {
      if (HoldsAnything(player) || lost_[player].has_value()) {
        players |= 1u << player;
      }
    }
  }
  if (players != reported_signed_in_) {
    reported_signed_in_ = players;
    if (signed_in_) {
      signed_in_(players);
    }
  }
  uint32_t connected = 0;
  for (int player = 0; player < static_cast<int>(kMaxGuestUsers); ++player) {
    if (Connected(player)) {
      connected |= 1u << player;
    }
  }
  if (connected != reported_connected_) {
    reported_connected_ = connected;
    if (connected_) {
      connected_(connected);
    }
  }
}

void PlayerAssignment::Log(const std::string& line) const {
  if (log_) {
    log_(line);
  }
}

bool PlayerAssignment::IsKeyboard(const DeviceInfo& device) const {
  return device.synthetic && device.id == kKeyboardDevice;
}

bool PlayerAssignment::IsMovable(const DeviceInfo& device) const {
  return !device.synthetic || IsKeyboard(device);
}

const DeviceInfo* PlayerAssignment::Find(DeviceId id) const {
  auto it = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return d.id == id; });
  return it != devices_.end() ? &*it : nullptr;
}

int PlayerAssignment::PlayerOf(const DeviceInfo& device) const {
  if (!coop_ || !IsMovable(device)) {
    // Solo, and the stand-in device in co-op: player 1.
    return 0;
  }
  auto it = owner_.find(device.id);
  return it != owner_.end() ? it->second : kUnassigned;
}

bool PlayerAssignment::HasController(int player) const {
  return std::any_of(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) {
    return !d.synthetic && PlayerOf(d) == player;
  });
}

bool PlayerAssignment::HasKeyboard(int player) const {
  return std::any_of(devices_.begin(), devices_.end(),
                     [&](const DeviceInfo& d) { return IsKeyboard(d) && PlayerOf(d) == player; });
}

bool PlayerAssignment::HoldsAnything(int player) const {
  return std::any_of(devices_.begin(), devices_.end(),
                     [&](const DeviceInfo& d) { return IsMovable(d) && PlayerOf(d) == player; });
}

uint32_t PlayerAssignment::Available() const {
  const auto movable = std::count_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return IsMovable(d); });
  uint32_t available = std::min<uint32_t>(static_cast<uint32_t>(movable), kMaxLocalPlayers);
  for (int player = 1; player < static_cast<int>(kMaxGuestUsers); ++player) {
    if (coop_ && (HoldsAnything(player) || lost_[player].has_value())) {
      available = std::max<uint32_t>(available, static_cast<uint32_t>(player) + 1);
    }
  }
  return std::max<uint32_t>(available, 1);
}

uint32_t PlayerAssignment::AvailablePlayers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Available();
}

void PlayerAssignment::SetOwner(const DeviceInfo& device, int player) {
  if (player == kUnassigned) {
    owner_.erase(device.id);
  } else {
    owner_[device.id] = player;
    // A controller takes the place of the one the player lost; the keyboard does not.
    if (!device.synthetic) {
      lost_[player].reset();
    }
  }
  Log(device.name + " -> " + PlayerName(player));
}

void PlayerAssignment::StartCoop() {
  coop_ = true;
  owner_.clear();
  lost_ = {};
  // Player 1 keeps the controller they used last, or the first one connected.
  const DeviceInfo* kept = Find(last_used_);
  if (!kept || kept->synthetic) {
    auto first = std::find_if(devices_.begin(), devices_.end(), [](const DeviceInfo& d) { return !d.synthetic; });
    kept = first != devices_.end() ? &*first : nullptr;
  }
  Log("Local co-op starts");
  for (const auto& device : devices_) {
    if (IsKeyboard(device) || (kept && device.id == kept->id)) {
      SetOwner(device, 0);
    } else if (IsMovable(device)) {
      Log(device.name + " -> nobody");
    }
  }
}

void PlayerAssignment::EndCoopIfAlone() {
  if (!coop_ || editing_) {
    return;
  }
  for (int player = 1; player < static_cast<int>(kMaxGuestUsers); ++player) {
    if (HoldsAnything(player) || lost_[player].has_value()) {
      return;
    }
  }
  coop_ = false;
  owner_.clear();
  Log("Back to one player: every device plays player 1");
}

void PlayerAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::vector<DeviceInfo> before = std::move(devices_);
  devices_ = devices;
  if (!Find(last_used_)) {
    last_used_ = DeviceId::kInvalid;
  }
  if (!coop_) {
    return;
  }
  // A player whose only device went away waits for it (the game asks to reconnect).
  for (const auto& gone : before) {
    if (Find(gone.id)) {
      continue;
    }
    auto it = owner_.find(gone.id);
    if (it == owner_.end()) {
      continue;
    }
    const int player = it->second;
    owner_.erase(it);
    Log(gone.name + " of " + PlayerName(player) + " disconnected");
    // Remembered even when the player still holds the keyboard, so the same controller comes back
    // to them; they only wait for it (the reconnect notice) when they hold nothing else.
    if (!gone.synthetic) {
      lost_[player] = gone;
    }
  }
  // A controller like the one a player lost goes back to them; any other new one is unassigned.
  for (const auto& device : devices_) {
    const bool is_new = std::none_of(before.begin(), before.end(), [&](const DeviceInfo& d) { return d.id == device.id; });
    if (!is_new || !IsMovable(device)) {
      continue;
    }
    int player = kUnassigned;
    if (IsKeyboard(device)) {
      player = HasKeyboard(0) ? kUnassigned : 0;
    } else {
      for (int p = 0; p < static_cast<int>(kMaxGuestUsers); ++p) {
        if (lost_[p] && lost_[p]->guid == device.guid && !HasController(p)) {
          player = p;
          break;
        }
      }
    }
    SetOwner(device, player);
  }
  ReportSignedIn();
}

void PlayerAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out.clear();
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  const int player = static_cast<int>(user_index);
  if (blocked_) {
    // The menu is open: players stay connected through the stand-in device, which has no input.
    if (Connected(player)) {
      for (const auto& device : devices_) {
        if (device.synthetic && !IsKeyboard(device)) {
          out.push_back(device.id);
        }
      }
    }
    return;
  }
  for (const auto& device : devices_) {
    // The stand-in keeps player 1 connected, except in co-op once they lost all they held: then the
    // game sees them disconnected, pauses and asks for their controller.
    const bool stand_in = device.synthetic && !IsKeyboard(device);
    if (PlayerOf(device) == player && (!stand_in || Connected(player))) {
      out.push_back(device.id);
    }
  }
}

void PlayerAssignment::OnActive(uint32_t user_index, DeviceId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const DeviceInfo* device = Find(id);
  if (user_index == 0 && device && !device->synthetic) {
    last_used_ = id;
  }
}

std::vector<PlayerAssignment::Slot> PlayerAssignment::Slots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Slot> slots;
  for (const auto& device : devices_) {
    if (IsMovable(device)) {
      slots.push_back({device, IsKeyboard(device), PlayerOf(device)});
    }
  }
  return slots;
}

bool PlayerAssignment::Move(DeviceId id, int direction) {
  std::lock_guard<std::mutex> lock(mutex_);
  const DeviceInfo* device = Find(id);
  if (!device || !IsMovable(*device) || direction == 0) {
    return false;
  }
  if (!coop_) {
    StartCoop();
  }
  const int from = PlayerOf(*device);
  const int step = direction < 0 ? -1 : 1;
  const int available = static_cast<int>(Available());
  for (int to = from + step; to >= kUnassigned && to < available; to += step) {
    // At most the keyboard and one controller per player.
    if (to != kUnassigned && (IsKeyboard(*device) ? HasKeyboard(to) : HasController(to))) {
      continue;
    }
    // Player 1 always keeps a device; players 2 to 4 left with none leave the game.
    if (from == 0 && to != 0) {
      const bool keeps_one = std::any_of(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) {
        return d.id != device->id && IsMovable(d) && PlayerOf(d) == 0;
      });
      if (!keeps_one) {
        break;
      }
    }
    SetOwner(*device, to);
    EndCoopIfAlone();
    ReportSignedIn();
    return true;
  }
  // Nowhere to go: a co-op that this move started ends again.
  EndCoopIfAlone();
  return false;
}

bool PlayerAssignment::Give(DeviceId id, int player) {
  std::lock_guard<std::mutex> lock(mutex_);
  const DeviceInfo* device = Find(id);
  if (!coop_ || !device || device->synthetic || PlayerOf(*device) != kUnassigned || player < 0 ||
      player >= static_cast<int>(kMaxLocalPlayers) || HasController(player)) {
    return false;
  }
  SetOwner(*device, player);
  ReportSignedIn();
  return true;
}

uint32_t PlayerAssignment::WaitingPlayers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t waiting = 0;
  if (!coop_) {
    return waiting;
  }
  for (int player = 0; player < static_cast<int>(kMaxGuestUsers); ++player) {
    if (lost_[player].has_value() && !HoldsAnything(player)) {
      waiting |= 1u << player;
    }
  }
  return waiting;
}

std::optional<DeviceInfo> PlayerAssignment::LostDevice(int player) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (player < 0 || player >= static_cast<int>(kMaxGuestUsers) || HoldsAnything(player)) {
    return std::nullopt;
  }
  return lost_[player];
}

bool PlayerAssignment::Editing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return editing_;
}

void PlayerAssignment::SetBlocked(bool blocked) {
  std::lock_guard<std::mutex> lock(mutex_);
  blocked_ = blocked;
}

void PlayerAssignment::BeginEditing() {
  std::lock_guard<std::mutex> lock(mutex_);
  editing_ = true;
  if (!coop_) {
    StartCoop();
  }
}

void PlayerAssignment::EndEditing() {
  std::lock_guard<std::mutex> lock(mutex_);
  editing_ = false;
  EndCoopIfAlone();
  ReportSignedIn();
}

uint32_t PlayerAssignment::Players() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!coop_) {
    return 1;
  }
  uint32_t players = 1;
  for (int player = 1; player < static_cast<int>(kMaxGuestUsers); ++player) {
    if (HoldsAnything(player)) {
      players = static_cast<uint32_t>(player) + 1;
    }
  }
  return players;
}

}  // namespace azura::input
