#include "coop_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>

#include <imgui_internal.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>

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

// The two small menu buttons (Back and Start), `dx` either side of the middle at height `y`: upright
// pills on a PlayStation controller, rounded rectangles on the others.
void MenuButtons(ImDrawList* draw_list, ImVec2 c, float h, float dx, float y, ImU32 color, bool upright) {
  const ImVec2 half = upright ? ImVec2(h * 0.018f, h * 0.035f) : ImVec2(h * 0.035f, h * 0.022f);
  for (float side : {-1.0f, 1.0f}) {
    const ImVec2 at(c.x + side * dx, y);
    draw_list->AddRectFilled(ImVec2(at.x - half.x, at.y - half.y), ImVec2(at.x + half.x, at.y + half.y), color,
                             std::min(half.x, half.y));
  }
}

// Four face buttons in a diamond: top, right, bottom, left.
void FaceButtons(ImDrawList* draw_list, ImVec2 at, float h, const ImU32 (&colors)[4]) {
  const float d = h * 0.08f, r = h * 0.04f;
  const ImVec2 places[4] = {{at.x, at.y - d}, {at.x + d, at.y}, {at.x, at.y + d}, {at.x - d, at.y}};
  for (int i = 0; i < 4; ++i) draw_list->AddCircleFilled(places[i], r, colors[i], 12);
}

}  // namespace

void AddTextCentered(ImDrawList* draw_list, ImFont* font, float size, ImVec2 center, ImU32 color, const char* text) {
  // The ink's bounds from the point AddText draws at, glyph by glyph, at the size it is drawn at.
  ImFontBaked* baked = font->GetFontBaked(size);
  const float scale = baked && baked->Size > 0.0f ? size / baked->Size : 1.0f;
  float pen = 0.0f;
  ImVec2 ink_min(FLT_MAX, FLT_MAX), ink_max(-FLT_MAX, -FLT_MAX);
  for (const char* s = text; baked && *s;) {
    unsigned int c = 0;
    s += ImTextCharFromUtf8(&c, s, nullptr);
    const ImFontGlyph* glyph = baked->FindGlyph(static_cast<ImWchar>(c));
    if (!glyph) continue;
    if (glyph->Visible) {
      ink_min = ImVec2(std::min(ink_min.x, pen + glyph->X0 * scale), std::min(ink_min.y, glyph->Y0 * scale));
      ink_max = ImVec2(std::max(ink_max.x, pen + glyph->X1 * scale), std::max(ink_max.y, glyph->Y1 * scale));
    }
    pen += glyph->AdvanceX * scale;
  }
  if (ink_min.x > ink_max.x) {
    // Nothing visible (or no font data): the line box instead.
    const ImVec2 box = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    ink_min = ImVec2(0.0f, 0.0f);
    ink_max = box;
  }
  const ImVec2 at(center.x - (ink_min.x + ink_max.x) * 0.5f, center.y - (ink_min.y + ink_max.y) * 0.5f);
  // Whole pixels, so thin strokes stay sharp.
  draw_list->AddText(font, size, ImVec2(std::round(at.x), std::round(at.y)), color, text);
}

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
      // The black middle below the touchpad, around the sticks and the PS button.
      draw_list->AddRectFilled(ImVec2(center.x - w * 0.19f, center.y - h * 0.14f),
                               ImVec2(center.x + w * 0.19f, center.y + h * 0.20f), col(26, 26, 30), h * 0.10f);
      // The touchpad across the top, with its light inside its edges: down both sides on the
      // DualSense, along the top on the DualShock 4.
      const ImVec2 pad_min(center.x - w * 0.15f, center.y - h * 0.33f);
      const ImVec2 pad_max(center.x + w * 0.15f, center.y - h * 0.08f);
      draw_list->AddRectFilled(pad_min, pad_max, dualsense ? col(214, 214, 218) : col(62, 64, 70), h * 0.04f);
      const ImU32 light = col(40, 130, 240);
      const float light_width = h * 0.018f;
      if (dualsense) {
        for (const float x : {pad_min.x + h * 0.022f, pad_max.x - h * 0.022f}) {
          draw_list->AddLine(ImVec2(x, pad_min.y + h * 0.06f), ImVec2(x, pad_max.y - h * 0.03f), light, light_width);
        }
      } else {
        draw_list->AddLine(ImVec2(pad_min.x + h * 0.06f, pad_min.y + h * 0.022f),
                           ImVec2(pad_max.x - h * 0.06f, pad_min.y + h * 0.022f), light, light_width);
      }
      // Sticks side by side below the touchpad, the PS button between them; D-pad and face buttons
      // on the wings, Create (Share) and Options at the touchpad's top corners.
      Stick(draw_list, ImVec2(center.x - w * 0.11f, center.y + h * 0.06f), h, col(90, 92, 98), col(20, 20, 24));
      Stick(draw_list, ImVec2(center.x + w * 0.11f, center.y + h * 0.06f), h, col(90, 92, 98), col(20, 20, 24));
      draw_list->AddCircleFilled(ImVec2(center.x, center.y + h * 0.14f), h * 0.028f, col(90, 92, 98), 12);
      const ImU32 marks = dualsense ? col(60, 62, 68) : col(150, 152, 158);
      Dpad(draw_list, ImVec2(center.x - w * 0.28f, center.y - h * 0.12f), h, marks);
      const ImVec2 start = PadPointPosition(center, size, look, PadPoint::kStart);
      MenuButtons(draw_list, center, h, start.x - center.x, start.y, marks, true);
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
      if (look != PadLook::kSwitchPro) {
        // View and Menu (Back and Start), below the guide button's place.
        const ImVec2 start = PadPointPosition(center, size, look, PadPoint::kStart);
        MenuButtons(draw_list, center, h, start.x - center.x, start.y, generic ? col(90, 92, 98) : col(120, 122, 128),
                    false);
      }
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

