// The performance overlay that the Video options turn on (Performance overlay, the
// performance_overlay setting): a small rounded panel in a corner of the game image (Overlay
// position, performance_overlay_position), in the launcher's colours as Azura's other screens
// (coop_ui.h). It has three modes:
//   - FPS: the game's frame rate in large figures, coloured by how close it is to the frame rate
//     limit, and the time per frame;
//   - Detailed: the same, and below it the game's CPU and GPU use in one row (perf_stats.h), N/A
//     where this computer or build cannot read one;
//   - Advanced: the frame rate row, then the CPU, GPU and memory use one per row, a graph of the
//     last seconds of frame times with the target frame time marked, and the average frame rate
//     and 1% low (the frame rate of the slowest 1% of frames) over the last ten seconds.
// The readings of a mode are only taken while it is on.
// It uses the Barlow fonts of the shader compilation notice and the Local co-op menu.
//
// The frame rate is the game's own: frames the game presents, counted at its call to VdSwap
// (sub_82AF6B10, the only function of the game that calls it), not the frames the host window
// draws. Frames are only timed while the overlay is on; off, the hook only reads a flag.
//
// The dialog lives for the whole run and draws nothing while the overlay is off. It draws on
// ImGui's foreground draw list, with no window, so it never takes input. In the top left corner it
// takes the place of the shader compilation notice, which then shows below it
// (PerfOverlayTopLeftHeight).

#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "perf_stats.h"

struct ImFont;

namespace azura {

class PerfOverlay : public rex::ui::ImGuiDialog {
 public:
  // The fonts are read at each draw, since the app loads them after creating its dialogs; a null
  // font draws in ImGui's default one.
  PerfOverlay(rex::ui::ImGuiDrawer* drawer, ImFont* const* number_font, ImFont* const* text_font);
  ~PerfOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  using Clock = std::chrono::steady_clock;

  ImFont* const* number_font_;
  ImFont* const* text_font_;
  // The frame rate shown and when it was last worked out: it changes a few times a second, so it
  // can be read.
  int shown_fps_ = 0;
  Clock::time_point shown_at_{};

  // The Detailed and Advanced modes' readings, worked out with the frame rate. The GPU use is the
  // average of the last frames' values gathered at each draw since then.
  bool was_reading_ = false;
  CpuUsage cpu_usage_;
  std::optional<float> shown_cpu_;
  std::optional<float> shown_gpu_;
  float gpu_sum_ = 0.0f;
  int gpu_samples_ = 0;
  // The Advanced mode's: memory use, and the average frame rate and 1% low (0 without frames).
  std::optional<MemoryUse> shown_memory_;
  int shown_average_fps_ = 0;
  int shown_low_fps_ = 0;
  // Frame times in milliseconds, kept to reuse their memory: the graph's and the statistics'.
  std::vector<float> graph_times_;
  std::vector<float> stats_times_;
};

// How far down from the top of the game image the overlay's panel reaches when it shows in the top
// left corner, in pixels at 720p; 0 when it shows elsewhere or not at all. The shader compilation
// notice moves down by this much so the two do not overlap.
float PerfOverlayTopLeftHeight();
// The same for the top right corner, where the online room notice shows (online_room_overlay.h).
float PerfOverlayTopRightHeight();

}  // namespace azura
