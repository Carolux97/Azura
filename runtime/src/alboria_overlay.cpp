#include "alboria_overlay.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ui/immediate_drawer.h>

#include "input/overlay_input.h"
#include "input/player_assignment.h"
#include "alboria_sounds.h"
#include "alboria_style.h"
#include "online_room_overlay.h"

namespace azura {

namespace {

using namespace coop_ui;
using namespace alboria_style;
using alboria_sounds::Sound;
using rex::input::DeviceId;
using rex::input::PadControl;
using Slot = input::PlayerAssignment::Slot;
namespace alboria = rex::system::xam::alboria;

// How long the overlay takes to fade in.
constexpr float kFadeSeconds = 0.18f;
// The opening: the panel grows from 90 % with a little bounce and rises into place; the closing
// runs the other way, faster.
constexpr float kOpenSeconds = 0.32f;
constexpr float kCloseSeconds = 0.2f;

float Ease01(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

// Overshoots a little before it settles.
float EaseOutBack(float t) {
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

// Moves and scales the vertices `draw_list` got since it was made: the whole panel, its text and
// its painting, about `center`, for the opening and closing animation.
class PanelMotion {
 public:
  PanelMotion(ImDrawList* draw_list, ImVec2 center, float scale, float rise)
      : draw_list_(draw_list), first_(draw_list->VtxBuffer.Size), center_(center), scale_(scale), rise_(rise) {}
  ~PanelMotion() {
    if (scale_ == 1.0f && rise_ == 0.0f) return;
    for (int i = first_; i < draw_list_->VtxBuffer.Size; ++i) {
      ImVec2& pos = draw_list_->VtxBuffer[i].pos;
      pos.x = center_.x + (pos.x - center_.x) * scale_;
      pos.y = center_.y + (pos.y - center_.y) * scale_ + rise_;
    }
  }
  PanelMotion(const PanelMotion&) = delete;
  PanelMotion& operator=(const PanelMotion&) = delete;

 private:
  ImDrawList* draw_list_;
  int first_;
  ImVec2 center_;
  float scale_;
  float rise_;
};
// How far the left stick goes before it counts as a press in a direction.
constexpr int kStickPress = 20000;
// How long the result of an action stays.
constexpr float kMessageSeconds = 4.0f;
// The game's title ID, for "playing" when a friend plays it.
constexpr uint32_t kGearsOfWar3 = 0x4D5308AB;

struct Texts {
  const char* title;
  const char* friends;
  const char* invitations;
  const char* room;
  const char* join;
  const char* invite;
  const char* not_now;
  const char* copy;
  const char* join_by_code;
  const char* close;
  const char* change_page;
  const char* playing_open;   // %s: the game
  const char* playing_invite; // %s: the game
  const char* playing_other;
  const char* online;
  const char* offline;
  const char* last_seen;      // %s: how long ago
  const char* minutes;        // %d
  const char* hours;          // %d
  const char* days;           // %d
  const char* no_friends;
  const char* no_invitations;
  const char* invites_you;    // %s: name
  const char* no_room;
  const char* room_code;
  const char* copied;
  const char* joining;
  const char* inviting;
  const char* invited;        // %s: name
  const char* declined;
  const char* unavailable;
  const char* unavailable_help;
  const char* failed;
};

// In the game's languages; the runtime loads the Cyrillic and CJK fonts each one needs.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"Alboria", "フレンド", "招待", "ルーム", "参加", "招待する", "今はしない", "コピー", "コードで参加",
              "閉じる", "ページ切り替え", "%sをプレイ中 · オープンルーム", "%sをプレイ中 · 招待のみ", "プレイ中",
              "オンライン", "オフライン", "最終ログイン %s前", "%d分", "%d時間", "%d日",
              "Alboria にまだフレンドがいません。ランチャーから追加してください。", "招待はありません。",
              "%s があなたをルームに招待しています", "ルームに参加していません。", "あなたのルームのコード",
              "コードをコピーしました", "ルームに参加中…", "招待を送信中…", "%s に招待を送りました", "招待を断りました",
              "現在 Alboria を利用できません", "接続と、ランチャーでアカウントがリンクされているか確認してください。",
              "できませんでした: %s"};
    case 7:  // Korean
      return {"Alboria", "친구", "초대", "방", "참가", "초대하기", "나중에", "복사", "코드로 참가", "닫기",
              "페이지 전환", "%s 플레이 중 · 열린 방", "%s 플레이 중 · 초대 전용", "플레이 중", "온라인", "오프라인",
              "%s 전 접속", "%d분", "%d시간", "%d일", "아직 Alboria 친구가 없습니다. 런처에서 추가하세요.",
              "초대가 없습니다.", "%s 님이 방으로 초대했습니다", "방에 있지 않습니다.", "내 방 코드", "코드를 복사했습니다",
              "방에 참가하는 중…", "초대를 보내는 중…", "%s 님에게 초대를 보냈습니다", "초대를 거절했습니다",
              "지금은 Alboria를 사용할 수 없습니다", "연결 상태와 런처에서 계정이 연결되어 있는지 확인하세요.",
              "실패했습니다: %s"};
    case 8:  // Traditional Chinese
      return {"Alboria", "好友", "邀請", "房間", "加入", "邀請", "稍後", "複製", "以代碼加入", "關閉", "切換頁面",
              "正在玩 %s · 開放房間", "正在玩 %s · 僅限邀請", "遊戲中", "線上", "離線", "最後上線：%s前", "%d 分鐘",
              "%d 小時", "%d 天", "你在 Alboria 還沒有好友。請從啟動器新增。", "你沒有邀請。", "%s 邀請你加入房間",
              "你不在任何房間。", "你的房間代碼", "已複製代碼", "正在加入房間…", "正在傳送邀請…", "已邀請 %s",
              "已拒絕邀請", "目前無法使用 Alboria", "請檢查連線，並確認帳號已在啟動器中連結。", "無法完成：%s"};
    case 12:  // Russian
      return {"Alboria", "Друзья", "Приглашения", "Комната", "Войти", "Пригласить", "Не сейчас", "Копировать",
              "Войти по коду", "Закрыть", "Сменить страницу", "Играет в %s · открытая комната",
              "Играет в %s · только по приглашению", "Играет", "В сети", "Не в сети", "Был(а) %s назад", "%d мин",
              "%d ч", "%d дн.", "У вас пока нет друзей в Alboria. Добавьте их в лаунчере.", "Приглашений нет.",
              "%s приглашает вас в комнату", "Вы не в комнате.", "Код вашей комнаты", "Код скопирован",
              "Вход в комнату…", "Отправка приглашения…", "Приглашение отправлено: %s", "Приглашение отклонено",
              "Alboria сейчас недоступна", "Проверьте подключение и привязку аккаунта в лаунчере.",
              "Не получилось: %s"};
    case 5:  // Spanish
      return {"Alboria", "Amigos", "Invitaciones", "Sala", "Unirse", "Invitar", "Ahora no", "Copiar",
              "Unirse por c\xC3\xB3""digo", "Cerrar", "Cambiar de p\xC3\xA1gina", "Jugando a %s \xC2\xB7 sala abierta",
              "Jugando a %s \xC2\xB7 solo por invitaci\xC3\xB3n", "Jugando", "En l\xC3\xAD""nea", "Desconectado",
              "Visto hace %s", "%d min", "%d h", "%d d", "Todav\xC3\xAD""a no tienes amigos en Alboria. A\xC3\xB1\xC3\xA1""delos desde el launcher.",
              "No tienes invitaciones.", "%s te invita a su sala", "No est\xC3\xA1s en ninguna sala.",
              "C\xC3\xB3""digo de tu sala", "C\xC3\xB3""digo copiado", "Entrando en la sala\xE2\x80\xA6",
              "Enviando la invitaci\xC3\xB3n\xE2\x80\xA6", "Invitaci\xC3\xB3n enviada a %s", "Invitaci\xC3\xB3n rechazada",
              "Alboria no est\xC3\xA1 disponible ahora mismo",
              "Comprueba tu conexi\xC3\xB3n y que tu cuenta est\xC3\xA9 vinculada en el launcher.",
              "No se ha podido: %s"};
    case 3:  // German
      return {"Alboria", "Freunde", "Einladungen", "Raum", "Beitreten", "Einladen", "Nicht jetzt", "Kopieren",
              "Per Code beitreten", "Schlie\xC3\x9F""en", "Seite wechseln", "Spielt %s \xC2\xB7 offener Raum",
              "Spielt %s \xC2\xB7 nur auf Einladung", "Spielt", "Online", "Offline", "Zuletzt gesehen vor %s",
              "%d Min.", "%d Std.", "%d Tg.", "Du hast noch keine Freunde auf Alboria. F\xC3\xBC""ge sie im Launcher hinzu.",
              "Du hast keine Einladungen.", "%s l\xC3\xA4""dt dich in seinen Raum ein", "Du bist in keinem Raum.",
              "Code deines Raums", "Code kopiert", "Raum wird betreten\xE2\x80\xA6", "Einladung wird gesendet\xE2\x80\xA6",
              "Einladung an %s gesendet", "Einladung abgelehnt", "Alboria ist gerade nicht erreichbar",
              "Pr\xC3\xBC""fe deine Verbindung und ob dein Konto im Launcher verkn\xC3\xBCpft ist.",
              "Das ging nicht: %s"};
    case 4:  // French
      return {"Alboria", "Amis", "Invitations", "Salon", "Rejoindre", "Inviter", "Pas maintenant", "Copier",
              "Rejoindre par code", "Fermer", "Changer de page", "Joue \xC3\xA0 %s \xC2\xB7 salon ouvert",
              "Joue \xC3\xA0 %s \xC2\xB7 sur invitation", "Joue", "En ligne", "Hors ligne", "Vu il y a %s", "%d min",
              "%d h", "%d j", "Tu n'as pas encore d'amis sur Alboria. Ajoute-les depuis le launcher.",
              "Tu n'as aucune invitation.", "%s t'invite dans son salon", "Tu n'es dans aucun salon.",
              "Code de ton salon", "Code copi\xC3\xA9", "Connexion au salon\xE2\x80\xA6", "Envoi de l'invitation\xE2\x80\xA6",
              "Invitation envoy\xC3\xA9""e \xC3\xA0 %s", "Invitation refus\xC3\xA9""e", "Alboria n'est pas disponible pour le moment",
              "V\xC3\xA9rifie ta connexion et que ton compte est li\xC3\xA9 dans le launcher.", "Impossible : %s"};
    case 6:  // Italian
      return {"Alboria", "Amici", "Inviti", "Stanza", "Unisciti", "Invita", "Non ora", "Copia",
              "Unisciti con codice", "Chiudi", "Cambia pagina", "Gioca a %s \xC2\xB7 stanza aperta",
              "Gioca a %s \xC2\xB7 solo su invito", "Sta giocando", "Online", "Offline", "Visto %s fa", "%d min",
              "%d h", "%d g", "Non hai ancora amici su Alboria. Aggiungili dal launcher.", "Non hai inviti.",
              "%s ti invita nella sua stanza", "Non sei in nessuna stanza.", "Codice della tua stanza",
              "Codice copiato", "Ingresso nella stanza\xE2\x80\xA6", "Invio dell'invito\xE2\x80\xA6", "Invito inviato a %s",
              "Invito rifiutato", "Alboria non \xC3\xA8 disponibile ora",
              "Controlla la connessione e che il tuo account sia collegato nel launcher.", "Non \xC3\xA8 stato possibile: %s"};
    case 9:  // Portuguese
      return {"Alboria", "Amigos", "Convites", "Sala", "Entrar", "Convidar", "Agora n\xC3\xA3o", "Copiar",
              "Entrar com c\xC3\xB3""digo", "Fechar", "Mudar de p\xC3\xA1gina", "Jogando %s \xC2\xB7 sala aberta",
              "Jogando %s \xC2\xB7 s\xC3\xB3 por convite", "Jogando", "Online", "Offline", "Visto h\xC3\xA1 %s", "%d min",
              "%d h", "%d d", "Voc\xC3\xAA ainda n\xC3\xA3o tem amigos na Alboria. Adicione-os pelo launcher.",
              "Voc\xC3\xAA n\xC3\xA3o tem convites.", "%s te convida para a sala", "Voc\xC3\xAA n\xC3\xA3o est\xC3\xA1 em nenhuma sala.",
              "C\xC3\xB3""digo da sua sala", "C\xC3\xB3""digo copiado", "Entrando na sala\xE2\x80\xA6", "Enviando o convite\xE2\x80\xA6",
              "Convite enviado para %s", "Convite recusado", "A Alboria n\xC3\xA3o est\xC3\xA1 dispon\xC3\xADvel agora",
              "Verifique a conex\xC3\xA3o e se a sua conta est\xC3\xA1 vinculada no launcher.", "N\xC3\xA3o foi poss\xC3\xADvel: %s"};
    case 11:  // Polish
      return {"Alboria", "Znajomi", "Zaproszenia", "Pok\xC3\xB3j", "Do\xC5\x82\xC4\x85""cz", "Zapro\xC5\x9B", "Nie teraz",
              "Kopiuj", "Do\xC5\x82\xC4\x85""cz kodem", "Zamknij", "Zmie\xC5\x84 stron\xC4\x99", "Gra w %s \xC2\xB7 pok\xC3\xB3j otwarty",
              "Gra w %s \xC2\xB7 tylko na zaproszenie", "Gra", "Online", "Offline", "Widziany %s temu", "%d min",
              "%d godz.", "%d dn.", "Nie masz jeszcze znajomych w Alborii. Dodaj ich w launcherze.",
              "Nie masz zaprosze\xC5\x84.", "%s zaprasza ci\xC4\x99 do pokoju", "Nie jeste\xC5\x9B w \xC5\xBC""adnym pokoju.",
              "Kod twojego pokoju", "Skopiowano kod", "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6", "Wysy\xC5\x82""anie zaproszenia\xE2\x80\xA6",
              "Wys\xC5\x82""ano zaproszenie do %s", "Odrzucono zaproszenie", "Alboria jest teraz niedost\xC4\x99pna",
              "Sprawd\xC5\xBA po\xC5\x82\xC4\x85""czenie i czy konto jest po\xC5\x82\xC4\x85""czone w launcherze.", "Nie uda\xC5\x82o si\xC4\x99: %s"};
    default:
      return {"Alboria", "Friends", "Invitations", "Room", "Join", "Invite", "Not now", "Copy", "Join by code",
              "Close", "Change page", "Playing %s \xC2\xB7 open room", "Playing %s \xC2\xB7 invite only", "Playing",
              "Online", "Offline", "Last seen %s ago", "%d min", "%d h", "%d d",
              "You have no friends on Alboria yet. Add them from the launcher.", "You have no invitations.",
              "%s invites you to their room", "You are not in a room.", "Your room's code", "Code copied",
              "Joining the room\xE2\x80\xA6", "Sending the invitation\xE2\x80\xA6", "Invitation sent to %s",
              "Invitation declined", "Alboria is not available right now",
              "Check your connection and that your account is linked in the launcher.", "That did not work: %s"};
  }
}

// The Add friend screen's texts.
struct AddTexts {
  const char* add_friend;
  const char* help;
  const char* gamertag;
  const char* tag;
  const char* send;
  const char* shift;
  const char* space;
  const char* erase;
  const char* back;
  const char* sending;
  const char* sent;          // %s: name
  const char* now_friends;   // %s: name
  const char* bad_name;
};

AddTexts LocalizedAddTexts(uint32_t language) {
  switch (language) {
    case 2:
      return {"フレンドを追加", "ゲーマータグと番号を入力してください（例: Nimbus#4827）。", "ゲーマータグ", "番号",
              "送信", "大文字", "スペース", "削除", "戻る", "リクエストを送信中…", "%s にリクエストを送りました",
              "%s とフレンドになりました", "ゲーマータグと番号を入力してください"};
    case 7:
      return {"친구 추가", "게이머태그와 번호를 입력하세요 (예: Nimbus#4827).", "게이머태그", "번호", "보내기",
              "대문자", "스페이스", "지우기", "뒤로", "요청을 보내는 중…", "%s 님에게 요청을 보냈습니다",
              "%s 님과 친구가 되었습니다", "게이머태그와 번호를 입력하세요"};
    case 8:
      return {"新增好友", "輸入對方的玩家代號和數字，例如 Nimbus#4827。", "玩家代號", "數字", "傳送", "大寫",
              "空白鍵", "刪除", "返回", "正在傳送請求…", "已向 %s 傳送請求", "你和 %s 已成為好友",
              "請輸入玩家代號和數字"};
    case 12:
      return {"Добавить друга", "Введите его Gamertag и номер, например Nimbus#4827.", "Gamertag", "Номер",
              "Отправить", "Заглавные", "Пробел", "Стереть", "Назад", "Отправка запроса…", "Запрос отправлен: %s",
              "Вы с %s теперь друзья", "Введите Gamertag и номер"};
    case 5:
      return {"A\xC3\xB1""adir amigo", "Escribe su Gamertag y su n\xC3\xBA""mero, como Nimbus#4827.", "Gamertag", "N\xC3\xBA""mero",
              "Enviar", "May\xC3\xBA""s", "Espacio", "Borrar", "Volver", "Enviando la solicitud\xE2\x80\xA6",
              "Solicitud enviada a %s", "%s y t\xC3\xBA ya sois amigos", "Escribe el Gamertag y su n\xC3\xBA""mero"};
    case 3:
      return {"Freund hinzuf\xC3\xBC""gen", "Gib Gamertag und Nummer ein, etwa Nimbus#4827.", "Gamertag", "Nummer",
              "Senden", "Gro\xC3\x9F", "Leerzeichen", "L\xC3\xB6schen", "Zur\xC3\xBC""ck", "Anfrage wird gesendet\xE2\x80\xA6",
              "Anfrage an %s gesendet", "Du und %s seid jetzt Freunde", "Gib Gamertag und Nummer ein"};
    case 4:
      return {"Ajouter un ami", "Saisis son Gamertag et son num\xC3\xA9ro, comme Nimbus#4827.", "Gamertag", "Num\xC3\xA9ro",
              "Envoyer", "Maj", "Espace", "Effacer", "Retour", "Envoi de la demande\xE2\x80\xA6", "Demande envoy\xC3\xA9""e \xC3\xA0 %s",
              "%s et toi \xC3\xAAtes maintenant amis", "Saisis le Gamertag et son num\xC3\xA9ro"};
    case 6:
      return {"Aggiungi amico", "Scrivi il suo Gamertag e il numero, come Nimbus#4827.", "Gamertag", "Numero", "Invia",
              "Maiusc", "Spazio", "Cancella", "Indietro", "Invio della richiesta\xE2\x80\xA6", "Richiesta inviata a %s",
              "Tu e %s ora siete amici", "Scrivi il Gamertag e il numero"};
    case 9:
      return {"Adicionar amigo", "Digite o Gamertag e o n\xC3\xBA""mero, como Nimbus#4827.", "Gamertag", "N\xC3\xBA""mero",
              "Enviar", "Mai\xC3\xBA""sc", "Espa\xC3\xA7o", "Apagar", "Voltar", "Enviando o pedido\xE2\x80\xA6",
              "Pedido enviado para %s", "Voc\xC3\xAA e %s agora s\xC3\xA3o amigos", "Digite o Gamertag e o n\xC3\xBA""mero"};
    case 11:
      return {"Dodaj znajomego", "Wpisz Gamertag i numer, np. Nimbus#4827.", "Gamertag", "Numer", "Wy\xC5\x9Blij", "Shift",
              "Spacja", "Usu\xC5\x84", "Wr\xC3\xB3\xC4\x87", "Wysy\xC5\x82""anie zaproszenia\xE2\x80\xA6",
              "Wys\xC5\x82""ano zaproszenie do %s", "Ty i %s jeste\xC5\x9B""cie znajomymi", "Wpisz Gamertag i numer"};
    default:
      return {"Add friend", "Type their Gamertag and number, like Nimbus#4827.", "Gamertag", "Number", "Send", "Shift",
              "Space", "Delete", "Back", "Sending the request\xE2\x80\xA6", "Request sent to %s", "You and %s are friends now",
              "Type the Gamertag and its number"};
  }
}

// The on-screen keyboard of the Add friend screen: the characters row by row, then the actions.
constexpr const char* kKeyRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
constexpr int kCharacterRows = 4;
enum class KeyAction { kShift, kSpace, kTag, kErase, kSend };
constexpr KeyAction kKeyActions[] = {KeyAction::kShift, KeyAction::kSpace, KeyAction::kTag, KeyAction::kErase,
                                     KeyAction::kSend};
constexpr int kKeyRowCount = kCharacterRows + 1;
int KeyRowLength(int row) {
  return row < kCharacterRows ? static_cast<int>(std::char_traits<char>::length(kKeyRows[row])) : 5;
}
// The longest a Gamertag and its number are.
constexpr size_t kGamertagLength = 15;
constexpr size_t kTagLength = 4;

std::string Format(const char* pattern, const std::string& value) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), pattern, value.c_str());
  return buffer;
}

