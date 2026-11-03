#include "versus_online_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/xam/alboria.h>

#include "script_hooks.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using rex::input::PadControl;
using Slot = input::PlayerAssignment::Slot;
namespace alboria = rex::system::xam::alboria;

constexpr float kFadeSeconds = 0.18f;
constexpr float kPi = 3.14159265f;
// How far the left stick goes before it counts as a press.
constexpr int kStickPress = 20000;
// The most players a match holds (5 against 5): the search screen's places.
constexpr int kMatchPlaces = 10;
// How long the match found shows before the game takes over with its lobby.
constexpr float kReadySeconds = 3.0f;
// How long "choose at least one mode" shows.
constexpr float kNoModeSeconds = 3.0f;

// Standard's modes: the game's social playlists, one game type each (alboria-notes:azura/
// reverse-engineering.md, "Playlists, experience and achievements"). Horde (16) and Beast (17) are
// not Versus. The search's modes are "playlist:<id>": players sharing one play a match of it, whose
// lobby the host opens with that playlist.
constexpr int kModes = 6;
constexpr std::array<int, kModes> kModePlaylists = {11, 15, 13, 12, 14, 18};

struct Texts {
  const char* section;  // above the title
  const char* title;
  const char* subtitle;
  const char* standard_tag;
  const char* standard;
  const char* standard_help;
  const char* ranked;
  const char* coming_soon;
  const char* ranked_help;
  const char* modes_label;
  std::array<const char*, kModes> modes;  // in kModePlaylists' order
  const char* no_mode;
  const char* search;
  const char* mark;
  const char* back;
  const char* searching;
  std::array<const char*, 4> steps;  // Alboria, the search, the players, ready
  const char* free_place;
  const char* found;  // "%d of %d", then why free places are no worry
  const char* cancel;
  const char* ready_title;
  const char* ready_subtitle;
  const char* bots;  // "+ %d bots"
  const char* entering;
  const char* entering_help;
  const char* error_title;
  const char* unreachable;
  const char* unreachable_help;
  const char* signed_out_help;
  const char* retry;
  const char* enter;  // the Enter key
  const char* space;  // the Space key
};

