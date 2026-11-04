#include "alboria_style.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>
#include <vector>

#include <imgui_internal.h>

namespace azura::alboria_style {

using coop_ui::AddTextCentered;
using coop_ui::ControlName;
using coop_ui::PadLook;
using rex::input::PadControl;

float AlboriaGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 center, float height, PadLook look, PadControl control,
                   int alpha) {
  const float r = height * 0.5f;
  const bool face = control == PadControl::kA || control == PadControl::kB || control == PadControl::kX ||
                    control == PadControl::kY;
  if (face) {
    draw_list->AddCircleFilled(center, r, Navy(alpha));
    const bool playstation = look == PadLook::kDualSense || look == PadLook::kDualShock4;
    const float s = r * 0.48f;
    const float t = height * 0.09f;
    if (playstation) {
      switch (control) {
        case PadControl::kA:
          draw_list->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), White(alpha), t);
          draw_list->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x + s, center.y - s), White(alpha), t);
          break;
        case PadControl::kB: draw_list->AddCircle(center, s, White(alpha), 0, t); break;
        case PadControl::kX:
          draw_list->AddRect(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), White(alpha), 0, 0, t);
          break;
        default:
          draw_list->AddTriangle(ImVec2(center.x, center.y - s * 1.1f), ImVec2(center.x + s * 1.05f, center.y + s * 0.75f),
                                 ImVec2(center.x - s * 1.05f, center.y + s * 0.75f), White(alpha), t);
          break;
      }
    } else {
      // Nintendo's letters sit swapped against Xbox's.
      const bool nintendo = look == PadLook::kSwitchPro;
      const char* letter = control == PadControl::kA ? (nintendo ? "B" : "A")
                           : control == PadControl::kB ? (nintendo ? "A" : "B")
                           : control == PadControl::kX ? (nintendo ? "Y" : "X")
                                                       : (nintendo ? "X" : "Y");
      AddTextCentered(draw_list, font, height * 0.72f, center, White(alpha), letter);
    }
    return height;
  }
  const char* name = ControlName(control, look);
  const float width = std::max(height * 1.6f, font->CalcTextSizeA(height * 0.62f, FLT_MAX, 0.0f, name).x + height * 0.6f);
  draw_list->AddRectFilled(ImVec2(center.x - width * 0.5f, center.y - r), ImVec2(center.x + width * 0.5f, center.y + r),
                           Navy(alpha), r * 0.6f);
  AddTextCentered(draw_list, font, height * 0.62f, center, White(alpha), name);
  return width;
}

void DrawAlboriaPlanet(ImDrawList* draw_list, ImVec2 center, float scale, int alpha, float moon) {
  constexpr float kTilt = -18.0f * 3.14159265f / 180.0f;
  const ImVec2 ring(19.0f * scale, 6.0f * scale);
  const float line = 3.0f * scale;
  // A point of the ring at angle `t` before it is tilted (the lower half, 0 to pi, is the front).
  const auto on_ring = [&](float t) {
    const float x = ring.x * std::cos(t);
    const float y = ring.y * std::sin(t);
    return ImVec2(center.x + x * std::cos(kTilt) - y * std::sin(kTilt), center.y + x * std::sin(kTilt) + y * std::cos(kTilt));
  };
  const float wrapped = moon >= 0.0f ? std::fmod(moon, 2.0f * 3.14159265f) : -1.0f;
  const bool moon_in_front = wrapped >= 0.0f && wrapped <= 3.14159265f;
  const float moon_radius = 2.6f * scale;
  draw_list->AddEllipse(center, ring, SkyDeep(alpha), kTilt, 48, line);
  if (wrapped >= 0.0f && !moon_in_front) draw_list->AddCircleFilled(on_ring(wrapped), moon_radius, Navy(alpha));
  draw_list->AddCircleFilled(center, 12.0f * scale, IM_COL32(255, 224, 122, alpha));
  draw_list->AddCircle(center, 12.0f * scale, Navy(alpha), 48, line);
  // The front half.
  constexpr int kSteps = 24;
  for (int i = 0; i <= kSteps; ++i) draw_list->PathLineTo(on_ring(3.14159265f * i / kSteps));
  draw_list->PathStroke(SkyDeep(alpha), 0, line);
  if (moon_in_front) draw_list->AddCircleFilled(on_ring(wrapped), moon_radius, Navy(alpha));
}