std::string FormatNumber(const char* pattern, int value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), pattern, value);
  return buffer;
}

// How long ago an ISO 8601 date in UTC ("2026-11-01T10:00:00Z") was, as "12 min", "3 h" or "2 days";
// empty when it cannot be read.
std::string Ago(const Texts& texts, const std::string& iso) {
  std::tm tm = {};
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                  &tm.tm_sec) != 6) {
    return {};
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
#if defined(_WIN32)
  const std::time_t then = _mkgmtime(&tm);
#else
  const std::time_t then = timegm(&tm);
#endif
  const double seconds = std::max(0.0, std::difftime(std::time(nullptr), then));
  if (seconds < 3600) return FormatNumber(texts.minutes, std::max(1, static_cast<int>(seconds / 60)));
  if (seconds < 86400) return FormatNumber(texts.hours, static_cast<int>(seconds / 3600));
  return FormatNumber(texts.days, static_cast<int>(seconds / 86400));
}

// The order of the friends: favourites, then who plays, who is online and who is not, then by name.
std::vector<alboria::Friend> SortedFriends() {
  std::vector<alboria::Friend> friends = alboria::Friends();
  auto rank = [](const alboria::Friend& f) { return f.playing ? 0 : f.online ? 1 : 2; };
  std::sort(friends.begin(), friends.end(), [&](const alboria::Friend& a, const alboria::Friend& b) {
    if (a.favourite != b.favourite) return a.favourite;
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    return a.name < b.name;
  });
  return friends;
}

