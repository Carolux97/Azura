// The performance overlay that the Video options turn on (Performance overlay, the
// performance_overlay setting): a small dark rounded panel in a corner of the game image (Overlay
// position, performance_overlay_position) with the game's frame rate in large figures, coloured by
// how close it is to the frame rate limit, and the time per frame. It uses the Barlow fonts of the
// shader compilation notice.
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

#include <rex/ui/imgui_dialog.h>

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
};

// How far down from the top of the game image the overlay's panel reaches when it shows in the top
// left corner, in pixels at 720p; 0 when it shows elsewhere or not at all. The shader compilation
// notice moves down by this much so the two do not overlap.
float PerfOverlayTopLeftHeight();

}  // namespace azura
