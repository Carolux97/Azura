// Layout fixes for the script properties that Azura's DLC script patch adds to existing classes.
//
// RAAM's Shadow's levels and script import a few properties that the game's Title Update added to
// classes the disc build already has (GearPawn_LocustGeneralRaamBase.bShieldDispersed,
// Vehicle_Reaver_Base.TaccomMesh, GearGRI.SHODTargets). Azura's script patch (see
// docs/raams-shadow-dlc.md) creates those property objects, so the DLC's imports find them, but the
// disc build's patch loader never links a property into an existing class: the property keeps
// offset 0, which would make every access read or write the object's first word.
//
// As the linker finishes preloading each of them, OnPreloaded gives it a place in the objects of its
// class, before any object of the class exists, without moving anything the disc build knows, and
// puts it back there whenever the class lays itself out again (as it does when a new function of it
// is preloaded):
//   - bShieldDispersed takes a free bit of an existing bit field word of the class or a parent;
//   - TaccomMesh goes at the end of Vehicle_Reaver_Base, inside the alignment padding of its size,
//     which grows by 4 bytes (subclasses linked afterwards, such as the DLC's, start after it);
//   - SHODTargets gets no place in GearGRI, which has no free room: GearGRI has native subclasses,
//     so it cannot grow, and every array it has is in use in campaign games (sharing one breaks
//     both, and the garbage collector reads the other array's elements as objects). Its only user
//     is the satellite camera (GearPawn_SatelliteHODCamera.AddEnemyLocators, and
//     GearGRI.AddSHODTarget, which it calls), so BindShodTargets points it, as that function starts,
//     at the camera's own SHODTargetStore, a property of the patch's camera class that the garbage
//     collector follows like any other.
// Each step checks the layout it relies on and leaves the class untouched when it does not hold.
//
// The disc build's loader also breaks when a patch property is the first field of a class that had
// none (GearPawn_LocustGeneralRaamBase): it preloads the property while still reading the class,
// and the class then reads the rest of its data from the wrong place. DeferPreload makes that
// nested preload wait; the linker loads the property right after the class.

#pragma once

#include <cstdint>

namespace azura::dlc {

// Called after the linker preloads `object`: places the patch's properties (see above) once each
// of them, and the class it needs, is loaded. Each placement or failure is logged.
void OnPreloaded(uint8_t* base, uint32_t object);

// Points GearGRI.SHODTargets at `camera`'s SHODTargetStore, as `camera` (a satellite camera) starts
// AddEnemyLocators: script then reaches the store through the game's GRI.
void BindShodTargets(uint8_t* base, uint32_t camera);

// Whether the linker's preload of `object`, called from guest address `return_address`, must be
// skipped because it is a patch property preloaded from inside its class's serialization.
bool DeferPreload(const uint8_t* base, uint32_t object, uint32_t return_address);

}  // namespace azura::dlc
