#include "connection_menu.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/system/xam/alboria.h>

#include "coop_ui.h"
#include "input/player_assignment.h"
#include "menu/pc_options.h"

REXCVAR_DECLARE(bool, alboria_direct);
REXCVAR_DECLARE(bool, latency_overlay);

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using Slot = input::PlayerAssignment::Slot;
namespace alboria = rex::system::xam::alboria;

constexpr float kFadeSeconds = 0.18f;
// How far the left stick goes before it counts as a press.
constexpr int kStickPress = 20000;
constexpr int kRows = 2;

struct Texts {
  const char* title;
  const char* account;    // the label before Gamertag#tag
  const char* signing_in;
  const char* offline;    // no answer from Alboria
  const char* direct;
  const char* direct_help;
  const char* latency;
  const char* latency_help;
  const char* on;
  const char* off;
  const char* change;
  const char* back;
  const char* enter;  // the Enter key, on the keyboard's hint
  const char* locked;  // why Direct connection cannot change now
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"ALBORIA", "KONTO", "Anmeldung l\xC3\xA4uft\xE2\x80\xA6", "Keine Verbindung zu Alboria",
              "DIREKTE VERBINDUNG",
              "Verbindet dich direkt mit den anderen Spielern des Raums, wenn das Netzwerk es zul\xC3\xA4sst, mit "
              "dem Relay als Ersatz. Weniger Verz\xC3\xB6gerung, aber die anderen Spieler k\xC3\xB6nnen die "
              "\xC3\xB6""ffentliche IP-Adresse dieses Computers erfahren. Gilt ab dem n\xC3\xA4""chsten Raum.",
              "LATENZ ANZEIGEN",
              "Zeigt in einem Online-Raum die Verz\xC3\xB6gerung zum Host in der Ecke der Leistungsanzeige, und "
              "ob die Verbindung direkt ist oder \xC3\xBC""ber das Relay l\xC3\xA4uft.",
              "Ein", "Aus", "\xC3\x84ndern", "Zur\xC3\xBC""ck", "Eingabe",
              "Kann nicht ge\xC3\xA4ndert werden, solange Spieler im Raum sind"};
    case 4:  // French
      return {"ALBORIA", "COMPTE", "Connexion en cours\xE2\x80\xA6", "Impossible de joindre Alboria",
              "CONNEXION DIRECTE",
              "Vous relie directement aux autres joueurs du salon quand le r\xC3\xA9seau le permet, avec le relais "
              "en secours. Moins de latence, mais les autres joueurs peuvent conna\xC3\xAEtre l'adresse IP "
              "publique de cet ordinateur. S'applique au prochain salon.",
              "AFFICHER LA LATENCE",
              "Dans un salon en ligne, affiche le d\xC3\xA9lai avec l'h\xC3\xB4te dans le coin de l'indicateur de "
              "performances, et si la connexion est directe ou passe par le relais.",
              "Activ\xC3\xA9", "D\xC3\xA9sactiv\xC3\xA9", "Changer", "Retour", "Entr\xC3\xA9""e",
              "Impossible \xC3\xA0 changer tant que des joueurs sont dans le salon"};
    case 5:  // Spanish
      return {"ALBORIA", "CUENTA", "Iniciando sesi\xC3\xB3n\xE2\x80\xA6", "No se puede conectar con Alboria",
              "CONEXI\xC3\x93N DIRECTA",
              "Le conecta directamente con los dem\xC3\xA1s jugadores de la sala cuando la red lo permite, con el "
              "relay como respaldo. Menos retardo, pero los otros jugadores pueden conocer la IP p\xC3\xBA""blica "
              "de este equipo. Se aplica a partir de la pr\xC3\xB3xima sala.",
              "MOSTRAR LATENCIA",
              "En una sala online, muestra el retardo con el anfitri\xC3\xB3n en la esquina del indicador de "
              "rendimiento, y si la conexi\xC3\xB3n es directa o pasa por el relay.",
              "Activada", "Desactivada", "Cambiar", "Volver", "Intro",
              "No se puede cambiar con jugadores en la sala"};
    case 6:  // Italian
      return {"ALBORIA", "ACCOUNT", "Accesso in corso\xE2\x80\xA6", "Impossibile raggiungere Alboria",
              "CONNESSIONE DIRETTA",
              "Ti collega direttamente agli altri giocatori della stanza quando la rete lo permette, con il relay "
              "come riserva. Meno ritardo, ma gli altri giocatori possono conoscere l'indirizzo IP pubblico di "
              "questo computer. Vale dalla prossima stanza.",
              "MOSTRA LATENZA",
              "In una stanza online, mostra il ritardo con l'host nell'angolo dell'indicatore delle prestazioni, "
              "e se la connessione \xC3\xA8 diretta o passa dal relay.",
              "Attivata", "Disattivata", "Cambia", "Indietro", "Invio",
              "Non si pu\xC3\xB2 cambiare con giocatori nella stanza"};
    case 9:  // Portuguese
      return {"ALBORIA", "CONTA", "Entrando\xE2\x80\xA6", "N\xC3\xA3o foi poss\xC3\xADvel conectar ao Alboria",
              "CONEX\xC3\x83O DIRETA",
              "Conecta voc\xC3\xAA diretamente aos outros jogadores da sala quando a rede permite, com o relay como "
              "reserva. Menos atraso, mas os outros jogadores podem saber o IP p\xC3\xBA""blico deste computador. "
              "Vale a partir da pr\xC3\xB3xima sala.",
              "MOSTRAR LAT\xC3\x8ANCIA",
              "Numa sala online, mostra o atraso at\xC3\xA9 o anfitri\xC3\xA3o no canto do indicador de desempenho, "
              "e se a conex\xC3\xA3o \xC3\xA9 direta ou passa pelo relay.",
              "Ativada", "Desativada", "Mudar", "Voltar", "Enter",
              "N\xC3\xA3o pode ser mudada com jogadores na sala"};
    case 11:  // Polish
      return {"ALBORIA", "KONTO", "Logowanie\xE2\x80\xA6", "Brak po\xC5\x82\xC4\x85""czenia z Alboria",
              "PO\xC5\x81\xC4\x84""CZENIE BEZPO\xC5\x9AREDNIE",
              "\xC5\x81\xC4\x85""czy ci\xC4\x99 bezpo\xC5\x9Brednio z innymi graczami pokoju, gdy sie\xC4\x87 na to "
              "pozwala, z przeka\xC5\xBAnikiem jako zapasem. Mniejsze op\xC3\xB3\xC5\xBAnienie, ale inni gracze "
              "mog\xC4\x85 pozna\xC4\x87 publiczny adres IP tego komputera. Dzia\xC5\x82""a od nast\xC4\x99pnego pokoju.",
              "POKA\xC5\xBB OP\xC3\x93\xC5\xB9NIENIE",
              "W pokoju online pokazuje op\xC3\xB3\xC5\xBAnienie do gospodarza w rogu wska\xC5\xBAnika wydajno\xC5\x9B""ci "
              "i czy po\xC5\x82\xC4\x85""czenie jest bezpo\xC5\x9Brednie, czy przez przeka\xC5\xBAnik.",
              "W\xC5\x82\xC4\x85""czone", "Wy\xC5\x82\xC4\x85""czone", "Zmie\xC5\x84", "Wr\xC3\xB3\xC4\x87", "Enter",
              "Nie mo\xC5\xBCna zmieni\xC4\x87, gdy w pokoju s\xC4\x85 gracze"};
    default:
      return {"ALBORIA", "ACCOUNT", "Signing in\xE2\x80\xA6", "Cannot reach Alboria",
              "DIRECT CONNECTION",
              "Connects you directly to the other players of the room when the network allows it, with the relay "
              "as fallback. Lower latency, but the other players can learn this computer's public IP address. "
              "Takes effect from the next room.",
              "SHOW LATENCY",
              "In an online room, shows the round trip to the host in the performance overlay's corner, and "
              "whether the connection is direct or goes through the relay.",
              "On", "Off", "Change", "Back", "Enter",
              "Cannot be changed while there are players in the room"};
  }
}

