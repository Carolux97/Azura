#include "graphics_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#include "coop_ui.h"
#include "input/player_assignment.h"
#include "menu/pc_options.h"
#include "menu_language.h"
#include "script_hooks.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using Slot = input::PlayerAssignment::Slot;

constexpr float kFadeSeconds = 0.18f;
// How far the left stick goes before it counts as a press.
constexpr int kStickPress = 20000;
// A direction held on a controller repeats after this long, then this often.
constexpr auto kRepeatDelay = std::chrono::milliseconds(400);
constexpr auto kRepeatEvery = std::chrono::milliseconds(90);
constexpr uint16_t kDirections = rex::input::X_INPUT_GAMEPAD_DPAD_UP | rex::input::X_INPUT_GAMEPAD_DPAD_DOWN |
                                 rex::input::X_INPUT_GAMEPAD_DPAD_LEFT | rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;

// The menu's rows in order, each in a section (0 image, 1 performance, 2 effects, 3 overlay) and
// edited through the PC option of its cvar; the brightness row has none (PcOptionCalibrate).
struct Row {
  int section;
  const char* cvar;
};
constexpr Row kRows[] = {
    {0, nullptr},
    {0, "fullscreen"},
    {0, "upscaling_preset"},
    {0, "render_resolution"},
    {0, "present_effect"},
    {0, "upscaling_sharpness"},
    {1, "frame_rate_limit"},
    {1, "dynamic_shadows"},
    {1, "depth_of_field"},
    {1, "anisotropic_override"},
    {2, "swap_post_effect"},
    {2, "motion_blur"},
    {2, "film_grain"},
    {2, "ambient_occlusion"},
    {2, "bloom"},
    {2, "lens_flares"},
    {2, "field_of_view"},
    {3, "performance_overlay"},
    {3, "performance_overlay_position"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr int kBrightnessRow = 0;

// The row the menu starts on when it opens again after the brightness calibration it opened; -1
// when it opens from the Options.
int g_reopen_focus = -1;

int OptionOf(int row) {
  return kRows[row].cvar ? menu::PcOptionByCvar(kRows[row].cvar) : menu::PcOptionCalibrate();
}

// Whether row `row` is shown now: its option exists in this build and shows (the sharpness only with
// an effect that sharpens).
bool RowShown(int row) {
  const int option = OptionOf(row);
  return option >= 0 && menu::PcOptionShown(option);
}

struct Texts {
  const char* title;
  const char* sections[4];  // image, performance, effects, overlay
  const char* change;
  const char* back;
  const char* enter;      // the Enter key, on the keyboard's hint
  const char* by_preset;  // under a row the preset decides; %s is the preset's Custom value
  const char* restart;    // the tag of a change that applies at the next start
  const char* defaults;   // the defaults button's hint
  const char* confirm;    // that hint once pressed, until it is pressed again
};

// In the game's languages; the runtime loads the Cyrillic and CJK fonts each one needs.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"ビデオ", {"画像", "パフォーマンス", "エフェクト", "オーバーレイ"}, "変更", "戻る", "Enter",
              "アップスケーリングのプリセットが決めます。変更するとプリセットは「%s」になります。", "再起動後", "初期設定に戻す", "もう一度押すと戻します"};
    case 7:  // Korean
      return {"비디오", {"이미지", "성능", "효과", "오버레이"}, "변경", "뒤로", "Enter",
              "업스케일링 프리셋이 정합니다. 바꾸면 프리셋이 '%s'(으)로 바뀝니다.", "재시작 후", "기본값 복원", "한 번 더 누르면 복원합니다"};
    case 8:  // Traditional Chinese
      return {"視訊", {"影像", "效能", "特效", "疊加顯示"}, "變更", "返回", "Enter",
              "由放大預設決定。變更後，預設會切換為「%s」。", "重新啟動後", "還原預設值", "再按一次即可還原"};
    case 12:  // Russian
      return {"ВИДЕО", {"ИЗОБРАЖЕНИЕ", "ПРОИЗВОДИТЕЛЬНОСТЬ", "ЭФФЕКТЫ", "ОВЕРЛЕЙ"}, "Изменить", "Назад", "Enter",
              "Выбирается предустановкой масштабирования. Если изменить, предустановка станет «%s».",
              "ПОСЛЕ ПЕРЕЗАПУСКА", "По умолчанию", "Нажмите ещё раз, чтобы сбросить"};
    case 3:  // German
      return {"VIDEO", {"BILD", "LEISTUNG", "EFFEKTE", "EINBLENDUNG"}, "Ändern", "Zurück", "Eingabe",
              "Wird von der Skalierungsvoreinstellung gewählt. Eine Änderung stellt die Voreinstellung auf %s.",
              "NACH NEUSTART", "Standard", "Zum Zurücksetzen erneut drücken"};
    case 4:  // French
      return {"VIDÉO", {"IMAGE", "PERFORMANCES", "EFFETS", "INCRUSTATION"}, "Changer", "Retour", "Entrée",
              "Choisi par le préréglage de mise à l'échelle. Le modifier passe le préréglage sur %s.",
              "AU REDÉMARRAGE", "Par défaut", "Appuyez à nouveau pour rétablir"};
    case 5:  // Spanish
      return {"VÍDEO", {"IMAGEN", "RENDIMIENTO", "EFECTOS", "SUPERPOSICIÓN"}, "Cambiar", "Volver", "Intro",
              "Lo decide el preajuste de escalado. Al cambiarlo, el preajuste pasa a %s.", "AL REINICIAR", "Predeterminados", "Pulsa otra vez para restaurar"};
    case 6:  // Italian
      return {"VIDEO", {"IMMAGINE", "PRESTAZIONI", "EFFETTI", "SOVRAPPOSIZIONE"}, "Cambia", "Indietro", "Invio",
              "Lo sceglie il preset di scalatura. Cambiandolo, il preset passa a %s.", "AL RIAVVIO", "Predefiniti", "Premi di nuovo per ripristinare"};
    case 9:  // Portuguese
      return {"VÍDEO", {"IMAGEM", "DESEMPENHO", "EFEITOS", "SOBREPOSIÇÃO"}, "Mudar", "Voltar", "Enter",
              "Escolhido pela predefinição de escala. Ao mudá-lo, a predefinição passa a %s.", "AO REINICIAR", "Padrões", "Pressione de novo para restaurar"};
    case 11:  // Polish
      return {"OBRAZ", {"OBRAZ", "WYDAJNOŚĆ", "EFEKTY", "NAKŁADKA"}, "Zmień", "Wróć", "Enter",
              "Wybiera to ustawienie wstępne skalowania. Zmiana przełącza je na: %s.", "PO RESTARCIE", "Domyślne", "Naciśnij ponownie, aby przywrócić"};
    case kLanguageCzech:
      return {"OBRAZ", {"OBRAZ", "VÝKON", "EFEKTY", "PŘEKRYV"}, "Změnit", "Zpět", "Enter",
              "Určuje předvolba škálování. Změnou se předvolba přepne na: %s.", "PO RESTARTU", "Výchozí", "Stiskněte znovu pro obnovení"};
    case kLanguageHungarian:
      return {"VIDEÓ", {"KÉP", "TELJESÍTMÉNY", "EFFEKTEK", "ÁTFEDÉS"}, "Módosítás", "Vissza", "Enter",
              "A felskálázási előbeállítás választja. Ha módosítod, az előbeállítás ez lesz: %s.",
              "ÚJRAINDÍTÁS UTÁN", "Alapértékek", "Nyomd meg újra a visszaállításhoz"};
    default:
      return {"VIDEO", {"IMAGE", "PERFORMANCE", "EFFECTS", "OVERLAY"}, "Change", "Back", "Enter",
              "Chosen by the upscaling preset. Changing it sets the preset to %s.", "AFTER RESTART", "Defaults", "Press again to restore"};
  }
}

