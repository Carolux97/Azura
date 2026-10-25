// Windows-specific pieces of the runtime: start-up, and which parts of guest memory can be read.

#pragma once

#include <cstddef>
#include <vector>

namespace azura {

// Keeps out of the game the implicit Vulkan layers of overlay programs that close it as the intro
// movie starts (Overwolf's two and RivaTuner Statistics Server's, each enough on its own), by
// setting the variable each layer's manifest names to disable it. A variable the player already
// set is left alone, so setting one to 0 keeps that overlay. Call before Vulkan starts.
void DisableCrashingVulkanLayers();

// Logs which layers DisableCrashingVulkanLayers kept out. Call once logging is ready.
void LogDisabledVulkanLayers();

// A stretch of readable memory, as offsets from the start of the region it was asked about.
struct ReadableRange {
  size_t begin;
  size_t end;
};

// The parts of [base, base + size) that can be read, in order. The SDK reserves the guest's memory
// and commits pages as the game allocates them; reading a page that is only reserved is an access
// violation, which ends the process.
std::vector<ReadableRange> ReadableRanges(const void* base, size_t size);

// Whether all of [data, data + size) can be read.
bool IsReadable(const void* data, size_t size);

}  // namespace azura
