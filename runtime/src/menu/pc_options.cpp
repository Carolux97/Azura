#include "pc_options.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "coalesced.h"

REXCVAR_DEFINE_BOOL(motion_blur, true, "Azura/Graphics",
                    "Motion blur (the game's SystemSettings.MotionBlur); read when the game starts")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace azura::menu {

namespace {

enum class Kind { kAction, kToggle, kChoice, kSlider };

// Text keys of an option, looked up in Texts.
enum class Label { kCalibrate, kFullscreen, kVsync, kAntialiasing, kMotionBlur, kMouseSensitivity, kCount };

// One PC option: the row it takes in a category of the menu, the cvar it edits, the profile
// setting the row is tied to (`psid`) and, for lists, where that setting is defined with its value
// names (`mapping`, in GearProfileSettings.ProfileMappings). The menu reads and writes that
// setting too, which is harmless: it is one the game never uses.
struct Option {
  int category;
  int row;
  Label label;
  const char* cvar;  // nullptr for the action row
  Kind kind;
  int psid;
  int mapping;                       // -1 for sliders
  std::vector<const char*> choices;  // kChoice: the cvar value for each list index
  float slider_min = 0, slider_max = 0, slider_step = 0;
};

const std::vector<Option>& Options() {
  static const std::vector<Option> options = {
      // List rows use profile settings the game never uses, the racing game preferences of the
      // engine, and rewrite the value names of their definitions in place. Changing anything else
      // in the list of definitions (moving one, or defining new IDs in its empty slots) makes the
      // game hang while it reads the profile. A definition keeps as many values as it had, so the
      // calibration row (Movement Control, two values) names both "Open".
      {kVideoCategory, kCalibrateRow, Label::kCalibrate, nullptr, Kind::kAction, 18, 11, {}},
      {kVideoCategory, 1, Label::kFullscreen, "fullscreen", Kind::kToggle, 22, 15, {}},
      {kVideoCategory, 2, Label::kVsync, "vsync", Kind::kToggle, 19, 12, {}},
      {kVideoCategory, 3, Label::kAntialiasing, "swap_post_effect", Kind::kChoice, 20, 13,
       {"none", "fxaa", "fxaa_extreme"}},
      {kVideoCategory, 4, Label::kMotionBlur, "motion_blur", Kind::kToggle, 21, 14, {}},
      // A slider needs no definition, and its ID is one the profile does not have: the menu's reads
      // and writes of it fail and change nothing.
      {kKeyboardMouseCategory, 0, Label::kMouseSensitivity, "mnk_sensitivity", Kind::kSlider, 9105, -1, {}, 0.1f, 3.0f,
       0.1f},
  };
  return options;
}

struct Texts {
  std::pair<const char*, const char*> keyboard_mouse;  // category title, tooltip
  std::pair<const char*, const char*> labels[static_cast<int>(Label::kCount)];  // label, tooltip
  const char* off;
  const char* on;
  const char* fxaa;
  const char* fxaa_extreme;
  const char* open;  // the calibration row's only value
};

// Keyed by the extension of the localization files. Labels are upper case like the game's own.
const std::map<std::string, Texts>& LocalizedTexts() {
  static const std::map<std::string, Texts> texts = {
      {"int",
       {{"KEYBOARD AND MOUSE", "Adjust the keyboard and mouse settings."},
        {{"CALIBRATE BRIGHTNESS", "Open the brightness calibration screen."},
         {"FULL SCREEN", "Play in full screen or in a window."},
         {"VSYNC", "Synchronise the image with the display to avoid tearing."},
         {"ANTI-ALIASING", "Smooth jagged edges. Takes effect the next time the game starts."},
         {"MOTION BLUR", "Blur the image during fast movement. Takes effect the next time the game starts."},
         {"MOUSE SENSITIVITY", "Adjust how fast the camera turns with the mouse."}},
        "Off", "On", "FXAA", "FXAA extreme", "Open"}},
      {"esn",
       {{"TECLADO Y RATÓN", "Ajuste las opciones del teclado y el ratón."},
        {{"CALIBRAR BRILLO", "Abra la pantalla de calibración del brillo."},
         {"PANTALLA COMPLETA", "Juegue a pantalla completa o en una ventana."},
         {"SINCRONIZACIÓN VERTICAL", "Sincronice la imagen con la pantalla para evitar cortes."},
         {"ANTIALIASING", "Suavice los bordes dentados. Se aplica la próxima vez que inicie el juego."},
         {"DESENFOQUE DE MOVIMIENTO",
          "Desenfoque la imagen en los movimientos rápidos. Se aplica la próxima vez que inicie el juego."},
         {"SENSIBILIDAD DEL RATÓN", "Ajuste la velocidad a la que gira la cámara con el ratón."}},
        "Desactivado", "Activado", "FXAA", "FXAA extremo", "Abrir"}},
      {"deu",
       {{"TASTATUR UND MAUS", "Passen Sie die Einstellungen für Tastatur und Maus an."},
        {{"HELLIGKEIT KALIBRIEREN", "Öffnet den Bildschirm zur Kalibrierung der Helligkeit."},
         {"VOLLBILD", "Im Vollbild oder in einem Fenster spielen."},
         {"VERTIKALE SYNCHRONISATION", "Synchronisiert das Bild mit dem Bildschirm, um Tearing zu vermeiden."},
         {"KANTENGLÄTTUNG", "Glättet gezackte Kanten. Wird beim nächsten Spielstart wirksam."},
         {"BEWEGUNGSUNSCHÄRFE", "Verwischt das Bild bei schnellen Bewegungen. Wird beim nächsten Spielstart wirksam."},
         {"MAUSEMPFINDLICHKEIT", "Legt fest, wie schnell sich die Kamera mit der Maus dreht."}},
        "Aus", "Ein", "FXAA", "FXAA extrem", "Öffnen"}},
      {"ita",
       {{"TASTIERA E MOUSE", "Regola le impostazioni di tastiera e mouse."},
        {{"CALIBRA LUMINOSITÀ", "Apri la schermata di calibrazione della luminosità."},
         {"SCHERMO INTERO", "Gioca a schermo intero o in una finestra."},
         {"SINCRONIZZAZIONE VERTICALE", "Sincronizza l'immagine con lo schermo per evitare lo screen tearing."},
         {"ANTI-ALIASING", "Smussa i bordi frastagliati. Ha effetto al prossimo avvio del gioco."},
         {"SFOCATURA DI MOVIMENTO", "Sfoca l'immagine durante i movimenti rapidi. Ha effetto al prossimo avvio del gioco."},
         {"SENSIBILITÀ DEL MOUSE", "Regola la velocità con cui la visuale ruota con il mouse."}},
        "Disattivato", "Attivato", "FXAA", "FXAA estremo", "Apri"}},
      {"jpn",
       {{"キーボードとマウス", "キーボードとマウスの設定を調整します。"},
        {{"明るさの調整", "明るさの調整画面を開きます。"},
         {"フルスクリーン", "フルスクリーンまたはウィンドウでプレイします。"},
         {"垂直同期", "画面のティアリングを防ぐため、映像をディスプレイと同期させます。"},
         {"アンチエイリアス", "ギザギザした輪郭を滑らかにします。次回ゲーム起動時に適用されます。"},
         {"モーションブラー", "素早い動きの際に映像をぼかします。次回ゲーム起動時に適用されます。"},
         {"マウス感度", "マウスでカメラを回転させる速さを調整します。"}},
        "オフ", "オン", "FXAA", "FXAA (最大)", "開く"}},
      {"fra",
       {{"CLAVIER ET SOURIS", "Réglez les options du clavier et de la souris."},
        {{"CALIBRER LA LUMINOSITÉ", "Ouvrez l'écran de calibrage de la luminosité."},
         {"PLEIN ÉCRAN", "Jouez en plein écran ou dans une fenêtre."},
         {"SYNCHRO VERTICALE", "Synchronisez l'image avec l'écran pour éviter les déchirures."},
         {"ANTICRÉNELAGE", "Lissez les bords crénelés. Prend effet au prochain lancement du jeu."},
         {"FLOU DE MOUVEMENT", "Floutez l'image lors des mouvements rapides. Prend effet au prochain lancement du jeu."},
         {"SENSIBILITÉ DE LA SOURIS", "Réglez la vitesse de rotation de la caméra avec la souris."}},
        "Désactivé", "Activé", "FXAA", "FXAA extrême", "Ouvrir"}},
      {"ptb",
       {{"TECLADO E MOUSE", "Ajuste as opções do teclado e do mouse."},
        {{"CALIBRAR BRILHO", "Abra a tela de calibração do brilho."},
         {"TELA CHEIA", "Jogue em tela cheia ou em uma janela."},
         {"SINCRONIZAÇÃO VERTICAL", "Sincronize a imagem com a tela para evitar cortes."},
         {"ANTISSERRILHAMENTO", "Suaviza bordas serrilhadas. Aplica-se na próxima vez que o jogo for iniciado."},
         {"DESFOQUE DE MOVIMENTO",
          "Desfoca a imagem em movimentos rápidos. Aplica-se na próxima vez que o jogo for iniciado."},
         {"SENSIBILIDADE DO MOUSE", "Ajuste a velocidade com que a câmera gira com o mouse."}},
        "Desativado", "Ativado", "FXAA", "FXAA extremo", "Abrir"}},
      {"pol",
       {{"KLAWIATURA I MYSZ", "Dostosuj ustawienia klawiatury i myszy."},
        {{"KALIBRACJA JASNOŚCI", "Otwórz ekran kalibracji jasności."},
         {"PEŁNY EKRAN", "Graj na pełnym ekranie lub w oknie."},
         {"SYNCHRONIZACJA PIONOWA", "Synchronizuje obraz z ekranem, aby uniknąć rozrywania."},
         {"WYGŁADZANIE KRAWĘDZI", "Wygładza postrzępione krawędzie. Zadziała po ponownym uruchomieniu gry."},
         {"ROZMYCIE RUCHU", "Rozmywa obraz podczas szybkiego ruchu. Zadziała po ponownym uruchomieniu gry."},
         {"CZUŁOŚĆ MYSZY", "Dostosuj szybkość obrotu kamery myszą."}},
        "Wyłączone", "Włączone", "FXAA", "FXAA ekstremalne", "Otwórz"}},
      {"rus",
       {{"КЛАВИАТУРА И МЫШЬ", "Настройте параметры клавиатуры и мыши."},
        {{"КАЛИБРОВКА ЯРКОСТИ", "Откройте экран калибровки яркости."},
         {"ПОЛНОЭКРАННЫЙ РЕЖИМ", "Играйте в полноэкранном режиме или в окне."},
         {"ВЕРТИКАЛЬНАЯ СИНХРОНИЗАЦИЯ", "Синхронизирует изображение с экраном, чтобы избежать разрывов."},
         {"СГЛАЖИВАНИЕ", "Сглаживает неровные края. Вступит в силу при следующем запуске игры."},
         {"РАЗМЫТИЕ В ДВИЖЕНИИ", "Размывает изображение при быстром движении. Вступит в силу при следующем запуске игры."},
         {"ЧУВСТВИТЕЛЬНОСТЬ МЫШИ", "Настройте скорость поворота камеры мышью."}},
        "Выкл.", "Вкл.", "FXAA", "FXAA (макс.)", "Открыть"}},
      {"cze",
       {{"KLÁVESNICE A MYŠ", "Upravte nastavení klávesnice a myši."},
        {{"KALIBRACE JASU", "Otevřete obrazovku kalibrace jasu."},
         {"CELÁ OBRAZOVKA", "Hrajte na celé obrazovce nebo v okně."},
         {"VERTIKÁLNÍ SYNCHRONIZACE", "Synchronizuje obraz s displejem, aby se zabránilo trhání."},
         {"VYHLAZOVÁNÍ HRAN", "Vyhlazuje zubaté hrany. Projeví se při příštím spuštění hry."},
         {"ROZMAZÁNÍ POHYBU", "Rozmazává obraz při rychlém pohybu. Projeví se při příštím spuštění hry."},
         {"CITLIVOST MYŠI", "Upravte rychlost otáčení kamery myší."}},
        "Vypnuto", "Zapnuto", "FXAA", "FXAA extrémní", "Otevřít"}},
      {"hun",
       {{"BILLENTYŰZET ÉS EGÉR", "A billentyűzet és az egér beállításai."},
        {{"FÉNYERŐ KALIBRÁLÁSA", "Megnyitja a fényerő kalibrálási képernyőjét."},
         {"TELJES KÉPERNYŐ", "Játék teljes képernyőn vagy ablakban."},
         {"FÜGGŐLEGES SZINKRON", "Szinkronizálja a képet a kijelzővel a képtörés elkerülése érdekében."},
         {"ÉLSIMÍTÁS", "Kisimítja a recés éleket. A játék következő indításakor lép életbe."},
         {"MOZGÁSI ELMOSÓDÁS", "Elmossa a képet gyors mozgáskor. A játék következő indításakor lép életbe."},
         {"EGÉRÉRZÉKENYSÉG", "Az egérrel történő kameraforgatás sebessége."}},
        "Ki", "Be", "FXAA", "FXAA (extrém)", "Megnyitás"}},
      {"chn",
       {{"鍵盤與滑鼠", "調整鍵盤與滑鼠設定。"},
        {{"校正亮度", "開啟亮度校正畫面。"},
         {"全螢幕", "以全螢幕或視窗模式進行遊戲。"},
         {"垂直同步", "讓畫面與顯示器同步，以避免畫面撕裂。"},
         {"反鋸齒", "讓鋸齒狀的邊緣更平滑。下次啟動遊戲時生效。"},
         {"動態模糊", "在快速移動時模糊畫面。下次啟動遊戲時生效。"},
         {"滑鼠靈敏度", "調整用滑鼠轉動鏡頭的速度。"}},
        "關閉", "開啟", "FXAA", "FXAA 極致", "開啟"}},
      {"kor",
       {{"키보드 및 마우스", "키보드 및 마우스 설정을 조정합니다."},
        {{"밝기 보정", "밝기 보정 화면을 엽니다."},
         {"전체 화면", "전체 화면 또는 창 모드로 플레이합니다."},
         {"수직 동기화", "화면 찢어짐을 막기 위해 화면과 동기화합니다."},
         {"안티에일리어싱", "계단 현상을 부드럽게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"모션 블러", "빠르게 움직일 때 화면을 흐리게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"마우스 감도", "마우스로 카메라를 돌리는 속도를 조정합니다."}},
        "끔", "켬", "FXAA", "FXAA 최대", "열기"}},
  };
  return texts;
}

const Texts& TextsFor(const std::string& language) {
  // Latin American Spanish (esm) uses the Spanish texts.
  const auto found = LocalizedTexts().find(language == "esm" ? "esn" : language);
  return found != LocalizedTexts().end() ? found->second : LocalizedTexts().at("int");
}

std::mutex g_mutex;
std::filesystem::path g_config_path;
// Value shown for each option once read: settings that apply at the next start keep their old
// cvar value until then, so the menu shows what was chosen rather than what is in effect.
std::map<int, float> g_values;

bool Valid(int option) {
  return option >= 0 && option < static_cast<int>(Options().size());
}

float ReadCvar(const Option& option) {
  if (!option.cvar) return 0.0f;
  const std::string value = rex::cvar::GetFlagByName(option.cvar);
  switch (option.kind) {
    case Kind::kAction:
      return 0.0f;
    case Kind::kToggle:
      return value == "true" || value == "1" ? 1.0f : 0.0f;
    case Kind::kChoice:
      for (size_t i = 0; i < option.choices.size(); ++i) {
        if (value == option.choices[i]) return static_cast<float>(i);
      }
      return 0.0f;
    case Kind::kSlider:
      try {
        return std::stof(value);
      } catch (...) {
        return option.slider_min;
      }
  }
  return 0.0f;
}

std::string FormatFloat(float value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", value);
  return text;
}

// The value as the cvar takes it (`toml` set: as written in azura.toml).
std::string FormatValue(const Option& option, float value, bool toml) {
  switch (option.kind) {
    case Kind::kAction:
      return {};
    case Kind::kToggle:
      return value >= 0.5f ? "true" : "false";
    case Kind::kChoice: {
      const size_t index = std::min(static_cast<size_t>(value < 0 ? 0 : value + 0.5f), option.choices.size() - 1);
      return toml ? std::string("\"") + option.choices[index] + "\"" : option.choices[index];
    }
    case Kind::kSlider:
      return FormatFloat(value);
  }
  return {};
}

// Value of top-level `key` in the TOML file at `path` (quotes removed), or empty when missing.
std::string ReadTomlValue(const std::filesystem::path& path, const std::string& key) {
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '[') {
      if (start != std::string::npos) break;  // a table starts: no more top-level keys
      continue;
    }
    if (line.compare(start, key.size(), key) != 0) continue;
    const size_t equals = line.find_first_not_of(" \t", start + key.size());
    if (equals == std::string::npos || line[equals] != '=') continue;
    std::string value = line.substr(equals + 1);
    const size_t comment = value.find('#');
    if (comment != std::string::npos) value.erase(comment);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t\r") + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return value;
  }
  return {};
}

