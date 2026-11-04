// Alboria's look for the screens of the service over the game (the Alboria overlay and its
// notices), as on its website (alboria-notes:alboria-web/design.md): pearl glass in a carved golden frame
// with an engraved groove and a leaf finial at each end, buttons that are stones of light (amber for
// the main action, mother-of-pearl for the others) with a seal holding a rune, and Alboria's ring
// rune and planet. The controller glyphs and keyboard keys sit in the seals. The game's own menus
// keep the project's steel style (coop_ui.h).
#pragma once

#include <functional>

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
// The world's palette (alboria-web, global.css): the ink, the carved gold and its light, the gold of
// the place names, and the lilac of a room's type.
constexpr ImU32 Ink(int alpha) { return IM_COL32(24, 34, 63, alpha); }
constexpr ImU32 Gold(int alpha) { return IM_COL32(216, 184, 102, alpha); }
constexpr ImU32 GoldLight(int alpha) { return IM_COL32(255, 243, 204, alpha); }
constexpr ImU32 GoldDeep(int alpha) { return IM_COL32(201, 152, 56, alpha); }
constexpr ImU32 GoldText(int alpha) { return IM_COL32(201, 138, 16, alpha); }
constexpr ImU32 Lilac(int alpha) { return IM_COL32(124, 92, 224, alpha); }

// The stone of a carved button: amber for the main action, mother-of-pearl for the others.
enum class Stone { kAmber, kPearl };

// What a button's seal holds: Alboria's ring rune, or, on the focused row, the control that does the
// action: a controller's button (`pad`) or a key (`key`).
struct SealContent {
  bool control = false;
  bool pad = false;
  coop_ui::PadLook look = coop_ui::PadLook::kGeneric;
  rex::input::PadControl control_id = rex::input::PadControl::kA;
  const char* key = nullptr;
};

// A carved panel: pearl glass (white to lilac to sky, top to bottom, `opacity` of it), a white
// edge, the golden frame with its groove and a leaf finial at the middle of each side, and a soft
// shadow under it. `rounding` is its corner radius. `inside`, when given, draws what lies in the
// glass under its edge and groove (a painted head), so those lines run unbroken over it.
void DrawCarvedPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float rounding, float unit, int alpha,
                     float opacity = 0.97f, const std::function<void()>& inside = {});

// A leaf finial of the golden frame, centred on `center`, `size` across, pointing left (`left`) or
// right.
void DrawLeafFinial(ImDrawList* draw_list, ImVec2 center, float size, bool left, int alpha);

// A carved button: the stone with its gloss, its golden frame and groove, a leaf finial at each end,
// and its seal on the left with `seal`'s content; the label in ink. Its right end at
// `right_center.x`, its middle at `right_center.y`; `size` is the label's size. Returns its width
// (an alpha of 0 only measures it); `hit` says whether the mouse clicked it this frame.
float DrawRuneButton(ImDrawList* draw_list, ImFont* font, float size, ImVec2 right_center, const char* label, Stone stone,
                     float unit, int alpha, bool* hit, const SealContent& seal = {});

// Alboria's ring rune: the logo drawn in outline, in the official logo's proportions (a ring 19 by 6
// tilted by 18 degrees round a planet of radius 12), the back half of the ring hidden where it
// passes behind the planet. `radius` is the ring's half width.
void DrawRingRune(ImDrawList* draw_list, ImVec2 center, float radius, ImU32 color, float thickness);

// A round seal of colour, light at the top and dark below, ringed in gold, with the ring rune in
// white: what a friend is doing, or a page's sigil.
void DrawStatusSeal(ImDrawList* draw_list, ImVec2 center, float radius, ImU32 light, ImU32 dark, int alpha);

// The logo's pearl medallion ringed in gold, with a soft sky glow; the planet is drawn on it apart.
void DrawMedallion(ImDrawList* draw_list, ImVec2 center, float radius, int alpha);

// Shades the vertices added to `draw_list` since `first_vertex` from `top` (at `y0`) to `bottom`
// (at `y1`), keeping their alpha: a vertical gradient over any shape.
void ShadeVertical(ImDrawList* draw_list, int first_vertex, float y0, float y1, ImU32 top, ImU32 bottom);

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

// Two arrows, up and down for moving through a list (`vertical`) or left and right for changing a
// value: a navy cap with white arrows for a controller's D-pad, or a white key cap with a sky line
// and navy arrows for the keyboard's; centred on `center`, `height` tall. Returns its width (an
// alpha of 0 only measures it).
float AlboriaArrowsGlyph(ImDrawList* draw_list, ImVec2 center, float height, bool pad, bool vertical, int alpha);

// A keyboard key in Alboria's colours: a white cap with a sky line and its name in navy, its left
// edge at `left` and its middle at `middle`, `height` tall. Returns its width.
float AlboriaKey(ImDrawList* draw_list, ImFont* font, float left, float middle, float height, const char* name,
                 int alpha);

}  // namespace azura::alboria_style
