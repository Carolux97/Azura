#include "menu_patches.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "coalesced.h"
#include "pc_options.h"

REXCVAR_DEFINE_BOOL(menu_remove_unused, true, "Azura/Menu",
                    "Remove main menu options that cannot work (event calendar, Marketplace), in the "
                    "main menu and in the menu of a player who joined another's party");
REXCVAR_DEFINE_BOOL(menu_coming_soon, true, "Azura/Menu",
                    "Grey out online options and mark them \"(coming soon)\"");
REXCVAR_DEFINE_BOOL(dlc_campaign, false, "Azura/DLC",
                    "RAAM's Shadow is installed and Azura's script patch for it is mounted: the "
                    "campaign menu's DLC option opens it (set by the launcher)");
// Defined by the SDK (system/xam/system_link.cpp).
REXCVAR_DECLARE(std::string, network_mode);

namespace azura::menu {

namespace {

std::atomic<int> g_quit_option{-1};
std::atomic<int> g_options_option{-1};

// Label of the added option. Its text is the [MainMenu] key of the same name.
constexpr const char* kQuitLabel = "AzuraQuit";

// The game mounts its folder here (rex::Runtime::SetupVfs); the overlay device holds the patched
// copies.
constexpr const char* kGameMount = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kOverlayMount = "\\Device\\AzuraOverlay";

struct Texts {
  const char* quit;
  const char* quit_tooltip;
  const char* coming_soon;
  const char* dlc_name;  // the campaign add-on's title
  const char* dlc_tooltip;
  const char* not_installed;  // the option's whole title while it is not installed
  const char* not_installed_tooltip;
};

// Texts of online play on Alboria (network_mode = online), for each localization.
struct OnlineTexts {
  // The message box the party options show when Joinable or Invite only cannot be chosen, which
  // with online play on only happens when Alboria cannot be reached.
  const char* unreachable_title;
  const char* unreachable_message;
  // Manage/Invite's LB and RB buttons (UIScene_WhatsUp), in upper case as the game's are. LB shows
  // the room's code and then hides it (its texts change in script_hooks.cpp); this is its first.
  const char* show_id;
  const char* join_by_code;
};

const std::map<std::string, OnlineTexts>& LocalizedOnlineTexts() {
  static const std::map<std::string, OnlineTexts> texts = {
      {"int", {"Cannot reach Alboria", "Online play needs a connection to Alboria. Check your internet connection and try again; if it keeps failing, Alboria may be down for a while.", "SHOW ID", "JOIN BY CODE"}},
      {"esn", {"No se puede conectar con Alboria", "Para jugar online hace falta conectar con Alboria. Comprueba tu conexión a internet y vuelve a intentarlo; si sigue fallando, puede que Alboria no esté disponible ahora mismo.", "MOSTRAR ID", "UNIRSE POR CÓDIGO"}},
      {"esm", {"No se puede conectar con Alboria", "Para jugar en línea hace falta conectar con Alboria. Revisa tu conexión a internet y vuelve a intentarlo; si sigue fallando, puede que Alboria no esté disponible en este momento.", "MOSTRAR ID", "UNIRSE CON CÓDIGO"}},
      {"deu", {"Keine Verbindung zu Alboria", "Für das Online-Spiel ist eine Verbindung zu Alboria nötig. Prüfe deine Internetverbindung und versuche es erneut; wenn es weiter fehlschlägt, ist Alboria vielleicht gerade nicht erreichbar.", "ID ANZEIGEN", "PER CODE BEITRETEN"}},
      {"ita", {"Impossibile raggiungere Alboria", "Per giocare online serve una connessione ad Alboria. Controlla la connessione a internet e riprova; se continua a non funzionare, Alboria potrebbe essere momentaneamente non disponibile.", "MOSTRA ID", "UNISCITI CON CODICE"}},
      {"fra", {"Impossible de joindre Alboria", "Le jeu en ligne nécessite une connexion à Alboria. Vérifiez votre connexion internet et réessayez ; si le problème persiste, Alboria est peut-être indisponible pour le moment.", "AFFICHER L'ID", "REJOINDRE PAR CODE"}},
      {"ptb", {"Não foi possível conectar ao Alboria", "Para jogar online é preciso conectar ao Alboria. Verifique sua conexão com a internet e tente de novo; se continuar falhando, o Alboria pode estar indisponível no momento.", "MOSTRAR ID", "ENTRAR COM CÓDIGO"}},
      {"pol", {"Brak połączenia z Alborią", "Gra online wymaga połączenia z Alborią. Sprawdź połączenie z internetem i spróbuj ponownie; jeśli problem nie ustąpi, Alboria może być chwilowo niedostępna.", "POKAŻ ID", "DOŁĄCZ KODEM"}},
      {"rus", {"Нет связи с Alboria", "Для игры по сети нужно подключение к Alboria. Проверьте подключение к интернету и попробуйте снова; если ошибка повторяется, Alboria может быть временно недоступна.", "ПОКАЗАТЬ ID", "ВОЙТИ ПО КОДУ"}},
      {"cze", {"Nelze se připojit k Alborii", "Hra online vyžaduje připojení k Alborii. Zkontrolujte připojení k internetu a zkuste to znovu; pokud to stále nejde, Alboria může být dočasně nedostupná.", "ZOBRAZIT ID", "PŘIPOJIT SE KÓDEM"}},
      {"hun", {"Nem érhető el az Alboria", "Az online játékhoz kapcsolódni kell az Alboriához. Ellenőrizd az internetkapcsolatot, és próbáld újra; ha továbbra sem megy, lehet, hogy az Alboria átmenetileg nem érhető el.", "AZONOSÍTÓ MEGJELENÍTÉSE", "CSATLAKOZÁS KÓDDAL"}},
      {"jpn", {"Alboria に接続できません", "オンラインプレイには Alboria への接続が必要です。インターネット接続を確認してもう一度お試しください。解決しない場合は、Alboria が一時的に利用できない可能性があります。", "IDを表示", "コードで参加"}},
      {"chn", {"無法連線至 Alboria", "線上遊玩需要連線至 Alboria。請檢查網際網路連線後再試一次；若仍失敗，Alboria 可能暫時無法使用。", "顯示 ID", "以代碼加入"}},
      {"kor", {"Alboria에 연결할 수 없습니다", "온라인 플레이를 하려면 Alboria에 연결해야 합니다. 인터넷 연결을 확인하고 다시 시도하세요. 계속 실패하면 Alboria를 일시적으로 사용할 수 없는 것일 수 있습니다.", "ID 표시", "코드로 참가"}},
  };
  return texts;
}

const OnlineTexts& OnlineTextsFor(const std::string& language) {
  const auto it = LocalizedOnlineTexts().find(language);
  return it != LocalizedOnlineTexts().end() ? it->second : LocalizedOnlineTexts().at("int");
}

// Texts for each localization, keyed by the extension of its files.
const std::map<std::string, Texts>& LocalizedTexts() {
  static const std::map<std::string, Texts> texts = {
      {"int", {"Quit Game", "Close Gears of War 3 and return to the desktop.", "(coming soon)", "RAAM's Shadow", "Play the RAAM's Shadow campaign.", "Not Installed", "Add RAAM's Shadow from the launcher (Installation, Downloadable content) to play it."}},
      {"esn", {"Salir del juego", "Cierra Gears of War 3 y vuelve al escritorio.", "(próximamente)", "Sombras de RAAM", "Juega la campaña de Sombras de RAAM.", "No instalado", "Añade Sombras de RAAM desde el launcher (Instalación, Contenido descargable) para jugarla."}},
      {"deu", {"Spiel beenden", "Beendet Gears of War 3 und kehrt zum Desktop zurück.", "(demnächst)", "RAAMs Schatten", "Spiele die Kampagne RAAM's Shadow.", "Nicht installiert", "Füge RAAMs Schatten im Launcher hinzu (Installation, Herunterladbare Inhalte), um es zu spielen."}},
      {"ita", {"Esci dal gioco", "Chiudi Gears of War 3 e torna al desktop.", "(prossimamente)", "L'ombra di RAAM", "Gioca la campagna RAAM's Shadow.", "Non installato", "Aggiungi L'ombra di RAAM dal launcher (Installazione, Contenuti scaricabili) per giocarlo."}},
      {"jpn", {"ゲームを終了", "Gears of War 3 を終了してデスクトップに戻ります。", "(近日公開)", "RAAM's Shadow", "RAAM's Shadow のキャンペーンをプレイします。", "未インストール", "ランチャー (インストール、ダウンロードコンテンツ) から RAAM's Shadow を追加するとプレイできます。"}},
      {"esm", {"Salir del juego", "Cierra Gears of War 3 y vuelve al escritorio.", "(próximamente)", "Sombras de RAAM", "Juega la campaña de Sombras de RAAM.", "No instalado", "Agrega Sombras de RAAM desde el launcher (Instalación, Contenido descargable) para jugarla."}},
      {"fra", {"Quitter le jeu", "Fermer Gears of War 3 et revenir au bureau.", "(bientôt)", "L'Ombre de RAAM", "Jouer la campagne RAAM's Shadow.", "Non installé", "Ajoutez L'Ombre de RAAM depuis le launcher (Installation, Contenu téléchargeable) pour y jouer."}},
      {"ptb", {"Sair do jogo", "Fecha o Gears of War 3 e volta para a área de trabalho.", "(em breve)", "A Sombra de RAAM", "Jogue a campanha RAAM's Shadow.", "Não instalado", "Adicione A Sombra de RAAM pelo launcher (Instalação, Conteúdo para download) para jogar."}},
      {"pol", {"Wyjdź z gry", "Zamknij Gears of War 3 i wróć do pulpitu.", "(wkrótce)", "Cień RAAM-a", "Zagraj w kampanię RAAM's Shadow.", "Niezainstalowane", "Dodaj Cień RAAM-a w launcherze (Instalacja, Zawartość do pobrania), aby zagrać."}},
      {"rus", {"Выйти из игры", "Закрыть Gears of War 3 и вернуться на рабочий стол.", "(скоро)", "Тень RAAM", "Пройти кампанию RAAM's Shadow.", "Не установлено", "Добавьте «Тень RAAM» в лаунчере (Установка, Загружаемый контент), чтобы играть."}},
      {"cze", {"Ukončit hru", "Zavřít Gears of War 3 a vrátit se na plochu.", "(již brzy)", "RAAMův stín", "Hrát kampaň RAAM's Shadow.", "Nenainstalováno", "Přidejte RAAMův stín v launcheru (Instalace, Stahovatelný obsah) a můžete hrát."}},
      {"hun", {"Kilépés a játékból", "A Gears of War 3 bezárása és visszatérés az asztalra.", "(hamarosan)", "RAAM árnyéka", "A RAAM's Shadow kampány játszása.", "Nincs telepítve", "Add hozzá a RAAM árnyékát a launcherben (Telepítés, Letölthető tartalom) a játékhoz."}},
      {"chn", {"退出遊戲", "關閉 Gears of War 3 並返回桌面。", "(即將推出)", "RAAM's Shadow", "遊玩 RAAM's Shadow 戰役。", "未安裝", "在啟動器（安裝、下載內容）中加入 RAAM's Shadow 即可遊玩。"}},
      {"kor", {"게임 종료", "Gears of War 3를 닫고 바탕 화면으로 돌아갑니다.", "(출시 예정)", "RAAM's Shadow", "RAAM's Shadow 캠페인을 플레이합니다.", "설치되지 않음", "런처(설치, 다운로드 콘텐츠)에서 RAAM's Shadow를 추가하면 플레이할 수 있습니다."}},
  };
  return texts;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// File name after the last path separator, lower-cased ("geargameui.esn").
std::string BaseName(const CoalescedString& path) {
  std::string name = Lower(path.Ascii());
  const size_t slash = name.find_last_of("\\/");
  return slash == std::string::npos ? name : name.substr(slash + 1);
}

// --- Editing an AllOptions value -----------------------------------------------------------------
//
// An AllOptions value looks like
//   (Identifier=MAINMENUOP_X,Title="...",List=((Label="A",Next=...),(Label="B",...)))
// The helpers below split the List into its entries, edit them as strings and join them back.

struct OptionList {
  std::u16string before;               // up to and including "List=("
  std::vector<std::u16string> entries;  // each "(Label=...)"
  std::u16string after;                // from the list's closing ")" on
};

std::optional<OptionList> SplitList(const std::u16string& value) {
  const std::u16string marker = u"List=(";
  const size_t start = value.find(marker);
  if (start == std::u16string::npos) return std::nullopt;
  OptionList list;
  list.before = value.substr(0, start + marker.size());
  size_t i = start + marker.size();
  int depth = 0;
  bool quoted = false;
  size_t entry_start = 0;
  for (; i < value.size(); ++i) {
    const char16_t c = value[i];
    if (c == u'"') quoted = !quoted;
    if (quoted) continue;
    if (c == u'(') {
      if (depth == 0) entry_start = i;
      ++depth;
    } else if (c == u')') {
      if (depth == 0) break;  // end of the list
      if (--depth == 0) list.entries.push_back(value.substr(entry_start, i - entry_start + 1));
    }
  }
  if (i >= value.size()) return std::nullopt;
  list.after = value.substr(i);
  return list;
}

std::u16string JoinList(const OptionList& list) {
  std::u16string out = list.before;
  for (size_t i = 0; i < list.entries.size(); ++i) {
    if (i) out += u',';
    out += list.entries[i];
  }
  return out + list.after;
}

std::u16string EntryLabel(const std::u16string& entry) {
  const std::u16string marker = u"Label=\"";
  const size_t start = entry.find(marker);
  if (start == std::u16string::npos) return {};
  const size_t end = entry.find(u'"', start + marker.size());
  return end == std::u16string::npos ? std::u16string() : entry.substr(start + marker.size(), end - start - marker.size());
}

void DisableEntry(std::u16string& entry) {
  const std::u16string enabled = u"bIsDisabled=false";
  const size_t at = entry.find(enabled);
  if (at != std::u16string::npos) {
    entry.replace(at, enabled.size(), u"bIsDisabled=true");
  } else if (entry.find(u"bIsDisabled=true") == std::u16string::npos) {
    entry.insert(entry.size() - 1, u",bIsDisabled=true");
  }
}

std::u16string U16(const char* ascii) {
  std::u16string out;
  for (const char* p = ascii; *p; ++p) out.push_back(static_cast<unsigned char>(*p));
  return out;
}

// Applies `edit` to the option list of screen `identifier`. Returns false when it is not there.
template <typename Edit>
bool EditScreen(CoalescedSection& section, const char* identifier, Edit edit) {
  const std::u16string prefix = u"(Identifier=" + U16(identifier) + u",";
  for (auto& key : section.keys) {
    if (key.name.Ascii() != "AllOptions" || key.value.text.rfind(prefix, 0) != 0) continue;
    auto list = SplitList(key.value.text);
    if (!list) return false;
    edit(list->entries);
    key.value.text = JoinList(*list);
    return true;
  }
  return false;
}

// Greys out the options whose labels are in `labels`.
auto DisableLabels(std::initializer_list<const char*> labels) {
  std::vector<std::u16string> wanted;
  for (const char* l : labels) wanted.push_back(U16(l));
  return [wanted](std::vector<std::u16string>& entries) {
    for (auto& e : entries) {
      if (std::find(wanted.begin(), wanted.end(), EntryLabel(e)) != wanted.end()) DisableEntry(e);
    }
  };
}

// --- Patches -------------------------------------------------------------------------------------

// Menu structure, in Xbox360-GearUI.ini. Returns the Quit Game index or -1.
int PatchMenuStructure(CoalescedSection& section) {
  int quit_option = -1;
  const bool main_found = EditScreen(section, "MAINMENUOP_MainMenu", [&](std::vector<std::u16string>& entries) {
    // Calendar and Marketplace are the last two options, so removing them moves nothing else.
    while (REXCVAR_GET(menu_remove_unused) && !entries.empty()) {
      const std::u16string label = EntryLabel(entries.back());
      if (label != u"Calendar" && label != u"DLC") break;
      entries.pop_back();
    }
    // Options, as the party client's menu has it (its Label "Options" and TOOLTIP_Options are in
    // every language's [MainMenu]): the main menu only opened them with Start, from its button bar.
    // Without bHaltProgression, as there: a halted menu waits for the script's
    // ContinueFromHaltedState, which never comes for an option the script does not know, and takes
    // no more input once the Options close.
    entries.push_back(u"(Label=\"Options\",ImagePath=\"UI_Portraits_G3.Menu.T_Portraits_MenuSections_MainMenu\","
                      u"Next=MAINMENUOP_None)");
    g_options_option.store(static_cast<int>(entries.size()) - 1, std::memory_order_release);
    entries.push_back(u"(Label=\"" + U16(kQuitLabel) +
                      u"\",ImagePath=\"UI_Portraits_G3.Menu.T_Portraits_MenuSections_MainMenu\","
                      u"Next=MAINMENUOP_None,bHaltProgression=true)");
    quit_option = static_cast<int>(entries.size()) - 1;
  });
  if (!main_found) return -1;
  // The menu of a player who joined another's party: Journal, Calendar, Options, Marketplace (DLC),
  // Leave party. Its choices are handled by position (Callback_ClientMenuOptionWasSelected), so
  // script_hooks.cpp turns the positions left into the original ones (ClientMenuPosition).
  if (REXCVAR_GET(menu_remove_unused)) {
    EditScreen(section, "MAINMENUOP_ClientMenu", [](std::vector<std::u16string>& entries) {
      std::erase_if(entries, [](const std::u16string& e) {
        return EntryLabel(e) == u"Calendar" || EntryLabel(e) == u"DLC";
      });
    });
  }
  // With RAAM's Shadow ready to play, the campaign menu's DLC option ("Coming Soon!" and greyed
  // out on the disc) opens the standard campaign choices; the runtime remembers it was chosen and
  // starts the DLC's chapters (see PatchTravelURL in script_hooks.cpp).
  if (REXCVAR_GET(dlc_campaign)) {
    EditScreen(section, "MAINMENUOP_Campaign", [](std::vector<std::u16string>& entries) {
      for (auto& e : entries) {
        if (EntryLabel(e) == u"CampaignDLC") e = u"(Label=\"CampaignDLC\",Next=MAINMENUOP_CampaignStandard)";
      }
    });
  }
  if (!REXCVAR_GET(menu_coming_soon)) return quit_option;
  EditScreen(section, "MAINMENUOP_CampaignStandard", DisableLabels({"CreatePublic", "JoinPublic"}));
  EditScreen(section, "MAINMENUOP_CampaignArcade", DisableLabels({"CreatePublic", "JoinPublic"}));
  EditScreen(section, "MAINMENUOP_Horde", DisableLabels({"HordeStandard"}));
  EditScreen(section, "MAINMENUOP_Siege", DisableLabels({"SiegeStandard"}));
  EditScreen(section, "MAINMENUOP_VS_Competitiveness", DisableLabels({"Recreational", "Competitive"}));
  return quit_option;
}

// Option texts, in the [MainMenu] section of a GearGameUI localization file.
void PatchMenuTexts(CoalescedSection& section, const Texts& texts) {
  const bool coming_soon = REXCVAR_GET(menu_coming_soon);
  const auto soon = CoalescedString::FromUtf8(std::string(" ") + texts.coming_soon);
  const bool dlc = REXCVAR_GET(dlc_campaign);
  for (auto& key : section.keys) {
    const std::string name = key.name.Ascii();
    // The campaign DLC option: its title, or while the add-on is not installed (the option stays
    // greyed out and unselectable, as the disc has it) "Not Installed" instead of "Coming Soon!".
    if (name == "CampaignDLC") {
      key.value = CoalescedString::FromUtf8(dlc ? texts.dlc_name : texts.not_installed);
    }
    if (name == "TOOLTIP_CampaignDLC") {
      key.value = CoalescedString::FromUtf8(dlc ? texts.dlc_tooltip : texts.not_installed_tooltip);
    }
    // Versus itself is not marked: its private matches play locally and over System Link.
    if (coming_soon && (name == "CreatePublic" || name == "JoinPublic" || name == "HordeStandard" ||
                        name == "SiegeStandard" || name == "Recreational" || name == "Competitive")) {
      key.value.text += soon.text;
      key.value.wide = key.value.wide || soon.wide;
      key.value.empty_marker = false;
    }
  }
  section.keys.push_back({CoalescedString::FromAscii(kQuitLabel), CoalescedString::FromUtf8(texts.quit)});
  section.keys.push_back({CoalescedString::FromAscii(std::string("TOOLTIP_") + kQuitLabel),
                          CoalescedString::FromUtf8(texts.quit_tooltip)});
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Reads a game file through the runtime's file system, which serves it from the game folder or
// from the disc image alike.
bool ReadGameFile(rex::filesystem::Entry* entry, std::vector<uint8_t>& out) {
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &file)) || !file) return false;
  out.resize(entry->size());
  size_t read = 0;
  const auto status = file->ReadSync(std::span<uint8_t>(out), 0, &read);
  file->Destroy();
  return XSUCCEEDED(status) && read == out.size();
}

}  // namespace

