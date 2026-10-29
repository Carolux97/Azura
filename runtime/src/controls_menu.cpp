#include "controls_menu.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>

#include "coop_ui.h"
#include "input/player_assignment.h"
#include "script_hooks.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::PadControl;

// How long the menu takes to fade in, and how long a wait for a key or a button lasts.
constexpr float kFadeSeconds = 0.18f;
constexpr float kWaitSeconds = 6.0f;
// How far the left stick goes before it counts as a press, and the triggers.
constexpr int kStickPress = 20000;
constexpr int kTriggerPress = 100;
// A text that does not fit its box is drawn smaller, down to this share of its size, then cut.
constexpr float kSmallestText = 0.6f;

// Bits of a controller's state the menu reads: the XInput buttons, and the triggers above them.
constexpr uint32_t kLeftTriggerBit = 1u << 16;
constexpr uint32_t kRightTriggerBit = 1u << 17;

// --- Texts ---------------------------------------------------------------------------------------

struct Texts {
  const char* keyboard_title;
  const char* controls;
  const char* keyboard_help;
  const char* controller_help;
  const char* assignment;
  const char* settings;
  const char* movement;
  const char* weapons;
  const char* combat;
  const char* actions;
  const char* forward;
  const char* backward;
  const char* left;
  const char* right;
  const char* weapon;  // printf format with the weapon's number as %d
  const char* roadie_run;
  const char* wait_key;
  const char* wait_button;
  const char* fixed;
  const char* custom;
  const char* look;
  const char* mouse_sensitivity;
  const char* aim_sensitivity;
  const char* zoom_sensitivity;
  const char* invert_y;
  const char* acceleration;
  const char* aim;
  const char* zoom;
  const char* run;
  const char* hold;
  const char* press;
  const char* off;
  const char* no;
  const char* yes;
  const char* change;
  const char* clear;
  const char* defaults;
  const char* tab;
  const char* close;
  const char* scheme;
  const char* unavailable;
  // Key and mouse names.
  const char* space;
  const char* shift;
  const char* caps;
  const char* left_click;
  const char* right_click;
  const char* middle_click;
  const char* side_1;
  const char* side_2;
  const char* wheel_up;
  const char* wheel_down;
};