bool RowValue(int row) {
  return row == 0 ? REXCVAR_GET(alboria_direct) : REXCVAR_GET(latency_overlay);
}

// Whether row `row` cannot change now: Direct connection while other players are in the room,
// whose connections were set up with the setting as it was (it applies from the next room).
bool RowLocked(int row) {
  return row == 0 && alboria::HasOtherMembers();
}

// A padlock, `size` tall, its top left at `top_left`.
void DrawPadlock(ImDrawList* draw_list, ImVec2 top_left, float size, ImU32 color) {
  const float body_top = top_left.y + size * 0.45f;
  const float width = size * 0.8f;
  const float line = std::max(1.0f, size * 0.12f);
  draw_list->PathArcTo(ImVec2(top_left.x + width * 0.5f, body_top), width * 0.3f, 3.14159265f, 2.0f * 3.14159265f, 12);
  draw_list->PathStroke(color, 0, line);
  draw_list->AddRectFilled(ImVec2(top_left.x, body_top), ImVec2(top_left.x + width, top_left.y + size), color,
                           size * 0.1f);
}

}  // namespace

ConnectionMenu::ConnectionMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                               input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                               ImFont* body_font, ConnectionMenu** owner)
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
  // The tag only comes from a sign-in: ask now (or reuse the session's), so the menu shows it once
  // Alboria answers.
  alboria::RefreshIdentity();
}