// What a friend is doing, in words.
std::string Activity(const Texts& texts, const alboria::Friend& f) {
  if (f.playing) {
    if (f.title_id != kGearsOfWar3) return texts.playing_other;
    return Format(f.joinable ? texts.playing_open : texts.playing_invite, "Gears of War 3");
  }
  if (f.online) return texts.online;
  const std::string ago = f.last_seen.empty() ? std::string() : Ago(texts, f.last_seen);
  return ago.empty() ? std::string(texts.offline) : Format(texts.last_seen, ago);
}

// The control that does a button's action, shown inside it: a controller's button (`pad` set) or a
// key (`key`).
struct PillGlyph {
  bool pad = false;
  PadLook look = PadLook::kGeneric;
  PadControl control = PadControl::kA;
  const char* key = nullptr;
};

// "Navigate", for moving up and down through a list, in the game's languages.
const char* NavigateText(uint32_t language) {
  switch (language) {
    case 2: return "移動";
    case 7: return "이동";
    case 8: return "移動";
    case 12: return "Навигация";
    case 5: return "Navegar";
    case 3: return "Navigieren";
    case 4: return "Naviguer";
    case 6: return "Naviga";
    case 9: return "Navegar";
    case 11: return "Nawigacja";
    default: return "Navigate";
  }
}

// The Settings page's texts, in the game's languages.
struct SettingsTexts {
  const char* tab;
  const char* menu;
  const char* menu_help;
  const char* notices;
  const char* notices_help;
  const char* adjust;
};

SettingsTexts LocalizedSettingsTexts(uint32_t language) {
  switch (language) {
    case 2:
      return {"設定", "メニューの効果音", "Alboria の画面での移動・決定・戻る", "通知",
              "ゲーム上に表示される招待やその他の通知", "調整"};
    case 7:
      return {"설정", "메뉴 효과음", "Alboria 화면에서 이동, 선택, 뒤로 가기", "알림",
              "게임 위에 표시되는 초대 및 기타 알림", "조정"};
    case 8:
      return {"設定", "選單音效", "在 Alboria 畫面中移動、選擇和返回", "通知", "遊戲上方顯示的邀請和其他通知", "調整"};
    case 12:
      return {"Настройки", "Звуки меню", "Перемещение, выбор и возврат на экранах Alboria", "Уведомления",
              "Приглашения и другие уведомления поверх игры", "Настроить"};
    case 5:
      return {"Ajustes", "Sonidos del menú", "Moverte, elegir y volver en las pantallas de Alboria", "Notificaciones",
              "Invitaciones y otros avisos sobre el juego", "Ajustar"};
    case 3:
      return {"Einstellungen", "Menütöne", "Bewegen, Auswählen und Zurück in Alborias Bildschirmen",
              "Benachrichtigungen", "Einladungen und andere Hinweise über dem Spiel", "Anpassen"};
    case 4:
      return {"Réglages", "Sons du menu", "Se déplacer, choisir et revenir dans les écrans d'Alboria", "Notifications",
              "Invitations et autres avis par-dessus le jeu", "Régler"};
    case 6:
      return {"Impostazioni", "Suoni del menu", "Muoversi, scegliere e tornare nelle schermate di Alboria", "Notifiche",
              "Inviti e altri avvisi sopra il gioco", "Regola"};
    case 9:
      return {"Configurações", "Sons do menu", "Mover, escolher e voltar nas telas da Alboria", "Notificações",
              "Convites e outros avisos sobre o jogo", "Ajustar"};
    case 11:
      return {"Ustawienia", "Dźwięki menu", "Poruszanie się, wybór i powrót na ekranach Alborii", "Powiadomienia",
              "Zaproszenia i inne powiadomienia nad grą", "Dostosuj"};
    default:
      return {"Settings", "Menu sounds", "Moving, choosing and going back in Alboria's screens", "Notifications",
              "Invitations and other notices over the game", "Adjust"};
  }
}