ImVec2 PadPointPosition(ImVec2 center, float size, PadLook look, PadPoint point) {
  // The same proportions as DrawControllerIcon and its helpers (Stick, Dpad, FaceButtons, PadShape).
  const float w = size * 1.7f, h = size;
  const bool playstation = look == PadLook::kDualSense || look == PadLook::kDualShock4;
  const ImVec2 left_stick = playstation ? ImVec2(center.x - w * 0.11f, center.y + h * 0.06f)
                                        : ImVec2(center.x - w * 0.26f, center.y - h * 0.12f);
  const ImVec2 right_stick = playstation ? ImVec2(center.x + w * 0.11f, center.y + h * 0.06f)
                                         : ImVec2(center.x + w * 0.12f, center.y + h * 0.06f);
  const ImVec2 dpad = playstation ? ImVec2(center.x - w * 0.28f, center.y - h * 0.12f)
                                  : ImVec2(center.x - w * 0.12f, center.y + h * 0.06f);
  const ImVec2 face = playstation ? ImVec2(center.x + w * 0.28f, center.y - h * 0.12f)
                                  : ImVec2(center.x + w * 0.26f, center.y - h * 0.12f);
  const float d = h * 0.08f;  // FaceButtons' distance from the diamond's middle
  const float top = center.y - h * 0.36f;
  // Share and Options beside the touchpad; View and Menu (and their likes) beside the guide button.
  const float menu_x = playstation ? w * 0.15f + h * 0.04f : w * 0.08f;
  const float menu_y = playstation                    ? center.y - h * 0.28f
                       : look == PadLook::kSwitchPro ? center.y - h * 0.28f
                                                     : center.y - h * 0.14f;
  switch (point) {
    case PadPoint::kA: return ImVec2(face.x, face.y + d);
    case PadPoint::kB: return ImVec2(face.x + d, face.y);
    case PadPoint::kX: return ImVec2(face.x - d, face.y);
    case PadPoint::kY: return ImVec2(face.x, face.y - d);
    case PadPoint::kLeftShoulder: return ImVec2(center.x - w * 0.32f, top + h * 0.02f);
    case PadPoint::kRightShoulder: return ImVec2(center.x + w * 0.32f, top + h * 0.02f);
    case PadPoint::kLeftTrigger: return ImVec2(center.x - w * 0.24f, top);
    case PadPoint::kRightTrigger: return ImVec2(center.x + w * 0.24f, top);
    case PadPoint::kLeftStick: return left_stick;
    case PadPoint::kRightStick: return right_stick;
    case PadPoint::kDpad: return dpad;
    case PadPoint::kBack: return ImVec2(center.x - menu_x, menu_y);
    case PadPoint::kStart: return ImVec2(center.x + menu_x, menu_y);
  }
  return center;
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
    AddTextCentered(draw_list, font, key_size, ImVec2((top_left.x + key_max.x) * 0.5f, (top_left.y + key_max.y) * 0.5f),
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
    AddTextCentered(draw_list, font, 11.0f * unit, center, Panel(alpha), "A");
  }
  return 16.0f * unit;
}