float AlboriaArrowsGlyph(ImDrawList* draw_list, ImVec2 center, float height, bool pad, bool vertical, int alpha) {
  const float width = height * 1.3f;
  const ImVec2 min(center.x - width * 0.5f, center.y - height * 0.5f);
  const ImVec2 max(center.x + width * 0.5f, center.y + height * 0.5f);
  if (pad) {
    draw_list->AddRectFilled(min, max, Navy(alpha), height * 0.3f);
  } else {
    draw_list->AddRectFilled(min, max, White(alpha), height * 0.28f);
    draw_list->AddRect(min, max, SkyDeep(alpha), height * 0.28f, 0, height * 0.08f);
  }
  const ImU32 arrow = pad ? White(alpha) : Navy(alpha);
  const float w = height * 0.2f;
  const float h = height * 0.17f;
  const float gap = height * 0.07f;
  if (vertical) {
    draw_list->AddTriangleFilled(ImVec2(center.x, center.y - gap - h * 2.0f), ImVec2(center.x + w, center.y - gap),
                                 ImVec2(center.x - w, center.y - gap), arrow);
    draw_list->AddTriangleFilled(ImVec2(center.x - w, center.y + gap), ImVec2(center.x + w, center.y + gap),
                                 ImVec2(center.x, center.y + gap + h * 2.0f), arrow);
  } else {
    draw_list->AddTriangleFilled(ImVec2(center.x - gap - h * 2.0f, center.y), ImVec2(center.x - gap, center.y - w),
                                 ImVec2(center.x - gap, center.y + w), arrow);
    draw_list->AddTriangleFilled(ImVec2(center.x + gap, center.y - w), ImVec2(center.x + gap + h * 2.0f, center.y),
                                 ImVec2(center.x + gap, center.y + w), arrow);
  }
  return width;
}

float AlboriaKey(ImDrawList* draw_list, ImFont* font, float left, float middle, float height, const char* name,
                 int alpha) {
  const float width = std::max(height * 1.4f, font->CalcTextSizeA(height * 0.6f, FLT_MAX, 0.0f, name).x + height * 0.7f);
  const ImVec2 min(left, middle - height * 0.5f);
  const ImVec2 max(left + width, middle + height * 0.5f);
  draw_list->AddRectFilled(min, max, White(alpha), height * 0.28f);
  draw_list->AddRect(min, max, SkyDeep(alpha), height * 0.28f, 0, height * 0.08f);
  AddTextCentered(draw_list, font, height * 0.6f, ImVec2((min.x + max.x) * 0.5f, middle), Navy(alpha), name);
  return width;
}


void ShadeVertical(ImDrawList* draw_list, int first_vertex, float y0, float y1, ImU32 top, ImU32 bottom) {
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw_list, first_vertex, draw_list->VtxBuffer.Size, ImVec2(0.0f, y0),
                                               ImVec2(0.0f, y1), top, bottom);
}

void DrawLeafFinial(ImDrawList* draw_list, ImVec2 center, float size, bool left, int alpha) {
  // A leaf lying across the frame's line: two arcs from tip to tip, its outer tip sharper.
  constexpr int kSteps = 12;
  const float half = size * 0.5f;
  const float width = size * 0.26f;
  const int first = draw_list->VtxBuffer.Size;
  draw_list->PathClear();
  for (int i = 0; i <= kSteps; ++i) {
    const float u = static_cast<float>(i) / kSteps;
    draw_list->PathLineTo(ImVec2(center.x - half + size * u, center.y - width * std::sin(3.14159265f * u)));
  }
  for (int i = kSteps - 1; i > 0; --i) {
    const float u = static_cast<float>(i) / kSteps;
    draw_list->PathLineTo(ImVec2(center.x - half + size * u, center.y + width * std::sin(3.14159265f * u)));
  }
  draw_list->PathFillConvex(GoldDeep(alpha));
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw_list, first, draw_list->VtxBuffer.Size,
                                               ImVec2(center.x + (left ? half : -half), center.y),
                                               ImVec2(center.x + (left ? -half : half), center.y), GoldLight(alpha),
                                               GoldDeep(alpha));
}

void DrawCarvedPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float rounding, float unit, int alpha,
                     float opacity, const std::function<void()>& inside) {
  const auto out = [&](float by) { return std::pair(ImVec2(min.x - by, min.y - by), ImVec2(max.x + by, max.y + by)); };
  // A soft shadow below and a faint golden glow around: many thin layers, each a little wider and
  // fainter, so neither has an edge.
  constexpr int kLayers = 14;
  for (int i = kLayers; i >= 1; --i) {
    const float spread = 2.2f * unit * i;
    const int shade = alpha * (kLayers + 1 - i) / (kLayers * 9);
    draw_list->AddRectFilled(ImVec2(min.x - spread * 0.6f, min.y + 4.0f * unit), ImVec2(max.x + spread * 0.6f, max.y + spread),
                             IM_COL32(4, 8, 20, shade), rounding + spread);
  }
  for (int i = 6; i >= 1; --i) {
    const auto [glow_min, glow_max] = out(4.5f * unit + 1.6f * unit * i);
    draw_list->AddRect(glow_min, glow_max, IM_COL32(255, 214, 120, alpha * (7 - i) / 90), rounding + 4.5f * unit + 1.6f * unit * i,
                       0, 1.6f * unit);
  }
  // The golden frame: a dark edge, the gold, then its light, then the glass.
  {
    const auto [a_min, a_max] = out(5.5f * unit);
    draw_list->AddRectFilled(a_min, a_max, IM_COL32(80, 50, 10, alpha * 60 / 255), rounding + 5.5f * unit);
    const auto [b_min, b_max] = out(4.5f * unit);
    draw_list->AddRectFilled(b_min, b_max, Gold(alpha), rounding + 4.5f * unit);
    const auto [c_min, c_max] = out(2.0f * unit);
    draw_list->AddRectFilled(c_min, c_max, GoldLight(alpha), rounding + 2.0f * unit);
  }
  const int glass_alpha = static_cast<int>(alpha * opacity);
  const int first = draw_list->VtxBuffer.Size;
  draw_list->AddRectFilled(min, max, White(glass_alpha), rounding);
  ShadeVertical(draw_list, first, min.y, max.y, IM_COL32(255, 255, 255, 255), IM_COL32(236, 241, 252, 255));
  if (inside) inside();
  // The white edge, and the engraved groove inside the frame, over everything in the glass.
  draw_list->AddRect(ImVec2(min.x + 1.0f * unit, min.y + 1.0f * unit), ImVec2(max.x - 1.0f * unit, max.y - 1.0f * unit),
                     White(alpha * 200 / 255), rounding, 0, 1.5f * unit);
  draw_list->AddRect(ImVec2(min.x + 7.0f * unit, min.y + 7.0f * unit), ImVec2(max.x - 7.0f * unit, max.y - 7.0f * unit),
                     Gold(alpha * 150 / 255), std::max(0.0f, rounding - 7.0f * unit), 0, 1.0f * unit);
  const float middle = (min.y + max.y) * 0.5f;
  DrawLeafFinial(draw_list, ImVec2(min.x - 9.0f * unit, middle), 20.0f * unit, true, alpha);
  DrawLeafFinial(draw_list, ImVec2(max.x + 9.0f * unit, middle), 20.0f * unit, false, alpha);
}

void DrawRingRune(ImDrawList* draw_list, ImVec2 center, float radius, ImU32 color, float thickness) {
  // In the official logo's proportions (its 40 x 40 drawing): a ring 19 by 6 tilted by -18 degrees
  // round a planet of radius 12.
  constexpr float kTilt = -18.0f * 3.14159265f / 180.0f;
  const float rx = radius;
  const float ry = radius * 6.0f / 19.0f;
  const float planet = radius * 12.0f / 19.0f;
  const auto at = [&](float t) {
    const float x = rx * std::cos(t);
    const float y = ry * std::sin(t);
    return ImVec2(center.x + x * std::cos(kTilt) - y * std::sin(kTilt), center.y + x * std::sin(kTilt) + y * std::cos(kTilt));
  };
  // The back half (the upper half before the tilt), left out only inside the planet's outline: its
  // ends meet the outline's stroke, so the ring reads as passing behind the planet. Finely sampled,
  // so the cut falls on the outline itself.
  constexpr int kBackSteps = 96;
  bool drawing = false;
  for (int i = 0; i <= kBackSteps; ++i) {
    const ImVec2 p = at(3.14159265f + 3.14159265f * i / kBackSteps);
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    const bool hidden = dx * dx + dy * dy < planet * planet;
    if (hidden) {
      if (drawing) draw_list->PathStroke(color, 0, thickness);
      drawing = false;
      continue;
    }
    draw_list->PathLineTo(p);
    drawing = true;
  }
  if (drawing) draw_list->PathStroke(color, 0, thickness);
  draw_list->AddCircle(center, planet, color, 0, thickness);
  constexpr int kSteps = 48;
  for (int i = 0; i <= kSteps; ++i) draw_list->PathLineTo(at(3.14159265f * i / kSteps));
  draw_list->PathStroke(color, 0, thickness);
}