// The colours of a row's seal, light at its top and dark below; none (0) leaves the row without one.
struct SealTone {
  ImU32 light = 0;
  ImU32 dark = 0;
  // The same colours at `alpha`.
  SealTone At(int alpha) const {
    const auto with = [alpha](ImU32 c) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT); };
    return {with(light), with(dark)};
  }
};
// Gold while playing, green online, grey offline (the launcher's friend rows), and sky for a setting.
constexpr SealTone kPlayingTone{IM_COL32(246, 196, 69, 255), IM_COL32(183, 122, 12, 255)};
constexpr SealTone kOnlineTone{IM_COL32(70, 199, 154, 255), IM_COL32(18, 121, 90, 255)};
constexpr SealTone kOfflineTone{IM_COL32(142, 154, 184, 255), IM_COL32(76, 87, 120, 255)};
constexpr SealTone kSkyTone{IM_COL32(47, 94, 168, 255), IM_COL32(27, 47, 99, 255)};

// A carved button (alboria_style.h): amber for the main action, mother-of-pearl for the others,
// the control that does it in its seal when `glyph` is set (the focused row's buttons), Alboria's
// ring rune there otherwise. Returns its width; `hit` says whether the mouse clicked it this frame.
float DrawPill(ImDrawList* draw_list, ImFont* font, float size, ImVec2 right_center, const char* label, bool primary,
               float unit, int alpha, bool* hit, const PillGlyph* glyph = nullptr) {
  SealContent seal;
  if (glyph) {
    seal.control = true;
    seal.pad = glyph->pad;
    seal.look = glyph->look;
    seal.control_id = glyph->control;
    seal.key = glyph->key;
  }
  return DrawRuneButton(draw_list, font, size, right_center, label, primary ? Stone::kAmber : Stone::kPearl, unit, alpha,
                        hit, seal);
}

}  // namespace

AlboriaOverlay::AlboriaOverlay(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                               input::PlayerAssignment* assignment, uint32_t language, Page page,
                               ImFont* title_font, ImFont* body_font, rex::ui::ImmediateTexture* header,
                               std::function<void()> join_by_code, AlboriaOverlay** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      page_(page),
      title_font_(title_font),
      body_font_(body_font),
      header_(header),
      join_by_code_(std::move(join_by_code)),
      owner_(owner),
      opened_(Clock::now()),
      last_input_(input) {
  input::SetOverlayBlocking(true);
  alboria::SetOverlayShown(true);
  alboria::RefreshFriends();
  alboria_sounds::Play(Sound::kOpen);
}

void AlboriaOverlay::SetMessage(std::string text, bool error) {
  if (error) alboria_sounds::Play(Sound::kError);
  std::lock_guard lock(result_->mutex);
  result_->message = std::move(text);
  result_->error = error;
  result_->since = Clock::now();
}

void AlboriaOverlay::Start(const std::string& pending,
                           const std::function<void(alboria::ActionDone)>& action, const std::string& done_text,
                           bool close_when_done, Sound sound) {
  {
    std::lock_guard lock(result_->mutex);
    if (result_->busy) return;
    result_->busy = true;
    result_->message = pending;
    result_->error = false;
    result_->since = Clock::now();
  }
  alboria_sounds::Play(sound);
  const std::string failed = LocalizedTexts(language_).failed;
  action([result = result_, done_text, failed, close_when_done, language = language_](const std::string& error) {
    std::lock_guard lock(result->mutex);
    result->busy = false;
    result->since = Clock::now();
    if (error.empty()) {
      result->message = done_text;
      result->error = false;
      result->close_after = close_when_done;
    } else {
      result->message = Format(failed.c_str(), AlboriaErrorText(language, error));
      result->error = true;
      alboria_sounds::Play(Sound::kError);
    }
  });
}

void AlboriaOverlay::Act(bool secondary) {
  const Texts texts = LocalizedTexts(language_);
  if (!alboria::LiveSignedIn()) return;
  if (page_ == Page::kFriends) {
    // The first row is Add friend.
    if (focus_ == 0) {
      if (!secondary) {
        alboria_sounds::Play(Sound::kAccept);
        adding_ = true;
        add_name_.clear();
        add_tag_.clear();
        add_in_tag_ = false;
        key_row_ = 1;
        key_column_ = 0;
      }
      return;
    }
    const auto friends = SortedFriends();
    const int index = focus_ - 1;
    if (index >= static_cast<int>(friends.size())) return;
    const alboria::Friend f = friends[index];
    if (!secondary && f.joinable) {
      Start(texts.joining, [id = f.id](alboria::ActionDone done) { alboria::JoinFriend(id, std::move(done)); },
            texts.joining, true);
    } else if (secondary && alboria::InRoom() && f.online) {
      Start(texts.inviting,
            [id = f.id](alboria::ActionDone done) {
              alboria::InviteFriend(id);
              done({});
            },
            Format(texts.invited, f.name), false);
    }
  } else if (page_ == Page::kInvitations) {
    const auto invitations = alboria::Invitations();
    if (focus_ < 0 || focus_ >= static_cast<int>(invitations.size())) return;
    const std::string id = invitations[focus_].id;
    if (!secondary) {
      Start(texts.joining, [id](alboria::ActionDone done) { alboria::AcceptInvitation(id, std::move(done)); },
            texts.joining, true);
    } else {
      Start(texts.declined, [id](alboria::ActionDone done) { alboria::DeclineInvitation(id, std::move(done)); },
            texts.declined, false, Sound::kBack);
    }
  } else if (page_ == Page::kRoom && !secondary) {
    alboria_sounds::Play(Sound::kAccept);
    open_join_by_code_ = true;
    closing_ = true;
  }
}

AlboriaOverlay::Actions AlboriaOverlay::ReadInput() {
  Actions actions;
  if (assignment_ && input_) {
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
      if (pressed & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) actions.page = -1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) actions.page = 1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) actions.move = -1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) actions.move = 1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) actions.horizontal = -1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) actions.horizontal = 1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_A) actions.primary = true;
      if (pressed & rex::input::X_INPUT_GAMEPAD_X) actions.secondary = true;
      if (pressed & rex::input::X_INPUT_GAMEPAD_Y) actions.copy = true;
      if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) actions.close = true;
    }
  }
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) actions.close = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) actions.page = ImGui::GetIO().KeyShift ? -1 : 1;
  // Left and right change a setting's value on the Settings page, and the page elsewhere.
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) actions.horizontal = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) actions.horizontal = 1;
  // Up and down with the arrows, or with W and S as in the game.
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)) actions.move = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S)) actions.move = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    actions.primary = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_I, false)) actions.secondary = true;
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_C, false)) actions.copy = true;
  return actions;
}

void AlboriaOverlay::AddType(char c) {
  alboria_sounds::Play(Sound::kType);
  if (c == '#') {
    add_in_tag_ = !add_in_tag_;
    return;
  }
  if (add_in_tag_) {
    if (c >= '0' && c <= '9' && add_tag_.size() < kTagLength) add_tag_ += c;
    return;
  }
  if (add_name_.size() < kGamertagLength && (std::isalnum(static_cast<unsigned char>(c)) || c == ' ')) add_name_ += c;
}

void AlboriaOverlay::AddErase() {
  alboria_sounds::Play(Sound::kType);
  std::string& field = add_in_tag_ ? add_tag_ : add_name_;
  if (!field.empty()) {
    field.pop_back();
  } else if (add_in_tag_) {
    add_in_tag_ = false;
  }
}

