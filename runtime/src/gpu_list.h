// The graphics cards the game can run on, for the launcher's choice (--list_gpus).

#pragma once

namespace azura {

// Whether the runtime was started to list the graphics cards (list_gpus), not to run the game.
bool GpuListRequested();

// Prints the graphics cards Vulkan sees, and the one the game chooses automatically, as one line
// of JSON on standard output:
//   {"devices":[{"name":"...","uuid":"...","type":"discrete","vendorId":4318}],"automatic":"..."}
// The launcher passes the chosen card's uuid back as vulkan_device_uuid. Returns the exit code:
// 0, or 1 if Vulkan is not available.
int PrintGpuList();

}  // namespace azura