// With online play on, the Xbox LIVE Gold message of the party options says that Alboria cannot
// be reached: in [MessageBoxErrorStrings] of a GearGameUI localization file.
void PatchOnlineMessages(CoalescedSection& section, const std::string& language) {
  const OnlineTexts& texts = OnlineTextsFor(language);
  for (auto& key : section.keys) {
    const std::string name = key.name.Ascii();
    if (name == "NeedGoldTier_Title" || name == "SignedOut_Title") {
      key.value = CoalescedString::FromUtf8(texts.unreachable_title);
    } else if (name == "NeedGoldTierFunctionality_Message") {
      key.value = CoalescedString::FromUtf8(texts.unreachable_message);
    }
  }
}

// With online play on, Manage/Invite's LB and RB buttons show a room's code and join one instead of
// inviting Xbox LIVE friends and parties: in [UIScene_WhatsUp] of a GearGame localization file.
// Each keeps its button's controller font character, which the button prompts replace.
void PatchManageInviteButtons(CoalescedSection& section, const std::string& language) {
  const OnlineTexts& texts = OnlineTextsFor(language);
  for (auto& key : section.keys) {
    const std::string name = key.name.Ascii();
    if (name == "InviteGearsButtonActive") {
      key.value = CoalescedString::FromUtf8(std::string("<Font:$ControllerFont>_</Font> ") + texts.show_id);
    } else if (name == "InvitePartyButtonActive") {
      key.value = CoalescedString::FromUtf8(std::string("<Font:$ControllerFont>`</Font> ") + texts.join_by_code);
    }
  }
}

