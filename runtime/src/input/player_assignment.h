// Which device plays which guest user (player), for one player or local co-op.
//
// The SDK's default gives controller N, by connection order, to user N, and only user 0 has a
// signed-in profile. PlayerAssignment instead starts with everything on player 1 and lets the Local
// co-op menu hand devices to the other players:
//   - solo (the start, and whenever player 2 hold nothing): every controller, and the
//     keyboard and mouse, drive user 0. The SDK makes the one pressed last the active one, so the
//     button prompts (ActiveKind(0)) and the vibration follow the controller in the player's hands;
//   - co-op (a device moved to player 2): each player holds at most the keyboard and mouse
//     plus one controller, and devices nobody holds play nobody. When co-op starts, player 1 keeps
//     the keyboard and mouse and the controller they used last; the other controllers are left
//     unassigned. A controller connected during co-op stays unassigned, except one like the
//     controller a player lost, which goes back to that player.
// A guest user exists for the game while it holds a device: the game adds player 2 when user 1
// gets one, and treats a player who loses theirs as disconnected. Nothing is remembered between
// sessions. The SDK's stand-in device ("None", which keeps user 0 connected with no input) always
// stays with player 1 and is not one of the devices the menu moves.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/input/device_assignment.h>

namespace rex {
class Runtime;
}

namespace azura::input {

class PlayerAssignment final : public rex::input::DeviceAssignment {
 public:
  // Players the game lets play on one console: two ("on an Xbox 360 console only two people can
  // play", as the game says when a third joins).
  static constexpr uint32_t kMaxLocalPlayers = 2;
  // Where a device is: a player (0 to 3), or nobody.
  static constexpr int kUnassigned = -1;

  // Receives each log line (who gets which device), without the "[Azura co-op]" prefix.
  using LogFn = std::function<void(const std::string&)>;

  // Receives the players that should be signed in (bit N for player N + 1: player 1 always, and
  // player 2 while they hold a device or wait for theirs) each time that changes. Called
  // with the assignment's lock held, so it must not call back into it.
  using SignedInFn = std::function<void(uint32_t players)>;

  // Receives the players the game sees connected (bit N for player N + 1: those holding a device)
  // each time that changes, so the game hears of it as the console told it of controllers coming
  // and going. Called with the lock held, like SignedInFn.
  using ConnectedFn = std::function<void(uint32_t players)>;

  explicit PlayerAssignment(LogFn log = nullptr, SignedInFn signed_in = nullptr, ConnectedFn connected = nullptr);

  // A device the Local co-op menu shows, with the player it is on.
  struct Slot {
    rex::input::DeviceInfo device;
    bool keyboard = false;
    int player = kUnassigned;
  };

  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override;
  void OnActive(uint32_t user_index, rex::input::DeviceId id) override;

  // The devices the menu moves (controllers, and the keyboard and mouse), in connection order,
  // each with its player. Callable from any thread.
  std::vector<Slot> Slots() const;
  // Moves `id` one place to the left (direction < 0) or to the right (> 0) along nobody, player 1,
  // player 2... up to AvailablePlayers(). Players that already hold a controller (or the keyboard, when moving the keyboard)
  // are skipped. Player 1 keeps at least one device; player 2 left with none leave the game.
  // Returns whether the device moved. Callable from any thread.
  bool Move(rex::input::DeviceId id, int direction);
  // Gives `id`, an unassigned controller, to `player` when that player has no controller (the
  // controller that answers the reconnect notice). Returns whether it was given.
  bool Give(rex::input::DeviceId id, int player);
  // The players who lost their only device and wait for one (bit N for player N + 1): a player who
  // still holds the keyboard and mouse, or another controller, does not wait.
  uint32_t WaitingPlayers() const;
  // The controller a waiting player lost, for the reconnect notice.
  std::optional<rex::input::DeviceInfo> LostDevice(int player) const;
  // Whether the Local co-op menu is open (between BeginEditing and EndEditing).
  bool Editing() const;
  // While blocked (the Local co-op menu is open), no device reaches the game.
  void SetBlocked(bool blocked);
  // The Local co-op menu opens: co-op starts (player 1 keeps the keyboard and the controller they
  // used last, the other controllers are unassigned) and stays on while the menu is open, even
  // with player 2 empty.
  void BeginEditing();
  // The menu closes: with player 2 holding nothing, every device plays player 1 again.
  void EndEditing();

  // The players the menu offers devices to: as many as the devices it moves (the keyboard and mouse
  // count as one), up to kMaxLocalPlayers, and never fewer than the highest player holding or
  // waiting for one.
  // Callable from any thread.
  uint32_t AvailablePlayers() const;

  // The highest player holding a device: 1 in solo, 2 in co-op.
  uint32_t Players() const;

 private:
  bool IsKeyboard(const rex::input::DeviceInfo& device) const;
  bool IsMovable(const rex::input::DeviceInfo& device) const;
  const rex::input::DeviceInfo* Find(rex::input::DeviceId id) const;
  // The player `device` is on (with the lock held).
  int PlayerOf(const rex::input::DeviceInfo& device) const;
  bool HasController(int player) const;
  bool HasKeyboard(int player) const;
  bool HoldsAnything(int player) const;
  // AvailablePlayers with the lock held.
  uint32_t Available() const;
  // Turns solo into co-op: player 1 keeps the keyboard and the controller they used last.
  void StartCoop();
  // Back to solo when player 2 hold nothing.
  void EndCoopIfAlone();
  void SetOwner(const rex::input::DeviceInfo& device, int player);
  void Log(const std::string& line) const;
  // Calls signed_in_ and connected_ when the players to sign in, or the connected ones, changed
  // (with the lock held).
  void ReportSignedIn();
  // Whether the game sees `player` connected: in solo, player 1; in co-op, a player holding a
  // device, and player 1 through the stand-in device unless they lost all they held (with the lock
  // held).
  bool Connected(int player) const;

  LogFn log_;
  SignedInFn signed_in_;
  uint32_t reported_signed_in_ = 1;
  ConnectedFn connected_;
  uint32_t reported_connected_ = 1;

  mutable std::mutex mutex_;
  std::vector<rex::input::DeviceInfo> devices_;
  bool coop_ = false;
  bool blocked_ = false;
  bool editing_ = false;
  // In co-op, the player each movable device is on (missing means unassigned).
  std::map<rex::input::DeviceId, int> owner_;
  // The controller player 1 used last, which they keep when co-op starts.
  rex::input::DeviceId last_used_ = rex::input::DeviceId::kInvalid;
  // In co-op, the controller each player lost: one with the same GUID that connects again goes back
  // to them. They wait for it (WaitingPlayers) only when they hold nothing else.
  std::array<std::optional<rex::input::DeviceInfo>, rex::input::kMaxGuestUsers> lost_ = {};
};

// The name of local player `player` (0 to 3) for a gamertag: the gamertag itself for player 1, and
// "Carolux97 (1)", "Carolux97 (2)", "Carolux97 (3)" for the others. Names hold 15 characters at
// most (the game reads them through a 16-byte buffer), so the gamertag is cut to make room.
std::string LocalPlayerName(const std::string& gamertag, uint32_t player);

// Installs PlayerAssignment on the runtime's input system, signing player 2 in and out as
// they get and lose devices.
// `post` runs a function later on another thread (the UI thread), where the sign-ins happen.
void InstallPlayerAssignment(rex::Runtime* runtime, std::function<void(std::function<void()>)> post);
// The installed PlayerAssignment, or nullptr before InstallPlayerAssignment.
PlayerAssignment* InstalledPlayerAssignment();

}  // namespace azura::input
