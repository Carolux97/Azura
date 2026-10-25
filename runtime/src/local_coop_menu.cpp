#include "local_coop_menu.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>

#include "coop_ui.h"
#include "input/player_assignment.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using Slot = input::PlayerAssignment::Slot;

// How long the menu takes to fade in, and how long a moved device glows.
constexpr float kFadeSeconds = 0.18f;
constexpr float kGlowSeconds = 0.6f;
// How far the left stick goes before it counts as a press left or right.
constexpr int kStickPress = 20000;

struct Texts {
  const char* title;
  const char* help;
  const char* unassigned;
  // printf format with the player's number as %d.
  const char* player;
  const char* keyboard;
  const char* nobody;  // an empty column
  const char* move;
  const char* done;
  const char* enter;  // the Enter key, on the keyboard's hint
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"LOKALER KOOP",
              "Dr\xC3\xBC""cke links oder rechts auf einem Ger\xC3\xA4t, um es einem anderen Spieler zu geben.",
              "NICHT ZUGEWIESEN", "SPIELER %d", "Tastatur und Maus", "Kein Ger\xC3\xA4t", "Verschieben",
              "Fertig", "Eingabe"};
    case 4:  // French
      return {"COOP\xC3\x89RATION LOCALE",
              "Appuyez \xC3\xA0 gauche ou \xC3\xA0 droite sur un appareil pour le donner \xC3\xA0 un autre joueur.",
              "NON ASSIGN\xC3\x89", "JOUEUR %d", "Clavier et souris", "Aucun appareil", "D\xC3\xA9placer",
              "Termin\xC3\xA9", "Entr\xC3\xA9""e"};
    case 5:  // Spanish
      return {"COOPERATIVO LOCAL",
              "Pulse izquierda o derecha en un dispositivo para pas\xC3\xA1rselo a otro jugador.",
              "SIN ASIGNAR", "JUGADOR %d", "Teclado y rat\xC3\xB3n", "Sin dispositivo", "Mover", "Listo", "Intro"};
    case 6:  // Italian
      return {"COOPERATIVA LOCALE",
              "Premi sinistra o destra su un dispositivo per darlo a un altro giocatore.",
              "NON ASSEGNATO", "GIOCATORE %d", "Tastiera e mouse", "Nessun dispositivo", "Sposta", "Fatto", "Invio"};
    case 9:  // Portuguese
      return {"COOPERATIVO LOCAL",
              "Pressione esquerda ou direita num dispositivo para pass\xC3\xA1-lo a outro jogador.",
              "N\xC3\x83O ATRIBU\xC3\x8D""DO", "JOGADOR %d", "Teclado e mouse", "Nenhum dispositivo", "Mover",
              "Pronto", "Enter"};
    case 11:  // Polish
      return {"LOKALNA KOOPERACJA",
              "Naci\xC5\x9Bnij w lewo lub w prawo na urz\xC4\x85""dzeniu, aby da\xC4\x87 je innemu graczowi.",
              "NIEPRZYPISANE", "GRACZ %d", "Klawiatura i mysz", "Brak urz\xC4\x85""dzenia", "Przenie\xC5\x9B",
              "Gotowe", "Enter"};
    default:
      return {"LOCAL CO-OP", "Press left or right on a device to give it to another player.", "UNASSIGNED",
              "PLAYER %d", "Keyboard and mouse", "No device", "Move", "Done", "Enter"};
  }
}

}  // namespace

LocalCoopMenu::LocalCoopMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                             input::PlayerAssignment* assignment, uint32_t language, std::string gamertag,
                             ImFont* title_font, ImFont* body_font, LocalCoopMenu** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      gamertag_(std::move(gamertag)),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner),
      opened_(Clock::now()) {
  if (assignment_) {
    assignment_->BeginEditing();
    assignment_->SetBlocked(true);
  }
}

