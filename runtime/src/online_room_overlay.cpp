#include "online_room_overlay.h"

#include <algorithm>
#include <string>

#include <imgui.h>

#include <rex/system/xam/alboria.h>

#include "perf_overlay.h"
#include "script_hooks.h"
#include "menu_language.h"

namespace azura {

namespace {

namespace alboria = rex::system::xam::alboria;

// How long a failure stays on screen.
constexpr auto kErrorShownFor = std::chrono::seconds(8);

struct Texts {
  const char* title;
  const char* creating;    // setting up a room the player hosts
  const char* joining;     // setting up a room the player joins
  const char* failed;      // the title of a failure
  // Failures, by the error code of alboria::Status.
  const char* network;
  const char* not_found;
  const char* full;
  const char* closed;
  const char* banned;
  const char* too_old;
  // This installation is not linked to an Alboria account (alboria-notes:azura/alboria-relay.md, section 3).
  const char* no_account;
  // A new room cannot be made now: the service or the game is in maintenance (section 4.2).
  const char* maintenance;
  // Online play of this game is switched off in the console (section 4.2).
  const char* game_off;
  const char* other;
  // Joining a friend in a matchmade match: their team has no place left, or the match has not
  // opened yet.
  const char* team_full;
  const char* starting;
  // A party leader joining another party with its own: they do not all fit in it
  // (alboria-notes:alboria-server/online-rules.md, section 5).
  const char* party_does_not_fit;
  // Joining the room the player leads, by its code.
  const char* already_in;
};

// In the game's languages; the runtime loads the Cyrillic and CJK fonts each one needs.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"オンラインルーム", "ルームを作成中…", "ルームに参加中…", "オンライン", "Alboria に接続できません",
              "そのコードのゲームはありません", "ゲームは満員です", "ルームは閉じられました", "Alboria から利用を禁止されています",
              "オンラインで遊ぶには Azura を更新してください", "ランチャーから Alboria にサインインしてください",
              "Alboria はメンテナンス中です。後でもう一度お試しください",
              "このゲームのオンラインプレイは停止中です",
              "Alboria から応答がありません", "フレンドのチームに空きがありません",
              "マッチを開始しています。少し待ってからもう一度お試しください",
              "あなたのパーティーはフレンドのパーティーに入りきりません",
              "すでにそのルームにいます"};
    case 7:  // Korean
      return {"온라인 방", "방을 만드는 중…", "방에 참가하는 중…", "온라인", "Alboria에 연결할 수 없습니다",
              "그 코드의 게임이 없습니다", "게임이 꽉 찼습니다", "방이 닫혔습니다", "Alboria에서 이용이 정지되었습니다",
              "온라인으로 플레이하려면 Azura를 업데이트하세요", "런처에서 Alboria에 로그인하세요", "Alboria 점검 중입니다. 나중에 다시 시도하세요",
              "이 게임의 온라인 플레이가 꺼져 있습니다",
              "Alboria가 응답하지 않았습니다",
              "친구의 팀에 빈자리가 없습니다",
              "매치가 시작되는 중입니다. 잠시 후 다시 시도하세요",
              "내 파티가 친구의 파티에 다 들어갈 수 없습니다", "이미 그 방에 있습니다"};
    case 8:  // Traditional Chinese
      return {"線上房間", "正在建立房間…", "正在加入房間…", "線上", "無法連線到 Alboria", "沒有使用這個代碼的遊戲",
              "遊戲已滿", "房間已關閉", "你已被 Alboria 停權", "請更新 Azura 才能線上遊玩", "請從啟動器登入 Alboria",
              "Alboria 正在維護，請稍後再試",
              "這款遊戲的線上遊戲已關閉",
              "Alboria 沒有回應", "好友的隊伍已沒有空位",
              "對戰正在開始，請稍後再試", "你的隊伍無法全部加入好友的隊伍", "你已經在這個房間裡"};
    case 12:  // Russian
      return {"ОНЛАЙН-КОМНАТА", "Создание комнаты…", "Вход в комнату…", "ОНЛАЙН", "Нет связи с Alboria",
              "Нет игры с таким кодом", "Игра заполнена", "Комната закрыта", "Alboria заблокировала вас",
              "Обновите Azura, чтобы играть онлайн", "Войдите в Alboria в лаунчере", "Alboria на обслуживании, попробуйте позже",
              "Онлайн-игра в этой игре отключена",
              "Alboria не ответила",
              "В команде друга нет мест",
              "Матч начинается, попробуйте чуть позже",
              "Ваша группа не помещается в группу друга",
              "Вы уже в этой комнате"};
    case 3:  // German
      return {"ONLINE-RAUM", "Raum wird erstellt\xE2\x80\xA6", "Raum wird betreten\xE2\x80\xA6", "ONLINE",
              "Keine Verbindung zu Alboria", "Kein Spiel hat diesen Code", "Das Spiel ist voll",
              "Der Raum wurde geschlossen", "Du wurdest von Alboria gesperrt",
              "Aktualisiere Azura, um online zu spielen", "Melde dich im Launcher bei Alboria an",
              "Alboria wird gewartet, versuche es sp\xC3\xA4ter erneut",
              "Das Onlinespiel dieses Spiels ist ausgeschaltet",
              "Alboria hat nicht geantwortet", "Im Team deines Freundes ist kein Platz frei",
              "Das Match beginnt gerade, versuch es gleich noch einmal",
              "Deine Gruppe passt nicht in die deines Freundes",
              "Du bist bereits in diesem Raum"};
    case 4:  // French
      return {"SALON EN LIGNE", "Cr\xC3\xA9""ation du salon\xE2\x80\xA6", "Connexion au salon\xE2\x80\xA6", "EN LIGNE",
              "Impossible de joindre Alboria", "Aucune partie n'a ce code", "La partie est pleine",
              "Le salon a \xC3\xA9t\xC3\xA9 ferm\xC3\xA9", "Vous avez \xC3\xA9t\xC3\xA9 banni d'Alboria",
              "Mettez \xC3\xA0 jour Azura pour jouer en ligne", "Connectez-vous \xC3\xA0 Alboria depuis le launcher",
              "Alboria est en maintenance, r\xC3\xA9""essayez plus tard",
              "Le jeu en ligne est d\xC3\xA9sactiv\xC3\xA9 pour ce jeu",
              "Alboria n'a pas r\xC3\xA9pondu", "Il n'y a plus de place dans l'\xC3\xA9quipe de votre ami",
              "Le match commence, r\xC3\xA9""essayez dans un instant",
              "Votre groupe ne tient pas dans celui de votre ami",
              "Vous \xC3\xAAtes d\xC3\xA9j\xC3\xA0 dans ce salon"};
    case 5:  // Spanish
      return {"SALA ONLINE", "Creando la sala\xE2\x80\xA6", "Entrando en la sala\xE2\x80\xA6", "ONLINE",
              "No se puede conectar con Alboria", "Ninguna partida tiene ese c\xC3\xB3""digo",
              "La partida est\xC3\xA1 llena", "La sala se ha cerrado", "Alboria te ha vetado",
              "Actualiza Azura para jugar online", "Inicia sesi\xC3\xB3n en Alboria desde el launcher",
              "Alboria est\xC3\xA1 en mantenimiento, int\xC3\xA9ntalo m\xC3\xA1s tarde",
              "El online de este juego est\xC3\xA1 desactivado",
              "Alboria no ha respondido", "No queda sitio en el equipo de tu amigo",
              "La partida est\xC3\xA1 empezando; prueba otra vez en un momento",
              "Tu grupo no cabe en el de tu amigo",
              "Ya est\xC3\xA1s en esa sala"};
    case 6:  // Italian
      return {"STANZA ONLINE", "Creazione della stanza\xE2\x80\xA6", "Ingresso nella stanza\xE2\x80\xA6", "ONLINE",
              "Impossibile raggiungere Alboria", "Nessuna partita ha questo codice",
              "La partita \xC3\xA8 piena", "La stanza \xC3\xA8 stata chiusa", "Alboria ti ha bandito",
              "Aggiorna Azura per giocare online", "Accedi ad Alboria dal launcher", "Alboria \xC3\xA8 in manutenzione, riprova pi\xC3\xB9 tardi",
              "Il gioco online \xC3\xA8 disattivato per questo gioco",
              "Alboria non ha risposto",
              "Non c'\xC3\xA8 pi\xC3\xB9 posto nella squadra del tuo amico",
              "La partita sta iniziando, riprova tra un attimo",
              "Il tuo gruppo non entra in quello del tuo amico",
              "Sei gi\xC3\xA0 in questa stanza"};
    case 9:  // Portuguese
      return {"SALA ONLINE", "Criando a sala\xE2\x80\xA6", "Entrando na sala\xE2\x80\xA6", "ONLINE",
              "N\xC3\xA3o foi poss\xC3\xADvel conectar ao Alboria",
              "Nenhuma partida tem esse c\xC3\xB3""digo", "A partida est\xC3\xA1 cheia",
              "A sala foi fechada", "O Alboria te baniu", "Atualize o Azura para jogar online",
              "Entre no Alboria pelo launcher", "O Alboria est\xC3\xA1 em manuten\xC3\xA7\xC3\xA3o, tente mais tarde",
              "O online deste jogo est\xC3\xA1 desativado",
              "O Alboria n\xC3\xA3o respondeu", "N\xC3\xA3o h\xC3\xA1 vaga na equipe do seu amigo",
              "A partida est\xC3\xA1 come\xC3\xA7""ando, tente de novo em instantes",
              "Seu grupo n\xC3\xA3o cabe no do seu amigo",
              "Voc\xC3\xAA j\xC3\xA1 est\xC3\xA1 nessa sala"};
    case 11:  // Polish
      return {"POK\xC3\x93J ONLINE", "Tworzenie pokoju\xE2\x80\xA6", "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6",
              "ONLINE",
              "Brak po\xC5\x82\xC4\x85""czenia z Albori\xC4\x85", "\xC5\xBB""adna gra nie ma tego kodu",
              "Gra jest pe\xC5\x82na", "Pok\xC3\xB3j zosta\xC5\x82 zamkni\xC4\x99ty",
              "Alboria ci\xC4\x99 zablokowa\xC5\x82""a", "Zaktualizuj Azur\xC4\x99, aby gra\xC4\x87 online",
              "Zaloguj si\xC4\x99 do Alborii w launcherze", "Alboria jest w trakcie konserwacji, spr\xC3\xB3""buj p\xC3\xB3\xC5\xBAniej",
              "Gra online w tej grze jest wy\xC5\x82\xC4\x85""czona",
              "Alboria nie odpowiedzia\xC5\x82""a", "W dru\xC5\xBCynie znajomego nie ma miejsca",
              "Mecz si\xC4\x99 zaczyna, spr\xC3\xB3""buj za chwil\xC4\x99",
              "Twoja dru\xC5\xBCyna nie zmie\xC5\x9B""ci si\xC4\x99 w dru\xC5\xBCynie znajomego",
              "Ju\xC5\xBC jeste\xC5\x9B w tym pokoju"};
    case kLanguageCzech:
      return {"ONLINE MÍSTNOST", "Vytváření místnosti…", "Připojování k místnosti…", "ONLINE",
              "Nelze se spojit s Alborií", "Žádná hra nemá tento kód", "Hra je plná", "Místnost byla zavřena",
              "Alboria vám zablokovala přístup", "Pro hraní online aktualizujte Azura",
              "Přihlaste se k Alborii v launcheru", "Alboria je v údržbě, zkuste to později",
              "Online hraní této hry je vypnuto", "Alboria neodpověděla", "V týmu vašeho přítele už není místo",
              "Zápas začíná; zkuste to za chvíli znovu", "Vaše skupina se nevejde do skupiny vašeho přítele",
              "V této místnosti už jste"};
    case kLanguageHungarian:
      return {"ONLINE SZOBA", "Szoba létrehozása…", "Csatlakozás a szobához…", "ONLINE", "Az Alboria nem érhető el",
              "Nincs ilyen kódú játék", "A játék megtelt", "A szobát bezárták", "Az Alboria kitiltott",
              "Frissítsd az Azurát az online játékhoz", "Jelentkezz be az Alboriába a launcherben",
              "Az Alboria karbantartás alatt áll, próbáld újra később",
              "Ennek a játéknak az online módja ki van kapcsolva", "Az Alboria nem válaszolt",
              "A barátod csapatában nincs több hely", "A meccs most indul; próbáld újra egy kicsit később",
              "A csoportod nem fér be a barátod csoportjába", "Már ebben a szobában vagy"};
    default:
      return {"ONLINE ROOM", "Creating the room\xE2\x80\xA6", "Joining the room\xE2\x80\xA6", "ONLINE", "Cannot reach Alboria",
              "No game has that code", "The game is full", "The room was closed",
              "Alboria has banned you", "Update Azura to play online", "Sign in to Alboria from the launcher",
              "Alboria is in maintenance, try again later",
              "Online play of this game is switched off",
              "Alboria did not answer", "Your friend's team has no place left",
              "The match is starting; try again in a moment",
              "Your party does not fit in your friend's",
              "You are in that room already"};
  }
}

