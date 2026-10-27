// Video options that switch the engine's own settings (its SystemSettings object) while the game
// runs: depth of field and dynamic shadows. Each one writes the setting's fields in guest memory
// (docs/reverse-engineering.md, "Engine settings (SystemSettings)").

#pragma once

#include <cstdint>

namespace azura::system_settings {

// Remembers the guest address of the engine's settings object. The ambient occlusion hook gives
// it: the game reads that setting from the object as it draws, in every edition.
void NoteSettingsObject(uint32_t address);

// Writes the settings the options turn off, and puts back the game's value of those turned on
// again. Called on the game thread every frame (as each player's input runs).
void Apply(uint8_t* base);

}  // namespace azura::system_settings
