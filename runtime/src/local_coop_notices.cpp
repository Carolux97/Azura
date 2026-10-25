#include "local_coop_notices.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <optional>
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
using rex::input::DeviceInfo;
using Slot = input::PlayerAssignment::Slot;

// How long the connected notice stays, and how long it takes to fade in and out.
constexpr float kConnectedSeconds = 4.0f;
constexpr float kFadeSeconds = 0.25f;

struct Texts {
  const char* disconnected;
  // printf format with the player's number as %d.
  const char* player;
  // The reconnect line, before and after the confirm button.
  const char* reconnect;
  const char* reconnect_tail;
  const char* connected;
  const char* assign;
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"CONTROLLER GETRENNT", "SPIELER %d", "Verbinde den Controller erneut oder drücke",
              "auf einem anderen Controller, um ihn zu verwenden.", "CONTROLLER VERBUNDEN",
              "Weise ihn unter Optionen > Lokaler Koop einem Spieler zu."};
    case 4:  // French
      return {"MANETTE DÉCONNECTÉE", "JOUEUR %d", "Reconnectez la manette ou appuyez sur",
              "sur une autre manette pour l'utiliser.", "MANETTE CONNECTÉE",
              "Attribuez-la à un joueur dans Options > Coopération locale."};
    case 5:  // Spanish
      return {"MANDO DESCONECTADO", "JUGADOR %d", "Vuelva a conectar el mando o pulse",
              "en otro mando para usarlo.", "MANDO CONECTADO",
              "Asígnelo a un jugador en Opciones > Cooperativo local."};
    case 6:  // Italian
      return {"CONTROLLER DISCONNESSO", "GIOCATORE %d", "Ricollega il controller o premi",
              "su un altro controller per usarlo.", "CONTROLLER CONNESSO",
              "Assegnalo a un giocatore in Opzioni > Cooperativa locale."};
    case 9:  // Portuguese
      return {"CONTROLE DESCONECTADO", "JOGADOR %d", "Reconecte o controle ou pressione", "em outro controle para usá-lo.",
              "CONTROLE CONECTADO", "Atribua-o a um jogador em Opções > Cooperativo local."};
    case 11:  // Polish
      return {"KONTROLER ODŁĄCZONY", "GRACZ %d", "Podłącz ponownie kontroler lub naciśnij",
              "na innym kontrolerze, aby go użyć.", "KONTROLER PODŁĄCZONY",
              "Przypisz go graczowi w Opcje > Lokalna kooperacja."};
    default:
      return {"CONTROLLER DISCONNECTED", "PLAYER %d", "Reconnect the controller, or press",
              "on another controller to use it.", "CONTROLLER CONNECTED",
              "Give it to a player in Options > Local co-op."};
  }
}

// A device's icon, `size` tall, centred on `center`.
void DrawDeviceIcon(ImDrawList* draw_list, ImVec2 center, float size, const DeviceInfo& device, int alpha) {
  if (device.synthetic) {
    DrawKeyboardIcon(draw_list, center, size, IM_COL32(200, 196, 188, alpha), IM_COL32(48, 52, 58, alpha));
  } else {
    DrawControllerIcon(draw_list, center, size, LookOf(device), alpha);
  }
}

}  // namespace

LocalCoopNotices::LocalCoopNotices(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                                   input::PlayerAssignment* assignment, uint32_t language, std::string gamertag,
                                   ImFont* title_font, ImFont* body_font)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      gamertag_(std::move(gamertag)),
      title_font_(title_font),
      body_font_(body_font) {}

void LocalCoopNotices::TakeOverWithUnassigned(int player) {
  for (const Slot& slot : assignment_->Slots()) {
    if (slot.keyboard || slot.player != input::PlayerAssignment::kUnassigned) {
      continue;
    }
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) {
      continue;
    }
    const uint16_t buttons = state.gamepad.buttons;
    auto found = previous_buttons_.find(slot.device.id);
    const bool seen = found != previous_buttons_.end();
    const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
    previous_buttons_[slot.device.id] = buttons;
    if ((pressed & rex::input::X_INPUT_GAMEPAD_A) && assignment_->Give(slot.device.id, player)) {
      previous_buttons_.clear();
      return;
    }
  }
}

