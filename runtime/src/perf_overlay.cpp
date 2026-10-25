#include "perf_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "menu/pc_options.h"

REXCVAR_DECLARE(double, frame_rate_limit);

REXCVAR_DEFINE_STRING(performance_overlay, "off", "Azura/Graphics",
                      "Performance overlay: off, or fps for the game's frame rate in a corner");
REXCVAR_DEFINE_STRING(performance_overlay_position, "top_right", "Azura/Graphics",
                      "Corner of the performance overlay: top_left, top_right, bottom_left or bottom_right");

// The game's function that presents a frame (it calls VdSwap). The code generator emits every
// function as a weak symbol forwarding to __imp__<name>, so defining it here runs our code first.
extern "C" void __imp__sub_82AF6B10(PPCContext& __restrict ctx, uint8_t* base);

namespace azura {

namespace {

using Clock = std::chrono::steady_clock;

// Times of the most recent frames the game presented, oldest overwritten first.
constexpr size_t kFrameHistory = 256;
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

// The frame rate the game aims for: the Video menu's frame rate limit, or 60 without one.
double TargetFramesPerSecond() {
  const double limit = REXCVAR_GET(frame_rate_limit);
  return (limit <= 0.0 || limit >= menu::kFrameRateUnlimited) ? 60.0 : limit;
}

// The panel's height below the top of the game image while it shows in the top left corner, in
// pixels at 720p (PerfOverlayTopLeftHeight). Only the UI thread reads and writes it.
float g_top_left_height = 0.0f;

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

PerfOverlay::PerfOverlay(rex::ui::ImGuiDrawer* drawer, ImFont* const* number_font,
                         ImFont* const* text_font)
    : ImGuiDialog(drawer), number_font_(number_font), text_font_(text_font) {}

PerfOverlay::~PerfOverlay() {
  g_timing.store(false, std::memory_order_relaxed);
  g_top_left_height = 0.0f;
}

void PerfOverlay::OnDraw(ImGuiIO& io) {
  const bool on = REXCVAR_GET(performance_overlay) == "fps";
  if (on != g_timing.load(std::memory_order_relaxed)) {
    // Turned on: count from now, not from frames timed the last time it was on.
    if (on) ForgetFrames();
    g_timing.store(on, std::memory_order_relaxed);
  }
  g_top_left_height = 0.0f;
  if (!on) return;

  const auto now = Clock::now();
  if (now - shown_at_ >= kShowInterval) {
    shown_fps_ = FramesPerSecond(now);
    shown_at_ = now;
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

  // One row: the frame rate, "FPS" and the time per frame, sitting on the number's baseline.
  char number[16];
  std::snprintf(number, sizeof(number), "%d", shown_fps_);
  char time[24] = "";
  if (shown_fps_ > 0) std::snprintf(time, sizeof(time), "(%.1f ms)", 1000.0 / shown_fps_);
  const ImVec2 number_extent = number_font->CalcTextSizeA(number_size, FLT_MAX, 0.0f, number);
  const float label_width =
      DrawSpacedText(nullptr, text_font, label_size, ImVec2(), 0, "FPS", label_spacing);
  const float time_width =
      time[0] ? text_font->CalcTextSizeA(time_size, FLT_MAX, 0.0f, time).x + gap : 0.0f;
  const ImVec2 panel_size(padding_x * 2.0f + number_extent.x + gap + label_width + time_width,
                          padding_y * 2.0f + number_extent.y);

  // The corner the Video options choose, inside the game image.
  const float margin = 16.0f * unit;
  const std::string corner = REXCVAR_GET(performance_overlay_position);
  const bool right = corner != "top_left" && corner != "bottom_left";
  const bool bottom = corner == "bottom_left" || corner == "bottom_right";
  const ImVec2 panel_min(right ? game_left + game_width - margin - panel_size.x : game_left + margin,
                         bottom ? game_top + game_height - margin - panel_size.y : game_top + margin);
  const ImVec2 panel_max(panel_min.x + panel_size.x, panel_min.y + panel_size.y);
  if (!right && !bottom) g_top_left_height = (panel_max.y - game_top) / unit;

  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  const float rounding = 8.0f * unit;
  draw_list->AddRectFilled(panel_min, panel_max, IM_COL32(16, 17, 20, 214), rounding);
  draw_list->AddRect(panel_min, panel_max, IM_COL32(255, 255, 255, 36), rounding, 0,
                     std::max(1.0f, unit));

  // Green at the frame rate the game aims for, amber a little under it, red well under it (25 and
  // 29 of 30, as the F6 overlay had for the console's 30).
  const double target = TargetFramesPerSecond();
  ImU32 color = IM_COL32(120, 222, 140, 255);
  if (shown_fps_ < target * 25.0 / 30.0) {
    color = IM_COL32(255, 96, 86, 255);
  } else if (shown_fps_ < target * 29.0 / 30.0) {
    color = IM_COL32(255, 176, 46, 255);
  }
  float x = panel_min.x + padding_x;
  const float top = panel_min.y + padding_y;
  draw_list->AddText(number_font, number_size, ImVec2(x, top), color, number);
  x += number_extent.x + gap;
  // Barlow's baseline sits at about 80% of the line height: line the small texts up with it.
  const float baseline = top + number_size * 0.80f;
  DrawSpacedText(draw_list, text_font, label_size, ImVec2(x, baseline - label_size * 0.80f),
                 IM_COL32(236, 236, 238, 255), "FPS", label_spacing);
  x += label_width + gap;
  if (time[0]) {
    draw_list->AddText(text_font, time_size, ImVec2(x, baseline - time_size * 0.80f),
                       IM_COL32(150, 152, 158, 255), time);
  }
}

}  // namespace azura

extern "C" void sub_82AF6B10(PPCContext& __restrict ctx, uint8_t* base) {
  if (azura::g_timing.load(std::memory_order_relaxed)) {
    azura::RecordFrame();
  }
  __imp__sub_82AF6B10(ctx, base);
}