// The Xbox 360 language IDs; the fonts draw Cyrillic and, for those languages, Japanese, Korean and
// traditional Chinese behind Barlow (OnConfigureFonts in azura_app.h).
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"キーボードとマウス", "操作設定", "アクションを選んで、キーまたはマウスボタンを押してください。",
              "ゲームの操作タイプ、または自分で設定したもの。", "割り当て", "設定", "移動", "武器", "戦闘",
              "アクション", "前進", "後退", "左", "右", "武器 %d", "ダッシュ（専用キー）",
              "キーまたはマウスボタンを押してください…", "このアクションに使うボタンを押してください…",
              "固定: Esc、Enter、Backspace、矢印キー", "カスタム", "視点", "マウス感度", "エイム時の感度",
              "ズーム時の感度", "Y軸反転", "マウス加速", "エイム", "ズーム", "ダッシュ", "長押し", "押す", "オフ",
              "いいえ", "はい", "変更", "解除", "初期設定", "タブ", "戻る", "操作タイプ", "使用不可", "Space", "Shift",
              "Caps Lock", "左クリック", "右クリック", "中クリック", "サイド1", "サイド2", "ホイール上", "ホイール下"};
    case 7:  // Korean
      return {"키보드 및 마우스", "조작", "동작을 선택하고 키나 마우스 버튼을 누르세요.",
              "게임의 조작 방식 또는 직접 설정한 방식.", "할당", "설정", "이동", "무기", "전투", "동작", "앞으로",
              "뒤로", "왼쪽", "오른쪽", "무기 %d", "질주 (전용 키)", "키나 마우스 버튼을 누르세요...",
              "이 동작에 사용할 버튼을 누르세요...", "고정: Esc, Enter, Backspace, 방향키", "사용자 지정", "시점",
              "마우스 감도", "조준 시 감도", "줌 감도", "Y축 반전", "마우스 가속", "조준", "줌", "질주", "누르고 있기",
              "누르기", "끔", "아니요", "예", "변경", "지우기", "기본값", "탭", "뒤로", "조작 방식", "사용 불가",
              "Space", "Shift", "Caps Lock", "왼쪽 클릭", "오른쪽 클릭", "가운데 클릭", "측면 1", "측면 2", "휠 위로",
              "휠 아래로"};
    case 8:  // Traditional Chinese
      return {"鍵盤與滑鼠", "控制", "選擇一個動作，然後按下按鍵或滑鼠按鈕。", "遊戲的配置，或你自己的配置。",
              "指派", "設定", "移動", "武器", "戰鬥", "動作", "前進", "後退", "左", "右", "武器 %d",
              "衝刺（獨立按鍵）", "請按下按鍵或滑鼠按鈕…", "請按下要用於此動作的按鈕…",
              "固定：Esc、Enter、Backspace 與方向鍵。", "自訂", "視角", "滑鼠靈敏度", "瞄準時靈敏度", "縮放靈敏度",
              "反轉 Y 軸", "滑鼠加速", "瞄準", "縮放", "衝刺", "按住", "按一下", "關閉", "否", "是", "變更", "清除",
              "預設值", "分頁", "返回", "配置", "無法使用", "空白鍵", "Shift", "Caps Lock", "左鍵", "右鍵", "中鍵",
              "側鍵 1", "側鍵 2", "滾輪向上", "滾輪向下"};
    case 12:  // Russian
      return {"КЛАВИАТУРА И МЫШЬ", "УПРАВЛЕНИЕ", "Выберите действие и нажмите клавишу или кнопку мыши.",
              "Схема игры или ваша собственная.", "НАЗНАЧЕНИЕ", "НАСТРОЙКИ", "ДВИЖЕНИЕ", "ОРУЖИЕ", "БОЙ",
              "ДЕЙСТВИЯ", "Вперёд", "Назад", "Влево", "Вправо", "Оружие %d", "Бег пригнувшись (отдельная клавиша)",
              "Нажмите клавишу или кнопку мыши...", "Нажмите нужную кнопку для этого действия...",
              "Фиксированные: Esc, Enter, Backspace и стрелки.", "СВОЯ", "Обзор", "Чувствительность мыши",
              "Чувствительность при прицеливании", "Чувствительность зума", "Инверсия оси Y", "Ускорение мыши",
              "Прицеливание", "Зум", "Бег пригнувшись", "Удержание", "Нажатие", "Выкл.", "Нет", "Да", "Изменить",
              "Очистить", "По умолчанию", "Вкладка", "Назад", "Схема", "Недоступно", "Пробел", "Shift", "Caps Lock",
              "Левый клик", "Правый клик", "Средний клик", "Боковая 1", "Боковая 2", "Колесо вверх", "Колесо вниз"};
    case 3:  // German
      return {"TASTATUR UND MAUS", "STEUERUNG", "W\xC3\xA4hle eine Aktion und dr\xC3\xBC""cke die Taste oder Maustaste.",
              "Das Schema des Spiels oder dein eigenes.", "BELEGUNG", "EINSTELLUNGEN", "BEWEGUNG", "WAFFEN", "KAMPF",
              "AKTIONEN", "Vorw\xC3\xA4rts", "R\xC3\xBC""ckw\xC3\xA4rts", "Links", "Rechts", "Waffe %d",
              "Sprinten (eigene Taste)", "Dr\xC3\xBC""cke eine Taste oder Maustaste ...",
              "Dr\xC3\xBC""cke die Taste f\xC3\xBCr diese Aktion ...",
              "Fest: Esc, Eingabe, R\xC3\xBC""cktaste und Pfeiltasten.", "EIGENES", "Umsehen", "Mausempfindlichkeit",
              "Empfindlichkeit beim Zielen", "Empfindlichkeit beim Zoomen", "Y-Achse umkehren", "Mausbeschleunigung",
              "Zielen", "Zoom", "Sprinten", "Halten", "Dr\xC3\xBC""cken", "Aus", "Nein", "Ja", "\xC3\x84ndern",
              "Entfernen", "Standard", "Reiter", "Zur\xC3\xBC""ck", "Schema", "Nicht verf\xC3\xBC""gbar", "Leertaste",
              "Umschalt", "Feststell", "Linksklick", "Rechtsklick", "Mittelklick", "Seite 1", "Seite 2", "Rad hoch",
              "Rad runter"};
    case 4:  // French
      return {"CLAVIER ET SOURIS", "COMMANDES", "Choisissez une action et appuyez sur la touche ou le bouton de la souris.",
              "Le sch\xC3\xA9ma du jeu, ou le v\xC3\xB4tre.", "ATTRIBUTION", "R\xC3\x89GLAGES", "D\xC3\x89PLACEMENT",
              "ARMES", "COMBAT", "ACTIONS", "Avancer", "Reculer", "Gauche", "Droite", "Arme %d",
              "Sprint (touche \xC3\xA0 part)", "Appuyez sur une touche ou un bouton de la souris...",
              "Appuyez sur le bouton voulu pour cette action...",
              "Fixes : \xC3\x89""chap, Entr\xC3\xA9""e, Retour arri\xC3\xA8re et les fl\xC3\xA8""ches.", "PERSONNALIS\xC3\x89",
              "Regarder", "Sensibilit\xC3\xA9 de la souris", "Sensibilit\xC3\xA9 en vis\xC3\xA9""e",
              "Sensibilit\xC3\xA9 du zoom", "Inverser l'axe Y", "Acc\xC3\xA9l\xC3\xA9ration de la souris", "Viser", "Zoom",
              "Sprint", "Maintenir", "Appuyer", "D\xC3\xA9sactiv\xC3\xA9""e", "Non", "Oui", "Changer", "Retirer",
              "Par d\xC3\xA9""faut", "Onglet", "Retour", "Sch\xC3\xA9ma", "Indisponible", "Espace", "Maj", "Verr. maj",
              "Clic gauche", "Clic droit", "Clic molette", "Lat\xC3\xA9ral 1", "Lat\xC3\xA9ral 2", "Molette haut",
              "Molette bas"};
    case 5:  // Spanish
      return {"TECLADO Y RAT\xC3\x93N", "CONTROLES", "Elige una acci\xC3\xB3n y pulsa la tecla o el bot\xC3\xB3n del rat\xC3\xB3n.",
              "El esquema del juego, o el tuyo.", "ASIGNACI\xC3\x93N", "AJUSTES", "MOVIMIENTO", "ARMAS", "COMBATE",
              "ACCIONES", "Adelante", "Atr\xC3\xA1s", "Izquierda", "Derecha", "Arma %d", "Correr agachado (tecla aparte)",
              "Pulsa una tecla o un bot\xC3\xB3n del rat\xC3\xB3n...", "Pulsa el bot\xC3\xB3n que quieras para esta acci\xC3\xB3n...",
              "Fijas: Esc, Intro, Retroceso y las flechas.", "PERSONALIZADO", "Mirar", "Sensibilidad del rat\xC3\xB3n",
              "Sensibilidad al apuntar", "Sensibilidad del zoom", "Invertir eje Y", "Aceleraci\xC3\xB3n del rat\xC3\xB3n",
              "Apuntar", "Zoom", "Correr agachado", "Mantener", "Pulsar", "Desactivada", "No", "S\xC3\xAD", "Cambiar",
              "Quitar", "Predeterminados", "Pesta\xC3\xB1""a", "Volver", "Esquema", "No disponible", "Espacio", "May\xC3\xBA""s",
              "Bloq May\xC3\xBA""s", "Clic izq.", "Clic der.", "Clic central", "Lateral 1", "Lateral 2", "Rueda arriba",
              "Rueda abajo"};
    case 6:  // Italian
      return {"TASTIERA E MOUSE", "COMANDI", "Scegli un'azione e premi il tasto o il pulsante del mouse.",
              "Lo schema del gioco, o il tuo.", "ASSEGNAZIONE", "IMPOSTAZIONI", "MOVIMENTO", "ARMI", "COMBATTIMENTO",
              "AZIONI", "Avanti", "Indietro", "Sinistra", "Destra", "Arma %d", "Corsa (tasto a parte)",
              "Premi un tasto o un pulsante del mouse...", "Premi il pulsante che vuoi per questa azione...",
              "Fissi: Esc, Invio, Backspace e le frecce.", "PERSONALIZZATO", "Visuale", "Sensibilit\xC3\xA0 del mouse",
              "Sensibilit\xC3\xA0 in mira", "Sensibilit\xC3\xA0 dello zoom", "Inverti asse Y", "Accelerazione del mouse",
              "Mira", "Zoom", "Corsa", "Tieni premuto", "Premi", "Disattivata", "No", "S\xC3\xAC", "Cambia", "Togli",
              "Predefiniti", "Scheda", "Indietro", "Schema", "Non disponibile", "Spazio", "Maiusc", "Bloc Maiusc",
              "Clic sinistro", "Clic destro", "Clic centrale", "Laterale 1", "Laterale 2", "Rotella su", "Rotella gi\xC3\xB9"};
    case 9:  // Portuguese
      return {"TECLADO E MOUSE", "CONTROLES", "Escolha uma a\xC3\xA7\xC3\xA3o e pressione a tecla ou o bot\xC3\xA3o do mouse.",
              "O esquema do jogo, ou o seu.", "ATRIBUI\xC3\x87\xC3\x83O", "AJUSTES", "MOVIMENTO", "ARMAS", "COMBATE",
              "A\xC3\x87\xC3\x95""ES", "Para frente", "Para tr\xC3\xA1s", "Esquerda", "Direita", "Arma %d", "Corrida (tecla pr\xC3\xB3pria)",
              "Pressione uma tecla ou um bot\xC3\xA3o do mouse...", "Pressione o bot\xC3\xA3o que quiser para esta a\xC3\xA7\xC3\xA3o...",
              "Fixas: Esc, Enter, Backspace e as setas.", "PERSONALIZADO", "Olhar", "Sensibilidade do mouse",
              "Sensibilidade ao mirar", "Sensibilidade do zoom", "Inverter eixo Y", "Acelera\xC3\xA7\xC3\xA3o do mouse",
              "Mirar", "Zoom", "Corrida", "Segurar", "Pressionar", "Desativada", "N\xC3\xA3o", "Sim", "Mudar", "Remover",
              "Padr\xC3\xB5""es", "Aba", "Voltar", "Esquema", "Indispon\xC3\xADvel", "Espa\xC3\xA7o", "Shift", "Caps Lock",
              "Clique esq.", "Clique dir.", "Clique central", "Lateral 1", "Lateral 2", "Roda para cima",
              "Roda para baixo"};
    case 11:  // Polish
      return {"KLAWIATURA I MYSZ", "STEROWANIE", "Wybierz akcj\xC4\x99 i naci\xC5\x9Bnij klawisz lub przycisk myszy.",
              "Schemat gry albo tw\xC3\xB3j w\xC5\x82""asny.", "PRZYPISANIE", "USTAWIENIA", "RUCH", "BRO\xC5\x83", "WALKA",
              "AKCJE", "Naprz\xC3\xB3""d", "Do ty\xC5\x82u", "W lewo", "W prawo", "Bro\xC5\x84 %d", "Bieg (osobny klawisz)",
              "Naci\xC5\x9Bnij klawisz lub przycisk myszy...", "Naci\xC5\x9Bnij przycisk dla tej akcji...",
              "Sta\xC5\x82""e: Esc, Enter, Backspace i strza\xC5\x82ki.", "W\xC5\x81""ASNY", "Rozgl\xC4\x85""danie",
              "Czu\xC5\x82o\xC5\x9B\xC4\x87 myszy", "Czu\xC5\x82o\xC5\x9B\xC4\x87 przy celowaniu", "Czu\xC5\x82o\xC5\x9B\xC4\x87 zoomu",
              "Odwr\xC3\xB3\xC4\x87 o\xC5\x9B Y", "Przyspieszenie myszy", "Celowanie", "Zoom", "Bieg", "Przytrzymaj", "Naci\xC5\x9Bnij",
              "Wy\xC5\x82\xC4\x85""czone", "Nie", "Tak", "Zmie\xC5\x84", "Usu\xC5\x84", "Domy\xC5\x9Blne", "Karta", "Wr\xC3\xB3\xC4\x87",
              "Schemat", "Niedost\xC4\x99pne", "Spacja", "Shift", "Caps Lock", "Lewy klik", "Prawy klik", "\xC5\x9Arodkowy klik",
              "Boczny 1", "Boczny 2", "K\xC3\xB3\xC5\x82ko w g\xC3\xB3r\xC4\x99", "K\xC3\xB3\xC5\x82ko w d\xC3\xB3\xC5\x82"};
    default:
      return {"KEYBOARD AND MOUSE", "CONTROLS", "Choose an action and press the key or mouse button.",
              "The game's scheme, or your own.", "ASSIGNMENT", "SETTINGS", "MOVEMENT", "WEAPONS", "COMBAT", "ACTIONS",
              "Forward", "Back", "Left", "Right", "Weapon %d", "Roadie run (key of its own)",
              "Press a key or mouse button...", "Press the button you want for this action...",
              "Fixed: Esc, Enter, Backspace and the arrow keys.", "CUSTOM", "Look", "Mouse sensitivity",
              "Sensitivity while aiming", "Zoom sensitivity", "Invert Y axis", "Mouse acceleration", "Aim", "Zoom",
              "Roadie run", "Hold", "Press", "Off", "No", "Yes", "Change", "Clear", "Defaults", "Tab", "Back", "Scheme",
              "Unavailable", "Space", "Shift", "Caps Lock", "Left click", "Right click", "Middle click", "Side 1",
              "Side 2", "Wheel up", "Wheel down"};
  }
}

// A description of the game's (UIScene_Options' controller view): its first line, and the rest as
// a smaller second line, without the parentheses the game puts around a line of its own.
std::pair<std::string, std::string> SplitDescription(std::string text) {
  for (char& c : text) {
    if (c == '\r') c = ' ';
  }
  auto clean = [](std::string s) {
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') s = s.substr(1, s.size() - 2);
    return s;
  };
  const size_t newline = text.find('\n');
  if (newline == std::string::npos) return {clean(text), {}};
  return {clean(text.substr(0, newline)), clean(text.substr(newline + 1))};
}

// --- Bindable keys -------------------------------------------------------------------------------

struct KeyName {
  ImGuiKey key;
  const char* token;  // as the keybind cvars write it
};