// The text of the value of a slider (one decimal, as its steps).
std::string SliderText(float value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%.1f", value);
  return text;
}

bool Inside(ImVec2 point, ImVec2 min, ImVec2 max) {
  return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
}

}  // namespace

GraphicsMenu::GraphicsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                           input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                           ImFont* body_font, GraphicsMenu** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner),
      opened_(Clock::now()),
      last_input_(input) {
  if (assignment_) assignment_->SetBlocked(true);
  if (g_reopen_focus >= 0 && g_reopen_focus < kRowCount) focus_ = g_reopen_focus;
  g_reopen_focus = -1;
  if (!RowShown(focus_)) MoveFocus(1);
  script::PlayMenuSound(script::MenuSound::kOpen);
}

void GraphicsMenu::MoveFocus(int step) {
  for (int tries = 0; tries < kRowCount; ++tries) {
    focus_ = (focus_ + step + kRowCount) % kRowCount;
    if (RowShown(focus_)) return;
  }
}

void GraphicsMenu::Defaults() {
  const Clock::time_point now = Clock::now();
  if (now >= confirm_defaults_until_) {
    script::PlayMenuSound(script::MenuSound::kAccept);
    confirm_defaults_until_ = now + std::chrono::seconds(3);
    return;
  }
  confirm_defaults_until_ = {};
  script::PlayMenuSound(script::MenuSound::kAccept);
  REXLOG_INFO("[Azura options] The graphics menu puts the Video settings back to their defaults");
  menu::PcOptionsResetDefaults(menu::kVideoCategory);
}