// Sets `key = value` in the TOML file at `path`, keeping every other line as it is.
void UpsertTomlKey(const std::filesystem::path& path, const std::string& key, const std::string& value) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string assignment = key + " = " + value;
  bool replaced = false;
  for (auto& line : lines) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
    const size_t after = line.find_first_not_of(" \t", start + key.size());
    if (after != std::string::npos && line[after] == '=') {
      line = assignment;
      replaced = true;
      break;
    }
  }
  if (!replaced) lines.push_back(assignment);
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    for (const auto& line : lines) out << line << '\n';
    if (!out) {
      REXLOG_ERROR("[Azura options] Cannot write {}", temporary.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) REXLOG_ERROR("[Azura options] Cannot save {}: {}", path.string(), ec.message());
}

const CoalescedKey* FindKey(const CoalescedSection& section, const std::string& name) {
  for (const auto& key : section.keys) {
    if (key.name.Ascii() == name) return &key;
  }
  return nullptr;
}

// Sets `name` in `section`, adding it when missing.
void SetKey(CoalescedSection& section, const std::string& name, const std::string& utf8_value) {
  for (auto& key : section.keys) {
    if (key.name.Ascii() == name) {
      key.value = CoalescedString::FromUtf8(utf8_value);
      return;
    }
  }
  section.keys.push_back({CoalescedString::FromAscii(name), CoalescedString::FromUtf8(utf8_value)});
}