int PatchMenus(Coalesced& coalesced) {
  const bool online = REXCVAR_GET(network_mode) == "online";
  int quit_option = -1;
  for (auto& file : coalesced.files) {
    const std::string name = BaseName(file.name);
    const bool ui_config = name == "xbox360-gearui.ini";
    const bool ui_texts = name.rfind("geargameui.", 0) == 0;
    const bool game_texts = name.rfind("geargame.", 0) == 0;
    const bool engine_config = name == "xbox360-gearengine.ini";
    if (!ui_config && !ui_texts && !game_texts && !engine_config) continue;
    const std::string language = ui_texts || game_texts ? name.substr(name.find('.') + 1) : std::string();
    const auto texts = LocalizedTexts().find(language);
    for (auto& section : file.sections) {
      const std::string section_name = section.name.Ascii();
      if (ui_config && section_name == "GearGame.UIWidget_MainMenu") {
        quit_option = PatchMenuStructure(section);
      } else if (game_texts && section_name == "UIWidget_MainMenu") {
        // The button bar's Start "Options": the main menu has an Options option instead. A space,
        // as the engine keeps the cooked text for a localized property whose text is empty.
        for (auto& key : section.keys) {
          if (key.name.Ascii() == "OptionsButtonStr") key.value = CoalescedString::FromAscii(" ");
        }
      } else if (game_texts && section_name == "UIScene_Options") {
        PatchOptionsMenu(section, language);
      } else if (game_texts && section_name == "GearProfileSettings") {
        PatchValueNames(section, language);
      } else if (engine_config && section_name == "SystemSettings") {
        PatchSystemSettings(section);
      } else if (engine_config && section_name == "Engine.Engine") {
        PatchEngineSettings(section);
      } else if (online && ui_texts && section_name == "MessageBoxErrorStrings") {
        PatchOnlineMessages(section, language);
      } else if (online && game_texts && section_name == "UIScene_WhatsUp") {
        PatchManageInviteButtons(section, language);
      } else if (ui_texts && section_name == "MessageBoxStrings") {
        PatchMessageBoxTexts(section, language);
      } else if (ui_texts && section_name == "MainMenu") {
        PatchMenuTexts(section, texts != LocalizedTexts().end() ? texts->second : LocalizedTexts().at("int"));
      }
    }
  }
  return quit_option;
}

