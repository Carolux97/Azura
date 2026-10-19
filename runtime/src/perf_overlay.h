#pragma once

#include <rex/ui/imgui_dialog.h>
#include <chrono>
#include <cstdint>
#include <string>

namespace azura {

void RecordGuestFrame();
void GetGuestFrameStats(float& out_fps, float& out_frametime_ms, uint64_t& out_frame_count);

class PerfOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlayDialog(rex::ui::ImGuiDrawer* drawer);
  ~PerfOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void UpdateStats();

  std::chrono::steady_clock::time_point last_update_{};
  float cpu_percent_ = 0.0f;
  uint64_t prev_cpu_total_ = 0;
  uint64_t prev_cpu_idle_ = 0;

  uint64_t ram_process_mb_ = 0;
  uint64_t ram_total_mb_ = 0;
  uint64_t ram_used_mb_ = 0;

  uint64_t vram_used_mb_ = 0;
  uint64_t vram_total_mb_ = 0;
  bool nvml_initialized_ = false;
  void* nvml_lib_ = nullptr;
  void* nvml_device_ = nullptr;
};

}  // namespace azura