bool LocalCoopMenu::HandleInput() {
  bool close = false;
  for (const Slot& slot : assignment_->Slots()) {
    if (slot.keyboard) {
      continue;
    }
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) {
      continue;
    }
    uint16_t buttons = state.gamepad.buttons;
    const int stick = state.gamepad.thumb_lx;
    if (stick <= -kStickPress) {
      buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
    } else if (stick >= kStickPress) {
      buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
    }
    auto found = previous_buttons_.find(slot.device.id);
    const bool seen = found != previous_buttons_.end();
    const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
    previous_buttons_[slot.device.id] = buttons;
    const int direction = (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT)    ? -1
                          : (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) ? 1
                                                                                : 0;
    if (direction && assignment_->Move(slot.device.id, direction)) {
      moved_ = slot.device.id;
      moved_at_ = Clock::now();
    }
    if (pressed & (rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_START)) {
      close = true;
    }
  }
  // The keyboard and mouse, through ImGui's keys (the game's keyboard input is blocked).
  for (const Slot& slot : assignment_->Slots()) {
    if (!slot.keyboard) {
      continue;
    }
    int direction = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_A, false)) {
      direction = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_D, false)) {
      direction = 1;
    }
    if (direction && assignment_->Move(slot.device.id, direction)) {
      moved_ = slot.device.id;
      moved_at_ = Clock::now();
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    close = true;
  }
  return close;
}

