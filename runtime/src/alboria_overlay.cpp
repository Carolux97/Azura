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

#include "input/overlay_input.h"
#include "input/player_assignment.h"
#include "online_room_overlay.h"

namespace azura {

namespace {

using namespace coop_ui;
using rex::input::DeviceId;
using rex::input::PadControl;
using Slot = input::PlayerAssignment::Slot;
namespace alboria = rex::system::xam::alboria;

// How long the overlay takes to fade in.
constexpr float kFadeSeconds = 0.15f;
// How far the left stick goes before it counts as a press in a direction.
constexpr int kStickPress = 20000;
// How long the result of an action stays.
constexpr float kMessageSeconds = 4.0f;
// The game's title ID, for "playing" when a friend plays it.
constexpr uint32_t kGearsOfWar3 = 0x4D5308AB;

// Alboria's palette (launcher/src/styles.css, from alboria-web).
constexpr ImU32 Navy(int alpha) { return IM_COL32(30, 42, 79, alpha); }
constexpr ImU32 NightSoft(int alpha) { return IM_COL32(74, 90, 133, alpha); }
constexpr ImU32 Sky(int alpha) { return IM_COL32(159, 211, 255, alpha); }
constexpr ImU32 SkyDeep(int alpha) { return IM_COL32(90, 169, 240, alpha); }
constexpr ImU32 SkyLine(int alpha) { return IM_COL32(184, 218, 248, alpha); }
constexpr ImU32 Paper(int alpha) { return IM_COL32(227, 242, 255, alpha); }
constexpr ImU32 PaperDeep(int alpha) { return IM_COL32(201, 229, 255, alpha); }
constexpr ImU32 Sun(int alpha) { return IM_COL32(255, 201, 60, alpha); }
constexpr ImU32 SunShadow(int alpha) { return IM_COL32(217, 162, 31, alpha); }
constexpr ImU32 Crimson(int alpha) { return IM_COL32(179, 32, 42, alpha); }
constexpr ImU32 White(int alpha) { return IM_COL32(255, 255, 255, alpha); }
constexpr ImU32 Online(int alpha) { return IM_COL32(110, 180, 80, alpha); }

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

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 5:  // Spanish
      return {"Alboria", "Amigos", "Invitaciones", "Sala", "Unirse", "Invitar", "Ahora no", "Copiar",
              "Unirse por c\xC3\xB3""digo", "Cerrar", "Cambiar de p\xC3\xA1gina", "Jugando a %s \xC2\xB7 sala abierta",
              "Jugando a %s \xC2\xB7 solo por invitaci\xC3\xB3n", "Jugando", "En l\xC3\xAD""nea", "Desconectado",
              "Visto hace %s", "%d min", "%d h", "%d d\xC3\xAD""as", "Todav\xC3\xAD""a no tienes amigos en Alboria. A\xC3\xB1\xC3\xA1""delos desde el launcher.",
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
              "%d Min.", "%d Std.", "%d Tagen", "Du hast noch keine Freunde auf Alboria. F\xC3\xBC""ge sie im Launcher hinzu.",
              "Du hast keine Einladungen.", "%s l\xC3\xA4""dt dich in seinen Raum ein", "Du bist in keinem Raum.",
              "Code deines Raums", "Code kopiert", "Raum wird betreten\xE2\x80\xA6", "Einladung wird gesendet\xE2\x80\xA6",
              "Einladung an %s gesendet", "Einladung abgelehnt", "Alboria ist gerade nicht erreichbar",
              "Pr\xC3\xBC""fe deine Verbindung und ob dein Konto im Launcher verkn\xC3\xBCpft ist.",
              "Das ging nicht: %s"};
    case 4:  // French
      return {"Alboria", "Amis", "Invitations", "Salon", "Rejoindre", "Inviter", "Pas maintenant", "Copier",
              "Rejoindre par code", "Fermer", "Changer de page", "Joue \xC3\xA0 %s \xC2\xB7 salon ouvert",
              "Joue \xC3\xA0 %s \xC2\xB7 sur invitation", "Joue", "En ligne", "Hors ligne", "Vu il y a %s", "%d min",
              "%d h", "%d jours", "Tu n'as pas encore d'amis sur Alboria. Ajoute-les depuis le launcher.",
              "Tu n'as aucune invitation.", "%s t'invite dans son salon", "Tu n'es dans aucun salon.",
              "Code de ton salon", "Code copi\xC3\xA9", "Connexion au salon\xE2\x80\xA6", "Envoi de l'invitation\xE2\x80\xA6",
              "Invitation envoy\xC3\xA9""e \xC3\xA0 %s", "Invitation refus\xC3\xA9""e", "Alboria n'est pas disponible pour le moment",
              "V\xC3\xA9rifie ta connexion et que ton compte est li\xC3\xA9 dans le launcher.", "Impossible : %s"};
    case 6:  // Italian
      return {"Alboria", "Amici", "Inviti", "Stanza", "Unisciti", "Invita", "Non ora", "Copia",
              "Unisciti con codice", "Chiudi", "Cambia pagina", "Gioca a %s \xC2\xB7 stanza aperta",
              "Gioca a %s \xC2\xB7 solo su invito", "Sta giocando", "Online", "Offline", "Visto %s fa", "%d min",
              "%d h", "%d giorni", "Non hai ancora amici su Alboria. Aggiungili dal launcher.", "Non hai inviti.",
              "%s ti invita nella sua stanza", "Non sei in nessuna stanza.", "Codice della tua stanza",
              "Codice copiato", "Ingresso nella stanza\xE2\x80\xA6", "Invio dell'invito\xE2\x80\xA6", "Invito inviato a %s",
              "Invito rifiutato", "Alboria non \xC3\xA8 disponibile ora",
              "Controlla la connessione e che il tuo account sia collegato nel launcher.", "Non \xC3\xA8 stato possibile: %s"};
    case 9:  // Portuguese
      return {"Alboria", "Amigos", "Convites", "Sala", "Entrar", "Convidar", "Agora n\xC3\xA3o", "Copiar",
              "Entrar com c\xC3\xB3""digo", "Fechar", "Mudar de p\xC3\xA1gina", "Jogando %s \xC2\xB7 sala aberta",
              "Jogando %s \xC2\xB7 s\xC3\xB3 por convite", "Jogando", "Online", "Offline", "Visto h\xC3\xA1 %s", "%d min",
              "%d h", "%d dias", "Voc\xC3\xAA ainda n\xC3\xA3o tem amigos na Alboria. Adicione-os pelo launcher.",
              "Voc\xC3\xAA n\xC3\xA3o tem convites.", "%s te convida para a sala", "Voc\xC3\xAA n\xC3\xA3o est\xC3\xA1 em nenhuma sala.",
              "C\xC3\xB3""digo da sua sala", "C\xC3\xB3""digo copiado", "Entrando na sala\xE2\x80\xA6", "Enviando o convite\xE2\x80\xA6",
              "Convite enviado para %s", "Convite recusado", "A Alboria n\xC3\xA3o est\xC3\xA1 dispon\xC3\xADvel agora",
              "Verifique a conex\xC3\xA3o e se a sua conta est\xC3\xA1 vinculada no launcher.", "N\xC3\xA3o foi poss\xC3\xADvel: %s"};
    case 11:  // Polish
      return {"Alboria", "Znajomi", "Zaproszenia", "Pok\xC3\xB3j", "Do\xC5\x82\xC4\x85""cz", "Zapro\xC5\x9B", "Nie teraz",
              "Kopiuj", "Do\xC5\x82\xC4\x85""cz kodem", "Zamknij", "Zmie\xC5\x84 stron\xC4\x99", "Gra w %s \xC2\xB7 pok\xC3\xB3j otwarty",
              "Gra w %s \xC2\xB7 tylko na zaproszenie", "Gra", "Online", "Offline", "Widziany %s temu", "%d min",
              "%d godz.", "%d dni", "Nie masz jeszcze znajomych w Alborii. Dodaj ich w launcherze.",
              "Nie masz zaprosze\xC5\x84.", "%s zaprasza ci\xC4\x99 do pokoju", "Nie jeste\xC5\x9B w \xC5\xBC""adnym pokoju.",
              "Kod twojego pokoju", "Skopiowano kod", "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6", "Wysy\xC5\x82""anie zaproszenia\xE2\x80\xA6",
              "Wys\xC5\x82""ano zaproszenie do %s", "Odrzucono zaproszenie", "Alboria jest teraz niedost\xC4\x99pna",
              "Sprawd\xC5\xBA po\xC5\x82\xC4\x85""czenie i czy konto jest po\xC5\x82\xC4\x85""czone w launcherze.", "Nie uda\xC5\x82o si\xC4\x99: %s"};
    default:
      return {"Alboria", "Friends", "Invitations", "Room", "Join", "Invite", "Not now", "Copy", "Join by code",
              "Close", "Change page", "Playing %s \xC2\xB7 open room", "Playing %s \xC2\xB7 invite only", "Playing",
              "Online", "Offline", "Last seen %s ago", "%d min", "%d h", "%d days",
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

// A rounded pill button: the sun for the main action, white with a sky line for the others. Returns
// its width; `hit` says whether the mouse clicked it this frame.
float DrawPill(ImDrawList* draw_list, ImFont* font, float size, ImVec2 right_center, const char* label, bool primary,
               float unit, int alpha, bool* hit) {
  const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x + 28.0f * unit;
  const float height = size + 14.0f * unit;
  const ImVec2 min(right_center.x - width, right_center.y - height * 0.5f);
  const ImVec2 max(right_center.x, right_center.y + height * 0.5f);
  if (primary) {
    draw_list->AddRectFilled(ImVec2(min.x, min.y + 3.0f * unit), ImVec2(max.x, max.y + 3.0f * unit), SunShadow(alpha),
                             height * 0.5f);
    draw_list->AddRectFilled(min, max, Sun(alpha), height * 0.5f);
  } else {
    draw_list->AddRectFilled(min, max, White(alpha), height * 0.5f);
    draw_list->AddRect(min, max, SkyLine(alpha), height * 0.5f, 0, 2.0f * unit);
  }
  AddTextCentered(draw_list, font, size, ImVec2((min.x + max.x) * 0.5f, right_center.y), Navy(alpha), label);
  if (hit) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    *hit = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y &&
           mouse.y <= max.y;
  }
  return width;
}

// A controller's button in Alboria's colours: a navy disc for the face buttons, with the letters or
// the PlayStation shapes in white, and a navy key cap with the name for the others; centred on
// `center`, `height` tall. Returns its width.
float AlboriaGlyph(ImDrawList* draw_list, ImFont* font, ImVec2 center, float height, PadLook look, PadControl control,
                   int alpha) {
  const float r = height * 0.5f;
  const bool face = control == PadControl::kA || control == PadControl::kB || control == PadControl::kX ||
                    control == PadControl::kY;
  if (face) {
    draw_list->AddCircleFilled(center, r, Navy(alpha));
    const bool playstation = look == PadLook::kDualSense || look == PadLook::kDualShock4;
    const float s = r * 0.48f;
    const float t = height * 0.09f;
    if (playstation) {
      switch (control) {
        case PadControl::kA:
          draw_list->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), White(alpha), t);
          draw_list->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x + s, center.y - s), White(alpha), t);
          break;
        case PadControl::kB: draw_list->AddCircle(center, s, White(alpha), 0, t); break;
        case PadControl::kX:
          draw_list->AddRect(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), White(alpha), 0, 0, t);
          break;
        default:
          draw_list->AddTriangle(ImVec2(center.x, center.y - s * 1.1f), ImVec2(center.x + s * 1.05f, center.y + s * 0.75f),
                                 ImVec2(center.x - s * 1.05f, center.y + s * 0.75f), White(alpha), t);
          break;
      }
    } else {
      // Nintendo's letters sit swapped against Xbox's.
      const bool nintendo = look == PadLook::kSwitchPro;
      const char* letter = control == PadControl::kA ? (nintendo ? "B" : "A")
                           : control == PadControl::kB ? (nintendo ? "A" : "B")
                           : control == PadControl::kX ? (nintendo ? "Y" : "X")
                                                       : (nintendo ? "X" : "Y");
      AddTextCentered(draw_list, font, height * 0.72f, center, White(alpha), letter);
    }
    return height;
  }
  const char* name = ControlName(control, look);
  const float width = std::max(height * 1.6f, font->CalcTextSizeA(height * 0.62f, FLT_MAX, 0.0f, name).x + height * 0.6f);
  draw_list->AddRectFilled(ImVec2(center.x - width * 0.5f, center.y - r), ImVec2(center.x + width * 0.5f, center.y + r),
                           Navy(alpha), r * 0.6f);
  AddTextCentered(draw_list, font, height * 0.62f, center, White(alpha), name);
  return width;
}

