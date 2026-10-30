// HLE graphics path (docs/hle-graphics/02-design.md). With gfx = hle, render targets are host
// textures driven by the game's D3D calls instead of emulated EDRAM. The game's D3D library keeps
// running; hooks on its render target, clear, resolve and frame calls (hooks.cpp) write markers
// (rex/graphics/hle/markers.h) into its command buffer, and the frame planner (planner.cpp)
// receives them on the command processor thread, in order with the packets around them.

#pragma once

namespace azura::hle_gfx {

// Whether the HLE path is on (gfx = hle). Read once, at the first call; the option needs a
// restart, so a frame never mixes the two paths.
bool Enabled();

// Registers the frame planner as the command processor's marker handler when the HLE path is on.
// Called once at start-up, before the game runs.
void Install();

}  // namespace azura::hle_gfx
