#include "alboria_notices.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>

#include "alboria_sounds.h"
#include "alboria_style.h"
#include "input/overlay_input.h"
#include "online_room_overlay.h"
#include "menu_language.h"

namespace azura {

namespace {

using namespace coop_ui;
using namespace alboria_style;
using rex::input::PadControl;
using Slot = input::PlayerAssignment::Slot;
namespace alboria = rex::system::xam::alboria;

// The animation, in seconds: the planet grows (with a bounce), then the panel unfolds while the
// planet settles, then the text fades in. Leaving runs it backwards, faster.
constexpr float kGrowSeconds = 0.45f;
constexpr float kUnfoldStart = 0.3f;
constexpr float kUnfoldSeconds = 0.4f;
constexpr float kTextStart = 0.55f;
constexpr float kTextSeconds = 0.25f;
constexpr float kFoldSeconds = 0.3f;
constexpr float kShrinkStart = 0.22f;
constexpr float kShrinkSeconds = 0.28f;
// How long each kind stays, from its first frame.
constexpr float kInvitationSeconds = 12.0f;
constexpr float kOtherSeconds = 7.0f;
// Alboria's service notices stay longer: they are read, not glanced at.
constexpr float kServiceSeconds = 12.0f;
// The game's name in the service notices' texts ({game}).
constexpr const char* kGameName = "Gears of War 3";
// How long the end of an accept (joining, or why it failed) stays before the notice leaves.
constexpr float kAcceptedSeconds = 1.5f;
constexpr float kFailedSeconds = 4.0f;
// The pause between two notices.
constexpr float kGapSeconds = 0.4f;
// How fast the moon travels the ring, in radians a second, and the hint's pulse, in cycles a second.
constexpr float kMoonSpeed = 1.8f;
constexpr float kPulseSpeed = 1.1f;
// The space after each of the hints, in units.
constexpr float kHintGap = 12.0f;

struct Texts {
  const char* invitation;
  const char* friend_request;
  const char* in_your_room;
  const char* invites;
  const char* wants_friend;
  const char* answer_in_launcher;
  const char* joined;
  const char* accept;
  const char* alboria;
  const char* joining;
  // The label and the line of a match that ended as its host left (shown in the name's place).
  const char* match;
  const char* host_left;
  // The label and the line of a friend who came online.
  const char* friend_online;
  const char* came_online;
  // The line of a map the host chose that a player of the pre-game lobby does not have.
  const char* map_not_shared;
};

// In the game's languages; the runtime loads the Cyrillic and CJK fonts each one needs.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"招待", "フレンドリクエスト", "あなたのルーム", "あなたをルームに招待しています", "フレンドになりたがっています", "Alboria ランチャーから返答してください",
              "あなたのルームに参加しました", "承諾", "Alboria", "ルームに参加中…", "マッチ", "マッチが終了しました",
              "フレンドがオンライン", "オンラインになりました",
              "ロビーにこのマップを持っていないプレイヤーがいます"};
    case 7:  // Korean
      return {"초대", "친구 요청", "내 방", "방에 초대했습니다", "친구가 되고 싶어 합니다", "Alboria 런처에서 응답하세요", "내 방에 들어왔습니다", "수락",
              "Alboria", "방에 참가 중…", "매치", "매치가 끝났습니다",
              "친구 접속", "온라인이 되었습니다",
              "로비에 이 맵이 없는 플레이어가 있습니다"};
    case 8:  // Traditional Chinese
      return {"邀請", "好友請求", "你的房間", "邀請你加入房間", "想成為你的好友", "請從 Alboria 啟動器回覆", "加入了你的房間", "接受", "Alboria",
              "正在加入房間…", "對戰", "對戰已結束",
              "好友上線", "上線了",
              "大廳中有玩家沒有這張地圖"};
    case 12:  // Russian
      return {"ПРИГЛАШЕНИЕ", "ЗАПРОС В ДРУЗЬЯ", "В ВАШЕЙ КОМНАТЕ", "приглашает вас в комнату",
              "хочет добавить вас в друзья", "Ответьте в лаунчере Alboria", "вошёл в вашу комнату", "Принять",
              "Alboria", "Вход в комнату…", "МАТЧ", "Матч завершён",
              "ДРУГ В СЕТИ", "теперь в сети",
              "У игрока в лобби нет этой карты"};
    case 5:  // Spanish
      return {"INVITACI\xC3\x93N", "SOLICITUD DE AMISTAD", "EN TU SALA", "te invita a su sala",
              "quiere ser tu amigo", "Resp\xC3\xB3ndela desde el launcher de Alboria", "ha entrado en tu sala",
              "Aceptar", "Alboria", "Uni\xC3\xA9ndote a la sala\xE2\x80\xA6", "PARTIDA",
              "La partida ha terminado",
              "AMIGO CONECTADO", "se ha conectado",
              "Un jugador de la sala no tiene este mapa"};
    case 3:  // German
      return {"EINLADUNG", "FREUNDSCHAFTSANFRAGE", "IN DEINEM RAUM", "l\xC3\xA4""dt dich in den Raum ein",
              "m\xC3\xB6""chte dein Freund sein", "Beantworte sie im Alboria-Launcher",
              "ist deinem Raum beigetreten", "Annehmen", "Alboria", "Raum wird betreten\xE2\x80\xA6", "MATCH",
              "Das Match ist beendet",
              "FREUND ONLINE", "ist jetzt online",
              "Ein Spieler in der Lobby hat diese Karte nicht"};
    case 4:  // French
      return {"INVITATION", "DEMANDE D'AMI", "DANS VOTRE SALON", "vous invite dans son salon",
              "veut \xC3\xAAtre votre ami", "R\xC3\xA9pondez-y depuis le launcher Alboria", "a rejoint votre salon",
              "Accepter", "Alboria", "Connexion au salon\xE2\x80\xA6", "PARTIE",
              "La partie est termin\xC3\xA9""e",
              "AMI EN LIGNE", "est en ligne",
              "Un joueur du salon n'a pas cette carte"};
    case 6:  // Italian
      return {"INVITO", "RICHIESTA DI AMICIZIA", "NELLA TUA STANZA", "ti invita nella sua stanza",
              "vuole essere tuo amico", "Rispondi dal launcher di Alboria", "\xC3\xA8 entrato nella tua stanza",
              "Accetta", "Alboria", "Ingresso nella stanza\xE2\x80\xA6", "PARTITA",
              "La partita \xC3\xA8 finita",
              "AMICO ONLINE", "\xC3\xA8 online",
              "Un giocatore della lobby non ha questa mappa"};
    case 9:  // Portuguese
      return {"CONVITE", "PEDIDO DE AMIZADE", "NA SUA SALA", "convidou voc\xC3\xAA para a sala",
              "quer ser seu amigo", "Responda pelo launcher da Alboria", "entrou na sua sala", "Aceitar",
              "Alboria", "Entrando na sala\xE2\x80\xA6", "PARTIDA", "A partida terminou",
              "AMIGO ONLINE", "ficou online",
              "Um jogador da sala n\xC3\xA3o tem este mapa"};
    case 11:  // Polish
      return {"ZAPROSZENIE", "ZAPROSZENIE DO ZNAJOMYCH", "W TWOIM POKOJU", "zaprasza ci\xC4\x99 do pokoju",
              "chce zosta\xC4\x87 twoim znajomym", "Odpowiedz w launcherze Alboria",
              "do\xC5\x82\xC4\x85""czy\xC5\x82 do twojego pokoju", "Akceptuj", "Alboria",
              "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6", "MECZ",
              "Mecz si\xC4\x99 zako\xC5\x84""czy\xC5\x82",
              "ZNAJOMY DOST\xC4\x98PNY", "jest dost\xC4\x99pny",
              "Gracz w poczekalni nie ma tej mapy"};
    case kLanguageCzech:
      return {"POZVÁNKA", "ŽÁDOST O PŘÁTELSTVÍ", "VE VAŠÍ MÍSTNOSTI", "vás zve do své místnosti",
              "si vás chce přidat mezi přátele", "Odpovězte v launcheru Alboria", "je teď ve vaší místnosti",
              "Přijmout", "Alboria", "Připojování k místnosti…", "ZÁPAS", "Zápas skončil",
              "PŘÍTEL ONLINE", "je teď online",
              "Hráč v předsálí tuto mapu nemá"};
    case kLanguageHungarian:
      return {"MEGHÍVÓ", "BARÁTKÉRELEM", "A SZOBÁDBAN", "meghívott a szobájába", "szeretne a barátod lenni",
              "Válaszolj rá az Alboria launcherben", "csatlakozott a szobádhoz", "Elfogadás", "Alboria",
              "Csatlakozás a szobához…", "MECCS", "A meccs véget ért",
              "BARÁT ONLINE", "online lett",
              "A váróterem egyik játékosának nincs meg ez a pálya"};
    default:
      return {"INVITATION", "FRIEND REQUEST", "IN YOUR ROOM", "invites you to their room",
              "wants to be your friend", "Answer it from the Alboria launcher", "joined your room", "Accept",
              "Alboria", "Joining the room\xE2\x80\xA6", "MATCH", "The match has ended",
              "FRIEND ONLINE", "is online",
              "A player in the lobby does not have this map"};
  }
}