// Alboria's planet as the launcher draws it (its alboria-chip, a 40 x 40 drawing scaled by `scale`):
// a sun-light planet with a navy line, and a sky ring tilted by 18 degrees whose back half passes
// behind the planet and its front half in front.
void DrawAlboriaPlanet(ImDrawList* draw_list, ImVec2 center, float scale, int alpha) {
  constexpr float kTilt = -18.0f * 3.14159265f / 180.0f;
  const ImVec2 ring(19.0f * scale, 6.0f * scale);
  const float line = 3.0f * scale;
  draw_list->AddEllipse(center, ring, SkyDeep(alpha), kTilt, 48, line);
  draw_list->AddCircleFilled(center, 12.0f * scale, IM_COL32(255, 224, 122, alpha));
  draw_list->AddCircle(center, 12.0f * scale, Navy(alpha), 48, line);
  // The front half: the lower half of the ring before it is tilted.
  constexpr int kSteps = 24;
  for (int i = 0; i <= kSteps; ++i) {
    const float t = 3.14159265f * i / kSteps;
    const float x = ring.x * std::cos(t);
    const float y = ring.y * std::sin(t);
    draw_list->PathLineTo(ImVec2(center.x + x * std::cos(kTilt) - y * std::sin(kTilt),
                                 center.y + x * std::sin(kTilt) + y * std::cos(kTilt)));
  }
  draw_list->PathStroke(SkyDeep(alpha), 0, line);
}