// In the game's languages; the runtime loads the Cyrillic and CJK fonts each one needs. The modes'
// names are Azura's own, not the game's localization.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"VERSUS · ALBORIA", "オンライン対戦", "世界中のプレイヤーと対戦", "オープンマッチ", "スタンダード",
              "いつでも参加・退出できます · 実績の対象です", "ランクマッチ", "近日公開", "ディビジョンとシーズンランク",
              "モード · 1つ以上",
              {"チームデスマッチ", "キング・オブ・ザ・ヒル", "エクセキューション", "ウォーゾーン", "キャプチャー・ザ・リーダー", "ウィングマン"},
              "モードを1つ以上選んでください", "検索", "モードを選択", "戻る", "検索中",
              {"Alboria", "検索", "プレイヤー", "準備完了"}, "空き", "%d / %d · 空きはボットが埋めます",
              "検索をキャンセル", "マッチ準備完了", "全員接続しました", "+ ボット %d", "投票に移動します",
              "マップとモードは全員で選びます", "オンライン対戦", "Alboria に接続できません",
              "インターネット接続を確認して、もう一度お試しください。",
              "ランチャーから Alboria にサインインしてください。", "もう一度検索", "Enter", "Space"};
    case 7:  // Korean
      return {"VERSUS · ALBORIA", "온라인 대전", "전 세계 플레이어와 대결", "오픈 매치", "스탠다드",
              "언제든 참가하고 나갈 수 있습니다 · 업적에 포함됩니다", "랭크", "출시 예정", "디비전과 시즌 랭크",
              "모드 · 하나 이상", {"팀 데스매치", "킹 오브 더 힐", "처형", "워존", "리더 포획", "윙맨"},
              "모드를 하나 이상 선택하세요", "검색", "모드 선택", "뒤로", "검색 중", {"Alboria", "검색", "플레이어", "준비"},
              "빈자리", "%d / %d · 빈자리는 봇이 채웁니다", "검색 취소", "매치 준비 완료", "모두 연결되었습니다",
              "+ 봇 %d", "투표로 이동합니다", "맵과 모드는 모두가 함께 고릅니다", "온라인 대전",
              "Alboria에 연결할 수 없습니다", "인터넷 연결을 확인하고 다시 시도하세요.",
              "런처에서 Alboria에 로그인하세요.", "다시 검색", "Enter", "Space"};
    case 8:  // Traditional Chinese
      return {"VERSUS · ALBORIA", "線上對戰", "與全世界的玩家對戰", "開放對戰", "標準", "隨時加入或離開 · 計入成就",
              "排名", "即將推出", "分級與賽季排名", "模式 · 一個或多個",
              {"團隊死鬥", "山丘之王", "處決", "戰區", "奪取首領", "僚機"}, "請至少選擇一個模式", "搜尋", "選擇模式",
              "返回", "搜尋中", {"Alboria", "搜尋", "玩家", "就緒"}, "空位", "%d / %d · 空位由機器人補上",
              "取消搜尋", "對戰就緒", "所有人都已連線", "+ %d 個機器人", "正在進入投票", "地圖與模式由大家一起選",
              "線上對戰", "無法連線到 Alboria", "請檢查網路連線後再試一次。", "請從啟動器登入 Alboria。", "再次搜尋",
              "Enter", "Space"};
    case 12:  // Russian
      return {"VERSUS · ALBORIA", "VERSUS ОНЛАЙН", "Сражайтесь с игроками со всего мира", "ОТКРЫТЫЕ МАТЧИ",
              "СТАНДАРТ", "Входите и выходите когда угодно · засчитывается для достижений", "РЕЙТИНГ", "СКОРО",
              "Дивизионы и сезонные ранги", "РЕЖИМЫ · ОДИН ИЛИ НЕСКОЛЬКО",
              {"Командный бой насмерть", "Царь горы", "Казнь", "Зона войны", "Захват лидера", "Напарник"},
              "Выберите хотя бы один режим", "Искать", "Отметить режим", "Назад", "ПОИСК",
              {"Alboria", "Поиск", "Игроки", "Готово"}, "свободно", "%d из %d · свободные места займут боты",
              "Отменить поиск", "МАТЧ ГОТОВ", "Все подключены", "+ ботов: %d", "Переход к голосованию",
              "Карту и режим выбирают все вместе", "VERSUS ОНЛАЙН", "Нет связи с Alboria",
              "Проверьте подключение к интернету и попробуйте снова.", "Войдите в Alboria в лаунчере.",
              "Искать снова", "Enter", "Пробел"};
    case 3:  // German
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Spiele gegen die Welt", "OFFENE MATCHES", "STANDARD",
              "Jederzeit beitreten und gehen · z\xC3\xA4hlt f\xC3\xBCr Erfolge", "RANGLISTE", "DEMN\xC3\x84""CHST",
              "Divisionen und Saisonr\xC3\xA4nge", "MODI · EINER ODER MEHRERE",
              {"Team-Deathmatch", "King of the Hill", "Hinrichtung", "Kriegsgebiet", "Anf\xC3\xBChrer schnappen",
               "Wingman"},
              "W\xC3\xA4hle mindestens einen Modus", "Suchen", "Modus markieren", "Zur\xC3\xBC""ck", "SUCHE",
              {"Alboria", "Suche", "Spieler", "Bereit"}, "frei",
              "%d von %d · freie Pl\xC3\xA4tze \xC3\xBC""bernehmen Bots", "Suche abbrechen", "MATCH BEREIT",
              "Alle sind verbunden", "+ %d Bots", "Weiter zur Abstimmung", "Karte und Modus w\xC3\xA4hlen alle gemeinsam",
              "VERSUS ONLINE", "Keine Verbindung zu Alboria",
              "Pr\xC3\xBC""fe deine Internetverbindung und versuche es erneut.",
              "Melde dich im Launcher bei Alboria an.", "Erneut suchen", "Eingabe", "Leertaste"};
    case 4:  // French
      return {"VERSUS · ALBORIA", "VERSUS EN LIGNE", "Affrontez le monde entier", "PARTIES OUVERTES", "STANDARD",
              "Rejoignez et quittez quand vous voulez · compte pour les succ\xC3\xA8s", "CLASS\xC3\x89""E",
              "BIENT\xC3\x94T", "Divisions et rangs de saison", "MODES · UN OU PLUSIEURS",
              {"Match \xC3\xA0 mort en \xC3\xA9quipe", "Roi de la colline", "Ex\xC3\xA9""cution", "Zone de guerre",
               "Capture du chef", "Co\xC3\xA9quipier"},
              "Choisissez au moins un mode", "Rechercher", "Cocher le mode", "Retour", "RECHERCHE",
              {"Alboria", "Recherche", "Joueurs", "Pr\xC3\xAAt"}, "libre",
              "%d sur %d · les places libres seront prises par des bots", "Annuler la recherche", "PARTIE PR\xC3\x8ATE",
              "Tout le monde est connect\xC3\xA9", "+ %d bots", "Passage au vote",
              "La carte et le mode se choisissent ensemble", "VERSUS EN LIGNE", "Impossible de joindre Alboria",
              "V\xC3\xA9rifiez votre connexion internet et r\xC3\xA9""essayez.",
              "Connectez-vous \xC3\xA0 Alboria depuis le launcher.", "Rechercher \xC3\xA0 nouveau", "Entr\xC3\xA9""e",
              "Espace"};
    case 5:  // Spanish
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Juega contra el mundo", "PARTIDAS ABIERTAS", "EST\xC3\x81NDAR",
              "Entra y sal cuando quieras · cuenta para los logros", "CLASIFICATORIA", "PR\xC3\x93XIMAMENTE",
              "Divisiones y rangos de temporada", "MODOS · UNO O VARIOS",
              {"Duelo por equipos", "Rey de la colina", "Ejecuci\xC3\xB3n", "Zona de guerra", "Capturar al l\xC3\xAD""der",
               "Compa\xC3\xB1""eros"},
              "Elige al menos un modo", "Buscar", "Marcar modo", "Atr\xC3\xA1s", "BUSCANDO",
              {"Alboria", "B\xC3\xBAsqueda", "Jugadores", "Listos"}, "libre",
              "%d de %d · los huecos libres al empezar los ocupan bots", "Cancelar b\xC3\xBAsqueda",
              "PARTIDA LISTA", "Todos conectados", "+ %d bots", "Entrando en la votaci\xC3\xB3n",
              "Mapa y modo se eligen entre todos", "VERSUS ONLINE", "No se puede conectar con Alboria",
              "Comprueba tu conexi\xC3\xB3n a internet y vuelve a intentarlo.",
              "Inicia sesi\xC3\xB3n en Alboria desde el launcher.", "Buscar otra vez", "Intro", "Espacio"};
    case 6:  // Italian
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Gioca contro il mondo", "PARTITE APERTE", "STANDARD",
              "Entra ed esci quando vuoi · conta per gli obiettivi", "CLASSIFICATA", "PROSSIMAMENTE",
              "Divisioni e gradi stagionali", "MODALIT\xC3\x80 · UNA O PI\xC3\x99",
              {"Deathmatch a squadre", "Re della collina", "Esecuzione", "Zona di guerra", "Cattura il leader",
               "Spalla"},
              "Scegli almeno una modalit\xC3\xA0", "Cerca", "Segna modalit\xC3\xA0", "Indietro", "RICERCA",
              {"Alboria", "Ricerca", "Giocatori", "Pronti"}, "libero",
              "%d di %d · i posti liberi saranno presi dai bot", "Annulla ricerca", "PARTITA PRONTA",
              "Tutti connessi", "+ %d bot", "Si passa alla votazione", "Mappa e modalit\xC3\xA0 si scelgono insieme",
              "VERSUS ONLINE", "Impossibile raggiungere Alboria",
              "Controlla la connessione a internet e riprova.", "Accedi ad Alboria dal launcher.", "Cerca di nuovo",
              "Invio", "Spazio"};
    case 9:  // Portuguese
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Jogue contra o mundo", "PARTIDAS ABERTAS", "PADR\xC3\x83O",
              "Entre e saia quando quiser · conta para as conquistas", "RANQUEADA", "EM BREVE",
              "Divis\xC3\xB5""es e patentes da temporada", "MODOS · UM OU MAIS",
              {"Mata-mata em equipe", "Rei da colina", "Execu\xC3\xA7\xC3\xA3o", "Zona de guerra", "Capturar o l\xC3\xAD""der",
               "Parceiro"},
              "Escolha pelo menos um modo", "Buscar", "Marcar modo", "Voltar", "BUSCANDO",
              {"Alboria", "Busca", "Jogadores", "Prontos"}, "livre",
              "%d de %d · as vagas livres ser\xC3\xA3o ocupadas por bots", "Cancelar busca", "PARTIDA PRONTA",
              "Todos conectados", "+ %d bots", "Indo para a vota\xC3\xA7\xC3\xA3o", "Mapa e modo s\xC3\xA3o escolhidos por todos",
              "VERSUS ONLINE", "N\xC3\xA3o foi poss\xC3\xADvel conectar ao Alboria",
              "Verifique sua conex\xC3\xA3o com a internet e tente de novo.", "Entre no Alboria pelo launcher.",
              "Buscar de novo", "Enter", "Espa\xC3\xA7o"};
    case 11:  // Polish
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Zagraj przeciwko \xC5\x9Bwiatu", "OTWARTE MECZE", "STANDARD",
              "Do\xC5\x82\xC4\x85""czaj i wychod\xC5\xBA kiedy chcesz · liczy si\xC4\x99 do osi\xC4\x85gni\xC4\x99\xC4\x87",
              "RANKINGOWY", "WKR\xC3\x93TCE", "Dywizje i rangi sezonowe", "TRYBY · JEDEN LUB WI\xC4\x98""CEJ",
              {"Dru\xC5\xBCynowy deathmatch", "Kr\xC3\xB3l wzg\xC3\xB3rza", "Egzekucja", "Strefa wojny",
               "Schwytaj przyw\xC3\xB3""dc\xC4\x99", "Skrzyd\xC5\x82owy"},
              "Wybierz co najmniej jeden tryb", "Szukaj", "Zaznacz tryb", "Wr\xC3\xB3\xC4\x87", "SZUKANIE",
              {"Alboria", "Szukanie", "Gracze", "Gotowi"}, "wolne",
              "%d z %d · wolne miejsca zajm\xC4\x85 boty", "Anuluj szukanie", "MECZ GOTOWY",
              "Wszyscy po\xC5\x82\xC4\x85""czeni", "+ %d bot\xC3\xB3w", "Przechodzimy do g\xC5\x82osowania",
              "Map\xC4\x99 i tryb wybieraj\xC4\x85 wszyscy", "VERSUS ONLINE", "Brak po\xC5\x82\xC4\x85""czenia z Albori\xC4\x85",
              "Sprawd\xC5\xBA po\xC5\x82\xC4\x85""czenie z internetem i spr\xC3\xB3""buj ponownie.",
              "Zaloguj si\xC4\x99 do Alborii w launcherze.", "Szukaj ponownie", "Enter", "Spacja"};
    default:
      return {"VERSUS · ALBORIA", "VERSUS ONLINE", "Play against the world", "OPEN MATCHES", "STANDARD",
              "Join and leave at any time · counts for achievements", "RANKED", "COMING SOON",
              "Divisions and season ranks", "MODES · ONE OR MORE",
              {"Team Deathmatch", "King of the Hill", "Execution", "Warzone", "Capture the Leader", "Wingman"},
              "Choose at least one mode", "Search", "Mark mode", "Back", "SEARCHING",
              {"Alboria", "Search", "Players", "Ready"}, "free",
              "%d of %d · free places at the start are taken by bots", "Cancel search", "MATCH READY",
              "Everyone connected", "+ %d bots", "Entering the vote", "Map and mode are chosen by everyone",
              "VERSUS ONLINE", "Cannot reach Alboria", "Check your internet connection and try again.",
              "Sign in to Alboria from the launcher.", "Search again", "Enter", "Space"};
  }
}