// The keys the menu binds. Escape, Enter, Backspace and the arrows are the driver's fixed keys,
// Delete and F1 the menu's own: none of them can be bound.
constexpr KeyName kKeys[] = {
    {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"}, {ImGuiKey_E, "E"},
    {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"}, {ImGuiKey_H, "H"}, {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"},
    {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"}, {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"}, {ImGuiKey_O, "O"},
    {ImGuiKey_P, "P"}, {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"},
    {ImGuiKey_U, "U"}, {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"}, {ImGuiKey_Y, "Y"},
    {ImGuiKey_Z, "Z"}, {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"},
    {ImGuiKey_4, "4"}, {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"}, {ImGuiKey_7, "7"}, {ImGuiKey_8, "8"},
    {ImGuiKey_9, "9"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"}, {ImGuiKey_F4, "F4"}, {ImGuiKey_F5, "F5"},
    {ImGuiKey_F6, "F6"}, {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"}, {ImGuiKey_F9, "F9"}, {ImGuiKey_F10, "F10"},
    {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"}, {ImGuiKey_Space, "Space"}, {ImGuiKey_Tab, "Tab"},
    {ImGuiKey_LeftShift, "Shift"}, {ImGuiKey_RightShift, "Shift"}, {ImGuiKey_LeftCtrl, "Ctrl"},
    {ImGuiKey_RightCtrl, "Ctrl"}, {ImGuiKey_LeftAlt, "Alt"}, {ImGuiKey_RightAlt, "Alt"},
    {ImGuiKey_CapsLock, "CapsLock"}, {ImGuiKey_GraveAccent, "Backtick"}, {ImGuiKey_Minus, "Minus"},
    {ImGuiKey_Equal, "Plus"}, {ImGuiKey_Comma, "Comma"}, {ImGuiKey_Period, "Period"},
    {ImGuiKey_Semicolon, "Semicolon"}, {ImGuiKey_Slash, "Slash"}, {ImGuiKey_Backslash, "Backslash"},
    {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"}, {ImGuiKey_Apostrophe, "Quote"},
    {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"}, {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Keypad0, "Numpad0"}, {ImGuiKey_Keypad1, "Numpad1"},
    {ImGuiKey_Keypad2, "Numpad2"}, {ImGuiKey_Keypad3, "Numpad3"}, {ImGuiKey_Keypad4, "Numpad4"},
    {ImGuiKey_Keypad5, "Numpad5"}, {ImGuiKey_Keypad6, "Numpad6"}, {ImGuiKey_Keypad7, "Numpad7"},
    {ImGuiKey_Keypad8, "Numpad8"}, {ImGuiKey_Keypad9, "Numpad9"}, {ImGuiKey_KeypadAdd, "NumpadPlus"},
    {ImGuiKey_KeypadSubtract, "NumpadMinus"}, {ImGuiKey_KeypadMultiply, "NumpadStar"},
    {ImGuiKey_KeypadDivide, "NumpadSlash"},
};

// How a bind token is shown on a key cap.
std::string KeyLabel(const std::string& token, const Texts& texts) {
  static const std::pair<const char*, const char* Texts::*> kNamed[] = {
      {"Space", &Texts::space},       {"Shift", &Texts::shift},           {"CapsLock", &Texts::caps},
      {"LMB", &Texts::left_click},    {"RMB", &Texts::right_click},       {"MMB", &Texts::middle_click},
      {"MiddleMouse", &Texts::middle_click}, {"Mouse4", &Texts::side_1},  {"MB4", &Texts::side_1},
      {"XButton1", &Texts::side_1},   {"Mouse5", &Texts::side_2},         {"MB5", &Texts::side_2},
      {"XButton2", &Texts::side_2},   {"WheelUp", &Texts::wheel_up},      {"WheelDown", &Texts::wheel_down},
  };
  for (const auto& [name, text] : kNamed) {
    if (token == name) return texts.*text;
  }
  static const std::pair<const char*, const char*> kSymbols[] = {
      {"Control", "Ctrl"}, {"Backtick", "`"}, {"Minus", "-"}, {"Plus", "+"}, {"Comma", ","}, {"Period", "."},
      {"Semicolon", ";"}, {"Slash", "/"}, {"Backslash", "\\"}, {"LBracket", "["}, {"RBracket", "]"},
      {"Quote", "'"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"}, {"Insert", "Ins"},
  };
  for (const auto& [name, symbol] : kSymbols) {
    if (token == name) return symbol;
  }
  if (token.rfind("Numpad", 0) == 0) return "Num " + token.substr(6);
  return token;
}

// The first two alternatives of a bind ("Q,Shift+W" -> "Q", "Shift+W"); the menu edits two.
std::array<std::string, 2> BindSlots(const std::string& value) {
  std::array<std::string, 2> slots;
  size_t start = 0;
  for (int i = 0; i < 2 && start <= value.size(); ++i) {
    const size_t comma = value.find(',', start);
    std::string token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    token.erase(0, token.find_first_not_of(' '));
    token.erase(token.find_last_not_of(' ') + 1);
    slots[i] = token;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (slots[0].empty()) std::swap(slots[0], slots[1]);
  return slots;
}

std::string JoinSlots(const std::array<std::string, 2>& slots) {
  if (slots[0].empty()) return slots[1];
  if (slots[1].empty()) return slots[0];
  return slots[0] + "," + slots[1];
}

// --- Rows ----------------------------------------------------------------------------------------

// What a keyboard row's label is: one of the game's button descriptions, or a text of ours.
enum class LabelSource { kGame, kForward, kBackward, kLeft, kRight, kWeapon, kRoadieRun };

struct KeyRow {
  const char* cvar;
  LabelSource source;
  const char* game_key;  // kGame: the description's key in UIScene_Options
  int weapon;            // kWeapon: its number
  int group;             // 0 movement, 1 weapons, 2 combat, 3 actions
};

constexpr KeyRow kKeyRows[] = {
    {"keybind_lstick_up", LabelSource::kForward, nullptr, 0, 0},
    {"keybind_lstick_down", LabelSource::kBackward, nullptr, 0, 0},
    {"keybind_lstick_left", LabelSource::kLeft, nullptr, 0, 0},
    {"keybind_lstick_right", LabelSource::kRight, nullptr, 0, 0},
    {"keybind_a", LabelSource::kGame, "AButtonString", 0, 0},
    {"keybind_run", LabelSource::kRoadieRun, nullptr, 0, 0},
    {"keybind_lstick_press", LabelSource::kGame, "LeftStickClickString", 0, 0},
    {"keybind_dpad_left", LabelSource::kWeapon, nullptr, 1, 1},
    {"keybind_dpad_right", LabelSource::kWeapon, nullptr, 2, 1},
    {"keybind_dpad_down", LabelSource::kWeapon, nullptr, 3, 1},
    {"keybind_dpad_up", LabelSource::kWeapon, nullptr, 4, 1},
    {"keybind_right_trigger", LabelSource::kGame, "RightTriggerString", 0, 2},
    {"keybind_left_trigger", LabelSource::kGame, "LeftTriggerString", 0, 2},
    {"keybind_right_shoulder", LabelSource::kGame, "RightBumperString", 0, 2},
    {"keybind_b", LabelSource::kGame, "BButtonString", 0, 2},
    {"keybind_rstick_press", LabelSource::kGame, "RightStickClickString", 0, 2},
    {"keybind_x", LabelSource::kGame, "XButtonString", 0, 3},
    {"keybind_y", LabelSource::kGame, "YButtonString", 0, 3},
    {"keybind_left_shoulder", LabelSource::kGame, "LeftBumperString", 0, 3},
    {"keybind_back", LabelSource::kGame, "BackButtonString", 0, 3},
};
constexpr int kKeyRowCount = static_cast<int>(std::size(kKeyRows));
// The first row of the second column (combat).
constexpr int kSecondColumn = 11;

// English descriptions when the game's are missing.
const std::map<std::string, std::string>& FallbackDescriptions() {
  static const std::map<std::string, std::string> texts = {
      {"AButtonString", "Take Cover / Evade / Mantle\n(Hold for Roadie Run)"},
      {"AButtonAltString", "Take Cover/Mantle"},
      {"BButtonString", "Melee / Grenade Tag\n(Hold for Chainsaw/Bayonet)"},
      {"XButtonString", "Use / Revive / Meatshield\n(Hold to Pick Up)"},
      {"XButtonAltString", "Evade\n(Hold for Roadie Run)"},
      {"YButtonString", "Point of Interest / Curb Stomp\n(Hold to Execute)"},
      {"YButtonAltString", "Use / Revive / Meatshield\n(Hold to Pick Up)"},
      {"LeftTriggerString", "Aim"},
      {"RightTriggerString", "Shoot / Throw Grenade"},
      {"LeftBumperString", "Tac/Com / Objectives"},
      {"RightBumperString", "Reload / Active Reload"},
      {"DPadString", "Switch Weapons"},
      {"LeftStickXString", "Strafe"},
      {"LeftStickYString", "/ Move"},
      {"LeftStickClickString", "(Click to Cover Crouch)\n(Aim & Click to Mark Target)"},
      {"RightStickXString", "Rotate"},
      {"RightStickYString", "/ Look"},
      {"RightStickClickString", "(Click to Zoom)"},
      {"RightStickClickAltString", "Point of Interest / Zoom / Execute"},
      {"BackButtonString", "Skip Cinematic / View Scoreboard"},
      {"StartButtonString", "Start / Pause (Options)"},
  };
  return texts;
}

// A settings row: a cvar of ours (keyboard and mouse) or a profile setting of the game's
// (controller), as a slider or a list of values.
struct SettingRow {
  const char* cvar;  // keyboard and mouse
  int psid;          // controller, 0 for none
  bool slider;
  float min, max, step;
  // A list: the cvar's values, or the profile setting's value IDs, in order.
  std::vector<std::string> values;
  std::vector<int> ids;
};

const std::vector<SettingRow>& KeyboardSettings() {
  static const std::vector<SettingRow> rows = {
      {"mnk_sensitivity", 0, true, 0.1f, 3.0f, 0.05f, {}, {}},
      {"mnk_aim_sensitivity", 0, true, 0.1f, 3.0f, 0.05f, {}, {}},
      {"mnk_zoom_sensitivity", 0, true, 0.1f, 3.0f, 0.05f, {}, {}},
      {"mnk_invert_y", 0, false, 0, 0, 0, {"false", "true"}, {}},
      {"mnk_mouse_acceleration", 0, true, 0.0f, 1.0f, 0.1f, {}, {}},
      {"mnk_aim_toggle", 0, false, 0, 0, 0, {"false", "true"}, {}},
      {"mnk_zoom_hold", 0, false, 0, 0, 0, {"false", "true"}, {}},
      {"mnk_run_toggle", 0, false, 0, 0, 0, {"false", "true"}, {}},
  };
  return rows;
}

// The Controller category's rows but the control scheme, which the Assignment tab chooses. Value
// IDs as GearProfileSettings defines them (docs/reverse-engineering.md); the sensitivities are
// floats from 0 to 1 in steps of 0.05, as the game's sliders.
const std::vector<SettingRow>& ControllerSettings() {
  static const std::vector<SettingRow> rows = {
      {nullptr, 2, false, 0, 0, 0, {}, {0, 1}},
      {nullptr, 58, false, 0, 0, 0, {}, {0, 1}},
      {nullptr, 375, false, 0, 0, 0, {}, {1, 0}},
      {nullptr, 368, true, 0.0f, 1.0f, 0.05f, {}, {}},
      {nullptr, 59, true, 0.0f, 1.0f, 0.05f, {}, {}},
      {nullptr, 60, true, 0.0f, 1.0f, 0.05f, {}, {}},
      {nullptr, 1, false, 0, 0, 0, {}, {3, 0}},
      {nullptr, 38, false, 0, 0, 0, {}, {0, 1, 2, 3}},
      {nullptr, 39, false, 0, 0, 0, {}, {0, 1}},
  };
  return rows;
}

// The game's defaults for those rows (the values the Defaults button of its list gives); the
// sensitivities' middle is inferred, not read from the game.
constexpr std::pair<int, uint32_t> kControllerDefaults[] = {
    {2, 0}, {58, 0}, {375, 1}, {1, 3}, {38, 0}, {39, 0},
};
constexpr float kDefaultSensitivity = 0.5f;

// The profile setting of the control scheme, and the guest user whose profile the menu edits.
constexpr int kControlSchemeSetting = 51;
constexpr uint32_t kProfileUser = 0;

float CvarFloat(const char* name) {
  return static_cast<float>(std::atof(rex::cvar::GetFlagByName(name).c_str()));
}

std::string FormatFloat(float value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%.2f", value);
  return text;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

float BitsFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// --- Controller rows ----------------------------------------------------------------------------

// One line of the controller's Assignment tab: a physical control (or a stick, or the D-pad), on
// the left or the right of the drawing.
struct PadRow {
  enum Type { kControl, kLeftStick, kRightStick, kDpad } type;
  PadControl control;
  bool right;
};

constexpr PadRow kPadRows[] = {
    {PadRow::kControl, PadControl::kLeftTrigger, false},   {PadRow::kControl, PadControl::kLeftShoulder, false},
    {PadRow::kLeftStick, PadControl::kCount, false},       {PadRow::kControl, PadControl::kLeftThumb, false},
    {PadRow::kDpad, PadControl::kCount, false},            {PadRow::kControl, PadControl::kBack, false},
    {PadRow::kControl, PadControl::kRightTrigger, true},   {PadRow::kControl, PadControl::kRightShoulder, true},
    {PadRow::kControl, PadControl::kY, true},              {PadRow::kControl, PadControl::kB, true},
    {PadRow::kControl, PadControl::kX, true},              {PadRow::kControl, PadControl::kA, true},
    {PadRow::kRightStick, PadControl::kCount, true},       {PadRow::kControl, PadControl::kRightThumb, true},
    {PadRow::kControl, PadControl::kStart, true},
};
constexpr int kPadRowCount = static_cast<int>(std::size(kPadRows));

// Where a row's control is on the drawing of the controller.
coop_ui::PadPoint PointOf(const PadRow& row) {
  using coop_ui::PadPoint;
  switch (row.type) {
    case PadRow::kLeftStick: return PadPoint::kLeftStick;
    case PadRow::kRightStick: return PadPoint::kRightStick;
    case PadRow::kDpad: return PadPoint::kDpad;
    case PadRow::kControl: break;
  }
  switch (row.control) {
    case PadControl::kA: return PadPoint::kA;
    case PadControl::kB: return PadPoint::kB;
    case PadControl::kX: return PadPoint::kX;
    case PadControl::kY: return PadPoint::kY;
    case PadControl::kLeftShoulder: return PadPoint::kLeftShoulder;
    case PadControl::kRightShoulder: return PadPoint::kRightShoulder;
    case PadControl::kLeftTrigger: return PadPoint::kLeftTrigger;
    case PadControl::kRightTrigger: return PadPoint::kRightTrigger;
    case PadControl::kLeftThumb: return PadPoint::kLeftStick;
    case PadControl::kRightThumb: return PadPoint::kRightStick;
    case PadControl::kBack: return PadPoint::kBack;
    default: return PadPoint::kStart;
  }
}

// Controls a player can move: not Start (the pause) nor the D-pad (weapons and menus).
bool Remappable(PadControl control) {
  return control != PadControl::kStart && control != PadControl::kCount && control != PadControl::kDpadUp &&
         control != PadControl::kDpadDown && control != PadControl::kDpadLeft && control != PadControl::kDpadRight;
}

// The key of the game's description of what a control does in a scheme (0 default, 1 alternate).
const char* DescriptionKey(PadControl control, int scheme) {
  switch (control) {
    case PadControl::kA: return scheme == 1 ? "AButtonAltString" : "AButtonString";
    case PadControl::kB: return "BButtonString";
    case PadControl::kX: return scheme == 1 ? "XButtonAltString" : "XButtonString";
    case PadControl::kY: return scheme == 1 ? "YButtonAltString" : "YButtonString";
    case PadControl::kLeftShoulder: return "LeftBumperString";
    case PadControl::kRightShoulder: return "RightBumperString";
    case PadControl::kLeftTrigger: return "LeftTriggerString";
    case PadControl::kRightTrigger: return "RightTriggerString";
    case PadControl::kLeftThumb: return "LeftStickClickString";
    case PadControl::kRightThumb: return scheme == 1 ? "RightStickClickAltString" : "RightStickClickString";
    case PadControl::kBack: return "BackButtonString";
    case PadControl::kStart: return "StartButtonString";
    default: return "DPadString";
  }
}

// The XInput bit of a control, the triggers as the menu's own bits.
uint32_t ControlBit(PadControl control) {
  switch (control) {
    case PadControl::kA: return rex::input::X_INPUT_GAMEPAD_A;
    case PadControl::kB: return rex::input::X_INPUT_GAMEPAD_B;
    case PadControl::kX: return rex::input::X_INPUT_GAMEPAD_X;
    case PadControl::kY: return rex::input::X_INPUT_GAMEPAD_Y;
    case PadControl::kLeftShoulder: return rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
    case PadControl::kRightShoulder: return rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    case PadControl::kLeftTrigger: return kLeftTriggerBit;
    case PadControl::kRightTrigger: return kRightTriggerBit;
    case PadControl::kLeftThumb: return rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
    case PadControl::kRightThumb: return rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
    case PadControl::kBack: return rex::input::X_INPUT_GAMEPAD_BACK;
    case PadControl::kStart: return rex::input::X_INPUT_GAMEPAD_START;
    case PadControl::kDpadUp: return rex::input::X_INPUT_GAMEPAD_DPAD_UP;
    case PadControl::kDpadDown: return rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
    case PadControl::kDpadLeft: return rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
    case PadControl::kDpadRight: return rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
    default: return 0;
  }
}

// --- Drawing helpers -----------------------------------------------------------------------------

// Draws `text` at `at` in at most `max_width`: smaller when it does not fit, down to kSmallestText
// of `size`, then cut with an ellipsis.
void DrawFitted(ImDrawList* draw_list, ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text,
                float max_width) {
  float fitted = size;
  const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  if (width > max_width && width > 0.0f) fitted = std::max(size * kSmallestText, size * max_width / width);
  const std::string shown = Fit(font, fitted, text, max_width);
  // Smaller text stays centred on the line it would have taken.
  draw_list->AddText(font, fitted, ImVec2(at.x, at.y + (size - fitted) * 0.5f), color, shown.c_str());
}

// A key cap with `label`, its top left at `at`, `height` tall and at least `min_width` wide.
// Returns its width.
float DrawKeyCap(ImDrawList* draw_list, ImFont* font, ImVec2 at, float height, float min_width,
                 const std::string& label, ImU32 fill, ImU32 line, ImU32 text_color, float unit) {
  const float size = height * 0.55f;
  const float text_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str()).x;
  const float width = std::max(min_width, text_width + 12.0f * unit);
  draw_list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), fill, 3.0f * unit);
  draw_list->AddRect(at, ImVec2(at.x + width, at.y + height), line, 3.0f * unit, 0, 1.0f * unit);
  AddTextCentered(draw_list, font, size, ImVec2(at.x + width * 0.5f, at.y + height * 0.5f), text_color, label.c_str());
  return width;
}

}  // namespace

ControlsMenu::ControlsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                           input::PlayerAssignment* assignment, Kind kind, uint32_t language, ImFont* title_font,
                           ImFont* body_font, ControlsMenu** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      kind_(kind),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner),
      game_texts_(menu::ControlsTextsFromGame()),
      opened_(Clock::now()),
      last_input_(input) {
  layout_ = rex::input::ParsePadLayout(rex::cvar::GetFlagByName("pad_remap"));
  uint32_t scheme = 0;
  if (rex::cvar::GetFlagByName("pad_custom") == "true") {
    scheme_ = 2;
  } else if (script::ReadProfileSetting(kProfileUser, kControlSchemeSetting, scheme)) {
    scheme_ = scheme == 1 ? 1 : 0;
  }
  if (assignment_) {
    assignment_->SetBlocked(true);
  }
}

const rex::input::DeviceInfo* ControlsMenu::ShownController() const {
  static thread_local rex::input::DeviceInfo shown;
  if (!assignment_) return nullptr;
  const rex::input::DeviceInfo* any = nullptr;
  const auto slots = assignment_->Slots();
  // The controller last pressed in the menu, while it is connected.
  for (const auto& slot : slots) {
    if (!slot.keyboard && slot.device.id == last_input_.pad()) {
      shown = slot.device;
      return &shown;
    }
  }
  for (const auto& slot : slots) {
    if (slot.keyboard) continue;
    if (slot.player == 0) {
      shown = slot.device;
      return &shown;
    }
    if (!any) {
      shown = slot.device;
      any = &shown;
    }
  }
  return any;
}

ControlsMenu::Actions ControlsMenu::ReadActions() {
  Actions actions;
  // The keyboard, through ImGui's keys (the game's keyboard input is blocked).
  if (keyboard_ready_) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) actions.vertical = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) actions.vertical = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) actions.horizontal = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) actions.horizontal = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) actions.tab = 1;
    // The controller menu's schemes, from the keyboard: Q and E, where LB and RB would be.
    if (kind_ == Kind::kController && ImGui::IsKeyPressed(ImGuiKey_Q, false)) actions.scheme = -1;
    if (kind_ == Kind::kController && ImGui::IsKeyPressed(ImGuiKey_E, false)) actions.scheme = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      actions.accept = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) actions.clear = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) actions.defaults = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) actions.back = true;
  }
  if (keyboard_ready_) last_input_.NoteKeyboard();
  keyboard_ready_ = true;
  // Every controller.
  if (!assignment_ || !input_) return actions;
  for (const auto& slot : assignment_->Slots()) {
    if (slot.keyboard) continue;
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
    uint32_t buttons = state.gamepad.buttons;
    const int stick_x = state.gamepad.thumb_lx;
    const int stick_y = state.gamepad.thumb_ly;
    if (stick_x <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
    if (stick_x >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (stick_y >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
    if (stick_y <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
    if (state.gamepad.left_trigger > kTriggerPress) buttons |= kLeftTriggerBit;
    if (state.gamepad.right_trigger > kTriggerPress) buttons |= kRightTriggerBit;
    auto found = previous_buttons_.find(slot.device.id);
    const bool seen = found != previous_buttons_.end();
    const uint32_t pressed = seen ? buttons & ~found->second : 0;
    previous_buttons_[slot.device.id] = buttons;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) actions.vertical = -1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) actions.vertical = 1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) actions.horizontal = -1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) actions.horizontal = 1;
    if (pressed & kLeftTriggerBit) actions.tab = -1;
    if (pressed & kRightTriggerBit) actions.tab = 1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) actions.scheme = -1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) actions.scheme = 1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_A) actions.accept = true;
    if (pressed & rex::input::X_INPUT_GAMEPAD_Y) actions.defaults = true;
    // X clears a key of the keyboard and mouse's, as Delete does.
    if (kind_ == Kind::kKeyboardMouse && (pressed & rex::input::X_INPUT_GAMEPAD_X)) actions.clear = true;
    if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_START)) actions.back = true;
    if (pressed) last_input_.NotePad(slot.device.id);
  }
  return actions;
}

