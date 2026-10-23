#include "pc_options.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include "coalesced.h"

REXCVAR_DEFINE_DOUBLE(frame_rate_limit, 30.0, "Azura/Graphics",
                      "Most frames per second: 30 as on the Xbox 360, 60, 90 or 120; 150 or more "
                      "removes the limit");
REXCVAR_DEFINE_BOOL(motion_blur, true, "Azura/Graphics",
                    "Motion blur (the game's SystemSettings.MotionBlur); read when the game starts")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace azura::menu {

namespace {

enum class Kind { kAction, kToggle, kChoice, kSlider };

// Text keys of an option, looked up in Texts.
enum class Label {
  kCalibrate,
  kFullscreen,
  kFrameRateLimit,
  kAntialiasing,
  kMotionBlur,
  kMouseSensitivity,
  kAnisotropic,
  kUpscaling,
  kCount
};

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

// The upscaling effects present_effect accepts in this build of the SDK, in menu order: the Metal
// presenter's on macOS (CAS, MetalFX), the FidelityFX ones with the FidelityFX SDK, bilinear
// always. Others it may accept (the experimental temporal FSR) are not offered.
std::vector<const char*> UpscalingChoices() {
  static const char* const kOffered[] = {"bilinear", "cas", "fsr", "metalfx"};
  std::vector<const char*> choices;
  const rex::cvar::FlagEntry* flag = rex::cvar::GetFlagInfo("present_effect");
  for (const char* effect : kOffered) {
    if (flag && std::find(flag->constraints.allowed_values.begin(), flag->constraints.allowed_values.end(),
                          effect) != flag->constraints.allowed_values.end()) {
      choices.push_back(effect);
    }
  }
  if (choices.empty()) choices.push_back("bilinear");
  return choices;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> options = {
      // List rows use profile settings the game never uses (here the racing game preferences of the
      // engine) and rewrite the value names of their definitions in place. Changing anything else
      // in the list of definitions (moving one, or defining new IDs in its empty slots) makes the
      // game hang while it reads the profile. A definition keeps as many values as it had, so the
      // calibration row (Movement Control, two values) names both "Open".
      {kVideoCategory, kCalibrateRow, Label::kCalibrate, nullptr, Kind::kAction, 18, 11, {}},
      {kVideoCategory, 1, Label::kFullscreen, "fullscreen", Kind::kToggle, 22, 15, {}},
      // A list of our own (no definition, ID 9106, which the profile does not have): its value
      // names come from PcOptionValueNames when the menu asks for them (script_hooks.cpp).
      {kVideoCategory, 2, Label::kFrameRateLimit, "frame_rate_limit", Kind::kChoice, 9106, -1,
       {"30", "60", "90", "120", "150"}},
      {kVideoCategory, 3, Label::kAntialiasing, "swap_post_effect", Kind::kChoice, 20, 13,
       {"none", "fxaa", "fxaa_extreme"}},
      {kVideoCategory, 4, Label::kMotionBlur, "motion_blur", Kind::kToggle, 21, 14, {}},
      // Generic Xbox profile settings with three values that Gears never reads: Controller
      // Sensitivity (it has its own, 368, 59 and 60) and Voice Via Speakers.
      {kVideoCategory, 5, Label::kAnisotropic, "anisotropic_override", Kind::kChoice, 13, 2, {"0", "3", "5"}},
      // A list of our own too: its values depend on the platform (UpscalingChoices).
      {kVideoCategory, 6, Label::kUpscaling, "present_effect", Kind::kChoice, 9107, -1, UpscalingChoices()},
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
  const char* bilinear;
  const char* cas;
  const char* fsr;
};

// Keyed by the extension of the localization files. Labels are upper case like the game's own.
const std::map<std::string, Texts>& LocalizedTexts() {
  static const std::map<std::string, Texts> texts = {
      {"int",
       {{"KEYBOARD AND MOUSE", "Adjust the keyboard and mouse settings."},
        {{"CALIBRATE BRIGHTNESS", "Open the brightness calibration screen."},
         {"FULL SCREEN", "Play in full screen or in a window."},
         {"FRAME RATE LIMIT", "Most frames per second the game draws. 30 is the Xbox 360's."},
         {"ANTI-ALIASING", "Smooth jagged edges. Takes effect the next time the game starts."},
         {"MOTION BLUR", "Blur the image during fast movement. Takes effect the next time the game starts."},
         {"MOUSE SENSITIVITY", "Adjust how fast the camera turns with the mouse."},
         {"ANISOTROPIC FILTERING", "Keep textures sharp when seen at an angle."},
         {"IMAGE SCALING", "How the image is scaled to the window. Takes effect the next time the game starts."}},
        "Off", "On", "FXAA", "FXAA extreme", "Open", "Bilinear (original)", "AMD CAS (sharpening)", "AMD FSR 1"}},
      {"esn",
       {{"TECLADO Y RATÓN", "Ajuste las opciones del teclado y el ratón."},
        {{"CALIBRAR BRILLO", "Abra la pantalla de calibración del brillo."},
         {"PANTALLA COMPLETA", "Juegue a pantalla completa o en una ventana."},
         {"LÍMITE DE FOTOGRAMAS", "Máximo de fotogramas por segundo. 30 es el de Xbox 360."},
         {"ANTIALIASING", "Suavice los bordes dentados. Se aplica la próxima vez que inicie el juego."},
         {"DESENFOQUE DE MOVIMIENTO",
          "Desenfoque la imagen en los movimientos rápidos. Se aplica la próxima vez que inicie el juego."},
         {"SENSIBILIDAD DEL RATÓN", "Ajuste la velocidad a la que gira la cámara con el ratón."},
         {"FILTRADO ANISOTRÓPICO", "Mantenga nítidas las texturas vistas en ángulo."},
         {"ESCALADO DE IMAGEN", "Cómo se escala la imagen a la ventana. Se aplica la próxima vez que inicie el juego."}},
        "Desactivado", "Activado", "FXAA", "FXAA extremo", "Abrir", "Bilineal (original)", "AMD CAS (nitidez)", "AMD FSR 1"}},
      {"deu",
       {{"TASTATUR UND MAUS", "Passen Sie die Einstellungen für Tastatur und Maus an."},
        {{"HELLIGKEIT KALIBRIEREN", "Öffnet den Bildschirm zur Kalibrierung der Helligkeit."},
         {"VOLLBILD", "Im Vollbild oder in einem Fenster spielen."},
         {"BILDRATENBEGRENZUNG", "Höchstzahl der Bilder pro Sekunde. 30 entspricht der Xbox 360."},
         {"KANTENGLÄTTUNG", "Glättet gezackte Kanten. Wird beim nächsten Spielstart wirksam."},
         {"BEWEGUNGSUNSCHÄRFE", "Verwischt das Bild bei schnellen Bewegungen. Wird beim nächsten Spielstart wirksam."},
         {"MAUSEMPFINDLICHKEIT", "Legt fest, wie schnell sich die Kamera mit der Maus dreht."},
         {"ANISOTROPE FILTERUNG", "Hält schräg betrachtete Texturen scharf."},
         {"BILDSKALIERUNG", "Wie das Bild auf das Fenster skaliert wird. Wird beim nächsten Spielstart wirksam."}},
        "Aus", "Ein", "FXAA", "FXAA extrem", "Öffnen", "Bilinear (Original)", "AMD CAS (Schärfung)", "AMD FSR 1"}},
      {"ita",
       {{"TASTIERA E MOUSE", "Regola le impostazioni di tastiera e mouse."},
        {{"CALIBRA LUMINOSITÀ", "Apri la schermata di calibrazione della luminosità."},
         {"SCHERMO INTERO", "Gioca a schermo intero o in una finestra."},
         {"LIMITE FOTOGRAMMI", "Numero massimo di fotogrammi al secondo. 30 è quello di Xbox 360."},
         {"ANTI-ALIASING", "Smussa i bordi frastagliati. Ha effetto al prossimo avvio del gioco."},
         {"SFOCATURA DI MOVIMENTO", "Sfoca l'immagine durante i movimenti rapidi. Ha effetto al prossimo avvio del gioco."},
         {"SENSIBILITÀ DEL MOUSE", "Regola la velocità con cui la visuale ruota con il mouse."},
         {"FILTRO ANISOTROPICO", "Mantiene nitide le texture viste di sbieco."},
         {"RIDIMENSIONAMENTO IMMAGINE", "Come l'immagine viene scalata nella finestra. Ha effetto al prossimo avvio del gioco."}},
        "Disattivato", "Attivato", "FXAA", "FXAA estremo", "Apri", "Bilineare (originale)", "AMD CAS (nitidezza)", "AMD FSR 1"}},
      {"jpn",
       {{"キーボードとマウス", "キーボードとマウスの設定を調整します。"},
        {{"明るさの調整", "明るさの調整画面を開きます。"},
         {"フルスクリーン", "フルスクリーンまたはウィンドウでプレイします。"},
         {"フレームレート上限", "1秒あたりの最大フレーム数。Xbox 360は30です。"},
         {"アンチエイリアス", "ギザギザした輪郭を滑らかにします。次回ゲーム起動時に適用されます。"},
         {"モーションブラー", "素早い動きの際に映像をぼかします。次回ゲーム起動時に適用されます。"},
         {"マウス感度", "マウスでカメラを回転させる速さを調整します。"},
         {"異方性フィルタリング", "斜めから見たテクスチャを鮮明に保ちます。"},
         {"画像スケーリング", "映像をウィンドウに合わせて拡大する方法です。次回ゲーム起動時に適用されます。"}},
        "オフ", "オン", "FXAA", "FXAA (最大)", "開く", "バイリニア（オリジナル）", "AMD CAS（シャープ化）", "AMD FSR 1"}},
      {"fra",
       {{"CLAVIER ET SOURIS", "Réglez les options du clavier et de la souris."},
        {{"CALIBRER LA LUMINOSITÉ", "Ouvrez l'écran de calibrage de la luminosité."},
         {"PLEIN ÉCRAN", "Jouez en plein écran ou dans une fenêtre."},
         {"LIMITE D'IMAGES PAR SECONDE", "Nombre maximal d'images par seconde. 30 correspond à la Xbox 360."},
         {"ANTICRÉNELAGE", "Lissez les bords crénelés. Prend effet au prochain lancement du jeu."},
         {"FLOU DE MOUVEMENT", "Floutez l'image lors des mouvements rapides. Prend effet au prochain lancement du jeu."},
         {"SENSIBILITÉ DE LA SOURIS", "Réglez la vitesse de rotation de la caméra avec la souris."},
         {"FILTRAGE ANISOTROPE", "Gardez les textures nettes vues en biais."},
         {"MISE À L'ÉCHELLE", "Mise à l'échelle de l'image dans la fenêtre. Prend effet au prochain lancement du jeu."}},
        "Désactivé", "Activé", "FXAA", "FXAA extrême", "Ouvrir", "Bilinéaire (d'origine)", "AMD CAS (netteté)", "AMD FSR 1"}},
      {"ptb",
       {{"TECLADO E MOUSE", "Ajuste as opções do teclado e do mouse."},
        {{"CALIBRAR BRILHO", "Abra a tela de calibração do brilho."},
         {"TELA CHEIA", "Jogue em tela cheia ou em uma janela."},
         {"LIMITE DE QUADROS", "Máximo de quadros por segundo. 30 é o do Xbox 360."},
         {"ANTISSERRILHAMENTO", "Suaviza bordas serrilhadas. Aplica-se na próxima vez que o jogo for iniciado."},
         {"DESFOQUE DE MOVIMENTO",
          "Desfoca a imagem em movimentos rápidos. Aplica-se na próxima vez que o jogo for iniciado."},
         {"SENSIBILIDADE DO MOUSE", "Ajuste a velocidade com que a câmera gira com o mouse."},
         {"FILTRAGEM ANISOTRÓPICA", "Mantém nítidas as texturas vistas em ângulo."},
         {"ESCALA DA IMAGEM", "Como a imagem é dimensionada para a janela. Aplica-se na próxima vez que o jogo for iniciado."}},
        "Desativado", "Ativado", "FXAA", "FXAA extremo", "Abrir", "Bilinear (original)", "AMD CAS (nitidez)", "AMD FSR 1"}},
      {"pol",
       {{"KLAWIATURA I MYSZ", "Dostosuj ustawienia klawiatury i myszy."},
        {{"KALIBRACJA JASNOŚCI", "Otwórz ekran kalibracji jasności."},
         {"PEŁNY EKRAN", "Graj na pełnym ekranie lub w oknie."},
         {"LIMIT KLATEK", "Maksymalna liczba klatek na sekundę. 30 to wartość z Xbox 360."},
         {"WYGŁADZANIE KRAWĘDZI", "Wygładza postrzępione krawędzie. Zadziała po ponownym uruchomieniu gry."},
         {"ROZMYCIE RUCHU", "Rozmywa obraz podczas szybkiego ruchu. Zadziała po ponownym uruchomieniu gry."},
         {"CZUŁOŚĆ MYSZY", "Dostosuj szybkość obrotu kamery myszą."},
         {"FILTROWANIE ANIZOTROPOWE", "Zachowuje ostrość tekstur oglądanych pod kątem."},
         {"SKALOWANIE OBRAZU", "Sposób skalowania obrazu do okna. Zadziała po ponownym uruchomieniu gry."}},
        "Wyłączone", "Włączone", "FXAA", "FXAA ekstremalne", "Otwórz", "Dwuliniowe (oryginalne)", "AMD CAS (wyostrzanie)", "AMD FSR 1"}},
      {"rus",
       {{"КЛАВИАТУРА И МЫШЬ", "Настройте параметры клавиатуры и мыши."},
        {{"КАЛИБРОВКА ЯРКОСТИ", "Откройте экран калибровки яркости."},
         {"ПОЛНОЭКРАННЫЙ РЕЖИМ", "Играйте в полноэкранном режиме или в окне."},
         {"ОГРАНИЧЕНИЕ ЧАСТОТЫ КАДРОВ", "Максимальное число кадров в секунду. 30, как на Xbox 360."},
         {"СГЛАЖИВАНИЕ", "Сглаживает неровные края. Вступит в силу при следующем запуске игры."},
         {"РАЗМЫТИЕ В ДВИЖЕНИИ", "Размывает изображение при быстром движении. Вступит в силу при следующем запуске игры."},
         {"ЧУВСТВИТЕЛЬНОСТЬ МЫШИ", "Настройте скорость поворота камеры мышью."},
         {"АНИЗОТРОПНАЯ ФИЛЬТРАЦИЯ", "Сохраняет чёткость текстур, видимых под углом."},
         {"МАСШТАБИРОВАНИЕ ИЗОБРАЖЕНИЯ", "Способ масштабирования изображения под окно. Вступит в силу при следующем запуске игры."}},
        "Выкл.", "Вкл.", "FXAA", "FXAA (макс.)", "Открыть", "Билинейное (оригинал)", "AMD CAS (резкость)", "AMD FSR 1"}},
      {"cze",
       {{"KLÁVESNICE A MYŠ", "Upravte nastavení klávesnice a myši."},
        {{"KALIBRACE JASU", "Otevřete obrazovku kalibrace jasu."},
         {"CELÁ OBRAZOVKA", "Hrajte na celé obrazovce nebo v okně."},
         {"OMEZENÍ SNÍMKŮ", "Nejvyšší počet snímků za sekundu. 30 odpovídá Xbox 360."},
         {"VYHLAZOVÁNÍ HRAN", "Vyhlazuje zubaté hrany. Projeví se při příštím spuštění hry."},
         {"ROZMAZÁNÍ POHYBU", "Rozmazává obraz při rychlém pohybu. Projeví se při příštím spuštění hry."},
         {"CITLIVOST MYŠI", "Upravte rychlost otáčení kamery myší."},
         {"ANIZOTROPNÍ FILTROVÁNÍ", "Udržuje ostré textury viděné pod úhlem."},
         {"ŠKÁLOVÁNÍ OBRAZU", "Způsob škálování obrazu do okna. Projeví se při příštím spuštění hry."}},
        "Vypnuto", "Zapnuto", "FXAA", "FXAA extrémní", "Otevřít", "Bilineární (původní)", "AMD CAS (zostření)", "AMD FSR 1"}},
      {"hun",
       {{"BILLENTYŰZET ÉS EGÉR", "A billentyűzet és az egér beállításai."},
        {{"FÉNYERŐ KALIBRÁLÁSA", "Megnyitja a fényerő kalibrálási képernyőjét."},
         {"TELJES KÉPERNYŐ", "Játék teljes képernyőn vagy ablakban."},
         {"KÉPKOCKASZÁM-KORLÁT", "Másodpercenkénti képkockák legnagyobb száma. A 30 az Xbox 360 értéke."},
         {"ÉLSIMÍTÁS", "Kisimítja a recés éleket. A játék következő indításakor lép életbe."},
         {"MOZGÁSI ELMOSÓDÁS", "Elmossa a képet gyors mozgáskor. A játék következő indításakor lép életbe."},
         {"EGÉRÉRZÉKENYSÉG", "Az egérrel történő kameraforgatás sebessége."},
         {"ANIZOTRÓP SZŰRÉS", "Élesen tartja a ferdén látott textúrákat."},
         {"KÉPSKÁLÁZÁS", "A kép ablakhoz méretezésének módja. A játék következő indításakor lép életbe."}},
        "Ki", "Be", "FXAA", "FXAA (extrém)", "Megnyitás", "Bilineáris (eredeti)", "AMD CAS (élesítés)", "AMD FSR 1"}},
      {"chn",
       {{"鍵盤與滑鼠", "調整鍵盤與滑鼠設定。"},
        {{"校正亮度", "開啟亮度校正畫面。"},
         {"全螢幕", "以全螢幕或視窗模式進行遊戲。"},
         {"畫面更新率上限", "每秒最多的畫面數。30 與 Xbox 360 相同。"},
         {"反鋸齒", "讓鋸齒狀的邊緣更平滑。下次啟動遊戲時生效。"},
         {"動態模糊", "在快速移動時模糊畫面。下次啟動遊戲時生效。"},
         {"滑鼠靈敏度", "調整用滑鼠轉動鏡頭的速度。"},
         {"非等向性過濾", "讓斜角觀看的材質保持清晰。"},
         {"影像縮放", "影像縮放至視窗的方式。下次啟動遊戲時生效。"}},
        "關閉", "開啟", "FXAA", "FXAA 極致", "開啟", "雙線性（原始）", "AMD CAS（銳利化）", "AMD FSR 1"}},
      {"kor",
       {{"키보드 및 마우스", "키보드 및 마우스 설정을 조정합니다."},
        {{"밝기 보정", "밝기 보정 화면을 엽니다."},
         {"전체 화면", "전체 화면 또는 창 모드로 플레이합니다."},
         {"프레임 속도 제한", "초당 최대 프레임 수. 30은 Xbox 360과 같습니다."},
         {"안티에일리어싱", "계단 현상을 부드럽게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"모션 블러", "빠르게 움직일 때 화면을 흐리게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"마우스 감도", "마우스로 카메라를 돌리는 속도를 조정합니다."},
         {"비등방성 필터링", "비스듬히 보이는 텍스처를 선명하게 유지합니다."},
         {"이미지 스케일링", "화면을 창 크기에 맞게 조정하는 방식입니다. 다음에 게임을 시작할 때 적용됩니다."}},
        "끔", "켬", "FXAA", "FXAA 최대", "열기", "바이리니어 (원본)", "AMD CAS (선명화)", "AMD FSR 1"}},
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
// Language of the game's texts (of the menu patched last), for the value names of our own lists.
std::string g_menu_language = "int";
// Value shown for each option once read: settings that apply at the next start keep their old
// cvar value until then, so the menu shows what was chosen rather than what is in effect.
std::map<int, float> g_values;
// Value each option had when the game started (the first time it was read), to tell whether one
// that only applies at start-up has changed since.
std::map<int, float> g_started;
// Values of the start-up options the player was last asked about, so that answering "Later" is
// not asked again until something changes.
std::string g_prompted_for;

bool Valid(int option) {
  return option >= 0 && option < static_cast<int>(Options().size());
}

// Options that the runtime only reads when the game starts.
bool NeedsRestart(const Option& option) {
  return option.label == Label::kAntialiasing || option.label == Label::kUpscaling ||
         option.label == Label::kMotionBlur;
}

float ReadCvar(const Option& option) {
  if (!option.cvar) return 0.0f;
  const std::string value = rex::cvar::GetFlagByName(option.cvar);
  switch (option.kind) {
    case Kind::kAction:
      return 0.0f;
    case Kind::kToggle:
      return value == "true" || value == "1" ? 1.0f : 0.0f;
    case Kind::kChoice: {
      for (size_t i = 0; i < option.choices.size(); ++i) {
        if (value == option.choices[i]) return static_cast<float>(i);
      }
      // A value the list does not offer (set by hand, or by an older launcher): the nearest one
      // when the values are numbers, else the first.
      size_t nearest = 0;
      try {
        const double wanted = std::stod(value);
        double best = 1e300;
        for (size_t i = 0; i < option.choices.size(); ++i) {
          const double distance = std::abs(std::stod(option.choices[i]) - wanted);
          if (distance < best) {
            best = distance;
            nearest = i;
          }
        }
      } catch (...) {
        nearest = 0;
      }
      return static_cast<float>(nearest);
    }
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
      const std::string& choice = option.choices[index];
      const bool number = !choice.empty() && choice.find_first_not_of("0123456789.") == std::string::npos;
      return toml && !number ? std::string("\"") + choice + "\"" : choice;
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
  // A list without a definition of its own takes its value names from the script's special case
  // (bUsesRawValue), which script_hooks.cpp answers with PcOptionValueNames.
  if (option.kind == Kind::kChoice && option.mapping < 0) {
    std::string text = row.str();
    const std::string from = "bUsesRawValue=false";
    text.replace(text.find(from), from.size(), "bUsesRawValue=true");
    return text;
  }
  return row.str();
}

}  // namespace

bool PcOptionValueNames(int psid, std::vector<std::u16string>& names) {
  for (const Option& option : Options()) {
    if (option.psid != psid || option.kind != Kind::kChoice || option.mapping >= 0) continue;
    names.clear();
    if (option.label == Label::kFrameRateLimit) {
      // Xbox 360 language IDs (user_language).
      static const std::map<uint32_t, const char16_t*> unlimited = {
          {1, u"Unlimited"},  {2, u"無制限"},     {3, u"Unbegrenzt"},    {4, u"Illimité"},
          {5, u"Sin límite"}, {6, u"Illimitato"}, {7, u"무제한"},        {8, u"無限制"},
          {9, u"Ilimitado"},  {11, u"Bez limitu"}, {12, u"Без ограничения"}};
      const auto found = unlimited.find(REXCVAR_GET(user_language));
      names = {u"30", u"60", u"90", u"120", found != unlimited.end() ? found->second : u"Unlimited"};
    }
    if (option.label == Label::kUpscaling) {
      std::string language;
      {
        std::lock_guard lock(g_mutex);
        language = g_menu_language;
      }
      const Texts& texts = TextsFor(language);
      for (const char* effect : option.choices) {
        const std::string effect_name = effect;
        const char* name = effect_name == "cas"       ? texts.cas
                           : effect_name == "fsr"     ? texts.fsr
                           : effect_name == "metalfx" ? "Apple MetalFX"
                                                      : texts.bilinear;
        names.push_back(CoalescedString::FromUtf8(name).text);
      }
    }
    return true;
  }
  return false;
}

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
  if (found == g_values.end()) {
    found = g_values.emplace(option, ReadCvar(Options()[option])).first;
    g_started.emplace(option, found->second);
  }
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
    g_started.emplace(option, shown);
    if (FormatValue(entry, shown, false) == FormatValue(entry, value, false)) return;
    g_values[option] = value;
    config_path = g_config_path;
  }
  rex::cvar::SetFlagByName(entry.cvar, FormatValue(entry, value, false));
  REXLOG_INFO("[Azura options] {} = {}", entry.cvar, FormatValue(entry, value, false));
  if (entry.label == Label::kFrameRateLimit) ApplyFrameRateLimit();
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
    if (option.label == Label::kFrameRateLimit) ApplyFrameRateLimit();
  }
}

bool PcOptionsCategoryClosed(int category) {
  std::lock_guard lock(g_mutex);
  bool pending = false;
  std::string chosen;
  for (const auto& [option, value] : g_values) {
    const Option& entry = Options()[option];
    if (entry.category != category || !NeedsRestart(entry)) continue;
    const auto started = g_started.find(option);
    if (started != g_started.end() && FormatValue(entry, started->second, false) != FormatValue(entry, value, false)) {
      pending = true;
    }
    chosen += std::string(entry.cvar) + "=" + FormatValue(entry, value, false) + ";";
  }
  if (!pending || chosen == g_prompted_for) return false;
  g_prompted_for = chosen;
  return true;
}

void PatchMessageBoxTexts(CoalescedSection& section, const std::string& language) {
  // Title and question of the restart message box; its buttons are the game's Accept and Cancel.
  static const std::map<std::string, std::pair<const char*, const char*>> texts = {
      {"int", {"Restart required", "Some Video changes take effect when the game restarts. Restart now?"}},
      {"esn", {"Reinicio necesario", "Algunos cambios de Vídeo se aplican al reiniciar el juego. ¿Reiniciar ahora?"}},
      {"deu", {"Neustart erforderlich", "Einige Videoänderungen werden nach einem Neustart des Spiels wirksam. Jetzt neu starten?"}},
      {"fra", {"Redémarrage nécessaire", "Certaines modifications vidéo prennent effet au redémarrage du jeu. Redémarrer maintenant ?"}},
      {"ita", {"Riavvio necessario", "Alcune modifiche video hanno effetto al riavvio del gioco. Riavviare ora?"}},
      {"ptb", {"Reinício necessário", "Algumas alterações de vídeo são aplicadas ao reiniciar o jogo. Reiniciar agora?"}},
      {"pol", {"Wymagane ponowne uruchomienie", "Niektóre zmiany wideo zadziałają po ponownym uruchomieniu gry. Uruchomić ponownie teraz?"}},
      {"rus", {"Требуется перезапуск", "Некоторые изменения видео вступят в силу после перезапуска игры. Перезапустить сейчас?"}},
      {"cze", {"Je nutný restart", "Některé změny videa se projeví po restartu hry. Restartovat nyní?"}},
      {"hun", {"Újraindítás szükséges", "Néhány videóbeállítás a játék újraindítása után lép életbe. Újraindítod most?"}},
      {"jpn", {"再起動が必要です", "一部のビデオ設定はゲームの再起動後に適用されます。今すぐ再起動しますか？"}},
      {"kor", {"다시 시작 필요", "일부 비디오 설정은 게임을 다시 시작하면 적용됩니다. 지금 다시 시작하시겠습니까?"}},
      {"chn", {"需要重新啟動", "部分視訊設定會在重新啟動遊戲後生效。要立即重新啟動嗎？"}},
  };
  const auto found = texts.find(language == "esm" ? "esn" : language);
  const auto& [title, message] = found != texts.end() ? found->second : texts.at("int");
  SetKey(section, kRestartTitleKey, title);
  SetKey(section, kRestartMessageKey, message);
}

void PatchOptionsMenu(CoalescedSection& section, const std::string& language) {
  {
    std::lock_guard lock(g_mutex);
    g_menu_language = language;
  }
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
      case Kind::kChoice: {
        std::vector<std::string> names;
        if (option.label == Label::kAntialiasing) names = {texts.off, texts.fxaa, texts.fxaa_extreme};
        if (option.label == Label::kAnisotropic) names = {texts.off, "4×", "16×"};
        for (size_t i = 0; i < names.size(); ++i) {
          values += (i ? "," : "") + std::string("(Id=") + std::to_string(i) + ",Name=\"" + names[i] + "\")";
        }
        break;
      }
      case Kind::kSlider:
        break;
    }
    SetKey(section, "ProfileMappings[" + std::to_string(option.mapping) + "]", "(ValueMappings=(" + values + "))");
  }
}