void InstallMenuPatches(rex::Runtime* runtime) {
  namespace fs = std::filesystem;
  auto* vfs = runtime->file_system();
  rex::filesystem::Entry* cooked = vfs->ResolvePath(std::string(kGameMount) + "\\GearGame\\CookedXbox360");
  if (!cooked) {
    REXLOG_WARN("[Azura menu] The game has no GearGame\\CookedXbox360; no menu changes applied");
    return;
  }
  const fs::path cache_root = runtime->cache_root().empty() ? runtime->user_data_root() : runtime->cache_root();
  const fs::path overlay = cache_root / "menu_overlay";
  std::error_code ec;
  fs::create_directories(overlay, ec);
  if (ec) {
    REXLOG_ERROR("[Azura menu] Cannot create {}: {}", overlay.string(), ec.message());
    return;
  }

  std::vector<std::string> patched;
  int quit_option = -1;
  for (const auto& entry : cooked->children()) {
    const std::string file_name = entry->name();
    const std::string lower = Lower(file_name);
    if (lower.rfind("coalesced_", 0) != 0 || !lower.ends_with(".bin")) continue;
    std::vector<uint8_t> data;
    auto coalesced = ReadGameFile(entry.get(), data) ? ParseCoalesced(data) : std::nullopt;
    if (!coalesced) {
      REXLOG_WARN("[Azura menu] {} is not a coalesced file the menu patch understands; left as is", file_name);
      continue;
    }
    const int option = PatchMenus(*coalesced);
    if (option < 0) {
      REXLOG_WARN("[Azura menu] {} has no main menu definition; left as is", file_name);
      continue;
    }
    const std::vector<uint8_t> out = SerializeCoalesced(*coalesced);
    std::vector<uint8_t> existing;
    if (!ReadFile(overlay / file_name, existing) || existing != out) {
      std::ofstream(overlay / file_name, std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                                                 static_cast<std::streamsize>(out.size()));
    }
    patched.push_back(file_name);
    quit_option = option;
  }
  if (patched.empty()) {
    REXLOG_WARN("[Azura menu] No menu changes applied");
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kOverlayMount, overlay, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("[Azura menu] Cannot mount {}", overlay.string());
    return;
  }
  // The game's paths resolve to kGameMount first; these links then send the coalesced files on to
  // the overlay (the resolver follows links until none matches).
  for (const auto& file_name : patched) {
    vfs->RegisterSymbolicLink(std::string(kGameMount) + "\\GearGame\\CookedXbox360\\" + file_name,
                              std::string(kOverlayMount) + "\\" + file_name);
  }
  g_quit_option.store(quit_option, std::memory_order_release);
  REXLOG_INFO("[Azura menu] Menu changes applied to {} file(s); Quit Game is main menu option {}", patched.size(),
              quit_option);
}

int MainMenuQuitOption() {
  return g_quit_option.load(std::memory_order_acquire);
}

int MainMenuOptionsOption() {
  return g_options_option.load(std::memory_order_acquire);
}

}  // namespace azura::menu
