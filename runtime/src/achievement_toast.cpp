#include "achievement_toast.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

namespace azura {

namespace {

// Timing, in seconds: the panel fades and slides in, stays, and fades out. The next notice, when
// several achievements unlock together, starts a moment after the previous one has gone.
constexpr float kFadeIn = 0.3f;
constexpr float kFadeOut = 0.4f;
constexpr float kShowSeconds = 5.0f;
constexpr auto kGap = std::chrono::milliseconds(500);

// The gold of the launcher's accent colour.
constexpr ImU32 Gold(int alpha) { return IM_COL32(255, 184, 28, alpha); }

// "ACHIEVEMENT UNLOCKED" in each language of the Xbox 360 language IDs. Simplified Chinese (10)
// has no font and uses the English text.
const char* LocalizedHeading(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return "実績のロックが解除されました";
    case 7:  // Korean
      return "도전 과제 잠금 해제";
    case 8:  // Traditional Chinese
      return "成就已解鎖";
    case 12:  // Russian
      return "ДОСТИЖЕНИЕ ПОЛУЧЕНО";
    case 3:  // German
      return "ERFOLG FREIGESCHALTET";
    case 4:  // French
      return "SUCC\xC3\x88S D\xC3\x89""BLOQU\xC3\x89";
    case 5:  // Spanish
      return "LOGRO DESBLOQUEADO";
    case 6:  // Italian
      return "OBIETTIVO SBLOCCATO";
    case 9:  // Portuguese
      return "CONQUISTA DESBLOQUEADA";
    case 11:  // Polish
      return "OSI\xC4\x84GNI\xC4\x98" "CIE ODBLOKOWANE";
    default:
      return "ACHIEVEMENT UNLOCKED";
  }
}

// Decodes the next UTF-8 code point of `text` starting at `index`, and moves `index` past it.
// Malformed input yields U+FFFD.
uint32_t NextCodePoint(std::string_view text, size_t& index) {
  const auto byte = [&](size_t i) { return static_cast<uint8_t>(text[i]); };
  uint32_t lead = byte(index);
  size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
  if (length == 0 || index + length > text.size()) {
    ++index;
    return 0xFFFD;
  }
  uint32_t code = length == 1 ? lead : lead & (0xFF >> (length + 1));
  for (size_t i = 1; i < length; ++i) {
    code = (code << 6) | (byte(index + i) & 0x3F);
  }
  index += length;
  return code;
}

// Whether `font` can draw every character of `text`.
bool CanDraw(ImFont* font, std::string_view text) {
  size_t index = 0;
  while (index < text.size()) {
    uint32_t code = NextCodePoint(text, index);
    if (code < 0x20) {
      continue;
    }
    if (code > 0xFFFF || !font->IsGlyphInFont(static_cast<ImWchar>(code))) {
      return false;
    }
  }
  return true;
}

// `text` cut to fit `max_width` at `size`, with "..." at the end when it was cut.
std::string Fit(ImFont* font, float size, const std::string& text, float max_width) {
  if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
    return text;
  }
  // Cut one character at a time (whole UTF-8 sequences) until the text and the dots fit.
  std::vector<size_t> ends;
  for (size_t index = 0; index < text.size();) {
    NextCodePoint(text, index);
    ends.push_back(index);
  }
  while (!ends.empty()) {
    ends.pop_back();
    std::string cut = text.substr(0, ends.empty() ? 0 : ends.back()) + "...";
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, cut.c_str()).x <= max_width) {
      return cut;
    }
  }
  return "...";
}

}  // namespace

AchievementToast::AchievementToast(rex::ui::ImGuiDrawer* drawer,
                                   rex::ui::ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime, uint32_t language, ImFont* title_font,
                                   ImFont* body_font)
    : AchievementNotificationDialog(drawer),
      runtime_(runtime),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      icon_cache_(immediate_drawer, runtime),
      next_start_(Clock::now()) {}

void AchievementToast::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(event);
}

bool AchievementToast::EnglishTexts(uint32_t id, std::string& name,
                                    std::string& description) const {
  if (!runtime_ || !runtime_->kernel_state()) {
    return false;
  }
  const rex::system::util::XdbfGameData data = runtime_->kernel_state()->title_xdbf();
  if (!data.is_valid()) {
    return false;
  }
  for (const auto& entry : data.GetAchievements()) {
    if (static_cast<uint32_t>(entry.id) != id) {
      continue;
    }
    name = data.GetStringTableEntry(rex::system::XLanguage::kEnglish, entry.label_id);
    description = data.GetStringTableEntry(rex::system::XLanguage::kEnglish, entry.description_id);
    return !name.empty();
  }
  return false;
}

void AchievementToast::Prepare(Display& display) {
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const rex::system::AchievementInfo& achievement = display.event.achievement;

  display.heading = LocalizedHeading(language_);
  if (!CanDraw(title_font, display.heading)) {
    display.heading = LocalizedHeading(1);
  }
  display.name = achievement.label;
  display.description = achievement.description;
  if (!CanDraw(title_font, display.name) || !CanDraw(body_font, display.description)) {
    std::string english_name;
    std::string english_description;
    if (EnglishTexts(achievement.id, english_name, english_description)) {
      display.name = english_name;
      display.description = english_description;
    }
  }
  if (achievement.gamerscore > 0) {
    display.score = std::to_string(achievement.gamerscore) + " G";
  }
}

