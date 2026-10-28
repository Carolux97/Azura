// A small notice in the top right corner of the game about the player's online room on Alboria
// (rex/system/xam/alboria.h), in the style of the shader compilation notice: a bold upper-case
// title and a line below, white with a soft shadow and no panel. Below the performance overlay
// when that one shows in the same corner.
//
// It shows while a room is being set up, and for a few seconds when an attempt fails or the room
// ends, with the reason. Once connected it draws nothing: the room's code shows, hidden until the
// player asks, in Manage/Invite (script_hooks.cpp).
//
// The app creates it at start-up when network_mode is online and it lives as long as the app. It
// draws on ImGui's foreground draw list, with no window, so it never takes input.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <rex/ui/imgui_dialog.h>

struct ImFont;

namespace azura {

// What the player reads, in the language `language` (an Xbox 360 language ID), for a failure of
// Alboria (rex::system::xam::alboria::Status::error).
const char* AlboriaErrorText(uint32_t language, const std::string& error);

class OnlineRoomOverlay : public rex::ui::ImGuiDialog {
 public:
  // `owner` is cleared when the dialog closes and deletes itself. `language` is the Xbox 360
  // language ID of the user_language setting. The fonts may be null (ImGui's default is used).
  OnlineRoomOverlay(rex::ui::ImGuiDrawer* drawer, uint32_t language, ImFont* title_font,
                    ImFont* body_font, OnlineRoomOverlay** owner);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  using Clock = std::chrono::steady_clock;

  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  OnlineRoomOverlay** owner_;
  // The error last shown and since when, so each failure shows for a while and then goes.
  std::string error_;
  Clock::time_point error_since_;
};

}  // namespace azura