// A keyboard key in Alboria's colours: a white cap with a sky line and its name in navy, its left
// edge at `left` and its middle at `middle`, `height` tall. Returns its width.
float AlboriaKey(ImDrawList* draw_list, ImFont* font, float left, float middle, float height, const char* name,
                 int alpha) {
  const float width = std::max(height * 1.4f, font->CalcTextSizeA(height * 0.6f, FLT_MAX, 0.0f, name).x + height * 0.7f);
  const ImVec2 min(left, middle - height * 0.5f);
  const ImVec2 max(left + width, middle + height * 0.5f);
  draw_list->AddRectFilled(min, max, White(alpha), height * 0.28f);
  draw_list->AddRect(min, max, SkyDeep(alpha), height * 0.28f, 0, height * 0.08f);
  AddTextCentered(draw_list, font, height * 0.6f, ImVec2((min.x + max.x) * 0.5f, middle), Navy(alpha), name);
  return width;
}

}  // namespace

AlboriaOverlay::AlboriaOverlay(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                               input::PlayerAssignment* assignment, uint32_t language, Page page,
                               ImFont* title_font, ImFont* body_font, std::function<void()> join_by_code,
                               AlboriaOverlay** owner)
    : ImGuiDialog(drawer),
      input_(input),
      assignment_(assignment),
      language_(language),
      page_(page),
      title_font_(title_font),
      body_font_(body_font),
      join_by_code_(std::move(join_by_code)),
      owner_(owner),
      opened_(Clock::now()),
      last_input_(input) {
  input::SetOverlayBlocking(true);
  alboria::SetOverlayShown(true);
  alboria::RefreshFriends();
}