rex::input::PadControl ControlsMenu::ReadPadPress() {
  PadControl result = PadControl::kCount;
  if (!assignment_ || !input_) return result;
  for (const auto& slot : assignment_->Slots()) {
    if (slot.keyboard) continue;
    rex::input::X_INPUT_STATE state = {};
    if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
    uint32_t buttons = state.gamepad.buttons;
    if (state.gamepad.left_trigger > kTriggerPress) buttons |= kLeftTriggerBit;
    if (state.gamepad.right_trigger > kTriggerPress) buttons |= kRightTriggerBit;
    auto found = previous_buttons_.find(slot.device.id);
    const uint32_t pressed = found != previous_buttons_.end() ? buttons & ~found->second : 0;
    previous_buttons_[slot.device.id] = buttons;
    for (size_t i = 0; i < rex::input::kPadControlCount && result == PadControl::kCount; ++i) {
      const auto control = static_cast<PadControl>(i);
      if (pressed & ControlBit(control)) result = control;
    }
  }
  return result;
}

std::string ControlsMenu::ReadKeyPress(bool& cancel) {
  cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  if (cancel) return {};
  for (const auto& key : kKeys) {
    if (ImGui::IsKeyPressed(key.key, false)) return key.token;
  }
  static constexpr const char* kMouse[] = {"LMB", "RMB", "MMB", "Mouse4", "Mouse5"};
  for (int button = 0; button < 5; ++button) {
    if (ImGui::IsMouseClicked(button)) return kMouse[button];
  }
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel > 0.0f) return "WheelUp";
  if (wheel < 0.0f) return "WheelDown";
  return {};
}