void GraphicsMenu::Change(int row, int direction) {
  const int option = OptionOf(row);
  if (option < 0) return;
  const menu::PcOptionInfo info = menu::PcOptionDescribe(option);
  if (info.action) {
    // The calibration screen opens once the menu has closed (OnClose), over the Options.
    script::PlayMenuSound(script::MenuSound::kAccept);
    to_calibration_ = true;
    closing_ = true;
    return;
  }
  // A row the preset decides starts from the preset's pick, which becomes the player's own.
  float preset_value = 0.0f;
  if (menu::PcOptionDecidedByPreset(option, preset_value)) menu::PcOptionsLeavePreset();
  const float value = menu::PcOptionDisplayValue(option);
  float next = value;
  if (info.slider) {
    const float step = info.slider_step > 0 ? info.slider_step : 0.1f;
    next = direction == 0 && value + step > info.slider_max + step * 0.5f ? info.slider_min : value + (direction < 0 ? -step : step);
    next = std::clamp(std::round(next / step) * step, info.slider_min, info.slider_max);
  } else {
    const int count = static_cast<int>(info.values.size());
    if (count <= 0) return;
    const int index = std::clamp(static_cast<int>(std::lround(value)), 0, count - 1);
    // Two values flip either way; longer lists stop at their ends, except going forward with A.
    int wanted = index + (direction < 0 ? -1 : 1);
    if (count == 2 || direction == 0) {
      wanted = (wanted + count) % count;
    } else {
      wanted = std::clamp(wanted, 0, count - 1);
    }
    next = static_cast<float>(wanted);
  }
  if (std::abs(next - value) < 1e-4f) return;
  if (info.slider) {
    script::PlayMenuSound(next < value ? script::MenuSound::kSliderLeft : script::MenuSound::kSliderRight);
  } else {
    script::PlayMenuSound(direction == 0 ? script::MenuSound::kAccept
                                         : direction < 0 ? script::MenuSound::kLeft : script::MenuSound::kRight);
  }
  // Choosing Custom in the preset row starts from what the preset picked too.
  if (kRows[row].cvar == std::string("upscaling_preset") && next == 0.0f) {
    menu::PcOptionsLeavePreset();
    return;
  }
  menu::PcOptionStoreValue(option, next);
}