void ConnectionMenu::Toggle(int row) {
  if (RowLocked(row)) return;
  const bool value = !RowValue(row);
  menu::StoreControlsSetting(row == 0 ? "alboria_direct" : "latency_overlay", value ? "true" : "false", false);
}

bool ConnectionMenu::HandleInput() {
  bool close = false;
  int move = 0;
  bool toggle = false;
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
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) move = -1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) move = 1;
    if (pressed & (rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_DPAD_LEFT |
                   rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT)) {
      toggle = true;
    }
    if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_START)) close = true;
  }
  // The keyboard, through ImGui's keys (the game's keyboard input is blocked).
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_W, false)) move = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_S, false)) move = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
      ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
    toggle = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
  if (move) focus_ = (focus_ + move + kRows) % kRows;
  if (toggle && !close) Toggle(focus_);
  return close;
}

void ConnectionMenu::OnDraw(ImGuiIO& io) {
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
  if (HandleInput()) {
    closing_ = true;
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

  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, a(150)));

  const float margin = 30.0f * unit;
  const float panel_width = 620.0f * unit;
  const float panel_height = 430.0f * unit;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  DrawCutPanel(draw_list, panel_min, panel_max, 14.0f * unit, Panel(a(240)), SteelLine(a(255)), 1.0f * unit);
  draw_list->AddText(title_font, 26.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 20.0f * unit),
                     Imulsion(a(255)), texts.title);
  draw_list->AddLine(ImVec2(panel_min.x + margin, panel_min.y + 62.0f * unit),
                     ImVec2(panel_max.x - margin, panel_min.y + 62.0f * unit), Imulsion(a(200)), 1.25f * unit);

  // The account: Gamertag#tag, the tag dimmer; while signing in or without an answer, a line saying so.
  const float account_top = panel_min.y + 76.0f * unit;
  draw_list->AddText(title_font, 12.0f * unit, ImVec2(panel_min.x + margin, account_top), TextFaint(a(255)),
                     texts.account);
  const alboria::Identity identity = alboria::GetIdentity();
  const float name_top = account_top + 18.0f * unit;
  if (identity.known) {
    const std::string name = Fit(title_font, 20.0f * unit, identity.gamertag, panel_width * 0.6f);
    draw_list->AddText(title_font, 20.0f * unit, ImVec2(panel_min.x + margin, name_top), Text(a(255)), name.c_str());
    const float name_width = title_font->CalcTextSizeA(20.0f * unit, FLT_MAX, 0.0f, name.c_str()).x;
    const std::string tag = "#" + std::to_string(identity.tag);
    draw_list->AddText(title_font, 20.0f * unit, ImVec2(panel_min.x + margin + name_width, name_top),
                       Imulsion(a(255)), tag.c_str());
  } else {
    const bool waiting = std::chrono::duration<float>(now - opened_).count() < 10.0f;
    draw_list->AddText(body_font, 14.0f * unit, ImVec2(panel_min.x + margin, name_top + 3.0f * unit),
                       TextDim(a(255)), waiting ? texts.signing_in : texts.offline);
  }

  // The settings, each a card with its name, its value and what it does.
  const char* names[kRows] = {texts.direct, texts.latency};
  const char* helps[kRows] = {texts.direct_help, texts.latency_help};
  const float card_width = panel_width - 2.0f * margin;
  const float card_height = 112.0f * unit;
  float card_top = name_top + 40.0f * unit;
  for (int row = 0; row < kRows; ++row) {
    const ImVec2 card_min(panel_min.x + margin, card_top);
    const ImVec2 card_max(card_min.x + card_width, card_top + card_height);
    const bool focused = row == focus_;
    DrawCutPanel(draw_list, card_min, card_max, 6.0f * unit, PanelSolid(a(255)),
                 focused ? Imulsion(a(230)) : SteelLine(a(255)), (focused ? 1.6f : 1.0f) * unit);
    const float inner = 14.0f * unit;
    draw_list->AddText(title_font, 15.0f * unit, ImVec2(card_min.x + inner, card_top + 10.0f * unit),
                       focused ? Imulsion(a(255)) : Text(a(255)), names[row]);
    // The value, on the right: green when on.
    const bool on = RowValue(row);
    const bool locked = RowLocked(row);
    const char* value = on ? texts.on : texts.off;
    const float value_width = body_font->CalcTextSizeA(14.0f * unit, FLT_MAX, 0.0f, value).x;
    const float value_x = card_max.x - inner - value_width;
    draw_list->AddText(body_font, 14.0f * unit, ImVec2(value_x, card_top + 11.0f * unit),
                       locked ? TextFaint(a(255)) : on ? Ok(a(255)) : TextDim(a(255)), value);
    // Locked: a padlock before the value, and why under the help.
    if (locked) DrawPadlock(draw_list, ImVec2(value_x - 18.0f * unit, card_top + 12.0f * unit), 14.0f * unit, TextDim(a(255)));
    draw_list->AddText(body_font, 12.5f * unit, ImVec2(card_min.x + inner, card_top + 36.0f * unit), TextDim(a(255)),
                       helps[row], nullptr, card_width - 2.0f * inner);
    if (locked) {
      draw_list->AddText(body_font, 12.5f * unit, ImVec2(card_min.x + inner, card_max.y - 22.0f * unit),
                         Imulsion(a(255)), texts.locked);
    }
    card_top += card_height + 12.0f * unit;
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
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), Text(a(255)), texts.change);
  x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, texts.change).x + 26.0f * unit;
  x += DrawCancelGlyph(draw_list, title_font, ImVec2(x, hints_y), unit, pad, "Esc", a(255)) + 6.0f * unit;
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), Text(a(255)), texts.back);
}

void ConnectionMenu::OnClose() {
  if (assignment_) assignment_->SetBlocked(false);
  if (owner_) *owner_ = nullptr;
}

}  // namespace azura