// --- Keyboard and mouse ---------------------------------------------------------------------------

void ControlsMenu::HandleKeyboardAssignment(const Actions& actions) {
  const KeyRow& row = kKeyRows[row_];
  if (waiting_) {
    bool cancel = false;
    const std::string token = ReadKeyPress(cancel);
    if (cancel || std::chrono::duration<float>(Clock::now() - waiting_since_).count() > kWaitSeconds) {
      waiting_ = false;
      return;
    }
    if (token.empty()) return;
    waiting_ = false;
    auto slots = BindSlots(rex::cvar::GetFlagByName(row.cvar));
    const std::string previous = slots[slot_];
    // A key another action had moves to this one, and that action gets this one's old key.
    for (const KeyRow& other : kKeyRows) {
      if (&other == &row) continue;
      auto other_slots = BindSlots(rex::cvar::GetFlagByName(other.cvar));
      for (auto& other_key : other_slots) {
        if (other_key == token) {
          other_key = previous;
          menu::StoreControlsSetting(other.cvar, JoinSlots(other_slots), true);
        }
      }
    }
    for (auto& key : slots) {
      if (key == token) key.clear();
    }
    slots[slot_] = token;
    menu::StoreControlsSetting(row.cvar, JoinSlots(slots), true);
    return;
  }
  if (actions.vertical) row_ = (row_ + actions.vertical + kKeyRowCount) % kKeyRowCount;
  if (actions.horizontal) slot_ = std::clamp(slot_ + actions.horizontal, 0, 1);
  if (actions.accept) {
    waiting_ = true;
    waiting_since_ = Clock::now();
    // The press that started the wait is not taken as the key.
    ImGui::GetIO().MouseWheel = 0.0f;
  }
  if (actions.clear) {
    auto slots = BindSlots(rex::cvar::GetFlagByName(row.cvar));
    slots[slot_].clear();
    menu::StoreControlsSetting(row.cvar, JoinSlots(slots), true);
  }
  if (actions.defaults) {
    for (const KeyRow& each : kKeyRows) {
      const std::string project = menu::ControlsSettingDefault(each.cvar);
      if (project.empty()) {
        rex::cvar::ResetToDefault(each.cvar);
        menu::StoreControlsSetting(each.cvar, rex::cvar::GetFlagByName(each.cvar), true);
      } else {
        menu::StoreControlsSetting(each.cvar, project, true);
      }
    }
  }
}

void ControlsMenu::HandleKeyboardSettings(const Actions& actions) {
  const auto& rows = KeyboardSettings();
  const int count = static_cast<int>(rows.size());
  if (actions.vertical) row_ = (row_ + actions.vertical + count) % count;
  const SettingRow& row = rows[row_];
  if (actions.horizontal) {
    if (row.slider) {
      const float value = std::clamp(CvarFloat(row.cvar) + actions.horizontal * row.step, row.min, row.max);
      menu::StoreControlsSetting(row.cvar, FormatFloat(value), false);
    } else {
      const std::string now = rex::cvar::GetFlagByName(row.cvar);
      const auto at = std::find(row.values.begin(), row.values.end(), now);
      const int index = at == row.values.end() ? 0 : static_cast<int>(at - row.values.begin());
      const int count_values = static_cast<int>(row.values.size());
      menu::StoreControlsSetting(row.cvar, row.values[(index + actions.horizontal + count_values) % count_values], false);
    }
  }
  if (actions.defaults) {
    for (const SettingRow& each : rows) {
      const std::string project = menu::ControlsSettingDefault(each.cvar);
      if (project.empty()) rex::cvar::ResetToDefault(each.cvar);
      menu::StoreControlsSetting(each.cvar, project.empty() ? rex::cvar::GetFlagByName(each.cvar) : project, false);
    }
  }
}

// --- Controller ----------------------------------------------------------------------------------

void ControlsMenu::HandleControllerAssignment(const Actions& actions) {
  if (waiting_) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        std::chrono::duration<float>(Clock::now() - waiting_since_).count() > kWaitSeconds) {
      waiting_ = false;
      return;
    }
    const PadControl pressed = ReadPadPress();
    if (pressed == PadControl::kCount) return;
    waiting_ = false;
    if (!Remappable(pressed)) return;
    // The chosen control's action moves to the button pressed, and that button's to the chosen one.
    const PadControl chosen = kPadRows[row_].control;
    std::swap(layout_[static_cast<size_t>(chosen)], layout_[static_cast<size_t>(pressed)]);
    menu::StoreControlsSetting("pad_remap", rex::input::FormatPadLayout(layout_), true);
    return;
  }
  if (actions.scheme) {
    scheme_ = (scheme_ + actions.scheme + 3) % 3;
    // Custom builds on the default scheme; the game's two are its profile's.
    script::WriteProfileSetting(kProfileUser, kControlSchemeSetting, scheme_ == 1 ? 1 : 0);
    menu::StoreControlsSetting("pad_custom", scheme_ == 2 ? "true" : "false", false);
  }
  if (scheme_ != 2) return;
  if (actions.vertical) {
    // Only the controls a player can move are chosen.
    for (int step = 0; step < kPadRowCount; ++step) {
      row_ = (row_ + actions.vertical + kPadRowCount) % kPadRowCount;
      if (kPadRows[row_].type == PadRow::kControl && Remappable(kPadRows[row_].control)) break;
    }
  }
  if (actions.horizontal) {
    // To the other side of the drawing, to its first control a player can move.
    const bool right = !kPadRows[row_].right;
    for (int i = 0; i < kPadRowCount; ++i) {
      if (kPadRows[i].right == right && kPadRows[i].type == PadRow::kControl && Remappable(kPadRows[i].control)) {
        row_ = i;
        break;
      }
    }
  }
  // Waiting for a button needs a controller to press it on.
  if (actions.accept && ShownController() && kPadRows[row_].type == PadRow::kControl && Remappable(kPadRows[row_].control)) {
    waiting_ = true;
    waiting_since_ = Clock::now();
  }
  if (actions.defaults) {
    layout_ = rex::input::IdentityPadLayout();
    menu::StoreControlsSetting("pad_remap", rex::input::FormatPadLayout(layout_), true);
  }
}