GraphicsMenu::Presses GraphicsMenu::ReadInput() {
  Presses presses;
  const Clock::time_point now = Clock::now();
  uint16_t held = 0;
  uint16_t pressed_all = 0;
  for (const Slot& slot : assignment_->Slots()) {
    if (slot.keyboard) continue;
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
    uint16_t buttons = state.gamepad.buttons;
    if (state.gamepad.thumb_ly >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
    if (state.gamepad.thumb_ly <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
    if (state.gamepad.thumb_lx <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
    if (state.gamepad.thumb_lx >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
    auto found = previous_buttons_.find(slot.device.id);
    const bool seen = found != previous_buttons_.end();
    const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
    previous_buttons_[slot.device.id] = buttons;
    if (pressed) last_input_.NotePad(slot.device.id);
    if (seen) held |= buttons & kDirections;
    pressed_all |= pressed;
  }
  // A direction held keeps repeating, as the game's own lists do.
  if (held && held == repeat_buttons_) {
    if (now >= repeat_at_) {
      pressed_all |= held;
      repeat_at_ = now + kRepeatEvery;
    }
  } else {
    repeat_buttons_ = held;
    repeat_at_ = now + kRepeatDelay;
  }
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_DPAD_UP) presses.move = -1;
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) presses.move = 1;
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) presses.change = -1;
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) presses.change = 1;
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_A) presses.accept = true;
  if (pressed_all & rex::input::X_INPUT_GAMEPAD_X) presses.defaults = true;
  if (pressed_all & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_START)) presses.close = true;
  // The keyboard, through ImGui's keys (the game's keyboard input is blocked); held keys repeat.
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)) presses.move = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S)) presses.move = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) presses.change = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) presses.change = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    presses.accept = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_R, false)) presses.defaults = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) presses.close = true;
  return presses;
}

