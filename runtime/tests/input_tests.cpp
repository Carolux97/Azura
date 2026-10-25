// Tests for which device plays which player (src/input/player_assignment.h), with made-up devices:
// no controller or runtime needed.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "../src/input/player_assignment.h"

using azura::input::PlayerAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal, const std::string& guid) {
  DeviceInfo device;
  device.id = static_cast<DeviceId>(id);
  device.ordinal = ordinal;
  device.name = "pad " + std::to_string(id);
  device.guid = guid;
  return device;
}

// The SDK's keyboard and mouse device (its id is the one mnk_input_driver.cpp gives it).
DeviceInfo Keyboard() {
  DeviceInfo device;
  device.id = static_cast<DeviceId>(0x4D4E4B00);
  device.ordinal = UINT32_MAX;
  device.name = "Keyboard and Mouse";
  device.synthetic = true;
  return device;
}

// The SDK's stand-in device, which keeps user 0 connected with no input.
DeviceInfo StandIn() {
  DeviceInfo device;
  device.id = static_cast<DeviceId>(0x4E4F5000);
  device.ordinal = UINT32_MAX;
  device.name = "None";
  device.synthetic = true;
  return device;
}

std::vector<DeviceId> For(const PlayerAssignment& assignment, uint32_t user) {
  std::vector<DeviceId> ids;
  assignment.DevicesForUser(user, ids);
  return ids;
}

const DeviceId kXbox = static_cast<DeviceId>(1);
const DeviceId kDualSense = static_cast<DeviceId>(2);
const DeviceId kSwitch = static_cast<DeviceId>(3);
const DeviceId kKeyboard = static_cast<DeviceId>(0x4D4E4B00);
const DeviceId kStandIn = static_cast<DeviceId>(0x4E4F5000);

// An Xbox controller, a DualSense, the keyboard and the stand-in, as the SDK lists them.
std::vector<DeviceInfo> TwoPads() {
  return {Pad(1, 0, "xbox"), Pad(2, 1, "dualsense"), Keyboard(), StandIn()};
}

}  // namespace

TEST_CASE("solo: every device plays player 1", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kDualSense, kKeyboard, kStandIn});
  CHECK(For(assignment, 1).empty());
  CHECK(assignment.Players() == 1);
  // Moving the mouse or pressing on a controller changes nothing.
  assignment.OnActive(0, kKeyboard);
  assignment.OnActive(0, kDualSense);
  CHECK(For(assignment, 0).size() == 4);
  CHECK(For(assignment, 1).empty());
}

TEST_CASE("the first move starts co-op: player 1 keeps the keyboard and the controller they used", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  // The mouse moving afterwards does not count as the controller used.
  assignment.OnActive(0, kKeyboard);
  // The DualSense moves right: player 1 already holds a controller, so it goes to player 2.
  REQUIRE(assignment.Move(kDualSense, +1));
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kKeyboard, kStandIn});
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kDualSense});
  CHECK(assignment.Players() == 2);
}

TEST_CASE("a player holds at most the keyboard and one controller", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Pad(2, 1, "dualsense"), Pad(3, 2, "switch"), Keyboard()});
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  // The Switch controller, unassigned, finds both players holding a controller: it stays.
  CHECK_FALSE(assignment.Move(kSwitch, +1));
  CHECK(assignment.Slots()[2].player == PlayerAssignment::kUnassigned);
  // The keyboard can join player 2, who has no keyboard yet.
  REQUIRE(assignment.Move(kKeyboard, +1));
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kDualSense, kKeyboard});
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox});
}

TEST_CASE("player 2 can be left with no device, player 1 always keeps one", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  // Player 2's only controller moves back to nobody: player 2 leaves.
  REQUIRE(assignment.Move(kDualSense, -1));
  CHECK(For(assignment, 1).empty());
  // Player 1 gives the keyboard to player 2, and cannot then give away their last device.
  REQUIRE(assignment.Move(kKeyboard, +1));
  CHECK_FALSE(assignment.Move(kXbox, -1));
  CHECK_FALSE(assignment.Move(kXbox, +1));
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kStandIn});
}