// What the player reads for a failure, by alboria::Status::error.
const char* ErrorText(const Texts& texts, const std::string& error) {
  if (error == "network" || error == "relay_unreachable" || error == "timed_out") return texts.network;
  if (error == "room_not_found" || error == "wrong_title") return texts.not_found;
  if (error == "room_full") return texts.full;
  if (error == "room_closed" || error == "replaced") return texts.closed;
  if (error.rfind("banned", 0) == 0) return texts.banned;
  if (error == "client_too_old" || error == "launcher_outdated") return texts.too_old;
  if (error == "no_account") return texts.no_account;
  if (error == "maintenance" || error == "game_maintenance") return texts.maintenance;
  if (error == "game_offline") return texts.game_off;
  if (error == "team_full") return texts.team_full;
  if (error == "match_starting") return texts.starting;
  if (error == "party_does_not_fit") return texts.party_does_not_fit;
  if (error == "already_in_party") return texts.already_in;
  return texts.other;
}

}  // namespace

const char* AlboriaErrorText(uint32_t language, const std::string& error) {
  return ErrorText(LocalizedTexts(language), error);
}

namespace {

// Text with a soft drop shadow, readable over bright and dark scenes alike.
void DrawShadowedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position,
                      const char* text) {
  float offset = std::max(1.0f, size * 0.06f);
  draw_list->AddText(font, size, ImVec2(position.x + offset, position.y + offset),
                     IM_COL32(0, 0, 0, 170), text);
  draw_list->AddText(font, size, position, IM_COL32(242, 242, 242, 255), text);
}

}  // namespace