// --- The search -----------------------------------------------------------------------------------
//
// It runs on a thread of its own (alboria::Matchmake blocks until Alboria decides) and outlives the
// menu: what it decides is done by the game on its next input frame (script::RequestMatchmadeLobby,
// RequestMatchInvite), whether the menu is still open or not.

// Standard's modes the player marked, kept while the game runs. Team Deathmatch, King of the Hill and
// Execution at first.
std::array<bool, kModes> g_marked = {true, true, true, false, false, false};
std::atomic<bool> g_search_running{false};
// The player cancelled the search running: whatever it decides is not acted on.
std::atomic<bool> g_search_cancelled{false};
// When the search running started, for the clock.
std::atomic<int64_t> g_search_started{0};

int64_t NowTicks() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

// Searches for a match of the modes marked. The game then opens the lobby of the playlist Alboria
// decided on (this player hosts: never said as such), or takes the match's invite.
void StartSearch() {
  if (g_search_running.exchange(true)) return;
  std::vector<std::string> modes;
  for (int i = 0; i < kModes; ++i) {
    if (g_marked[i]) modes.push_back("playlist:" + std::to_string(kModePlaylists[i]));
  }
  g_search_cancelled.store(false);
  g_search_started.store(NowTicks());
  alboria::ResetSearchStatus();
  alboria::SetSearchModes(modes);
  std::thread([] {
    alboria::Matchmake("versus");
    const alboria::SearchStatus status = alboria::GetSearchStatus();
    alboria::SetSearchModes({});
    if (!g_search_cancelled.load()) {
      if (status.phase == alboria::SearchPhase::kHost) {
        int playlist = -1;
        if (std::sscanf(status.mode.c_str(), "playlist:%d", &playlist) == 1 && playlist > 0) {
          script::RequestMatchmadeLobby(playlist);
        } else {
          REXLOG_WARN("[Azura online] Versus online: Alboria decided on an unknown mode ({})", status.mode);
        }
      } else if (status.phase == alboria::SearchPhase::kJoin) {
        script::RequestMatchInvite();
      }
    }
    g_search_running.store(false);
  }).detach();
}

void CancelSearch() {
  if (!g_search_running.load()) return;
  g_search_cancelled.store(true);
  alboria::StopMatchmaking();
}

// --- Drawing --------------------------------------------------------------------------------------

ImVec2 Rotate(ImVec2 point, ImVec2 center, float angle) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const float x = point.x - center.x;
  const float y = point.y - center.y;
  return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
}

// A cog, the menu's background mark, `radius` to its teeth's tips, turned by `angle`.
void DrawCog(ImDrawList* draw_list, ImVec2 center, float radius, float angle, ImU32 color) {
  const float ring = radius * 0.72f;
  const float thickness = radius * 0.14f;
  draw_list->AddCircle(center, ring, color, 64, thickness);
  draw_list->AddCircle(center, radius * 0.33f, color, 48, thickness);
  constexpr int kTeeth = 12;
  const float half = radius * 0.11f;
  for (int i = 0; i < kTeeth; ++i) {
    const float a = angle + 2.0f * kPi * i / kTeeth;
    const ImVec2 points[4] = {
        Rotate(ImVec2(center.x - half, center.y - ring), center, a),
        Rotate(ImVec2(center.x + half, center.y - ring), center, a),
        Rotate(ImVec2(center.x + half * 0.8f, center.y - radius), center, a),
        Rotate(ImVec2(center.x - half * 0.8f, center.y - radius), center, a),
    };
    draw_list->AddConvexPolyFilled(points, 4, color);
  }
}

// A square on its corner, `size` across: filled when `filled`.
void DrawDiamond(ImDrawList* draw_list, ImVec2 center, float size, ImU32 line, ImU32 fill, bool filled, float thickness) {
  const float r = size * 0.5f;
  const ImVec2 points[4] = {{center.x, center.y - r}, {center.x + r, center.y}, {center.x, center.y + r}, {center.x - r, center.y}};
  if (filled) draw_list->AddConvexPolyFilled(points, 4, fill);
  draw_list->AddPolyline(points, 4, line, ImDrawFlags_Closed, thickness);
}

// --- Alboriano on the radar ------------------------------------------------------------------------
//
// The radar's blips are glyphs of Alboriano, Alboria's writing (alboria-notes:alboria-web/
// alboriano.md; its source is alboria-web's src/alboriano/glyphs.mjs): while dark they keep changing,
// and as the sweep lights them they settle on one, as if the search were decoding it. Only the
// glyphs made of lines, circles, quadratic curves and dots are here, in the source's units (a 24 by
// 24 box centred on 0,0).

struct GlyphPart {
  enum Kind { kLine, kCircle, kQuad, kDot, kTriangle } kind;
  // Line: x0 y0 x1 y1; circle: cx cy r; curve: x0 y0 control x1 y1 (the control as cx cy); dot: x
  // y; triangle: three points.
  float v[6];
};
struct Glyph {
  int parts;
  GlyphPart part[6];
};

