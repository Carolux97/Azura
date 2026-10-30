#include "join_by_code_menu.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <vector>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/system/xam/alboria.h>

#include "coop_ui.h"
#include "input/player_assignment.h"
#include "online_room_overlay.h"
#include "script_hooks.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using Slot = input::PlayerAssignment::Slot;
using rex::input::PadControl;
namespace alboria = rex::system::xam::alboria;

// How long the menu takes to fade in.
constexpr float kFadeSeconds = 0.18f;
// How far the left stick goes before it counts as a press in a direction.
constexpr int kStickPress = 20000;
// The codes' alphabet (Crockford's base 32, as the service makes them).
constexpr const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// The on-screen keyboard: the alphabet's characters row by row, then the row of actions.
constexpr const char* kKeyRows[] = {"1234567890", "ABCDEFGHJK", "MNPQRSTVWX", "YZ"};
constexpr int kCharacterRows = 4;
enum class Action { kDelete, kBack, kJoin };
constexpr Action kActions[] = {Action::kDelete, Action::kBack, Action::kJoin};
constexpr int kRowCount = kCharacterRows + 1;

int RowLength(int row) {
  return row < kCharacterRows ? static_cast<int>(std::strlen(kKeyRows[row])) : 3;
}

struct Texts {
  const char* title;
  const char* help;
  const char* erase;
  const char* back;
  const char* join;
  const char* press;
  const char* type;      // the keyboard's hint: type the code
  const char* joining;
  const char* incomplete;
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"PER CODE BEITRETEN", "Gib den Code des Raums ein, den dir der Gastgeber gegeben hat.", "L\xC3\x96SCHEN",
              "ZUR\xC3\x9C""CK", "BEITRETEN", "Dr\xC3\xBC""cken", "Code eintippen", "Raum wird betreten\xE2\x80\xA6",
              "Der Code hat 8 Zeichen"};
    case 4:  // French
      return {"REJOINDRE PAR CODE", "Saisissez le code du salon que l'h\xC3\xB4te vous a donn\xC3\xA9.", "EFFACER",
              "RETOUR", "REJOINDRE", "Appuyer", "Tapez le code", "Connexion au salon\xE2\x80\xA6",
              "Le code a 8 caract\xC3\xA8res"};
    case 5:  // Spanish
      return {"UNIRSE POR C\xC3\x93""DIGO", "Escribe el c\xC3\xB3""digo de la sala que te ha pasado el anfitri\xC3\xB3n.",
              "BORRAR", "VOLVER", "UNIRSE", "Pulsar", "Escribe el c\xC3\xB3""digo", "Entrando en la sala\xE2\x80\xA6",
              "El c\xC3\xB3""digo tiene 8 caracteres"};
    case 6:  // Italian
      return {"UNISCITI CON CODICE", "Inserisci il codice della stanza che ti ha dato l'host.", "CANCELLA", "INDIETRO",
              "UNISCITI", "Premi", "Digita il codice", "Ingresso nella stanza\xE2\x80\xA6", "Il codice ha 8 caratteri"};
    case 9:  // Portuguese
      return {"ENTRAR COM C\xC3\x93""DIGO", "Digite o c\xC3\xB3""digo da sala que o anfitri\xC3\xA3o te passou.",
              "APAGAR", "VOLTAR", "ENTRAR", "Apertar", "Digite o c\xC3\xB3""digo", "Entrando na sala\xE2\x80\xA6",
              "O c\xC3\xB3""digo tem 8 caracteres"};
    case 11:  // Polish
      return {"DO\xC5\x81\xC4\x84""CZ KODEM", "Wpisz kod pokoju, kt\xC3\xB3ry poda\xC5\x82 ci gospodarz.", "USU\xC5\x83",
              "WR\xC3\x93\xC4\x86", "DO\xC5\x81\xC4\x84""CZ", "Naci\xC5\x9Bnij", "Wpisz kod",
              "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6", "Kod ma 8 znak\xC3\xB3w"};
    default:
      return {"JOIN BY CODE", "Type the code of the room the host gave you.", "DELETE", "BACK", "JOIN", "Press",
              "Type the code", "Joining the room\xE2\x80\xA6", "The code has 8 characters"};
  }
}

// The controller button that does an action from anywhere on the keyboard.
PadControl ActionButton(Action action) {
  switch (action) {
    case Action::kDelete: return PadControl::kX;
    case Action::kBack: return PadControl::kB;
    case Action::kJoin: return PadControl::kStart;
  }
  return PadControl::kA;
}