void LocalCoopMenu::OnDraw(ImGuiIO& io) {
  if (!assignment_ || !input_) {
    Close();
    return;
  }
  if (HandleInput()) {
    Close();
    return;
  }

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

  // The game dimmed behind the menu.
  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, a(150)));

  // Columns: nobody, then players 1 to AvailablePlayers().
  const std::vector<Slot> slots = assignment_->Slots();
  const int players = static_cast<int>(assignment_->AvailablePlayers());
  const int columns = players + 1;
  const float column_width = 176.0f * unit;
  const float gap = 12.0f * unit;
  const float margin = 30.0f * unit;
  const float panel_width = columns * column_width + (columns - 1) * gap + 2.0f * margin;
  const float panel_height = 382.0f * unit;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  DrawCutPanel(draw_list, panel_min, panel_max, 14.0f * unit, Panel(a(240)), SteelLine(a(255)), 1.0f * unit);
  // The gold line under the title, as the achievement notice's.
  draw_list->AddLine(ImVec2(panel_min.x + margin, panel_min.y + 62.0f * unit),
                     ImVec2(panel_max.x - margin, panel_min.y + 62.0f * unit), Imulsion(a(200)), 1.25f * unit);

  draw_list->AddText(title_font, 26.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 20.0f * unit),
                     Imulsion(a(255)), texts.title);
  const std::string help = Fit(body_font, 13.0f * unit, texts.help, panel_width - 2.0f * margin);
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 72.0f * unit),
                     TextDim(a(255)), help.c_str());

  const float columns_top = panel_min.y + 102.0f * unit;
  const float header_height = 48.0f * unit;
  const float card_height = 50.0f * unit;
  const float card_gap = 8.0f * unit;
  const float glow = moved_ != DeviceId::kInvalid
                         ? std::max(0.0f, 1.0f - std::chrono::duration<float>(now - moved_at_).count() / kGlowSeconds)
                         : 0.0f;
  for (int column = 0; column < columns; ++column) {
    const int player = column - 1;  // kUnassigned for the first column
    const float left = panel_min.x + margin + column * (column_width + gap);
    const float right = left + column_width;

    // Header: the column's title and, for a player, their name.
    const ImVec2 header_min(left, columns_top);
    const ImVec2 header_max(right, columns_top + header_height);
    DrawCutPanel(draw_list, header_min, header_max, 8.0f * unit, Steel(a(255)), SteelLine(a(255)), 1.0f * unit);
    char title[48];
    if (player == input::PlayerAssignment::kUnassigned) {
      std::snprintf(title, sizeof(title), "%s", texts.unassigned);
    } else {
      std::snprintf(title, sizeof(title), texts.player, player + 1);
    }
    draw_list->AddText(title_font, 14.0f * unit, ImVec2(left + 12.0f * unit, columns_top + 8.0f * unit),
                       player == input::PlayerAssignment::kUnassigned ? TextDim(a(255)) : Imulsion(a(255)), title);
    if (player >= 0) {
      const std::string name = Fit(body_font, 12.5f * unit,
                                   input::LocalPlayerName(gamertag_, static_cast<uint32_t>(player)),
                                   column_width - 24.0f * unit);
      draw_list->AddText(body_font, 12.5f * unit, ImVec2(left + 12.0f * unit, columns_top + 27.0f * unit),
                         Text(a(255)), name.c_str());
    }

    // The column's devices, one card each.
    float card_top = header_max.y + 10.0f * unit;
    bool any = false;
    for (const Slot& slot : slots) {
      if (slot.player != player) {
        continue;
      }
      any = true;
      const ImVec2 card_min(left, card_top);
      const ImVec2 card_max(right, card_top + card_height);
      const bool glowing = slot.device.id == moved_ && glow > 0.0f;
      DrawCutPanel(draw_list, card_min, card_max, 6.0f * unit, PanelSolid(a(255)),
                   glowing ? Imulsion(a(static_cast<int>(120 + 135 * glow))) : SteelLine(a(255)),
                   (glowing ? 1.6f : 1.0f) * unit);
      const ImVec2 icon_center(left + 32.0f * unit, card_top + card_height * 0.5f);
      const ImU32 body = IM_COL32(200, 196, 188, a(255));
      const ImU32 detail = IM_COL32(48, 52, 58, a(255));
      if (slot.keyboard) {
        DrawKeyboardIcon(draw_list, icon_center, 26.0f * unit, body, detail);
      } else {
        DrawControllerIcon(draw_list, icon_center, 26.0f * unit, LookOf(slot.device), a(255));
      }
      const std::string name = Fit(body_font, 12.5f * unit, slot.keyboard ? texts.keyboard : slot.device.name,
                                   column_width - 70.0f * unit);
      draw_list->AddText(body_font, 12.5f * unit, ImVec2(left + 62.0f * unit, card_top + card_height * 0.5f - 8.0f * unit),
                         Text(a(255)), name.c_str());
      card_top += card_height + card_gap;
    }
    if (!any) {
      draw_list->AddText(body_font, 12.0f * unit, ImVec2(left + 12.0f * unit, card_top + 6.0f * unit),
                         TextFaint(a(255)), texts.nobody);
    }
  }

  // Button hints along the bottom: left and right move, and the button that closes the menu as
  // player 1's device shows it: their controller's (the cross on PlayStation, A on the others), or
  // Enter when they only have the keyboard.
  const float hints_y = panel_max.y - 30.0f * unit;
  const Slot* player_one_pad = nullptr;
  for (const Slot& slot : slots) {
    if (slot.player == 0 && !slot.keyboard) {
      player_one_pad = &slot;
      break;
    }
  }
  float x = panel_min.x + margin;
  DrawArrow(draw_list, ImVec2(x + 5.0f * unit, hints_y + 8.0f * unit), 10.0f * unit, true, Imulsion(a(255)));
  DrawArrow(draw_list, ImVec2(x + 19.0f * unit, hints_y + 8.0f * unit), 10.0f * unit, false, Imulsion(a(255)));
  x += 30.0f * unit;
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), Text(a(255)), texts.move);
  x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, texts.move).x + 26.0f * unit;
  x += DrawConfirmGlyph(draw_list, title_font, ImVec2(x, hints_y), unit,
                        player_one_pad ? &player_one_pad->device : nullptr, texts.enter, a(255)) +
       6.0f * unit;
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), Text(a(255)), texts.done);
}

void LocalCoopMenu::OnClose() {
  if (assignment_) {
    assignment_->SetBlocked(false);
    assignment_->EndEditing();
  }
  if (owner_) {
    *owner_ = nullptr;
  }
}

}  // namespace azura
