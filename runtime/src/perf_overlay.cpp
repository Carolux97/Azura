#include "perf_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "coop_ui.h"
#include "menu/pc_options.h"

REXCVAR_DECLARE(double, frame_rate_limit);

REXCVAR_DEFINE_STRING(performance_overlay, "off", "Azura/Graphics",
                      "Performance overlay: off, fps for the game's frame rate in a corner, detailed to "
                      "add the game's CPU and GPU use, or advanced to add its memory use, a graph of "
                      "frame times, the average frame rate and the 1% low");
REXCVAR_DEFINE_STRING(performance_overlay_position, "top_right", "Azura/Graphics",
                      "Corner of the performance overlay: top_left, top_right, bottom_left or bottom_right");

// The game's function that presents a frame (it calls VdSwap). The code generator emits every
// function as a weak symbol forwarding to __imp__<name>, so defining it here runs our code first.
extern "C" void __imp__sub_82AF6B10(PPCContext& __restrict ctx, uint8_t* base);

namespace azura {

namespace {

using Clock = std::chrono::steady_clock;

// Times of the most recent frames the game presented, oldest overwritten first: enough for the
// Advanced mode's statistics window (kStatsWindow) at up to about 200 frames per second.
constexpr size_t kFrameHistory = 2048;
std::array<Clock::time_point, kFrameHistory> g_frames{};
size_t g_next_frame = 0;
size_t g_frame_count = 0;
std::mutex g_frames_mutex;

// Whether frames are timed: only while the overlay shows something.
std::atomic<bool> g_timing{false};

// The frame rate is the average over about this long: steady enough to read.
constexpr auto kAverageWindow = std::chrono::milliseconds(500);
// How often the number shown changes.
constexpr auto kShowInterval = std::chrono::milliseconds(250);
// Without a frame for this long the game is paused or loading, and the overlay shows 0.
constexpr auto kNoFrames = std::chrono::milliseconds(1500);
// The Advanced mode: how far back its graph of frame times reaches, and the window its average
// frame rate and 1% low are worked out over.
constexpr auto kGraphWindow = std::chrono::seconds(5);
constexpr auto kStatsWindow = std::chrono::seconds(10);

void RecordFrame() {
  const auto now = Clock::now();
  std::lock_guard lock(g_frames_mutex);
  g_frames[g_next_frame] = now;
  g_next_frame = (g_next_frame + 1) % kFrameHistory;
  g_frame_count = std::min(g_frame_count + 1, kFrameHistory);
}

void ForgetFrames() {
  std::lock_guard lock(g_frames_mutex);
  g_frame_count = 0;
}

// Frames per second over the last kAverageWindow, from the time between the first and the last
// frame in it; 0 when the game has not presented a frame lately.
int FramesPerSecond(Clock::time_point now) {
  std::lock_guard lock(g_frames_mutex);
  if (g_frame_count < 2) return 0;
  const Clock::time_point newest = g_frames[(g_next_frame + kFrameHistory - 1) % kFrameHistory];
  if (now - newest > kNoFrames) return 0;
  size_t frames = 0;
  Clock::time_point oldest = newest;
  for (size_t i = 1; i < g_frame_count; ++i) {
    const Clock::time_point frame = g_frames[(g_next_frame + kFrameHistory - 1 - i) % kFrameHistory];
    if (newest - frame > kAverageWindow) break;
    oldest = frame;
    frames = i;
  }
  const double seconds = std::chrono::duration<double>(newest - oldest).count();
  return seconds > 0.0 ? static_cast<int>(std::lround(frames / seconds)) : 0;
}

// The time between each pair of consecutive frames of the last `window`, oldest first, in
// milliseconds; empty when the game has not presented a frame lately.
void RecentFrameTimes(Clock::time_point now, Clock::duration window, std::vector<float>& out) {
  out.clear();
  std::lock_guard lock(g_frames_mutex);
  if (g_frame_count < 2) return;
  const size_t newest_index = (g_next_frame + kFrameHistory - 1) % kFrameHistory;
  const Clock::time_point newest = g_frames[newest_index];
  if (now - newest > kNoFrames) return;
  // Walk back to the oldest frame inside the window, then forward from there.
  size_t count = 1;
  while (count < g_frame_count &&
         newest - g_frames[(g_next_frame + kFrameHistory - 1 - count) % kFrameHistory] <= window) {
    ++count;
  }
  out.reserve(count - 1);
  for (size_t i = count - 1; i > 0; --i) {
    const Clock::time_point earlier = g_frames[(g_next_frame + kFrameHistory - 1 - i) % kFrameHistory];
    const Clock::time_point later = g_frames[(g_next_frame + kFrameHistory - i) % kFrameHistory];
    out.push_back(std::chrono::duration<float, std::milli>(later - earlier).count());
  }
}

// The frame rate the game aims for: the Video menu's frame rate limit, or 60 without one.
double TargetFramesPerSecond() {
  const double limit = REXCVAR_GET(frame_rate_limit);
  return (limit <= 0.0 || limit >= menu::kFrameRateUnlimited) ? 60.0 : limit;
}

// The panel's height below the top of the game image while it shows in the top left or the top
// right corner, in pixels at 720p (PerfOverlayTopLeftHeight, PerfOverlayTopRightHeight). Only the
// UI thread reads and writes them.
float g_top_left_height = 0.0f;
float g_top_right_height = 0.0f;

// Draws text with extra space between its letters, as the panel's "FPS" label; returns its width.
float DrawSpacedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position, ImU32 color,
                     const char* text, float spacing) {
  float x = position.x;
  for (const char* c = text; *c; ++c) {
    const char letter[2] = {*c, '\0'};
    if (draw_list) draw_list->AddText(font, size, ImVec2(x, position.y), color, letter);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, letter).x + (c[1] ? spacing : 0.0f);
  }
  return x - position.x;
}

}  // namespace