const char* ActionLabel(const Texts& texts, Action action) {
  switch (action) {
    case Action::kDelete: return texts.erase;
    case Action::kBack: return texts.back;
    case Action::kJoin: return texts.join;
  }
  return "";
}

}  // namespace

JoinByCodeMenu::JoinByCodeMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                               input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                               ImFont* body_font, JoinByCodeMenu** owner)
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
}

void JoinByCodeMenu::MoveFocus(int rows, int columns) {
  if (rows) {
    // Keeps about the same place across rows of other lengths.
    const float place = (focus_column_ + 0.5f) / RowLength(focus_row_);
    focus_row_ = std::clamp(focus_row_ + rows, 0, kRowCount - 1);
    focus_column_ = std::clamp(static_cast<int>(place * RowLength(focus_row_)), 0, RowLength(focus_row_) - 1);
  }
  focus_column_ = std::clamp(focus_column_ + columns, 0, RowLength(focus_row_) - 1);
}

void JoinByCodeMenu::PressFocused() {
  if (focus_row_ < kCharacterRows) {
    Type(kKeyRows[focus_row_][focus_column_]);
    return;
  }
  switch (kActions[focus_column_]) {
    case Action::kDelete:
      if (!code_.empty()) code_.pop_back();
      error_.clear();
      break;
    case Action::kBack:
      going_back_ = true;
      closing_ = true;
      break;
    case Action::kJoin:
      Join();
      break;
  }
}

void JoinByCodeMenu::Type(char c) {
  const std::string normalized = alboria::NormalizeCode(std::string(1, c));
  if (normalized.size() != 1 || !std::strchr(kAlphabet, normalized[0]) || code_.size() >= kCodeLength) return;
  code_ += normalized[0];
  error_.clear();
}

void JoinByCodeMenu::Paste(const char* text) {
  if (!text) return;
  code_.clear();
  for (char c : alboria::NormalizeCode(text)) {
    if (code_.size() < kCodeLength && std::strchr(kAlphabet, c)) code_ += c;
  }
  error_.clear();
}

void JoinByCodeMenu::Join() {
  if (!alboria::IsValidCode(code_)) {
    error_ = LocalizedTexts(language_).incomplete;
    return;
  }
  error_.clear();
  joining_ = true;
  alboria::JoinGame(code_);
}

bool JoinByCodeMenu::HandleInput() {
  bool close = false;
  if (assignment_ && input_) {
    for (const Slot& slot : assignment_->Slots()) {
      if (slot.keyboard) continue;
      rex::input::X_INPUT_STATE state = {};
      // 0 is X_ERROR_SUCCESS.
      if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
      uint16_t buttons = state.gamepad.buttons;
      const int stick_x = state.gamepad.thumb_lx;
      const int stick_y = state.gamepad.thumb_ly;
      if (stick_x <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
      if (stick_x >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
      if (stick_y >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
      if (stick_y <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
      auto found = previous_buttons_.find(slot.device.id);
      const bool seen = found != previous_buttons_.end();
      const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
      previous_buttons_[slot.device.id] = buttons;
      if (pressed) last_input_.NotePad(slot.device.id);
      if (pressed & rex::input::X_INPUT_GAMEPAD_B) close = true;
      if (joining_) continue;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) MoveFocus(-1, 0);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) MoveFocus(1, 0);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) MoveFocus(0, -1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) MoveFocus(0, 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_A) PressFocused();
      if ((pressed & rex::input::X_INPUT_GAMEPAD_X) && !code_.empty()) {
        code_.pop_back();
        error_.clear();
      }
      if (pressed & rex::input::X_INPUT_GAMEPAD_START) Join();
    }
  }

  // The keyboard, through ImGui (the game's keyboard input is blocked).
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
  if (joining_) return close;
  ImGuiIO& io = ImGui::GetIO();
  if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
    Paste(ImGui::GetClipboardText());
  } else {
    // The keys themselves: the runtime gives ImGui key presses but no text input.
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) Type(static_cast<char>('A' + (k - ImGuiKey_A)));
    }
    for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) Type(static_cast<char>('0' + (k - ImGuiKey_0)));
    }
    for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) Type(static_cast<char>('0' + (k - ImGuiKey_Keypad0)));
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !code_.empty()) {
    code_.pop_back();
    error_.clear();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) Join();
  return close;
}