// Alboria's service notices over the game: the launcher's own texts (launcher/src/locales,
// alboriaService), with {game} and {minutes} filled in.
struct ServiceTexts {
  const char* warn_maintenance_all;
  const char* warn_maintenance_game;
  const char* warn_restart;
  const char* warn_game_off;
  const char* warn_close_rooms;
  const char* then_maintenance;
  const char* then_restart;
  const char* then_game_off;
  const char* then_close_rooms;
  const char* in_a_minute;
  const char* in_minutes;
  const char* for_everyone;
  const char* for_game;
};

ServiceTexts LocalizedServiceTexts(uint32_t language) {
  switch (language) {
    case 2:  // Japanese
      return {"Alboria がメンテナンスに入ります", "{game} のオンラインがメンテナンスに入ります", "Alboria が再起動します", "{game} のオンラインプレイが停止します", "{game} のルームが閉じます", "開いているルームは続きます。新しいルームは終了まで待機します。", "新しいルームは復帰まで一時停止します。開いているルームは最大 30 分で終了します。", "そのときルームが閉じます。", "それまでに試合を終えてください。", "1 分後", "{minutes} 分後", "Alboria · 全員へ", "Alboria · {game}"};
    case 7:  // Korean
      return {"Alboria가 점검에 들어갑니다", "{game} 온라인이 점검에 들어갑니다", "Alboria가 다시 시작됩니다", "{game} 온라인 플레이가 꺼집니다", "{game} 방이 닫힙니다", "열린 방은 계속되고 새 방은 끝날 때까지 기다립니다.", "새 방은 돌아올 때까지 멈춥니다. 열린 방은 최대 30분 안에 끝내야 합니다.", "그때 방이 닫힙니다.", "그 전에 경기를 마치세요.", "1분 후", "{minutes}분 후", "Alboria · 모두에게", "Alboria · {game}"};
    case 8:  // Traditional Chinese
      return {"Alboria 即將進入維護", "{game} 線上即將進入維護", "Alboria 即將重新啟動", "{game} 的線上遊戲即將關閉", "{game} 的房間即將關閉", "已開啟的房間會繼續；新房間需等到結束。", "新房間暫停至恢復為止。已開啟的房間最多有 30 分鐘可完成。", "屆時其房間會關閉。", "請在那之前結束對戰。", "1 分鐘後", "{minutes} 分鐘後", "Alboria · 給所有人", "Alboria · {game}"};
    case 12:  // Russian
      return {"Alboria уходит на обслуживание", "Онлайн {game} уходит на обслуживание", "Alboria перезапускается", "Онлайн-игра в {game} отключается", "Комнаты {game} закрываются", "Открытые комнаты продолжают работу; новые ждут окончания.", "Новые комнаты приостановлены до возвращения. У открытых есть до 30 минут, чтобы закончить.", "Тогда её комнаты закроются.", "Закончите матч до этого.", "через минуту", "через {minutes} мин", "Alboria · для всех", "Alboria · {game}"};
    case 5:  // Spanish
      return {"Alboria entra en mantenimiento", "El online de {game} entra en mantenimiento", "Alboria se reinicia", "El online de {game} se desactiva", "Las salas de {game} se cierran", "Las salas abiertas siguen; las nuevas esperan hasta que acabe.", "Las salas nuevas se pausan hasta que vuelva. Las abiertas tienen hasta 30 minutos para terminar.", "Entonces se cierran sus salas.", "Termina tu partida antes.", "en un minuto", "en {minutes} min", "Alboria · para todos", "Alboria · {game}"};
    case 3:  // German
      return {"Alboria geht in Wartung", "Online von {game} geht in Wartung", "Alboria startet neu", "Onlinespiel von {game} wird ausgeschaltet", "Räume von {game} schließen", "Offene Räume laufen weiter; neue warten bis zum Ende.", "Neue Räume pausieren bis zur Rückkehr. Offene Räume haben bis zu 30 Minuten zum Beenden.", "Dann schließen seine Räume.", "Beende deine Partie vorher.", "in einer Minute", "in {minutes} Min.", "Alboria · für alle", "Alboria · {game}"};
    case 4:  // French
      return {"Alboria passe en maintenance", "Le jeu en ligne de {game} passe en maintenance", "Alboria redémarre", "Le jeu en ligne de {game} se désactive", "Les salons de {game} ferment", "Les salons ouverts continuent ; les nouveaux attendent la fin.", "Les nouveaux salons sont suspendus jusqu'au retour. Les salons ouverts ont jusqu'à 30 minutes pour finir.", "Ses salons ferment alors.", "Terminez votre partie avant.", "dans une minute", "dans {minutes} min", "Alboria · pour tous", "Alboria · {game}"};
    case 6:  // Italian
      return {"Alboria entra in manutenzione", "L'online di {game} entra in manutenzione", "Alboria si riavvia", "Il gioco online di {game} si disattiva", "Le stanze di {game} chiudono", "Le stanze aperte continuano; le nuove attendono la fine.", "Le nuove stanze si fermano fino al ritorno. Quelle aperte hanno fino a 30 minuti per finire.", "Allora le sue stanze chiudono.", "Finisci la partita prima.", "tra un minuto", "tra {minutes} min", "Alboria · per tutti", "Alboria · {game}"};
    case 9:  // Portuguese
      return {"O Alboria entra em manutenção", "O online de {game} entra em manutenção", "O Alboria reinicia", "O jogo online de {game} será desativado", "As salas de {game} fecham", "As salas abertas continuam; as novas esperam até o fim.", "Novas salas pausam até voltar. As abertas têm até 30 minutos para terminar.", "Então as salas fecham.", "Termine sua partida antes.", "em um minuto", "em {minutes} min", "Alboria · para todos", "Alboria · {game}"};
    case 11:  // Polish
      return {"Alboria przechodzi w konserwację", "Online {game} przechodzi w konserwację", "Alboria uruchamia się ponownie", "Gra online w {game} zostanie wyłączona", "Pokoje {game} zostaną zamknięte", "Otwarte pokoje działają dalej; nowe czekają do końca.", "Nowe pokoje są wstrzymane do powrotu. Otwarte mają do 30 minut na zakończenie.", "Wtedy jej pokoje zostaną zamknięte.", "Dokończ mecz wcześniej.", "za minutę", "za {minutes} min", "Alboria · dla wszystkich", "Alboria · {game}"};
    case kLanguageCzech:
      return {"Alboria přechází do údržby", "Online {game} přechází do údržby", "Alboria se restartuje", "Online hra {game} se vypne", "Místnosti {game} se zavřou", "Otevřené místnosti pokračují; nové čekají do konce.", "Nové místnosti se pozastaví do návratu. Otevřené mají až 30 minut na dokončení.", "Pak se její místnosti zavřou.", "Dohrajte zápas předtím.", "za minutu", "za {minutes} min", "Alboria · pro všechny", "Alboria · {game}"};
    case kLanguageHungarian:
      return {"Az Alboria karbantartásba lép", "A(z) {game} online része karbantartásba lép", "Az Alboria újraindul", "A(z) {game} online játéka kikapcsol", "A(z) {game} szobái bezárnak", "A nyitott szobák folytatódnak; az újak a végéig várnak.", "Új szobák szünetelnek a visszatérésig. A nyitottaknak legfeljebb 30 percük van befejezni.", "Ekkor bezárnak a szobái.", "Fejezd be előtte a meccset.", "egy perc múlva", "{minutes} perc múlva", "Alboria · mindenkinek", "Alboria · {game}"};
    default:
      return {"Alboria goes into maintenance", "{game} online goes into maintenance", "Alboria restarts", "Online play of {game} switches off", "{game} rooms close", "Rooms already open go on; new rooms wait until it ends.", "New rooms pause until it is back. Open rooms get up to 30 minutes to finish.", "Its rooms close then.", "Finish your match before then.", "in a minute", "in {minutes} min", "Alboria · for everyone", "Alboria · {game}"};
  }
}

