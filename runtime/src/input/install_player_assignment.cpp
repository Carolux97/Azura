// The `local_players` setting and the installation of PlayerAssignment (player_assignment.h) on the
// runtime's input system. Apart from the policy itself, which the unit tests build on its own.

#include <algorithm>
#include <memory>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "player_assignment.h"

REXCVAR_DEFINE_INT32(local_players, 1, "Azura/Input",
                     "Players on this computer (1 to 4): one, and every controller plays player 1; "
                     "more, and each controller belongs to the player it first pressed something for");

namespace azura::input {

void InstallPlayerAssignment(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {
    return;
  }
  auto assignment = std::make_unique<PlayerAssignment>(static_cast<uint32_t>(std::max(1, REXCVAR_GET(local_players))));
  REXLOG_INFO("[Azura] Controllers: {} local player(s)", assignment->players());
  input->SetDeviceAssignment(std::move(assignment));
}

}  // namespace azura::input
