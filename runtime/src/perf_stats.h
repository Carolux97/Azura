// Readings of the performance overlay's Detailed and Advanced modes (perf_overlay.h): how much of
// the computer's CPU the game uses, how much of each frame the GPU spends on the game, and how much
// memory the game takes of the computer's total. One interface for every platform; a reading a
// platform or build cannot take is reported as missing, and the overlay shows N/A for it.

#pragma once

#include <cstdint>
#include <optional>

namespace azura {

// The game's CPU use: the CPU time the whole process used between two calls of Sample, as a share
// of all the computer's logical processors over that time (0 to 100, as the system's activity
// monitors show it with all cores counted together).
class CpuUsage {
 public:
  // The use since the previous call, or nothing on the first call and where the process's CPU
  // time cannot be read.
  std::optional<float> Sample();

 private:
  double last_cpu_seconds_ = -1.0;
  double last_wall_seconds_ = 0.0;
};

// The game's GPU use: the GPU time of the game's last frame (the SDK's host_gpu_time_us counter,
// measured by the Metal and Vulkan backends) over that frame's duration, 0 to 100. Nothing when
// the build has no performance counters, when the backend does not time its GPU work (a Vulkan
// queue without timestamps) or before the game has presented two frames. It is the game's own GPU
// time, not what other programs use.
std::optional<float> LastFrameGpuUsage();

// The game's memory use against the computer's: what the process holds in physical memory (the
// footprint the Activity Monitor shows on macOS, the resident set on Linux, the working set on
// Windows) and the computer's physical memory, in bytes.
struct MemoryUse {
  uint64_t used = 0;
  uint64_t total = 0;
};
std::optional<MemoryUse> ProcessMemoryUse();

}  // namespace azura