TEST_CASE("when players 2 to 4 hold nothing, everything plays player 1 again", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  // The DualSense moves back left: player 1 holds a controller, so it is left with nobody, and
  // with players 2 to 4 empty the game is solo again.
  REQUIRE(assignment.Move(kDualSense, -1));
  CHECK(assignment.Players() == 1);
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kDualSense, kKeyboard, kStandIn});
}

TEST_CASE("a lost controller leaves its player waiting, and coming back returns it", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  // The DualSense switches off: player 2 has nothing and waits; co-op goes on.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Keyboard(), StandIn()});
  CHECK(For(assignment, 1).empty());
  CHECK(assignment.WaitingPlayers() == 0b10);
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kKeyboard, kStandIn});
  // It comes back with a new id (the SDK gives one per connection) and the same GUID.
  const DeviceId again = static_cast<DeviceId>(7);
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Pad(7, 2, "dualsense"), Keyboard(), StandIn()});
  CHECK(For(assignment, 1) == std::vector<DeviceId>{again});
  CHECK(assignment.WaitingPlayers() == 0);
}

TEST_CASE("another controller can take a waiting player's place", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Keyboard(), StandIn()});
  // A Switch controller connects during co-op: it is nobody's until given.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Pad(3, 2, "switch"), Keyboard(), StandIn()});
  CHECK(For(assignment, 1).empty());
  REQUIRE(assignment.Give(kSwitch, 1));
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kSwitch});
  CHECK(assignment.WaitingPlayers() == 0);
  // A player who already holds a controller is not given another.
  CHECK_FALSE(assignment.Give(kSwitch, 0));
}

TEST_CASE("while the menu is open, players stay connected with no input", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  assignment.SetBlocked(true);
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kStandIn});
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kStandIn});
  CHECK(For(assignment, 2).empty());
  assignment.SetBlocked(false);
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kDualSense});
}

TEST_CASE("the menu lists the controllers and the keyboard with their players", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  auto slots = assignment.Slots();
  // The stand-in is not listed.
  REQUIRE(slots.size() == 3);
  CHECK(slots[2].keyboard);
  for (const auto& slot : slots) {
    CHECK(slot.player == 0);
  }
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  slots = assignment.Slots();
  CHECK(slots[1].player == 1);
}

TEST_CASE("extra players are named after the gamertag, in 15 characters", "[input]") {
  using azura::input::LocalPlayerName;
  CHECK(LocalPlayerName("Carolux97", 0) == "Carolux97");
  CHECK(LocalPlayerName("Carolux97", 1) == "Carolux97 (1)");
  CHECK(LocalPlayerName("Carolux97", 3) == "Carolux97 (3)");
  // A gamertag too long for the suffix is cut to 11 characters before it.
  CHECK(LocalPlayerName("ABCDEFGHIJKLMNO", 1) == "ABCDEFGHIJK (1)");
  CHECK(LocalPlayerName("ABCDEFGHIJKLMNO", 0) == "ABCDEFGHIJKLMNO");
}

TEST_CASE("players 2 to 4 are reported for sign-in while they hold a device or wait for one", "[input]") {
  std::vector<uint32_t> reports;
  PlayerAssignment assignment(nullptr, [&](uint32_t players) { reports.push_back(players); });
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  // The DualSense switches off: player 2 waits, still signed in.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Keyboard(), StandIn()});
  // Back with another controller, then moved away: player 2 signs out.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Pad(3, 2, "switch"), Keyboard(), StandIn()});
  REQUIRE(assignment.Give(kSwitch, 1));
  REQUIRE(assignment.Move(kSwitch, -1));
  CHECK(reports == std::vector<uint32_t>{0b11, 0b1});
}