void AlboriaOverlay::PressKey() {
  if (key_row_ < kCharacterRows) {
    char c = kKeyRows[key_row_][key_column_];
    if (!add_shift_ && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    AddType(c);
    return;
  }
  switch (kKeyActions[key_column_]) {
    case KeyAction::kShift:
      alboria_sounds::Play(Sound::kType);
      add_shift_ = !add_shift_;
      break;
    case KeyAction::kSpace: AddType(' '); break;
    case KeyAction::kTag: AddType('#'); break;
    case KeyAction::kErase: AddErase(); break;
    case KeyAction::kSend: SendRequest(); break;
  }
}

void AlboriaOverlay::SendRequest() {
  const AddTexts texts = LocalizedAddTexts(language_);
  std::string name = add_name_;
  while (!name.empty() && name.back() == ' ') name.pop_back();
  while (!name.empty() && name.front() == ' ') name.erase(name.begin());
  if (name.empty() || add_tag_.empty()) {
    SetMessage(texts.bad_name, true);
    return;
  }
  const std::string full = name + "#" + std::to_string(std::stoi(add_tag_));
  {
    std::lock_guard lock(result_->mutex);
    if (result_->busy) return;
    result_->busy = true;
    result_->message = texts.sending;
    result_->error = false;
    result_->since = Clock::now();
  }
  adding_ = false;
  alboria_sounds::Play(Sound::kAccept);
  const std::string failed = LocalizedTexts(language_).failed;
  alboria::SendFriendRequest(full, [result = result_, full, texts, failed, language = language_](const std::string& error) {
    std::lock_guard lock(result->mutex);
    result->busy = false;
    result->since = Clock::now();
    result->error = !error.empty() && error != "friends";
    if (result->error) alboria_sounds::Play(Sound::kError);
    result->message = error.empty()         ? Format(texts.sent, full)
                      : error == "friends" ? Format(texts.now_friends, full)
                                           : Format(failed.c_str(), AlboriaErrorText(language, error));
  });
}

bool AlboriaOverlay::AddFriendInput() {
  bool back = false;
  if (assignment_ && input_) {
    for (const Slot& slot : assignment_->Slots()) {
      if (slot.keyboard) continue;
      rex::input::X_INPUT_STATE state = {};
      // 0 is X_ERROR_SUCCESS.
      if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
      uint16_t buttons = state.gamepad.buttons;
      if (state.gamepad.thumb_lx <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
      if (state.gamepad.thumb_lx >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
      if (state.gamepad.thumb_ly >= kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
      if (state.gamepad.thumb_ly <= -kStickPress) buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
      auto found = previous_buttons_.find(slot.device.id);
      const bool seen = found != previous_buttons_.end();
      const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
      previous_buttons_[slot.device.id] = buttons;
      if (pressed) last_input_.NotePad(slot.device.id);
      const int key_row = key_row_;
      const int key_column = key_column_;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) key_row_ = std::max(0, key_row_ - 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) key_row_ = std::min(kKeyRowCount - 1, key_row_ + 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) key_column_--;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) key_column_++;
      key_column_ = std::clamp(key_column_, 0, KeyRowLength(key_row_) - 1);
      if (key_row_ != key_row || key_column_ != key_column) alboria_sounds::Play(Sound::kMove);
      if (pressed & rex::input::X_INPUT_GAMEPAD_A) PressKey();
      if (pressed & rex::input::X_INPUT_GAMEPAD_X) AddErase();
      if (pressed & rex::input::X_INPUT_GAMEPAD_Y) {
        alboria_sounds::Play(Sound::kType);
        add_shift_ = !add_shift_;
      }
      if (pressed & rex::input::X_INPUT_GAMEPAD_START) SendRequest();
      if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) back = true;
    }
  }
  // The keyboard, through ImGui: the runtime gives it key presses but no text, so the letters come
  // from the keys, with Shift for the capitals. Tab or # moves between the Gamertag and the number.
  last_input_.NoteKeyboard();
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) back = true;
  for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) AddType(static_cast<char>((io.KeyShift ? 'A' : 'a') + (k - ImGuiKey_A)));
  }
  for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k) {
    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) AddType(io.KeyShift && k == ImGuiKey_3 ? '#' : static_cast<char>('0' + (k - ImGuiKey_0)));
  }
  for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; ++k) {
    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) AddType(static_cast<char>('0' + (k - ImGuiKey_Keypad0)));
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Space)) AddType(' ');
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) AddType('#');
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) AddErase();
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) SendRequest();
  return back;
}

