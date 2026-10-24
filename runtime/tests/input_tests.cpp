// Tests for which controller plays which player (src/input/player_assignment.h), with made-up
// devices: no controller or runtime needed.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "../src/input/player_assignment.h"

using azura::input::PlayerAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo device;
  device.id = static_cast<DeviceId>(id);
  device.ordinal = ordinal;
  return device;
}

DeviceInfo Keyboard(uint64_t id) {
  DeviceInfo device;
  device.id = static_cast<DeviceId>(id);
  device.ordinal = UINT32_MAX;
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
const DeviceId kKeyboard = static_cast<DeviceId>(9);

}  // namespace

TEST_CASE("one player: every controller and the keyboard play player 1", "[input]") {
  PlayerAssignment assignment(1);
  assignment.OnDevicesChanged({Pad(1, 0), Pad(2, 1), Keyboard(9)});
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kDualSense, kKeyboard});
  CHECK(For(assignment, 1).empty());
  // Pressing on the second controller changes nothing: it already plays player 1.
  assignment.OnActive(0, kDualSense);
  CHECK(For(assignment, 0).size() == 3);
}

TEST_CASE("two players: the first controller pressed is player 1, the next one player 2", "[input]") {
  PlayerAssignment assignment(2);
  assignment.OnDevicesChanged({Pad(1, 0), Pad(2, 1), Keyboard(9)});
  // Nobody has pressed anything: both controllers are offered to player 1, with the keyboard.
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kXbox, kDualSense, kKeyboard});
  CHECK(For(assignment, 1).empty());

  // The DualSense, connected second, is pressed first: it is player 1.
  assignment.OnActive(0, kDualSense);
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kDualSense, kKeyboard});
  // The Xbox controller is now offered to player 2, and becomes theirs when pressed.
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kXbox});
  assignment.OnActive(1, kXbox);
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kXbox});
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kDualSense, kKeyboard});
}

TEST_CASE("the keyboard claims only player 1", "[input]") {
  PlayerAssignment assignment(2);
  assignment.OnDevicesChanged({Pad(1, 0), Keyboard(9)});
  assignment.OnActive(0, kKeyboard);
  // Player 1 plays on the keyboard; the controller is offered to player 2.
  CHECK(For(assignment, 0) == std::vector<DeviceId>{kKeyboard});
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kXbox});
}

TEST_CASE("a controller that disconnects frees its player", "[input]") {
  PlayerAssignment assignment(2);
  assignment.OnDevicesChanged({Pad(1, 0), Pad(2, 1)});
  assignment.OnActive(0, kXbox);
  assignment.OnActive(1, kDualSense);
  // Player 1's controller goes away: player 1 is free, the DualSense stays player 2's.
  assignment.OnDevicesChanged({Pad(2, 1)});
  CHECK(For(assignment, 0).empty());
  CHECK(For(assignment, 1) == std::vector<DeviceId>{kDualSense});
  // It comes back (with a new id, as the SDK gives one per connection) and claims player 1 again.
  assignment.OnDevicesChanged({Pad(2, 1), Pad(3, 2)});
  CHECK(For(assignment, 0) == std::vector<DeviceId>{static_cast<DeviceId>(3)});
  assignment.OnActive(0, static_cast<DeviceId>(3));
  CHECK(For(assignment, 0) == std::vector<DeviceId>{static_cast<DeviceId>(3)});
}

TEST_CASE("the number of players is kept between 1 and 4", "[input]") {
  CHECK(PlayerAssignment(0).players() == 1);
  CHECK(PlayerAssignment(7).players() == 4);
}