void ControlsMenu::HandleControllerSettings(const Actions& actions) {
  const auto& rows = ControllerSettings();
  const int count = static_cast<int>(rows.size());
  if (actions.vertical) row_ = (row_ + actions.vertical + count) % count;
  const SettingRow& row = rows[row_];
  uint32_t bits = 0;
  if (actions.horizontal && script::ReadProfileSetting(kProfileUser, row.psid, bits)) {
    if (row.slider) {
      const float value = std::clamp(std::round((BitsFloat(bits) + actions.horizontal * row.step) / row.step) * row.step,
                                     row.min, row.max);
      script::WriteProfileSetting(kProfileUser, row.psid, FloatBits(value));
    } else {
      const auto at = std::find(row.ids.begin(), row.ids.end(), static_cast<int>(bits));
      const int index = at == row.ids.end() ? 0 : static_cast<int>(at - row.ids.begin());
      const int ids = static_cast<int>(row.ids.size());
      script::WriteProfileSetting(kProfileUser, row.psid, static_cast<uint32_t>(row.ids[(index + actions.horizontal + ids) % ids]));
    }
  }
  if (actions.defaults) {
    for (const auto& [psid, value] : kControllerDefaults) script::WriteProfileSetting(kProfileUser, psid, value);
    for (int psid : {368, 59, 60}) script::WriteProfileSetting(kProfileUser, psid, FloatBits(kDefaultSensitivity));
  }
}

// --- Drawing -------------------------------------------------------------------------------------

void ControlsMenu::DrawKeyboardAssignment(ImDrawList* draw_list, float left, float top, float width, float) {
  const Texts texts = LocalizedTexts(language_);
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const float u = unit_;
  const float gap = 18.0f * u;
  const float column_width = (width - gap) * 0.5f;
  const float row_height = 27.0f * u;
  const float slot_width = 92.0f * u;
  const char* groups[4] = {texts.movement, texts.weapons, texts.combat, texts.actions};
  for (int column = 0; column < 2; ++column) {
    const float x = left + column * (column_width + gap);
    float y = top;
    int group = -1;
    for (int i = column == 0 ? 0 : kSecondColumn; i < (column == 0 ? kSecondColumn : kKeyRowCount); ++i) {
      const KeyRow& row = kKeyRows[i];
      if (row.group != group) {
        group = row.group;
        draw_list->AddText(title_font, 12.0f * u, ImVec2(x + 4.0f * u, y + 4.0f * u), Imulsion(255), groups[group]);
        y += 22.0f * u;
      }
      const bool selected = i == row_;
      const ImVec2 min(x, y);
      const ImVec2 max(x + column_width, y + row_height - 3.0f * u);
      draw_list->AddRectFilled(min, max, selected ? IM_COL32(34, 31, 25, 255) : PanelSolid(255));
      if (selected) draw_list->AddRect(min, max, Imulsion(255), 0.0f, 0, 1.0f * u);
      // The label: one of the game's descriptions (its first line) or ours.
      std::string label;
      char weapon[48];
      switch (row.source) {
        case LabelSource::kForward: label = texts.forward; break;
        case LabelSource::kBackward: label = texts.backward; break;
        case LabelSource::kLeft: label = texts.left; break;
        case LabelSource::kRight: label = texts.right; break;
        case LabelSource::kRoadieRun: label = texts.roadie_run; break;
        case LabelSource::kWeapon:
          std::snprintf(weapon, sizeof(weapon), texts.weapon, row.weapon);
          label = weapon;
          break;
        case LabelSource::kGame: {
          const auto found = game_texts_.buttons.find(row.game_key);
          label = SplitDescription(found != game_texts_.buttons.end() ? found->second
                                                                      : FallbackDescriptions().at(row.game_key))
                      .first;
          break;
        }
      }
      const float label_width = column_width - 2.0f * slot_width - 30.0f * u;
      DrawFitted(draw_list, body_font, 13.0f * u, ImVec2(x + 10.0f * u, y + 5.0f * u), Text(255), label, label_width);
      // The two keys, or the wait for one across both.
      const auto slots = BindSlots(rex::cvar::GetFlagByName(row.cvar));
      const float slots_left = x + column_width - 2.0f * slot_width - 14.0f * u;
      if (selected && waiting_) {
        const ImVec2 wait_min(slots_left, y + 3.0f * u);
        const ImVec2 wait_max(slots_left + 2.0f * slot_width + 6.0f * u, y + row_height - 6.0f * u);
        draw_list->AddRect(wait_min, wait_max, Imulsion(255), 0.0f, 0, 1.0f * u);
        DrawFitted(draw_list, body_font, 12.0f * u, ImVec2(wait_min.x + 6.0f * u, wait_min.y + 3.0f * u), Imulsion(255),
                   texts.wait_key, wait_max.x - wait_min.x - 12.0f * u);
      } else {
        for (int s = 0; s < 2; ++s) {
          const float sx = slots_left + s * (slot_width + 6.0f * u);
          const ImVec2 smin(sx, y + 3.0f * u);
          const ImVec2 smax(sx + slot_width, y + row_height - 6.0f * u);
          const bool on = selected && s == slot_;
          draw_list->AddRectFilled(smin, smax, IM_COL32(29, 32, 36, 255));
          draw_list->AddRect(smin, smax, on ? Imulsion(255) : SteelLine(255), 0.0f, 0, 1.0f * u);
          const std::string key = slots[s].empty() ? std::string("-") : KeyLabel(slots[s], texts);
          const float size = 12.0f * u;
          float fitted = size;
          const float kw = body_font->CalcTextSizeA(size, FLT_MAX, 0.0f, key.c_str()).x;
          if (kw > slot_width - 10.0f * u) fitted = std::max(size * kSmallestText, size * (slot_width - 10.0f * u) / kw);
          const std::string shown = Fit(body_font, fitted, key, slot_width - 10.0f * u);
          const float sw = body_font->CalcTextSizeA(fitted, FLT_MAX, 0.0f, shown.c_str()).x;
          draw_list->AddText(body_font, fitted, ImVec2(sx + (slot_width - sw) * 0.5f, smin.y + (smax.y - smin.y - fitted) * 0.5f),
                             slots[s].empty() ? TextFaint(255) : (on ? Imulsion(255) : Text(255)), shown.c_str());
        }
      }
      y += row_height;
    }
  }
}

void ControlsMenu::DrawSettings(ImDrawList* draw_list, float left, float top, float width, float) {
  const Texts texts = LocalizedTexts(language_);
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const float u = unit_;
  const bool keyboard = kind_ == Kind::kKeyboardMouse;
  const auto& rows = keyboard ? KeyboardSettings() : ControllerSettings();
  const char* keyboard_labels[] = {texts.mouse_sensitivity, texts.aim_sensitivity, texts.zoom_sensitivity,
                                   texts.invert_y,          texts.acceleration,    texts.aim,
                                   texts.zoom,              texts.run};
  const float row_height = 38.0f * u;
  const float control_width = 330.0f * u;
  for (size_t i = 0; i < rows.size(); ++i) {
    const SettingRow& row = rows[i];
    const float y = top + i * (row_height + 4.0f * u);
    const bool selected = static_cast<int>(i) == row_;
    const ImVec2 min(left, y);
    const ImVec2 max(left + width, y + row_height);
    draw_list->AddRectFilled(min, max, selected ? IM_COL32(34, 31, 25, 255) : PanelSolid(255));
    if (selected) draw_list->AddRect(min, max, Imulsion(255), 0.0f, 0, 1.0f * u);
    std::string label;
    if (keyboard) {
      label = keyboard_labels[i];
    } else {
      const auto found = game_texts_.setting_labels.find(row.psid);
      label = found != game_texts_.setting_labels.end() ? found->second : std::to_string(row.psid);
    }
    DrawFitted(draw_list, body_font, 14.0f * u, ImVec2(left + 14.0f * u, y + 11.0f * u), Text(255), label,
               width - control_width - 40.0f * u);
    const float cx = left + width - control_width - 14.0f * u;
    // The value: a slider with its number, or the list's value between arrows.
    bool known = true;
    float number = 0.0f;
    std::string value;
    if (keyboard) {
      if (row.slider) {
        number = CvarFloat(row.cvar);
      } else {
        const bool on = rex::cvar::GetFlagByName(row.cvar) == "true";
        // Aim and roadie run toggle with a press; zoom is held when on.
        if (std::strcmp(row.cvar, "mnk_invert_y") == 0) value = on ? texts.yes : texts.no;
        else if (std::strcmp(row.cvar, "mnk_zoom_hold") == 0) value = on ? texts.hold : texts.press;
        else value = on ? texts.press : texts.hold;
      }
    } else {
      uint32_t bits = 0;
      known = script::ReadProfileSetting(kProfileUser, row.psid, bits);
      if (known && row.slider) {
        number = BitsFloat(bits);
      } else if (known) {
        const auto at = std::find(row.ids.begin(), row.ids.end(), static_cast<int>(bits));
        const auto names = game_texts_.value_names.find(row.psid);
        const size_t index = at == row.ids.end() ? 0 : static_cast<size_t>(at - row.ids.begin());
        value = names != game_texts_.value_names.end() && index < names->second.size() ? names->second[index]
                                                                                        : std::to_string(bits);
      }
    }
    if (!known) {
      DrawFitted(draw_list, body_font, 13.0f * u, ImVec2(cx, y + 11.0f * u), TextFaint(255), texts.unavailable,
                 control_width);
      continue;
    }
    if (row.slider) {
      const float track_left = cx;
      const float track_right = cx + control_width - 60.0f * u;
      const float cy = y + row_height * 0.5f;
      const float t = std::clamp((number - row.min) / (row.max - row.min), 0.0f, 1.0f);
      draw_list->AddRectFilled(ImVec2(track_left, cy - 3.0f * u), ImVec2(track_right, cy + 3.0f * u), IM_COL32(44, 48, 54, 255));
      const float knob = track_left + t * (track_right - track_left);
      draw_list->AddRectFilled(ImVec2(track_left, cy - 3.0f * u), ImVec2(knob, cy + 3.0f * u), Imulsion(255));
      draw_list->AddRectFilled(ImVec2(knob - 2.0f * u, cy - 8.0f * u), ImVec2(knob + 2.0f * u, cy + 8.0f * u), Text(255));
      const std::string shown = keyboard && std::strcmp(row.cvar, "mnk_mouse_acceleration") == 0 && number <= 0.0f
                                    ? std::string(texts.off)
                                    : FormatFloat(number);
      const float sw = body_font->CalcTextSizeA(13.0f * u, FLT_MAX, 0.0f, shown.c_str()).x;
      draw_list->AddText(body_font, 13.0f * u, ImVec2(cx + control_width - sw, y + 11.0f * u), Text(255), shown.c_str());
    } else {
      const ImU32 arrows = selected ? Imulsion(255) : TextFaint(255);
      DrawArrow(draw_list, ImVec2(cx + 6.0f * u, y + row_height * 0.5f), 9.0f * u, true, arrows);
      DrawArrow(draw_list, ImVec2(cx + control_width - 6.0f * u, y + row_height * 0.5f), 9.0f * u, false, arrows);
      const std::string shown = Fit(body_font, 14.0f * u, value, control_width - 40.0f * u);
      const float sw = body_font->CalcTextSizeA(14.0f * u, FLT_MAX, 0.0f, shown.c_str()).x;
      draw_list->AddText(body_font, 14.0f * u, ImVec2(cx + (control_width - sw) * 0.5f, y + 10.0f * u), Text(255),
                         shown.c_str());
    }
  }
}