void PatchSystemSettings(CoalescedSection& section) {
  SetKey(section, "MotionBlur", REXCVAR_GET(motion_blur) ? "True" : "False");
}

void PatchEngineSettings(CoalescedSection& section) {
  // The engine sleeps when a frame would go past MaxSmoothedFrameRate (62 in the game's ini). The
  // limit comes from the vertical blanks instead (ApplyFrameRateLimit), so the ceiling is raised
  // above any limit; at 30 the game still waits for its two vertical blanks per frame.
  SetKey(section, "MaxSmoothedFrameRate", "1000");
}

void ApplyFrameRateLimit() {
  const double limit = REXCVAR_GET(frame_rate_limit);
  if (limit >= kFrameRateUnlimited) {
    // Vertical blanks every millisecond: the game never waits for them.
    rex::cvar::SetFlagByName("vsync", "false");
    REXLOG_INFO("[Azura options] Frame rate not limited");
    return;
  }
  // The game presents a frame every two vertical blanks (30 FPS at the Xbox 360's 60 Hz), so the
  // guest sees twice as many vertical blanks per second as the frames it may draw.
  const int frames = static_cast<int>(std::lround(std::max(30.0, limit)));
  rex::cvar::SetFlagByName("vsync", "true");
  rex::cvar::SetFlagByName("guest_vblank_rate", std::to_string(frames * 2));
  REXLOG_INFO("[Azura options] Frame rate limited to {} ({} vertical blanks per second)", frames,
              frames * 2);
}

}  // namespace azura::menu
