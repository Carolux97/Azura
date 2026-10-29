#include "online_room_overlay.h"

#include <algorithm>
#include <string>

#include <imgui.h>

#include <rex/system/xam/alboria.h>

#include "perf_overlay.h"

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
  // This installation is not linked to an Alboria account (docs/alboria-relay.md, section 3).
  const char* no_account;
  const char* other;
};

// Fonts carry Latin-1 only, so the languages written in other scripts use the English texts.
Texts LocalizedTexts(uint32_t language) {
  switch (language) {
    case 3:  // German
      return {"ONLINE-RAUM", "Raum wird erstellt\xE2\x80\xA6", "Raum wird betreten\xE2\x80\xA6", "ONLINE",
              "Keine Verbindung zu Alboria", "Kein Spiel hat diesen Code", "Das Spiel ist voll",
              "Der Raum wurde geschlossen", "Du wurdest von Alboria gesperrt",
              "Aktualisiere Azura, um online zu spielen", "Melde dich im Launcher bei Alboria an",
              "Alboria hat nicht geantwortet"};
    case 4:  // French
      return {"SALON EN LIGNE", "Cr\xC3\xA9""ation du salon\xE2\x80\xA6", "Connexion au salon\xE2\x80\xA6", "EN LIGNE",
              "Impossible de joindre Alboria", "Aucune partie n'a ce code", "La partie est pleine",
              "Le salon a \xC3\xA9t\xC3\xA9 ferm\xC3\xA9", "Tu as \xC3\xA9t\xC3\xA9 banni d'Alboria",
              "Mets \xC3\xA0 jour Azura pour jouer en ligne", "Connecte-toi \xC3\xA0 Alboria depuis le launcher",
              "Alboria n'a pas r\xC3\xA9pondu"};
    case 5:  // Spanish
      return {"SALA ONLINE", "Creando la sala\xE2\x80\xA6", "Entrando en la sala\xE2\x80\xA6", "ONLINE",
              "No se puede conectar con Alboria", "Ninguna partida tiene ese c\xC3\xB3""digo",
              "La partida est\xC3\xA1 llena", "La sala se ha cerrado", "Alboria te ha vetado",
              "Actualiza Azura para jugar online", "Inicia sesi\xC3\xB3n en Alboria desde el launcher",
              "Alboria no ha respondido"};
    case 6:  // Italian
      return {"STANZA ONLINE", "Creazione della stanza\xE2\x80\xA6", "Ingresso nella stanza\xE2\x80\xA6", "ONLINE",
              "Impossibile raggiungere Alboria", "Nessuna partita ha questo codice",
              "La partita \xC3\xA8 piena", "La stanza \xC3\xA8 stata chiusa", "Alboria ti ha bandito",
              "Aggiorna Azura per giocare online", "Accedi ad Alboria dal launcher", "Alboria non ha risposto"};
    case 9:  // Portuguese
      return {"SALA ONLINE", "Criando a sala\xE2\x80\xA6", "Entrando na sala\xE2\x80\xA6", "ONLINE",
              "N\xC3\xA3o foi poss\xC3\xADvel conectar ao Alboria",
              "Nenhuma partida tem esse c\xC3\xB3""digo", "A partida est\xC3\xA1 cheia",
              "A sala foi fechada", "O Alboria te baniu", "Atualize o Azura para jogar online",
              "Entre no Alboria pelo launcher", "O Alboria n\xC3\xA3o respondeu"};
    case 11:  // Polish
      return {"POK\xC3\x93J ONLINE", "Tworzenie pokoju\xE2\x80\xA6", "Do\xC5\x82\xC4\x85""czanie do pokoju\xE2\x80\xA6",
              "ONLINE",
              "Brak po\xC5\x82\xC4\x85""czenia z Alboria", "\xC5\xBB""adna gra nie ma tego kodu",
              "Gra jest pe\xC5\x82na", "Pok\xC3\xB3j zosta\xC5\x82 zamkni\xC4\x99ty",
              "Alboria ci\xC4\x99 zablokowa\xC5\x82""a", "Zaktualizuj Azur\xC4\x99, aby gra\xC4\x87 online",
              "Zaloguj si\xC4\x99 do Alborii w launcherze", "Alboria nie odpowiedzia\xC5\x82""a"};
    default:
      return {"ONLINE ROOM", "Creating the room\xE2\x80\xA6", "Joining the room\xE2\x80\xA6", "ONLINE", "Cannot reach Alboria",
              "No game has that code", "The game is full", "The room was closed",
              "Alboria has banned you", "Update Azura to play online", "Sign in to Alboria from the launcher",
              "Alboria did not answer"};
  }
}

// What the player reads for a failure, by alboria::Status::error.
const char* ErrorText(const Texts& texts, const std::string& error) {
  if (error == "network" || error == "relay_unreachable" || error == "timed_out") return texts.network;
  if (error == "room_not_found" || error == "wrong_title") return texts.not_found;
  if (error == "room_full") return texts.full;
  if (error == "room_closed" || error == "replaced") return texts.closed;
  if (error.rfind("banned", 0) == 0) return texts.banned;
  if (error == "client_too_old") return texts.too_old;
  if (error == "no_account") return texts.no_account;
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
    case alboria::State::kOffline:
      error_.clear();
      return;
    case alboria::State::kWorking:
      error_.clear();
      title = texts.title;
      line = status.host ? texts.creating : texts.joining;
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
