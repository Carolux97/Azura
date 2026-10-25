#include "perf_stats.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs. K32GetProcessMemoryInfo is in kernel32, so no psapi.lib.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#else
#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#endif

#include <rex/perf/counter.h>

namespace azura {

namespace {

// CPU time the process has used so far, user and system together, in seconds; negative when it
// cannot be read.
double ProcessCpuSeconds() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  // FILETIME counts 100 ns intervals.
  auto seconds = [](const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) * 1e-7;
  };
  return seconds(kernel) + seconds(user);
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return -1.0;
  auto seconds = [](const timeval& time) {
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) * 1e-6;
  };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}

double WallSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::optional<float> CpuUsage::Sample() {
  const double cpu = ProcessCpuSeconds();
  const double wall = WallSeconds();
  if (cpu < 0.0) return std::nullopt;
  const bool first = last_cpu_seconds_ < 0.0;
  const double cpu_used = cpu - last_cpu_seconds_;
  const double elapsed = wall - last_wall_seconds_;
  last_cpu_seconds_ = cpu;
  last_wall_seconds_ = wall;
  if (first || elapsed <= 0.0) return std::nullopt;
  const unsigned processors = std::max(1u, std::thread::hardware_concurrency());
  const double share = cpu_used / (elapsed * processors) * 100.0;
  return static_cast<float>(std::clamp(share, 0.0, 100.0));
}

std::optional<float> LastFrameGpuUsage() {
#if defined(REXGLUE_ENABLE_PERF_COUNTERS)
  // Both are the values of the last frame the command processor finished (the counters' snapshot,
  // taken at each of the game's swaps): its duration, and the GPU time of the work that completed
  // during it.
  const int64_t frame_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
  const int64_t gpu_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kHostGpuTimeUs);
  if (frame_us <= 0 || gpu_us <= 0) return std::nullopt;
  const double share = static_cast<double>(gpu_us) / static_cast<double>(frame_us) * 100.0;
  return static_cast<float>(std::clamp(share, 0.0, 100.0));
#else
  return std::nullopt;
#endif
}

std::optional<MemoryUse> ProcessMemoryUse() {
  MemoryUse memory;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return std::nullopt;
  memory.used = counters.WorkingSetSize;
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return std::nullopt;
  memory.total = status.ullTotalPhys;
#elif defined(__APPLE__)
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return std::nullopt;
  }
  memory.used = info.phys_footprint;
  uint64_t total = 0;
  size_t size = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0) return std::nullopt;
  memory.total = total;
#else
  // /proc/self/statm: total program size, then resident pages.
  std::ifstream statm("/proc/self/statm");
  uint64_t size_pages = 0, resident_pages = 0;
  if (!(statm >> size_pages >> resident_pages)) return std::nullopt;
  const long page_size = sysconf(_SC_PAGE_SIZE);
  const long pages = sysconf(_SC_PHYS_PAGES);
  if (page_size <= 0 || pages <= 0) return std::nullopt;
  memory.used = resident_pages * static_cast<uint64_t>(page_size);
  memory.total = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
  if (memory.total == 0) return std::nullopt;
  return memory;
}

}  // namespace azura
