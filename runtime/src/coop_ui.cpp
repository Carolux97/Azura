#include "coop_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>

namespace azura::coop_ui {

namespace {

// A filled capsule from `a` to `b`, `radius` wide on each side.
void Capsule(ImDrawList* draw_list, ImVec2 a, ImVec2 b, float radius, ImU32 color) {
  draw_list->AddLine(a, b, color, radius * 2.0f);
  draw_list->AddCircleFilled(a, radius, color, 24);
  draw_list->AddCircleFilled(b, radius, color, 24);
}

// The outline of a controller: a body across the top and two grips angled down and out, `grow`
// pixels larger on every side (for the rim).
void PadShape(ImDrawList* draw_list, ImVec2 c, float w, float h, float grow, ImU32 color) {
  for (float side : {-1.0f, 1.0f}) {
    Capsule(draw_list, ImVec2(c.x + side * w * 0.30f, c.y), ImVec2(c.x + side * w * 0.38f, c.y + h * 0.34f),
            h * 0.20f + grow, color);
  }
  draw_list->AddRectFilled(ImVec2(c.x - w * 0.40f - grow, c.y - h * 0.36f - grow),
                           ImVec2(c.x + w * 0.40f + grow, c.y + h * 0.20f + grow), color, h * 0.22f + grow);
}

// A thumbstick: a dark cap with a lighter ring.
void Stick(ImDrawList* draw_list, ImVec2 at, float h, ImU32 ring, ImU32 cap) {
  draw_list->AddCircleFilled(at, h * 0.105f, ring, 20);
  draw_list->AddCircleFilled(at, h * 0.072f, cap, 20);
}

// A D-pad cross.
void Dpad(ImDrawList* draw_list, ImVec2 at, float h, ImU32 color) {
  const float arm = h * 0.095f, half = h * 0.032f;
  draw_list->AddRectFilled(ImVec2(at.x - arm, at.y - half), ImVec2(at.x + arm, at.y + half), color);
  draw_list->AddRectFilled(ImVec2(at.x - half, at.y - arm), ImVec2(at.x + half, at.y + arm), color);
}

// Four face buttons in a diamond: top, right, bottom, left.
void FaceButtons(ImDrawList* draw_list, ImVec2 at, float h, const ImU32 (&colors)[4]) {
  const float d = h * 0.08f, r = h * 0.04f;
  const ImVec2 places[4] = {{at.x, at.y - d}, {at.x + d, at.y}, {at.x, at.y + d}, {at.x - d, at.y}};
  for (int i = 0; i < 4; ++i) draw_list->AddCircleFilled(places[i], r, colors[i], 12);
}

}  // namespace

// `text` cut with an ellipsis to fit `max_width`.
std::string Fit(ImFont* font, float size, const std::string& text, float max_width) {
  if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
    return text;
  }
  std::string cut = text;
  while (!cut.empty()) {
    cut.pop_back();
    // Not in the middle of a UTF-8 sequence.
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) {
      cut.pop_back();
    }
    if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0) {
      cut.pop_back();
    }
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, (cut + "...").c_str()).x <= max_width) {
      return cut + "...";
    }
  }
  return "...";
}

// A panel with the launcher's cut corners (top left and bottom right).
void DrawCutPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float cut, ImU32 fill, ImU32 line,
                  float thickness) {
  const ImVec2 points[6] = {{min.x + cut, min.y}, {max.x, min.y}, {max.x, max.y - cut},
                            {max.x - cut, max.y}, {min.x, max.y},  {min.x, min.y + cut}};
  draw_list->AddConvexPolyFilled(points, 6, fill);
  if (thickness > 0.0f) {
    draw_list->AddPolyline(points, 6, line, ImDrawFlags_Closed, thickness);
  }
}

// A keyboard with a mouse beside it, `size` tall.
void DrawKeyboardIcon(ImDrawList* draw_list, ImVec2 center, float size, ImU32 body, ImU32 detail) {
  const float width = size * 1.35f;
  const float height = size * 0.62f;
  const ImVec2 min(center.x - width * 0.62f, center.y - height * 0.5f);
  const ImVec2 max(min.x + width, min.y + height);
  draw_list->AddRectFilled(min, max, body, size * 0.08f);
  // Three rows of keys and a space bar.
  const float key = width / 9.0f;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 7; ++col) {
      const ImVec2 key_min(min.x + key * (0.9f + col * 1.05f), min.y + height * (0.14f + row * 0.21f));
      draw_list->AddRectFilled(key_min, ImVec2(key_min.x + key * 0.8f, key_min.y + height * 0.14f), detail,
                               size * 0.02f);
    }
  }
  draw_list->AddRectFilled(ImVec2(min.x + key * 2.5f, min.y + height * 0.78f),
                           ImVec2(max.x - key * 2.5f, min.y + height * 0.9f), detail, size * 0.02f);
  // The mouse.
  const ImVec2 mouse_min(max.x + size * 0.12f, center.y - size * 0.3f);
  const ImVec2 mouse_max(mouse_min.x + size * 0.34f, center.y + size * 0.3f);
  draw_list->AddRectFilled(mouse_min, mouse_max, body, size * 0.17f);
  draw_list->AddLine(ImVec2((mouse_min.x + mouse_max.x) * 0.5f, mouse_min.y + size * 0.04f),
                     ImVec2((mouse_min.x + mouse_max.x) * 0.5f, mouse_min.y + size * 0.22f), detail,
                     size * 0.04f);
}