constexpr Glyph kGlyphs[] = {
    {2, {{GlyphPart::kCircle, {0, 0, 7}}, {GlyphPart::kLine, {-11, 4, 11, -4}}}},                        // a
    {2, {{GlyphPart::kCircle, {0, 3, 5.5f}}, {GlyphPart::kDot, {0, -8.5f}}}},                              // i
    {2, {{GlyphPart::kCircle, {0, 0, 7.5f}}, {GlyphPart::kDot, {0, 0}}}},                                  // o
    {2, {{GlyphPart::kCircle, {0, -3, 5}}, {GlyphPart::kQuad, {-9, 3, 0, 13, 9, 3}}}},                     // u
    {2, {{GlyphPart::kLine, {0, -9, 0, 9}}, {GlyphPart::kQuad, {-7, -4, 0, 2, 7, -4}}}},                   // b
    {2, {{GlyphPart::kQuad, {-9, -6, 0, 6, 9, -6}}, {GlyphPart::kQuad, {-9, 6, 0, -6, 9, 6}}}},           // g
    {3, {{GlyphPart::kLine, {-8, -8, 8, 8}}, {GlyphPart::kLine, {8, -8, 2, -2}}, {GlyphPart::kCircle, {-4, 4, 3}}}},  // k
    {2, {{GlyphPart::kQuad, {-9, 8, -2, -2, 9, -8}}, {GlyphPart::kDot, {5, 5}}}},                          // l
    {2, {{GlyphPart::kQuad, {-9, 0, -4.5f, -9, 0, 0}}, {GlyphPart::kQuad, {0, 0, 4.5f, 9, 9, 0}}}},        // n
    {3, {{GlyphPart::kLine, {-7, 8, -1, -8}}, {GlyphPart::kLine, {1, 8, 7, -8}}, {GlyphPart::kDot, {-6, -6}}}},  // r
    {5, {{GlyphPart::kDot, {-7, 0}}, {GlyphPart::kDot, {0, 0}}, {GlyphPart::kDot, {7, 0}},
         {GlyphPart::kLine, {-9, -7, 9, -7}}, {GlyphPart::kLine, {-9, 7, 9, 7}}}},                          // s
    {2, {{GlyphPart::kTriangle, {0, -9, 8, 7, -8, 7}}, {GlyphPart::kDot, {0, 2}}}},                       // t
    {2, {{GlyphPart::kQuad, {-8, -6, 0, 12, 8, -6}}, {GlyphPart::kDot, {0, -6}}}},                         // v
    {2, {{GlyphPart::kQuad, {-8, -8, 0, 0, 8, -8}}, {GlyphPart::kQuad, {-8, 0, 0, 8, 8, 0}}}},             // w
    {3, {{GlyphPart::kLine, {-9, -3, -3, -9}}, {GlyphPart::kLine, {-5, 5, 5, -5}}, {GlyphPart::kLine, {3, 9, 9, 3}}}},  // x
    {3, {{GlyphPart::kLine, {0, -9, 0, 9}}, {GlyphPart::kDot, {-6, -3}}, {GlyphPart::kDot, {6, 3}}}},      // y
    {4, {{GlyphPart::kQuad, {-8, -5, -4, -10, 0, -5}}, {GlyphPart::kQuad, {0, -5, 4, 0, 8, -5}},
         {GlyphPart::kQuad, {-8, 5, -4, 0, 0, 5}}, {GlyphPart::kQuad, {0, 5, 4, 10, 8, 5}}}},               // z
    {1, {{GlyphPart::kCircle, {0, 0, 4.5f}}}},                                                              // 0
    {3, {{GlyphPart::kLine, {-4.5f, -8, -4.5f, 8}}, {GlyphPart::kLine, {0, -8, 0, 8}}, {GlyphPart::kLine, {4.5f, -8, 4.5f, 8}}}},  // 3
    {2, {{GlyphPart::kQuad, {-8, 2, 0, 12, 8, 2}}, {GlyphPart::kLine, {0, -9, 0, -2}}}},                   // 6
};
constexpr int kGlyphCount = static_cast<int>(sizeof(kGlyphs) / sizeof(kGlyphs[0]));

// Draws glyph `glyph` of kGlyphs centred on `center`, `size` across (the source's 24 units).
void DrawAlboriano(ImDrawList* draw_list, int glyph, ImVec2 center, float size, ImU32 color, float thickness) {
  const float k = size / 24.0f;
  auto p = [&](float x, float y) { return ImVec2(center.x + x * k, center.y + y * k); };
  const Glyph& g = kGlyphs[glyph % kGlyphCount];
  for (int i = 0; i < g.parts; ++i) {
    const GlyphPart& part = g.part[i];
    const float* v = part.v;
    switch (part.kind) {
      case GlyphPart::kLine: draw_list->AddLine(p(v[0], v[1]), p(v[2], v[3]), color, thickness); break;
      case GlyphPart::kCircle: draw_list->AddCircle(p(v[0], v[1]), v[2] * k, color, 24, thickness); break;
      case GlyphPart::kQuad: draw_list->AddBezierQuadratic(p(v[0], v[1]), p(v[2], v[3]), p(v[4], v[5]), color, thickness, 12); break;
      case GlyphPart::kDot: draw_list->AddCircleFilled(p(v[0], v[1]), 2.6f * k, color, 10); break;
      case GlyphPart::kTriangle: draw_list->AddTriangle(p(v[0], v[1]), p(v[2], v[3]), p(v[4], v[5]), color, thickness); break;
    }
  }
}

// A small hash, for the glyphs' places and the ones they show.
uint32_t Mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// The glyph of kGlyphs that writes `c`: its own letter's or digit's when kGlyphs has it, else one
// chosen by the character (the same each time), for the letters not here and other scripts.
int GlyphOf(uint32_t c) {
  if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  constexpr const char kWritten[] = "aioubgklnrstvwxyz036";
  for (int i = 0; kWritten[i]; ++i) {
    if (static_cast<uint32_t>(kWritten[i]) == c) return i;
  }
  return static_cast<int>(Mix(c * 2654435761u) % kGlyphCount);
}

// `text` as a cipher being broken: each character shows as itself (deciphered) or as a glyph of
// Alboriano, `share` of them deciphered (0 none, 1 all). Which ones are is partly fixed by the
// character's place and partly flickers with time `t`, so the text seems half read and lost again.
// Each character takes the width of its letter or of a glyph, whichever is wider, so the text does
// not move as it changes. `seed` tells texts apart. Returns its width; with no `draw_list`, only
// measures.
float AddCipherText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text,
                    float spacing, float share, uint32_t seed, float t) {
  const float glyph_size = size * 0.85f;
  float x = at.x;
  uint32_t index = 0;
  for (const char* p = text; *p; ++index) {
    const char* next = p + 1;
    while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    // The character's code point, for its glyph.
    uint32_t c = static_cast<unsigned char>(*p);
    if (c >= 0xC0) {
      const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      c &= 0x3F >> extra;
      for (int k = 1; k <= extra && p[k]; ++k) c = (c << 6) | (static_cast<unsigned char>(p[k]) & 0x3F);
    }
    const float letter_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, next).x;
    const float width = std::max(letter_width, glyph_size * 0.72f);
    const bool space = c == ' ';
    const float settled = static_cast<float>(Mix(seed * 977u + index) % 1000) / 1000.0f;
    const float flicker = static_cast<float>(Mix(seed * 31u + index * 7919u + static_cast<uint32_t>(t * 5.0f)) % 1000) / 1000.0f;
    const bool deciphered = share >= 1.0f || settled < share || flicker < share * 0.45f;
    if (draw_list && !space) {
      if (deciphered) {
        draw_list->AddText(font, size, ImVec2(x + (width - letter_width) * 0.5f, at.y), color, p, next);
      } else {
        DrawAlboriano(draw_list, GlyphOf(c), ImVec2(x + width * 0.5f, at.y + size * 0.55f), glyph_size, color,
                      std::max(1.0f, size * 0.09f));
      }
    }
    x += width + spacing;
    p = next;
  }
  return x - at.x - spacing;
}

// A hexagon with a dot, a player's emblem, `size` across.
void DrawEmblem(ImDrawList* draw_list, ImVec2 center, float size, ImU32 color, float thickness) {
  ImVec2 points[6];
  for (int i = 0; i < 6; ++i) {
    const float a = kPi / 6.0f + i * kPi / 3.0f;
    points[i] = ImVec2(center.x + std::cos(a) * size * 0.5f, center.y + std::sin(a) * size * 0.5f);
  }
  draw_list->AddConvexPolyFilled(points, 6, (color & 0x00FFFFFF) | (((color >> 24) * 45 / 255) << 24));
  draw_list->AddPolyline(points, 6, color, ImDrawFlags_Closed, thickness);
  draw_list->AddCircleFilled(center, size * 0.13f, color);
}