void ControlsMenu::DrawControllerAssignment(ImDrawList* draw_list, float left, float top, float width, float height) {
  const Texts texts = LocalizedTexts(language_);
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const float u = unit_;
  const rex::input::DeviceInfo* pad = ShownController();
  const PadLook look = pad ? LookOf(*pad) : PadLook::kGeneric;

  // The schemes, the bumpers changing them.
  float x = left;
  const float scheme_y = top;
  x += DrawPadGlyph(draw_list, title_font, ImVec2(x + 17.0f * u, scheme_y + 14.0f * u), 22.0f * u, look,
                    PadControl::kLeftShoulder, 255, u) + 10.0f * u;
  const auto scheme_names = game_texts_.value_names.find(kControlSchemeSetting);
  for (int i = 0; i < 3; ++i) {
    std::string name = i == 2 ? std::string(texts.custom)
                              : scheme_names != game_texts_.value_names.end() && i < static_cast<int>(scheme_names->second.size())
                                    ? scheme_names->second[i]
                                    : std::string(i == 0 ? "DEFAULT" : "ALTERNATE");
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const float tw = title_font->CalcTextSizeA(14.0f * u, FLT_MAX, 0.0f, name.c_str()).x;
    const ImVec2 min(x, scheme_y);
    const ImVec2 max(x + tw + 28.0f * u, scheme_y + 28.0f * u);
    const bool on = i == scheme_;
    draw_list->AddRectFilled(min, max, on ? Imulsion(255) : PanelSolid(255));
    draw_list->AddRect(min, max, on ? Imulsion(255) : SteelLine(255), 0.0f, 0, 1.0f * u);
    draw_list->AddText(title_font, 14.0f * u, ImVec2(x + 14.0f * u, scheme_y + 6.0f * u), on ? Panel(255) : TextDim(255),
                       name.c_str());
    x = max.x + 6.0f * u;
  }
  DrawPadGlyph(draw_list, title_font, ImVec2(x + 17.0f * u, scheme_y + 14.0f * u), 22.0f * u, look,
               PadControl::kRightShoulder, 255, u);

  // The controller in the middle, what each control does around it.
  const float area_top = top + 46.0f * u;
  const float area_height = height - 46.0f * u;
  const ImVec2 pad_center(left + width * 0.5f, area_top + area_height * 0.5f);
  const float pad_size = 150.0f * u;
  DrawControllerIcon(draw_list, pad_center, pad_size, look, 255);
  const float column_width = width * 0.5f - 170.0f * u;
  const float row_height = 40.0f * u;
  const int scheme_texts = scheme_ == 1 ? 1 : 0;
  int left_index = 0;
  int right_index = 0;
  for (int i = 0; i < kPadRowCount; ++i) {
    const PadRow& row = kPadRows[i];
    const int index = row.right ? right_index++ : left_index++;
    const float y = area_top + index * row_height;
    const float rx = row.right ? left + width - column_width : left;
    // What the control does: in Custom, the action its layout gives it.
    std::string key;
    std::string fallback_main;
    switch (row.type) {
      case PadRow::kLeftStick: key = "LeftStickXString"; break;
      case PadRow::kRightStick: key = "RightStickXString"; break;
      case PadRow::kDpad: key = "DPadString"; break;
      case PadRow::kControl: {
        const PadControl acting = scheme_ == 2 ? layout_[static_cast<size_t>(row.control)] : row.control;
        key = DescriptionKey(acting, scheme_texts);
        break;
      }
    }
    auto text_of = [&](const std::string& name) {
      const auto found = game_texts_.buttons.find(name);
      if (found != game_texts_.buttons.end()) return found->second;
      const auto fallback = FallbackDescriptions().find(name);
      return fallback != FallbackDescriptions().end() ? fallback->second : std::string();
    };
    std::pair<std::string, std::string> description;
    if (row.type == PadRow::kLeftStick) {
      description = {SplitDescription(text_of("LeftStickXString")).first + " " + SplitDescription(text_of("LeftStickYString")).first, {}};
    } else if (row.type == PadRow::kRightStick) {
      description = {SplitDescription(text_of("RightStickXString")).first + " " + SplitDescription(text_of("RightStickYString")).first, {}};
    } else {
      description = SplitDescription(text_of(key));
    }
    const bool selected = scheme_ == 2 && i == row_;
    if (selected) {
      draw_list->AddRectFilled(ImVec2(rx - 4.0f * u, y - 3.0f * u), ImVec2(rx + column_width + 4.0f * u, y + row_height - 5.0f * u),
                               IM_COL32(34, 31, 25, 255));
      draw_list->AddRect(ImVec2(rx - 4.0f * u, y - 3.0f * u), ImVec2(rx + column_width + 4.0f * u, y + row_height - 5.0f * u),
                         Imulsion(255), 0.0f, 0, 1.0f * u);
    }
    // The glyph on the controller's side of the text.
    const float glyph = 22.0f * u;
    const float glyph_x = row.right ? rx + 20.0f * u : rx + column_width - 20.0f * u;
    // A line from the row to its control on the drawing, ending in a dot.
    {
      const ImVec2 from(row.right ? rx - 2.0f * u : rx + column_width + 2.0f * u, y + 15.0f * u);
      const ImVec2 to = PadPointPosition(pad_center, pad_size, look, PointOf(row));
      const ImVec2 bend(row.right ? from.x - 14.0f * u : from.x + 14.0f * u, from.y);
      const ImU32 line = selected ? Imulsion(255) : IM_COL32(120, 116, 106, 200);
      draw_list->AddLine(from, bend, line, 1.0f * u);
      draw_list->AddLine(bend, to, line, 1.0f * u);
      draw_list->AddCircleFilled(to, 2.5f * u, line, 10);
    }
    if (row.type == PadRow::kControl) {
      DrawPadGlyph(draw_list, title_font, ImVec2(glyph_x, y + 15.0f * u), glyph, look, row.control, 255, u);
    } else {
      const char* name = row.type == PadRow::kDpad ? "+" : row.type == PadRow::kLeftStick ? "L" : "R";
      DrawKeyCap(draw_list, title_font, ImVec2(glyph_x - 14.0f * u, y + 4.0f * u), glyph, 28.0f * u, name,
                 IM_COL32(58, 63, 70, 255), IM_COL32(58, 63, 70, 255), Text(255), u);
    }
    const float text_left = row.right ? rx + 44.0f * u : rx;
    const float text_width = column_width - 48.0f * u;
    const ImU32 colour = selected ? Imulsion(255) : Text(255);
    std::string second = description.second;
    if (selected && waiting_) second = texts.wait_button;
    // Left of the drawing, the text is right-aligned towards it.
    auto place = [&](const std::string& text, float size) {
      if (row.right) return text_left;
      const float w = std::min(text_width, body_font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x);
      return text_left + text_width - w;
    };
    DrawFitted(draw_list, body_font, 13.0f * u, ImVec2(place(description.first, 13.0f * u), y + 2.0f * u), colour,
               description.first, text_width);
    if (!second.empty()) {
      DrawFitted(draw_list, body_font, 11.0f * u, ImVec2(place(second, 11.0f * u), y + 18.0f * u),
                 selected && waiting_ ? Imulsion(255) : TextFaint(255), second, text_width);
    }
  }
}