std::string Utf8(const std::u16string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size()) c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | c >> 6));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | c >> 12));
      out.push_back(static_cast<char>(0x80 | (c >> 6 & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | c >> 18));
      out.push_back(static_cast<char>(0x80 | (c >> 12 & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c >> 6 & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

// Every value of `key="..."` in a struct value, in order, as UTF-8. The key must start the field
// (after '(' or ','), so a key does not match inside a longer name.
std::vector<std::string> Quoted(const std::u16string& text, const std::u16string& key) {
  std::vector<std::string> values;
  const std::u16string marker = key + u"=\"";
  for (size_t at = text.find(marker); at != std::u16string::npos; at = text.find(marker, at)) {
    const size_t start = at + marker.size();
    if (at > 0 && text[at - 1] != u'(' && text[at - 1] != u',') {
      at = start;
      continue;
    }
    const size_t end = text.find(u'"', start);
    if (end == std::u16string::npos) break;
    values.push_back(Utf8(text.substr(start, end - start)));
    at = end + 1;
  }
  return values;
}

std::u16string CategoryText(const CoalescedSection& section, int category) {
  const CoalescedKey* key = FindKey(section, "OptionCategories[" + std::to_string(category) + "]");
  return key ? key->value.text : std::u16string();
}

// The row text of `option` with every field: a localized struct keeps whatever the text leaves out
// from the row that was at that position before.
std::string RowText(const Option& option, const Texts& texts) {
  const auto& [label, tooltip] = texts.labels[static_cast<int>(option.label)];
  std::ostringstream row;
  row << "(ProfileSettingId=" << option.psid << ",SettingDescription=\"" << label << "\",SettingTooltip=\"" << tooltip
      << "\",bUsesRawValue=false,bIsGlobalValue=false,bHasOnChangeCallback=false,"
      << "bRequiresMatureLanguageSupport=false,";
  if (option.kind == Kind::kSlider) {
    row << "Type=OST_Slider,Slider_Minimum=" << FormatFloat(option.slider_min)
        << ",Slider_Maximum=" << FormatFloat(option.slider_max)
        << ",Slider_Increment=" << FormatFloat(option.slider_step) << ")";
  } else {
    row << "Type=OST_OptionList,Slider_Minimum=0.00,Slider_Maximum=0.00,Slider_Increment=0.00)";
  }
  return row.str();
}

}  // namespace

void SetPcOptionsConfigPath(std::filesystem::path config_path) {
  std::lock_guard lock(g_mutex);
  g_config_path = std::move(config_path);
}

int FindPcOption(int category, int row) {
  for (size_t i = 0; i < Options().size(); ++i) {
    if (Options()[i].category == category && Options()[i].row == row) return static_cast<int>(i);
  }
  return -1;
}

bool PcOptionIsSlider(int option) {
  return Valid(option) && Options()[option].kind == Kind::kSlider;
}

bool PcOptionIsAction(int option) {
  return Valid(option) && Options()[option].kind == Kind::kAction;
}

float PcOptionDisplayValue(int option) {
  if (!Valid(option)) return 0.0f;
  std::lock_guard lock(g_mutex);
  auto found = g_values.find(option);
  if (found == g_values.end()) found = g_values.emplace(option, ReadCvar(Options()[option])).first;
  return found->second;
}

void PcOptionStoreValue(int option, float value) {
  if (!Valid(option) || PcOptionIsAction(option)) return;
  const Option& entry = Options()[option];
  std::filesystem::path config_path;
  {
    std::lock_guard lock(g_mutex);
    const auto found = g_values.find(option);
    const float shown = found != g_values.end() ? found->second : ReadCvar(entry);
    if (FormatValue(entry, shown, false) == FormatValue(entry, value, false)) return;
    g_values[option] = value;
    config_path = g_config_path;
  }
  rex::cvar::SetFlagByName(entry.cvar, FormatValue(entry, value, false));
  REXLOG_INFO("[Azura options] {} = {}", entry.cvar, FormatValue(entry, value, false));
  if (!config_path.empty()) UpsertTomlKey(config_path, entry.cvar, FormatValue(entry, value, true));
}

void PcOptionsResetDefaults(int category) {
  std::filesystem::path config_path;
  {
    std::lock_guard lock(g_mutex);
    config_path = g_config_path;
  }
  // The project's defaults are in azura.default.toml beside azura.toml; a key it does not set
  // falls back to the SDK's default.
  const std::filesystem::path defaults_path = config_path.parent_path() / "azura.default.toml";
  for (size_t i = 0; i < Options().size(); ++i) {
    const Option& option = Options()[i];
    if (option.category != category || !option.cvar) continue;
    const std::string project_default = ReadTomlValue(defaults_path, option.cvar);
    if (project_default.empty()) {
      rex::cvar::ResetToDefault(option.cvar);
    } else {
      rex::cvar::SetFlagByName(option.cvar, project_default);
    }
    const float value = ReadCvar(option);
    {
      std::lock_guard lock(g_mutex);
      g_values[static_cast<int>(i)] = value;
    }
    if (!config_path.empty()) UpsertTomlKey(config_path, option.cvar, FormatValue(option, value, true));
    REXLOG_INFO("[Azura options] {} reset to {}", option.cvar, FormatValue(option, value, false));
  }
}

void PatchOptionsMenu(CoalescedSection& section, const std::string& language) {
  const Texts& texts = TextsFor(language);

  // Button texts from the game's Audio category, in this file's language: Back and Defaults.
  const auto buttons = Quoted(CategoryText(section, 2), u"ButtonDescription");
  const std::string back = buttons.size() > 0 ? buttons[0] : "BACK";
  const std::string defaults = buttons.size() > 1 ? buttons[1] : "DEFAULTS";

  // Video: a list of our rows. The category keeps its texts from the file; OnSelectedCallback is
  // cleared so that choosing it opens the list instead of the calibration screen (now the first
  // row). Buttons as in the game's categories (ButtonType 1 = B, 2 = X): Back and Defaults.
  const std::u16string video = CategoryText(section, kVideoCategory);
  const auto video_title = Quoted(video, u"CategoryDescription");
  const auto video_tooltip = Quoted(video, u"CategoryTooltip");
  std::string rows;
  for (const Option& option : Options()) {
    if (option.category == kVideoCategory) rows += (rows.empty() ? "" : ",") + RowText(option, texts);
  }
  SetKey(section, "OptionCategories[" + std::to_string(kVideoCategory) + "]",
         "(CategoryDescription=\"" + (video_title.empty() ? std::string("VIDEO") : video_title[0]) +
             "\",CategoryTooltip=\"" + (video_tooltip.empty() ? std::string() : video_tooltip[0]) +
             "\",OnSelectedCallback=\"\",bIsGlobalValue=false,CategoryOptions=(" + rows +
             "),MenuButtons=((ButtonDescription=\"" + back +
             "\",ButtonType=1,ButtonHandler=OBH_MAX),(ButtonDescription=\"" + defaults +
             "\",ButtonType=2,ButtonHandler=OBH_Defaults)))");

  // Keyboard and mouse, in place of the storage location (a device picker without rows): every
  // field is written, since the position held a different category.
  std::string keyboard_mouse_rows;
  for (const Option& option : Options()) {
    if (option.category == kKeyboardMouseCategory) {
      keyboard_mouse_rows += (keyboard_mouse_rows.empty() ? "" : ",") + RowText(option, texts);
    }
  }
  SetKey(section, "OptionCategories[" + std::to_string(kKeyboardMouseCategory) + "]",
         std::string("(CategoryDescription=\"") + texts.keyboard_mouse.first + "\",CategoryTooltip=\"" +
             texts.keyboard_mouse.second + "\",OnSelectedCallback=\"\",bIsGlobalValue=false,CategoryOptions=(" +
             keyboard_mouse_rows + "),MenuButtons=((ButtonDescription=\"" + back +
             "\",ButtonType=1,ButtonHandler=OBH_MAX),(ButtonDescription=\"" + defaults +
             "\",ButtonType=2,ButtonHandler=OBH_Defaults)))");
}

void PatchValueNames(CoalescedSection& section, const std::string& language) {
  const Texts& texts = TextsFor(language);
  for (const Option& option : Options()) {
    if (option.mapping < 0) continue;
    std::string values;
    switch (option.kind) {
      case Kind::kAction:
        values = std::string("(Id=0,Name=\"") + texts.open + "\"),(Id=1,Name=\"" + texts.open + "\")";
        break;
      case Kind::kToggle:
        values = std::string("(Id=0,Name=\"") + texts.off + "\"),(Id=1,Name=\"" + texts.on + "\")";
        break;
      case Kind::kChoice:
        values = std::string("(Id=0,Name=\"") + texts.off + "\"),(Id=1,Name=\"" + texts.fxaa + "\"),(Id=2,Name=\"" +
                 texts.fxaa_extreme + "\")";
        break;
      case Kind::kSlider:
        break;
    }
    SetKey(section, "ProfileMappings[" + std::to_string(option.mapping) + "]", "(ValueMappings=(" + values + "))");
  }
}

void PatchSystemSettings(CoalescedSection& section) {
  SetKey(section, "MotionBlur", REXCVAR_GET(motion_blur) ? "True" : "False");
  SetKey(section, "AmbientOcclusion", "False");
  SetKey(section, "Bloom", "False");
  SetKey(section, "DepthOfField", "False");
  SetKey(section, "DynamicShadows", "False");
  SetKey(section, "LightEnvironmentShadows", "False");
  SetKey(section, "bAllowWholeSceneDominantShadows", "False");
  SetKey(section, "bEnableForegroundShadowsOnWorld", "False");
  SetKey(section, "bEnableForegroundSelfShadowing", "False");
  SetKey(section, "LensFlares", "False");
  SetKey(section, "FogVolumes", "False");
}

void PatchEngineSettings(CoalescedSection& section) {
  // Disable background level streaming to guarantee synchronous loading of initial sub-levels
  // and prevent pawns from spawning in the void before collision geometry is ready.
  SetKey(section, "bUseBackgroundLevelStreaming", "False");
}

}  // namespace azura::menu
