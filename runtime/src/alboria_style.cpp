#include "alboria_style.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

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

}  // namespace azura::alboria_style
