#include "shader_compile_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include <rex/system/interfaces/graphics.h>

namespace azura {

namespace {

// Work that ends sooner than this shows nothing.
constexpr auto kShowAfter = std::chrono::milliseconds(400);
// How long the notice stays (fading out) once nothing is pending.
constexpr auto kHideAfter = std::chrono::milliseconds(800);

constexpr float kPi = 3.14159265f;

struct Texts {
  const char* title;
  // printf format with the percentage as %d.
  const char* progress;
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"SHADER WERDEN KOMPILIERT", "Grafik wird vorbereitet (%d %%)"};
    case 4:  // French
      return {"COMPILATION DES SHADERS", "Pr\xC3\xA9paration des graphismes (%d %%)"};
    case 5:  // Spanish
      return {"COMPILANDO SHADERS", "Preparando los gr\xC3\xA1""ficos (%d %%)"};
    case 6:  // Italian
      return {"COMPILAZIONE SHADER", "Preparazione della grafica (%d%%)"};
    case 9:  // Portuguese
      return {"COMPILANDO SHADERS", "Preparando os gr\xC3\xA1""ficos (%d%%)"};
    case 11:  // Polish
      return {"KOMPILOWANIE SHADER\xC3\x93W", "Przygotowywanie grafiki (%d%%)"};
    default:
      return {"COMPILING SHADERS", "Preparing graphics (%d%%)"};
  }
}

// A cog like the one of the game's checkpoint indicator: a ring with square-ish teeth and a hole
// in the middle, light silver with a darker rim.
void DrawCog(ImDrawList* draw_list, ImVec2 center, float radius, float angle, float alpha) {
  constexpr int kTeeth = 10;
  const float body_radius = radius * 0.80f;
  const float hole_radius = radius * 0.46f;
  // Half the angular width of a tooth at its root and at its tip.
  const float tooth_root = kPi / kTeeth * 0.52f;
  const float tooth_tip = kPi / kTeeth * 0.38f;
  auto draw = [&](float grow, ImU32 color) {
    float ring_radius = (body_radius + hole_radius) * 0.5f;
    float ring_width = body_radius - hole_radius + grow * 2.0f;
    draw_list->AddCircle(center, ring_radius, color, 48, ring_width);
    for (int i = 0; i < kTeeth; ++i) {
      float a = angle + i * (2.0f * kPi / kTeeth);
      float r0 = body_radius - radius * 0.05f;
      float r1 = radius + grow;
      ImVec2 points[4] = {
          {center.x + std::cos(a - tooth_root) * r0, center.y + std::sin(a - tooth_root) * r0},
          {center.x + std::cos(a - tooth_tip) * r1, center.y + std::sin(a - tooth_tip) * r1},
          {center.x + std::cos(a + tooth_tip) * r1, center.y + std::sin(a + tooth_tip) * r1},
          {center.x + std::cos(a + tooth_root) * r0, center.y + std::sin(a + tooth_root) * r0},
      };
      draw_list->AddConvexPolyFilled(points, 4, color);
    }
  };
  // Shadow, rim, then the silver face slightly inside it.
  ImVec2 shadow_offset(radius * 0.04f, radius * 0.06f);
  draw_list->AddCircle(ImVec2(center.x + shadow_offset.x, center.y + shadow_offset.y),
                       (body_radius + hole_radius) * 0.5f, IM_COL32(0, 0, 0, int(110 * alpha)),
                       48, body_radius - hole_radius + radius * 0.1f);
  draw(radius * 0.05f, IM_COL32(70, 72, 78, int(255 * alpha)));
  draw(0.0f, IM_COL32(214, 216, 220, int(255 * alpha)));
  // A darker inner edge around the hole, as the game's cog has.
  draw_list->AddCircle(center, hole_radius + radius * 0.03f,
                       IM_COL32(120, 122, 128, int(255 * alpha)), 48, radius * 0.06f);
}

// Text with a soft drop shadow, readable over bright and dark scenes alike.
void DrawShadowedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position,
                      const char* text, float alpha) {
  float offset = std::max(1.0f, size * 0.06f);
  draw_list->AddText(font, size, ImVec2(position.x + offset, position.y + offset),
                     IM_COL32(0, 0, 0, int(170 * alpha)), text);
  draw_list->AddText(font, size, position, IM_COL32(242, 242, 242, int(255 * alpha)), text);
}

}  // namespace

ShaderCompileOverlay::ShaderCompileOverlay(rex::ui::ImGuiDrawer* drawer,
                                           rex::system::IGraphicsSystem* graphics_system,
                                           uint32_t language, ImFont* title_font,
                                           ImFont* body_font, ShaderCompileOverlay** owner)
    : ImGuiDialog(drawer),
      graphics_system_(graphics_system),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner),
      created_(Clock::now()),
      busy_since_(created_) {}

void ShaderCompileOverlay::OnDraw(ImGuiIO& io) {
  rex::system::IGraphicsSystem::ShaderCompilation work = graphics_system_->GetShaderCompilation();
  Clock::time_point now = Clock::now();
  if (work.queued) {
    if (idle_) {
      // More work before the notice went away: one notice for both.
      idle_ = false;
      if (!shown_) {
        busy_since_ = now;
      }
    }
    if (now - busy_since_ >= kShowAfter) {
      shown_ = true;
    }
  } else if (!idle_) {
    idle_ = true;
    idle_since_ = now;
  }
  if (idle_ && (!shown_ || now - idle_since_ >= kHideAfter)) {
    Close();
    return;
  }
  if (!shown_) {
    return;
  }

  // Fades out after the work is done, at 100%.
  float alpha = 1.0f;
  int percent = 100;
  if (idle_) {
    alpha = 1.0f - std::chrono::duration<float>(now - idle_since_) /
                       std::chrono::duration<float>(kHideAfter);
  } else {
    percent = int(100.0f * float(work.done) / float(std::max(work.queued, uint32_t(1))));
  }

  // Small, in the top left corner of the 16:9 area the game image takes in the window, away from
  // the game's own indicators at the bottom; sizes in pixels at 720p.
  float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  float game_width = game_height * 16.0f / 9.0f;
  float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  float unit = game_height / 720.0f;
  alpha *= 0.85f;
  ImVec2 cog_center(game_left + 34.0f * unit, game_top + 34.0f * unit);
  float seconds = std::chrono::duration<float>(now - created_).count();
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  DrawCog(draw_list, cog_center, 12.0f * unit, seconds * 1.6f, alpha);

  Texts texts = LocalizedTexts(language_);
  char progress[96];
  std::snprintf(progress, sizeof(progress), texts.progress, percent);
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  float title_size = 13.0f * unit;
  float body_size = 11.0f * unit;
  float text_left = game_left + 52.0f * unit;
  DrawShadowedText(draw_list, title_font, title_size, ImVec2(text_left, game_top + 21.0f * unit),
                   texts.title, alpha);
  DrawShadowedText(draw_list, body_font, body_size, ImVec2(text_left, game_top + 35.0f * unit),
                   progress, alpha);
}

void ShaderCompileOverlay::OnClose() {
  if (owner_) {
    *owner_ = nullptr;
  }
}

}  // namespace azura
