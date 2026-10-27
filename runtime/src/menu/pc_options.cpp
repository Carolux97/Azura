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

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "coalesced.h"

REXCVAR_DEFINE_DOUBLE(frame_rate_limit, 30.0, "Azura/Graphics",
                      "Most frames per second: 30 as on the Xbox 360, 60, 90 or 120; 150 or more "
                      "removes the limit");
REXCVAR_DEFINE_BOOL(motion_blur, true, "Azura/Graphics",
                    "Motion blur (the game's SystemSettings.MotionBlur); read when the game starts")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(render_resolution, "auto", "Azura/Graphics",
                      "Resolution the game is drawn at: auto (chosen for this computer), 1280x720 (the "
                      "Xbox 360's), 2560x1440 or 3840x2160. Sets draw_resolution_scale_x/y and the "
                      "texture cache limits when the game starts")
    .allowed({"auto", "1280x720", "2560x1440", "3840x2160"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(upscaling_sharpness, 0.0, "Azura/Graphics",
                      "Extra sharpening of the image scaling effects that sharpen (AMD CAS, and AMD FSR 1 "
                      "with the FidelityFX SDK), from 0 (the effect's own) to 1 (the most). Sets "
                      "present_cas_additional_sharpness and present_fsr_sharpness_reduction")
    .range(0.0, 1.0);

REXCVAR_DEFINE_DOUBLE(field_of_view, 80.0, "Azura/Graphics",
                      "Horizontal field of view of the game's base camera, in degrees: 80 (the "
                      "original), 85, 90, 95 or 100. Every gameplay camera mode is scaled by "
                      "field_of_view / 80")
    .range(80.0, 100.0);
REXCVAR_DEFINE_BOOL(film_grain, true, "Azura/Graphics",
                    "The game's film grain (UberPostProcessEffect.bEnableImageGrain); applies while the "
                    "game runs");
REXCVAR_DEFINE_BOOL(depth_of_field, true, "Azura/Graphics",
                    "The game's depth of field (SystemSettings.DepthOfField); applies while the game "
                    "runs");
REXCVAR_DEFINE_BOOL(dynamic_shadows, true, "Azura/Graphics",
                    "The game's dynamic shadows (SystemSettings.DynamicShadows); applies while the game "
                    "runs");
// Off by default on Linux, where the game's ambient occlusion leaves black patches on surfaces.
#if defined(__linux__)
constexpr bool kAmbientOcclusionDefault = false;
#else
constexpr bool kAmbientOcclusionDefault = true;
#endif
REXCVAR_DEFINE_BOOL(ambient_occlusion, kAmbientOcclusionDefault, "Azura/Graphics",
                    "The game's ambient occlusion (SystemSettings.AmbientOcclusion); applies while the "
                    "game runs. Off by default on Linux");
REXCVAR_DEFINE_BOOL(bloom, true, "Azura/Graphics",
                    "The game's bloom (SystemSettings.Bloom); applies while the game runs");
REXCVAR_DEFINE_BOOL(lens_flares, true, "Azura/Graphics",
                    "The game's lens flares (SystemSettings.LensFlares); applies while the game runs");

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
  kRenderResolution,
  kSharpness,
  kLocalCoop,
  kPerformanceOverlay,
  kPerformanceOverlayPosition,
  kFieldOfView,
  kFilmGrain,
  kDepthOfField,
  kDynamicShadows,
  kAmbientOcclusion,
  kBloom,
  kLensFlares,
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

// The upscaling effects present_effect accepts in this build of the SDK, in menu order. Others it
// may accept (the experimental temporal FSR) are not offered. The menu shows the ones the running
// presenter draws (ChoicesOf).
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
      // Applies while the game runs: the camera modes read it each frame (script_hooks.cpp, "Field
      // of view"). A list of our own (ID 9112) rather than a slider, because the menu's slider only
      // shows the step it is on, and the player should see the degrees.
      {kVideoCategory, 3, Label::kFieldOfView, "field_of_view", Kind::kChoice, 9112, -1,
       {"80", "85", "90", "95", "100"}},
      {kVideoCategory, 4, Label::kAntialiasing, "swap_post_effect", Kind::kChoice, 20, 13,
       {"none", "fxaa", "fxaa_extreme"}},
      {kVideoCategory, 5, Label::kMotionBlur, "motion_blur", Kind::kToggle, 21, 14, {}},
      // Applies while the game runs (script_hooks.cpp, "Film grain"). A list of our own (ID 9113):
      // a toggle would need a definition of a profile setting the game does not use, and none is
      // left with two values.
      {kVideoCategory, 6, Label::kFilmGrain, "film_grain", Kind::kChoice, 9113, -1, {"false", "true"}},
      // Apply while the game runs, as the engine's settings object is written every frame
      // (system_settings.cpp). Lists of our own (IDs 9114 and 9115), as film grain's; these two
      // save the most GPU time.
      {kVideoCategory, 7, Label::kDepthOfField, "depth_of_field", Kind::kChoice, 9114, -1, {"false", "true"}},
      {kVideoCategory, 8, Label::kDynamicShadows, "dynamic_shadows", Kind::kChoice, 9115, -1, {"false", "true"}},
      // The same for the effects that change the look more than the frame rate (IDs 9116 to 9118).
      {kVideoCategory, 9, Label::kAmbientOcclusion, "ambient_occlusion", Kind::kChoice, 9116, -1, {"false", "true"}},
      {kVideoCategory, 10, Label::kBloom, "bloom", Kind::kChoice, 9117, -1, {"false", "true"}},
      {kVideoCategory, 11, Label::kLensFlares, "lens_flares", Kind::kChoice, 9118, -1, {"false", "true"}},
      // Generic Xbox profile settings with three values that Gears never reads: Controller
      // Sensitivity (it has its own, 368, 59 and 60) and Voice Via Speakers.
      {kVideoCategory, 12, Label::kAnisotropic, "anisotropic_override", Kind::kChoice, 13, 2, {"0", "3", "5"}},
      // Lists of our own too: the resolution's value names include "Automatic" in the player's
      // language, and the upscaling values depend on the platform (UpscalingChoices).
      {kVideoCategory, 13, Label::kRenderResolution, "render_resolution", Kind::kChoice, 9108, -1,
       {"auto", "1280x720", "2560x1440", "3840x2160"}},
      // Right above the sharpness row, which belongs to it.
      {kVideoCategory, 14, Label::kUpscaling, "present_effect", Kind::kChoice, 9107, -1, UpscalingChoices()},
      // Applies while the game runs, so the player sees the sharpening change (ApplyUpscalingSharpness).
      // Only shown with an image scaling effect that sharpens; the rows below it then move up one.
      {kVideoCategory, 15, Label::kSharpness, "upscaling_sharpness", Kind::kSlider, 9109, -1, {}, 0.0f, 1.0f,
       0.1f},
      // A list of our own (ID 9110): its values grow as the overlay gets more modes. Last in the
      // list, with its position below it.
      {kVideoCategory, 16, Label::kPerformanceOverlay, "performance_overlay", Kind::kChoice, 9110, -1,
       {"off", "fps", "detailed", "advanced"}},
      // Our own list too (ID 9111): the corner the overlay is shown in.
      {kVideoCategory, 17, Label::kPerformanceOverlayPosition, "performance_overlay_position", Kind::kChoice, 9111, -1,
       {"top_left", "top_right", "bottom_left", "bottom_right"}},
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
  const char* automatic;  // the render resolution chosen for this computer
  const char* corners[4];  // the performance overlay's positions: top left, top right, bottom left, bottom right
  const char* detailed;    // the performance overlay's mode with the CPU and GPU use
  const char* advanced;    // the performance overlay's mode with memory use, frame time graph and 1% low
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
         {"IMAGE SCALING", "How the image is scaled to the window. Takes effect the next time the game starts."},
         {"RENDER RESOLUTION",
          "Resolution the game is drawn at, before image scaling. Automatic chooses one for this computer. Takes effect the next time the game starts."},
         {"SHARPNESS",
          "How much extra sharpening the image scaling adds. 0 keeps its own."},
         {"LOCAL CO-OP", "Choose which controller or keyboard each player uses, to play the campaign together on this computer."},
         {"PERFORMANCE OVERLAY", "Show the game's frame rate in a corner of the screen."},
         {"OVERLAY POSITION", "Corner of the screen the performance overlay is shown in."},
         {"FIELD OF VIEW", "How wide the camera sees. 80 is the original; aiming and cover widen or narrow with it."},
         {"FILM GRAIN", "The film grain noise the game adds to the image."},
         {"DEPTH OF FIELD", "The blur of what is out of focus. Turn it off to make the game run faster."},
         {"DYNAMIC SHADOWS", "The shadows of characters and moving objects. Turn them off to make the game run faster."},
         {"AMBIENT OCCLUSION", "The soft shadows in corners and where objects meet."},
         {"BLOOM", "The glow around bright lights."},
         {"LENS FLARES", "The flares bright lights draw over the image."}},
        "Off", "On", "FXAA", "FXAA extreme", "Open", "Bilinear (original)", "AMD CAS (sharpening)", "AMD FSR 1", "Automatic", {"Top left", "Top right", "Bottom left", "Bottom right"}, "Detailed", "Advanced"}},
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
         {"ESCALADO DE IMAGEN", "Cómo se escala la imagen a la ventana. Se aplica la próxima vez que inicie el juego."},
         {"RESOLUCIÓN DE RENDERIZADO",
          "Resolución a la que se dibuja el juego, antes del escalado de imagen. Automática elige una para este equipo. Se aplica la próxima vez que inicie el juego."},
         {"NITIDEZ",
          "Cuánto enfoque añade el escalado de imagen. 0 deja el suyo propio."},
         {"COOPERATIVO LOCAL", "Elija qué mando o teclado usa cada jugador para jugar la campaña juntos en este equipo."},
         {"INDICADOR DE RENDIMIENTO", "Muestre los fotogramas por segundo del juego en una esquina de la pantalla."},
         {"POSICIÓN DEL INDICADOR", "Esquina de la pantalla en la que se muestra el indicador de rendimiento."},
         {"CAMPO DE VISIÓN", "Amplitud de la vista de la cámara. 80 es el original; apuntar y cubrirse cambian en la misma proporción."},
         {"GRANO DE PELÍCULA", "El ruido de grano de película que el juego añade a la imagen."},
         {"PROFUNDIDAD DE CAMPO", "El desenfoque de lo que queda fuera de foco. Desactívela para que el juego vaya más rápido."},
         {"SOMBRAS DINÁMICAS", "Las sombras de los personajes y de los objetos que se mueven. Desactívelas para que el juego vaya más rápido."},
         {"OCLUSIÓN AMBIENTAL", "Las sombras suaves en las esquinas y donde se juntan los objetos."},
         {"RESPLANDOR", "El halo que rodea las luces intensas."},
         {"DESTELLOS DE LENTE", "Los destellos que las luces intensas dibujan sobre la imagen."}},
        "Desactivado", "Activado", "FXAA", "FXAA extremo", "Abrir", "Bilineal (original)", "AMD CAS (nitidez)", "AMD FSR 1", "Automática", {"Arriba a la izquierda", "Arriba a la derecha", "Abajo a la izquierda", "Abajo a la derecha"}, "Detallado", "Avanzado"}},
      {"deu",
       {{"TASTATUR UND MAUS", "Passen Sie die Einstellungen für Tastatur und Maus an."},
        {{"HELLIGKEIT KALIBRIEREN", "Öffnet den Bildschirm zur Kalibrierung der Helligkeit."},
         {"VOLLBILD", "Im Vollbild oder in einem Fenster spielen."},
         {"BILDRATENBEGRENZUNG", "Höchstzahl der Bilder pro Sekunde. 30 entspricht der Xbox 360."},
         {"KANTENGLÄTTUNG", "Glättet gezackte Kanten. Wird beim nächsten Spielstart wirksam."},
         {"BEWEGUNGSUNSCHÄRFE", "Verwischt das Bild bei schnellen Bewegungen. Wird beim nächsten Spielstart wirksam."},
         {"MAUSEMPFINDLICHKEIT", "Legt fest, wie schnell sich die Kamera mit der Maus dreht."},
         {"ANISOTROPE FILTERUNG", "Hält schräg betrachtete Texturen scharf."},
         {"BILDSKALIERUNG", "Wie das Bild auf das Fenster skaliert wird. Wird beim nächsten Spielstart wirksam."},
         {"RENDERAUFLÖSUNG",
          "Auflösung, in der das Spiel vor der Bildskalierung gezeichnet wird. Automatisch wählt eine für diesen Computer. Wird beim nächsten Spielstart wirksam."},
         {"SCHÄRFE",
          "Wie viel zusätzliche Schärfe die Bildskalierung hinzufügt. 0 behält ihre eigene."},
         {"LOKALER KOOP", "Wählen Sie, welchen Controller oder welche Tastatur jeder Spieler verwendet, um die Kampagne gemeinsam an diesem Computer zu spielen."},
         {"LEISTUNGSANZEIGE", "Zeigt die Bildrate des Spiels in einer Ecke des Bildschirms an."},
         {"POSITION DER ANZEIGE", "Bildschirmecke, in der die Leistungsanzeige erscheint."},
         {"SICHTFELD", "Wie weit die Kamera sieht. 80 ist der ursprüngliche Wert; Zielen und Deckung ändern sich im gleichen Verhältnis."},
         {"FILMKORN", "Das Filmkorn, das das Spiel dem Bild hinzufügt."},
         {"TIEFENSCHÄRFE", "Die Unschärfe außerhalb des Fokus. Ausgeschaltet läuft das Spiel schneller."},
         {"DYNAMISCHE SCHATTEN", "Die Schatten von Figuren und bewegten Objekten. Ausgeschaltet läuft das Spiel schneller."},
         {"UMGEBUNGSVERDECKUNG", "Die weichen Schatten in Ecken und dort, wo Objekte aufeinandertreffen."},
         {"BLOOM", "Das Leuchten um helle Lichter."},
         {"LINSENREFLEXE", "Die Reflexe, die helle Lichter über das Bild legen."}},
        "Aus", "Ein", "FXAA", "FXAA extrem", "Öffnen", "Bilinear (Original)", "AMD CAS (Schärfung)", "AMD FSR 1", "Automatisch", {"Oben links", "Oben rechts", "Unten links", "Unten rechts"}, "Detailliert", "Erweitert"}},
      {"ita",
       {{"TASTIERA E MOUSE", "Regola le impostazioni di tastiera e mouse."},
        {{"CALIBRA LUMINOSITÀ", "Apri la schermata di calibrazione della luminosità."},
         {"SCHERMO INTERO", "Gioca a schermo intero o in una finestra."},
         {"LIMITE FOTOGRAMMI", "Numero massimo di fotogrammi al secondo. 30 è quello di Xbox 360."},
         {"ANTI-ALIASING", "Smussa i bordi frastagliati. Ha effetto al prossimo avvio del gioco."},
         {"SFOCATURA DI MOVIMENTO", "Sfoca l'immagine durante i movimenti rapidi. Ha effetto al prossimo avvio del gioco."},
         {"SENSIBILITÀ DEL MOUSE", "Regola la velocità con cui la visuale ruota con il mouse."},
         {"FILTRO ANISOTROPICO", "Mantiene nitide le texture viste di sbieco."},
         {"RIDIMENSIONAMENTO IMMAGINE", "Come l'immagine viene scalata nella finestra. Ha effetto al prossimo avvio del gioco."},
         {"RISOLUZIONE DI RENDERING",
          "Risoluzione a cui viene disegnato il gioco, prima del ridimensionamento. Automatica ne sceglie una per questo computer. Ha effetto al prossimo avvio del gioco."},
         {"NITIDEZZA",
          "Quanta nitidezza aggiuntiva applica il ridimensionamento dell'immagine. 0 mantiene la sua."},
         {"COOPERATIVA LOCALE", "Scegli il controller o la tastiera di ogni giocatore per giocare insieme la campagna su questo computer."},
         {"INDICATORE PRESTAZIONI", "Mostra la frequenza dei fotogrammi del gioco in un angolo dello schermo."},
         {"POSIZIONE INDICATORE", "Angolo dello schermo in cui compare l'indicatore delle prestazioni."},
         {"CAMPO VISIVO", "Ampiezza della visuale. 80 è il valore originale; mira e copertura cambiano nella stessa proporzione."},
         {"GRANA PELLICOLA", "La grana pellicola che il gioco aggiunge all'immagine."},
         {"PROFONDITÀ DI CAMPO", "La sfocatura di ciò che è fuori fuoco. Disattivala per rendere il gioco più veloce."},
         {"OMBRE DINAMICHE", "Le ombre dei personaggi e degli oggetti in movimento. Disattivale per rendere il gioco più veloce."},
         {"OCCLUSIONE AMBIENTALE", "Le ombre morbide negli angoli e dove gli oggetti si incontrano."},
         {"BLOOM", "Il bagliore attorno alle luci intense."},
         {"RIFLESSI DELLA LENTE", "I riflessi che le luci intense disegnano sull'immagine."}},
        "Disattivato", "Attivato", "FXAA", "FXAA estremo", "Apri", "Bilineare (originale)", "AMD CAS (nitidezza)", "AMD FSR 1", "Automatica", {"In alto a sinistra", "In alto a destra", "In basso a sinistra", "In basso a destra"}, "Dettagliato", "Avanzato"}},
      {"jpn",
       {{"キーボードとマウス", "キーボードとマウスの設定を調整します。"},
        {{"明るさの調整", "明るさの調整画面を開きます。"},
         {"フルスクリーン", "フルスクリーンまたはウィンドウでプレイします。"},
         {"フレームレート上限", "1秒あたりの最大フレーム数。Xbox 360は30です。"},
         {"アンチエイリアス", "ギザギザした輪郭を滑らかにします。次回ゲーム起動時に適用されます。"},
         {"モーションブラー", "素早い動きの際に映像をぼかします。次回ゲーム起動時に適用されます。"},
         {"マウス感度", "マウスでカメラを回転させる速さを調整します。"},
         {"異方性フィルタリング", "斜めから見たテクスチャを鮮明に保ちます。"},
         {"画像スケーリング", "映像をウィンドウに合わせて拡大する方法です。次回ゲーム起動時に適用されます。"},
         {"レンダリング解像度",
          "画像スケーリングの前にゲームを描画する解像度です。自動ではこのコンピューターに合わせて選びます。次回ゲーム起動時に適用されます。"},
         {"シャープネス",
          "画像スケーリングに加えるシャープ化の強さです。0 は標準のままです。"},
         {"ローカル協力プレイ", "このコンピューターでキャンペーンを一緒にプレイするため、各プレイヤーが使うコントローラーまたはキーボードを選びます。"},
         {"パフォーマンス表示", "ゲームのフレームレートを画面の隅に表示します。"},
         {"表示位置", "パフォーマンス表示を出す画面の隅。"},
         {"視野角", "カメラの視野の広さです。80 がオリジナルです。エイムやカバーも同じ割合で変わります。"},
         {"フィルムグレイン", "ゲームが映像に加えるフィルムグレインのノイズです。"},
         {"被写界深度", "ピントの合っていない部分のぼかしです。オフにするとゲームが軽くなります。"},
         {"動的な影", "キャラクターや動く物体の影です。オフにするとゲームが軽くなります。"},
         {"アンビエントオクルージョン", "隅や物が接する部分の柔らかい影です。"},
         {"ブルーム", "明るい光のまわりの輝きです。"},
         {"レンズフレア", "明るい光が画面に描くフレアです。"}},
        "オフ", "オン", "FXAA", "FXAA (最大)", "開く", "バイリニア（オリジナル）", "AMD CAS（シャープ化）", "AMD FSR 1", "自動", {"左上", "右上", "左下", "右下"}, "詳細", "高度"}},
      {"fra",
       {{"CLAVIER ET SOURIS", "Réglez les options du clavier et de la souris."},
        {{"CALIBRER LA LUMINOSITÉ", "Ouvrez l'écran de calibrage de la luminosité."},
         {"PLEIN ÉCRAN", "Jouez en plein écran ou dans une fenêtre."},
         {"LIMITE D'IMAGES PAR SECONDE", "Nombre maximal d'images par seconde. 30 correspond à la Xbox 360."},
         {"ANTICRÉNELAGE", "Lissez les bords crénelés. Prend effet au prochain lancement du jeu."},
         {"FLOU DE MOUVEMENT", "Floutez l'image lors des mouvements rapides. Prend effet au prochain lancement du jeu."},
         {"SENSIBILITÉ DE LA SOURIS", "Réglez la vitesse de rotation de la caméra avec la souris."},
         {"FILTRAGE ANISOTROPE", "Gardez les textures nettes vues en biais."},
         {"MISE À L'ÉCHELLE", "Mise à l'échelle de l'image dans la fenêtre. Prend effet au prochain lancement du jeu."},
         {"RÉSOLUTION DE RENDU",
          "Résolution à laquelle le jeu est dessiné, avant la mise à l'échelle. Automatique en choisit une pour cet ordinateur. Prend effet au prochain lancement du jeu."},
         {"NETTETÉ",
          "Netteté supplémentaire ajoutée par la mise à l'échelle de l'image. 0 conserve la sienne."},
         {"COOPÉRATION LOCALE", "Choisissez la manette ou le clavier de chaque joueur pour jouer la campagne ensemble sur cet ordinateur."},
         {"INDICATEUR DE PERFORMANCES", "Affiche la fréquence d'images du jeu dans un coin de l'écran."},
         {"POSITION DE L'INDICATEUR", "Coin de l'écran où s'affiche l'indicateur de performances."},
         {"CHAMP DE VISION", "Largeur de la vue de la caméra. 80 est la valeur d'origine ; la visée et le couvert changent dans la même proportion."},
         {"GRAIN DE FILM", "Le grain de film que le jeu ajoute à l'image."},
         {"PROFONDEUR DE CHAMP", "Le flou de ce qui est hors de la mise au point. Désactivez-la pour que le jeu soit plus rapide."},
         {"OMBRES DYNAMIQUES", "Les ombres des personnages et des objets en mouvement. Désactivez-les pour que le jeu soit plus rapide."},
         {"OCCLUSION AMBIANTE", "Les ombres douces dans les coins et là où les objets se touchent."},
         {"BLOOM", "Le halo autour des lumières vives."},
         {"REFLETS DE LENTILLE", "Les reflets que les lumières vives dessinent sur l'image."}},
        "Désactivé", "Activé", "FXAA", "FXAA extrême", "Ouvrir", "Bilinéaire (d'origine)", "AMD CAS (netteté)", "AMD FSR 1", "Automatique", {"En haut à gauche", "En haut à droite", "En bas à gauche", "En bas à droite"}, "Détaillé", "Avancé"}},
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
         {"ESCALA DA IMAGEM", "Como a imagem é dimensionada para a janela. Aplica-se na próxima vez que o jogo for iniciado."},
         {"RESOLUÇÃO DE RENDERIZAÇÃO",
          "Resolução em que o jogo é desenhado, antes da escala da imagem. Automática escolhe uma para este computador. Aplica-se na próxima vez que o jogo for iniciado."},
         {"NITIDEZ",
          "Quanta nitidez extra a escala da imagem adiciona. 0 mantém a dela."},
         {"COOPERATIVO LOCAL", "Escolha o controle ou o teclado de cada jogador para jogar a campanha juntos neste computador."},
         {"INDICADOR DE DESEMPENHO", "Mostra a taxa de quadros do jogo em um canto da tela."},
         {"POSIÇÃO DO INDICADOR", "Canto da tela em que o indicador de desempenho aparece."},
         {"CAMPO DE VISÃO", "Amplitude da visão da câmera. 80 é o original; mirar e se proteger mudam na mesma proporção."},
         {"GRANULAÇÃO DE FILME", "A granulação de filme que o jogo adiciona à imagem."},
         {"PROFUNDIDADE DE CAMPO", "O desfoque do que está fora de foco. Desative para o jogo ficar mais rápido."},
         {"SOMBRAS DINÂMICAS", "As sombras dos personagens e dos objetos em movimento. Desative para o jogo ficar mais rápido."},
         {"OCLUSÃO DE AMBIENTE", "As sombras suaves nos cantos e onde os objetos se encontram."},
         {"BLOOM", "O brilho em volta das luzes fortes."},
         {"REFLEXOS DE LENTE", "Os reflexos que as luzes fortes desenham sobre a imagem."}},
        "Desativado", "Ativado", "FXAA", "FXAA extremo", "Abrir", "Bilinear (original)", "AMD CAS (nitidez)", "AMD FSR 1", "Automática", {"Superior esquerdo", "Superior direito", "Inferior esquerdo", "Inferior direito"}, "Detalhado", "Avançado"}},
      {"pol",
       {{"KLAWIATURA I MYSZ", "Dostosuj ustawienia klawiatury i myszy."},
        {{"KALIBRACJA JASNOŚCI", "Otwórz ekran kalibracji jasności."},
         {"PEŁNY EKRAN", "Graj na pełnym ekranie lub w oknie."},
         {"LIMIT KLATEK", "Maksymalna liczba klatek na sekundę. 30 to wartość z Xbox 360."},
         {"WYGŁADZANIE KRAWĘDZI", "Wygładza postrzępione krawędzie. Zadziała po ponownym uruchomieniu gry."},
         {"ROZMYCIE RUCHU", "Rozmywa obraz podczas szybkiego ruchu. Zadziała po ponownym uruchomieniu gry."},
         {"CZUŁOŚĆ MYSZY", "Dostosuj szybkość obrotu kamery myszą."},
         {"FILTROWANIE ANIZOTROPOWE", "Zachowuje ostrość tekstur oglądanych pod kątem."},
         {"SKALOWANIE OBRAZU", "Sposób skalowania obrazu do okna. Zadziała po ponownym uruchomieniu gry."},
         {"ROZDZIELCZOŚĆ RENDEROWANIA",
          "Rozdzielczość, w jakiej rysowana jest gra, przed skalowaniem obrazu. Automatyczna dobiera ją do tego komputera. Zadziała po ponownym uruchomieniu gry."},
         {"OSTROŚĆ",
          "Ile dodatkowej ostrości dodaje skalowanie obrazu. 0 zachowuje domyślną."},
         {"LOKALNA KOOPERACJA", "Wybierz kontroler lub klawiaturę każdego gracza, aby grać razem w kampanię na tym komputerze."},
         {"WSKAŹNIK WYDAJNOŚCI", "Wyświetla liczbę klatek na sekundę gry w rogu ekranu."},
         {"POŁOŻENIE WSKAŹNIKA", "Róg ekranu, w którym wyświetla się wskaźnik wydajności."},
         {"POLE WIDZENIA", "Szerokość widoku kamery. 80 to wartość oryginalna; celowanie i osłona zmieniają się w tej samej proporcji."},
         {"ZIARNO FILMOWE", "Ziarno filmowe, które gra dodaje do obrazu."},
         {"GŁĘBIA OSTROŚCI", "Rozmycie tego, co jest poza ostrością. Wyłącz, aby gra działała szybciej."},
         {"DYNAMICZNE CIENIE", "Cienie postaci i ruchomych obiektów. Wyłącz, aby gra działała szybciej."},
         {"OKLUZJA OTOCZENIA", "Miękkie cienie w narożnikach i tam, gdzie stykają się obiekty."},
         {"POŚWIATA", "Blask wokół jasnych świateł."},
         {"ODBLASKI SOCZEWKI", "Odblaski, które jasne światła rysują na obrazie."}},
        "Wyłączone", "Włączone", "FXAA", "FXAA ekstremalne", "Otwórz", "Dwuliniowe (oryginalne)", "AMD CAS (wyostrzanie)", "AMD FSR 1", "Automatyczna", {"Lewy górny róg", "Prawy górny róg", "Lewy dolny róg", "Prawy dolny róg"}, "Szczegółowy", "Zaawansowany"}},
      {"rus",
       {{"КЛАВИАТУРА И МЫШЬ", "Настройте параметры клавиатуры и мыши."},
        {{"КАЛИБРОВКА ЯРКОСТИ", "Откройте экран калибровки яркости."},
         {"ПОЛНОЭКРАННЫЙ РЕЖИМ", "Играйте в полноэкранном режиме или в окне."},
         {"ОГРАНИЧЕНИЕ ЧАСТОТЫ КАДРОВ", "Максимальное число кадров в секунду. 30, как на Xbox 360."},
         {"СГЛАЖИВАНИЕ", "Сглаживает неровные края. Вступит в силу при следующем запуске игры."},
         {"РАЗМЫТИЕ В ДВИЖЕНИИ", "Размывает изображение при быстром движении. Вступит в силу при следующем запуске игры."},
         {"ЧУВСТВИТЕЛЬНОСТЬ МЫШИ", "Настройте скорость поворота камеры мышью."},
         {"АНИЗОТРОПНАЯ ФИЛЬТРАЦИЯ", "Сохраняет чёткость текстур, видимых под углом."},
         {"МАСШТАБИРОВАНИЕ ИЗОБРАЖЕНИЯ", "Способ масштабирования изображения под окно. Вступит в силу при следующем запуске игры."},
         {"РАЗРЕШЕНИЕ РЕНДЕРИНГА",
          "Разрешение, в котором отрисовывается игра, до масштабирования изображения. «Автоматически» подбирает его для этого компьютера. Вступит в силу при следующем запуске игры."},
         {"РЕЗКОСТЬ",
          "Сколько дополнительной резкости добавляет масштабирование изображения. 0 — без изменений."},
         {"ЛОКАЛЬНЫЙ КООПЕРАТИВ", "Выберите контроллер или клавиатуру для каждого игрока, чтобы пройти кампанию вместе на этом компьютере."},
         {"ПОКАЗАТЕЛЬ ПРОИЗВОДИТЕЛЬНОСТИ", "Показывает частоту кадров игры в углу экрана."},
         {"ПОЛОЖЕНИЕ ПОКАЗАТЕЛЯ", "Угол экрана, в котором показывается показатель производительности."},
         {"ПОЛЕ ЗРЕНИЯ", "Ширина обзора камеры. 80 — исходное значение; прицеливание и укрытие меняются в той же пропорции."},
         {"ЗЕРНИСТОСТЬ ПЛЁНКИ", "Эффект зернистости плёнки, который игра добавляет к изображению."},
         {"ГЛУБИНА РЕЗКОСТИ", "Размытие того, что находится не в фокусе. Выключите, чтобы игра работала быстрее."},
         {"ДИНАМИЧЕСКИЕ ТЕНИ", "Тени персонажей и движущихся объектов. Выключите, чтобы игра работала быстрее."},
         {"ФОНОВОЕ ЗАТЕНЕНИЕ", "Мягкие тени в углах и там, где соприкасаются предметы."},
         {"СВЕЧЕНИЕ", "Ореол вокруг ярких источников света."},
         {"БЛИКИ ОБЪЕКТИВА", "Блики, которые яркий свет рисует поверх изображения."}},
        "Выкл.", "Вкл.", "FXAA", "FXAA (макс.)", "Открыть", "Билинейное (оригинал)", "AMD CAS (резкость)", "AMD FSR 1", "Автоматически", {"Сверху слева", "Сверху справа", "Снизу слева", "Снизу справа"}, "Подробно", "Расширенный"}},
      {"cze",
       {{"KLÁVESNICE A MYŠ", "Upravte nastavení klávesnice a myši."},
        {{"KALIBRACE JASU", "Otevřete obrazovku kalibrace jasu."},
         {"CELÁ OBRAZOVKA", "Hrajte na celé obrazovce nebo v okně."},
         {"OMEZENÍ SNÍMKŮ", "Nejvyšší počet snímků za sekundu. 30 odpovídá Xbox 360."},
         {"VYHLAZOVÁNÍ HRAN", "Vyhlazuje zubaté hrany. Projeví se při příštím spuštění hry."},
         {"ROZMAZÁNÍ POHYBU", "Rozmazává obraz při rychlém pohybu. Projeví se při příštím spuštění hry."},
         {"CITLIVOST MYŠI", "Upravte rychlost otáčení kamery myší."},
         {"ANIZOTROPNÍ FILTROVÁNÍ", "Udržuje ostré textury viděné pod úhlem."},
         {"ŠKÁLOVÁNÍ OBRAZU", "Způsob škálování obrazu do okna. Projeví se při příštím spuštění hry."},
         {"ROZLIŠENÍ VYKRESLOVÁNÍ",
          "Rozlišení, ve kterém se hra vykresluje, před škálováním obrazu. Automaticky zvolí rozlišení pro tento počítač. Projeví se při příštím spuštění hry."},
         {"OSTROST",
          "Kolik dalšího zostření přidá škálování obrazu. 0 ponechá výchozí."},
         {"MÍSTNÍ KOOPERACE", "Vyberte ovladač nebo klávesnici každého hráče a hrajte kampaň společně na tomto počítači."},
         {"UKAZATEL VÝKONU", "Zobrazí snímkovou frekvenci hry v rohu obrazovky."},
         {"UMÍSTĚNÍ UKAZATELE", "Roh obrazovky, ve kterém se zobrazuje ukazatel výkonu."},
         {"ZORNÉ POLE", "Šířka záběru kamery. 80 je původní hodnota; míření a kryt se mění ve stejném poměru."},
         {"FILMOVÉ ZRNO", "Filmové zrno, které hra přidává do obrazu."},
         {"HLOUBKA OSTROSTI", "Rozmazání toho, co je mimo ostrost. Vypněte ji, aby hra běžela rychleji."},
         {"DYNAMICKÉ STÍNY", "Stíny postav a pohyblivých objektů. Vypněte je, aby hra běžela rychleji."},
         {"OKOLNÍ ZASTÍNĚNÍ", "Měkké stíny v rozích a tam, kde se objekty stýkají."},
         {"ZÁŘE", "Záře kolem jasných světel."},
         {"ODLESKY OBJEKTIVU", "Odlesky, které jasná světla kreslí přes obraz."}},
        "Vypnuto", "Zapnuto", "FXAA", "FXAA extrémní", "Otevřít", "Bilineární (původní)", "AMD CAS (zostření)", "AMD FSR 1", "Automaticky", {"Vlevo nahoře", "Vpravo nahoře", "Vlevo dole", "Vpravo dole"}, "Podrobný", "Pokročilý"}},
      {"hun",
       {{"BILLENTYŰZET ÉS EGÉR", "A billentyűzet és az egér beállításai."},
        {{"FÉNYERŐ KALIBRÁLÁSA", "Megnyitja a fényerő kalibrálási képernyőjét."},
         {"TELJES KÉPERNYŐ", "Játék teljes képernyőn vagy ablakban."},
         {"KÉPKOCKASZÁM-KORLÁT", "Másodpercenkénti képkockák legnagyobb száma. A 30 az Xbox 360 értéke."},
         {"ÉLSIMÍTÁS", "Kisimítja a recés éleket. A játék következő indításakor lép életbe."},
         {"MOZGÁSI ELMOSÓDÁS", "Elmossa a képet gyors mozgáskor. A játék következő indításakor lép életbe."},
         {"EGÉRÉRZÉKENYSÉG", "Az egérrel történő kameraforgatás sebessége."},
         {"ANIZOTRÓP SZŰRÉS", "Élesen tartja a ferdén látott textúrákat."},
         {"KÉPSKÁLÁZÁS", "A kép ablakhoz méretezésének módja. A játék következő indításakor lép életbe."},
         {"RENDERELÉSI FELBONTÁS",
          "A játék rajzolási felbontása a képskálázás előtt. Az Automatikus ehhez a számítógéphez választ. A játék következő indításakor lép életbe."},
         {"ÉLESSÉG",
          "Mennyi további élesítést ad a képskálázás. A 0 a sajátját hagyja meg."},
         {"HELYI KOOPERATÍV", "Válaszd ki, melyik kontrollert vagy billentyűzetet használják az egyes játékosok, hogy együtt játsszatok a kampányban ezen a számítógépen."},
         {"TELJESÍTMÉNYJELZŐ", "Megjeleníti a játék képkockasebességét a képernyő sarkában."},
         {"JELZŐ HELYE", "A képernyő sarka, ahol a teljesítményjelző megjelenik."},
         {"LÁTÓMEZŐ", "A kamera látószögének szélessége. A 80 az eredeti; a célzás és a fedezék ugyanilyen arányban változik."},
         {"FILMSZEMCSÉZET", "A filmszemcsés zaj, amelyet a játék a képhez ad."},
         {"MÉLYSÉGÉLESSÉG", "A fókuszon kívüli részek elmosása. Kapcsold ki, hogy gyorsabban fusson a játék."},
         {"DINAMIKUS ÁRNYÉKOK", "A szereplők és a mozgó tárgyak árnyékai. Kapcsold ki őket, hogy gyorsabban fusson a játék."},
         {"KÖRNYEZETI TAKARÁS", "A lágy árnyékok a sarkokban és ott, ahol a tárgyak összeérnek."},
         {"RAGYOGÁS", "Az erős fények körüli fényudvar."},
         {"LENCSECSILLANÁS", "A csillanások, amelyeket az erős fények a képre rajzolnak."}},
        "Ki", "Be", "FXAA", "FXAA (extrém)", "Megnyitás", "Bilineáris (eredeti)", "AMD CAS (élesítés)", "AMD FSR 1", "Automatikus", {"Bal felső sarok", "Jobb felső sarok", "Bal alsó sarok", "Jobb alsó sarok"}, "Részletes", "Haladó"}},
      {"chn",
       {{"鍵盤與滑鼠", "調整鍵盤與滑鼠設定。"},
        {{"校正亮度", "開啟亮度校正畫面。"},
         {"全螢幕", "以全螢幕或視窗模式進行遊戲。"},
         {"畫面更新率上限", "每秒最多的畫面數。30 與 Xbox 360 相同。"},
         {"反鋸齒", "讓鋸齒狀的邊緣更平滑。下次啟動遊戲時生效。"},
         {"動態模糊", "在快速移動時模糊畫面。下次啟動遊戲時生效。"},
         {"滑鼠靈敏度", "調整用滑鼠轉動鏡頭的速度。"},
         {"非等向性過濾", "讓斜角觀看的材質保持清晰。"},
         {"影像縮放", "影像縮放至視窗的方式。下次啟動遊戲時生效。"},
         {"渲染解析度",
          "遊戲在影像縮放前的繪製解析度。自動會為這台電腦選擇。下次啟動遊戲時生效。"},
         {"銳利度",
          "影像縮放額外增加的銳利度。0 為保持原樣。"},
         {"本機合作", "選擇每位玩家使用的控制器或鍵盤，在這台電腦上一起遊玩戰役。"},
         {"效能顯示", "在畫面角落顯示遊戲的每秒影格數。"},
         {"顯示位置", "效能顯示出現在畫面的哪個角落。"},
         {"視野", "鏡頭視野的寬度。80 為原始值；瞄準與掩護會以相同比例改變。"},
         {"底片顆粒", "遊戲加在畫面上的底片顆粒雜訊。"},
         {"景深", "焦點以外部分的模糊效果。關閉可讓遊戲執行得更快。"},
         {"動態陰影", "角色與移動物體的陰影。關閉可讓遊戲執行得更快。"},
         {"環境光遮蔽", "角落與物體相接處的柔和陰影。"},
         {"光暈", "明亮光源周圍的光暈。"},
         {"鏡頭光暈", "明亮光源在畫面上產生的鏡頭光斑。"}},
        "關閉", "開啟", "FXAA", "FXAA 極致", "開啟", "雙線性（原始）", "AMD CAS（銳利化）", "AMD FSR 1", "自動", {"左上角", "右上角", "左下角", "右下角"}, "詳細", "進階"}},
      {"kor",
       {{"키보드 및 마우스", "키보드 및 마우스 설정을 조정합니다."},
        {{"밝기 보정", "밝기 보정 화면을 엽니다."},
         {"전체 화면", "전체 화면 또는 창 모드로 플레이합니다."},
         {"프레임 속도 제한", "초당 최대 프레임 수. 30은 Xbox 360과 같습니다."},
         {"안티에일리어싱", "계단 현상을 부드럽게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"모션 블러", "빠르게 움직일 때 화면을 흐리게 합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"마우스 감도", "마우스로 카메라를 돌리는 속도를 조정합니다."},
         {"비등방성 필터링", "비스듬히 보이는 텍스처를 선명하게 유지합니다."},
         {"이미지 스케일링", "화면을 창 크기에 맞게 조정하는 방식입니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"렌더링 해상도",
          "이미지 스케일링 전에 게임을 그리는 해상도입니다. 자동은 이 컴퓨터에 맞게 선택합니다. 다음에 게임을 시작할 때 적용됩니다."},
         {"선명도",
          "이미지 스케일링에 더할 선명도입니다. 0은 기본값을 유지합니다."},
         {"로컬 협동", "이 컴퓨터에서 함께 캠페인을 플레이하도록 각 플레이어가 사용할 컨트롤러나 키보드를 선택합니다."},
         {"성능 표시", "화면 모서리에 게임의 프레임 속도를 표시합니다."},
         {"표시 위치", "성능 표시가 나타나는 화면 모서리입니다."},
         {"시야각", "카메라 시야의 넓이입니다. 80이 원래 값이며, 조준과 엄폐도 같은 비율로 바뀝니다."},
         {"필름 그레인", "게임이 화면에 더하는 필름 그레인 노이즈입니다."},
         {"피사계 심도", "초점 밖 부분의 흐림 효과입니다. 끄면 게임이 더 빨리 실행됩니다."},
         {"동적 그림자", "캐릭터와 움직이는 물체의 그림자입니다. 끄면 게임이 더 빨리 실행됩니다."},
         {"앰비언트 오클루전", "구석과 물체가 맞닿는 곳의 부드러운 그림자입니다."},
         {"블룸", "밝은 빛 주위의 번짐입니다."},
         {"렌즈 플레어", "밝은 빛이 화면 위에 그리는 플레어입니다."}},
        "끔", "켬", "FXAA", "FXAA 최대", "열기", "바이리니어 (원본)", "AMD CAS (선명화)", "AMD FSR 1", "자동", {"왼쪽 위", "오른쪽 위", "왼쪽 아래", "오른쪽 아래"}, "상세", "고급"}},
  };
  return texts;
}

