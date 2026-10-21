#include "perf_overlay.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#endif
#include <fstream>
#include <sstream>
#include <imgui.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>

#include <rex/cvar.h>
#if defined(__APPLE__)
#include "platform_mac.h"
#endif

REXCVAR_DECLARE(bool, perf_overlay);
REXCVAR_DECLARE(bool, perf_overlay_fps_only);
REXCVAR_DECLARE(bool, perf_overlay_advanced);

namespace azura {

namespace {

// Keep the latest 600 guest-frame timestamps (about 10 seconds at 60 FPS) for low statistics.
constexpr size_t kMaxFrameHistory = 600;
std::array<std::chrono::steady_clock::time_point, kMaxFrameHistory> g_frame_timestamps{};
size_t g_frame_head = 0;
size_t g_frame_count = 0;
uint64_t g_total_frames = 0;
std::mutex g_frame_mutex;

#if defined(__linux__)
struct NvmlMemory {
  uint64_t total;
  uint64_t free;
  uint64_t used;
};

using NvmlInitFn = int (*)(void);
using NvmlDeviceGetHandleByIndexFn = int (*)(unsigned int, void**);
using NvmlDeviceGetMemoryInfoFn = int (*)(void*, NvmlMemory*);
using NvmlShutdownFn = int (*)(void);
#endif

}  // namespace

void RecordGuestFrame() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  g_frame_timestamps[g_frame_head] = now;
  g_frame_head = (g_frame_head + 1) % kMaxFrameHistory;
  if (g_frame_count < kMaxFrameHistory) {
    g_frame_count++;
  }
  g_total_frames++;
}

void GetGuestFrameStats(float& out_fps, float& out_frametime_ms, float& out_low_fps,
                        float& out_one_percent_low_fps, uint64_t& out_frame_count) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  out_frame_count = g_total_frames;
  out_fps = 0.0f;
  out_frametime_ms = 0.0f;
  out_low_fps = 0.0f;
  out_one_percent_low_fps = 0.0f;
  if (g_frame_count < 2) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const size_t last_idx = (g_frame_head + kMaxFrameHistory - 1) % kMaxFrameHistory;
  const auto last_time = g_frame_timestamps[last_idx];

  // If no frame has been presented for over 1.5 seconds, the game is paused or loading
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() > 1500) {
    return;
  }

  const size_t oldest_idx = (g_frame_head + kMaxFrameHistory - g_frame_count) % kMaxFrameHistory;
  std::array<float, kMaxFrameHistory> frame_times_ms{};
  size_t sample_count = 0;
  for (size_t i = 1; i < g_frame_count; ++i) {
    const size_t previous = (oldest_idx + i - 1) % kMaxFrameHistory;
    const size_t current = (oldest_idx + i) % kMaxFrameHistory;
    const float frame_ms = std::chrono::duration<float, std::milli>(
                               g_frame_timestamps[current] - g_frame_timestamps[previous])
                               .count();
    if (frame_ms > 0.0f) frame_times_ms[sample_count++] = frame_ms;
  }
  if (sample_count > 0) {
    const auto slowest = std::max_element(frame_times_ms.begin(), frame_times_ms.begin() + sample_count);
    out_low_fps = 1000.0f / *slowest;
    const size_t percentile_count = std::max<size_t>(1, (sample_count + 99) / 100);
    std::sort(frame_times_ms.begin(), frame_times_ms.begin() + sample_count, std::greater<float>());
    float slowest_total_ms = 0.0f;
    for (size_t i = 0; i < percentile_count; ++i) slowest_total_ms += frame_times_ms[i];
    out_one_percent_low_fps = 1000.0f * static_cast<float>(percentile_count) / slowest_total_ms;
  }

  // Calculate moving average over the last ~500ms for stable and accurate reading
  size_t frames_back = 0;
  for (size_t i = 1; i < g_frame_count; ++i) {
    size_t prev_idx = (g_frame_head + kMaxFrameHistory - 1 - i) % kMaxFrameHistory;
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(last_time - g_frame_timestamps[prev_idx]).count();
    frames_back = i;
    if (elapsed_ms >= 500) {
      break;
    }
  }

  if (frames_back > 0) {
    const size_t average_oldest_idx = (g_frame_head + kMaxFrameHistory - 1 - frames_back) % kMaxFrameHistory;
    int64_t us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            last_time - g_frame_timestamps[average_oldest_idx]).count();
    if (us > 0) {
      out_fps = static_cast<float>(frames_back) * 1000000.0f / static_cast<float>(us);
      out_frametime_ms = 1000.0f / out_fps;
      return;
    }
  }

}

