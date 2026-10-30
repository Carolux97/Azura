// Calls to a parent function the disc build does not have, made by RAAM's Shadow's script.
//
// The DLC's Raam Reaver (Vehicle_ReaverGeneralRaam) overrides PlayHitReactionAnimation and
// ShouldTurnCursorRedFor, which the Title Update added to Vehicle_Reaver_Base, and calls them with
// Super. Azura's script patch cannot add them to that class: the disc build's patch loader breaks
// on a class that receives both a new property (Vehicle_Reaver_Base.TaccomMesh) and new functions
// (see docs/raams-shadow-dlc.md). So the DLC's import of them stays unresolved, and its Super call
// reaches UObject::CallFunction with no function.
//
// HandleMissingSuper takes such a call: it runs the nearest function of the same name in the
// caller's parent classes (GearVehicle.ShouldTurnCursorRedFor), or, when there is none, skips the
// call's arguments and leaves the result empty, as the Title Update's empty version did.

#pragma once

#include <cstdint>

struct PPCContext;

namespace azura::dlc {

// For UObject::CallFunction (r3 = object, r4 = FFrame of the calling code, r5 = result, r6 =
// UFunction) with r6 = 0. Returns true when the call is done and CallFunction must not run; false
// when CallFunction must run (r6 now names the parent's function), or when the call is not one it
// knows how to take.
bool HandleMissingSuper(PPCContext& ctx, uint8_t* base);

}  // namespace azura::dlc