TEST_CASE("the menu offers two players at most, as the game allows", "[input]") {
  PlayerAssignment assignment;
  // Keyboard and mouse + 1 controller: 2 players.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Keyboard(), StandIn()});
  CHECK(assignment.AvailablePlayers() == 2);
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kKeyboard, +1));
  // There is no player 3: the keyboard cannot go further.
  CHECK_FALSE(assignment.Move(kKeyboard, +1));
  // More devices still offer 2.
  PlayerAssignment four;
  four.OnDevicesChanged({Pad(1, 0, "a"), Pad(2, 1, "b"), Pad(3, 2, "c"), Pad(4, 3, "d"), Keyboard()});
  CHECK(four.AvailablePlayers() == 2);
}

TEST_CASE("the menu opens with player 1 on the keyboard and one controller, the rest unassigned", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kDualSense);
  assignment.BeginEditing();
  auto slots = assignment.Slots();
  CHECK(slots[0].player == PlayerAssignment::kUnassigned);  // Xbox
  CHECK(slots[1].player == 0);                              // DualSense, used last
  CHECK(slots[2].player == 0);                              // keyboard
  // Moving the Xbox controller to player 2 and back leaves co-op on while the menu is open.
  REQUIRE(assignment.Move(kXbox, +1));
  CHECK(assignment.Slots()[0].player == 1);
  REQUIRE(assignment.Move(kXbox, -1));
  CHECK(assignment.Slots()[0].player == PlayerAssignment::kUnassigned);
  // Closing it with players 2 to 4 empty: every device plays player 1 again.
  assignment.EndEditing();
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kDualSense, kKeyboard, kStandIn});
}

TEST_CASE("player 1 waits too when they lose their only device", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  REQUIRE(assignment.Move(kKeyboard, +1));
  // Player 1's Xbox controller switches off: they hold nothing and wait; player 2 keeps the
  // keyboard and does not.
  assignment.OnDevicesChanged({Pad(2, 1, "dualsense"), Keyboard(), StandIn()});
  CHECK(assignment.WaitingPlayers() == 0b01);
  REQUIRE(assignment.LostDevice(0).has_value());
  CHECK(assignment.LostDevice(0)->guid == "xbox");
  CHECK_FALSE(assignment.LostDevice(1).has_value());
  // It comes back and plays player 1 again.
  assignment.OnDevicesChanged({Pad(2, 1, "dualsense"), Pad(8, 2, "xbox"), Keyboard(), StandIn()});
  CHECK(assignment.WaitingPlayers() == 0);
  CHECK(For(assignment, 0) == std::vector<DeviceId>{static_cast<DeviceId>(8), kStandIn});
}

TEST_CASE("a player who keeps the keyboard does not wait for a lost controller", "[input]") {
  PlayerAssignment assignment;
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  REQUIRE(assignment.Move(kKeyboard, +1));
  // Player 2 loses the DualSense but still has the keyboard and mouse: no wait.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Keyboard(), StandIn()});
  CHECK(assignment.WaitingPlayers() == 0);
  CHECK_FALSE(assignment.LostDevice(1).has_value());
  // The same controller coming back still goes to player 2.
  assignment.OnDevicesChanged({Pad(1, 0, "xbox"), Pad(9, 2, "dualsense"), Keyboard(), StandIn()});
  CHECK(For(assignment, 1) == std::vector<DeviceId>{static_cast<DeviceId>(9), kKeyboard});
}

TEST_CASE("the game hears when a player is left with nothing, and when they are back", "[input]") {
  std::vector<uint32_t> connected;
  PlayerAssignment assignment(nullptr, nullptr, [&](uint32_t players) { connected.push_back(players); });
  assignment.OnDevicesChanged(TwoPads());
  assignment.OnActive(0, kXbox);
  REQUIRE(assignment.Move(kDualSense, +1));
  REQUIRE(assignment.Move(kKeyboard, +1));
  // Player 1's only controller goes: they are disconnected, even without the stand-in.
  assignment.OnDevicesChanged({Pad(2, 1, "dualsense"), Keyboard(), StandIn()});
  CHECK(For(assignment, 0).empty());
  // Back: connected again.
  assignment.OnDevicesChanged({Pad(2, 1, "dualsense"), Pad(8, 2, "xbox"), Keyboard(), StandIn()});
  CHECK(connected == std::vector<uint32_t>{0b11, 0b10, 0b11});
}