void AlboriaOverlay::OnDraw(ImGuiIO& io) {
  // Closing plays its animation, then waits for what closed it to be released
  // (coop_ui::CloseInputHeld); meanwhile the panel takes no input.
  if (closing_) {
    if (!closing_since_) {
      closing_since_ = Clock::now();
      alboria_sounds::Play(Sound::kClose);
    }
    std::vector<DeviceId> pads;
    if (assignment_) {
      for (const Slot& slot : assignment_->Slots()) {
        if (!slot.keyboard) pads.push_back(slot.device.id);
      }
    }
    const bool shown = std::chrono::duration<float>(Clock::now() - *closing_since_).count() < kCloseSeconds;
    if (!shown && !CloseInputHeld(input_, pads)) {
      Close();
      return;
    }
    if (!shown) return;
  }

  const Texts texts = LocalizedTexts(language_);
  const bool signed_in = alboria::LiveSignedIn();
  const auto friends = SortedFriends();
  const auto invitations = alboria::Invitations();
  const alboria::Status status = alboria::GetStatus();
  const bool in_room = status.state == alboria::State::kConnected && !status.code.empty();
  const int pages = 4;
  auto rows_on_page = [&]() -> int {
    if (page_ == Page::kFriends) return static_cast<int>(friends.size()) + 1;
    if (page_ == Page::kInvitations) return static_cast<int>(invitations.size());
    if (page_ == Page::kSettings) return 2;
    return 1;
  };
  const SettingsTexts settings = LocalizedSettingsTexts(language_);
  // The Settings page's rows: the group of sounds each one sets, and the sound it plays as a sample.
  const alboria_sounds::Group setting_groups[] = {alboria_sounds::Group::kMenu, alboria_sounds::Group::kNotices};
  auto change_volume = [&](int row, int volume) {
    const auto group = setting_groups[row];
    volume = std::clamp(volume, 0, alboria_sounds::kMaxVolume);
    if (volume == alboria_sounds::Volume(group)) return;
    alboria_sounds::SetVolume(group, volume);
    alboria_sounds::Play(group == alboria_sounds::Group::kMenu ? Sound::kAccept : Sound::kNotice);
  };

  Actions actions = adding_ || closing_ ? Actions{} : ReadInput();
  if (adding_ && !closing_ && AddFriendInput()) {
    alboria_sounds::Play(Sound::kBack);
    adding_ = false;
  }
  {
    std::lock_guard lock(result_->mutex);
    if (result_->close_after) {
      result_->close_after = false;
      closing_ = true;
    }
  }
  if (actions.close) closing_ = true;
  if (page_ == Page::kSettings && actions.horizontal) {
    change_volume(focus_, alboria_sounds::Volume(setting_groups[std::clamp(focus_, 0, 1)]) + actions.horizontal);
  } else if (actions.horizontal && !actions.page) {
    actions.page = actions.horizontal;
  }
  if (actions.page) {
    alboria_sounds::Play(Sound::kPage);
    page_ = static_cast<Page>((static_cast<int>(page_) + actions.page + pages) % pages);
    focus_ = 0;
  }
  const int focus = focus_;
  if (actions.move) focus_ += actions.move;
  focus_ = std::clamp(focus_, 0, std::max(0, rows_on_page() - 1));
  if (focus_ != focus) alboria_sounds::Play(Sound::kMove);
  if (actions.primary) Act(false);
  if (actions.secondary) Act(true);
  if (actions.copy && in_room) {
    ImGui::SetClipboardText(alboria::DisplayCode(status.code).c_str());
    alboria_sounds::Play(Sound::kAccept);
    SetMessage(texts.copied, false);
  }

  const Clock::time_point now = Clock::now();
  // How far the opening and the closing have gone, for the fade and the panel's motion.
  const float opened = std::chrono::duration<float>(now - opened_).count();
  const float closed = closing_since_ ? Ease01(std::chrono::duration<float>(now - *closing_since_).count() / kCloseSeconds)
                                      : 0.0f;
  const float fade = Ease01(opened / kFadeSeconds) * (1.0f - closed);
  auto a = [&](int alpha) { return static_cast<int>(alpha * fade); };
  const std::vector<Slot> slots = assignment_ ? assignment_->Slots() : std::vector<Slot>{};
  const rex::input::DeviceInfo* pad = nullptr;
  if (!last_input_.keyboard()) {
    for (const Slot& slot : slots) {
      if (!slot.keyboard && slot.device.id == last_input_.pad()) pad = &slot.device;
    }
    for (const Slot& slot : slots) {
      if (!pad && !slot.keyboard) pad = &slot.device;
    }
  }
  const PadLook look = pad ? LookOf(*pad) : PadLook::kGeneric;
  // The glyph a button shows: `control` on a controller, `key` on the keyboard.
  const PillGlyph enter_glyph{pad != nullptr, look, PadControl::kA, "Enter"};
  const PillGlyph second_glyph{pad != nullptr, look, PadControl::kX, "I"};

  // Sizes in pixels at 720p, over the 16:9 area the game image takes in the window.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_width = game_height * 16.0f / 9.0f;
  const float game_left = (io.DisplaySize.x - game_width) * 0.5f;
  const float game_top = (io.DisplaySize.y - game_height) * 0.5f;
  const float unit = game_height / 720.0f;
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  const ImVec2 mouse = io.MousePos;
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !closing_;
  auto inside = [&](ImVec2 min, ImVec2 max) {
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
  };

  // The game dimmed behind, with a touch of Alboria's night.
  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(8, 12, 30, a(160)));
  // Everything drawn from here on moves with the opening and the closing: scaled about the panel's
  // centre and lowered a little, until the drawing ends.
  const float opening = Ease01(opened / kOpenSeconds);
  const float scale = closing_since_ ? 1.0f - 0.06f * closed * closed : 0.9f + 0.1f * EaseOutBack(opening);
  const float rise = (closing_since_ ? 14.0f * closed : 24.0f * std::pow(1.0f - opening, 3.0f)) * unit;
  const PanelMotion motion(draw_list, ImVec2(game_left + game_width * 0.5f, game_top + game_height * 0.5f), scale, rise);

  // The panel: pearl glass in its carved golden frame (alboria_style.h).
  const float panel_width = 740.0f * unit;
  const float panel_height = 580.0f * unit;
  const float margin = 34.0f * unit;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  const float radius = 26.0f * unit;
  // Its head: the Ring Plaza painted, fading into the glass, as the website's places do. It lies in
  // the glass, under the panel's edge and groove, which run round it unbroken. The panel is opaque
  // over the game, so the painting fades into exactly the glass's colour.
  const float head_height = 120.0f * unit;
  const ImVec2 head_max(panel_max.x, panel_min.y + head_height);
  DrawCarvedPanel(draw_list, panel_min, panel_max, radius, unit, a(255), 1.0f, [&] {
    if (header_) {
      // The painting is wider than the head: its middle band, at the head's proportions.
      const float band = std::min(1.0f, (head_height / panel_width) * (1024.0f / 320.0f));
      draw_list->AddImageRounded(reinterpret_cast<ImTextureID>(header_), panel_min, head_max,
                                 ImVec2(0.0f, 0.5f - band * 0.5f), ImVec2(1.0f, 0.5f + band * 0.5f), White(a(255)),
                                 radius, ImDrawFlags_RoundCornersTop);
    }
    // The veil: clear at the top, the glass's own colour where the painting ends (DrawCarvedPanel's
    // gradient at that height), and a few pixels past it, so no seam shows.
    const float fade_end = head_height + 3.0f * unit;
    const float at = fade_end / panel_height;
    const ImU32 glass = IM_COL32(255 - static_cast<int>(19 * at), 255 - static_cast<int>(14 * at),
                                 255 - static_cast<int>(3 * at), a(255));
    const ImU32 clear = glass & ~IM_COL32_A_MASK;
    draw_list->AddRectFilledMultiColor(ImVec2(panel_min.x, panel_min.y + head_height * 0.25f),
                                       ImVec2(panel_max.x, panel_min.y + fade_end), clear, clear, glass, glass);
  });

  // The title on the painting: Alboria's planet in its pearl medallion, the name in ink, and the
  // account on a tag of glass on the right, its tag in gold.
  const float medallion = 26.0f * unit;
  const ImVec2 medallion_center(panel_min.x + margin + medallion, panel_min.y + head_height - 28.0f * unit);
  DrawMedallion(draw_list, medallion_center, medallion, a(255));
  DrawAlboriaPlanet(draw_list, medallion_center, medallion * 0.72f / 20.0f, a(255));
  draw_list->AddText(title_font, 32.0f * unit,
                     ImVec2(medallion_center.x + medallion + 14.0f * unit, medallion_center.y - 19.0f * unit), Ink(a(255)),
                     texts.title);
  const alboria::Identity identity = alboria::GetIdentity();
  if (identity.known) {
    const std::string name = identity.gamertag;
    const std::string tag = "#" + std::to_string(identity.tag);
    const float name_width = title_font->CalcTextSizeA(17.0f * unit, FLT_MAX, 0.0f, name.c_str()).x;
    const float tag_width = title_font->CalcTextSizeA(17.0f * unit, FLT_MAX, 0.0f, tag.c_str()).x;
    const ImVec2 max(panel_max.x - margin, medallion_center.y + 16.0f * unit);
    const ImVec2 min(max.x - name_width - tag_width - 28.0f * unit, medallion_center.y - 16.0f * unit);
    draw_list->AddRectFilled(min, max, White(a(215)), 16.0f * unit);
    draw_list->AddRect(min, max, White(a(255)), 16.0f * unit, 0, 1.0f * unit);
    draw_list->AddText(title_font, 17.0f * unit, ImVec2(min.x + 14.0f * unit, min.y + 6.0f * unit), Ink(a(255)), name.c_str());
    draw_list->AddText(title_font, 17.0f * unit, ImVec2(min.x + 14.0f * unit + name_width, min.y + 6.0f * unit),
                       GoldText(a(255)), tag.c_str());
  }

  // The tabs: small carved stones, the current one amber.
  const char* tab_names[] = {texts.friends, texts.invitations, texts.room, settings.tab};
  float tab_x = panel_min.x + margin + 10.0f * unit;
  const float tab_middle = panel_min.y + head_height + 42.0f * unit;
  for (int i = 0; i < pages; ++i) {
    std::string label = tab_names[i];
    if (i == 1 && !invitations.empty()) label += " (" + std::to_string(invitations.size()) + ")";
    const bool current = static_cast<int>(page_) == i;
    const Stone stone = current ? Stone::kAmber : Stone::kPearl;
    const float width =
        DrawRuneButton(draw_list, title_font, 16.0f * unit, ImVec2(0, 0), label.c_str(), stone, unit, 0, nullptr);
    bool hit = false;
    DrawRuneButton(draw_list, title_font, 16.0f * unit, ImVec2(tab_x + width, tab_middle), label.c_str(), stone, unit,
                   a(255), &hit);
    if (hit && !current) {
      page_ = static_cast<Page>(i);
      focus_ = 0;
    }
    tab_x += width + 26.0f * unit;
  }

  // The page.
  const float list_top = tab_middle + 38.0f * unit;
  const float list_bottom = panel_max.y - 72.0f * unit;
  const float row_height = 58.0f * unit;
  const float row_gap = 10.0f * unit;
  const float list_left = panel_min.x + margin;
  const float list_right = panel_max.x - margin;
  auto empty_text = [&](const char* text, const char* help = nullptr) {
    draw_list->AddText(body_font, 18.0f * unit, ImVec2(list_left, list_top + 10.0f * unit), NightSoft(a(255)),
                       Fit(body_font, 18.0f * unit, text, list_right - list_left).c_str());
    if (help) {
      draw_list->AddText(body_font, 15.0f * unit, ImVec2(list_left, list_top + 40.0f * unit), NightSoft(a(220)),
                         Fit(body_font, 15.0f * unit, help, list_right - list_left).c_str());
    }
  };
  // A row of the list: glass, the focused one bright in a golden halo; a seal of what it is (`seal`:
  // its light and dark colours, a ring rune in it), its name and what it says. Returns the row's
  // vertical middle.
  const int visible_rows = std::max(1, static_cast<int>((list_bottom - list_top + row_gap) / (row_height + row_gap)));
  const int first_row = std::clamp(focus_ - visible_rows + 1, 0, std::max(0, rows_on_page() - visible_rows));
  auto row = [&](int index, SealTone seal, const std::string& name, const std::string& detail) -> float {
    const float top = list_top + (index - first_row) * (row_height + row_gap);
    const ImVec2 min(list_left, top);
    const ImVec2 max(list_right, top + row_height);
    const bool focused = index == focus_;
    const float round = 22.0f * unit;
    if (focused) {
      draw_list->AddRect(ImVec2(min.x - 6.0f * unit, min.y - 6.0f * unit), ImVec2(max.x + 6.0f * unit, max.y + 6.0f * unit),
                         IM_COL32(255, 214, 120, a(70)), round + 6.0f * unit, 0, 6.0f * unit);
      draw_list->AddRect(ImVec2(min.x - 3.0f * unit, min.y - 3.0f * unit), ImVec2(max.x + 3.0f * unit, max.y + 3.0f * unit),
                         Gold(a(255)), round + 3.0f * unit, 0, 1.6f * unit);
      draw_list->AddRect(ImVec2(min.x - 1.0f * unit, min.y - 1.0f * unit), ImVec2(max.x + 1.0f * unit, max.y + 1.0f * unit),
                         GoldLight(a(255)), round + 1.0f * unit, 0, 2.0f * unit);
    }
    draw_list->AddRectFilled(min, max, focused ? White(a(250)) : White(a(165)), round);
    if (!focused) draw_list->AddRect(min, max, White(a(250)), round, 0, 1.0f * unit);
    const float middle = top + row_height * 0.5f;
    if (seal.light) DrawStatusSeal(draw_list, ImVec2(min.x + 28.0f * unit, middle), 15.0f * unit, seal.light, seal.dark, a(255));
    draw_list->AddText(title_font, 19.0f * unit, ImVec2(min.x + 54.0f * unit, top + 8.0f * unit), Ink(a(255)), name.c_str());
    draw_list->AddText(body_font, 14.0f * unit, ImVec2(min.x + 54.0f * unit, top + 32.0f * unit), NightSoft(a(255)),
                       Fit(body_font, 14.0f * unit, detail, (max.x - min.x) * 0.5f).c_str());
    if (clicked && inside(min, max)) focus_ = index;
    return middle;
  };
  bool busy;
  {
    std::lock_guard lock(result_->mutex);
    busy = result_->busy;
  }

  // Settings work without Alboria; the other pages need it.
  if (!signed_in && page_ != Page::kSettings) {
    empty_text(texts.unavailable, texts.unavailable_help);
  } else if (adding_) {
    // The Add friend screen: the two fields, then the on-screen keyboard with a controller.
    const AddTexts add = LocalizedAddTexts(language_);
    draw_list->AddText(title_font, 22.0f * unit, ImVec2(list_left, list_top - 2.0f * unit), Navy(a(255)), add.add_friend);
    draw_list->AddText(body_font, 14.0f * unit, ImVec2(list_left, list_top + 26.0f * unit), NightSoft(a(255)),
                       Fit(body_font, 14.0f * unit, add.help, list_right - list_left).c_str());
    const float field_top = list_top + 50.0f * unit;
    const float field_height = 40.0f * unit;
    const float tag_width = 110.0f * unit;
    const float hash_width = 24.0f * unit;
    const float name_width = std::min(360.0f * unit, list_right - list_left - tag_width - hash_width);
    auto field = [&](float left, float width, const std::string& text, const char* placeholder, bool focused) {
      const ImVec2 min(left, field_top);
      const ImVec2 max(left + width, field_top + field_height);
      draw_list->AddRectFilled(min, max, White(a(235)), 16.0f * unit);
      draw_list->AddRect(min, max, focused ? GoldDeep(a(255)) : Gold(a(190)), 16.0f * unit, 0, (focused ? 2.0f : 1.2f) * unit);
      if (focused) {
        draw_list->AddRect(ImVec2(min.x - 3.0f * unit, min.y - 3.0f * unit), ImVec2(max.x + 3.0f * unit, max.y + 3.0f * unit),
                           IM_COL32(255, 214, 120, a(90)), 19.0f * unit, 0, 3.0f * unit);
      }
      const bool empty = text.empty();
      std::string shown = empty ? placeholder : text;
      if (focused && std::fmod(std::chrono::duration<float>(now - opened_).count(), 1.0f) < 0.5f) shown = text + "|";
      draw_list->AddText(title_font, 20.0f * unit, ImVec2(min.x + 12.0f * unit, min.y + 9.0f * unit),
                         empty && !focused ? NightSoft(a(160)) : Navy(a(255)), shown.c_str());
      if (clicked && inside(min, max)) add_in_tag_ = &text == &add_tag_;
    };
    field(list_left, name_width, add_name_, add.gamertag, !add_in_tag_);
    AddTextCentered(draw_list, title_font, 24.0f * unit,
                    ImVec2(list_left + name_width + hash_width * 0.5f, field_top + field_height * 0.5f), Navy(a(255)), "#");
    field(list_left + name_width + hash_width, tag_width, add_tag_, add.tag, add_in_tag_);
    bool hit = false;
    const PillGlyph send_glyph{pad != nullptr, look, PadControl::kStart, "Enter"};
    const float send_width =
        DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(0, 0), add.send, true, unit, 0, nullptr, &send_glyph);
    DrawPill(draw_list, title_font, 16.0f * unit,
             ImVec2(std::min(list_right, list_left + name_width + hash_width + tag_width + 18.0f * unit + send_width),
                    field_top + field_height * 0.5f),
             add.send, true, unit, a(255), &hit, &send_glyph);
    if (hit) SendRequest();
    if (pad) {
      const float key = 30.0f * unit;
      const float gap = 5.0f * unit;
      const float keys_width = 10 * key + 9 * gap;
      const float keys_top = field_top + field_height + 16.0f * unit;
      const float keys_left = panel_min.x + (panel_width - keys_width) * 0.5f;
      for (int r = 0; r < kKeyRowCount; ++r) {
        const int length = KeyRowLength(r);
        const bool actions_row = r == kCharacterRows;
        const float width = actions_row ? (keys_width - (length - 1) * gap) / length : key;
        const float total = length * width + (length - 1) * gap;
        float key_x = keys_left + (keys_width - total) * 0.5f;
        const float key_y = keys_top + r * (key + gap);
        for (int c = 0; c < length; ++c) {
          const bool focused = r == key_row_ && c == key_column_;
          const ImVec2 min(key_x, key_y);
          const ImVec2 max(key_x + width, key_y + key);
          draw_list->AddRectFilled(min, max, focused ? Sun(a(255)) : White(a(255)), 8.0f * unit);
          draw_list->AddRect(min, max, focused ? SunShadow(a(255)) : SkyLine(a(255)), 8.0f * unit, 0, 1.5f * unit);
          const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
          std::string label;
          if (actions_row) {
            switch (kKeyActions[c]) {
              case KeyAction::kShift: label = add.shift; break;
              case KeyAction::kSpace: label = add.space; break;
              case KeyAction::kTag: label = "#"; break;
              case KeyAction::kErase: label = add.erase; break;
              case KeyAction::kSend: label = add.send; break;
            }
          } else {
            char ch = kKeyRows[r][c];
            if (!add_shift_ && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            label = std::string(1, ch);
          }
          const bool lit = actions_row && kKeyActions[c] == KeyAction::kShift && add_shift_;
          AddTextCentered(draw_list, title_font, (actions_row ? 14.0f : 17.0f) * unit, center,
                          lit && !focused ? SkyDeep(a(255)) : Navy(a(255)), label.c_str());
          key_x += width + gap;
        }
      }
    }
  } else if (page_ == Page::kFriends) {
    // The first row: Add friend.
    if (first_row == 0) {
      const float middle = row(0, SealTone{}, LocalizedAddTexts(language_).add_friend, LocalizedAddTexts(language_).help);
      // Its seal: amber, with a plus.
      const ImVec2 seal(list_left + 28.0f * unit, middle);
      draw_list->AddCircleFilled(seal, 17.4f * unit, Gold(a(255)), 40);
      draw_list->AddCircleFilled(seal, 16.2f * unit, GoldLight(a(255)), 40);
      const int first = draw_list->VtxBuffer.Size;
      draw_list->AddCircleFilled(seal, 15.0f * unit, White(a(255)), 40);
      ShadeVertical(draw_list, first, seal.y - 15.0f * unit, seal.y + 15.0f * unit, IM_COL32(255, 216, 102, 255),
                    IM_COL32(201, 138, 16, 255));
      AddTextCentered(draw_list, title_font, 24.0f * unit, seal, White(a(255)), "+");
      // Focused, a carved button at its end with the control that opens it, as the other rows have.
      if (focus_ == 0) {
        DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(list_right - 18.0f * unit, middle),
                 LocalizedAddTexts(language_).add_friend, true, unit, a(255), nullptr, &enter_glyph);
      }
      bool hit = false;
      if (clicked && inside(ImVec2(list_left, middle - row_height * 0.5f), ImVec2(list_right, middle + row_height * 0.5f))) {
        hit = true;
      }
      if (hit && !busy) {
        focus_ = 0;
        Act(false);
      }
    }
    if (friends.empty()) {
      draw_list->AddText(body_font, 15.0f * unit, ImVec2(list_left, list_top + row_height + 18.0f * unit), NightSoft(a(255)),
                         Fit(body_font, 15.0f * unit, texts.no_friends, list_right - list_left).c_str());
    }
    for (int i = std::max(first_row, 1); i <= static_cast<int>(friends.size()) && i < first_row + visible_rows; ++i) {
      const alboria::Friend& f = friends[i - 1];
      const SealTone tone = f.playing ? kPlayingTone : f.online ? kOnlineTone : kOfflineTone;
      const float middle = row(i, tone.At(a(255)), f.name, Activity(texts, f));
      // A favourite: a ring of golden light around its seal (the fonts have no star).
      if (f.favourite) {
        draw_list->AddCircle(ImVec2(list_left + 28.0f * unit, middle), 21.0f * unit, IM_COL32(255, 201, 60, a(255)), 0,
                             2.0f * unit);
      }
      float right = list_right - 18.0f * unit;
      bool hit = false;
      if (f.joinable) {
        right -= DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.join, true, unit, a(255), &hit,
                          i == focus_ ? &enter_glyph : nullptr) +
                 24.0f * unit;
        if (hit && !busy) {
          focus_ = i;
          Act(false);
        }
      }
      if (in_room && f.online) {
        DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.invite, false, unit, a(255), &hit,
                 i == focus_ ? &second_glyph : nullptr);
        if (hit && !busy) {
          focus_ = i;
          Act(true);
        }
      }
    }
  } else if (page_ == Page::kInvitations) {
    if (invitations.empty()) empty_text(texts.no_invitations);
    for (int i = first_row; i < static_cast<int>(invitations.size()) && i < first_row + visible_rows; ++i) {
      const float middle =
          row(i, kPlayingTone.At(a(255)), invitations[i].from_name, Format(texts.invites_you, invitations[i].from_name));
      float right = list_right - 18.0f * unit;
      bool hit = false;
      right -= DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.join, true, unit, a(255), &hit,
                        i == focus_ ? &enter_glyph : nullptr) +
               24.0f * unit;
      if (hit && !busy) {
        focus_ = i;
        Act(false);
      }
      DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.not_now, false, unit, a(255), &hit,
               i == focus_ ? &second_glyph : nullptr);
      if (hit && !busy) {
        focus_ = i;
        Act(true);
      }
    }
  } else if (page_ == Page::kSettings) {
    // Two volumes, each a bar of ten steps at the row's end; the mouse sets one by clicking a step.
    const char* labels[] = {settings.menu, settings.notices};
    const char* helps[] = {settings.menu_help, settings.notices_help};
    for (int i = 0; i < 2; ++i) {
      const float middle = row(i, kSkyTone.At(a(255)), labels[i], helps[i]);
      const int volume = alboria_sounds::Volume(setting_groups[i]);
      const float step = 16.0f * unit;
      const float gap = 4.0f * unit;
      const float number_width = title_font->CalcTextSizeA(18.0f * unit, FLT_MAX, 0.0f, "10").x;
      const float bar_right = list_right - 22.0f * unit - number_width - 12.0f * unit;
      const float bar_left = bar_right - alboria_sounds::kMaxVolume * (step + gap) + gap;
      for (int v = 0; v < alboria_sounds::kMaxVolume; ++v) {
        const ImVec2 min(bar_left + v * (step + gap), middle - 9.0f * unit);
        const ImVec2 max(min.x + step, middle + 9.0f * unit);
        // A lit step is a small amber stone, the others empty glass with a golden line.
        if (v < volume) {
          const int first = draw_list->VtxBuffer.Size;
          draw_list->AddRectFilled(min, max, White(a(255)), 5.0f * unit);
          ShadeVertical(draw_list, first, min.y, max.y, IM_COL32(255, 243, 196, 255), IM_COL32(244, 179, 58, 255));
          draw_list->AddRect(min, max, GoldDeep(a(255)), 5.0f * unit, 0, 1.2f * unit);
        } else {
          draw_list->AddRectFilled(min, max, IM_COL32(216, 184, 102, a(50)), 5.0f * unit);
          draw_list->AddRect(min, max, Gold(a(140)), 5.0f * unit, 0, 1.0f * unit);
        }
        if (clicked && inside(ImVec2(min.x, min.y - 6.0f * unit), ImVec2(max.x + gap, max.y + 6.0f * unit))) {
          focus_ = i;
          change_volume(i, v + 1 == volume ? v : v + 1);
        }
      }
      const std::string number = std::to_string(volume);
      draw_list->AddText(title_font, 18.0f * unit,
                         ImVec2(list_right - 22.0f * unit - title_font->CalcTextSizeA(18.0f * unit, FLT_MAX, 0.0f,
                                                                                        number.c_str()).x,
                                middle - 11.0f * unit),
                         Navy(a(255)), number.c_str());
    }
  } else {
    // The room: its code in big letters, to copy, and Join by code.
    if (in_room) {
      draw_list->AddText(body_font, 16.0f * unit, ImVec2(list_left, list_top + 6.0f * unit), NightSoft(a(255)),
                         texts.room_code);
      const std::string code = alboria::DisplayCode(status.code);
      draw_list->AddText(title_font, 48.0f * unit, ImVec2(list_left, list_top + 30.0f * unit), Navy(a(255)), code.c_str());
      const float code_width = title_font->CalcTextSizeA(48.0f * unit, FLT_MAX, 0.0f, code.c_str()).x;
      bool hit = false;
      const PillGlyph copy_glyph{pad != nullptr, look, PadControl::kY, "Ctrl+C"};
      const float pill_width = DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(0, 0), texts.copy, false, unit, 0,
                                        nullptr, &copy_glyph);
      DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(list_left + code_width + 24.0f * unit + pill_width,
                                                           list_top + 58.0f * unit),
               texts.copy, false, unit, a(255), &hit, &copy_glyph);
      if (hit) {
        ImGui::SetClipboardText(code.c_str());
        SetMessage(texts.copied, false);
      }
    } else {
      empty_text(texts.no_room);
    }
    bool hit = false;
    const float join_width = DrawPill(draw_list, title_font, 18.0f * unit, ImVec2(0, 0), texts.join_by_code, true, unit,
                                      0, nullptr, &enter_glyph);
    DrawPill(draw_list, title_font, 18.0f * unit, ImVec2(list_left + join_width, list_top + 130.0f * unit),
             texts.join_by_code, true, unit, a(255), &hit, &enter_glyph);
    if (hit) Act(false);
  }

  // The result of the last action, for a few seconds.
  {
    std::lock_guard lock(result_->mutex);
    if (!result_->message.empty() &&
        (result_->busy || std::chrono::duration<float>(now - result_->since).count() < kMessageSeconds)) {
      draw_list->AddText(body_font, 15.0f * unit, ImVec2(list_left, list_bottom + 4.0f * unit),
                         result_->error ? Crimson(a(255)) : NightSoft(a(255)),
                         Fit(body_font, 15.0f * unit, result_->message, list_right - list_left).c_str());
    }
  }

  // Button hints along the bottom, under an engraved golden line, with the glyphs of the input in use.
  const float hints_y = panel_max.y - 38.0f * unit;
  draw_list->AddLine(ImVec2(panel_min.x + margin, hints_y - 14.0f * unit), ImVec2(panel_max.x - margin, hints_y - 14.0f * unit),
                     Gold(a(150)), 1.0f * unit);
  float x = panel_min.x + margin;
  const float glyph = 20.0f * unit;
  auto hint = [&](PadControl control, const char* key, const char* text) {
    if (pad) {
      const float width = AlboriaGlyph(draw_list, title_font, ImVec2(0, 0), glyph, look, control, 0);
      x += AlboriaGlyph(draw_list, title_font, ImVec2(x + width * 0.5f, hints_y + 9.0f * unit), glyph, look, control, a(255)) +
           6.0f * unit;
    } else {
      x += AlboriaKey(draw_list, title_font, x, hints_y + 9.0f * unit, glyph, key, a(255)) + 6.0f * unit;
    }
    draw_list->AddText(body_font, 15.0f * unit, ImVec2(x, hints_y), Navy(a(255)), text);
    x += body_font->CalcTextSizeA(15.0f * unit, FLT_MAX, 0.0f, text).x + 20.0f * unit;
  };
  if (adding_) {
    const AddTexts add = LocalizedAddTexts(language_);
    if (pad) {
      hint(PadControl::kX, nullptr, add.erase);
      hint(PadControl::kY, nullptr, add.shift);
    } else {
      hint(PadControl::kA, "Tab", "#");
    }
    hint(PadControl::kB, "Esc", add.back);
    return;
  }
  if (pad) {
    const float width = AlboriaGlyph(draw_list, title_font, ImVec2(0, 0), glyph, look, PadControl::kLeftShoulder, 0);
    x += AlboriaGlyph(draw_list, title_font, ImVec2(x + width * 0.5f, hints_y + 9.0f * unit), glyph, look,
                      PadControl::kLeftShoulder, a(255)) +
         4.0f * unit;
    hint(PadControl::kRightShoulder, nullptr, texts.change_page);
  } else {
    hint(PadControl::kA, "Tab", texts.change_page);
  }
  // Up and down through a list; what each button does shows on the focused row's buttons.
  auto arrows_hint = [&](bool vertical, const char* text) {
    const float width = AlboriaArrowsGlyph(draw_list, ImVec2(0, 0), glyph, pad != nullptr, vertical, 0);
    x += AlboriaArrowsGlyph(draw_list, ImVec2(x + width * 0.5f, hints_y + 9.0f * unit), glyph, pad != nullptr, vertical,
                            a(255)) +
         6.0f * unit;
    draw_list->AddText(body_font, 15.0f * unit, ImVec2(x, hints_y), Navy(a(255)), text);
    x += body_font->CalcTextSizeA(15.0f * unit, FLT_MAX, 0.0f, text).x + 20.0f * unit;
  };
  if (page_ != Page::kRoom && (signed_in || page_ == Page::kSettings) && rows_on_page() > 1) {
    arrows_hint(true, NavigateText(language_));
  }
  if (page_ == Page::kSettings) arrows_hint(false, settings.adjust);
  hint(PadControl::kB, "Esc", texts.close);
}

void AlboriaOverlay::OnClose() {
  if (!closing_since_) alboria_sounds::Play(Sound::kClose);
  input::SetOverlayBlocking(false);
  alboria::SetOverlayShown(false);
  if (owner_) *owner_ = nullptr;
  if (open_join_by_code_ && join_by_code_) join_by_code_();
}

}  // namespace azura