PadLook LookOf(const rex::input::DeviceInfo& device) {
  std::string name = device.name;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  switch (device.controller_subtype) {
    case rex::input::ControllerSubtype::kPlayStation:
      return name.find("dualsense") != std::string::npos ? PadLook::kDualSense : PadLook::kDualShock4;
    case rex::input::ControllerSubtype::kXbox:
      return PadLook::kXbox;
    case rex::input::ControllerSubtype::kNintendo:
      return PadLook::kSwitchPro;
    default:
      return PadLook::kGeneric;
  }
}

// A controller, `size` tall, drawn after the real one of its family: the DualSense white with its
// black middle, touchpad and blue light bar; the DualShock 4 dark with the light bar on top; Xbox
// and Switch Pro black with offset sticks (Xbox with its coloured A B X Y and guide button); others
// in a neutral grey.
void DrawControllerIcon(ImDrawList* draw_list, ImVec2 center, float size, PadLook look, int alpha) {
  const float w = size * 1.7f, h = size;
  auto col = [&](int r, int g, int b) { return IM_COL32(r, g, b, alpha); };
  const ImU32 rim = col(150, 150, 156);
  switch (look) {
    case PadLook::kDualSense:
    case PadLook::kDualShock4: {
      const bool dualsense = look == PadLook::kDualSense;
      PadShape(draw_list, center, w, h, h * 0.035f, dualsense ? col(70, 72, 78) : rim);
      PadShape(draw_list, center, w, h, 0.0f, dualsense ? col(236, 236, 238) : col(46, 48, 54));
      // The black middle between the grips, with the touchpad on top.
      draw_list->AddRectFilled(ImVec2(center.x - w * 0.19f, center.y - h * 0.31f),
                               ImVec2(center.x + w * 0.19f, center.y + h * 0.20f), col(26, 26, 30), h * 0.12f);
      const ImVec2 pad_min(center.x - w * 0.15f, center.y - h * 0.33f);
      const ImVec2 pad_max(center.x + w * 0.15f, center.y - h * 0.09f);
      draw_list->AddRectFilled(pad_min, pad_max, dualsense ? col(214, 214, 218) : col(62, 64, 70), h * 0.04f);
      const ImU32 light = col(40, 130, 240);
      if (dualsense) {
        // The light bar runs down both sides of the touchpad.
        draw_list->AddLine(ImVec2(pad_min.x - h * 0.03f, pad_min.y + h * 0.03f),
                           ImVec2(pad_min.x - h * 0.03f, pad_max.y), light, h * 0.03f);
        draw_list->AddLine(ImVec2(pad_max.x + h * 0.03f, pad_min.y + h * 0.03f),
                           ImVec2(pad_max.x + h * 0.03f, pad_max.y), light, h * 0.03f);
      } else {
        draw_list->AddLine(ImVec2(pad_min.x + h * 0.04f, pad_min.y - h * 0.015f),
                           ImVec2(pad_max.x - h * 0.04f, pad_min.y - h * 0.015f), light, h * 0.03f);
      }
      // Sticks side by side below the touchpad; D-pad and face buttons on the wings.
      Stick(draw_list, ImVec2(center.x - w * 0.11f, center.y + h * 0.06f), h, col(90, 92, 98), col(20, 20, 24));
      Stick(draw_list, ImVec2(center.x + w * 0.11f, center.y + h * 0.06f), h, col(90, 92, 98), col(20, 20, 24));
      const ImU32 marks = dualsense ? col(60, 62, 68) : col(150, 152, 158);
      Dpad(draw_list, ImVec2(center.x - w * 0.28f, center.y - h * 0.12f), h, marks);
      const ImU32 face[4] = {marks, marks, marks, marks};
      FaceButtons(draw_list, ImVec2(center.x + w * 0.28f, center.y - h * 0.12f), h, face);
      break;
    }
    case PadLook::kXbox:
    case PadLook::kSwitchPro:
    case PadLook::kGeneric: {
      const bool generic = look == PadLook::kGeneric;
      PadShape(draw_list, center, w, h, h * 0.035f, generic ? col(70, 72, 78) : rim);
      PadShape(draw_list, center, w, h, 0.0f, generic ? col(176, 178, 184) : col(40, 42, 47));
      // Offset sticks: the left one high, the right one low, the D-pad below the left stick.
      const ImU32 ring = generic ? col(120, 122, 128) : col(96, 98, 104);
      Stick(draw_list, ImVec2(center.x - w * 0.26f, center.y - h * 0.12f), h, ring, col(18, 18, 22));
      Stick(draw_list, ImVec2(center.x + w * 0.12f, center.y + h * 0.06f), h, ring, col(18, 18, 22));
      Dpad(draw_list, ImVec2(center.x - w * 0.12f, center.y + h * 0.06f), h, generic ? col(90, 92, 98) : col(120, 122, 128));
      if (look == PadLook::kXbox) {
        // Y yellow, B red, A green, X blue; the guide button with its green glow.
        const ImU32 face[4] = {col(240, 196, 24), col(222, 48, 44), col(30, 170, 60), col(40, 116, 230)};
        FaceButtons(draw_list, ImVec2(center.x + w * 0.26f, center.y - h * 0.12f), h, face);
        draw_list->AddCircleFilled(ImVec2(center.x, center.y - h * 0.24f), h * 0.07f, col(16, 124, 16), 16);
        draw_list->AddCircleFilled(ImVec2(center.x, center.y - h * 0.24f), h * 0.042f, col(220, 222, 226), 16);
      } else {
        const ImU32 grey = generic ? col(90, 92, 98) : col(150, 152, 158);
        const ImU32 face[4] = {grey, grey, grey, grey};
        FaceButtons(draw_list, ImVec2(center.x + w * 0.26f, center.y - h * 0.12f), h, face);
        if (look == PadLook::kSwitchPro) {
          // The Home button, and the minus and plus beside it.
          draw_list->AddCircleFilled(ImVec2(center.x, center.y - h * 0.28f), h * 0.04f, col(150, 152, 158), 12);
          draw_list->AddRectFilled(ImVec2(center.x - w * 0.10f, center.y - h * 0.29f),
                                   ImVec2(center.x - w * 0.06f, center.y - h * 0.27f), col(150, 152, 158));
          draw_list->AddRectFilled(ImVec2(center.x + w * 0.06f, center.y - h * 0.29f),
                                   ImVec2(center.x + w * 0.10f, center.y - h * 0.27f), col(150, 152, 158));
        }
      }
      break;
    }
  }
}