const Texts& TextsFor(const std::string& language) {
  // Latin American Spanish (esm) uses the Spanish texts.
  const auto found = LocalizedTexts().find(language == "esm" ? "esn" : language);
  return found != LocalizedTexts().end() ? found->second : LocalizedTexts().at("int");
}

// The extension of the localization files in the player's language (user_language, an Xbox 360
// language ID). The value names of our lists are asked for while the menu is open, after every
// language's file has been patched, so they follow the language the game shows.
std::string PlayerLanguage() {
  static const std::map<uint32_t, const char*> extensions = {
      {1, "int"}, {2, "jpn"}, {3, "deu"}, {4, "fra"},  {5, "esn"},  {6, "ita"},
      {7, "kor"}, {8, "chn"}, {9, "ptb"}, {11, "pol"}, {12, "rus"}};
  const auto found = extensions.find(REXCVAR_GET(user_language));
  return found != extensions.end() ? found->second : "int";
}

std::mutex g_mutex;
std::filesystem::path g_config_path;
// The game's texts of the controls menus, read as each language's localization file is patched
// (the menu shows the player's language's).
std::map<std::string, GameControlsTexts> g_controls_texts;
// Value shown for each option once read: settings that apply at the next start keep their old
// cvar value until then, so the menu shows what was chosen rather than what is in effect.
std::map<int, float> g_values;
// Value each option had when the game started (the first time it was read), to tell whether one
// that only applies at start-up has changed since.
std::map<int, float> g_started;
// Values of the start-up options the player was last asked about, so that answering "Later" is
// not asked again until something changes.
std::string g_prompted_for;
// Shows a new upscaling_sharpness while the game runs (SetSharpnessChangedHandler).
std::function<void()> g_sharpness_changed;
// Whether the running presenter draws a present_effect value (SetUpscalingSupport), behind its own
// mutex because ChoicesOf runs with g_mutex held and without it.
std::mutex g_upscaling_support_mutex;
std::function<bool(const std::string&)> g_upscaling_support;

