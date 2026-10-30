// A controller that presses buttons on a fixed schedule, for unattended test runs (see
// scripted_pad.cpp).

#pragma once

namespace rex::input {
class InputSystem;
}

namespace azura::input {

// Adds the scripted controller to the input system when AZURA_SCRIPTED_PAD is set; otherwise does
// nothing.
void InstallScriptedPad(rex::input::InputSystem* input);

}  // namespace azura::input
