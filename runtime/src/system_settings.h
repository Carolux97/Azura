// Video options that switch the engine's own settings (its SystemSettings object) while the game
// runs: depth of field, dynamic shadows, ambient occlusion, bloom and lens flares. Each one writes
// the setting's fields in guest memory (docs/reverse-engineering.md, "Engine settings
// (SystemSettings)").

#pragma once

#include <cstdint>

namespace azura::system_settings {

// Writes the settings the options turn off, and puts back the game's value of those turned on
// again. Called on the game thread every frame (as each player's input runs).
void Apply(uint8_t* base);

}  // namespace azura::system_settings