// A dashed rectangle, an empty place.
void DrawDashedRect(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImU32 color, float dash, float thickness) {
  auto line = [&](ImVec2 a, ImVec2 b) {
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    for (float t = 0.0f; t < length; t += dash * 2.0f) {
      const float e = std::min(length, t + dash);
      draw_list->AddLine(ImVec2(a.x + (b.x - a.x) * t / length, a.y + (b.y - a.y) * t / length),
                         ImVec2(a.x + (b.x - a.x) * e / length, a.y + (b.y - a.y) * e / length), color, thickness);
    }
  };
  line(min, ImVec2(max.x, min.y));
  line(ImVec2(max.x, min.y), max);
  line(max, ImVec2(min.x, max.y));
  line(ImVec2(min.x, max.y), min);
}

// `text` with its letters spaced by `spacing`; returns its width.
float AddSpacedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text,
                    float spacing) {
  float x = at.x;
  for (const char* p = text; *p;) {
    const char* next = p + 1;
    while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
    if (draw_list) draw_list->AddText(font, size, ImVec2(x, at.y), color, p, next);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, next).x + spacing;
    p = next;
  }
  return x - at.x - spacing;
}

}  // namespace

VersusOnlineMenu::VersusOnlineMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                                   input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                                   ImFont* body_font, VersusOnlineMenu** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner),
      opened_(Clock::now()),
      last_input_(input),
      screen_since_(Clock::now()) {
  if (assignment_) assignment_->SetBlocked(true);
  alboria::RefreshIdentity();
  // A search still running (the menu was closed meanwhile) shows again.
  if (g_search_running.load()) screen_ = Screen::kSearching;
  script::PlayMenuSound(script::MenuSound::kOpen);
}

VersusOnlineMenu::Presses VersusOnlineMenu::ReadInput() {
  Presses presses;
  for (const Slot& slot : assignment_->Slots()) {
    if (slot.keyboard) continue;
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
    uint16_t buttons = state.gamepad.buttons;
    if (state.gamepad.thumb_ly >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
    if (state.gamepad.thumb_ly <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
    auto found = previous_buttons_.find(slot.device.id);
    const bool seen = found != previous_buttons_.end();
    const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
    previous_buttons_[slot.device.id] = buttons;
    if (pressed) last_input_.NotePad(slot.device.id);
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) presses.move = -1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) presses.move = 1;
    if (pressed & rex::input::X_INPUT_GAMEPAD_A) presses.accept = true;
    if (pressed & rex::input::X_INPUT_GAMEPAD_X) presses.mark = true;
    if (pressed & rex::input::X_INPUT_GAMEPAD_B) presses.back = true;
  }
  // The keyboard, through ImGui's keys (the game's keyboard input is blocked).
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_W, false)) presses.move = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_S, false)) presses.move = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    presses.accept = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) presses.mark = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) presses.back = true;
  return presses;
}

void VersusOnlineMenu::Search() {
  if (std::none_of(g_marked.begin(), g_marked.end(), [](bool marked) { return marked; })) {
    no_mode_since_ = Clock::now();
    script::PlayMenuSound(script::MenuSound::kBack);
    return;
  }
  script::PlayMenuSound(script::MenuSound::kAccept);
  if (!alboria::LiveSignedIn()) {
    error_ = "signed_out";
    screen_ = Screen::kError;
    screen_since_ = Clock::now();
    return;
  }
  StartSearch();
  screen_ = Screen::kSearching;
  screen_since_ = Clock::now();
}

void VersusOnlineMenu::Update(const Presses& presses) {
  switch (screen_) {
    case Screen::kChoose:
      if (presses.back) {
        script::PlayMenuSound(script::MenuSound::kBack);
        closing_ = true;
      } else if (presses.accept) {
        Search();
      } else if (presses.mark) {
        script::PlayMenuSound(script::MenuSound::kAccept);
        g_marked[focus_] = !g_marked[focus_];
      } else if (presses.move) {
        script::PlayMenuSound(presses.move < 0 ? script::MenuSound::kUp : script::MenuSound::kDown);
        focus_ = (focus_ + presses.move + kModes) % kModes;
      }
      break;
    case Screen::kSearching: {
      if (presses.back) {
        script::PlayMenuSound(script::MenuSound::kBack);
        CancelSearch();
        screen_ = Screen::kChoose;
        screen_since_ = Clock::now();
        break;
      }
      const alboria::SearchStatus status = alboria::GetSearchStatus();
      if (status.phase == alboria::SearchPhase::kHost || status.phase == alboria::SearchPhase::kJoin) {
        script::PlayMenuSound(script::MenuSound::kAccept);
        screen_ = Screen::kReady;
        screen_since_ = Clock::now();
      } else if (status.phase == alboria::SearchPhase::kFailed) {
        error_ = status.error;
        screen_ = Screen::kError;
        screen_since_ = Clock::now();
      }
      break;
    }
    case Screen::kReady:
      // The game takes over with its lobby.
      if (std::chrono::duration<float>(Clock::now() - screen_since_).count() >= kReadySeconds) Close();
      break;
    case Screen::kError:
      if (presses.back) {
        script::PlayMenuSound(script::MenuSound::kBack);
        screen_ = Screen::kChoose;
        screen_since_ = Clock::now();
      } else if (presses.accept && !g_search_running.load()) {
        Search();
      }
      break;
  }
}

