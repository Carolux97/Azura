// The look of Azura's own screens over the game (the Local co-op menu and its notices, the
// performance overlay): the launcher's colours (launcher/src/styles.css), its panels with cut
// corners, and icons of the input devices drawn after the real ones. Everything draws on an ImGui
// draw list; sizes are in pixels.

#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#include <rex/input/device.h>

namespace azura::coop_ui {

// The launcher's colours (launcher/src/styles.css).
constexpr ImU32 Panel(int alpha) { return IM_COL32(20, 22, 25, alpha); }
constexpr ImU32 PanelSolid(int alpha) { return IM_COL32(22, 24, 27, alpha); }
constexpr ImU32 Steel(int alpha) { return IM_COL32(38, 42, 47, alpha); }
constexpr ImU32 SteelLine(int alpha) { return IM_COL32(54, 59, 66, alpha); }
constexpr ImU32 Imulsion(int alpha) { return IM_COL32(255, 184, 28, alpha); }
constexpr ImU32 Text(int alpha) { return IM_COL32(236, 230, 218, alpha); }
constexpr ImU32 TextDim(int alpha) { return IM_COL32(162, 156, 144, alpha); }
constexpr ImU32 TextFaint(int alpha) { return IM_COL32(109, 105, 96, alpha); }
// The launcher's state colours: --ok, and the text of its error badges (on --crimson).
constexpr ImU32 Ok(int alpha) { return IM_COL32(143, 191, 106, alpha); }
constexpr ImU32 ErrorText(int alpha) { return IM_COL32(255, 136, 136, alpha); }

// `text` cut with an ellipsis to fit `max_width`.
std::string Fit(ImFont* font, float size, const std::string& text, float max_width);

// A panel with the launcher's cut corners (top left and bottom right).
void DrawCutPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float cut, ImU32 fill, ImU32 line,
                  float thickness);

// A keyboard with a mouse beside it, `size` tall.
void DrawKeyboardIcon(ImDrawList* draw_list, ImVec2 center, float size, ImU32 body, ImU32 detail);

// The look of a controller family, as the real controllers: body colours and layout.
enum class PadLook { kDualSense, kDualShock4, kXbox, kSwitchPro, kGeneric };
PadLook LookOf(const rex::input::DeviceInfo& device);

// A controller, `size` tall, drawn after the real one of its family.
void DrawControllerIcon(ImDrawList* draw_list, ImVec2 center, float size, PadLook look, int alpha);

// A small left or right arrow, for the button hints.
void DrawArrow(ImDrawList* draw_list, ImVec2 center, float size, bool left, ImU32 color);

// The button that confirms, as `pad` shows it (the cross on PlayStation, A on the others), or the
// Enter key named `enter` when `pad` is null; its top left at `top_left`, 16 * `unit` tall. Returns
// its width.
float DrawConfirmGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 top_left, float unit,
                       const rex::input::DeviceInfo* pad, const char* enter, int alpha);

}  // namespace azura::coop_ui