void DrawStatusSeal(ImDrawList* draw_list, ImVec2 center, float radius, ImU32 light, ImU32 dark, int alpha) {
  draw_list->AddCircleFilled(center, radius * 1.16f, Gold(alpha), 40);
  draw_list->AddCircleFilled(center, radius * 1.08f, GoldLight(alpha), 40);
  const int first = draw_list->VtxBuffer.Size;
  draw_list->AddCircleFilled(center, radius, light, 40);
  ShadeVertical(draw_list, first, center.y - radius, center.y + radius, light, dark);
  DrawRingRune(draw_list, center, radius * 0.62f, White(alpha), std::max(1.0f, radius * 0.11f));
}

void DrawMedallion(ImDrawList* draw_list, ImVec2 center, float radius, int alpha) {
  draw_list->AddCircleFilled(center, radius * 1.3f, IM_COL32(140, 200, 255, alpha * 50 / 255), 48);
  draw_list->AddCircleFilled(center, radius * 1.1f, Gold(alpha), 48);
  draw_list->AddCircleFilled(center, radius * 1.05f, GoldLight(alpha), 48);
  const int first = draw_list->VtxBuffer.Size;
  draw_list->AddCircleFilled(center, radius, White(alpha), 48);
  ShadeVertical(draw_list, first, center.y - radius, center.y + radius, IM_COL32(255, 255, 255, 255),
                IM_COL32(207, 227, 255, 255));
}