const char* ControlName(rex::input::PadControl control, PadLook look) {
  const bool playstation = look == PadLook::kDualSense || look == PadLook::kDualShock4;
  const bool nintendo = look == PadLook::kSwitchPro;
  switch (control) {
    case rex::input::PadControl::kLeftShoulder: return playstation ? "L1" : nintendo ? "L" : "LB";
    case rex::input::PadControl::kRightShoulder: return playstation ? "R1" : nintendo ? "R" : "RB";
    case rex::input::PadControl::kLeftTrigger: return playstation ? "L2" : nintendo ? "ZL" : "LT";
    case rex::input::PadControl::kRightTrigger: return playstation ? "R2" : nintendo ? "ZR" : "RT";
    case rex::input::PadControl::kLeftThumb: return playstation ? "L3" : "LS";
    case rex::input::PadControl::kRightThumb: return playstation ? "R3" : "RS";
    case rex::input::PadControl::kBack:
      return look == PadLook::kDualSense ? "Create" : look == PadLook::kDualShock4 ? "Share" : nintendo ? "-" : look == PadLook::kXbox ? "View" : "Select";
    case rex::input::PadControl::kStart:
      return playstation ? "Options" : nintendo ? "+" : look == PadLook::kXbox ? "Menu" : "Start";
    default: return "";
  }
}

float DrawPadGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 center, float height, PadLook look, rex::input::PadControl control,
                   int alpha, float unit) {
  auto col = [&](int r, int g, int b) { return IM_COL32(r, g, b, alpha); };
  const float radius = height * 0.5f;
  const bool face = control == rex::input::PadControl::kA || control == rex::input::PadControl::kB || control == rex::input::PadControl::kX ||
                    control == rex::input::PadControl::kY;
  if (!face) {
    const std::string name = ControlName(control, look);
    const float size = height * 0.52f;
    const float text_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str()).x;
    const float width = std::max(height * 1.3f, text_width + 10.0f * unit);
    const ImVec2 min(center.x - width * 0.5f, center.y - radius);
    draw_list->AddRectFilled(min, ImVec2(min.x + width, center.y + radius), col(58, 63, 70), 5.0f * unit);
    AddTextCentered(draw_list, font, size, center, Text(alpha), name.c_str());
    return width;
  }
  // The face buttons by position: A at the bottom, B on the right, X on the left, Y at the top.
  const int position = control == rex::input::PadControl::kY ? 0 : control == rex::input::PadControl::kB ? 1 : control == rex::input::PadControl::kA ? 2 : 3;
  switch (look) {
    case PadLook::kDualSense:
    case PadLook::kDualShock4: {
      // The PlayStation shapes: triangle, circle, cross, square; coloured on the DualShock 4.
      const ImU32 colours[4] = {col(63, 191, 154), col(228, 90, 98), col(122, 167, 230), col(217, 138, 201)};
      const ImU32 shape = look == PadLook::kDualShock4 ? colours[position] : col(217, 219, 224);
      draw_list->AddCircleFilled(center, radius, col(42, 44, 49), 24);
      const float s = radius * 0.5f;
      const float t = 1.6f * unit;
      switch (position) {
        case 0:
          draw_list->AddTriangle(ImVec2(center.x, center.y - s * 1.05f), ImVec2(center.x + s, center.y + s * 0.7f),
                                 ImVec2(center.x - s, center.y + s * 0.7f), shape, t);
          break;
        case 1: draw_list->AddCircle(center, s, shape, 20, t); break;
        case 2:
          draw_list->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), shape, t);
          draw_list->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x + s, center.y - s), shape, t);
          break;
        default: draw_list->AddRect(ImVec2(center.x - s * 0.85f, center.y - s * 0.85f), ImVec2(center.x + s * 0.85f, center.y + s * 0.85f), shape, 0, 0, t); break;
      }
      return height;
    }
    case PadLook::kXbox: {
      const char* letters[4] = {"Y", "B", "A", "X"};
      const ImU32 colours[4] = {col(232, 181, 44), col(216, 64, 58), col(95, 179, 60), col(59, 119, 216)};
      draw_list->AddCircleFilled(center, radius, colours[position], 24);
      const float size = height * 0.6f;
      AddTextCentered(draw_list, font, size, center, col(17, 17, 17), letters[position]);
      return height;
    }
    default: {
      // Switch Pro: X at the top, A on the right, B at the bottom, Y on the left. Others: 4, 2, 1, 3.
      const char* switch_letters[4] = {"X", "A", "B", "Y"};
      const char* numbers[4] = {"4", "2", "1", "3"};
      const char* label = look == PadLook::kSwitchPro ? switch_letters[position] : numbers[position];
      draw_list->AddCircleFilled(center, radius, col(70, 74, 81), 24);
      const float size = height * 0.6f;
      AddTextCentered(draw_list, font, size, center, Text(alpha), label);
      return height;
    }
  }
}

float DrawCancelGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 top_left, float unit,
                      const rex::input::DeviceInfo* pad, const char* escape, int alpha) {
  if (!pad) {
    return DrawConfirmGlyph(draw_list, font, top_left, unit, nullptr, escape, alpha);
  }
  const ImVec2 center(top_left.x + 8.0f * unit, top_left.y + 8.0f * unit);
  draw_list->AddCircleFilled(center, 8.0f * unit, Imulsion(alpha), 24);
  const PadLook look = LookOf(*pad);
  if (look == PadLook::kDualSense || look == PadLook::kDualShock4) {
    draw_list->AddCircle(ImVec2(center.x - 0.5f, center.y - 0.5f), 3.8f * unit, Panel(alpha), 24, 1.8f * unit);
  } else {
    AddTextCentered(draw_list, font, 11.0f * unit, center, Panel(alpha), "B");
  }
  return 16.0f * unit;
}

bool CloseInputHeld(rex::input::InputSystem* input, const std::vector<rex::input::DeviceId>& pads) {
  if (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) || ImGui::IsKeyDown(ImGuiKey_Escape) ||
      ImGui::IsKeyDown(ImGuiKey_Backspace)) {
    return true;
  }
  if (!input) return false;
  constexpr int kTriggerHeld = 30;
  for (const auto id : pads) {
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input->GetDeviceState(id, &state) != 0) continue;
    if (state.gamepad.buttons || state.gamepad.left_trigger > kTriggerHeld || state.gamepad.right_trigger > kTriggerHeld) {
      return true;
    }
  }
  return false;
}

LastUsedInput::LastUsedInput(rex::input::InputSystem* input)
    : keyboard_(input && input->ActiveKind(0) == rex::input::ActiveDeviceKind::kKeyboardMouse) {}

void LastUsedInput::NoteKeyboard() {
  for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
    // ImGui's own controller keys are the controllers' presses, which NotePad takes.
    if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown) continue;
    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) {
      keyboard_ = true;
      return;
    }
  }
}

}  // namespace azura::coop_ui