void AlboriaOverlay::SetMessage(std::string text, bool error) {
  std::lock_guard lock(result_->mutex);
  result_->message = std::move(text);
  result_->error = error;
  result_->since = Clock::now();
}

void AlboriaOverlay::Start(const std::string& pending,
                           const std::function<void(alboria::ActionDone)>& action, const std::string& done_text,
                           bool close_when_done) {
  {
    std::lock_guard lock(result_->mutex);
    if (result_->busy) return;
    result_->busy = true;
    result_->message = pending;
    result_->error = false;
    result_->since = Clock::now();
  }
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
            texts.declined, false);
    }
  } else if (!secondary) {
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
      auto found = previous_buttons_.find(slot.device.id);
      const bool seen = found != previous_buttons_.end();
      const uint16_t pressed = seen ? static_cast<uint16_t>(buttons & ~found->second) : 0;
      previous_buttons_[slot.device.id] = buttons;
      if (pressed) last_input_.NotePad(slot.device.id);
      if (pressed & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) actions.page = -1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) actions.page = 1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) actions.move = -1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) actions.move = 1;
      if (pressed & rex::input::X_INPUT_GAMEPAD_A) actions.primary = true;
      if (pressed & rex::input::X_INPUT_GAMEPAD_X) actions.secondary = true;
      if (pressed & rex::input::X_INPUT_GAMEPAD_Y) actions.copy = true;
      if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) actions.close = true;
    }
  }
  last_input_.NoteKeyboard();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) actions.close = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) actions.page = ImGui::GetIO().KeyShift ? -1 : 1;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) actions.page = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) actions.page = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) actions.move = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) actions.move = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    actions.primary = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_I, false)) actions.secondary = true;
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_C, false)) actions.copy = true;
  return actions;
}