float DrawRuneButton(ImDrawList* draw_list, ImFont* font, float size, ImVec2 right_center, const char* label, Stone stone,
                     float unit, int alpha, bool* hit, const SealContent& seal) {
  const bool amber = stone == Stone::kAmber;
  const float height = size + 18.0f * unit;
  const float seal_height = height - 8.0f * unit;
  // What the seal holds sets its width: a round seal, or a longer one for a key's name.
  const float content_height = seal_height * 0.74f;
  float content_width = 0.0f;
  if (seal.control) {
    content_width = seal.pad ? AlboriaGlyph(draw_list, font, ImVec2(0, 0), content_height, seal.look, seal.control_id, 0)
                             : AlboriaKey(draw_list, font, 0.0f, 0.0f, content_height, seal.key, 0);
  }
  const float seal_width = std::max(seal_height, content_width + 8.0f * unit);
  const float text_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x;
  const float width = 4.0f * unit + seal_width + 10.0f * unit + text_width + 20.0f * unit;
  const ImVec2 min(right_center.x - width, right_center.y - height * 0.5f);
  const ImVec2 max(right_center.x, right_center.y + height * 0.5f);
  if (alpha <= 0) return width;
  const float round = height * 0.5f;
  const ImU32 frame = amber ? IM_COL32(201, 152, 56, alpha) : Gold(alpha);
  const ImU32 frame_light = amber ? IM_COL32(255, 233, 168, alpha) : GoldLight(alpha);
  // The halo under it, its frame, then the stone with its gloss and groove.
  draw_list->AddRectFilled(ImVec2(min.x, min.y + 8.0f * unit), ImVec2(max.x, max.y + 6.0f * unit),
                           amber ? IM_COL32(255, 196, 70, alpha * 60 / 255) : IM_COL32(140, 180, 255, alpha * 50 / 255),
                           round + 4.0f * unit);
  DrawLeafFinial(draw_list, ImVec2(min.x - 6.0f * unit, right_center.y), 12.0f * unit, true, alpha);
  DrawLeafFinial(draw_list, ImVec2(max.x + 6.0f * unit, right_center.y), 12.0f * unit, false, alpha);
  draw_list->AddRectFilled(ImVec2(min.x - 4.5f * unit, min.y - 4.5f * unit), ImVec2(max.x + 4.5f * unit, max.y + 4.5f * unit),
                           IM_COL32(80, 50, 10, alpha * 64 / 255), round + 4.5f * unit);
  draw_list->AddRectFilled(ImVec2(min.x - 4.0f * unit, min.y - 4.0f * unit), ImVec2(max.x + 4.0f * unit, max.y + 4.0f * unit),
                           frame, round + 4.0f * unit);
  draw_list->AddRectFilled(ImVec2(min.x - 2.0f * unit, min.y - 2.0f * unit), ImVec2(max.x + 2.0f * unit, max.y + 2.0f * unit),
                           frame_light, round + 2.0f * unit);
  const int first = draw_list->VtxBuffer.Size;
  draw_list->AddRectFilled(min, max, White(alpha), round);
  ShadeVertical(draw_list, first, min.y, max.y, amber ? IM_COL32(255, 243, 196, 255) : IM_COL32(255, 255, 255, 255),
                amber ? IM_COL32(244, 179, 58, 255) : IM_COL32(230, 238, 250, 255));
  draw_list->AddLine(ImVec2(min.x + round, min.y + 1.5f * unit), ImVec2(max.x - round, min.y + 1.5f * unit),
                     White(alpha * 220 / 255), 1.5f * unit);
  draw_list->AddRect(ImVec2(min.x + 3.0f * unit, min.y + 3.0f * unit), ImVec2(max.x - 3.0f * unit, max.y - 3.0f * unit),
                     amber ? White(alpha * 140 / 255) : Gold(alpha * 140 / 255), round - 3.0f * unit, 0, 1.0f * unit);
  // The seal, ringed in the frame's light, and what it holds.
  const ImVec2 seal_center(min.x + 4.0f * unit + seal_width * 0.5f, right_center.y);
  const ImVec2 seal_min(seal_center.x - seal_width * 0.5f, seal_center.y - seal_height * 0.5f);
  const ImVec2 seal_max(seal_center.x + seal_width * 0.5f, seal_center.y + seal_height * 0.5f);
  draw_list->AddRectFilled(ImVec2(seal_min.x - 1.5f * unit, seal_min.y - 1.5f * unit),
                           ImVec2(seal_max.x + 1.5f * unit, seal_max.y + 1.5f * unit), frame_light, seal_height * 0.5f + 1.5f * unit);
  const ImU32 seal_light = amber ? IM_COL32(138, 90, 18, alpha) : IM_COL32(47, 94, 168, alpha);
  const ImU32 seal_dark = amber ? IM_COL32(90, 58, 10, alpha) : IM_COL32(27, 47, 99, alpha);
  const int seal_first = draw_list->VtxBuffer.Size;
  draw_list->AddRectFilled(seal_min, seal_max, seal_light, seal_height * 0.5f);
  ShadeVertical(draw_list, seal_first, seal_min.y, seal_max.y, seal_light, seal_dark);
  if (seal.control && seal.pad) {
    AlboriaGlyph(draw_list, font, seal_center, content_height, seal.look, seal.control_id, alpha);
  } else if (seal.control) {
    AlboriaKey(draw_list, font, seal_center.x - content_width * 0.5f, seal_center.y, content_height, seal.key, alpha);
  } else {
    DrawRingRune(draw_list, seal_center, seal_height * 0.36f,
                 amber ? IM_COL32(255, 244, 194, alpha) : IM_COL32(217, 241, 255, alpha), std::max(1.0f, 1.6f * unit));
  }
  draw_list->AddText(font, size, ImVec2(seal_max.x + 10.0f * unit, right_center.y - size * 0.55f),
                     amber ? IM_COL32(43, 29, 4, alpha) : Ink(alpha), label);
  if (hit) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    *hit = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y &&
           mouse.y <= max.y;
  }
  return width;
}

}  // namespace azura::alboria_style