void ControlsMenu::DrawHints(ImDrawList* draw_list, float left, float right, float y) {
  const Texts texts = LocalizedTexts(language_);
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const float u = unit_;
  const rex::input::DeviceInfo* pad = ShownController();
  const PadLook look = pad ? LookOf(*pad) : PadLook::kGeneric;
  // Right-aligned, as the game's button bars: each hint is a glyph and its text.
  struct Hint {
    std::vector<PadControl> pad;
    std::string key;
    const char* text;
  };
  std::vector<Hint> hints;
  if (kind_ == Kind::kKeyboardMouse && pad && !last_input_.keyboard()) {
    // Driven from a controller: its buttons (binding a key still waits for the keyboard).
    if (tab_ == 0) {
      hints.push_back({{PadControl::kA}, {}, texts.change});
      hints.push_back({{PadControl::kX}, {}, texts.clear});
    }
    hints.push_back({{PadControl::kY}, {}, texts.defaults});
    hints.push_back({{PadControl::kLeftTrigger, PadControl::kRightTrigger}, {}, texts.tab});
    hints.push_back({{PadControl::kB}, {}, texts.close});
  } else if (kind_ == Kind::kKeyboardMouse) {
    if (tab_ == 0) {
      hints = {{{}, "Enter", texts.change}, {{}, "Del", texts.clear}};
    }
    hints.push_back({{}, "F1", texts.defaults});
    hints.push_back({{}, "Tab", texts.tab});
    hints.push_back({{}, "Esc", texts.close});
  } else if (!pad || last_input_.keyboard()) {
    // No controller, or the keyboard was the last used: the keyboard's keys.
    if (tab_ == 0) hints.push_back({{}, "Q E", texts.scheme});
    if (tab_ == 1 || scheme_ == 2) hints.push_back({{}, "F1", texts.defaults});
    hints.push_back({{}, "Tab", texts.tab});
    hints.push_back({{}, "Esc", texts.close});
  } else {
    if (tab_ == 0) {
      hints.push_back({{PadControl::kLeftShoulder, PadControl::kRightShoulder}, {}, texts.scheme});
      if (scheme_ == 2) hints.push_back({{PadControl::kA}, {}, texts.change});
    }
    if (tab_ == 1 || scheme_ == 2) hints.push_back({{PadControl::kY}, {}, texts.defaults});
    hints.push_back({{PadControl::kLeftTrigger, PadControl::kRightTrigger}, {}, texts.tab});
    hints.push_back({{PadControl::kB}, {}, texts.close});
  }
  // Measured first, then drawn from the right.
  float x = right;
  for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
    const float text_width = body_font->CalcTextSizeA(13.0f * u, FLT_MAX, 0.0f, it->text).x;
    x -= text_width;
    draw_list->AddText(body_font, 13.0f * u, ImVec2(x, y), Text(255), it->text);
    x -= 6.0f * u;
    if (!it->pad.empty()) {
      for (auto control = it->pad.rbegin(); control != it->pad.rend(); ++control) {
        const float glyph = 18.0f * u;
        const bool face = *control == PadControl::kA || *control == PadControl::kB || *control == PadControl::kX ||
                          *control == PadControl::kY;
        const std::string name = ControlName(*control, look);
        const float w = face ? glyph
                             : std::max(glyph * 1.3f, title_font->CalcTextSizeA(glyph * 0.52f, FLT_MAX, 0.0f, name.c_str()).x +
                                                          10.0f * u);
        x -= w;
        DrawPadGlyph(draw_list, title_font, ImVec2(x + w * 0.5f, y + 8.0f * u), glyph, look, *control, 255, u);
        x -= 4.0f * u;
      }
    } else {
      // One key cap per key; keys separated by a space ("Q E") are two caps with a slash between
      // them, dimmer than the caps so it reads as a separator and not as part of a key.
      std::vector<std::string> keys;
      for (size_t start = 0; start <= it->key.size();) {
        const size_t space = std::min(it->key.find(' ', start), it->key.size());
        if (space > start) keys.push_back(it->key.substr(start, space - start));
        start = space + 1;
      }
      for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
        if (key != keys.rbegin()) {
          const float slash = 10.0f * u;
          x -= 2.0f * u + slash;
          AddTextCentered(draw_list, title_font, 13.0f * u, ImVec2(x + slash * 0.5f, y + 8.0f * u), TextFaint(255), "/");
          x -= 2.0f * u;
        }
        const float w = title_font->CalcTextSizeA(18.0f * u * 0.55f, FLT_MAX, 0.0f, key->c_str()).x + 12.0f * u;
        x -= std::max(26.0f * u, w);
        DrawKeyCap(draw_list, title_font, ImVec2(x, y - 1.0f * u), 18.0f * u, 26.0f * u, *key, IM_COL32(29, 32, 36, 255),
                   TextFaint(255), Text(255), u);
      }
    }
    x -= 22.0f * u;
    if (x < left) break;
  }
}

void ControlsMenu::OnDraw(ImGuiIO& io) {
  if (!assignment_ || !input_) {
    Close();
    return;
  }
  // Closing waits for what closed it to be released (coop_ui::CloseInputHeld).
  if (closing_) {
    std::vector<rex::input::DeviceId> pads;
    for (const auto& slot : assignment_->Slots()) {
      if (!slot.keyboard) pads.push_back(slot.device.id);
    }
    if (!CloseInputHeld(input_, pads)) Close();
    return;
  }
  // A new tab starts on its first row.
  const Actions actions = waiting_ ? Actions{} : ReadActions();
  if (!waiting_ && actions.back) {
    closing_ = true;
    return;
  }
  if (!waiting_ && actions.tab) {
    tab_ = (tab_ + 1) % 2;
    row_ = 0;
    slot_ = 0;
    if (kind_ == Kind::kController && tab_ == 0) {
      // The first control a player can move.
      for (int i = 0; i < kPadRowCount; ++i) {
        if (kPadRows[i].type == PadRow::kControl && Remappable(kPadRows[i].control)) {
          row_ = i;
          break;
        }
      }
    }
  } else if (kind_ == Kind::kKeyboardMouse) {
    tab_ == 0 ? HandleKeyboardAssignment(actions) : HandleKeyboardSettings(actions);
  } else {
    tab_ == 0 ? HandleControllerAssignment(actions) : HandleControllerSettings(actions);
  }
  if (kind_ == Kind::kController && tab_ == 0 &&
      (kPadRows[row_].type != PadRow::kControl || !Remappable(kPadRows[row_].control))) {
    row_ = 0;  // LT, the first control a player can move
  }

  const float fade = std::min(1.0f, std::chrono::duration<float>(Clock::now() - opened_).count() / kFadeSeconds);
  // Sizes in pixels at 720p, over the 16:9 area the game image takes in the window.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_width = game_height * 16.0f / 9.0f;
  const float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  const float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  unit_ = game_height / 720.0f;
  const float u = unit_;
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const Texts texts = LocalizedTexts(language_);
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  // The game dimmed behind the menu.
  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(150 * fade)));

  const float panel_width = 1080.0f * u;
  const float panel_height = 620.0f * u;
  const float margin = 28.0f * u;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  DrawCutPanel(draw_list, panel_min, panel_max, 14.0f * u, Panel(240), SteelLine(255), 1.0f * u);

  // Title: the category's name, then "Controls" in gold, and the help line.
  std::string category = kind_ == Kind::kKeyboardMouse ? texts.keyboard_title : game_texts_.controller_title;
  if (category.empty()) category = "CONTROLLER";
  const std::string title = category + " \xC2\xB7 ";
  draw_list->AddText(title_font, 26.0f * u, ImVec2(panel_min.x + margin, panel_min.y + 20.0f * u), Text(255), title.c_str());
  const float title_width = title_font->CalcTextSizeA(26.0f * u, FLT_MAX, 0.0f, title.c_str()).x;
  draw_list->AddText(title_font, 26.0f * u, ImVec2(panel_min.x + margin + title_width, panel_min.y + 20.0f * u),
                     Imulsion(255), texts.controls);
  const float controls_width = title_font->CalcTextSizeA(26.0f * u, FLT_MAX, 0.0f, texts.controls).x;
  const float help_left = panel_min.x + margin + title_width + controls_width + 18.0f * u;
  DrawFitted(draw_list, body_font, 13.0f * u, ImVec2(help_left, panel_min.y + 30.0f * u), TextDim(255),
             kind_ == Kind::kKeyboardMouse ? texts.keyboard_help : texts.controller_help,
             panel_max.x - margin - help_left);
  draw_list->AddLine(ImVec2(panel_min.x + margin, panel_min.y + 62.0f * u), ImVec2(panel_max.x - margin, panel_min.y + 62.0f * u),
                     Imulsion(200), 1.25f * u);

  // Tabs.
  const char* tab_names[2] = {texts.assignment, texts.settings};
  float tx = panel_min.x + margin;
  const float tab_y = panel_min.y + 74.0f * u;
  for (int i = 0; i < 2; ++i) {
    const float tw = title_font->CalcTextSizeA(15.0f * u, FLT_MAX, 0.0f, tab_names[i]).x;
    draw_list->AddText(title_font, 15.0f * u, ImVec2(tx, tab_y), i == tab_ ? Imulsion(255) : TextDim(255), tab_names[i]);
    if (i == tab_) draw_list->AddLine(ImVec2(tx, tab_y + 22.0f * u), ImVec2(tx + tw, tab_y + 22.0f * u), Imulsion(255), 2.0f * u);
    tx += tw + 26.0f * u;
  }
  draw_list->AddLine(ImVec2(panel_min.x + margin, tab_y + 24.0f * u), ImVec2(panel_max.x - margin, tab_y + 24.0f * u),
                     SteelLine(255), 1.0f * u);

  const float content_top = tab_y + 38.0f * u;
  const float content_height = panel_max.y - 50.0f * u - content_top;
  const float content_width = panel_width - 2.0f * margin;
  if (kind_ == Kind::kKeyboardMouse && tab_ == 0) {
    DrawKeyboardAssignment(draw_list, panel_min.x + margin, content_top, content_width, content_height);
    draw_list->AddText(body_font, 12.0f * u, ImVec2(panel_min.x + margin, panel_max.y - 34.0f * u), TextFaint(255), texts.fixed);
  } else if (kind_ == Kind::kController && tab_ == 0) {
    DrawControllerAssignment(draw_list, panel_min.x + margin, content_top, content_width, content_height);
  } else {
    DrawSettings(draw_list, panel_min.x + margin, content_top, content_width, content_height);
  }
  DrawHints(draw_list, panel_min.x + margin + 300.0f * u, panel_max.x - margin, panel_max.y - 34.0f * u);
}

void ControlsMenu::OnClose() {
  if (assignment_) {
    assignment_->SetBlocked(false);
  }
  if (owner_) {
    *owner_ = nullptr;
  }
}

}  // namespace azura