// `text` with each `{name}` replaced by `value`.
std::string Fill(std::string text, const char* name, const std::string& value) {
  const std::string field = std::string("{") + name + "}";
  for (size_t at = text.find(field); at != std::string::npos; at = text.find(field, at + value.size())) {
    text.replace(at, field.size(), value);
  }
  return text;
}

// A notice's text cut to about `limit` bytes at a space, with an ellipsis, so the panel stays on screen.
std::string Shortened(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t cut = text.rfind(' ', limit);
  if (cut == std::string::npos || cut < limit / 2) {
    cut = limit;
    // Not inside a UTF-8 character.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  }
  return text.substr(0, cut) + "\xE2\x80\xA6";
}

float Clamp01(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

// Overshoots a little before it settles, for the planet's bounce.
float EaseOutBack(float t) {
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

float EaseOutCubic(float t) {
  return 1.0f - std::pow(1.0f - t, 3.0f);
}

float Seconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<float>(duration).count();
}

// The overlay's key as the launcher set it (bind_alboria_overlay), for the keyboard's hints.
std::string OverlayKey() {
  std::string key = rex::cvar::GetFlagByName("bind_alboria_overlay");
  return key.empty() ? std::string("F1") : key;
}

}  // namespace

AlboriaNotices::AlboriaNotices(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                               input::PlayerAssignment* assignment, uint32_t language, ImFont* title_font,
                               ImFont* body_font, AlboriaNotices** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      title_font_(title_font),
      body_font_(body_font),
      owner_(owner) {}

bool AlboriaNotices::NeedsContinuousPaint() const {
  return shown_.has_value() || !queue_.empty();
}

bool AlboriaNotices::AcceptShown() {
  if (!shown_ || shown_->leaving || shown_->notice.kind != Notice::Kind::kInvitation || accepting_) return false;
  auto accepting = std::make_shared<Accepting>();
  accepting->busy = true;
  accepting_ = accepting;
  input::SetNoticeAccepting(false);
  alboria_sounds::Play(alboria_sounds::Sound::kAccept);
  // The notice stays until the SDK reports, then for a moment with how it went.
  shown_->seconds = FLT_MAX;
  alboria::AcceptInvitation(shown_->notice.id, [accepting](const std::string& error) {
    std::lock_guard lock(accepting->mutex);
    accepting->busy = false;
    accepting->done = true;
    accepting->error = error;
    if (!error.empty()) alboria_sounds::Play(alboria_sounds::Sound::kError);
  });
  return true;
}

AlboriaNotices::Page AlboriaNotices::OverlayPage() const {
  return shown_ && shown_->notice.kind == Notice::Kind::kInvitation ? Page::kInvitations : Page::kFriends;
}

void AlboriaNotices::Dismiss() {
  if (shown_ && !accepting_) Leave(Clock::now());
}

void AlboriaNotices::Leave(Clock::time_point now) {
  if (!shown_ || shown_->leaving) return;
  shown_->leaving = now;
  input::SetNoticeAccepting(false);
}

void AlboriaNotices::WatchPads() {
  if (!assignment_ || !input_) return;
  for (const Slot& slot : assignment_->Slots()) {
    if (slot.keyboard) continue;
    rex::input::X_INPUT_STATE state = {};
    // 0 is X_ERROR_SUCCESS.
    if (input_->GetDeviceState(slot.device.id, &state) != 0) continue;
    const uint16_t buttons = state.gamepad.buttons;
    auto found = previous_buttons_.find(slot.device.id);
    if (found != previous_buttons_.end() && (buttons & ~found->second)) last_pad_ = slot.device.id;
    previous_buttons_[slot.device.id] = buttons;
  }
}

const rex::input::DeviceInfo* AlboriaNotices::HintPad(const std::vector<Slot>& slots) const {
  // The game's own idea of what player 1 plays with: the device they last pressed something on.
  if (!input_ || input_->ActiveKind(0) == rex::input::ActiveDeviceKind::kKeyboardMouse) return nullptr;
  const rex::input::DeviceInfo* first = nullptr;
  for (const Slot& slot : slots) {
    if (slot.keyboard || slot.player != 0) continue;
    if (last_pad_ && slot.device.id == *last_pad_) return &slot.device;
    if (!first) first = &slot.device;
  }
  return first;
}

void AlboriaNotices::OnDraw(ImGuiIO& io) {
  for (auto& notice : alboria::TakeNotices()) queue_.push_back(std::move(notice));
  const Clock::time_point now = Clock::now();

  // No notice over the overlay: the one on screen goes, the others wait for it to close.
  if (alboria::OverlayShown() && shown_ && !accepting_) Leave(now);

  if (!shown_) {
    if (queue_.empty() || alboria::OverlayShown() || now < gap_until_) return;
    Notice notice = std::move(queue_.front());
    queue_.pop_front();
    // An invitation answered meanwhile (in the overlay or the launcher) is not shown.
    if (notice.kind == Notice::Kind::kInvitation) {
      const auto open = alboria::Invitations();
      const bool still_open =
          std::any_of(open.begin(), open.end(), [&](const alboria::Invitation& i) { return i.id == notice.id; });
      if (!still_open) return;
    }
    const bool service = notice.kind == Notice::Kind::kServiceNotice || notice.kind == Notice::Kind::kServiceWarning;
    const float seconds = notice.kind == Notice::Kind::kInvitation ? kInvitationSeconds : service ? kServiceSeconds : kOtherSeconds;
    alboria_sounds::Play(notice.kind == Notice::Kind::kInvitation ? alboria_sounds::Sound::kInvitation
                                                                   : alboria_sounds::Sound::kNotice);
    shown_ = Shown{std::move(notice), now, seconds, std::nullopt};
    previous_buttons_.clear();
    input::SetNoticeAccepting(shown_->notice.kind == Notice::Kind::kInvitation);
  }

  // How an accept went: the notice says so for a moment, then leaves.
  std::string accept_text;
  bool accept_failed = false;
  const Texts texts = LocalizedTexts(language_);
  if (accepting_) {
    std::lock_guard lock(accepting_->mutex);
    if (accepting_->busy) {
      accept_text = texts.joining;
    } else if (accepting_->done) {
      accept_failed = !accepting_->error.empty();
      accept_text = accept_failed ? AlboriaErrorText(language_, accepting_->error) : texts.joining;
      if (shown_->seconds == FLT_MAX) {
        shown_->seconds = Seconds(now - shown_->since) + (accept_failed ? kFailedSeconds : kAcceptedSeconds);
      }
    }
  }

  const float t = Seconds(now - shown_->since);
  if (!shown_->leaving && t >= shown_->seconds) Leave(now);

  // Where the animation is: the planet's size, how far the panel is unfolded, the text's opacity.
  float grow = EaseOutBack(Clamp01(t / kGrowSeconds));
  float unfold = EaseOutCubic(Clamp01((t - kUnfoldStart) / kUnfoldSeconds));
  float text = Clamp01((t - kTextStart) / kTextSeconds);
  if (shown_->leaving) {
    const float lt = Seconds(now - *shown_->leaving);
    text = std::min(text, 1.0f - Clamp01(lt / 0.12f));
    unfold = std::min(unfold, 1.0f - EaseOutCubic(Clamp01(lt / kFoldSeconds)));
    const float shrink = Clamp01((lt - kShrinkStart) / kShrinkSeconds);
    grow = std::min(grow, 1.0f - shrink * shrink);
    if (lt >= kShrinkStart + kShrinkSeconds) {
      shown_.reset();
      accepting_.reset();
      gap_until_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kGapSeconds));
      return;
    }
  }

  WatchPads();
  const std::vector<Slot> slots = assignment_ ? assignment_->Slots() : std::vector<Slot>();
  const rex::input::DeviceInfo* pad = HintPad(slots);
  const PadLook look = pad ? LookOf(*pad) : PadLook::kGeneric;

  // At the bottom middle of the 16:9 area the game image takes in the window.
  const float game_height = std::min(io.DisplaySize.y, io.DisplaySize.x * 9.0f / 16.0f);
  const float game_bottom = (io.DisplaySize.y + game_height) * 0.5f;
  const float unit = game_height / 720.0f;
  ImFont* title_font = title_font_ ? title_font_ : ImGui::GetFont();
  ImFont* body_font = body_font_ ? body_font_ : ImGui::GetFont();
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  const Notice& notice = shown_->notice;
  const bool invitation = notice.kind == Notice::Kind::kInvitation;
  // A join that was not made (its party did not fit) says why, under Alboria's name.
  const bool refused = notice.kind == Notice::Kind::kJoinRefused;
  // A map the host chose that a player of its lobby lacks says so, under the match's label.
  const bool map_not_shared = notice.kind == Notice::Kind::kMapNotShared;
  const char* label = invitation ? texts.invitation
                      : notice.kind == Notice::Kind::kFriendRequest ? texts.friend_request
                      : notice.kind == Notice::Kind::kHostLeft      ? texts.match
                      : map_not_shared                              ? texts.match
                      : notice.kind == Notice::Kind::kFriendOnline  ? texts.friend_online
                      : refused                                     ? texts.alboria
                                                                     : texts.in_your_room;
  // A match that ended as its host left says only that: who hosted is never shown
  // (alboria-notes:alboria-server/online-rules.md).
  const bool match_ended = notice.kind == Notice::Kind::kHostLeft;
  std::string name = match_ended      ? std::string(texts.host_left)
                           : map_not_shared ? std::string(texts.map_not_shared)
                           : refused        ? std::string(AlboriaErrorText(language_, notice.id))
                                            : notice.from_name;
  std::string detail = invitation ? texts.invites
                       : notice.kind == Notice::Kind::kFriendRequest   ? texts.wants_friend
                       : notice.kind == Notice::Kind::kFriendOnline    ? texts.came_online
                       : match_ended || refused || map_not_shared      ? std::string()
                                                                        : texts.joined;
  if (!accept_text.empty()) detail = accept_text;
  const char* help = notice.kind == Notice::Kind::kFriendRequest ? texts.answer_in_launcher : nullptr;
  // Alboria's service notices: a notice's text under who it is for, or what is coming and when, with
  // what it means on the line below (the launcher's own texts).
  std::string service_help;
  if (notice.kind == Notice::Kind::kServiceNotice || notice.kind == Notice::Kind::kServiceWarning) {
    const ServiceTexts service = LocalizedServiceTexts(language_);
    const bool game = notice.from_name == "game";
    label = "ALBORIA";
    if (notice.kind == Notice::Kind::kServiceNotice) {
      name = Fill(game ? service.for_game : service.for_everyone, "game", kGameName);
      detail = Shortened(notice.id, 110);
    } else {
      const std::string& action = notice.id;
      const char* what = action == "restart"      ? service.warn_restart
                         : action == "game_off"    ? service.warn_game_off
                         : action == "close_rooms" ? service.warn_close_rooms
                         : game                    ? service.warn_maintenance_game
                                                   : service.warn_maintenance_all;
      const char* then = action == "restart"      ? service.then_restart
                         : action == "game_off"    ? service.then_game_off
                         : action == "close_rooms" ? service.then_close_rooms
                                                   : service.then_maintenance;
      name = Fill(what, "game", kGameName);
      detail = notice.from_id <= 1 ? std::string(service.in_a_minute)
                                   : Fill(service.in_minutes, "minutes", std::to_string(notice.from_id));
      service_help = then;
      help = service_help.c_str();
    }
  }

  // The sizes of what the panel holds, to fit it.
  const float label_size = 13.0f * unit;
  const float name_size = 21.0f * unit;
  const float detail_size = 15.0f * unit;
  const float glyph = 20.0f * unit;
  const bool hints = invitation && !accepting_;
  const std::string key = OverlayKey();
  // The hints: "<Back>+<RT> Accept   <Back>+<RB> Alboria", or the keys. Drawn by `draw_hints` at
  // `x`; with an alpha of 0 it only measures.
  auto draw_hints = [&](float x, float middle, int alpha) {
    const float start = x;
    auto chord = [&](PadControl first, PadControl second, const char* first_key, const char* second_key,
                     const char* what) {
      if (pad) {
        const float w1 = AlboriaGlyph(draw_list, title_font, ImVec2(0, 0), glyph, look, first, 0);
        x += AlboriaGlyph(draw_list, title_font, ImVec2(x + w1 * 0.5f, middle), glyph, look, first, alpha);
        x += 2.0f * unit;
        AddTextMiddleLeft(draw_list, body_font, detail_size, ImVec2(x, middle), Ink(alpha), "+");
        x += body_font->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, "+").x + 2.0f * unit;
        const float w2 = AlboriaGlyph(draw_list, title_font, ImVec2(0, 0), glyph, look, second, 0);
        x += AlboriaGlyph(draw_list, title_font, ImVec2(x + w2 * 0.5f, middle), glyph, look, second, alpha);
      } else {
        if (first_key) {
          x += AlboriaKey(draw_list, title_font, x, middle, glyph, first_key, alpha) + 2.0f * unit;
          AddTextMiddleLeft(draw_list, body_font, detail_size, ImVec2(x, middle), Ink(alpha), "+");
          x += body_font->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, "+").x + 2.0f * unit;
        }
        x += AlboriaKey(draw_list, title_font, x, middle, glyph, second_key, alpha);
      }
      x += 5.0f * unit;
      AddTextMiddleLeft(draw_list, body_font, detail_size, ImVec2(x, middle), Ink(alpha), what);
      x += body_font->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, what).x + kHintGap * unit;
    };
    chord(PadControl::kBack, PadControl::kRightTrigger, "Shift", key.c_str(), texts.accept);
    chord(PadControl::kBack, PadControl::kRightShoulder, nullptr, key.c_str(), texts.alboria);
    return x - start - kHintGap * unit;
  };
  const float hints_width = hints ? draw_hints(0.0f, 0.0f, 0) : 0.0f;
  const float name_width = title_font->CalcTextSizeA(name_size, FLT_MAX, 0.0f, name.c_str()).x;
  const float detail_width = body_font->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, detail.c_str()).x;
  const float help_width = help ? body_font->CalcTextSizeA(13.0f * unit, FLT_MAX, 0.0f, help).x : 0.0f;
  const float badge = 40.0f * unit;
  // From the badge's middle to where the text starts.
  const float text_offset = badge + 12.0f * unit;
  const float content = std::max({name_width, detail_width, help_width, 150.0f * unit});
  // From the badge's left edge to the panel's right end.
  const float full_width =
      std::min(badge + text_offset + content + (hints ? hints_width + 20.0f * unit : 0.0f) + 20.0f * unit,
               game_height * 16.0f / 9.0f - 80.0f * unit);
  // Room for the text, a help line's too.
  const float height = (help ? 104.0f : 80.0f) * unit;

  // The panel and the planet's badge on its left end, the whole centred when unfolded.
  const float center_x = io.DisplaySize.x * 0.5f - full_width * 0.5f + badge;
  const float middle = game_bottom - 60.0f * unit - height * 0.5f;
  const float width = (full_width - badge) * unfold;
  const int panel_alpha = static_cast<int>(255.0f * Clamp01(unfold * 3.0f));
  if (width > 1.0f) {
    // A beam of pearl glass in Alboria's carved golden frame; its left end lies under the medallion.
    const ImVec2 min(center_x, middle - height * 0.5f);
    const ImVec2 max(center_x + width, middle + height * 0.5f);
    DrawCarvedPanel(draw_list, min, max, height * 0.5f, unit, panel_alpha);

    // The text, clipped to the panel while it unfolds.
    const int a = static_cast<int>(255.0f * text);
    if (a > 0) {
      draw_list->PushClipRect(min, max, true);
      const float left = center_x + text_offset;
      float y = middle - height * 0.5f + 11.0f * unit;
      draw_list->AddText(body_font, label_size, ImVec2(left, y), GoldText(a), label);
      y += label_size + 3.0f * unit;
      draw_list->AddText(title_font, name_size, ImVec2(left, y), Ink(a), name.c_str());
      y += name_size + 2.0f * unit;
      draw_list->AddText(body_font, detail_size, ImVec2(left, y), accept_failed ? Crimson(a) : NightSoft(a),
                         detail.c_str());
      if (help) {
        y += detail_size + 3.0f * unit;
        draw_list->AddText(body_font, 13.0f * unit, ImVec2(left, y), NightSoft(a * 3 / 4), help);
      }
      if (hints) {
        // The hint pulses gently, so the eye finds it.
        const float pulse = 0.72f + 0.28f * (0.5f + 0.5f * std::cos(t * kPulseSpeed * 2.0f * 3.14159265f));
        draw_hints(max.x - 20.0f * unit - hints_width, middle, static_cast<int>(a * pulse));
      }
      draw_list->PopClipRect();
    }
  }

  // The badge: the logo's pearl medallion ringed in gold, and the planet with its moon on it.
  if (grow > 0.0f) {
    const ImVec2 center(center_x, middle);
    const int alpha = static_cast<int>(255.0f * Clamp01(grow * 2.0f));
    const float radius = badge * std::max(grow, 0.0f);
    draw_list->AddCircleFilled(ImVec2(center.x, center.y + 5.0f * unit), radius * 1.1f, IM_COL32(0, 0, 0, alpha / 4), 48);
    DrawMedallion(draw_list, center, radius, alpha);
    DrawAlboriaPlanet(draw_list, center, 1.5f * unit * grow, alpha, 1.0f + t * kMoonSpeed);
    // The time left, as a ring of golden light around the medallion that empties clockwise from
    // the top, with round ends; a faint track shows the whole ring.
    if (shown_->seconds != FLT_MAX && !shown_->leaving) {
      const float total = shown_->seconds - kTextStart;
      const float left = Clamp01(1.0f - (t - kTextStart) / total);
      const float ring = radius + 4.5f * unit;
      const float width = 3.0f * unit;
      draw_list->AddCircle(center, ring, Gold(alpha * 60 / 255), 64, width);
      if (left > 0.0f) {
        constexpr float kTop = -3.14159265f * 0.5f;
        const float end = kTop + 2.0f * 3.14159265f * left;
        const int segments = std::max(4, static_cast<int>(64.0f * left));
        // Opaque, so its round ends (discs over the arc) do not show where they overlap it.
        const ImU32 color = IM_COL32(255, 212, 94, alpha);
        draw_list->PathArcTo(center, ring, kTop, end, segments);
        draw_list->PathStroke(color, 0, width);
        draw_list->AddCircleFilled(ImVec2(center.x + ring * std::cos(kTop), center.y + ring * std::sin(kTop)), width * 0.5f, color, 12);
        draw_list->AddCircleFilled(ImVec2(center.x + ring * std::cos(end), center.y + ring * std::sin(end)), width * 0.5f, color, 12);
      }
    }
  }
}

void AlboriaNotices::OnClose() {
  input::SetNoticeAccepting(false);
  if (owner_) {
    *owner_ = nullptr;
    owner_ = nullptr;
  }
}

}  // namespace azura