PerfOverlayDialog::PerfOverlayDialog(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {
#if defined(__linux__)
  // Attempt to dynamically load NVML for accurate GPU VRAM usage on NVIDIA
  nvml_lib_ = dlopen("libnvidia-ml.so.1", RTLD_NOW);
  if (!nvml_lib_) {
    nvml_lib_ = dlopen("libnvidia-ml.so", RTLD_NOW);
  }
  if (nvml_lib_) {
    auto init_fn = reinterpret_cast<NvmlInitFn>(dlsym(nvml_lib_, "nvmlInit_v2"));
    if (!init_fn) {
      init_fn = reinterpret_cast<NvmlInitFn>(dlsym(nvml_lib_, "nvmlInit"));
    }
    auto get_handle_fn = reinterpret_cast<NvmlDeviceGetHandleByIndexFn>(
        dlsym(nvml_lib_, "nvmlDeviceGetHandleByIndex_v2"));
    if (!get_handle_fn) {
      get_handle_fn = reinterpret_cast<NvmlDeviceGetHandleByIndexFn>(
          dlsym(nvml_lib_, "nvmlDeviceGetHandleByIndex"));
    }

    if (init_fn && get_handle_fn && init_fn() == 0) {
      void* dev = nullptr;
      if (get_handle_fn(0, &dev) == 0) {
        nvml_device_ = dev;
        nvml_initialized_ = true;
      }
    }
  }
#endif

  UpdateStats();
}

PerfOverlayDialog::~PerfOverlayDialog() {
#if defined(__linux__)
  if (nvml_lib_) {
    if (nvml_initialized_) {
      auto shutdown_fn = reinterpret_cast<NvmlShutdownFn>(dlsym(nvml_lib_, "nvmlShutdown"));
      if (shutdown_fn) {
        shutdown_fn();
      }
    }
    dlclose(nvml_lib_);
    nvml_lib_ = nullptr;
  }
#endif
}

void PerfOverlayDialog::UpdateStats() {
#if defined(__linux__)
  // 1. CPU Usage from /proc/stat
  std::ifstream stat_file("/proc/stat");
  if (stat_file.is_open()) {
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (stat_file >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
      const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
      const uint64_t idle_all = idle + iowait;
      if (prev_cpu_total_ > 0 && total > prev_cpu_total_) {
        const uint64_t total_diff = total - prev_cpu_total_;
        const uint64_t idle_diff = idle_all - prev_cpu_idle_;
        cpu_percent_ = 100.0f * static_cast<float>(total_diff - idle_diff) / static_cast<float>(total_diff);
      }
      prev_cpu_total_ = total;
      prev_cpu_idle_ = idle_all;
    }
  }

  // 2. RAM Usage (process RSS and system memory)
  std::ifstream status_file("/proc/self/status");
  if (status_file.is_open()) {
    std::string line;
    while (std::getline(status_file, line)) {
      if (line.rfind("VmRSS:", 0) == 0) {
        uint64_t kb = 0;
        if (sscanf(line.c_str() + 6, "%lu", &kb) == 1) {
          ram_process_mb_ = kb / 1024;
        }
        break;
      }
    }
  }

  struct sysinfo si;
  if (sysinfo(&si) == 0) {
    const uint64_t unit = si.mem_unit ? si.mem_unit : 1;
    const uint64_t total_bytes = si.totalram * unit;
    ram_total_mb_ = total_bytes / (1024 * 1024);
    ram_used_mb_ = ((si.totalram - si.freeram) * unit) / (1024 * 1024);

    std::ifstream meminfo_file("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo_file, line)) {
      if (line.rfind("MemAvailable:", 0) != 0) continue;
      unsigned long long available_kb = 0;
      if (std::sscanf(line.c_str() + 13, "%llu", &available_kb) == 1) {
        const uint64_t available_bytes = static_cast<uint64_t>(available_kb) * 1024;
        ram_used_mb_ = available_bytes < total_bytes
                           ? (total_bytes - available_bytes) / (1024 * 1024)
                           : 0;
      }
      break;
    }
  }

  // 3. VRAM Usage
  if (nvml_initialized_ && nvml_device_) {
    auto mem_fn = reinterpret_cast<NvmlDeviceGetMemoryInfoFn>(
        dlsym(nvml_lib_, "nvmlDeviceGetMemoryInfo"));
    if (mem_fn) {
      NvmlMemory mem{};
      if (mem_fn(nvml_device_, &mem) == 0) {
        vram_used_mb_ = mem.used / (1024 * 1024);
        vram_total_mb_ = mem.total / (1024 * 1024);
      }
    }
  }
#elif defined(__APPLE__)
  host_cpu_load_info_data_t cpu_info{};
  mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu_info),
                     &cpu_count) == KERN_SUCCESS) {
    uint64_t total = 0;
    for (int i = 0; i < CPU_STATE_MAX; ++i) total += cpu_info.cpu_ticks[i];
    const uint64_t idle = cpu_info.cpu_ticks[CPU_STATE_IDLE];
    if (prev_cpu_total_ > 0 && total > prev_cpu_total_ && idle >= prev_cpu_idle_) {
      const uint64_t total_diff = total - prev_cpu_total_;
      const uint64_t idle_diff = idle - prev_cpu_idle_;
      cpu_percent_ = 100.0f * static_cast<float>(total_diff - idle_diff) / static_cast<float>(total_diff);
    }
    prev_cpu_total_ = total;
    prev_cpu_idle_ = idle;
  }

  uint64_t total_bytes = 0;
  size_t total_size = sizeof(total_bytes);
  if (sysctlbyname("hw.memsize", &total_bytes, &total_size, nullptr, 0) == 0) {
    ram_total_mb_ = total_bytes / (1024 * 1024);
    vm_statistics64_data_t vm_info{};
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_info),
                         &vm_count) == KERN_SUCCESS) {
      const uint64_t free_bytes = static_cast<uint64_t>(vm_info.free_count) * vm_kernel_page_size;
      ram_used_mb_ = (total_bytes > free_bytes ? total_bytes - free_bytes : 0) / (1024 * 1024);
    }
  }

  task_basic_info_data_t task_info_data{};
  mach_msg_type_number_t task_count = TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task_info_data),
                &task_count) == KERN_SUCCESS) {
    ram_process_mb_ = task_info_data.resident_size / (1024 * 1024);
  }
  uint64_t gpu_used_bytes = 0;
  uint64_t gpu_budget_bytes = 0;
  GetMacGpuMemoryStats(gpu_used_bytes, gpu_budget_bytes);
  vram_used_mb_ = gpu_used_bytes / (1024 * 1024);
  vram_total_mb_ = gpu_budget_bytes / (1024 * 1024);