void GraphicsMenu::OnDraw(ImGuiIO& io) {
  if (!assignment_ || !input_) {
    Close();
    return;
  }
  const std::vector<Slot> slots = assignment_->Slots();
  if (closing_) {
    std::vector<DeviceId> pads;
    for (const Slot& slot : slots) {
      if (!slot.keyboard) pads.push_back(slot.device.id);
    }
    if (!CloseInputHeld(input_, pads)) Close();
    return;
  }
  const Presses presses = ReadInput();
  if (presses.close) {
    script::PlayMenuSound(script::MenuSound::kBack);
    closing_ = true;
    return;
  }
  if (presses.move) {
    script::PlayMenuSound(presses.move < 0 ? script::MenuSound::kUp : script::MenuSound::kDown);
    MoveFocus(presses.move);
    scroll_to_focus_ = true;
  }
  if (presses.change) Change(focus_, presses.change);
  if (presses.accept) Change(focus_, 0);
  if (presses.defaults) Defaults();
  if (closing_) return;
  // A row that stopped showing (the sharpness, once the effect no longer sharpens) hands the focus up.
  if (!RowShown(focus_)) MoveFocus(-1);

  const Clock::time_point now = Clock::now();
  const float fade = std::min(1.0f, std::chrono::duration<float>(now - opened_).count() / kFadeSeconds);
  auto a = [&](int alpha) { return static_cast<int>(alpha * fade); };

  // Sizes in pixels at 720p, over the 16:9 area the game image takes in the window.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_width = game_height * 16.0f / 9.0f;
  const float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  const float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  const float unit = game_height / 720.0f;
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const Texts texts = LocalizedTexts(language_);
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, a(150)));

  const float margin = 30.0f * unit;
  const float panel_width = 760.0f * unit;
  const float panel_height = 640.0f * unit;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  DrawCutPanel(draw_list, panel_min, panel_max, 14.0f * unit, Panel(a(240)), SteelLine(a(255)), 1.0f * unit);
  draw_list->AddText(title_font, 26.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 20.0f * unit),
                     Imulsion(a(255)), texts.title);
  draw_list->AddLine(ImVec2(panel_min.x + margin, panel_min.y + 62.0f * unit),
                     ImVec2(panel_max.x - margin, panel_min.y + 62.0f * unit), Imulsion(a(200)), 1.25f * unit);

  // The list, between the title and the description of the focused row.
  const float list_left = panel_min.x + margin;
  const float list_right = panel_max.x - margin;
  const float list_top = panel_min.y + 74.0f * unit;
  const float help_height = 64.0f * unit;
  const float list_bottom = panel_max.y - 44.0f * unit - help_height;
  const float view_height = list_bottom - list_top;
  const float section_height = 30.0f * unit;
  const float row_height = 32.0f * unit;

  // Where each shown row and section title goes, from the top of the list.
  struct Placed {
    int row;
    float y;
  };
  std::vector<Placed> placed;
  std::vector<std::pair<int, float>> titles;  // section, y
  float content = 0.0f;
  int section = -1;
  for (int row = 0; row < kRowCount; ++row) {
    if (!RowShown(row)) continue;
    if (kRows[row].section != section) {
      section = kRows[row].section;
      if (!titles.empty() || !placed.empty()) content += 6.0f * unit;
      titles.push_back({section, content});
      content += section_height;
    }
    placed.push_back({row, content});
    content += row_height;
  }

  // The wheel scrolls; the controllers and the keyboard bring the focused row (and the title of its
  // section, for its first row) into view.
  const ImVec2 mouse = io.MousePos;
  const bool mouse_over_list = Inside(mouse, ImVec2(list_left, list_top), ImVec2(list_right, list_bottom));
  if (mouse_over_list && io.MouseWheel != 0.0f) {
    scroll_ -= io.MouseWheel * row_height * 2.0f;
    scroll_to_focus_ = false;
  }
  if (scroll_to_focus_) {
    for (const Placed& item : placed) {
      if (item.row != focus_) continue;
      const bool first_of_section = std::any_of(titles.begin(), titles.end(), [&](const auto& title) {
        return std::abs(title.second + section_height - item.y) < 0.5f;
      });
      const float top = first_of_section ? item.y - section_height : item.y;
      if (top < scroll_) scroll_ = top;
      if (item.y + row_height > scroll_ + view_height) scroll_ = item.y + row_height - view_height;
    }
    scroll_to_focus_ = false;
  }
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - view_height));

  draw_list->PushClipRect(ImVec2(list_left - 2.0f * unit, list_top), ImVec2(list_right + 2.0f * unit, list_bottom), true);
  for (const auto& [title_section, y] : titles) {
    const float top = list_top + y - scroll_;
    draw_list->AddText(title_font, 12.0f * unit, ImVec2(list_left, top + 10.0f * unit), Imulsion(a(255)),
                       texts.sections[title_section]);
    const float title_width = title_font->CalcTextSizeA(12.0f * unit, FLT_MAX, 0.0f, texts.sections[title_section]).x;
    draw_list->AddLine(ImVec2(list_left + title_width + 10.0f * unit, top + 17.0f * unit),
                       ImVec2(list_right, top + 17.0f * unit), SteelLine(a(255)), 1.0f * unit);
  }
  const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const float value_width = 260.0f * unit;
  int clicked_row = -1;
  int clicked_direction = 0;
  for (const Placed& item : placed) {
    const int option = OptionOf(item.row);
    const menu::PcOptionInfo info = menu::PcOptionDescribe(option);
    const ImVec2 row_min(list_left, list_top + item.y - scroll_);
    const ImVec2 row_max(list_right, row_min.y + row_height - 4.0f * unit);
    if (row_max.y < list_top || row_min.y > list_bottom) continue;
    const ImVec2 value_min(row_max.x - value_width, row_min.y);
    // The mouse: pointing chooses the row, a click changes it (backwards on its left arrow).
    if (mouse_over_list && Inside(mouse, row_min, row_max)) {
      if (mouse_moved) focus_ = item.row;
      if (clicked) {
        focus_ = item.row;
        clicked_row = item.row;
        clicked_direction = !info.action && mouse.x < value_min.x + 28.0f * unit && mouse.x >= value_min.x ? -1 : 0;
      }
    }
    const bool focused = item.row == focus_;
    float preset_value = 0.0f;
    const bool by_preset = menu::PcOptionDecidedByPreset(option, preset_value);
    if (focused) {
      DrawCutPanel(draw_list, row_min, row_max, 5.0f * unit, PanelSolid(a(255)), Imulsion(a(230)), 1.4f * unit);
    }
    const float middle = (row_min.y + row_max.y) * 0.5f;
    const std::string label = Fit(title_font, 14.0f * unit, info.label, row_max.x - row_min.x - value_width - 130.0f * unit);
    AddTextMiddleLeft(draw_list, title_font, 14.0f * unit, ImVec2(row_min.x + 12.0f * unit, middle),
                      by_preset ? TextDim(a(255)) : focused ? Imulsion(a(255)) : Text(a(255)), label.c_str());

    // The value between two arrows, dimmed while the preset decides it.
    std::string value_text;
    float fraction = -1.0f;  // a slider's position, under its value
    if (info.action) {
      value_text = "";
    } else if (info.slider) {
      const float value = menu::PcOptionDisplayValue(option);
      value_text = SliderText(value);
      if (info.slider_max > info.slider_min) fraction = (value - info.slider_min) / (info.slider_max - info.slider_min);
    } else if (!info.values.empty()) {
      const float value = by_preset ? preset_value : menu::PcOptionDisplayValue(option);
      const size_t index = std::min(static_cast<size_t>(std::max(0L, std::lround(value))), info.values.size() - 1);
      value_text = info.values[index];
    }
    const ImU32 value_color = by_preset ? TextFaint(a(255)) : focused ? Text(a(255)) : TextDim(a(255));
    if (info.action) {
      // The brightness row: an arrow into the calibration screen.
      DrawArrow(draw_list, ImVec2(row_max.x - 16.0f * unit, middle), 9.0f * unit, false,
                focused ? Imulsion(a(255)) : TextDim(a(255)));
    } else {
      const ImU32 arrow_color = by_preset ? TextFaint(a(255)) : focused ? Imulsion(a(255)) : TextFaint(a(255));
      DrawArrow(draw_list, ImVec2(value_min.x + 14.0f * unit, middle), 8.0f * unit, true, arrow_color);
      DrawArrow(draw_list, ImVec2(row_max.x - 14.0f * unit, middle), 8.0f * unit, false, arrow_color);
      const float center_x = (value_min.x + row_max.x) * 0.5f;
      const std::string shown = Fit(body_font, 14.0f * unit, value_text, value_width - 64.0f * unit);
      const float text_width = body_font->CalcTextSizeA(14.0f * unit, FLT_MAX, 0.0f, shown.c_str()).x;
      AddTextMiddleLeft(draw_list, body_font, 14.0f * unit, ImVec2(center_x - text_width * 0.5f, middle - (fraction >= 0 ? 3.0f * unit : 0.0f)),
                        value_color, shown.c_str());
      if (fraction >= 0.0f) {
        const float bar_left = value_min.x + 36.0f * unit;
        const float bar_right = row_max.x - 36.0f * unit;
        const float bar_y = row_max.y - 6.0f * unit;
        draw_list->AddLine(ImVec2(bar_left, bar_y), ImVec2(bar_right, bar_y), SteelLine(a(255)), 2.0f * unit);
        draw_list->AddLine(ImVec2(bar_left, bar_y), ImVec2(bar_left + (bar_right - bar_left) * std::clamp(fraction, 0.0f, 1.0f), bar_y),
                           focused ? Imulsion(a(255)) : TextDim(a(255)), 2.0f * unit);
      }
    }
    // A change that applies at the next start: a tag before the value.
    if (menu::PcOptionPendingRestart(option)) {
      const float tag_size = 10.0f * unit;
      const float tag_width = title_font->CalcTextSizeA(tag_size, FLT_MAX, 0.0f, texts.restart).x;
      const ImVec2 tag_min(value_min.x - tag_width - 22.0f * unit, middle - 8.0f * unit);
      const ImVec2 tag_max(value_min.x - 8.0f * unit, middle + 8.0f * unit);
      draw_list->AddRect(tag_min, tag_max, Imulsion(a(200)), 3.0f * unit, 0, 1.0f * unit);
      AddTextMiddleLeft(draw_list, title_font, tag_size, ImVec2(tag_min.x + 7.0f * unit, middle), Imulsion(a(255)),
                        texts.restart);
    }
  }
  draw_list->PopClipRect();
  // More rows above or below: a fade at that edge of the list.
  if (scroll_ > 0.5f) {
    draw_list->AddRectFilledMultiColor(ImVec2(list_left, list_top), ImVec2(list_right, list_top + 14.0f * unit),
                                       Panel(a(240)), Panel(a(240)), Panel(0), Panel(0));
  }
  if (scroll_ < content - view_height - 0.5f) {
    draw_list->AddRectFilledMultiColor(ImVec2(list_left, list_bottom - 14.0f * unit), ImVec2(list_right, list_bottom),
                                       Panel(0), Panel(0), Panel(a(240)), Panel(a(240)));
  }

  // The description of the focused row; for a row the preset decides, how to take it over.
  draw_list->AddLine(ImVec2(list_left, list_bottom + 6.0f * unit), ImVec2(list_right, list_bottom + 6.0f * unit),
                     SteelLine(a(255)), 1.0f * unit);
  {
    const int option = OptionOf(focus_);
    std::string help = menu::PcOptionDescribe(option).help;
    float preset_value = 0.0f;
    if (menu::PcOptionDecidedByPreset(option, preset_value)) {
      const int preset = menu::PcOptionByCvar("upscaling_preset");
      const menu::PcOptionInfo preset_info = menu::PcOptionDescribe(preset);
      const std::string custom = preset_info.values.empty() ? std::string("Custom") : preset_info.values[0];
      char line[512];
      std::snprintf(line, sizeof(line), texts.by_preset, custom.c_str());
      help = std::string(line) + " " + help;
    }
    draw_list->AddText(body_font, 13.0f * unit, ImVec2(list_left, list_bottom + 14.0f * unit), TextDim(a(255)),
                       help.c_str(), nullptr, list_right - list_left);
  }

  // Button hints: the confirm button changes the focused row, the back button closes, as the device
  // last used shows them (or Enter and Escape on the keyboard).
  const rex::input::DeviceInfo* pad = [&]() -> const rex::input::DeviceInfo* {
    if (last_input_.keyboard()) return nullptr;
    for (const Slot& slot : slots) {
      if (!slot.keyboard && slot.device.id == last_input_.pad()) return &slot.device;
    }
    for (const Slot& slot : slots) {
      if (!slot.keyboard && slot.player == 0) return &slot.device;
    }
    return nullptr;
  }();
  const float hints_y = panel_max.y - 30.0f * unit;
  float x = panel_min.x + margin;
  x += DrawConfirmGlyph(draw_list, title_font, ImVec2(x, hints_y), unit, pad, texts.enter, a(255)) + 6.0f * unit;
  AddTextMiddleLeft(draw_list, body_font, 13.0f * unit, ImVec2(x, hints_y + 8.0f * unit), Text(a(255)), texts.change);
  x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, texts.change).x + 26.0f * unit;
  x += DrawCancelGlyph(draw_list, title_font, ImVec2(x, hints_y), unit, pad, "Esc", a(255)) + 6.0f * unit;
  AddTextMiddleLeft(draw_list, body_font, 13.0f * unit, ImVec2(x, hints_y + 8.0f * unit), Text(a(255)), texts.back);
  x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, texts.back).x + 26.0f * unit;
  // Defaults: X on a controller, R on the keyboard (a key cap, as Enter's); once pressed, gold and
  // asking for a second press.
  const bool confirming = now < confirm_defaults_until_;
  const float defaults_left = x;
  if (pad) {
    x += DrawPadGlyph(draw_list, title_font, ImVec2(x + 8.0f * unit, hints_y + 8.0f * unit), 16.0f * unit,
                      LookOf(*pad), rex::input::PadControl::kX, a(255), unit);
  } else {
    x += DrawConfirmGlyph(draw_list, title_font, ImVec2(x, hints_y), unit, nullptr, "R", a(255));
  }
  x += 6.0f * unit;
  const char* defaults_text = confirming ? texts.confirm : texts.defaults;
  AddTextMiddleLeft(draw_list, body_font, 13.0f * unit, ImVec2(x, hints_y + 8.0f * unit),
                    confirming ? Imulsion(a(255)) : Text(a(255)), defaults_text);
  x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, defaults_text).x;
  const bool defaults_clicked =
      clicked && Inside(mouse, ImVec2(defaults_left, hints_y - 4.0f * unit), ImVec2(x, hints_y + 20.0f * unit));

  // A click acts once the rows are drawn, so the row it changes is the one under the mouse.
  if (defaults_clicked) {
    last_input_.NoteKeyboard();
    Defaults();
  }
  if (clicked_row >= 0) {
    last_input_.NoteKeyboard();
    Change(clicked_row, clicked_direction);
  }
}

void GraphicsMenu::OnClose() {
  if (assignment_) assignment_->SetBlocked(false);
  if (to_calibration_) {
    // Back on this row once the calibration closes (script_hooks.cpp opens the menu again).
    g_reopen_focus = kBrightnessRow;
    script::RequestBrightnessCalibration();
  } else {
    script::PlayMenuSound(script::MenuSound::kClose);
    script::GraphicsMenuClosed();
  }
  if (owner_) *owner_ = nullptr;
}

}  // namespace azura