float PerfOverlayTopLeftHeight() {
  return g_top_left_height;
}

float PerfOverlayTopRightHeight() {
  return g_top_right_height;
}

PerfOverlay::PerfOverlay(rex::ui::ImGuiDrawer* drawer, ImFont* const* number_font,
                         ImFont* const* text_font)
    : ImGuiDialog(drawer), number_font_(number_font), text_font_(text_font) {}

PerfOverlay::~PerfOverlay() {
  g_timing.store(false, std::memory_order_relaxed);
  g_top_left_height = 0.0f;
  g_top_right_height = 0.0f;
}

void PerfOverlay::OnDraw(ImGuiIO& io) {
  const std::string mode = REXCVAR_GET(performance_overlay);
  const bool advanced = mode == "advanced";
  const bool detailed = mode == "detailed";
  const bool reading = detailed || advanced;
  const bool on = reading || mode == "fps";
  if (on != g_timing.load(std::memory_order_relaxed)) {
    // Turned on: count from now, not from frames timed the last time it was on.
    if (on) ForgetFrames();
    g_timing.store(on, std::memory_order_relaxed);
  }
  g_top_left_height = 0.0f;
  g_top_right_height = 0.0f;
  if (!reading) {
    was_reading_ = false;
  } else if (!was_reading_) {
    // Turned to Detailed or Advanced: the first CPU sample only sets its starting point.
    cpu_usage_ = CpuUsage();
    cpu_usage_.Sample();
    shown_cpu_.reset();
    shown_gpu_.reset();
    shown_memory_.reset();
    gpu_sum_ = 0.0f;
    gpu_samples_ = 0;
    was_reading_ = true;
  }
  if (!on) return;

  // The GPU use is the last frame's: add it at every draw and show the average, so the number
  // changes as slowly as the frame rate.
  if (reading) {
    if (const std::optional<float> gpu = LastFrameGpuUsage()) {
      gpu_sum_ += *gpu;
      ++gpu_samples_;
    }
  }

  const auto now = Clock::now();
  if (now - shown_at_ >= kShowInterval) {
    shown_fps_ = FramesPerSecond(now);
    shown_at_ = now;
    if (reading) {
      shown_cpu_ = cpu_usage_.Sample();
      shown_gpu_ = gpu_samples_ > 0 ? std::optional<float>(gpu_sum_ / gpu_samples_) : std::nullopt;
      gpu_sum_ = 0.0f;
      gpu_samples_ = 0;
    }
    if (advanced) {
      shown_memory_ = ProcessMemoryUse();
      // Average frame rate and 1% low over the statistics window: the frame rate of all its
      // frames, and of its slowest 1% (at least one frame).
      RecentFrameTimes(now, kStatsWindow, stats_times_);
      shown_average_fps_ = 0;
      shown_low_fps_ = 0;
      if (!stats_times_.empty()) {
        double total_ms = 0.0;
        for (float ms : stats_times_) total_ms += ms;
        const size_t slowest = std::max<size_t>(1, (stats_times_.size() + 99) / 100);
        std::nth_element(stats_times_.begin(), stats_times_.begin() + (slowest - 1), stats_times_.end(),
                         std::greater<float>());
        double slowest_ms = 0.0;
        for (size_t i = 0; i < slowest; ++i) slowest_ms += stats_times_[i];
        if (total_ms > 0.0) {
          shown_average_fps_ = static_cast<int>(std::lround(1000.0 * stats_times_.size() / total_ms));
        }
        if (slowest_ms > 0.0) shown_low_fps_ = static_cast<int>(std::lround(1000.0 * slowest / slowest_ms));
      }
    }
  }

  // Sizes in pixels at 720p, scaled with the 16:9 area the game image takes in the window, as the
  // shader compilation notice.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_width = game_height * 16.0f / 9.0f;
  const float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  const float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  const float unit = game_height / 720.0f;

  ImFont* number_font = (number_font_ && *number_font_) ? *number_font_ : ImGui::GetFont();
  ImFont* text_font = (text_font_ && *text_font_) ? *text_font_ : ImGui::GetFont();
  const float number_size = 30.0f * unit;
  const float label_size = 11.0f * unit;
  const float time_size = 12.0f * unit;
  const float label_spacing = 2.0f * unit;
  const float gap = 6.0f * unit;
  const float padding_x = 12.0f * unit;
  const float padding_y = 6.0f * unit;
  // Below the frame rate row: a line, then the readings.
  const float detail_size = 12.0f * unit;
  const float detail_gap = 6.0f * unit;
  const float column_gap = 14.0f * unit;
  // Advanced only: the readings' rows, the graph and the statistics row.
  const float row_height = 16.0f * unit;
  const float graph_height = 40.0f * unit;
  const float advanced_width = 180.0f * unit;

  auto text_width = [&](float size, const std::string& text) {
    return text_font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  };
  auto spaced_width = [&](const char* text) {
    return DrawSpacedText(nullptr, text_font, label_size, ImVec2(), 0, text, label_spacing);
  };

  // First row: the frame rate, "FPS" and the time per frame, sitting on the number's baseline.
  char number[16];
  std::snprintf(number, sizeof(number), "%d", shown_fps_);
  char time[24] = "";
  if (shown_fps_ > 0) std::snprintf(time, sizeof(time), "(%.1f ms)", 1000.0 / shown_fps_);
  const ImVec2 number_extent = number_font->CalcTextSizeA(number_size, FLT_MAX, 0.0f, number);
  const float label_width = spaced_width("FPS");
  const float time_width = time[0] ? text_width(time_size, time) + gap : 0.0f;
  const float first_row_width = number_extent.x + gap + label_width + time_width;

  // The readings, N/A for one this computer or build cannot take.
  auto percent = [](const std::optional<float>& value) {
    char text[16];
    if (value) {
      std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(std::lround(*value)));
    } else {
      std::snprintf(text, sizeof(text), "N/A");
    }
    return std::string(text);
  };
  const std::string cpu_text = percent(shown_cpu_);
  const std::string gpu_text = percent(shown_gpu_);
  std::string memory_text = "N/A";
  if (shown_memory_) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f / %.0f GB", shown_memory_->used / kGiB,
                  std::round(shown_memory_->total / kGiB));
    memory_text = text;
  }

  // Detailed: "CPU 34%   GPU 55%" in one row.
  float detailed_row_width = 0.0f;
  if (detailed) {
    detailed_row_width = spaced_width("CPU") + gap + text_width(detail_size, cpu_text) + column_gap +
                         spaced_width("GPU") + gap + text_width(detail_size, gpu_text);
  }
  float below_height = 0.0f;
  if (detailed) below_height = detail_gap * 2.0f + detail_size;
  if (advanced) below_height = detail_gap * 2.0f + row_height * 3.0f + detail_gap + graph_height + detail_gap + row_height;
  float content_width = std::max(first_row_width, detailed_row_width);
  if (advanced) content_width = std::max(content_width, advanced_width);
  const ImVec2 panel_size(padding_x * 2.0f + content_width,
                          padding_y * 2.0f + number_extent.y + below_height);

  // The corner the Video options choose, inside the game image.
  const float margin = 16.0f * unit;
  const std::string corner = REXCVAR_GET(performance_overlay_position);
  const bool right = corner != "top_left" && corner != "bottom_left";
  const bool bottom = corner == "bottom_left" || corner == "bottom_right";
  const ImVec2 panel_min(right ? game_left + game_width - margin - panel_size.x : game_left + margin,
                         bottom ? game_top + game_height - margin - panel_size.y : game_top + margin);
  const ImVec2 panel_max(panel_min.x + panel_size.x, panel_min.y + panel_size.y);
  if (!right && !bottom) g_top_left_height = (panel_max.y - game_top) / unit;
  if (right && !bottom) g_top_right_height = (panel_max.y - game_top) / unit;

  // The launcher's panel (coop_ui.h), with rounded corners.
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  const float rounding = 8.0f * unit;
  const float line = std::max(1.0f, unit);
  draw_list->AddRectFilled(panel_min, panel_max, coop_ui::Panel(214), rounding);
  draw_list->AddRect(panel_min, panel_max, coop_ui::SteelLine(255), rounding, 0, line);

  // The launcher's state colours: green at the frame rate the game aims for, imulsion gold a little
  // under it, red well under it (25 and 29 of 30, as the F6 overlay had for the console's 30).
  const double target = TargetFramesPerSecond();
  ImU32 color = coop_ui::Ok(255);
  if (shown_fps_ < target * 25.0 / 30.0) {
    color = coop_ui::ErrorText(255);
  } else if (shown_fps_ < target * 29.0 / 30.0) {
    color = coop_ui::Imulsion(255);
  }
  const float left = panel_min.x + padding_x;
  const float right_edge = panel_max.x - padding_x;
  float x = left;
  const float top = panel_min.y + padding_y;
  draw_list->AddText(number_font, number_size, ImVec2(x, top), color, number);
  x += number_extent.x + gap;
  // Barlow's baseline sits at about 80% of the line height: line the small texts up with it.
  const float baseline = top + number_size * 0.80f;
  DrawSpacedText(draw_list, text_font, label_size, ImVec2(x, baseline - label_size * 0.80f),
                 coop_ui::Text(255), "FPS", label_spacing);
  x += label_width + gap;
  if (time[0]) {
    // In Advanced the time per frame sits at the right edge, under which the readings' values line up.
    const float time_x = advanced ? right_edge - text_width(time_size, time) : x;
    draw_list->AddText(text_font, time_size, ImVec2(time_x, baseline - time_size * 0.80f),
                       coop_ui::TextDim(255), time);
  }
  if (!reading) return;

  float y = top + number_extent.y + detail_gap;
  draw_list->AddLine(ImVec2(left, y), ImVec2(right_edge, y), coop_ui::SteelLine(255), line);
  y += detail_gap;

  // A label in small spaced capitals with its value after it (`value_x` < 0) or against the right
  // edge, both on one baseline.
  auto reading_at = [&](float label_x, float row_top, const char* name, const std::string& value,
                        float value_x) {
    const float row_baseline = row_top + detail_size * 0.80f;
    const float name_width =
        DrawSpacedText(draw_list, text_font, label_size, ImVec2(label_x, row_baseline - label_size * 0.80f),
                       coop_ui::TextFaint(255), name, label_spacing);
    const float at = value_x >= 0.0f ? value_x - text_width(detail_size, value) : label_x + name_width + gap;
    draw_list->AddText(text_font, detail_size, ImVec2(at, row_top), coop_ui::Text(255), value.c_str());
    return at + text_width(detail_size, value);
  };

  if (detailed) {
    const float cpu_end = reading_at(left, y, "CPU", cpu_text, -1.0f);
    reading_at(cpu_end + column_gap, y, "GPU", gpu_text, -1.0f);
    return;
  }

  // Advanced: one reading per row, values against the right edge.
  reading_at(left, y, "CPU", cpu_text, right_edge);
  reading_at(left, y + row_height, "GPU", gpu_text, right_edge);
  reading_at(left, y + row_height * 2.0f, "RAM", memory_text, right_edge);
  y += row_height * 3.0f + detail_gap;

  // The graph of the last frame times: newest at the right, the frame time the game aims for as a
  // dashed line halfway up, longer frames clipped at the top.
  const ImVec2 graph_min(left, y);
  const ImVec2 graph_max(right_edge, y + graph_height);
  const float graph_rounding = 4.0f * unit;
  draw_list->AddRectFilled(graph_min, graph_max, IM_COL32(0, 0, 0, 90), graph_rounding);
  draw_list->AddRect(graph_min, graph_max, coop_ui::Steel(255), graph_rounding, 0, line);
  const float target_ms = static_cast<float>(1000.0 / target);
  const float top_ms = target_ms * 2.0f;
  const float inner_top = graph_min.y + 2.0f * unit;
  const float inner_bottom = graph_max.y - 2.0f * unit;
  auto graph_y = [&](float ms) {
    return inner_bottom - std::min(ms, top_ms) / top_ms * (inner_bottom - inner_top);
  };
  const float target_y = graph_y(target_ms);
  const float dash = 4.0f * unit;
  for (float dx = graph_min.x + 3.0f * unit; dx < graph_max.x - 3.0f * unit; dx += dash * 2.0f) {
    draw_list->AddLine(ImVec2(dx, target_y), ImVec2(std::min(dx + dash, graph_max.x - 3.0f * unit), target_y),
                       coop_ui::SteelLine(255), line);
  }
  RecentFrameTimes(now, kGraphWindow, graph_times_);
  if (graph_times_.size() >= 2) {
    // Each frame is placed by when it ended, so the graph scrolls at a steady speed whatever the
    // frame rate.
    const float window_ms = std::chrono::duration<float, std::milli>(kGraphWindow).count();
    const float inner_left = graph_min.x + 2.0f * unit;
    const float inner_right = graph_max.x - 2.0f * unit;
    std::vector<ImVec2> points;
    points.reserve(graph_times_.size());
    float ended_ms = 0.0f;  // back from the newest frame
    for (size_t i = graph_times_.size(); i-- > 0;) {
      const float px = inner_right - ended_ms / window_ms * (inner_right - inner_left);
      points.push_back(ImVec2(std::max(px, inner_left), graph_y(graph_times_[i])));
      ended_ms += graph_times_[i];
    }
    draw_list->AddPolyline(points.data(), static_cast<int>(points.size()), coop_ui::Imulsion(255), 0,
                           1.5f * unit);
  }
  y = graph_max.y + detail_gap;

  // The average frame rate and the 1% low.
  auto whole = [](int value) { return value > 0 ? std::to_string(value) : std::string("-"); };
  reading_at(left, y, "AVG", whole(shown_average_fps_), -1.0f);
  const std::string low = whole(shown_low_fps_);
  const float low_label_x = right_edge - text_width(detail_size, low) - gap - spaced_width("1% LOW");
  reading_at(low_label_x, y, "1% LOW", low, -1.0f);
}

}  // namespace azura

extern "C" void sub_82AF6B10(PPCContext& __restrict ctx, uint8_t* base) {
  if (azura::g_timing.load(std::memory_order_relaxed)) {
    azura::RecordFrame();
  }
  __imp__sub_82AF6B10(ctx, base);
}