void AlboriaOverlay::AddType(char c) {
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
    case KeyAction::kShift: add_shift_ = !add_shift_; break;
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
  const std::string failed = LocalizedTexts(language_).failed;
  alboria::SendFriendRequest(full, [result = result_, full, texts, failed, language = language_](const std::string& error) {
    std::lock_guard lock(result->mutex);
    result->busy = false;
    result->since = Clock::now();
    result->error = !error.empty() && error != "friends";
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
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) key_row_ = std::max(0, key_row_ - 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) key_row_ = std::min(kKeyRowCount - 1, key_row_ + 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) key_column_--;
      if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) key_column_++;
      key_column_ = std::clamp(key_column_, 0, KeyRowLength(key_row_) - 1);
      if (pressed & rex::input::X_INPUT_GAMEPAD_A) PressKey();
      if (pressed & rex::input::X_INPUT_GAMEPAD_X) AddErase();
      if (pressed & rex::input::X_INPUT_GAMEPAD_Y) add_shift_ = !add_shift_;
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
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) add_in_tag_ = !add_in_tag_;
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) AddErase();
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) SendRequest();
  return back;
}

void AlboriaOverlay::OnDraw(ImGuiIO& io) {
  // Closing waits for what closed it to be released (coop_ui::CloseInputHeld).
  if (closing_) {
    std::vector<DeviceId> pads;
    if (assignment_) {
      for (const Slot& slot : assignment_->Slots()) {
        if (!slot.keyboard) pads.push_back(slot.device.id);
      }
    }
    if (!CloseInputHeld(input_, pads)) Close();
    return;
  }

  const Texts texts = LocalizedTexts(language_);
  const bool signed_in = alboria::LiveSignedIn();
  const auto friends = SortedFriends();
  const auto invitations = alboria::Invitations();
  const alboria::Status status = alboria::GetStatus();
  const bool in_room = status.state == alboria::State::kConnected && !status.code.empty();
  const int pages = 3;
  auto rows_on_page = [&]() -> int {
    if (page_ == Page::kFriends) return static_cast<int>(friends.size()) + 1;
    if (page_ == Page::kInvitations) return static_cast<int>(invitations.size());
    return 1;
  };

  const Actions actions = adding_ ? Actions{} : ReadInput();
  if (adding_ && AddFriendInput()) adding_ = false;
  {
    std::lock_guard lock(result_->mutex);
    if (result_->close_after) {
      result_->close_after = false;
      closing_ = true;
    }
  }
  if (actions.close) closing_ = true;
  if (actions.page) {
    page_ = static_cast<Page>((static_cast<int>(page_) + actions.page + pages) % pages);
    focus_ = 0;
  }
  if (actions.move) focus_ += actions.move;
  focus_ = std::clamp(focus_, 0, std::max(0, rows_on_page() - 1));
  if (actions.primary) Act(false);
  if (actions.secondary) Act(true);
  if (actions.copy && in_room) {
    ImGui::SetClipboardText(alboria::DisplayCode(status.code).c_str());
    SetMessage(texts.copied, false);
  }

  const Clock::time_point now = Clock::now();
  const float fade = std::min(1.0f, std::chrono::duration<float>(now - opened_).count() / kFadeSeconds);
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
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  auto inside = [&](ImVec2 min, ImVec2 max) {
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
  };

  // The game dimmed behind, with a touch of Alboria's night.
  draw_list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(14, 20, 40, a(170)));

  const float panel_width = 720.0f * unit;
  const float panel_height = 500.0f * unit;
  const float margin = 32.0f * unit;
  const ImVec2 panel_min(game_left + (game_width - panel_width) * 0.5f, game_top + (game_height - panel_height) * 0.5f);
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  const float radius = 22.0f * unit;
  draw_list->AddRectFilled(ImVec2(panel_min.x, panel_min.y + 6.0f * unit), ImVec2(panel_max.x, panel_max.y + 6.0f * unit),
                           IM_COL32(10, 16, 34, a(90)), radius);
  draw_list->AddRectFilledMultiColor(ImVec2(panel_min.x, panel_min.y + radius), ImVec2(panel_max.x, panel_max.y - radius),
                                     Paper(a(250)), Paper(a(250)), PaperDeep(a(250)), PaperDeep(a(250)));
  draw_list->AddRectFilled(panel_min, ImVec2(panel_max.x, panel_min.y + radius * 2.0f), Paper(a(250)), radius,
                           ImDrawFlags_RoundCornersTop);
  draw_list->AddRectFilled(ImVec2(panel_min.x, panel_max.y - radius * 2.0f), panel_max, PaperDeep(a(250)), radius,
                           ImDrawFlags_RoundCornersBottom);
  draw_list->AddRect(panel_min, panel_max, SkyLine(a(255)), radius, 0, 2.0f * unit);

  // The title, Alboria's planet beside it (the launcher's, from its 40 x 40 drawing), and the account
  // on the right.
  const float logo = 36.0f * unit / 40.0f;
  const ImVec2 planet(panel_min.x + margin + 20.0f * logo, panel_min.y + 41.0f * unit);
  DrawAlboriaPlanet(draw_list, planet, logo, a(255));
  draw_list->AddText(title_font, 30.0f * unit, ImVec2(panel_min.x + margin + 46.0f * logo, panel_min.y + 23.0f * unit),
                     Navy(a(255)), texts.title);
  const alboria::Identity identity = alboria::GetIdentity();
  if (identity.known) {
    const std::string me = identity.gamertag + "#" + std::to_string(identity.tag);
    const float width = body_font->CalcTextSizeA(16.0f * unit, FLT_MAX, 0.0f, me.c_str()).x;
    draw_list->AddText(body_font, 16.0f * unit, ImVec2(panel_max.x - margin - width, panel_min.y + 32.0f * unit),
                       NightSoft(a(255)), me.c_str());
  }

  // The tabs.
  const char* tab_names[] = {texts.friends, texts.invitations, texts.room};
  float tab_x = panel_min.x + margin;
  const float tab_y = panel_min.y + 74.0f * unit;
  for (int i = 0; i < pages; ++i) {
    std::string label = tab_names[i];
    if (i == 1 && !invitations.empty()) label += " (" + std::to_string(invitations.size()) + ")";
    const float width = title_font->CalcTextSizeA(18.0f * unit, FLT_MAX, 0.0f, label.c_str()).x + 30.0f * unit;
    const ImVec2 min(tab_x, tab_y);
    const ImVec2 max(tab_x + width, tab_y + 34.0f * unit);
    const bool current = static_cast<int>(page_) == i;
    if (current) draw_list->AddRectFilled(min, max, Navy(a(255)), 17.0f * unit);
    else draw_list->AddRect(min, max, SkyLine(a(255)), 17.0f * unit, 0, 2.0f * unit);
    AddTextCentered(draw_list, title_font, 18.0f * unit, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                    current ? White(a(255)) : Navy(a(255)), label.c_str());
    if (clicked && inside(min, max) && !current) {
      page_ = static_cast<Page>(i);
      focus_ = 0;
    }
    tab_x += width + 10.0f * unit;
  }

  // The page.
  const float list_top = tab_y + 52.0f * unit;
  const float list_bottom = panel_max.y - 70.0f * unit;
  const float row_height = 58.0f * unit;
  const float row_gap = 8.0f * unit;
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
  // A row of the list, with a coloured dot, its name and what it says; the focused one outlined.
  // Returns the row's rectangle's vertical middle.
  const int visible_rows = std::max(1, static_cast<int>((list_bottom - list_top + row_gap) / (row_height + row_gap)));
  const int first_row = std::clamp(focus_ - visible_rows + 1, 0, std::max(0, rows_on_page() - visible_rows));
  auto row = [&](int index, ImU32 dot, const std::string& name, const std::string& detail) -> float {
    const float top = list_top + (index - first_row) * (row_height + row_gap);
    const ImVec2 min(list_left, top);
    const ImVec2 max(list_right, top + row_height);
    const bool focused = index == focus_;
    draw_list->AddRectFilled(min, max, focused ? White(a(255)) : White(a(170)), 14.0f * unit);
    draw_list->AddRect(min, max, focused ? SkyDeep(a(255)) : SkyLine(a(255)), 14.0f * unit, 0,
                       (focused ? 2.5f : 1.5f) * unit);
    const float middle = top + row_height * 0.5f;
    draw_list->AddCircleFilled(ImVec2(min.x + 22.0f * unit, middle), 6.0f * unit, dot);
    draw_list->AddText(title_font, 19.0f * unit, ImVec2(min.x + 40.0f * unit, top + 8.0f * unit), Navy(a(255)),
                       name.c_str());
    draw_list->AddText(body_font, 14.0f * unit, ImVec2(min.x + 40.0f * unit, top + 32.0f * unit), NightSoft(a(255)),
                       Fit(body_font, 14.0f * unit, detail, (max.x - min.x) * 0.55f).c_str());
    if (clicked && inside(min, max)) focus_ = index;
    return middle;
  };
  bool busy;
  {
    std::lock_guard lock(result_->mutex);
    busy = result_->busy;
  }

  if (!signed_in) {
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
      draw_list->AddRectFilled(min, max, White(a(255)), 12.0f * unit);
      draw_list->AddRect(min, max, focused ? SkyDeep(a(255)) : SkyLine(a(255)), 12.0f * unit, 0, (focused ? 2.5f : 2.0f) * unit);
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
    const float send_width = title_font->CalcTextSizeA(16.0f * unit, FLT_MAX, 0.0f, add.send).x + 28.0f * unit;
    DrawPill(draw_list, title_font, 16.0f * unit,
             ImVec2(std::min(list_right, list_left + name_width + hash_width + tag_width + 18.0f * unit + send_width),
                    field_top + field_height * 0.5f),
             add.send, true, unit, a(255), &hit);
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
      const float middle = row(0, Sun(a(255)), LocalizedAddTexts(language_).add_friend, LocalizedAddTexts(language_).help);
      draw_list->AddText(title_font, 22.0f * unit, ImVec2(list_left + 15.5f * unit, middle - 13.0f * unit), Navy(a(255)), "+");
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
      const ImU32 dot = f.playing ? Sun(a(255)) : f.online ? Online(a(255)) : SkyLine(a(255));
      const float middle = row(i, dot, f.name, Activity(texts, f));
      // A favourite: a sun ring around its dot (the fonts have no star).
      if (f.favourite) {
        draw_list->AddCircle(ImVec2(list_left + 22.0f * unit, middle), 10.0f * unit, Sun(a(255)), 0, 2.0f * unit);
      }
      float right = list_right - 14.0f * unit;
      bool hit = false;
      if (f.joinable) {
        right -= DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.join, true, unit, a(255), &hit) +
                 10.0f * unit;
        if (hit && !busy) {
          focus_ = i;
          Act(false);
        }
      }
      if (in_room && f.online) {
        DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.invite, false, unit, a(255), &hit);
        if (hit && !busy) {
          focus_ = i;
          Act(true);
        }
      }
    }
  } else if (page_ == Page::kInvitations) {
    if (invitations.empty()) empty_text(texts.no_invitations);
    for (int i = first_row; i < static_cast<int>(invitations.size()) && i < first_row + visible_rows; ++i) {
      const float middle = row(i, Sun(a(255)), invitations[i].from_name, Format(texts.invites_you, invitations[i].from_name));
      float right = list_right - 14.0f * unit;
      bool hit = false;
      right -= DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.join, true, unit, a(255), &hit) +
               10.0f * unit;
      if (hit && !busy) {
        focus_ = i;
        Act(false);
      }
      DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(right, middle), texts.not_now, false, unit, a(255), &hit);
      if (hit && !busy) {
        focus_ = i;
        Act(true);
      }
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
      const float pill_width = title_font->CalcTextSizeA(16.0f * unit, FLT_MAX, 0.0f, texts.copy).x + 28.0f * unit;
      DrawPill(draw_list, title_font, 16.0f * unit, ImVec2(list_left + code_width + 24.0f * unit + pill_width,
                                                           list_top + 58.0f * unit),
               texts.copy, false, unit, a(255), &hit);
      if (hit) {
        ImGui::SetClipboardText(code.c_str());
        SetMessage(texts.copied, false);
      }
    } else {
      empty_text(texts.no_room);
    }
    bool hit = false;
    const float join_width = title_font->CalcTextSizeA(18.0f * unit, FLT_MAX, 0.0f, texts.join_by_code).x + 28.0f * unit;
    DrawPill(draw_list, title_font, 18.0f * unit, ImVec2(list_left + join_width, list_top + 130.0f * unit),
             texts.join_by_code, true, unit, a(255), &hit);
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

  // Button hints along the bottom, with the glyphs of the input in use.
  const float hints_y = panel_max.y - 36.0f * unit;
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
      hint(PadControl::kStart, nullptr, add.send);
    } else {
      hint(PadControl::kA, "Tab", "#");
      hint(PadControl::kA, "Enter", add.send);
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
  if (page_ == Page::kFriends && signed_in && in_room) hint(PadControl::kX, "I", texts.invite);
  if (page_ == Page::kInvitations && !invitations.empty()) hint(PadControl::kX, "I", texts.not_now);
  if (page_ == Page::kRoom && in_room) hint(PadControl::kY, pad ? nullptr : "Ctrl+C", texts.copy);
  hint(PadControl::kB, "Esc", texts.close);
}

void AlboriaOverlay::OnClose() {
  input::SetOverlayBlocking(false);
  alboria::SetOverlayShown(false);
  if (owner_) *owner_ = nullptr;
  if (open_join_by_code_ && join_by_code_) join_by_code_();
}

}  // namespace azura