OnlineRoomOverlay::OnlineRoomOverlay(rex::ui::ImGuiDrawer* drawer, uint32_t language,
                                     ImFont* title_font, ImFont* body_font,
                                     OnlineRoomOverlay** owner)
    : ImGuiDialog(drawer),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner) {}

void OnlineRoomOverlay::OnDraw(ImGuiIO& io) {
  const alboria::Status status = alboria::GetStatus();
  const Texts texts = LocalizedTexts(language_);
  const Clock::time_point now = Clock::now();

  const char* title = nullptr;
  std::string line;
  switch (status.state) {
    case alboria::State::kOffline: {
      // No room. When the game has just stayed in Local because Alboria refused the sign-in (at
      // start-up, or as the player asked for a room), the refusal shows for a while from that
      // moment, as a failure does; so does the last failure when the player left right after it.
      if (std::string reason = azura::script::TakeRefusalNotice(); !reason.empty()) {
        error_ = std::move(reason);
        error_since_ = now;
      }
      if (error_.empty() || now - error_since_ >= kErrorShownFor) return;
      title = texts.failed;
      line = ErrorText(texts, error_);
      break;
    }
    case alboria::State::kWorking:
      error_.clear();
      title = texts.title;
      // A match room (matchmaking) says the same to everyone: who hosts is never shown
      // (alboria-notes:alboria-server/online-rules.md).
      line = status.host && !status.in_match ? texts.creating : texts.joining;
      break;
    case alboria::State::kConnected:
      error_.clear();
      return;
    case alboria::State::kFailed:
      if (status.error != error_) {
        error_ = status.error;
        error_since_ = now;
      }
      if (now - error_since_ >= kErrorShownFor) return;
      title = texts.failed;
      line = ErrorText(texts, status.error);
      break;
  }

  // In the top right corner of the 16:9 area the game image takes in the window, away from the
  // game's own indicators at the bottom and our notices on the left; sizes in pixels at 720p.
  // Below the performance overlay when that one shows in the same corner.
  float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  float game_width = game_height * 16.0f / 9.0f;
  float game_right = (io.DisplaySize.x + game_width) * 0.5f;
  float unit = game_height / 720.0f;
  float perf_overlay_height = PerfOverlayTopRightHeight();
  float game_top = (io.DisplaySize.y - game_height) * 0.5f +
                   (perf_overlay_height > 0.0f ? (perf_overlay_height - 8.0f) * unit : 0.0f);
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  float title_size = 13.0f * unit;
  float body_size = 11.0f * unit;
  float margin = 24.0f * unit;
  float title_width = title_font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title).x;
  float line_width = body_font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, line.c_str()).x;
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  DrawShadowedText(draw_list, title_font, title_size,
                   ImVec2(game_right - margin - title_width, game_top + 21.0f * unit), title);
  DrawShadowedText(draw_list, body_font, body_size,
                   ImVec2(game_right - margin - line_width, game_top + 35.0f * unit), line.c_str());
}

void OnlineRoomOverlay::OnClose() {
  if (owner_) {
    *owner_ = nullptr;
  }
}

}  // namespace azura