void JoinByCodeMenu::OnDraw(ImGuiIO& io) {
  // Closing waits for what closed it to be released (coop_ui::CloseInputHeld).
  if (closing_) {
    // Going back while joining gives the attempt up.
    if (going_back_ && joining_) {
      alboria::Leave();
      joining_ = false;
    }
    std::vector<DeviceId> pads;
    if (assignment_) {
      for (const Slot& slot : assignment_->Slots()) {
        if (!slot.keyboard) pads.push_back(slot.device.id);
      }
    }
    if (!CloseInputHeld(input_, pads)) Close();
    return;
  }
  if (HandleInput()) {
    going_back_ = true;
    closing_ = true;
    return;
  }
  if (joining_) {
    const alboria::Status status = alboria::GetStatus();
    if (status.state == alboria::State::kConnected) {
      // In the room: the System Link browser finds the host's party there and joins it.
      script::JoinSystemLinkPartyInRoom();
      closing_ = true;
      return;
    }
    if (status.state == alboria::State::kFailed || status.state == alboria::State::kOffline) {
      joining_ = false;
      error_ = AlboriaErrorText(language_, status.error);
    }
  }

  const Clock::time_point now = Clock::now();
  const float fade = std::min(1.0f, std::chrono::duration<float>(now - opened_).count() / kFadeSeconds);
  auto a = [&](int alpha) { return static_cast<int>(alpha * fade); };
  // The controller whose glyphs show: the one last pressed in the menu, else player 1's, else
  // any (from a copy of the slots that lives through this frame). None with the keyboard.
  const std::vector<Slot> slots = assignment_ ? assignment_->Slots() : std::vector<Slot>{};
  const rex::input::DeviceInfo* pad = nullptr;
  if (!last_input_.keyboard()) {
    for (const Slot& slot : slots) {
      if (!slot.keyboard && slot.device.id == last_input_.pad()) pad = &slot.device;
    }
    for (const Slot& slot : slots) {
      if (!pad && !slot.keyboard && slot.player == 0) pad = &slot.device;
    }
    for (const Slot& slot : slots) {
      if (!pad && !slot.keyboard) pad = &slot.device;
    }
  }
  const bool pad_input = pad != nullptr;
  const PadLook look = pad ? LookOf(*pad) : PadLook::kGeneric;

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

  // The keyboard's keys, as the launcher's: 46 pixels square at 1080p, so about 31 here.
  const float key = 34.0f * unit;
  const float key_gap = 6.0f * unit;
  const float keys_width = 10 * key + 9 * key_gap;
  const float keys_height = kRowCount * key + (kRowCount - 1) * key_gap;
  const float margin = 30.0f * unit;
  const float panel_width = std::max(520.0f * unit, keys_width + 2.0f * margin);
  const float panel_height = (pad_input ? 250.0f * unit + keys_height + 18.0f * unit : 250.0f * unit);
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  DrawCutPanel(draw_list, panel_min, panel_max, 14.0f * unit, Panel(a(240)), SteelLine(a(255)), 1.0f * unit);
  // The gold line under the title, as the Local co-op menu's.
  draw_list->AddLine(ImVec2(panel_min.x + margin, panel_min.y + 62.0f * unit),
                     ImVec2(panel_max.x - margin, panel_min.y + 62.0f * unit), Imulsion(a(200)), 1.25f * unit);
  draw_list->AddText(title_font, 26.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 20.0f * unit),
                     Imulsion(a(255)), texts.title);
  const std::string help = Fit(body_font, 13.0f * unit, texts.help, panel_width - 2.0f * margin);
  draw_list->AddText(body_font, 13.0f * unit, ImVec2(panel_min.x + margin, panel_min.y + 72.0f * unit),
                     TextDim(a(255)), help.c_str());

  // The eight boxes, a dash between the halves; the next one to fill outlined in gold.
  const float box = 40.0f * unit;
  const float gap = 8.0f * unit;
  const float dash = 22.0f * unit;
  const float row_width = kCodeLength * box + (kCodeLength - 1) * gap + dash;
  float x = panel_min.x + (panel_width - row_width) * 0.5f;
  const float row_top = panel_min.y + 104.0f * unit;
  for (size_t i = 0; i < kCodeLength; ++i) {
    if (i == kCodeLength / 2) {
      draw_list->AddLine(ImVec2(x + 3.0f * unit, row_top + box * 0.5f),
                         ImVec2(x + dash - 3.0f * unit - gap, row_top + box * 0.5f), TextDim(a(255)), 2.0f * unit);
      x += dash;
    }
    const bool next = i == code_.size() && !joining_;
    DrawCutPanel(draw_list, ImVec2(x, row_top), ImVec2(x + box, row_top + box), 5.0f * unit, Steel(a(255)),
                 next ? Imulsion(a(255)) : SteelLine(a(255)), (next ? 1.8f : 1.0f) * unit);
    if (i < code_.size()) {
      const char text[2] = {code_[i], '\0'};
      AddTextCentered(draw_list, title_font, 22.0f * unit, ImVec2(x + box * 0.5f, row_top + box * 0.5f), Text(a(255)), text);
    }
    x += box + gap;
  }

  // The state: joining, or why the last attempt did not work.
  const float status_y = row_top + box + 16.0f * unit;
  if (joining_) {
    draw_list->AddText(body_font, 13.0f * unit, ImVec2(panel_min.x + margin, status_y), Text(a(255)), texts.joining);
  } else if (!error_.empty()) {
    draw_list->AddText(body_font, 13.0f * unit, ImVec2(panel_min.x + margin, status_y), ErrorText(a(255)), error_.c_str());
  }

  // The on-screen keyboard, with a controller.
  if (pad_input) {
    const float keys_top = status_y + 30.0f * unit;
    for (int row = 0; row < kRowCount; ++row) {
      const int length = RowLength(row);
      const bool actions = row == kCharacterRows;
      // The action keys share the row's width; the character keys are square.
      const float width = actions ? (keys_width - 2 * key_gap) / 3.0f : key;
      const float total = length * width + (length - 1) * key_gap;
      float key_x = panel_min.x + (panel_width - total) * 0.5f;
      const float key_y = keys_top + row * (key + key_gap);
      for (int column = 0; column < length; ++column) {
        const bool focused = row == focus_row_ && column == focus_column_ && !joining_;
        const ImVec2 key_min(key_x, key_y);
        const ImVec2 key_max(key_x + width, key_y + key);
        draw_list->AddRectFilled(key_min, key_max, focused ? Imulsion(a(255)) : Steel(a(255)), 4.0f * unit);
        draw_list->AddRect(key_min, key_max, focused ? Imulsion(a(255)) : SteelLine(a(255)), 4.0f * unit, 0, 1.0f * unit);
        const ImVec2 center((key_min.x + key_max.x) * 0.5f, (key_min.y + key_max.y) * 0.5f);
        const ImU32 color = focused ? Panel(a(255)) : Text(a(255));
        if (actions) {
          // The action's name, with the glyph of the button that does it from anywhere.
          const char* label = ActionLabel(texts, kActions[column]);
          const float glyph = 18.0f * unit;
          const float label_width = title_font->CalcTextSizeA(14.0f * unit, FLT_MAX, 0.0f, label).x;
          const float glyph_width = glyph * (kActions[column] == Action::kJoin ? 2.4f : 1.0f);
          const float start = center.x - (glyph_width + 6.0f * unit + label_width) * 0.5f;
          DrawPadGlyph(draw_list, title_font, ImVec2(start + glyph_width * 0.5f, center.y), glyph, look,
                       ActionButton(kActions[column]), a(255), unit);
          AddTextCentered(draw_list, title_font, 14.0f * unit,
                          ImVec2(start + glyph_width + 6.0f * unit + label_width * 0.5f, center.y), color, label);
        } else {
          const char text[2] = {kKeyRows[row][column], '\0'};
          AddTextCentered(draw_list, title_font, 18.0f * unit, center, color, text);
        }
        key_x += width + key_gap;
      }
    }
  }

  // Button hints along the bottom, with the glyphs of the input used last: the controller's, or
  // the keys.
  const float hints_y = panel_max.y - 30.0f * unit;
  x = panel_min.x + margin;
  auto hint = [&](PadControl control, const char* key, const char* text) {
    if (pad) {
      x += DrawPadGlyph(draw_list, title_font, ImVec2(x + 9.0f * unit, hints_y + 8.0f * unit), 18.0f * unit, look,
                        control, a(255), unit) +
           6.0f * unit;
    } else {
      x += DrawConfirmGlyph(draw_list, title_font, ImVec2(x, hints_y), unit, nullptr, key, a(255)) + 6.0f * unit;
    }
    draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), Text(a(255)), text);
    x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, text).x + 20.0f * unit;
  };
  if (!joining_) {
    if (pad) {
      hint(PadControl::kA, nullptr, texts.press);
    } else {
      draw_list->AddText(body_font, 13.0f * unit, ImVec2(x, hints_y), TextDim(a(255)), texts.type);
      x += body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, texts.type).x + 20.0f * unit;
      hint(PadControl::kStart, "Enter", texts.join);
      hint(PadControl::kX, "Backspace", texts.erase);
    }
  }
  hint(PadControl::kB, "Esc", texts.back);
}

void JoinByCodeMenu::OnClose() {
  if (assignment_) assignment_->SetBlocked(false);
  if (owner_) *owner_ = nullptr;
}

}  // namespace azura