// The values `option` offers: for the upscaling row, the ones the running presenter draws, so a GPU
// or backend without an effect does not list it (bilinear when it supports none of them).
std::vector<const char*> ChoicesOf(const Option& option) {
  if (option.label != Label::kUpscaling) return option.choices;
  std::function<bool(const std::string&)> supported;
  {
    std::lock_guard lock(g_upscaling_support_mutex);
    supported = g_upscaling_support;
  }
  if (!supported) return option.choices;
  std::vector<const char*> choices;
  for (const char* effect : option.choices) {
    if (supported(effect)) choices.push_back(effect);
  }
  if (choices.empty()) choices.push_back("bilinear");
  return choices;
}

bool Valid(int option) {
  return option >= 0 && option < static_cast<int>(Options().size());
}

// Options that the runtime only reads when the game starts.
bool NeedsRestart(const Option& option) {
  return option.label == Label::kAntialiasing || option.label == Label::kUpscaling ||
         option.label == Label::kMotionBlur || option.label == Label::kRenderResolution;
}

// Rows whose changes the script hears of while the menu is open (bHasOnChangeCallback): Flash runs
// the callback that UIScene_Options.GetCallbackForOptionPSID gives for the row's profile setting.
// Ours borrow the callbacks of two game rows the Video list no longer has, the calibration
// screen's gamma (49) and TV type (53), and the script hooks run our code instead of theirs. Returns
// the borrowed profile setting ID, or -1 for a row without one.
int LiveCallbackPsid(const Option& option) {
  if (option.label == Label::kUpscaling) return 53;
  if (option.label == Label::kSharpness) return 49;
  return -1;
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
      const std::vector<const char*> choices = ChoicesOf(option);
      for (size_t i = 0; i < choices.size(); ++i) {
        if (value == choices[i]) return static_cast<float>(i);
      }
      // A value the list does not offer (set by hand, by an older launcher, or an effect this GPU
      // does not draw): the nearest one when the values are numbers, else the first.
      size_t nearest = 0;
      try {
        const double wanted = std::stod(value);
        double best = 1e300;
        for (size_t i = 0; i < choices.size(); ++i) {
          const double distance = std::abs(std::stod(choices[i]) - wanted);
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
      const std::vector<const char*> choices = ChoicesOf(option);
      const size_t index = std::min(static_cast<size_t>(value < 0 ? 0 : value + 0.5f), choices.size() - 1);
      const std::string choice = choices[index];
      const bool number = !choice.empty() && choice.find_first_not_of("0123456789.") == std::string::npos;
      const bool boolean = choice == "true" || choice == "false";
      return toml && !number && !boolean ? std::string("\"") + choice + "\"" : choice;
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
// The position of the parenthesis that closes the array that `opening` (such as
// "CategoryOptions=(") opens in `text`, outside quoted strings; npos when there is none.
size_t ArrayEnd(const std::string& text, const std::string& opening) {
  const size_t start = text.find(opening);
  if (start == std::string::npos) return std::string::npos;
  int depth = 1;
  bool quoted = false;
  for (size_t i = start + opening.size(); i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '(') ++depth;
    if (c == ')' && --depth == 0) return i;
  }
  return std::string::npos;
}

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
      << "\",bUsesRawValue=false,bIsGlobalValue=false,bHasOnChangeCallback="
      << (LiveCallbackPsid(option) >= 0 ? "true" : "false") << ","
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
    if (option.label == Label::kFilmGrain || option.label == Label::kDepthOfField ||
        option.label == Label::kDynamicShadows || option.label == Label::kAmbientOcclusion ||
        option.label == Label::kBloom || option.label == Label::kLensFlares) {
      const Texts& texts = TextsFor(PlayerLanguage());
      names = {CoalescedString::FromUtf8(texts.off).text, CoalescedString::FromUtf8(texts.on).text};
    }
    if (option.label == Label::kFieldOfView) {
      for (const char* degrees : option.choices) names.push_back(CoalescedString::FromAscii(degrees).text);
    }
    if (option.label == Label::kPerformanceOverlay) {
      const std::string language = PlayerLanguage();
      const Texts& texts = TextsFor(language);
      names = {CoalescedString::FromUtf8(texts.off).text, u"FPS", CoalescedString::FromUtf8(texts.detailed).text,
               CoalescedString::FromUtf8(texts.advanced).text};
    }
    if (option.label == Label::kPerformanceOverlayPosition) {
      const std::string language = PlayerLanguage();
      for (const char* corner : TextsFor(language).corners) names.push_back(CoalescedString::FromUtf8(corner).text);
    }
    if (option.label == Label::kRenderResolution) {
      const std::string language = PlayerLanguage();
      names = {CoalescedString::FromUtf8(TextsFor(language).automatic).text, u"1280 × 720", u"2560 × 1440",
               u"3840 × 2160"};
    }
    if (option.label == Label::kUpscaling) {
      const std::string language = PlayerLanguage();
      const Texts& texts = TextsFor(language);
      for (const char* effect : ChoicesOf(option)) {
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

GameControlsTexts ControlsTextsFromGame() {
  const std::string language = PlayerLanguage();
  std::lock_guard lock(g_mutex);
  for (const std::string& candidate : {language, std::string("int")}) {
    const auto found = g_controls_texts.find(candidate);
    if (found != g_controls_texts.end()) return found->second;
  }
  return g_controls_texts.empty() ? GameControlsTexts{} : g_controls_texts.begin()->second;
}

void StoreControlsSetting(const std::string& name, const std::string& value, bool quoted) {
  rex::cvar::SetFlagByName(name, value);
  REXLOG_INFO("[Azura controls] {} = {}", name, value);
  std::filesystem::path config_path;
  {
    std::lock_guard lock(g_mutex);
    config_path = g_config_path;
  }
  if (!config_path.empty()) UpsertTomlKey(config_path, name, quoted ? "\"" + value + "\"" : value);
}

std::string ControlsSettingDefault(const std::string& name) {
  std::filesystem::path config_path;
  {
    std::lock_guard lock(g_mutex);
    config_path = g_config_path;
  }
  std::string value = ReadTomlValue(config_path.parent_path() / "azura.default.toml", name);
  // A string as the file has it, between quotes.
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
  return value;
}

namespace {

// Whether the image scaling effect chosen in the menu (shown, maybe not in effect until a restart)
// sharpens, so the sharpness row applies to it.
bool ChosenUpscalingSharpens() {
  for (size_t i = 0; i < Options().size(); ++i) {
    const Option& option = Options()[i];
    if (option.label != Label::kUpscaling) continue;
    const std::string effect = FormatValue(option, PcOptionDisplayValue(static_cast<int>(i)), false);
    return effect == "cas" || effect == "fsr";
  }
  return false;
}

// Whether `option` is a row the menu shows now: the sharpness row only with an effect that sharpens.
bool Shown(const Option& option) {
  return option.label != Label::kSharpness || ChosenUpscalingSharpens();
}

}  // namespace

// The menu numbers only the rows it shows: `row` counts the shown options of `category` in the
// order of their Option::row, so the rows below a hidden one move up.
int FindPcOption(int category, int row) {
  int shown_row = 0;
  for (int option_row = 0;; ++option_row) {
    int found = -1;
    for (size_t i = 0; i < Options().size(); ++i) {
      if (Options()[i].category == category && Options()[i].row == option_row) found = static_cast<int>(i);
    }
    if (found < 0) return -1;
    if (!Shown(Options()[found])) continue;
    if (shown_row++ == row) return found;
  }
}

int PcOptionsShownRows(int category) {
  int rows = 0;
  while (FindPcOption(category, rows) >= 0) ++rows;
  return rows;
}

std::vector<int> PcOptionsArrangedPsids(int category) {
  std::vector<int> psids;
  for (int row = 0;; ++row) {
    const int option = FindPcOption(category, row);
    if (option < 0) break;
    psids.push_back(Options()[option].psid);
  }
  for (const Option& option : Options()) {
    if (option.category == category && !Shown(option)) psids.push_back(option.psid);
  }
  return psids;
}

int PcOptionLiveCallbackPsid(int psid) {
  for (const Option& option : Options()) {
    if (option.psid == psid) return LiveCallbackPsid(option);
  }
  return -1;
}

bool PcOptionLiveChange(int category, int borrowed_psid, float value, int& row, bool& rows_changed) {
  for (size_t i = 0; i < Options().size(); ++i) {
    const Option& option = Options()[i];
    if (option.category != category || LiveCallbackPsid(option) != borrowed_psid || !Shown(option)) continue;
    const int rows_before = PcOptionsShownRows(category);
    PcOptionStoreValue(static_cast<int>(i), value);
    // The option's place in the menu, among the rows shown now.
    row = 0;
    while (FindPcOption(category, row) != static_cast<int>(i)) ++row;
    rows_changed = PcOptionsShownRows(category) != rows_before;
    return true;
  }
  return false;
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
  if (entry.label == Label::kSharpness) ApplyUpscalingSharpness(true);
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
    if (option.label == Label::kSharpness) ApplyUpscalingSharpness(true);
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

  // Game: its own rows, then the two of the mature content category (violence and mature language),
  // whose position Local co-op takes. The category's text in the file is kept as it is (its
  // titles, rows and buttons in this language) and the two rows are added at the end of its list;
  // they are new elements of the array, so every field is written.
  {
    // Read before Local co-op is written over it, below.
    constexpr int kMatureContentCategory = kLocalCoopCategory;
    const std::string game = Utf8(CategoryText(section, kGameCategory));
    const std::u16string mature = CategoryText(section, kMatureContentCategory);
    const auto mature_labels = Quoted(mature, u"SettingDescription");
    const auto mature_tooltips = Quoted(mature, u"SettingTooltip");
    constexpr int kMatureRows[] = {46, 37};  // violence, mature language
    std::string rows;
    for (size_t i = 0; i < 2 && i < mature_labels.size() && i < mature_tooltips.size(); ++i) {
      rows += std::string(",(ProfileSettingId=") + std::to_string(kMatureRows[i]) + ",SettingDescription=\"" +
              mature_labels[i] + "\",SettingTooltip=\"" + mature_tooltips[i] +
              "\",bUsesRawValue=false,bIsGlobalValue=false,bHasOnChangeCallback=false,"
              "bRequiresMatureLanguageSupport=" + (kMatureRows[i] == 37 ? "true" : "false") +
              ",Type=OST_OptionList,Slider_Minimum=0.00,Slider_Maximum=0.00,Slider_Increment=0.00)";
    }
    const size_t list_end = ArrayEnd(game, "CategoryOptions=(");
    if (mature_labels.size() >= 2 && mature_tooltips.size() >= 2 && list_end != std::string::npos) {
      SetKey(section, "OptionCategories[" + std::to_string(kGameCategory) + "]",
             game.substr(0, list_end) + rows + game.substr(list_end));
    } else {
      REXLOG_WARN("[Azura options] The Game and mature content categories are not as expected in {}: mature "
                  "content stays out of the menu",
                  language);
    }
  }

  // Local co-op, in place of the mature content: choosing it runs Callback_SelectStorageLocation (the
  // storage location's, which no category uses any more) instead of opening a list, and
  // script_hooks.cpp opens the Local co-op menu there. Every field is written, since the position
  // held a different category; its two rows stay in the array, which never shrinks, but no list is
  // shown.
  const auto& [local_coop_title, local_coop_tooltip] = texts.labels[static_cast<int>(Label::kLocalCoop)];
  SetKey(section, "OptionCategories[" + std::to_string(kLocalCoopCategory) + "]",
         std::string("(CategoryDescription=\"") + local_coop_title + "\",CategoryTooltip=\"" + local_coop_tooltip +
             "\",OnSelectedCallback=\"Callback_SelectStorageLocation\",bIsGlobalValue=false,CategoryOptions=(),"
             "MenuButtons=((ButtonDescription=\"" + back + "\",ButtonType=1,ButtonHandler=OBH_MAX)))");

  // Keyboard and mouse, in place of the storage location (a device picker without rows): choosing
  // it runs Callback_ResetTutorials, where script_hooks.cpp opens the keyboard and mouse's controls
  // menu. Every field is written, since the position held a different category.
  SetKey(section, "OptionCategories[" + std::to_string(kKeyboardMouseCategory) + "]",
         std::string("(CategoryDescription=\"") + texts.keyboard_mouse.first + "\",CategoryTooltip=\"" +
             texts.keyboard_mouse.second +
             "\",OnSelectedCallback=\"Callback_ResetTutorials\",bIsGlobalValue=false,CategoryOptions=(),"
             "MenuButtons=((ButtonDescription=\"" + back + "\",ButtonType=1,ButtonHandler=OBH_MAX)))");

  // Controller: the game's category as it is (its texts in this language), choosing it running
  // Callback_ViewController, where script_hooks.cpp opens the controller's controls menu. Its rows
  // are kept for their texts, which the menu shows; no list of them is opened any more.
  const std::string controller = Utf8(CategoryText(section, kControllerCategory));
  const size_t rows_at = controller.find("CategoryOptions=(");
  if (rows_at != std::string::npos && controller.find("OnSelectedCallback=") == std::string::npos) {
    SetKey(section, "OptionCategories[" + std::to_string(kControllerCategory) + "]",
           controller.substr(0, rows_at) + "OnSelectedCallback=\"Callback_ViewController\"," + controller.substr(rows_at));
  } else {
    REXLOG_WARN("[Azura options] The Controller category is not as expected in {}: it keeps its list", language);
  }

  // What the controls menus show from the game, in this language.
  GameControlsTexts controls;
  for (const auto& key : section.keys) {
    const std::string name = key.name.Ascii();
    if (name.size() > 6 && name.compare(name.size() - 6, 6, "String") == 0) {
      controls.buttons[name] = Utf8(key.value.text);
    }
  }
  const std::u16string controller_text = CategoryText(section, kControllerCategory);
  const auto controller_title = Quoted(controller_text, u"CategoryDescription");
  controls.controller_title = controller_title.empty() ? std::string("CONTROLLER") : controller_title[0];
  // The Controller category's rows, in the game's order (docs/reverse-engineering.md).
  constexpr int kControllerRows[] = {2, 58, 375, 368, 59, 60, 1, 38, 39, 51};
  const auto row_labels = Quoted(controller_text, u"SettingDescription");
  for (size_t i = 0; i < std::size(kControllerRows) && i < row_labels.size(); ++i) {
    controls.setting_labels[kControllerRows[i]] = row_labels[i];
  }
  controls.back = back;
  controls.defaults = defaults;
  std::lock_guard lock(g_mutex);
  GameControlsTexts& stored = g_controls_texts[language];
  controls.value_names = std::move(stored.value_names);
  stored = std::move(controls);
}

void PatchValueNames(CoalescedSection& section, const std::string& language) {
  // The value names of the controller's list settings, for the controls menus: their definitions'
  // positions in ProfileMappings, by profile setting ID (from the class defaults).
  {
    constexpr std::pair<int, int> kControllerMappings[] = {
        {1, 0}, {2, 1}, {38, 30}, {39, 31}, {51, 38}, {58, 59}, {375, 159},
    };
    std::map<int, std::vector<std::string>> value_names;
    for (const auto& [psid, mapping] : kControllerMappings) {
      const CoalescedKey* key = FindKey(section, "ProfileMappings[" + std::to_string(mapping) + "]");
      if (key) value_names[psid] = Quoted(key->value.text, u"Name");
    }
    std::lock_guard lock(g_mutex);
    g_controls_texts[language].value_names = std::move(value_names);
  }
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

void ApplyPresentMode() {
#if defined(_WIN32)
  // azura.default.toml keeps presentation in step with the display (FIFO) on every platform. On
  // Windows, FIFO holds the game at a third of the display's refresh (20 FPS at 60 Hz) although
  // its frames take far less, while mailbox, which also never tears, lets it reach its frame rate
  // limit. A value in the player's azura.toml, which the runtime loads after this, still wins: it
  // is set here only when that file does not mention it, since a value set from code outranks the
  // settings files.
  constexpr const char* kMailbox = "vulkan_allow_present_mode_mailbox";
  std::filesystem::path config_path;
  {
    std::lock_guard lock(g_mutex);
    config_path = g_config_path;
  }
  if (!config_path.empty() && !ReadTomlValue(config_path, kMailbox).empty()) {
    return;
  }
  if (rex::cvar::SetFlagByName(kMailbox, "true")) {
    REXLOG_INFO("[Azura options] Mailbox presentation allowed (Windows)");
  } else {
    REXLOG_WARN("[Azura options] Cannot allow the mailbox presentation mode");
  }
#endif
}

namespace {

// Resolution scale "auto" stands for, as the launcher's Automatic (launcher/src/graphics.js): 2 on
// Apple Pro, Max and Ultra chips with at least 16 GB of memory (more GPU cores and memory
// bandwidth), else 1, the console's own resolution.
int AutoResolutionScale() {
#if defined(__APPLE__) && defined(__aarch64__)
  char chip[128] = {};
  size_t chip_size = sizeof(chip) - 1;
  uint64_t memory = 0;
  size_t memory_size = sizeof(memory);
  if (sysctlbyname("machdep.cpu.brand_string", chip, &chip_size, nullptr, 0) != 0 ||
      sysctlbyname("hw.memsize", &memory, &memory_size, nullptr, 0) != 0) {
    return 1;
  }
  if (memory < 16ull * 1024 * 1024 * 1024) return 1;
  std::istringstream words{std::string(chip)};
  for (std::string word; words >> word;) {
    if (word == "Pro" || word == "Max" || word == "Ultra") return 2;
  }
#endif
  return 1;
}

}  // namespace

void ApplyRenderResolution() {
  const std::string chosen = REXCVAR_GET(render_resolution);
  const int scale = chosen == "1280x720"    ? 1
                    : chosen == "2560x1440" ? 2
                    : chosen == "3840x2160" ? 3
                                            : AutoResolutionScale();
  // A larger render needs more texture memory; the hard limit is twice the soft one, as in the
  // SDK's defaults (384 and 768 MB).
  const int texture_memory = scale >= 2 ? 768 : 384;
  const std::pair<const char*, int> settings[] = {
      {"draw_resolution_scale_x", scale},
      {"draw_resolution_scale_y", scale},
      {"texture_cache_memory_limit_soft", texture_memory},
      {"texture_cache_memory_limit_hard", texture_memory * 2},
  };
  for (const auto& [name, value] : settings) {
    // Rejected when the setting is not registered yet (its GPU plugin is not loaded).
    if (!rex::cvar::SetFlagByName(name, std::to_string(value))) {
      REXLOG_WARN("[Azura options] Cannot set {} = {} for the render resolution", name, value);
    }
  }
  REXLOG_INFO("[Azura options] Render resolution {}: {}x{} (scale {}, texture memory {} MB)", chosen,
              1280 * scale, 720 * scale, scale, texture_memory);
  // These values are the ones the game starts with, not changes waiting for a restart.
  rex::cvar::ClearPendingRestartFlags();
}

void SetUpscalingSupport(std::function<bool(const std::string&)> supported) {
  std::lock_guard lock(g_upscaling_support_mutex);
  g_upscaling_support = std::move(supported);
}

void SetSharpnessChangedHandler(std::function<void()> handler) {
  std::lock_guard lock(g_mutex);
  g_sharpness_changed = std::move(handler);
}

void ApplyUpscalingSharpness(bool show_now) {
  const double sharpness = std::clamp(REXCVAR_GET(upscaling_sharpness), 0.0, 1.0);
  // CAS adds its sharpening on top of its own (0 to 1). FSR's RCAS takes a reduction in stops whose
  // default is 0.2: 0 here keeps that default and 1 removes the reduction, its sharpest.
  rex::cvar::SetFlagByName("present_cas_additional_sharpness", FormatFloat(float(sharpness)));
  if (rex::cvar::GetFlagInfo("present_fsr_sharpness_reduction")) {
    rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", FormatFloat(float(0.2 * (1.0 - sharpness))));
  }
  std::function<void()> handler;
  {
    std::lock_guard lock(g_mutex);
    handler = g_sharpness_changed;
  }
  if (show_now && handler) handler();
}

}  // namespace azura::menu