#endif
}

void PerfOverlayDialog::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(perf_overlay)) return;
  const bool fps_only = REXCVAR_GET(perf_overlay_fps_only);
  const bool advanced = REXCVAR_GET(perf_overlay_advanced) && !fps_only;

  const auto now = std::chrono::steady_clock::now();
  if (!fps_only &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count() >= 500) {
    UpdateStats();
    last_update_ = now;
  }

  ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.70f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##PerfOverlay", nullptr, flags)) {
    float guest_fps = 0.0f;
    float guest_ms = 0.0f;
    float low_fps = 0.0f;
    float one_percent_low_fps = 0.0f;
    uint64_t guest_frame_count = 0;
    GetGuestFrameStats(guest_fps, guest_ms, low_fps, one_percent_low_fps, guest_frame_count);

    // Exact game frames presented on screen
    const bool has_guest = (guest_frame_count > 0 && guest_fps > 0.0f);
    const float fps = has_guest ? guest_fps : io.Framerate;
    const float ms = fps > 0.0f ? 1000.0f / fps : 0.0f;

    // Color code FPS
    ImVec4 fps_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);  // Green
    if (fps < 25.0f) {
      fps_color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);     // Red
    } else if (fps < 29.0f) {
      fps_color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);      // Yellow
    }

    if (fps_only) {
      ImGui::TextColored(fps_color, "%.1f FPS", fps);
      ImGui::End();
      return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Gears 3 | Performance");
    ImGui::SameLine();
    ImGui::TextColored(fps_color, "%.1f FPS", fps);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f ms)", ms);

    if (has_guest && std::abs(io.Framerate - guest_fps) > 2.0f) {
      ImGui::SameLine();
      ImGui::TextDisabled("| GPU: %.0f", io.Framerate);
    }

    ImGui::Separator();

    if (advanced) {
      ImGui::Text("Frame time: %.2f ms", has_guest ? guest_ms : ms);
      if (low_fps > 0.0f && one_percent_low_fps > 0.0f) {
        ImGui::Text("Minimum: %.1f FPS | 1%% low: %.1f FPS", low_fps, one_percent_low_fps);
      } else {
        ImGui::TextDisabled("Low: warming up");
      }
      ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "CPU:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", cpu_percent_);

    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.9f, 1.0f), "RAM:");
    ImGui::SameLine();
    ImGui::Text("%lu MB", ram_process_mb_);
    ImGui::SameLine();
    ImGui::TextDisabled("(Sys: %lu/%lu MB)", ram_used_mb_, ram_total_mb_);

#if defined(__APPLE__)
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.9f, 1.0f), "GPU memory (unified):");
#else
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.9f, 1.0f), "VRAM:");
#endif
    ImGui::SameLine();
    if (vram_total_mb_ > 0) {
      ImGui::Text("%lu MB / %lu MB", vram_used_mb_, vram_total_mb_);
    } else {
      ImGui::TextDisabled("N/A");
    }
  }
  ImGui::End();
}

}  // namespace azura