void VersusOnlineMenu::OnDraw(ImGuiIO& io) {
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
  Update(ReadInput());
  if (closing_) return;

  const Clock::time_point now = Clock::now();
  const float t = std::chrono::duration<float>(now - opened_).count();
  const float fade = std::min(1.0f, t / kFadeSeconds);
  auto a = [&](int alpha) { return static_cast<int>(alpha * fade); };

  // Sizes in pixels at 720p, over the 16:9 area the game image takes in the window.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_width = game_height * 16.0f / 9.0f;
  const float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  const float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  const float u = game_height / 720.0f;
  auto at = [&](float x, float y) { return ImVec2(game_left + x * u, game_top + y * u); };
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  const Texts texts = LocalizedTexts(language_);
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  // The backdrop: the game darkened, a cog turning slowly while it searches, veins of Imulsion
  // across the bottom and an amber line along the top.
  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(6, 7, 9, a(248)));
  const float cog_turn = screen_ == Screen::kSearching ? t * 0.25f : 0.0f;
  DrawCog(draw_list, at(1250, 610), 170.0f * u, cog_turn, IM_COL32(24, 27, 31, a(255)));
  const ImVec2 vein[6] = {at(0, 610), at(240, 540), at(350, 575), at(560, 495), at(740, 525), at(1280, 420)};
  draw_list->AddPolyline(vein, 6, Imulsion(a(80)), 0, 1.6f * u);
  const ImVec2 vein2[6] = {at(0, 655), at(190, 615), at(390, 640), at(610, 580), at(810, 600), at(1280, 540)};
  draw_list->AddPolyline(vein2, 6, Imulsion(a(34)), 0, 1.2f * u);
  draw_list->AddLine(at(350, 575), at(400, 680), Imulsion(a(40)), 1.0f * u);
  draw_list->AddLine(at(740, 525), at(790, 680), Imulsion(a(40)), 1.0f * u);
  draw_list->AddRectFilled(at(0, 0), at(1280, 4), Imulsion(a(220)));

  // The header: the section, the screen's title and what it is about, an amber stroke under them;
  // the player's Alboria name on the right.
  const char* title = texts.title;
  std::string subtitle = texts.subtitle;
  if (screen_ == Screen::kSearching) {
    title = texts.searching;
    subtitle.clear();
    for (int i = 0; i < kModes; ++i) {
      if (!g_marked[i]) continue;
      if (!subtitle.empty()) subtitle += " · ";
      subtitle += texts.modes[i];
    }
  } else if (screen_ == Screen::kReady) {
    title = texts.ready_title;
    subtitle = texts.ready_subtitle;
  } else if (screen_ == Screen::kError) {
    title = texts.error_title;
    subtitle.clear();
  }
  AddSpacedText(draw_list, body_font, 12.0f * u, at(64, 44), TextFaint(a(255)), texts.section, 2.4f * u);
  AddSpacedText(draw_list, title_font, 36.0f * u, at(64, 62), Text(a(255)), title, 3.0f * u);
  if (!subtitle.empty()) {
    draw_list->AddText(body_font, 14.0f * u, at(64, 106), TextDim(a(255)),
                       Fit(body_font, 14.0f * u, subtitle, 640.0f * u).c_str());
  }
  draw_list->AddRectFilled(at(64, 132), at(128, 135), Imulsion(a(255)));
  const alboria::Identity identity = alboria::GetIdentity();
  const std::string me = identity.known ? identity.gamertag + "#" + std::to_string(identity.tag) : std::string();
  if (identity.known) {
    const std::string tag = "#" + std::to_string(identity.tag);
    const float name_width = title_font->CalcTextSizeA(16.0f * u, FLT_MAX, 0.0f, identity.gamertag.c_str()).x;
    const float tag_width = title_font->CalcTextSizeA(16.0f * u, FLT_MAX, 0.0f, tag.c_str()).x;
    const ImVec2 right = at(1216, 50);
    draw_list->AddText(title_font, 16.0f * u, ImVec2(right.x - tag_width - name_width - 18.0f * u, right.y), Text(a(255)),
                       identity.gamertag.c_str());
    draw_list->AddText(title_font, 16.0f * u, ImVec2(right.x - tag_width - 18.0f * u, right.y), Imulsion(a(255)),
                       tag.c_str());
    DrawDiamond(draw_list, ImVec2(right.x - 5.0f * u, right.y + 9.0f * u), 9.0f * u, Ok(a(255)), Ok(a(255)), true, 1.0f * u);
  }

  // The steps of a search, as the game's matchmaking panel had them: Alboria, the search, the
  // players, ready; `current` lit, those before it done.
  auto draw_steps = [&](ImVec2 left, float width, int current) {
    constexpr int kSteps = 4;
    float x = left.x;
    for (int i = 0; i < kSteps; ++i) {
      const bool done = i < current;
      const bool now_step = i == current;
      const float pulse = now_step ? 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 6.0f)) : 1.0f;
      const ImU32 color = done ? Ok(a(255)) : now_step ? Imulsion(a(static_cast<int>(255 * pulse))) : SteelLine(a(255));
      DrawDiamond(draw_list, ImVec2(x + 5.0f * u, left.y + 8.0f * u), 10.0f * u, color, color, done || now_step, 1.4f * u);
      x += 16.0f * u;
      // The step's name: deciphered once done, half read while it goes on, in Alboriano before.
      const float share = done ? 1.0f : now_step ? 0.5f + 0.25f * std::sin(t * 1.3f) : 0.12f;
      const float label_width = AddCipherText(draw_list, title_font, 12.0f * u, ImVec2(x, left.y),
                                              done || now_step ? Text(a(255)) : TextFaint(a(255)), texts.steps[i], 0.8f * u,
                                              share, static_cast<uint32_t>(i + 1), t);
      x += label_width + 10.0f * u;
      if (i < kSteps - 1) {
        const float segment = (left.x + width - x) / static_cast<float>(kSteps - 1 - i) - 76.0f * u;
        const float end = x + std::max(14.0f * u, segment);
        draw_list->AddLine(ImVec2(x, left.y + 8.0f * u), ImVec2(end, left.y + 8.0f * u), done ? Ok(a(200)) : SteelLine(a(255)),
                           1.0f * u);
        x = end + 10.0f * u;
      }
    }
  };

  // The places of a match: the players found (this player's emblem in amber), then the free ones.
  auto draw_places = [&](ImVec2 top_left, float width, int columns, const std::vector<std::string>& players,
                         int bots) {
    const float gap = 8.0f * u;
    const float place_width = (width - gap * (columns - 1)) / columns;
    const float place_height = 40.0f * u;
    const int shown = std::min(kMatchPlaces, static_cast<int>(players.size()));
    const int places = bots >= 0 ? shown + (bots > 0 ? 1 : 0) : kMatchPlaces;
    for (int i = 0; i < places; ++i) {
      const ImVec2 min(top_left.x + (i % columns) * (place_width + gap), top_left.y + (i / columns) * (place_height + gap));
      const ImVec2 max(min.x + place_width, min.y + place_height);
      const float appear = std::min(1.0f, std::max(0.0f, (std::chrono::duration<float>(now - screen_since_).count() - i * 0.06f) / 0.3f));
      if (i < shown) {
        const int alpha = a(static_cast<int>(255 * appear));
        DrawCutPanel(draw_list, min, max, 8.0f * u, PanelSolid(alpha), SteelLine(alpha), 1.0f * u);
        const std::string& name = players[i];
        const bool mine = name == me;
        DrawEmblem(draw_list, ImVec2(min.x + 22.0f * u, (min.y + max.y) * 0.5f), 20.0f * u, mine ? Imulsion(alpha) : TextDim(alpha), 1.3f * u);
        const size_t hash = name.rfind('#');
        const std::string gamertag = Fit(title_font, 15.0f * u, name.substr(0, hash), place_width - 110.0f * u);
        const ImVec2 text_at(min.x + 42.0f * u, min.y + 11.0f * u);
        draw_list->AddText(title_font, 15.0f * u, text_at, Text(alpha), gamertag.c_str());
        if (hash != std::string::npos) {
          const float w = title_font->CalcTextSizeA(15.0f * u, FLT_MAX, 0.0f, gamertag.c_str()).x;
          draw_list->AddText(title_font, 15.0f * u, ImVec2(text_at.x + w, text_at.y), TextFaint(alpha), name.c_str() + hash);
        }
      } else if (bots >= 0) {
        char text[48];
        std::snprintf(text, sizeof(text), texts.bots, bots);
        DrawDashedRect(draw_list, min, max, SteelLine(a(255)), 5.0f * u, 1.0f * u);
        AddTextCentered(draw_list, title_font, 13.0f * u, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), TextFaint(a(255)), text);
      } else {
        // An empty place, scanned now and then: a band crosses it left to right, waking glyphs of
        // Alboriano that change as it passes and fade behind it. Each place has its own rhythm.
        constexpr float kScanEvery = 4.5f;
        constexpr float kScanTakes = 1.4f;
        const float scan = std::fmod(t + static_cast<float>(Mix(i + 11) % 4500) / 1000.0f, kScanEvery) / kScanTakes;
        const float band = 70.0f * u;
        const float band_x = min.x - band + scan * (max.x - min.x + 2.0f * band);
        const bool scanning = scan < 1.0f;
        const float pulse = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(t * 3.6f + i * 0.7f));
        DrawDashedRect(draw_list, min, max, SteelLine(a(255)), 5.0f * u, 1.0f * u);
        if (scanning) {
          draw_list->PushClipRect(min, max, true);
          const float step = 22.0f * u;
          const float middle = (min.y + max.y) * 0.5f;
          for (float gx = min.x + step * 0.5f; gx < max.x; gx += step) {
            // Lit behind the band's edge, fading out over a band's width.
            const float behind = band_x - gx;
            const float light = behind < 0.0f ? std::max(0.0f, 1.0f + behind / (12.0f * u)) : std::max(0.0f, 1.0f - behind / band);
            if (light <= 0.0f) continue;
            const int glyph = static_cast<int>(Mix(static_cast<uint32_t>(gx) * 7u + i * 131u + static_cast<uint32_t>(t * 12.0f)) % kGlyphCount);
            DrawAlboriano(draw_list, glyph, ImVec2(gx, middle), 14.0f * u, Imulsion(a(static_cast<int>(200 * light))), 1.2f * u);
          }
          draw_list->AddLine(ImVec2(band_x, min.y + 2.0f * u), ImVec2(band_x, max.y - 2.0f * u), Imulsion(a(170)), 1.2f * u);
          draw_list->PopClipRect();
        }
        AddTextCentered(draw_list, body_font, 13.0f * u, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                        TextFaint(a(static_cast<int>(255 * pulse * (scanning ? 0.35f : 1.0f)))), texts.free_place);
      }
    }
  };

  // The button hints, on a bar along the bottom, as the device last used shows them.
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
  draw_list->AddRectFilled(at(0, 680), at(1280, 720), IM_COL32(13, 14, 16, a(240)));
  draw_list->AddLine(at(0, 680), at(1280, 680), SteelLine(a(255)), 1.0f * u);
  float hint_x = game_left + 64.0f * u;
  const float hint_y = game_top + 692.0f * u;
  auto hint = [&](int which, const char* label) {
    if (which == 0) {
      hint_x += DrawConfirmGlyph(draw_list, title_font, ImVec2(hint_x, hint_y), u, pad, texts.enter, a(255));
    } else if (which == 1) {
      hint_x += DrawCancelGlyph(draw_list, title_font, ImVec2(hint_x, hint_y), u, pad, "Esc", a(255));
    } else if (pad) {
      hint_x += DrawPadGlyph(draw_list, title_font, ImVec2(hint_x + 8.0f * u, hint_y + 8.0f * u), 16.0f * u, LookOf(*pad),
                             PadControl::kX, a(255), u);
    } else {
      hint_x += DrawConfirmGlyph(draw_list, title_font, ImVec2(hint_x, hint_y), u, nullptr, texts.space, a(255));
    }
    hint_x += 6.0f * u;
    draw_list->AddText(body_font, 13.0f * u, ImVec2(hint_x, hint_y), Text(a(255)), label);
    hint_x += body_font->CalcTextSizeA(13.0f * u, FLT_MAX, 0.0f, label).x + 26.0f * u;
  };

  switch (screen_) {
    case Screen::kChoose: {
      // Standard, chosen; Ranked, coming soon.
      const ImVec2 standard_min = at(64, 196);
      const ImVec2 standard_max = at(504, 300);
      DrawCutPanel(draw_list, standard_min, standard_max, 12.0f * u, IM_COL32(31, 26, 15, a(250)), Imulsion(a(230)), 1.6f * u);
      draw_list->AddRectFilled(ImVec2(standard_min.x + 1.0f * u, standard_min.y + 12.0f * u),
                               ImVec2(standard_min.x + 4.0f * u, standard_max.y - 1.0f * u), Imulsion(a(255)));
      AddSpacedText(draw_list, body_font, 11.0f * u, ImVec2(standard_min.x + 22.0f * u, standard_min.y + 14.0f * u),
                    Imulsion(a(255)), texts.standard_tag, 1.8f * u);
      AddSpacedText(draw_list, title_font, 30.0f * u, ImVec2(standard_min.x + 22.0f * u, standard_min.y + 30.0f * u),
                    Text(a(255)), texts.standard, 2.4f * u);
      draw_list->AddText(body_font, 12.5f * u, ImVec2(standard_min.x + 22.0f * u, standard_min.y + 72.0f * u),
                         TextDim(a(255)), Fit(body_font, 12.5f * u, texts.standard_help, 400.0f * u).c_str());
      const ImVec2 ranked_min = at(64, 314);
      const ImVec2 ranked_max = at(504, 398);
      DrawCutPanel(draw_list, ranked_min, ranked_max, 12.0f * u, PanelSolid(a(150)), SteelLine(a(150)), 1.0f * u);
      AddSpacedText(draw_list, title_font, 24.0f * u, ImVec2(ranked_min.x + 22.0f * u, ranked_min.y + 14.0f * u),
                    TextFaint(a(255)), texts.ranked, 2.0f * u);
      const float soon_width = AddSpacedText(nullptr, body_font, 10.5f * u, ImVec2(), 0, texts.coming_soon, 1.6f * u);
      const ImVec2 soon_min(ranked_max.x - soon_width - 34.0f * u, ranked_min.y + 18.0f * u);
      draw_list->AddRect(soon_min, ImVec2(ranked_max.x - 18.0f * u, soon_min.y + 20.0f * u), TextFaint(a(255)), 0, 0, 1.0f * u);
      AddSpacedText(draw_list, body_font, 10.5f * u, ImVec2(soon_min.x + 8.0f * u, soon_min.y + 4.0f * u), TextFaint(a(255)),
                    texts.coming_soon, 1.6f * u);
      draw_list->AddText(body_font, 12.5f * u, ImVec2(ranked_min.x + 22.0f * u, ranked_min.y + 52.0f * u), TextFaint(a(220)),
                         texts.ranked_help);

      // The modes, marked or not; the focused one lit.
      const ImVec2 modes_min = at(560, 196);
      const float row_height = 42.0f * u;
      const ImVec2 modes_max(at(1216, 0).x, modes_min.y + 46.0f * u + kModes * row_height + 10.0f * u);
      DrawCutPanel(draw_list, modes_min, modes_max, 12.0f * u, PanelSolid(a(245)), SteelLine(a(255)), 1.0f * u);
      AddSpacedText(draw_list, body_font, 11.0f * u, ImVec2(modes_min.x + 22.0f * u, modes_min.y + 16.0f * u), TextDim(a(255)),
                    texts.modes_label, 1.8f * u);
      for (int i = 0; i < kModes; ++i) {
        const ImVec2 row_min(modes_min.x + 10.0f * u, modes_min.y + 44.0f * u + i * row_height);
        const ImVec2 row_max(modes_max.x - 10.0f * u, row_min.y + row_height - 4.0f * u);
        const bool focused = i == focus_;
        if (focused) {
          draw_list->AddRectFilled(row_min, row_max, IM_COL32(31, 26, 15, a(255)));
          draw_list->AddRectFilled(row_min, ImVec2(row_min.x + 3.0f * u, row_max.y), Imulsion(a(255)));
        }
        const float middle = (row_min.y + row_max.y) * 0.5f;
        DrawDiamond(draw_list, ImVec2(row_min.x + 24.0f * u, middle), 12.0f * u, g_marked[i] ? Imulsion(a(255)) : TextFaint(a(255)),
                    Imulsion(a(255)), g_marked[i], 1.5f * u);
        draw_list->AddText(title_font, 16.0f * u, ImVec2(row_min.x + 44.0f * u, middle - 9.0f * u),
                           focused ? Imulsion(a(255)) : g_marked[i] ? Text(a(255)) : TextDim(a(255)), texts.modes[i]);
      }
      if (std::chrono::duration<float>(now - no_mode_since_).count() < kNoModeSeconds) {
        draw_list->AddText(body_font, 13.0f * u, ImVec2(modes_min.x + 10.0f * u, modes_max.y + 10.0f * u), ErrorText(a(255)),
                           texts.no_mode);
      }
      hint(0, texts.search);
      hint(2, texts.mark);
      hint(1, texts.back);
      break;
    }
    case Screen::kSearching:
    case Screen::kReady: {
      const alboria::SearchStatus status = alboria::GetSearchStatus();
      std::vector<std::string> players = status.players;
      if (players.empty() && !me.empty()) players.push_back(me);
      const bool ready = screen_ == Screen::kReady;
      if (!ready) {
        // The radar, its sweep going round, and the search's clock under it.
        const ImVec2 center = at(170, 330);
        const float r = 95.0f * u;
        draw_list->AddCircleFilled(center, r, IM_COL32(15, 17, 19, a(255)), 64);
        draw_list->AddCircle(center, r, SteelLine(a(255)), 64, 1.0f * u);
        draw_list->AddCircle(center, r * 0.66f, Steel(a(255)), 64, 1.0f * u);
        draw_list->AddCircle(center, r * 0.33f, Steel(a(255)), 64, 1.0f * u);
        draw_list->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x + r, center.y), Steel(a(255)), 1.0f * u);
        draw_list->AddLine(ImVec2(center.x, center.y - r), ImVec2(center.x, center.y + r), Steel(a(255)), 1.0f * u);
        const float sweep = t * 2.1f;
        constexpr int kTrail = 14;
        for (int i = 0; i < kTrail; ++i) {
          const float a0 = sweep - 0.06f * (i + 1);
          const float a1 = sweep - 0.06f * i;
          const ImVec2 p0(center.x + std::cos(a0) * r, center.y + std::sin(a0) * r);
          const ImVec2 p1(center.x + std::cos(a1) * r, center.y + std::sin(a1) * r);
          draw_list->AddTriangleFilled(center, p0, p1, Imulsion(a(70 - i * 5)));
        }
        draw_list->AddLine(center, ImVec2(center.x + std::cos(sweep) * r, center.y + std::sin(sweep) * r), Imulsion(a(255)), 2.0f * u);
        // Glyphs of Alboriano, one more for each player found: dark, they keep changing; lit by the
        // sweep, each settles on its own and glows, then fades and scrambles again.
        const int blips = std::min(kMatchPlaces, 5 + static_cast<int>(players.size()));
        for (int i = 0; i < blips; ++i) {
          const float angle = static_cast<float>(Mix(i * 2 + 1) % 6283) / 1000.0f;
          const float distance = r * (0.3f + 0.55f * static_cast<float>(Mix(i * 2 + 2) % 1000) / 1000.0f);
          const ImVec2 blip(center.x + std::cos(angle) * distance, center.y + std::sin(angle) * distance);
          const float since = std::fmod(sweep - angle + 8.0f * kPi, 2.0f * kPi);
          const float light = std::max(0.0f, 1.0f - since / 2.6f);
          const bool decoded = light > 0.35f;
          const int glyph = decoded ? static_cast<int>(Mix(i + 97) % kGlyphCount)
                                    : static_cast<int>(Mix(i * 131 + static_cast<uint32_t>(t * 9.0f)) % kGlyphCount);
          const int alpha = a(static_cast<int>(255 * (0.22f + 0.78f * light)));
          DrawAlboriano(draw_list, glyph, blip, (decoded ? 17.0f : 14.0f) * u, decoded ? Imulsion(alpha) : TextFaint(alpha),
                        (decoded ? 1.6f : 1.1f) * u);
        }
        const int64_t started = g_search_started.load();
        const int seconds = started ? static_cast<int>((NowTicks() - started) / std::chrono::steady_clock::period::den *
                                                       std::chrono::steady_clock::period::num)
                                    : 0;
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
        AddTextCentered(draw_list, title_font, 30.0f * u, ImVec2(center.x, center.y + r + 34.0f * u), Text(a(255)), clock);
      }
      const float list_left = ready ? 64.0f : 330.0f;
      const float list_width = 1216.0f - list_left;
      const int step = ready ? 4 : status.phase == alboria::SearchPhase::kIdle ? 0 : players.size() >= 2 ? 2 : 1;
      draw_steps(at(list_left, 196), (ready ? 700.0f : list_width) * u, step);
      const int columns = ready ? 3 : 2;
      const int bots = ready ? std::max(0, kMatchPlaces - static_cast<int>(players.size())) : -1;
      draw_places(at(list_left, 230), list_width * u, columns, players, bots);
      if (!ready) {
        char found[160];
        std::snprintf(found, sizeof(found), texts.found, std::min(kMatchPlaces, static_cast<int>(players.size())), kMatchPlaces);
        draw_list->AddText(body_font, 12.0f * u, at(list_left, 470), TextFaint(a(255)), found);
        hint(1, texts.cancel);
      } else {
        // The countdown to the vote.
        const int left = std::max(1, static_cast<int>(std::ceil(kReadySeconds - std::chrono::duration<float>(now - screen_since_).count())));
        const std::string digit = std::to_string(left);
        draw_list->AddText(title_font, 54.0f * u, at(64, 470), Imulsion(a(255)), digit.c_str());
        draw_list->AddText(title_font, 17.0f * u, at(118, 478), Text(a(255)), texts.entering);
        draw_list->AddText(body_font, 13.0f * u, at(118, 502), TextFaint(a(255)), texts.entering_help);
      }
      break;
    }
    case Screen::kError: {
      const ImVec2 min = at(360, 250);
      const ImVec2 max = at(920, 410);
      DrawCutPanel(draw_list, min, max, 12.0f * u, IM_COL32(26, 20, 22, a(250)), SteelLine(a(255)), 1.0f * u);
      draw_list->AddRectFilled(ImVec2(min.x + 1.0f * u, min.y + 12.0f * u), ImVec2(min.x + 4.0f * u, max.y - 1.0f * u),
                               ErrorText(a(255)));
      const ImVec2 icon(min.x + 46.0f * u, min.y + 48.0f * u);
      DrawEmblem(draw_list, icon, 34.0f * u, ErrorText(a(255)), 1.6f * u);
      const bool signed_out = error_ == "signed_out" || error_ == "no_account" || error_ == "unauthorized";
      draw_list->AddText(title_font, 22.0f * u, ImVec2(min.x + 80.0f * u, min.y + 30.0f * u), ErrorText(a(255)), texts.unreachable);
      draw_list->AddText(body_font, 14.0f * u, ImVec2(min.x + 80.0f * u, min.y + 64.0f * u), TextDim(a(255)),
                         signed_out ? texts.signed_out_help : texts.unreachable_help, nullptr, max.x - min.x - 110.0f * u);
      hint(0, texts.retry);
      hint(1, texts.back);
      break;
    }
  }
}

void VersusOnlineMenu::OnClose() {
  // Closed by a press while searching: the search stops (the menu's own back press already did).
  if (screen_ == Screen::kSearching) CancelSearch();
  script::PlayMenuSound(script::MenuSound::kClose);
  if (assignment_) assignment_->SetBlocked(false);
  if (owner_) *owner_ = nullptr;
}

}  // namespace azura
