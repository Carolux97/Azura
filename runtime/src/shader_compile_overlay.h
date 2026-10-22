// A small notice in the top left corner of the game window while the GPU backend compiles shaders,
// in the style of the game's own checkpoint indicator: a turning cog, a bold upper-case title and a
// line with the progress below, white with a soft shadow and no panel.
//
// The Metal backend makes guest shaders and pipelines in the background: at start-up it
// precompiles the ones stored by earlier runs, and while playing it makes the ones a draw needs
// for the first time (skipping that draw until they are ready). The graphics system counts that
// work; this dialog shows it in the player's language while it lasts.
//
// The dialog exists only while there is work: the app creates it when work starts (the graphics
// system's callback) and it closes itself once nothing has been pending for a moment. It draws on
// ImGui's foreground draw list, with no window, so it never takes input. Work shorter than a
// fraction of a second (one pipeline in a new area) shows nothing.

#pragma once

#include <chrono>
#include <cstdint>

#include <rex/ui/imgui_dialog.h>

struct ImFont;

namespace rex::system {
class IGraphicsSystem;
}

namespace azura {

class ShaderCompileOverlay : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the dialog closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. The fonts may be null (ImGui's default is used).
  ShaderCompileOverlay(rex::ui::ImGuiDrawer* drawer,
                       rex::system::IGraphicsSystem* graphics_system, uint32_t language,
                       ImFont* title_font, ImFont* body_font, ShaderCompileOverlay** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  rex::system::IGraphicsSystem* graphics_system_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  ShaderCompileOverlay** owner_;
  // When the dialog was created (the cog's rotation), when the current work started, and when
  // nothing was pending any more.
  Clock::time_point created_;
  Clock::time_point busy_since_;
  Clock::time_point idle_since_;
  bool idle_ = false;
  bool shown_ = false;
};

}  // namespace azura
