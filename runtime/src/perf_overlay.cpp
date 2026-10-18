#include "perf_overlay.h"

#include <dlfcn.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <imgui.h>

namespace azura {

namespace {

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
    const float fps = io.Framerate;
    const float ms = fps > 0.0f ? 1000.0f / fps : 0.0f;

    // Color code FPS
    ImVec4 fps_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);  // Green
    if (fps < 30.0f) {
      fps_color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);     // Red
    } else if (fps < 55.0f) {
      fps_color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);      // Yellow
    }

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Gears 3 [F6]");
    ImGui::SameLine();
    ImGui::TextColored(fps_color, "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f ms)", ms);

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
