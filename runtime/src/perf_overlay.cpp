#include "perf_overlay.h"

#include <dlfcn.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <imgui.h>

#include <array>
#include <cmath>
#include <mutex>

namespace azura {

namespace {

constexpr size_t kMaxFrameHistory = 120;
std::array<std::chrono::steady_clock::time_point, kMaxFrameHistory> g_frame_timestamps{};
size_t g_frame_head = 0;
size_t g_frame_count = 0;
uint64_t g_total_frames = 0;
std::mutex g_frame_mutex;

struct NvmlMemory {
  uint64_t total;
  uint64_t free;
  uint64_t used;
};

using NvmlInitFn = int (*)(void);
using NvmlDeviceGetHandleByIndexFn = int (*)(unsigned int, void**);
using NvmlDeviceGetMemoryInfoFn = int (*)(void*, NvmlMemory*);
using NvmlShutdownFn = int (*)(void);

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

void GetGuestFrameStats(float& out_fps, float& out_frametime_ms, uint64_t& out_frame_count) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  out_frame_count = g_total_frames;
  if (g_frame_count < 2) {
    out_fps = 0.0f;
    out_frametime_ms = 0.0f;
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const size_t last_idx = (g_frame_head + kMaxFrameHistory - 1) % kMaxFrameHistory;
  const auto last_time = g_frame_timestamps[last_idx];

  // If no frame has been presented for over 1.5 seconds, the game is paused or loading
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() > 1500) {
    out_fps = 0.0f;
    out_frametime_ms = 0.0f;
    return;
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
    size_t oldest_idx = (g_frame_head + kMaxFrameHistory - 1 - frames_back) % kMaxFrameHistory;
    int64_t us =
        std::chrono::duration_cast<std::chrono::microseconds>(last_time - g_frame_timestamps[oldest_idx]).count();
    if (us > 0) {
      out_fps = static_cast<float>(frames_back) * 1000000.0f / static_cast<float>(us);
      out_frametime_ms = 1000.0f / out_fps;
      return;
    }
  }

  out_fps = 0.0f;
  out_frametime_ms = 0.0f;
}

PerfOverlayDialog::PerfOverlayDialog(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {
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

  UpdateStats();
}

PerfOverlayDialog::~PerfOverlayDialog() {
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
}

void PerfOverlayDialog::UpdateStats() {
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
    ram_total_mb_ = (si.totalram * unit) / (1024 * 1024);
    ram_used_mb_ = ((si.totalram - si.freeram) * unit) / (1024 * 1024);
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
}

void PerfOverlayDialog::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count() >= 500) {
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
    uint64_t guest_frame_count = 0;
    GetGuestFrameStats(guest_fps, guest_ms, guest_frame_count);

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

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Gears 3 [F6]");
    ImGui::SameLine();
    ImGui::TextColored(fps_color, "%.1f FPS", fps);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f ms)", ms);

    if (has_guest && std::abs(io.Framerate - guest_fps) > 2.0f) {
      ImGui::SameLine();
      ImGui::TextDisabled("| GPU: %.0f", io.Framerate);
    }

    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "CPU:");
    ImGui::SameLine();
    ImGui::Text("%.1f%%", cpu_percent_);

    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.9f, 1.0f), "RAM:");
    ImGui::SameLine();
    ImGui::Text("%lu MB", ram_process_mb_);
    ImGui::SameLine();
    ImGui::TextDisabled("(Sys: %lu/%lu MB)", ram_used_mb_, ram_total_mb_);

    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.9f, 1.0f), "VRAM:");
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
