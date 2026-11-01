// Alboria's look for the screens of the service over the game (the Alboria overlay and its
// notices): the launcher's palette and the pieces both draw, the planet, the controller glyphs and
// the keyboard keys. The game's own menus keep the project's steel style (coop_ui.h).
#pragma once

#include <imgui.h>

#include <rex/input/pad_remap.h>

#include "coop_ui.h"

namespace azura::alboria_style {

// Alboria's palette (launcher/src/styles.css, from alboria-web).
constexpr ImU32 Navy(int alpha) { return IM_COL32(30, 42, 79, alpha); }
constexpr ImU32 NightSoft(int alpha) { return IM_COL32(74, 90, 133, alpha); }
constexpr ImU32 Sky(int alpha) { return IM_COL32(159, 211, 255, alpha); }
constexpr ImU32 SkyDeep(int alpha) { return IM_COL32(90, 169, 240, alpha); }
constexpr ImU32 SkyLine(int alpha) { return IM_COL32(184, 218, 248, alpha); }
constexpr ImU32 Paper(int alpha) { return IM_COL32(227, 242, 255, alpha); }
constexpr ImU32 PaperDeep(int alpha) { return IM_COL32(201, 229, 255, alpha); }
constexpr ImU32 Sun(int alpha) { return IM_COL32(255, 201, 60, alpha); }
constexpr ImU32 SunShadow(int alpha) { return IM_COL32(217, 162, 31, alpha); }
constexpr ImU32 Crimson(int alpha) { return IM_COL32(179, 32, 42, alpha); }
constexpr ImU32 White(int alpha) { return IM_COL32(255, 255, 255, alpha); }
constexpr ImU32 Online(int alpha) { return IM_COL32(110, 180, 80, alpha); }

// A controller's button in Alboria's colours: a navy disc for the face buttons, with the letters or
// the PlayStation shapes in white, and a navy key cap with the name for the others; centred on
// `center`, `height` tall. Returns its width (an alpha of 0 only measures it).
float AlboriaGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 center, float height, coop_ui::PadLook look,
                   rex::input::PadControl control, int alpha);

// Alboria's planet as the launcher draws it (its alboria-chip, a 40 x 40 drawing scaled by `scale`):
// a sun-light planet with a navy line, and a sky ring tilted by 18 degrees whose back half passes
// behind the planet and its front half in front. With `moon` at 0 or more, a small moon travels
// the ring at that angle (radians), hidden by the planet while it passes behind.
void DrawAlboriaPlanet(ImDrawList* draw_list, ImVec2 center, float scale, int alpha, float moon = -1.0f);

// Up and down, for moving through a list: a navy cap with two white arrows for a controller's
// D-pad, or a white key cap with a sky line and navy arrows for the keyboard's; centred on
// `center`, `height` tall. Returns its width (an alpha of 0 only measures it).
float AlboriaUpDownGlyph(ImDrawList* draw_list, ImVec2 center, float height, bool pad, int alpha);

// A keyboard key in Alboria's colours: a white cap with a sky line and its name in navy, its left
// edge at `left` and its middle at `middle`, `height` tall. Returns its width.
float AlboriaKey(ImDrawList* draw_list, ImFont* font, float left, float middle, float height, const char* name,
                 int alpha);

}  // namespace azura::alboria_style