void LocalCoopNotices::OnDraw(ImGuiIO& io) {
  showing_ = false;
  if (!assignment_ || !input_) {
    return;
  }
  const std::vector<Slot> slots = assignment_->Slots();
  const Clock::time_point now = Clock::now();

  // A controller that connects during co-op and plays nobody.
  for (const Slot& slot : slots) {
    if (slot.keyboard || known_.count(slot.device.id)) {
      continue;
    }
    known_.insert(slot.device.id);
    if (known_initialized_ && slot.player == input::PlayerAssignment::kUnassigned) {
      connected_ = slot.device;
      connected_at_ = now;
    }
  }
  known_initialized_ = true;

  if (assignment_->Editing()) {
    previous_buttons_.clear();
    return;
  }

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

  // The reconnect notice of each waiting player, one under the other.
  const uint32_t waiting = assignment_->WaitingPlayers();
  const DeviceInfo* unassigned_pad = nullptr;
  for (const Slot& slot : slots) {
    if (!slot.keyboard && slot.player == input::PlayerAssignment::kUnassigned) {
      unassigned_pad = &slot.device;
      break;
    }
  }
  float top = game_top + 96.0f * unit;
  bool any_waiting = false;
  for (int player = 0; player < static_cast<int>(input::PlayerAssignment::kMaxLocalPlayers); ++player) {
    if (!(waiting & (1u << player))) {
      continue;
    }
    const std::optional<DeviceInfo> lost = assignment_->LostDevice(player);
    if (!lost) {
      continue;
    }
    if (!any_waiting) {
      TakeOverWithUnassigned(player);
    }
    any_waiting = true;
    showing_ = true;
    const float width = 470.0f * unit;
    const float height = 92.0f * unit;
    const ImVec2 min(game_left + (game_width - width) * 0.5f, top);
    const ImVec2 max(min.x + width, min.y + height);
    DrawCutPanel(draw_list, min, max, 12.0f * unit, Panel(235), Imulsion(230), 1.25f * unit);
    DrawDeviceIcon(draw_list, ImVec2(min.x + 46.0f * unit, min.y + height * 0.5f), 30.0f * unit, *lost, 255);
    const float text_left = min.x + 92.0f * unit;
    const float text_width = max.x - 16.0f * unit - text_left;
    draw_list->AddText(title_font, 16.0f * unit, ImVec2(text_left, min.y + 12.0f * unit), Imulsion(255),
                       texts.disconnected);
    char who[64];
    std::snprintf(who, sizeof(who), texts.player, player + 1);
    const std::string line =
        Fit(body_font, 12.5f * unit, std::string(who) + "  ·  " + input::LocalPlayerName(gamertag_, player), text_width);
    draw_list->AddText(body_font, 12.5f * unit, ImVec2(text_left, min.y + 34.0f * unit), Text(255), line.c_str());
    // "Reconnect the controller, or press (A) on another controller to use it."
    float x = text_left;
    const float y = min.y + 58.0f * unit;
    draw_list->AddText(body_font, 12.0f * unit, ImVec2(x, y), TextDim(255), texts.reconnect);
    x += body_font->CalcTextSizeA(12.0f * unit, FLT_MAX, 0.0f, texts.reconnect).x + 5.0f * unit;
    x += DrawConfirmGlyph(draw_list, title_font, ImVec2(x, y - 1.5f * unit), unit,
                          unassigned_pad ? unassigned_pad : &*lost, "", 255) +
         5.0f * unit;
    const std::string tail = Fit(body_font, 12.0f * unit, texts.reconnect_tail, max.x - 16.0f * unit - x);
    draw_list->AddText(body_font, 12.0f * unit, ImVec2(x, y), TextDim(255), tail.c_str());
    top = max.y + 10.0f * unit;
  }
  if (!any_waiting) {
    previous_buttons_.clear();
  }

  // The connected notice, in the top right corner, while it lasts and the controller still plays
  // nobody.
  const float age = std::chrono::duration<float>(now - connected_at_).count();
  if (connected_.id != DeviceId::kInvalid && age < kConnectedSeconds) {
    bool still_unassigned = false;
    for (const Slot& slot : slots) {
      still_unassigned = still_unassigned ||
                         (slot.device.id == connected_.id && slot.player == input::PlayerAssignment::kUnassigned);
    }
    if (still_unassigned) {
      showing_ = true;
      const float fade = std::clamp(std::min(age, kConnectedSeconds - age) / kFadeSeconds, 0.0f, 1.0f);
      auto a = [&](int alpha) { return static_cast<int>(alpha * fade); };
      const float width = 330.0f * unit;
      const float height = 58.0f * unit;
      const ImVec2 max(game_left + game_width - 26.0f * unit, game_top + 26.0f * unit + height);
      const ImVec2 min(max.x - width, max.y - height);
      DrawCutPanel(draw_list, min, max, 10.0f * unit, Panel(a(230)), Imulsion(a(220)), 1.25f * unit);
      DrawDeviceIcon(draw_list, ImVec2(min.x + 34.0f * unit, min.y + height * 0.5f), 22.0f * unit, connected_, a(255));
      const float text_left = min.x + 70.0f * unit;
      const float text_width = max.x - 12.0f * unit - text_left;
      const std::string title =
          Fit(title_font, 12.5f * unit, std::string(texts.connected) + "  ·  " + connected_.name, text_width);
      draw_list->AddText(title_font, 12.5f * unit, ImVec2(text_left, min.y + 11.0f * unit), Imulsion(a(255)),
                         title.c_str());
      const std::string assign = Fit(body_font, 11.5f * unit, texts.assign, text_width);
      draw_list->AddText(body_font, 11.5f * unit, ImVec2(text_left, min.y + 32.0f * unit), Text(a(255)),
                         assign.c_str());
    }
  }
}

}  // namespace azura