// A small left or right arrow, for the button hints.
void DrawArrow(ImDrawList* draw_list, ImVec2 center, float size, bool left, ImU32 color) {
  const float s = left ? -1.0f : 1.0f;
  draw_list->AddTriangleFilled(ImVec2(center.x + s * size * 0.5f, center.y),
                               ImVec2(center.x - s * size * 0.4f, center.y - size * 0.5f),
                               ImVec2(center.x - s * size * 0.4f, center.y + size * 0.5f), color);
}

float DrawConfirmGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 top_left, float unit,
                       const rex::input::DeviceInfo* pad, const char* enter, int alpha) {
  if (!pad) {
    // The Enter key: a key cap with its name.
    const float key_size = 11.0f * unit;
    const ImVec2 label = font->CalcTextSizeA(key_size, FLT_MAX, 0.0f, enter);
    const ImVec2 key_max(top_left.x + label.x + 10.0f * unit, top_left.y + 16.0f * unit);
    draw_list->AddRectFilled(top_left, key_max, Imulsion(alpha), 3.0f * unit);
    draw_list->AddText(font, key_size, ImVec2(top_left.x + 5.0f * unit, (top_left.y + key_max.y - label.y) * 0.5f),
                       Panel(alpha), enter);
    return key_max.x - top_left.x;
  }
  const ImVec2 center(top_left.x + 8.0f * unit, top_left.y + 8.0f * unit);
  draw_list->AddCircleFilled(center, 8.0f * unit, Imulsion(alpha), 24);
  const PadLook look = LookOf(*pad);
  if (look == PadLook::kDualSense || look == PadLook::kDualShock4) {
    // AddLine moves its ends half a pixel down and right (to the pixel centres of thin lines) and
    // AddCircleFilled does not: the cross is moved back so it sits in the middle of the circle.
    const float arm = 3.6f * unit;
    const ImVec2 c(center.x - 0.5f, center.y - 0.5f);
    draw_list->AddLine(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), Panel(alpha), 1.8f * unit);
    draw_list->AddLine(ImVec2(c.x - arm, c.y + arm), ImVec2(c.x + arm, c.y - arm), Panel(alpha), 1.8f * unit);
  } else {
    const float letter_size = 11.0f * unit;
    const ImVec2 letter = font->CalcTextSizeA(letter_size, FLT_MAX, 0.0f, "A");
    draw_list->AddText(font, letter_size, ImVec2(center.x - letter.x * 0.5f, center.y - letter.y * 0.5f), Panel(alpha),
                       "A");
  }
  return 16.0f * unit;
}

}  // namespace azura::coop_ui