bool AchievementToast::NeedsContinuousPaint() const {
  // current_ belongs to the UI thread, which is the one asking.
  if (current_) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return !queue_.empty();
}

void AchievementToast::OnDraw(ImGuiIO& io) {
  Clock::time_point now = Clock::now();
  if (current_ &&
      std::chrono::duration<float>(now - current_->shown_since).count() >= kShowSeconds) {
    current_.reset();
    next_start_ = now + kGap;
  }
  if (!current_) {
    if (now < next_start_) {
      return;
    }
    Display next;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) {
        return;
      }
      next.event = queue_.front();
      queue_.pop_front();
    }
    Prepare(next);
    next.shown_since = now;
    current_ = std::move(next);
  }
  const Display& display = *current_;
  const float age = std::chrono::duration<float>(now - display.shown_since).count();

  // Fade and slide in from below, fade out at the end.
  float alpha = 1.0f;
  float slide = 0.0f;
  if (age < kFadeIn) {
    float t = age / kFadeIn;
    alpha = t;
    slide = std::pow(1.0f - t, 3.0f);
  } else if (age > kShowSeconds - kFadeOut) {
    alpha = std::max(0.0f, (kShowSeconds - age) / kFadeOut);
  }

  // The 16:9 area the game image takes in the window; sizes in pixels at 720p.
  float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  float game_width = game_height * 16.0f / 9.0f;
  float game_right = (io.DisplaySize.x + game_width) * 0.5f;
  float game_bottom = (io.DisplaySize.y + game_height) * 0.5f;
  float unit = game_height / 720.0f;

  float panel_width = 316.0f * unit;
  float panel_height = 62.0f * unit;
  float right = game_right - 26.0f * unit;
  float bottom = game_bottom - 62.0f * unit + slide * 14.0f * unit;
  ImVec2 top_left(right - panel_width, bottom - panel_height);
  ImVec2 bottom_right(right, bottom);

  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  auto faded = [alpha](int value) { return int(float(value) * alpha); };

  // Panel: dark and translucent with a thin gold line.
  draw_list->AddRectFilled(top_left, bottom_right, IM_COL32(12, 12, 15, faded(220)), 3.0f * unit);
  draw_list->AddRect(top_left, bottom_right, Gold(faded(235)), 3.0f * unit, 0, 1.25f * unit);

  // Icon on the left.
  float text_left = top_left.x + 14.0f * unit;
  rex::ui::ImmediateTexture* icon = icon_cache_.GetIcon(display.event.achievement);
  if (icon) {
    float icon_size = 44.0f * unit;
    ImVec2 icon_top_left(top_left.x + 9.0f * unit, top_left.y + 9.0f * unit);
    ImVec2 icon_bottom_right(icon_top_left.x + icon_size, icon_top_left.y + icon_size);
    draw_list->AddImage(reinterpret_cast<ImTextureID>(icon), icon_top_left, icon_bottom_right,
                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, faded(255)));
    draw_list->AddRect(icon_top_left, icon_bottom_right, Gold(faded(110)), 1.5f * unit, 0,
                       1.0f * unit);
    text_left = icon_bottom_right.x + 11.0f * unit;
  }

  float text_right = bottom_right.x - 11.0f * unit;
  float heading_size = 10.5f * unit;
  float name_size = 15.0f * unit;
  float description_size = 11.0f * unit;

  // Gamerscore at the right of the heading line.
  float heading_right = text_right;
  if (!display.score.empty()) {
    float score_size = 11.5f * unit;
    float score_width = title_font->CalcTextSizeA(score_size, FLT_MAX, 0.0f, display.score.c_str()).x;
    draw_list->AddText(title_font, score_size,
                       ImVec2(text_right - score_width, top_left.y + 8.0f * unit),
                       Gold(faded(255)), display.score.c_str());
    heading_right = text_right - score_width - 8.0f * unit;
  }

  std::string heading = Fit(title_font, heading_size, display.heading, heading_right - text_left);
  draw_list->AddText(title_font, heading_size, ImVec2(text_left, top_left.y + 8.5f * unit),
                     Gold(faded(255)), heading.c_str());
  std::string name = Fit(title_font, name_size, display.name, text_right - text_left);
  draw_list->AddText(title_font, name_size, ImVec2(text_left, top_left.y + 21.0f * unit),
                     IM_COL32(246, 246, 246, faded(255)), name.c_str());
  std::string description =
      Fit(body_font, description_size, display.description, text_right - text_left);
  draw_list->AddText(body_font, description_size, ImVec2(text_left, top_left.y + 41.5f * unit),
                     IM_COL32(176, 178, 184, faded(255)), description.c_str());
}

}  // namespace azura
