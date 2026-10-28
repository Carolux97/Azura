// Hooks into the game's UnrealScript interpreter (Unreal Engine 3, recompiled).
//
// Script calling script goes through UObject::CallFunction (guest 0x822DF4D0: r3 = object,
// r4 = FFrame& of the calling code, r5 = result, r6 = UFunction*), before the call's parameters
// are evaluated. Flash and native code call script functions through ProcessEvent, which skips it.
// Each generated function is a weak symbol that forwards to __imp__<name>, so defining
// sub_822DF4D0 here replaces it and __imp__sub_822DF4D0 still runs the original.
//
// Script names are FNames: an index into the global name table GNames (a TArray of FNameEntry*
// at guest 0x8343D148). An FNameEntry holds (index << 1 | unicode flag) at +0 and the characters
// at +8; a UObject holds its Outer at +40 and its FName at +44.
//
// The body of every script function runs in UObject::ProcessInternal (guest 0x822DF9E0:
// r3 = object, r4 = FFrame*, r5 = result), whoever called it. By then the parameters are in the
// frame's locals. FFrame: +16 Node (UFunction*), +20 Object, +24 Code, +28 Locals. A function's
// parameters are its first child properties (UStruct children at +68, UField::Next at +60), each
// with PropertyFlags (64-bit) at +72 and its offset in the locals at +84.
//
// With `script_trace` on, the first call of every script function is logged as Class.Function
// ("(event)" when it came through ProcessEvent), and every call of the functions listed in
// `script_trace_args` is logged with its parameters.
// That is how menu and gameplay functions, and what they receive, are identified.
//
// Quit Game: the main menu calls UIScene_FrontEnd.HandleMenuSelection(ParentScreen,
// SelectedOption, bIsSimulating, ...) when an option is chosen. When it is the Quit Game option
// added by menu_patches, the quit handler runs instead of the script, which would not know the
// option.
//
// PC options (menu/pc_options.h): the rows Azura adds to the Options menu are driven from here.
// While PopulateOptionsList builds them, each row's value is written into the script's locals
// just before it reaches Flash; when the menu saves, the values GetOptionValueAt returns are
// applied; the selected row of the Video list is followed to open the brightness calibration; and
// the menu's bytecode is patched where it would refuse to show Video and Keyboard and mouse as
// lists. The layouts and bytecode involved are in docs/reverse-engineering.md.
//
// Field of view: just before a gameplay camera mode answers GetDesiredFOV, its FOVAngle is set to
// the mode's own value scaled by the Video menu's field_of_view (see "Field of view" below).
//
// Film grain: as each player's input runs, the grain of the post-process effects that player can
// be drawn with is turned off or back on with the Video menu's film_grain (see "Film grain").
//
// To keep the hooks cheap, script functions are matched by FName index; the indices are found by
// scanning new GNames entries every so often until they appear.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vector>

#include "script_hooks.h"

#include <atomic>
#include <functional>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/system_link.h>

#include "class_dump.h"
#include "dlc/class_fixups.h"
#include "dlc/dlc_achievements.h"
#include "dlc/missing_super.h"
#include "system_settings.h"
#include "menu/menu_patches.h"
#include "menu/pc_options.h"
#include "input/mouse_look.h"
#include "input/player_assignment.h"

REXCVAR_DEFINE_BOOL(script_trace, false, "Azura/Debug",
                    "Log the first call of every UnrealScript function (slow, for reverse engineering)");
REXCVAR_DEFINE_STRING(script_trace_args, "HandleMenuSelection", "Azura/Debug",
                      "With script_trace: comma-separated script functions whose every call is logged "
                      "with its parameters");
REXCVAR_DEFINE_STRING(script_dump_classes, "", "Azura/Debug",
                      "Comma-separated script classes whose functions and properties are logged once loaded");
REXCVAR_DEFINE_BOOL(script_dump_code, false, "Azura/Debug",
                    "With script_dump_classes: also log a rough listing of each function's bytecode");
REXCVAR_DECLARE(double, field_of_view);
REXCVAR_DECLARE(bool, film_grain);

extern "C" void __imp__sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kGNames = 0x8343D148;     // TArray<FNameEntry*>: data, num, max
constexpr uint32_t kObjectOuter = 40;
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kObjectClass = 52;
constexpr uint32_t kFieldNext = 60;
constexpr uint32_t kStructSuper = 64;
constexpr uint32_t kStructChildren = 68;
constexpr uint32_t kPropertyFlags = 72;
constexpr uint32_t kPropertyOffset = 84;
// UProperty::ElementSize, between ArrayDim (64) and PropertyFlags in UE3's layout (inferred from
// the offsets above; it reads UIScene_FrontEnd.FilterButtonInput's int ControllerId right).
constexpr uint32_t kPropertyElementSize = 68;
constexpr uint32_t kFrameNode = 16;
constexpr uint32_t kFrameCode = 24;
constexpr uint32_t kFrameLocals = 28;
constexpr uint64_t kPropertyParm = 0x80;  // CPF_Parm

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}

// Text of FName `index`, or empty when the index or table looks wrong.
std::string NameText(const uint8_t* base, uint32_t index) {
  const uint32_t data = LoadU32(base, kGNames);
  const uint32_t count = LoadU32(base, kGNames + 4);
  if (data == 0 || index >= count) {
    return {};
  }
  const uint32_t entry = LoadU32(base, data + index * 4);
  if (entry == 0) {
    return {};
  }
  const bool wide = (LoadU32(base, entry) & 1) != 0;
  std::string text;
  for (uint32_t i = 0; i < 128; ++i) {
    char c;
    if (wide) {
      const uint16_t w = static_cast<uint16_t>(base[entry + 8 + i * 2] << 8 | base[entry + 8 + i * 2 + 1]);
      c = w < 128 ? static_cast<char>(w) : '?';
    } else {
      c = static_cast<char>(base[entry + 8 + i]);
    }
    if (c == 0) break;
    text.push_back(c);
  }
  return text;
}

std::string ObjectName(const uint8_t* base, uint32_t object) {
  return object ? NameText(base, LoadU32(base, object + kObjectName)) : std::string();
}

// Value of a script parameter at `address`, decoded by its property class.
std::string ParamValue(const uint8_t* base, const std::string& type, uint32_t address) {
  if (type == "IntProperty") return std::to_string(static_cast<int32_t>(LoadU32(base, address)));
  if (type == "ByteProperty") return std::to_string(base[address]);
  if (type == "BoolProperty") return LoadU32(base, address) ? "true" : "false";
  if (type == "FloatProperty") {
    const uint32_t bits = LoadU32(base, address);
    float f;
    std::memcpy(&f, &bits, 4);
    return std::to_string(f);
  }
  if (type == "NameProperty") return "'" + NameText(base, LoadU32(base, address)) + "'";
  if (type == "ObjectProperty" || type == "ClassProperty") {
    const uint32_t object = LoadU32(base, address);
    return object ? ObjectName(base, object) : "None";
  }
  if (type == "StrProperty") {  // FString: TArray<TCHAR> (data, num, max), UTF-16BE
    const uint32_t data = LoadU32(base, address);
    const uint32_t count = LoadU32(base, address + 4);
    std::string text = "\"";
    for (uint32_t i = 0; data && i + 1 < count && i < 200; ++i) {
      const uint16_t w = static_cast<uint16_t>(base[data + i * 2] << 8 | base[data + i * 2 + 1]);
      text.push_back(w < 128 ? static_cast<char>(w) : '?');
    }
    return text + "\"";
  }
  char raw[16];
  std::snprintf(raw, sizeof(raw), "0x%08X", LoadU32(base, address));
  return raw;
}

// "Class.Function(param=value, ...)" for a frame about to run. With `result`, the address the
// caller passed for the return value, ReturnValue is read there: a script function returns into
// the caller's result, not into its own ReturnValue local.
std::string DescribeCall(const uint8_t* base, uint32_t frame, uint32_t result = 0) {
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  std::string text = ObjectName(base, LoadU32(base, function + kObjectOuter)) + "." + ObjectName(base, function) + "(";
  bool first = true;
  for (uint32_t prop = LoadU32(base, function + kStructChildren); prop; prop = LoadU32(base, prop + kFieldNext)) {
    const uint64_t flags = (uint64_t(LoadU32(base, prop + kPropertyFlags)) << 32) | LoadU32(base, prop + kPropertyFlags + 4);
    if (!(flags & kPropertyParm)) break;  // parameters come first; locals follow
    const std::string type = ObjectName(base, LoadU32(base, prop + kObjectClass));
    const std::string name = ObjectName(base, prop);
    const uint32_t address = result && name == "ReturnValue" ? result : locals + LoadU32(base, prop + kPropertyOffset);
    text += (first ? "" : ", ") + name + "=" + ParamValue(base, type, address);
    first = false;
  }
  return text + ")";
}

// Whether `name` is in the comma-separated `list`.
bool InList(const std::string& list, const std::string& name) {
  size_t start = 0;
  while (start <= list.size()) {
    const size_t end = std::min(list.find(',', start), list.size());
    if (list.compare(start, end - start, name) == 0) return true;
    start = end + 1;
  }
  return false;
}

// Locals offset of parameter `name` of `function`, or -1.
int32_t ParamOffset(const uint8_t* base, uint32_t function, const char* name) {
  for (uint32_t prop = LoadU32(base, function + kStructChildren); prop; prop = LoadU32(base, prop + kFieldNext)) {
    const uint64_t flags = (uint64_t(LoadU32(base, prop + kPropertyFlags)) << 32) | LoadU32(base, prop + kPropertyFlags + 4);
    if (!(flags & kPropertyParm)) break;
    if (ObjectName(base, prop) == name) return static_cast<int32_t>(LoadU32(base, prop + kPropertyOffset));
  }
  return -1;
}

// The value of the int or byte parameter `name` of the frame about to run `function`, or -1.
int32_t SmallParam(const uint8_t* base, uint32_t function, uint32_t frame, const char* name) {
  for (uint32_t prop = LoadU32(base, function + kStructChildren); prop; prop = LoadU32(base, prop + kFieldNext)) {
    const uint64_t flags = (uint64_t(LoadU32(base, prop + kPropertyFlags)) << 32) | LoadU32(base, prop + kPropertyFlags + 4);
    if (!(flags & kPropertyParm)) break;
    if (ObjectName(base, prop) != name) continue;
    const uint32_t at = LoadU32(base, frame + kFrameLocals) + LoadU32(base, prop + kPropertyOffset);
    return LoadU32(base, prop + kPropertyElementSize) == 1 ? base[at] : static_cast<int32_t>(LoadU32(base, at));
  }
  return -1;
}

// --- Quit Game ---

std::mutex g_quit_mutex;
std::function<void()> g_quit_handler;
std::function<void()> g_restart_handler;
// Opens the Local co-op menu (SetLocalCoopHandler).
std::function<void()> g_local_coop_handler;
// Opens a controls menu (SetControlsMenuHandler).
std::function<void(azura::script::ControlsMenu)> g_controls_handler;
std::atomic<bool> g_quit_requested{false};

// Script functions the hooks react to, matched by FName index to keep the hooks cheap. The indices
// are found by scanning new GNames entries every so often until all of them have appeared.
enum WatchedName {
  kHandleMenuSelection,      // UIScene_FrontEnd: a main menu choice (Quit Game)
  kPopulateOptionsList,      // UIScene_Options: builds the rows of a category
  kGetOptionValueAt,         // UIScene_Options: the value of a row, read when the menu saves
  kResetDefaults,            // UIScene_Options.OnConfirm_ResetDefaults: Defaults was confirmed
  kFilterButtonInput,        // every button press and release a menu gets from Flash
  kCallbackVideoOptions,     // UIScene_Options: opens the brightness calibration screen
  kCloseVideoOptionsScene,   // UIScene_Options: the calibration screen closes
  kMenuOptionSelected,       // UIScene_Options.Callback_MenuOptionSelected: a category is chosen
  kReturnToMain,             // UIScene_Options.Callback_ReturnToMain: back to the categories
  kSpecialCaseValues,        // UIScene_Options: value names of a raw-value list row
  kResetAcceptAlias,         // button alias of the Defaults message box's Accept button
  kGenericAcceptAlias,       // button alias of the game's plain Accept button
  kBuildCampaignURL,         // GearMenuGame: builds travel URL for campaign
  kServerTravel,             // WorldInfo: travels to a new level
  kProcessServerTravel,      // GameInfo: processes server travel URL
  // Death trace (with script_trace): calls that show why the player dies around cinematics.
  kPawnDied,                 // Controller: its pawn died or was destroyed
  kShouldDeathCauseFailure,  // GearGameSP_Base: whether a death ends the game
  kGetDeathGameOverReason,   // GearGame: the game over text for a death
  kGameOverPlayer,           // GearGame: a player's game over
  kClientGameOver,           // GearPC: shows the game over screen
  kClientSetCinematicMode,   // PlayerController: a cinematic starts or ends
  kMatineeCancelled,         // a Matinee sequence is skipped
  kGetCallbackForOptionPsid,  // UIScene_Options: the live change callback of a row
  kTVTypeWasChanged,         // UIScene_Options callback of TV type, borrowed by a row of ours
  kGammaWasChanged,          // UIScene_Options callback of gamma, borrowed by a row of ours
  kSelectStorageLocation,    // UIScene_Options: the Local co-op category, at the storage location's place
  kShouldOptionBeDisabled,   // UIScene_Options: whether a category is greyed out in the list
  kPlayerInput,              // PlayerInput.PlayerInput: a player's input for the frame (mouse look)
  kViewController,           // UIScene_Options: the Controller category (its controls menu)
  kResetTutorials,           // UIScene_Options: Game's Reset tutorials, and the Keyboard and mouse category
  kPopulateOptionsMenu,      // UIScene_Options: the Options menu opens (its list of categories)
  kOnClose,                  // a menu scene closes (UIScene_Options: the Options menu)
  kWidgetInitialized,        // a menu scene sets up a widget (the main menu's scene, for the title screen)
  kGetDesiredFOV,            // a camera mode: the field of view it wants this frame
  kAddEnemyLocators,         // RAAM's Shadow's satellite camera: marks the enemies it sees
  kCompleteChapterByDifficulty,   // GearProfileSettings: a campaign chapter is completed
  kUpdateAchievementProgression,  // GearProfileSettings: progress towards a counted achievement
  kWatchedNameCount,
};
constexpr const char* kWatchedNames[kWatchedNameCount] = {
    "HandleMenuSelection",   "PopulateOptionsList",    "GetOptionValueAt",
    "OnConfirm_ResetDefaults", "FilterButtonInput",    "Callback_VideoOptions",
    "CloseVideoOptionsScene", "Callback_MenuOptionSelected", "Callback_ReturnToMain",
    "GetProfileSettingValue_SpecialCase",
    "ResetAccept",           "GenericAccept",
    "BuildCampaignURL",      "ServerTravel",           "ProcessServerTravel",
    "PawnDied",              "ShouldDeathCauseFailure", "GetDeathGameOverReason",
    "GameOverPlayer",        "ClientGameOver",         "ClientSetCinematicMode",
    "MatineeCancelled",      "GetCallbackForOptionPSID", "Callback_TVTypeWasChanged",
    "Callback_GammaWasChanged", "Callback_SelectStorageLocation", "ShouldOptionBeDisabled",
    "PlayerInput",           "Callback_ViewController", "Callback_ResetTutorials",
    "PopulateOptionsMenu",   "OnClose",     "WidgetInitialized",
    "GetDesiredFOV",         "AddEnemyLocators",       "CompleteChapterByDifficulty",
    "UpdateAchievementProgression"};

// FName index of each watched name, -1 until found (0 is a valid index: None).
std::atomic<int32_t> g_names[kWatchedNameCount];
[[maybe_unused]] const bool g_names_initialized = [] {
  for (auto& name : g_names) {
    name.store(-1, std::memory_order_relaxed);
  }
  return true;
}();
std::atomic<int> g_names_missing{kWatchedNameCount};
std::atomic<uint32_t> g_names_scanned{0};
std::atomic<uint32_t> g_calls{0};
std::mutex g_scan_mutex;

// Looks through the GNames entries added since the last scan for the watched names.
void ScanNewNames(const uint8_t* base) {
  std::unique_lock lock(g_scan_mutex, std::try_to_lock);
  if (!lock) return;
  const uint32_t count = LoadU32(base, kGNames + 4);
  for (uint32_t i = g_names_scanned.load(); i < count && g_names_missing.load() > 0; ++i) {
    const std::string text = NameText(base, i);
    for (int n = 0; n < kWatchedNameCount; ++n) {
      if (g_names[n].load() < 0 && text == kWatchedNames[n]) {
        g_names[n].store(static_cast<int32_t>(i));
        g_names_missing.fetch_sub(1);
      }
    }
  }
  g_names_scanned.store(count);
}

// Whether `function` (a UFunction*) has watched name `name`.
bool Is(const uint8_t* base, uint32_t function, WatchedName name) {
  const int32_t index = g_names[name].load(std::memory_order_relaxed);
  return function && index >= 0 && LoadU32(base, function + kObjectName) == static_cast<uint32_t>(index);
}

// --- Mouse look (input/mouse_look.h) --------------------------------------------------------------

// Offset of property `name` in objects of class `cls` (declared there or in a super class), or -1.
int32_t ClassPropertyOffset(const uint8_t* base, uint32_t cls, const char* name) {
  for (uint32_t owner = cls; owner; owner = LoadU32(base, owner + kStructSuper)) {
    for (uint32_t field = LoadU32(base, owner + kStructChildren); field; field = LoadU32(base, field + kFieldNext)) {
      if (ObjectName(base, field) != name) continue;
      // Functions and constants are fields too: only a property has a value offset.
      if (ObjectName(base, LoadU32(base, field + kObjectClass)).ends_with("Property")) {
        return static_cast<int32_t>(LoadU32(base, field + kPropertyOffset));
      }
    }
  }
  return -1;
}

// The engine's PlayerInput.PlayerInput (the one that adds the mouse axes; GearPlayerInput's own
// calls it), 0 until it first runs.
std::atomic<uint32_t> g_engine_player_input{0};

// Whether `function` is the engine's PlayerInput.PlayerInput.
bool IsEnginePlayerInput(const uint8_t* base, uint32_t function) {
  const uint32_t known = g_engine_player_input.load(std::memory_order_relaxed);
  if (known) return function == known;
  if (!Is(base, function, kPlayerInput) || ObjectName(base, LoadU32(base, function + kObjectOuter)) != "PlayerInput") {
    return false;
  }
  g_engine_player_input.store(function, std::memory_order_relaxed);
  return true;
}

// Offset of POV.FOV in a Camera's CameraCache (TCameraCache: float TimeStamp, then TPOV: vector
// Location, rotator Rotation, float FOV; the 32 bytes between CameraCache and LastFrameCameraCache
// in the dump of Camera match).
constexpr uint32_t kCameraCachePovFov = 4 + 12 + 12;

// Property offsets per class, found by name the first time a class is seen.
std::mutex g_mouse_look_mutex;
std::unordered_map<uint64_t, int32_t> g_class_offsets;

int32_t CachedOffset(const uint8_t* base, uint32_t cls, uint32_t slot, const char* name) {
  const uint64_t key = (static_cast<uint64_t>(cls) << 8) | slot;
  auto it = g_class_offsets.find(key);
  if (it == g_class_offsets.end()) {
    it = g_class_offsets.emplace(key, ClassPropertyOffset(base, cls, name)).first;
  }
  return it->second;
}

// As PlayerInput.PlayerInput starts on `player_input`, writes the mouse motion of its player into
// aMouseX and aMouseY. The player is its PlayerController's LocalPlayer's ControllerId, the guest
// user index, so in local co-op the mouse turns the camera of the player who holds it.
// Each guest user's PlayerController and the guest memory it is in, as their input last ran: where
// the controls menus find the player's profile (ReadProfileSetting).
std::atomic<uint32_t> g_player_controllers[4];
std::atomic<uint8_t*> g_guest_base{nullptr};
// Where player 1 is as of their last input (azura::script::Menus): in the front end (its party
// controller, GearPartyPC), with the game paused (WorldInfo.Pauser set), or playing; and whether
// the Options menu is open (from PopulateOptionsMenu to the scene's OnClose).
std::atomic<bool> g_front_end{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_options_open{false};
// The main menu's scene (UIScene_FrontEnd), as it last set up a widget or got a button, whose
// bOnTitleScreen tells the title screen from the main menu.
std::atomic<uint32_t> g_front_end_scene{0};
// The controller (ControllerId) that last pressed a button on the main menu's scene: the one that
// chose a main menu option.
std::atomic<int32_t> g_front_end_controller{0};

void ApplyFilmGrain(uint8_t* base, uint32_t controller, uint32_t local_player);

void ApplyMouseLookFor(uint8_t* base, uint32_t player_input) {
  std::lock_guard lock(g_mouse_look_mutex);
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const int32_t mouse_x = CachedOffset(base, input_class, 0, "aMouseX");
  const int32_t mouse_y = CachedOffset(base, input_class, 1, "aMouseY");
  const int32_t sensitivity = CachedOffset(base, input_class, 2, "MouseSensitivity");
  const uint32_t controller = LoadU32(base, player_input + kObjectOuter);
  if (mouse_x < 0 || mouse_y < 0 || sensitivity < 0 || !controller) return;
  const int32_t player = CachedOffset(base, LoadU32(base, controller + kObjectClass), 3, "Player");
  const uint32_t local_player = player >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(player)) : 0;
  if (!local_player) return;
  const int32_t controller_id = CachedOffset(base, LoadU32(base, local_player + kObjectClass), 4, "ControllerId");
  if (controller_id < 0) return;
  const uint32_t user = LoadU32(base, local_player + static_cast<uint32_t>(controller_id));
  if (user >= 4) return;
  ApplyFilmGrain(base, controller, local_player);
  azura::system_settings::Apply(base);
  g_player_controllers[user].store(controller, std::memory_order_relaxed);
  g_guest_base.store(base, std::memory_order_relaxed);
  if (user == 0) {
    const uint32_t controller_class = LoadU32(base, controller + kObjectClass);
    const std::string controller_name = ObjectName(base, controller_class);
    const bool front_end = controller_name == "GearPartyPC" || controller_name == "GearMenuPC";
    const int32_t world_at = CachedOffset(base, controller_class, 7, "WorldInfo");
    const uint32_t world = world_at >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(world_at)) : 0;
    bool paused = false;
    if (world) {
      const int32_t pauser_at = CachedOffset(base, LoadU32(base, world + kObjectClass), 8, "Pauser");
      paused = pauser_at >= 0 && LoadU32(base, world + static_cast<uint32_t>(pauser_at)) != 0;
    }
    g_paused.store(paused, std::memory_order_relaxed);
    if (g_front_end.exchange(front_end, std::memory_order_relaxed) != front_end) {
      // A level change closes every menu, whatever the hooks saw of it.
      g_options_open.store(false, std::memory_order_relaxed);
      REXLOG_INFO("[Azura menus] Player 1 {} the front end ({})", front_end ? "is in" : "left", controller_name);
    }
  }
  azura::input::MouseLookTarget target{player_input + static_cast<uint32_t>(mouse_x),
                                       player_input + static_cast<uint32_t>(mouse_y),
                                       player_input + static_cast<uint32_t>(sensitivity)};
  // The view angle PlayerInput's FOVScale is about to read: GetFOVAngle answers the player
  // camera's CameraCache.POV.FOV (TCameraCache: TimeStamp, then TPOV: Location, Rotation, FOV).
  const int32_t camera_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 11, "PlayerCamera");
  const uint32_t camera = camera_at >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(camera_at)) : 0;
  if (camera) {
    const int32_t cache_at = CachedOffset(base, LoadU32(base, camera + kObjectClass), 12, "CameraCache");
    if (cache_at >= 0) {
      const uint32_t bits = LoadU32(base, camera + static_cast<uint32_t>(cache_at) + kCameraCachePovFov);
      float fov;
      std::memcpy(&fov, &bits, 4);
      if (fov > 1.0f && fov < 170.0f) target.view_fov = fov;
    }
  }
  target.base_fov = static_cast<float>(REXCVAR_GET(field_of_view));
  azura::input::ApplyMouseLook(base, user, target);
}

// The campaign DLC (RAAM's Shadow), chosen in the campaign menu and not replaced by another choice
// since: the travel to a campaign starts its chapters (PatchTravelURL).
std::atomic<bool> g_dlc_campaign_chosen{false};
constexpr int kFirstDlcChapter = 31;

// Notes a campaign menu choice (UIScene_FrontEnd.HandleMenuSelection on MAINMENUOP_Campaign).
void NoteCampaignChoice(const uint8_t* base, uint32_t frame) {
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  const int32_t screen = ParamOffset(base, function, "ParentScreen");
  const int32_t option = ParamOffset(base, function, "SelectedOption");
  const int32_t simulating = ParamOffset(base, function, "bIsSimulating");
  if (screen < 0 || option < 0 || base[locals + screen] != azura::menu::kCampaignScreen) return;
  if (simulating >= 0 && LoadU32(base, locals + simulating) != 0) return;
  const bool dlc = static_cast<int32_t>(LoadU32(base, locals + option)) == azura::menu::kCampaignDlcOption;
  if (g_dlc_campaign_chosen.exchange(dlc) != dlc) {
    REXLOG_INFO("[Azura campaign] {} chosen", dlc ? "RAAM's Shadow" : "The main campaign");
  }
}

// Whether the frame about to run is the main menu choosing the Quit Game option.
bool IsQuitSelection(const uint8_t* base, uint32_t frame, int quit_option) {
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  const int32_t screen = ParamOffset(base, function, "ParentScreen");
  const int32_t option = ParamOffset(base, function, "SelectedOption");
  const int32_t simulating = ParamOffset(base, function, "bIsSimulating");
  if (screen < 0 || option < 0) return false;
  if (simulating >= 0 && LoadU32(base, locals + simulating) != 0) return false;
  return base[locals + screen] == azura::menu::kMainMenuScreen &&
         static_cast<int32_t>(LoadU32(base, locals + option)) == quit_option;
}

void RequestQuit() {
  if (g_quit_requested.exchange(true)) return;
  std::function<void()> handler;
  {
    std::lock_guard lock(g_quit_mutex);
    handler = g_quit_handler;
  }
  REXLOG_INFO("[Azura menu] Quit Game chosen");
  if (handler) handler();
}

std::mutex g_seen_mutex;
std::unordered_set<uint32_t> g_seen;  // UFunction addresses already logged

// --- PC options (see menu/pc_options.h) ---
//
// Offsets from the dump of UIScene_Options (docs/reverse-engineering.md, "Options menu").
constexpr uint32_t kFrameObject = 20;
constexpr uint32_t kOptionsCurrentMode = 1028;      // UIScene_Options.CurrentMode (byte)
// CurrentMode while the list of categories is shown (OMI_MAX).
constexpr int kNoCategory = 8;
constexpr uint32_t kPopulateRow = 0;                // PopulateOptionsList local I (row index)
constexpr uint32_t kPopulateSliderValue = 172;      // PopulateOptionsList local Slider_Value
constexpr uint32_t kPopulateListIndex = 176;        // PopulateOptionsList local OptionList_Index
constexpr uint32_t kGetOptionValueAtIndex = 0;      // GetOptionValueAt parameter Index
constexpr uint32_t kOptionsCategories = 1016;       // UIScene_Options.OptionCategories (TArray)
constexpr uint32_t kCategorySize = 64;              // sizeof(OptionCategoryInfo)
constexpr uint32_t kCategoryOptions = 40;           // OptionCategoryInfo.CategoryOptions (TArray)
constexpr uint32_t kCallbackForPsid = 0;            // GetCallbackForOptionPSID parameter OptionPSID
constexpr uint32_t kCallbackNewValue = 0;           // Callback_*WasChanged parameter NewValue (float)

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

// Category the options scene shows (UIScene_Options.CurrentMode; 8 is the list of categories).
int CurrentMode(const uint8_t* base, uint32_t options_scene) {
  return options_scene ? base[options_scene + kOptionsCurrentMode] : -1;
}

constexpr uint32_t kStructScript = 76;  // UStruct::Script, TArray<BYTE>
constexpr uint8_t kTokenLocalVariable = 0x00;
constexpr uint8_t kTokenIntConstByte = 0x2C;

// Cooked Xbox 360 bytecode stores object references (properties, functions) as 8-byte big-endian
// values whose high half is zero; the guest address is the low half.
constexpr uint32_t kScriptReferenceSize = 8;

// Whether the bytecode at `at` is a reference to local variable `property` (token + reference).
bool IsLocalVariable(const uint8_t* base, uint32_t at, uint32_t property) {
  return base[at] == kTokenLocalVariable && LoadU32(base, at + 1) == 0 && LoadU32(base, at + 5) == property;
}

constexpr uint8_t kTokenIntOne = 0x26;
constexpr uint8_t kTokenSelf = 0x17;
constexpr uint8_t kTokenByteConst = 0x24;
constexpr uint8_t kTokenEndFunctionParms = 0x16;
constexpr uint8_t kNativeEqualIntInt = 0x9A;
constexpr uint8_t kNativeGreaterEqualIntInt = 0x99;

// Callback_MenuOptionSelected(SelectedOption) starts with
//   if (SelectedOption == 5 || SelectedOption == 1 || SelectedOption == 7) return;
// because those positions (storage location, Video, Credits) opened another screen instead of a
// list. Now Video is a list, while Controller (4), Keyboard and mouse (5) and Local co-op (6) open
// menus of ours over the list of categories:
//   - `SelectedOption == 5` becomes `SelectedOption >= 4`: the native == (one byte) becomes >= and
//     the 5 (a one-byte int constant) becomes 4;
//   - in `SelectedOption == 1` the one (EX_IntOne, one byte) becomes EX_Self, also one byte: it
//     evaluates to the scene's own address, which is never a position.
// The bytecode keeps its size and its jump offsets. The ones patched are those of this condition
// (from `SelectedOption == 5` on), so an earlier one that only picks a sound is left alone. Done
// once.
void LetListsShow(uint8_t* base, uint32_t function) {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  const uint32_t selected_option = LoadU32(base, function + kStructChildren);
  const uint32_t code = LoadU32(base, function + kStructScript);
  const uint32_t size = LoadU32(base, function + kStructScript + 4);
  constexpr uint32_t kOperand = 2 + kScriptReferenceSize;  // after ==, the token and the reference
  bool after_five = false;
  for (uint32_t i = 0; code && i + kOperand + 2 <= size; ++i) {
    if (base[code + i] != kNativeEqualIntInt || !IsLocalVariable(base, code + i + 1, selected_option)) continue;
    const uint8_t token = base[code + i + kOperand];
    const uint8_t value = base[code + i + kOperand + 1];
    if (!after_five && token == kTokenIntConstByte && value == azura::menu::kKeyboardMouseCategory) {
      base[code + i] = kNativeGreaterEqualIntInt;
      base[code + i + kOperand + 1] = azura::menu::kControllerCategory;
      after_five = true;
    } else if (after_five && token == kTokenIntOne && value == kTokenEndFunctionParms) {
      base[code + i + kOperand] = kTokenSelf;
      REXLOG_INFO("[Azura options] Video shows its list; the categories from Controller on open menus of ours");
      return;
    }
  }
  REXLOG_WARN("[Azura options] Callback_MenuOptionSelected does not skip 5 and 1 as expected");
}

// Callback_ReturnToMain plays the "leave" sounds unless CurrentMode is Video (1) or no category
// (8); the 1 (a byte constant, the first one) becomes 255 so leaving Video sounds like the rest.
void LetVideoPlayLeaveSound(uint8_t* base, uint32_t function) {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  const uint32_t code = LoadU32(base, function + kStructScript);
  const uint32_t size = LoadU32(base, function + kStructScript + 4);
  for (uint32_t i = 0; code && i + 3 <= size; ++i) {
    if (base[code + i] == kTokenByteConst && base[code + i + 1] == azura::menu::kVideoCategory &&
        base[code + i + 2] == kTokenEndFunctionParms) {
      base[code + i + 1] = 0xFF;
      return;
    }
  }
}

// Whether OnConfirm_ResetDefaults, about to run in `frame`, is the player accepting the reset.
bool IsResetAccepted(const uint8_t* base, uint32_t frame) {
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const int32_t alias = ParamOffset(base, function, "SelectedInputAlias");
  return alias >= 0 && NameText(base, LoadU32(base, LoadU32(base, frame + kFrameLocals) + alias)) == "ResetAccept";
}

// PopulateOptionsList reads each row's value from the profile (which does not hold ours: it holds
// the unused settings the rows borrow) into its locals Slider_Value and OptionList_Index, then hands
// them to the row's Flash object. Every call it makes while on one of our rows rewrites those
// locals with the row's real value, so the last write lands right before the call that reads them
// (the call itself is entered here before its parameters are evaluated). `caller` is the frame of
// PopulateOptionsList.
void ShowPcOptionValue(uint8_t* base, uint32_t caller) {
  if (!Is(base, LoadU32(base, caller + kFrameNode), kPopulateOptionsList)) return;
  const uint32_t locals = LoadU32(base, caller + kFrameLocals);
  const int option = azura::menu::FindPcOption(CurrentMode(base, LoadU32(base, caller + kFrameObject)),
                                               static_cast<int32_t>(LoadU32(base, locals + kPopulateRow)));
  if (option < 0 || azura::menu::PcOptionIsAction(option)) return;
  const float value = azura::menu::PcOptionDisplayValue(option);
  const bool slider = azura::menu::PcOptionIsSlider(option);
  const uint32_t address = locals + (slider ? kPopulateSliderValue : kPopulateListIndex);
  const uint32_t bits = slider ? FloatBits(value) : static_cast<uint32_t>(static_cast<int32_t>(value));
  if (LoadU32(base, address) == bits) return;
  StoreU32(base, address, bits);
  REXLOG_DEBUG("[Azura options] Row {} of category {} shows {}", LoadU32(base, locals + kPopulateRow),
               CurrentMode(base, LoadU32(base, caller + kFrameObject)), value);
}

// --- Field of view ---
//
// GearGameplayCamera keeps one camera mode object per situation (GameplayCam_Default, _Cover,
// _Targeting, _RoadieRun, ...), each with its own FOVAngle from GearCamera.ini: 80 by default, 95
// in cover, 50 aiming, 105 roadie running. The mode in use answers GetDesiredFOV(ViewedPawn) from
// its FOVAngle, through the weapon's GetAdjustedFOV or GetTargetingFOV when there is one
// (docs/reverse-engineering.md, "Field of view"). Just before that function runs, FOVAngle is set
// to the mode's own value times field_of_view / 80, so every mode keeps its proportion to the base
// camera and a change in the menu shows on the next frame. Cinematics (Matinee) and a forced field
// of view (Kismet's Set Camera FOV) do not take it from the modes.

// The base camera's field of view in the game (GameplayCam_Default.FOVAngle), which
// field_of_view replaces.
constexpr float kGameFieldOfView = 80.0f;

// A camera mode whose FOVAngle the hook has written: its class, the value the game gave it, and
// the bits written, to tell when the game (or a new mode at a recycled address) has set it again.
struct ScaledFieldOfView {
  uint32_t mode_class;
  float original;
  uint32_t written;
};
std::mutex g_fov_mutex;
std::unordered_map<uint32_t, ScaledFieldOfView> g_scaled_fov;  // by camera mode object
// Offset of FOVAngle by class; -1 for a class that is not a camera mode.
std::unordered_map<uint32_t, int32_t> g_fov_offsets;

// Offset of FOVAngle in objects of `cls` when it derives from the engine's
// GameThirdPersonCameraMode (the gameplay camera modes), else -1.
int32_t CameraModeFovOffset(const uint8_t* base, uint32_t cls) {
  for (uint32_t owner = cls; owner; owner = LoadU32(base, owner + kStructSuper)) {
    if (ObjectName(base, owner) == "GameThirdPersonCameraMode") return ClassPropertyOffset(base, cls, "FOVAngle");
  }
  return -1;
}

// As GetDesiredFOV starts on `mode`: scales its FOVAngle by field_of_view.
void ApplyFieldOfView(uint8_t* base, uint32_t mode) {
  const uint32_t mode_class = LoadU32(base, mode + kObjectClass);
  std::lock_guard lock(g_fov_mutex);
  auto offset = g_fov_offsets.find(mode_class);
  if (offset == g_fov_offsets.end()) {
    offset = g_fov_offsets.emplace(mode_class, CameraModeFovOffset(base, mode_class)).first;
    if (offset->second >= 0) {
      REXLOG_INFO("[Azura fov] Camera mode {}: FOVAngle at +{}", ObjectName(base, mode_class), offset->second);
    }
  }
  if (offset->second < 0) return;
  const uint32_t address = mode + static_cast<uint32_t>(offset->second);
  const uint32_t current = LoadU32(base, address);
  auto [entry, added] = g_scaled_fov.try_emplace(mode);
  ScaledFieldOfView& scaled = entry->second;
  // A mode seen for the first time, or one whose FOVAngle is not what was written last: the value
  // there now is the game's own.
  if (added || scaled.mode_class != mode_class || scaled.written != current) {
    scaled.mode_class = mode_class;
    std::memcpy(&scaled.original, &current, 4);
  }
  const float scale = static_cast<float>(REXCVAR_GET(field_of_view)) / kGameFieldOfView;
  scaled.written = FloatBits(scaled.original * scale);
  if (scaled.written != current) StoreU32(base, address, scaled.written);
}

// As the engine's PlayerInput.PlayerInput returns on `player_input`: undoes field_of_view's share
// of the turn. That function multiplies the right stick's aTurn and aLookUp, and the mouse axes it
// then adds to them, by FOVScale, GetFOVAngle() * 0.01111 (docs/reverse-engineering.md, Mouse
// look), so a wider view would turn faster. Dividing both by field_of_view / 80 leaves the turn
// the player gets for a given mouse motion or stick deflection as with the game's field of view,
// while the slower turn when aiming or zoomed, which comes from those modes' own narrower angles,
// stays. The stick's deceleration keeps its state (LastaTurn, LastaLookup) before FOVScale is
// applied, so scaling the result does not feed back into the next frame.
void CompensateFieldOfView(uint8_t* base, uint32_t player_input) {
  const float scale = static_cast<float>(REXCVAR_GET(field_of_view)) / kGameFieldOfView;
  if (scale == 1.0f) return;
  int32_t turn;
  int32_t look_up;
  {
    std::lock_guard lock(g_mouse_look_mutex);
    const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
    turn = CachedOffset(base, input_class, 9, "aTurn");
    look_up = CachedOffset(base, input_class, 10, "aLookUp");
  }
  if (turn < 0 || look_up < 0) return;
  for (const int32_t offset : {turn, look_up}) {
    const uint32_t address = player_input + static_cast<uint32_t>(offset);
    const uint32_t bits = LoadU32(base, address);
    float value;
    std::memcpy(&value, &bits, 4);
    StoreU32(base, address, FloatBits(value / scale));
  }
}

// --- Film grain ---
//
// The game's film grain is drawn by the UberPostProcessEffect of the post-process chain: the
// player's own (LocalPlayer.PlayerPostProcess, the one drawn, a copy of the engine's with one more
// effect), the engine's default (GearEngine.DefaultPostProcess, War_PostProcess.
// DefaultScenePostProcess) and a level's (WorldInfo.WorldPostProcessChain, when it has one). The
// game ships it with bEnableImageGrain set and SceneImageGrainScale 0.0125, and no script reads or
// writes it (docs/reverse-engineering.md, "Film grain"). With film_grain off, bEnableImageGrain is
// cleared in every one of those effects each frame; on, each effect gets back the value the game
// gave it.

// UBoolProperty::BitMask: the bit of its 32-bit word a bool property is (read from the game: the
// first bool declared in a word is 0x80000000, the next 0x40000000, ...).
constexpr uint32_t kBoolPropertyBitMask = 104;

// An UberPostProcessEffect the hook has seen: its class, to tell a new object at a recycled
// address, and whether the game had its grain on.
struct GrainEffect {
  uint32_t effect_class;
  bool original;
};
// Guarded by g_mouse_look_mutex, like the offsets: ApplyFilmGrain runs from ApplyMouseLookFor.
std::unordered_map<uint32_t, GrainEffect> g_grain_effects;  // by effect object
// The UberPostProcessEffect class and its bEnableImageGrain (word offset and bit), once found.
uint32_t g_grain_class = 0;
int32_t g_grain_offset = -1;
uint32_t g_grain_mask = 0;

// Whether `cls` is UberPostProcessEffect; the first time it is, finds bEnableImageGrain in it.
bool IsGrainEffectClass(const uint8_t* base, uint32_t cls) {
  if (g_grain_class) return cls == g_grain_class;
  if (ObjectName(base, cls) != "UberPostProcessEffect") return false;
  g_grain_class = cls;
  for (uint32_t field = LoadU32(base, cls + kStructChildren); field; field = LoadU32(base, field + kFieldNext)) {
    if (ObjectName(base, field) != "bEnableImageGrain") continue;
    const uint32_t mask = LoadU32(base, field + kBoolPropertyBitMask);
    // A bool's mask has exactly one bit set; anything else means the layout is not the one read.
    if (mask && !(mask & (mask - 1))) {
      g_grain_offset = static_cast<int32_t>(LoadU32(base, field + kPropertyOffset));
      g_grain_mask = mask;
    }
  }
  REXLOG_INFO("[Azura grain] UberPostProcessEffect.bEnableImageGrain at +{}, bit 0x{:08X}", g_grain_offset,
              g_grain_mask);
  return true;
}

void ApplyFilmGrainToChain(uint8_t* base, uint32_t chain, bool enabled) {
  if (!chain) return;
  const int32_t effects_at = CachedOffset(base, LoadU32(base, chain + kObjectClass), 13, "Effects");
  if (effects_at < 0) return;
  const uint32_t data = LoadU32(base, chain + static_cast<uint32_t>(effects_at));
  const uint32_t count = std::min<uint32_t>(LoadU32(base, chain + static_cast<uint32_t>(effects_at) + 4), 64);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t effect = LoadU32(base, data + i * 4);
    if (!effect) continue;
    const uint32_t effect_class = LoadU32(base, effect + kObjectClass);
    if (!IsGrainEffectClass(base, effect_class) || g_grain_offset < 0) continue;
    const uint32_t address = effect + static_cast<uint32_t>(g_grain_offset);
    const uint32_t word = LoadU32(base, address);
    const bool on = (word & g_grain_mask) != 0;
    auto [entry, added] = g_grain_effects.try_emplace(effect, GrainEffect{effect_class, on});
    if (!added && entry->second.effect_class != effect_class) entry->second = {effect_class, on};
    const bool wanted = enabled && entry->second.original;
    if (on != wanted) StoreU32(base, address, wanted ? word | g_grain_mask : word & ~g_grain_mask);
  }
}

// Applies film_grain to the post-process chains `controller` (with its LocalPlayer `local_player`)
// can be drawn with. Called with g_mouse_look_mutex held.
void ApplyFilmGrain(uint8_t* base, uint32_t controller, uint32_t local_player) {
  const bool enabled = REXCVAR_GET(film_grain);
  const uint32_t player_class = LoadU32(base, local_player + kObjectClass);
  const int32_t player_chain_at = CachedOffset(base, player_class, 15, "PlayerPostProcess");
  if (player_chain_at >= 0) {
    ApplyFilmGrainToChain(base, LoadU32(base, local_player + static_cast<uint32_t>(player_chain_at)), enabled);
  }
  const int32_t world_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 7, "WorldInfo");
  const uint32_t world = world_at >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(world_at)) : 0;
  if (world) {
    const int32_t chain_at = CachedOffset(base, LoadU32(base, world + kObjectClass), 14, "WorldPostProcessChain");
    if (chain_at >= 0) ApplyFilmGrainToChain(base, LoadU32(base, world + static_cast<uint32_t>(chain_at)), enabled);
  }
  // The engine is the outer of the player's viewport client (GearGameViewportClient in GearEngine).
  const int32_t viewport_at = CachedOffset(base, player_class, 16, "ViewportClient");
  const uint32_t viewport = viewport_at >= 0 ? LoadU32(base, local_player + static_cast<uint32_t>(viewport_at)) : 0;
  const uint32_t engine = viewport ? LoadU32(base, viewport + kObjectOuter) : 0;
  if (engine) {
    const int32_t chain_at = CachedOffset(base, LoadU32(base, engine + kObjectClass), 17, "DefaultPostProcess");
    if (chain_at >= 0) ApplyFilmGrainToChain(base, LoadU32(base, engine + static_cast<uint32_t>(chain_at)), enabled);
  }
}

// --- Brightness calibration from the Video list ---
//
// The list of a category lives in Flash, which tells the script nothing about the selected row. But
// every button reaches UIScene_Options.FilterButtonInput(ControllerId, ButtonName, InputEvent), and
// Flash starts every list on its first row and wraps around at both ends, so the selected row is
// followed by counting Up and Down presses (D-pad, arrow keys, left stick, W/S). Accept, Left or
// Right on the calibration row then runs Callback_VideoOptions, the game's own function behind the
// original Video entry.
constexpr uint32_t kFilterButtonName = 4;    // FilterButtonInput parameter ButtonName (FName)
constexpr uint32_t kFilterInputEvent = 12;   // FilterButtonInput parameter InputEvent (byte)
constexpr uint8_t kInputPressed = 0;         // EInputEvent: IE_Pressed
constexpr uint8_t kInputRepeat = 2;          // EInputEvent: IE_Repeat
std::atomic<int> g_selected_row{0};
std::atomic<bool> g_calibration_open{false};
std::atomic<bool> g_returning_from_calibration{false};

int VideoRowCount() {
  int count = 0;
  while (azura::menu::FindPcOption(azura::menu::kVideoCategory, count) >= 0) ++count;
  return count;
}

// The function named `name` among the children of class `cls`, or 0.
uint32_t FindClassFunction(const uint8_t* base, uint32_t cls, const std::string& name) {
  for (uint32_t field = LoadU32(base, cls + kStructChildren); field; field = LoadU32(base, field + kFieldNext)) {
    if (ObjectName(base, field) == name) return field;
  }
  return 0;
}

// Runs `function` (a script function without parameters) on the frame that was about to run, or
// has just run, another one of the same object, by pointing the frame at its node and bytecode for
// the time of the call. `locals` replaces the frame's locals when the function has some (the
// frame's own are sized for the other function); 0 keeps them.
void RunInstead(PPCContext& ctx, uint8_t* base, uint32_t function, uint32_t locals = 0) {
  const uint32_t frame = ctx.r4.u32;
  const uint32_t node = LoadU32(base, frame + kFrameNode);
  const uint32_t code = LoadU32(base, frame + kFrameCode);
  const uint32_t own_locals = LoadU32(base, frame + kFrameLocals);
  StoreU32(base, frame + kFrameNode, function);
  StoreU32(base, frame + kFrameCode, LoadU32(base, function + kStructScript));
  if (locals) StoreU32(base, frame + kFrameLocals, locals);
  __imp__sub_822DF9E0(ctx, base);
  StoreU32(base, frame + kFrameNode, node);
  StoreU32(base, frame + kFrameCode, code);
  StoreU32(base, frame + kFrameLocals, own_locals);
}

// --- List rows of our own ---
//
// A raw-value list row (bUsesRawValue) gets its value names from
// UIScene_Options.GetProfileSettingValue_SpecialCase(OptionPSID, out OptionValues), which only
// knows the multiplayer characters and weapons and leaves the array empty for any other ID. For the
// IDs of our own lists (menu/pc_options.h), the script runs bytecode of ours instead of its own,
// `OptionValues[I] = "name"` for each name followed by a return, so the script interpreter grows
// the array and allocates the strings with the game's allocator, which frees them later.
constexpr uint8_t kTokenLet = 0x0F;
constexpr uint8_t kTokenDynArrayElement = 0x10;
constexpr uint8_t kTokenIntConst = 0x1D;
constexpr uint8_t kTokenUnicodeStringConst = 0x34;
constexpr uint8_t kTokenReturn = 0x04;
constexpr uint8_t kTokenNothing = 0x0B;
constexpr uint8_t kTokenEndOfScript = 0x53;
constexpr uint32_t kSpecialCasePsid = 0;  // GetProfileSettingValue_SpecialCase parameter OptionPSID

std::mutex g_value_names_mutex;
std::map<std::u16string, uint32_t> g_value_names_code;  // guest bytecode for each list of names

// The property `name` among the children of `function`, or 0.
uint32_t FindFunctionProperty(const uint8_t* base, uint32_t function, const std::string& name) {
  for (uint32_t prop = LoadU32(base, function + kStructChildren); prop; prop = LoadU32(base, prop + kFieldNext)) {
    if (ObjectName(base, prop) == name) return prop;
  }
  return 0;
}

// The token the function's own bytecode uses to read `property` (the byte before its first
// reference, an 8-byte big-endian object pointer), or 0 when it never refers to it.
uint8_t PropertyToken(const uint8_t* base, uint32_t function, uint32_t property) {
  const uint32_t code = LoadU32(base, function + kStructScript);
  const uint32_t size = LoadU32(base, function + kStructScript + 4);
  for (uint32_t i = 1; i + 8 <= size; ++i) {
    if (LoadU32(base, code + i) == 0 && LoadU32(base, code + i + 4) == property) return base[code + i - 1];
  }
  return 0;
}

// Guest bytecode that fills OptionValues with `names`, built once per list of names.
uint32_t ValueNamesCode(uint8_t* base, uint32_t function, const std::vector<std::u16string>& names) {
  std::u16string key;
  for (const auto& name : names) key += name + u'\0';
  std::lock_guard lock(g_value_names_mutex);
  if (const auto found = g_value_names_code.find(key); found != g_value_names_code.end()) return found->second;
  const uint32_t values = FindFunctionProperty(base, function, "OptionValues");
  const uint8_t values_token = values ? PropertyToken(base, function, values) : 0;
  if (!values_token) {
    REXLOG_WARN("[Azura options] GetProfileSettingValue_SpecialCase is not as expected; no value names");
    return 0;
  }
  std::vector<uint8_t> code;
  auto u32 = [&code](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) code.push_back(static_cast<uint8_t>(v >> shift));
  };
  for (size_t i = 0; i < names.size(); ++i) {
    code.push_back(kTokenLet);
    code.push_back(kTokenDynArrayElement);
    code.push_back(kTokenIntConst);
    u32(static_cast<uint32_t>(i));
    code.push_back(values_token);
    u32(0);
    u32(values);
    // UTF-16, big-endian like the rest of the console's bytecode, zero-terminated.
    code.push_back(kTokenUnicodeStringConst);
    for (char16_t c : names[i]) {
      code.push_back(static_cast<uint8_t>(c >> 8));
      code.push_back(static_cast<uint8_t>(c));
    }
    code.push_back(0);
    code.push_back(0);
  }
  code.push_back(kTokenReturn);
  code.push_back(kTokenNothing);
  code.push_back(kTokenEndOfScript);
  const uint32_t address = rex::system::kernel_state()->memory()->SystemHeapAlloc(static_cast<uint32_t>(code.size()));
  if (!address) return 0;
  std::memcpy(base + address, code.data(), code.size());
  g_value_names_code.emplace(key, address);
  REXLOG_INFO("[Azura options] Value names bytecode for {} names ({} bytes, OptionValues token 0x{:02X})",
              names.size(), code.size(), values_token);
  return address;
}

// Runs our bytecode in place of GetProfileSettingValue_SpecialCase when the row is one of our own
// lists. Returns false when the script's own code must run.
bool FillOwnValueNames(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const uint32_t frame = ctx.r4.u32;
  const int32_t psid = static_cast<int32_t>(LoadU32(base, LoadU32(base, frame + kFrameLocals) + kSpecialCasePsid));
  std::vector<std::u16string> names;
  if (!azura::menu::PcOptionValueNames(psid, names) || names.empty()) return false;
  const uint32_t code = ValueNamesCode(base, function, names);
  if (!code) return false;
  const uint32_t own_code = LoadU32(base, frame + kFrameCode);
  StoreU32(base, frame + kFrameCode, code);
  __imp__sub_822DF9E0(ctx, base);
  StoreU32(base, frame + kFrameCode, own_code);
  return true;
}

// --- Restart message box ---
//
// When the player leaves a category of the Options menu (from the main menu) with a new value for
// an option that only applies at start-up, the game's own message box offers to restart. It is the
// one Defaults opens: Callback_ResetDefaults builds the button list ('ResetAccept',
// 'GenericCancel'), then calls ShowUIMessage with the texts Localize("MessageBoxStrings",
// "ResetOptions_Title" / "ResetOptions_Message", "GearGameUI") and OnConfirm_ResetDefaults as the
// answer's delegate. For the time of one call, its bytecode is changed in place, every change
// keeping the size: the two key names become ours (menu/pc_options.h, texts added to the game's
// localization) and the 'ResetAccept' alias becomes 'GenericAccept' ("Accept"). The answer then
// reaches OnConfirm_ResetDefaults with g_restart_dialog set, which restarts on Accept and does not
// reset anything either way.
constexpr uint8_t kTokenStringConst = 0x1F;
constexpr uint8_t kTokenNameConst = 0x21;
constexpr uint32_t kResetDefaultsLocalsSize = 64;  // ButtonAliases (a 12-byte array), with room
std::atomic<bool> g_restart_dialog{false};
uint32_t g_dialog_locals = 0;  // guest memory for Callback_ResetDefaults' locals

// Offset of the zero-terminated string token holding `text` in `function`'s bytecode, or -1.
int64_t FindStringConst(const uint8_t* base, uint32_t code, uint32_t size, const std::string& text) {
  for (uint32_t i = 0; i + text.size() + 2 <= size; ++i) {
    if (base[code + i] == kTokenStringConst && std::memcmp(base + code + i + 1, text.data(), text.size()) == 0 &&
        base[code + i + 1 + text.size()] == 0) {
      return i + 1;
    }
  }
  return -1;
}

// Offset of the name token holding FName `index` in the bytecode, or -1.
int64_t FindNameConst(const uint8_t* base, uint32_t code, uint32_t size, uint32_t index) {
  for (uint32_t i = 0; i + 9 <= size; ++i) {
    if (base[code + i] == kTokenNameConst && LoadU32(base, code + i + 1) == index &&
        LoadU32(base, code + i + 5) == 0) {
      return i + 1;
    }
  }
  return -1;
}

// Guest memory for the locals and the return value of IsInFrontEnd (four 4-byte locals).
constexpr uint32_t kFrontEndCallSize = 32;
uint32_t g_front_end_call = 0;

// Whether the options scene in `ctx` (the frame of Callback_ReturnToMain, which has just run) was
// opened from the main menu. The game's own UIScene_Options.IsInFrontEnd decides it: the player's
// HUD is the menu HUD and shows the front end movie. Its UBOOL return value is written to r5.
bool IsInFrontEnd(PPCContext& ctx, uint8_t* base, uint32_t return_to_main) {
  const uint32_t is_in_front_end =
      FindClassFunction(base, LoadU32(base, return_to_main + kObjectOuter), "IsInFrontEnd");
  if (!is_in_front_end) return false;
  if (!g_front_end_call) {
    g_front_end_call = rex::system::kernel_state()->memory()->SystemHeapAlloc(kFrontEndCallSize);
    if (!g_front_end_call) return false;
  }
  std::memset(base + g_front_end_call, 0, kFrontEndCallSize);
  const uint32_t result = g_front_end_call + kFrontEndCallSize - 4;
  const uint64_t own_result = ctx.r5.u64;
  ctx.r5.u64 = result;
  RunInstead(ctx, base, is_in_front_end, g_front_end_call);
  ctx.r5.u64 = own_result;
  return LoadU32(base, result) != 0;
}

// Opens the restart message box. `ctx` holds the frame of Callback_ReturnToMain, which has just
// run, on the options scene; `return_to_main` is that function (its class has the Defaults one).
void OpenRestartDialog(PPCContext& ctx, uint8_t* base, uint32_t return_to_main) {
  const int32_t reset_accept = g_names[kResetAcceptAlias].load();
  const int32_t generic_accept = g_names[kGenericAcceptAlias].load();
  const uint32_t reset_defaults =
      FindClassFunction(base, LoadU32(base, return_to_main + kObjectOuter), "Callback_ResetDefaults");
  if (!reset_defaults || reset_accept < 0 || generic_accept < 0) {
    REXLOG_WARN("[Azura options] The restart message box is not available");
    return;
  }
  const uint32_t code = LoadU32(base, reset_defaults + kStructScript);
  const uint32_t size = LoadU32(base, reset_defaults + kStructScript + 4);
  const int64_t title = FindStringConst(base, code, size, "ResetOptions_Title");
  const int64_t message = FindStringConst(base, code, size, "ResetOptions_Message");
  const int64_t alias = FindNameConst(base, code, size, static_cast<uint32_t>(reset_accept));
  if (title < 0 || message < 0 || alias < 0) {
    REXLOG_WARN("[Azura options] Callback_ResetDefaults is not as expected; no restart message box");
    return;
  }
  if (!g_dialog_locals) {
    g_dialog_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kResetDefaultsLocalsSize);
    if (!g_dialog_locals) return;
  }
  // A fresh, empty button list every time (the previous one, a few bytes, is left behind).
  std::memset(base + g_dialog_locals, 0, kResetDefaultsLocalsSize);

  const std::string title_key = azura::menu::kRestartTitleKey;
  const std::string message_key = azura::menu::kRestartMessageKey;
  std::memcpy(base + code + title, title_key.data(), title_key.size());
  std::memcpy(base + code + message, message_key.data(), message_key.size());
  StoreU32(base, code + alias, static_cast<uint32_t>(generic_accept));
  g_restart_dialog.store(true);
  RunInstead(ctx, base, reset_defaults, g_dialog_locals);
  std::memcpy(base + code + title, "ResetOptions_Title", title_key.size());
  std::memcpy(base + code + message, "ResetOptions_Message", message_key.size());
  StoreU32(base, code + alias, static_cast<uint32_t>(reset_accept));
  REXLOG_INFO("[Azura options] Offered to restart the game to apply Video settings");
}

// Guest memory for the locals of UIScene_FrontEnd.Callback_StartButtonPressed (ControllerIdx,
// OpeningPlayer, LP).
constexpr uint32_t kStartButtonLocalsSize = 16;
uint32_t g_start_button_locals = 0;

// Opens the Options from the main menu. `ctx` holds the frame of HandleMenuSelection, about to run
// on the front end scene, whose Callback_StartButtonPressed (what Start does on the main menu)
// opens them for the controller that chose the option, which has to be a player's.
void OpenOptionsFromMainMenu(PPCContext& ctx, uint8_t* base, uint32_t handle_menu_selection) {
  const uint32_t start_pressed =
      FindClassFunction(base, LoadU32(base, handle_menu_selection + kObjectOuter), "Callback_StartButtonPressed");
  if (!start_pressed) {
    REXLOG_WARN("[Azura menus] Callback_StartButtonPressed not found; the Options option does nothing");
    return;
  }
  if (!g_start_button_locals) {
    g_start_button_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kStartButtonLocalsSize);
    if (!g_start_button_locals) return;
  }
  std::memset(base + g_start_button_locals, 0, kStartButtonLocalsSize);
  StoreU32(base, g_start_button_locals, static_cast<uint32_t>(g_front_end_controller.load()));  // ControllerIdx
  REXLOG_INFO("[Azura menus] Opening the Options from the main menu");
  RunInstead(ctx, base, start_pressed, g_start_button_locals);
}

void RequestRestart() {
  std::function<void()> handler;
  {
    std::lock_guard lock(g_quit_mutex);
    handler = g_restart_handler;
  }
  REXLOG_INFO("[Azura options] Restarting the game to apply Video settings");
  if (handler) handler();
}

// --- The title screen in local co-op ---
//
// When player 2 signs in, the game goes back to the title screen ("press Start"), which is the
// main menu's own scene, UIScene_FrontEnd, with bOnTitleScreen set. There, whoever presses Start
// becomes the primary player (FilterButtonInput calls BecomePrimaryPlayer for its ControllerId),
// so player 2 pressing it would start the game on their own, empty profile instead of player 1's.
// On the title screen only player 1 (ControllerId 0, the PlayerAssignment's user 0) may start.
constexpr uint32_t kFrontEndFlags = 1000;  // UIScene_FrontEnd: the word of its bools (verified, dump)
// bOnTitleScreen, the first bool declared in that word: the Xbox 360 build gives it the highest bit
// (verified: the word reads 88000000 on the title screen and 08000000 in the main menu).
constexpr uint32_t kOnTitleScreenBit = 0x80000000;

// Whether the press about to reach UIScene_FrontEnd.FilterButtonInput is another player's Start or
// Accept on the title screen; then it is used up so that only player 1 starts the game.
bool IgnoreOtherPlayersStart(PPCContext& ctx, uint8_t* base, uint32_t function) {
  if (ObjectName(base, LoadU32(base, function + kObjectOuter)) != "UIScene_FrontEnd") return false;
  const uint32_t scene = ctx.r3.u32;
  const uint32_t flags = LoadU32(base, scene + kFrontEndFlags);
  if (!(flags & kOnTitleScreenBit)) return false;
  const int32_t controller = SmallParam(base, function, ctx.r4.u32, "ControllerId");
  if (controller <= 0) return false;
  const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
  const std::string button = NameText(base, LoadU32(base, locals + kFilterButtonName));
  if (button != "XboxTypeS_Start" && button != "XboxTypeS_A") return false;
  if (base[locals + kFilterInputEvent] == kInputPressed) {
    REXLOG_INFO("[Azura co-op] Title screen: player {} pressed {}; only player 1 starts", controller + 1,
                button);
  }
  if (const uint32_t result = ctx.r5.u32) StoreU32(base, result, 1);  // the press was handled
  return true;
}

// Return and Escape both press Start (the SDK's keyboard and mouse driver cannot tell a menu from
// play): pausing in play, and taken as Start by every menu. A press of either that reaches a menu's
// FilterButtonInput is a menu's, so there it is used up and the driver presses A for Return
// (accept) or B for Escape (back) instead, as on a PC. On the title screen Return stays Start,
// which starts, and Escape does nothing. The press's repeats and release are used up too, even once the key is
// up. Each player's (ControllerId's) press is followed on its own. Returns whether the press was
// used up, so FilterButtonInput must not run.
std::atomic<bool> g_start_taken[4];
bool TakeKeyboardStartAsMenuKey(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
  if (NameText(base, LoadU32(base, locals + kFilterButtonName)) != "XboxTypeS_Start") return false;
  const int32_t controller = SmallParam(base, function, ctx.r4.u32, "ControllerId");
  if (controller < 0 || controller >= 4) return false;
  const uint8_t event = base[locals + kFilterInputEvent];
  if (event == kInputPressed) {
    using rex::input::mnk::StartKey;
    const StartKey key = rex::input::mnk::HeldStartKey();
    const std::string scene = ObjectName(base, LoadU32(base, function + kObjectOuter));
    const bool front_end = scene == "UIScene_FrontEnd";
    const bool title_screen = front_end && (LoadU32(base, ctx.r3.u32 + kFrontEndFlags) & kOnTitleScreenBit);
    const bool keep_start = key == StartKey::kNone || (title_screen && key == StartKey::kReturn);
    g_start_taken[controller].store(!keep_start);
    if (keep_start) return false;
    if (title_screen) {
      // Escape on the title screen: used up, nothing else.
      if (const uint32_t result = ctx.r5.u32) StoreU32(base, result, 1);
      return true;
    }
    rex::input::mnk::PressForMenu(key == StartKey::kReturn ? rex::input::X_INPUT_GAMEPAD_A : rex::input::X_INPUT_GAMEPAD_B);
    REXLOG_INFO("[Azura menus] {} takes the keyboard's {} as {}", scene, key == StartKey::kReturn ? "Return" : "Escape",
                key == StartKey::kReturn ? "A" : "B");
  } else if (!g_start_taken[controller].load()) {
    return false;
  } else if (event != kInputRepeat) {
    g_start_taken[controller].store(false);  // the release
  }
  if (const uint32_t result = ctx.r5.u32) StoreU32(base, result, 1);  // the press was handled
  return true;
}

// Handles a button the options scene got. Returns true when it opened the calibration screen, in
// which case FilterButtonInput must not run (the press is used up).
bool HandleOptionsButton(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const uint32_t scene = ctx.r3.u32;
  if (ObjectName(base, LoadU32(base, function + kObjectOuter)) != "UIScene_Options") return false;
  if (CurrentMode(base, scene) != azura::menu::kVideoCategory || g_calibration_open.load()) return false;
  const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
  const uint8_t event = base[locals + kFilterInputEvent];
  if (event != kInputPressed && event != kInputRepeat) return false;
  const std::string button = NameText(base, LoadU32(base, locals + kFilterButtonName));
  const int rows = VideoRowCount();
  if (button == "XboxTypeS_DPad_Down" || button == "Gamepad_LeftStick_Down") {
    g_selected_row.store((g_selected_row.load() + 1) % rows);
    return false;
  }
  if (button == "XboxTypeS_DPad_Up" || button == "Gamepad_LeftStick_Up") {
    g_selected_row.store((g_selected_row.load() + rows - 1) % rows);
    return false;
  }
  // Accept, or Left/Right (which would otherwise step through the row's only value), opens it.
  const bool opens = button == "XboxTypeS_A" || button == "XboxTypeS_DPad_Left" || button == "XboxTypeS_DPad_Right" ||
                     button == "Gamepad_LeftStick_Left" || button == "Gamepad_LeftStick_Right";
  if (!opens || event != kInputPressed || g_selected_row.load() != azura::menu::kCalibrateRow) return false;
  const uint32_t video_options = FindClassFunction(base, LoadU32(base, function + kObjectOuter), "Callback_VideoOptions");
  if (!video_options) {
    REXLOG_WARN("[Azura options] Callback_VideoOptions not found; brightness calibration unavailable");
    return false;
  }
  const uint32_t result = ctx.r5.u32;
  g_calibration_open.store(true);
  REXLOG_INFO("[Azura options] Opening the brightness calibration screen");
  RunInstead(ctx, base, video_options);
  if (result) StoreU32(base, result, 1);  // FilterButtonInput's ReturnValue: the press was handled
  return true;
}

// Removes every ?listen (or &listen) option from a travel URL.
static void StripListenParam(std::string& s) {
  std::string lower = s;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  size_t pos = 0;
  while ((pos = lower.find("listen", pos)) != std::string::npos) {
    if (pos == 0 || (s[pos - 1] != '?' && s[pos - 1] != '&')) {
      pos += 6;
      continue;
    }

    const size_t after = pos + 6;
    if (after < s.size() && s[after] != '?' && s[after] != '&' && s[after] != '#') {
      pos = after;
      continue;
    }

    const size_t start = pos - 1;
    if (s[start] == '?' && after < s.size() && (s[after] == '?' || s[after] == '&')) {
      s.replace(start, after - start + 1, "?");
      lower.replace(start, after - start + 1, "?");
    } else {
      s.erase(start, after - start);
      lower.erase(start, after - start);
    }
    pos = start;
  }
}

// Campaign travel URLs (GearGame_P?...?chapter=N) carry ?listen, which makes the game a network
// host for co-op; on macOS the load then hangs, with the main and guest worker threads waiting on
// each other. The option is removed so the chapter loads as single player. GearGame_P itself is
// kept: it is the container map that stays loaded for the whole campaign, and each chapter's map
// (sp_hanover_p, sp_warehouse_p...) is loaded into it. When one chapter flows into the next
// without a loading screen, the game swaps those chapter maps inside GearGame_P with
// PrepareMapChange and CommitMapChange, which cannot work if a chapter map is the main map.
// A System Link host keeps ?listen (rex::system::xam::HostingSystemLinkSession): without it the
// game accepts no clients.
bool PatchTravelURL(uint8_t* base, uint32_t fstring_addr) {
  if (!fstring_addr) return false;
  const uint32_t data = LoadU32(base, fstring_addr);
  const uint32_t count = LoadU32(base, fstring_addr + 4);
  const uint32_t max = LoadU32(base, fstring_addr + 8);
  if (!data || count <= 1) return false;
  if (max == 0 || count > max) {
    REXLOG_WARN("[Azura travel] Invalid FString capacity (count={}, max={}); URL left unchanged", count, max);
    return false;
  }

  // FString: UTF-16BE characters and a terminating zero, counted in `count`.
  std::string url;
  url.reserve(count);
  for (uint32_t i = 0; i + 1 < count; ++i) {
    const uint16_t w = static_cast<uint16_t>(base[data + i * 2] << 8 | base[data + i * 2 + 1]);
    url.push_back(w < 128 ? static_cast<char>(w) : '?');
  }

  std::string lower_url = url;
  for (char& c : lower_url) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower_url.rfind("geargame_p", 0) != 0) {
    return false;
  }

  std::string new_url = url;
  if (rex::system::xam::HostingSystemLinkSession()) {
    REXLOG_INFO("[Azura campaign] System Link host: travel URL keeps listen: '{}'", url);
  } else {
    StripListenParam(new_url);
  }
  // RAAM's Shadow, chosen from the campaign menu: its chapters are 31 to 35. A campaign of the main
  // game (a new one, its chapter selection, or its checkpoint to continue) starts the DLC's first.
  if (g_dlc_campaign_chosen.load(std::memory_order_relaxed)) {
    const size_t at = new_url.find("chapter=");
    if (at != std::string::npos) {
      size_t end = at + 8;
      while (end < new_url.size() && std::isdigit(static_cast<unsigned char>(new_url[end]))) ++end;
      const int chapter = end > at + 8 ? std::stoi(new_url.substr(at + 8, end - at - 8)) : 0;
      if (chapter < kFirstDlcChapter) {
        new_url.replace(at + 8, end - at - 8, std::to_string(kFirstDlcChapter));
        const size_t checkpoint = new_url.find("?loadcheckpoint");
        if (checkpoint != std::string::npos) new_url.erase(checkpoint, 15);
      }
    }
  }
  if (new_url == url || new_url.size() + 1 > max) return false;

  // The URL fits in the buffer (checked against its capacity above).
  for (size_t i = 0; i < new_url.size(); ++i) {
    base[data + i * 2] = 0;
    base[data + i * 2 + 1] = static_cast<uint8_t>(new_url[i]);
  }
  base[data + new_url.size() * 2] = 0;
  base[data + new_url.size() * 2 + 1] = 0;
  StoreU32(base, fstring_addr + 4, static_cast<uint32_t>(new_url.size() + 1));
  REXLOG_INFO("[Azura campaign] Travel URL: '{}' -> '{}'", url, new_url);
  return true;
}

// --- Rows that depend on other rows ---
//
// Some rows of ours are only shown with certain values of others (the sharpness row only with an
// image scaling effect that sharpens). The list is built from OptionCategories[CurrentMode].
// CategoryOptions, filled with every row of ours from the localization file. Before the list is
// built, the array's elements are put in the order PcOptionsArrangedPsids gives (the shown rows,
// then the hidden ones), and its number of rows (its TArray count) is lowered to leave the hidden
// ones out, or raised again over the same elements to show them, while its allocation holds them.
constexpr uint32_t kOptionInfoSize = 48;  // sizeof(OptionInfo), ProfileSettingId at +0

void SetShownRows(uint8_t* base, uint32_t scene, int category) {
  const uint32_t categories = LoadU32(base, scene + kOptionsCategories);
  if (!categories || category < 0 || static_cast<uint32_t>(category) >= LoadU32(base, scene + kOptionsCategories + 4)) {
    return;
  }
  const uint32_t options = categories + static_cast<uint32_t>(category) * kCategorySize + kCategoryOptions;
  const uint32_t shown = static_cast<uint32_t>(azura::menu::PcOptionsShownRows(category));
  if (shown == 0) return;
  const std::vector<int> psids = azura::menu::PcOptionsArrangedPsids(category);
  if (psids.size() > LoadU32(base, options + 8)) {
    REXLOG_WARN("[Azura options] Category {} holds fewer rows than it has ({})", category, psids.size());
    return;
  }
  // Each element moves whole (its strings' pointers go with it). A category whose elements are not
  // exactly our options keeps its order.
  const uint32_t data = LoadU32(base, options);
  const auto psid_at = [&](size_t i) { return static_cast<int32_t>(LoadU32(base, data + i * kOptionInfoSize)); };
  std::vector<int> held(psids.size());
  for (size_t i = 0; i < psids.size(); ++i) held[i] = psid_at(i);
  if (data && std::is_permutation(held.begin(), held.end(), psids.begin())) {
    for (size_t i = 0; i < psids.size(); ++i) {
      if (psid_at(i) == psids[i]) continue;
      size_t from = i + 1;
      while (psid_at(from) != psids[i]) ++from;
      uint8_t element[kOptionInfoSize];
      std::memcpy(element, base + data + i * kOptionInfoSize, kOptionInfoSize);
      std::memcpy(base + data + i * kOptionInfoSize, base + data + from * kOptionInfoSize, kOptionInfoSize);
      std::memcpy(base + data + from * kOptionInfoSize, element, kOptionInfoSize);
    }
  }
  if (LoadU32(base, options + 4) == shown) return;
  StoreU32(base, options + 4, shown);
  REXLOG_DEBUG("[Azura options] Category {} shows {} rows", category, shown);
}

// Live changes. A row with bHasOnChangeCallback gets the callback GetCallbackForOptionPSID returns
// for its profile setting, and Flash runs it every time the row's value changes. Rows of ours
// borrow two callbacks of game rows that the Video list no longer has (see
// PcOptionLiveCallbackPsid): the profile setting ID is swapped as GetCallbackForOptionPSID starts.
void BorrowLiveCallback(uint8_t* base, uint32_t frame) {
  const uint32_t psid_address = LoadU32(base, frame + kFrameLocals) + kCallbackForPsid;
  const int borrowed = azura::menu::PcOptionLiveCallbackPsid(static_cast<int32_t>(LoadU32(base, psid_address)));
  if (borrowed >= 0) StoreU32(base, psid_address, static_cast<uint32_t>(borrowed));
}

// Guest bytecode `PopulateOptionsList(); return;`, built once.
uint32_t g_rebuild_list_code = 0;

// Builds the current category's list again from the borrowed callback about to run (its frame in
// ctx.r4), keeping the selection on row `row`: PopulateOptionsList ends with
// `OptionsList.selectedIndex = 0.0`, whose float constant is `row` for the time of the call.
void RebuildList(PPCContext& ctx, uint8_t* base, uint32_t function, int row) {
  const uint32_t populate = FindClassFunction(base, LoadU32(base, function + kObjectOuter), "PopulateOptionsList");
  if (!populate) {
    REXLOG_WARN("[Azura options] PopulateOptionsList not found; the list is not rebuilt");
    return;
  }
  if (!g_rebuild_list_code) {
    constexpr uint8_t kTokenFinalFunction = 0x1C;
    constexpr uint8_t kTokenEndFunctionParms = 0x16;
    const uint8_t code[] = {kTokenFinalFunction, 0, 0, 0, 0, 0, 0, 0, 0, kTokenEndFunctionParms,
                            kTokenReturn, kTokenNothing, kTokenEndOfScript};
    const uint32_t address = rex::system::kernel_state()->memory()->SystemHeapAlloc(sizeof(code));
    if (!address) return;
    std::memcpy(base + address, code, sizeof(code));
    StoreU32(base, address + 5, populate);
    g_rebuild_list_code = address;
  }
  // The selection constant, found by the string before it: "selectedIndex", then a float token.
  const uint32_t populate_code = LoadU32(base, populate + kStructScript);
  const uint32_t populate_size = LoadU32(base, populate + kStructScript + 4);
  const int64_t text = FindStringConst(base, populate_code, populate_size, "selectedIndex");
  constexpr uint8_t kTokenFloatConst = 0x1E;
  const uint32_t selection = text < 0 ? 0 : populate_code + static_cast<uint32_t>(text) + sizeof("selectedIndex");
  const bool keep_selection = selection && selection + 5 <= populate_code + populate_size &&
                              base[selection] == kTokenFloatConst && LoadU32(base, selection + 1) == 0;
  if (keep_selection) StoreU32(base, selection + 1, FloatBits(static_cast<float>(row)));
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_code = LoadU32(base, frame + kFrameCode);
  StoreU32(base, frame + kFrameCode, g_rebuild_list_code);
  __imp__sub_822DF9E0(ctx, base);
  StoreU32(base, frame + kFrameCode, own_code);
  if (keep_selection) StoreU32(base, selection + 1, 0);
  g_selected_row.store(keep_selection ? row : 0);
  REXLOG_INFO("[Azura options] List rebuilt with {} rows, selection on row {}",
              azura::menu::PcOptionsShownRows(CurrentMode(base, ctx.r3.u32)), keep_selection ? row : 0);
}

// Runs our code in place of a borrowed callback (see BorrowLiveCallback) when it comes from a row
// of ours. Returns false when the game's own callback must run.
bool HandleLiveChange(PPCContext& ctx, uint8_t* base, uint32_t function, int borrowed_psid) {
  if (ObjectName(base, LoadU32(base, function + kObjectOuter)) != "UIScene_Options") return false;
  const int category = CurrentMode(base, ctx.r3.u32);
  const uint32_t bits = LoadU32(base, LoadU32(base, ctx.r4.u32 + kFrameLocals) + kCallbackNewValue);
  float value;
  std::memcpy(&value, &bits, 4);
  int row = 0;
  bool rows_changed = false;
  if (!azura::menu::PcOptionLiveChange(category, borrowed_psid, value, row, rows_changed)) return false;
  if (rows_changed) RebuildList(ctx, base, function, row);
  return true;
}

}  // namespace

extern "C" void sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base) {
  // A Super call of RAAM's Shadow to a parent function the disc build lacks (dlc/missing_super.h).
  if (ctx.r6.u32 == 0 && azura::dlc::HandleMissingSuper(ctx, base)) return;
  ShowPcOptionValue(base, ctx.r4.u32);
  if (!REXCVAR_GET(script_dump_classes).empty()) {
    azura::script::StartClassDump(base, REXCVAR_GET(script_dump_classes), REXCVAR_GET(script_dump_code));
    if (ctx.r6.u32) azura::script::NoteClass(LoadU32(base, ctx.r6.u32 + kObjectOuter));
    if (ctx.r3.u32) azura::script::NoteClass(LoadU32(base, ctx.r3.u32 + kObjectClass));
  }
  if (REXCVAR_GET(script_trace)) {
    const uint32_t function = ctx.r6.u32;
    bool first = false;
    {
      std::lock_guard lock(g_seen_mutex);
      first = function != 0 && g_seen.insert(function).second;
    }
    if (first) {
      const uint32_t outer = LoadU32(base, function + kObjectOuter);
      REXLOG_INFO("[Azura script] {}.{}", ObjectName(base, outer), ObjectName(base, function));
    }
  }
  __imp__sub_822DF4D0(ctx, base);
}

namespace azura::script {

void SetQuitHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_quit_handler = std::move(handler);
}

void SetRestartHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_restart_handler = std::move(handler);
}

void SetLocalCoopHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_local_coop_handler = std::move(handler);
}

void SetControlsMenuHandler(std::function<void(ControlsMenu)> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_controls_handler = std::move(handler);
}

namespace {

// OnlineProfileSettings.ProfileSettings is a TArray of OnlineProfileSetting: Owner (a byte), then
// a SettingsProperty: PropertyId, then its SettingsData: Type (a byte), Value1, Value2.
constexpr uint32_t kSettingId = 4;
constexpr uint32_t kSettingValue = 12;

// The address of profile setting `id` of guest user `user`, 0 when it cannot be found (the player
// has no controller yet, or no profile). The element size is found from the array itself: the IDs
// of consecutive settings are small numbers, and 368 (look sensitivity) is among them.
uint32_t ProfileSettingAddress(uint32_t user, int id, uint8_t*& base) {
  base = g_guest_base.load(std::memory_order_relaxed);
  const uint32_t controller = user < 4 ? g_player_controllers[user].load(std::memory_order_relaxed) : 0;
  if (!base || !controller) return 0;
  std::lock_guard lock(g_mouse_look_mutex);
  const int32_t profile_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 5, "ProfileSettings");
  if (profile_at < 0) return 0;
  const uint32_t profile = LoadU32(base, controller + static_cast<uint32_t>(profile_at));
  if (!profile) return 0;
  const int32_t array_at = CachedOffset(base, LoadU32(base, profile + kObjectClass), 6, "ProfileSettings");
  if (array_at < 0) return 0;
  const uint32_t data = LoadU32(base, profile + static_cast<uint32_t>(array_at));
  const uint32_t count = LoadU32(base, profile + static_cast<uint32_t>(array_at) + 4);
  if (!data || count == 0 || count > 1024) return 0;
  for (const uint32_t size : {24u, 20u, 28u}) {
    bool has_368 = false;
    bool plausible = true;
    for (uint32_t i = 0; i < count && plausible; ++i) {
      const uint32_t setting = LoadU32(base, data + i * size + kSettingId);
      plausible = setting < 100000;
      has_368 = has_368 || setting == 368;
    }
    if (!plausible || !has_368) continue;
    for (uint32_t i = 0; i < count; ++i) {
      if (LoadU32(base, data + i * size + kSettingId) == static_cast<uint32_t>(id)) {
        return data + i * size + kSettingValue;
      }
    }
    return 0;
  }
  return 0;
}

}  // namespace

Menus WhereInMenus() {
  if (g_front_end.load(std::memory_order_relaxed)) {
    if (g_options_open.load(std::memory_order_relaxed)) return Menus::kMenu;
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    const uint32_t scene = g_front_end_scene.load(std::memory_order_relaxed);
    if (base && scene && (LoadU32(base, scene + kFrontEndFlags) & kOnTitleScreenBit)) return Menus::kTitleScreen;
    return Menus::kMenu;
  }
  return g_paused.load(std::memory_order_relaxed) || g_options_open.load(std::memory_order_relaxed) ? Menus::kMenu
                                                                                                      : Menus::kPlaying;
}

bool ReadProfileSetting(uint32_t user, int id, uint32_t& value) {
  uint8_t* base = nullptr;
  const uint32_t address = ProfileSettingAddress(user, id, base);
  if (!address) return false;
  value = LoadU32(base, address);
  return true;
}

bool WriteProfileSetting(uint32_t user, int id, uint32_t value) {
  uint8_t* base = nullptr;
  const uint32_t address = ProfileSettingAddress(user, id, base);
  if (!address) return false;
  StoreU32(base, address, value);
  return true;
}

}  // namespace azura::script

// The game's fatal error handler (appErrorf), logged with its message arguments (ASCII or UTF-16)
// before it parks the thread: otherwise the game just stops, with nothing in the log (a missing
// script function, a failed import...).
static std::string GuestText(const uint8_t* base, uint32_t address) {
  if (address < 0x40000000u || address >= 0xA0000000u) return "";
  std::string ascii, wide;
  for (uint32_t i = 0; i < 400; ++i) {
    const uint8_t c = base[address + i];
    if (c == 0) break;
    ascii.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '.');
  }
  for (uint32_t i = 0; i < 400; ++i) {
    const uint8_t hi = base[address + i * 2], lo = base[address + i * 2 + 1];
    if (hi == 0 && lo == 0) break;
    wide.push_back(hi == 0 && lo >= 32 && lo < 127 ? static_cast<char>(lo) : '.');
  }
  return wide.size() > ascii.size() ? wide : ascii;
}

extern "C" void __imp__sub_82319958(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82319958(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_ERROR("[Azura] fatal error handler: r3 '{}' r4 '{}' r5 '{}' r6 '{}' (lr {:08X})",
               GuestText(base, ctx.r3.u32), GuestText(base, ctx.r4.u32), GuestText(base, ctx.r5.u32),
               GuestText(base, ctx.r6.u32), static_cast<uint32_t>(ctx.lr));
  __imp__sub_82319958(ctx, base);
}

extern "C" void sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base) {
  if (g_names_missing.load(std::memory_order_relaxed) > 0 &&
      (g_calls.fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) {
    ScanNewNames(base);
  }
  const uint32_t function = LoadU32(base, ctx.r4.u32 + kFrameNode);
  // Before it runs: it reads aMouseX and aMouseY.
  if (function && IsEnginePlayerInput(base, function)) {
    const uint32_t player_input = ctx.r3.u32;
    ApplyMouseLookFor(base, player_input);
    __imp__sub_822DF9E0(ctx, base);
    // After it: it has scaled the look input by the field of view.
    CompensateFieldOfView(base, player_input);
    return;
  }
  // Before it runs: it reads FOVAngle.
  if (Is(base, function, kGetDesiredFOV)) ApplyFieldOfView(base, ctx.r3.u32);
  // Before it runs: it reaches GearGRI.SHODTargets.
  if (Is(base, function, kAddEnemyLocators)) azura::dlc::BindShodTargets(base, ctx.r3.u32);
  if (REXCVAR_GET(script_trace) &&
      (Is(base, function, kPawnDied) || Is(base, function, kShouldDeathCauseFailure) ||
       Is(base, function, kGetDeathGameOverReason) || Is(base, function, kGameOverPlayer) ||
       Is(base, function, kClientGameOver) || Is(base, function, kClientSetCinematicMode) ||
       Is(base, function, kMatineeCancelled))) {
    REXLOG_INFO("[Azura death trace] {}", DescribeCall(base, ctx.r4.u32));
  }
  // After they run (their optional parameters have their values then): what RAAM's Shadow's
  // achievements need, which the Title Update's versions of these functions handle
  // (dlc/dlc_achievements.h).
  if (Is(base, function, kCompleteChapterByDifficulty) || Is(base, function, kUpdateAchievementProgression)) {
    const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
    __imp__sub_822DF9E0(ctx, base);
    if (Is(base, function, kCompleteChapterByDifficulty)) {
      const int32_t chapter = ParamOffset(base, function, "Chapter");
      const int32_t arcade = ParamOffset(base, function, "bWasArcade");
      const int32_t cheat = ParamOffset(base, function, "bIsCheatCommand");
      if (chapter >= 0 && arcade >= 0 && (cheat < 0 || LoadU32(base, locals + cheat) == 0)) {
        azura::dlc::OnChapterCompleted(static_cast<int32_t>(LoadU32(base, locals + chapter)),
                                       LoadU32(base, locals + arcade) != 0);
      }
    } else {
      const int32_t achievement = ParamOffset(base, function, "Achievement");
      const int32_t count = ParamOffset(base, function, "Count");
      if (achievement >= 0 && count >= 0) {
        azura::dlc::OnAchievementProgress(base[locals + achievement],
                                          static_cast<int32_t>(LoadU32(base, locals + count)));
      }
    }
    return;
  }
  if (Is(base, function, kBuildCampaignURL)) {
    const uint32_t result = ctx.r5.u32;
    __imp__sub_822DF9E0(ctx, base);
    if (result) {
      PatchTravelURL(base, result);
    }
    return;
  }
  if (Is(base, function, kServerTravel) || Is(base, function, kProcessServerTravel)) {
    const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
    const int32_t url_offset = ParamOffset(base, function, "URL");
    if (url_offset >= 0) {
      PatchTravelURL(base, locals + static_cast<uint32_t>(url_offset));
    }
  }
  if (Is(base, function, kHandleMenuSelection)) NoteCampaignChoice(base, ctx.r4.u32);
  // The main menu's Options option opens the Options as Start does there (the script, which does
  // not know the option, does not run).
  const int options_option = azura::menu::MainMenuOptionsOption();
  if (options_option >= 0 && Is(base, function, kHandleMenuSelection) &&
      IsQuitSelection(base, ctx.r4.u32, options_option)) {
    OpenOptionsFromMainMenu(ctx, base, function);
    return;
  }
  const int quit_option = azura::menu::MainMenuQuitOption();
  if (quit_option >= 0 && Is(base, function, kHandleMenuSelection) && IsQuitSelection(base, ctx.r4.u32, quit_option)) {
    RequestQuit();
    return;  // the script would not know the option and would leave the menu waiting
  }
  if (Is(base, function, kMenuOptionSelected)) LetListsShow(base, function);
  if (Is(base, function, kPopulateOptionsMenu)) g_options_open.store(true, std::memory_order_relaxed);
  // OnClose, not the OnOptionsMenuClose delegate, whose body never runs: whoever opened the Options
  // binds it to a function of its own (the main menu's OptionsMenuClosed).
  if (Is(base, function, kOnClose) && ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    g_options_open.store(false, std::memory_order_relaxed);
  }
  if ((Is(base, function, kWidgetInitialized) || Is(base, function, kFilterButtonInput)) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd") {
    g_front_end_scene.store(ctx.r3.u32, std::memory_order_relaxed);
    if (Is(base, function, kFilterButtonInput)) {
      const int32_t controller = SmallParam(base, function, ctx.r4.u32, "ControllerId");
      if (controller >= 0 && controller < 4) g_front_end_controller.store(controller, std::memory_order_relaxed);
    }
  }
  if (Is(base, function, kReturnToMain)) LetVideoPlayLeaveSound(base, function);
  // Closing the calibration screen runs Callback_ReturnToMain, which puts the script back on the
  // list of categories: right when the screen was opened from there, as in the game. Opened from
  // the Video list, Flash goes back to that list instead, so the script must stay on it too: that
  // one Callback_ReturnToMain is skipped, and the selection stays on the calibration row.
  if (Is(base, function, kCloseVideoOptionsScene) && g_calibration_open.exchange(false)) {
    g_returning_from_calibration.store(true);
  }
  if (Is(base, function, kReturnToMain) && g_returning_from_calibration.exchange(false)) return;
  // A list being built: rows that depend on other rows' values are shown or hidden first.
  if (Is(base, function, kPopulateOptionsList)) SetShownRows(base, ctx.r3.u32, CurrentMode(base, ctx.r3.u32));
  // A list being built, or the menu back on its categories: Flash starts every list on its first row.
  if (Is(base, function, kPopulateOptionsList) || Is(base, function, kReturnToMain)) {
    g_calibration_open.store(false);
    g_selected_row.store(0);
  }
  // Leaving a category: the script saves its values first, then options that need a restart can
  // offer one, only from the main menu.
  if (Is(base, function, kReturnToMain)) {
    const uint32_t scene = ctx.r3.u32;
    const uint32_t frame = ctx.r4.u32;
    const uint32_t result = ctx.r5.u32;
    const int category = CurrentMode(base, scene);
    __imp__sub_822DF9E0(ctx, base);
    // In the middle of a game a restart would lose progress: only the rows' descriptions say when
    // those options apply.
    ctx.r3.u64 = scene;
    ctx.r4.u64 = frame;
    ctx.r5.u64 = result;
    if (IsInFrontEnd(ctx, base, function) && azura::menu::PcOptionsCategoryClosed(category)) {
      ctx.r3.u64 = scene;
      ctx.r4.u64 = frame;
      ctx.r5.u64 = result;
      OpenRestartDialog(ctx, base, function);
    }
    return;
  }
  if (Is(base, function, kFilterButtonInput) && TakeKeyboardStartAsMenuKey(ctx, base, function)) return;
  if (Is(base, function, kFilterButtonInput) && HandleOptionsButton(ctx, base, function)) return;
  if (Is(base, function, kFilterButtonInput) && IgnoreOtherPlayersStart(ctx, base, function)) return;
  // The Local co-op category runs the storage location's callback: it opens the Local co-op menu.
  if (Is(base, function, kSelectStorageLocation) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    std::function<void()> handler;
    {
      std::lock_guard lock(g_quit_mutex);
      handler = g_local_coop_handler;
    }
    REXLOG_INFO("[Azura options] Opening the Local co-op menu");
    if (handler) handler();
    return;
  }
  // Controller runs Callback_ViewController, and Keyboard and mouse Callback_ResetTutorials from the
  // list of categories (CurrentMode 8, as LetListsShow keeps it; Game's list runs it for its own
  // row): each opens its controls menu instead.
  if ((Is(base, function, kViewController) ||
       (Is(base, function, kResetTutorials) && CurrentMode(base, ctx.r3.u32) == kNoCategory)) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    const auto menu = Is(base, function, kViewController) ? azura::script::ControlsMenu::kController
                                                          : azura::script::ControlsMenu::kKeyboardMouse;
    std::function<void(azura::script::ControlsMenu)> handler;
    {
      std::lock_guard lock(g_quit_mutex);
      handler = g_controls_handler;
    }
    REXLOG_INFO("[Azura options] Opening the {} controls menu",
                menu == azura::script::ControlsMenu::kController ? "controller" : "keyboard and mouse");
    if (handler) handler(menu);
    return;
  }
  // Local co-op is greyed out with no controller connected (the keyboard and mouse alone play one
  // player); Keyboard and mouse, at the storage location's place, never is.
  if (Is(base, function, kShouldOptionBeDisabled) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    const int32_t option = SmallParam(base, function, ctx.r4.u32, "Option");
    const uint32_t result = ctx.r5.u32;
    __imp__sub_822DF9E0(ctx, base);
    if (result && option == azura::menu::kLocalCoopCategory) {
      const auto* assignment = azura::input::InstalledPlayerAssignment();
      bool controller = false;
      if (assignment) {
        for (const auto& slot : assignment->Slots()) controller = controller || !slot.keyboard;
      }
      StoreU32(base, result, controller ? 0 : 1);
    } else if (result && option == azura::menu::kKeyboardMouseCategory) {
      StoreU32(base, result, 0);
    }
    return;
  }
  if (Is(base, function, kGetCallbackForOptionPsid) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    BorrowLiveCallback(base, ctx.r4.u32);
  }
  if (Is(base, function, kTVTypeWasChanged) && HandleLiveChange(ctx, base, function, 53)) return;
  if (Is(base, function, kGammaWasChanged) && HandleLiveChange(ctx, base, function, 49)) return;
  if (Is(base, function, kSpecialCaseValues) && FillOwnValueNames(ctx, base, function)) return;
  // The answer of the restart message box (see OpenRestartDialog): Accept restarts; nothing is
  // reset either way, so the Defaults code does not run.
  if (Is(base, function, kResetDefaults) && g_restart_dialog.exchange(false)) {
    const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
    const int32_t alias = ParamOffset(base, function, "SelectedInputAlias");
    if (alias >= 0 && LoadU32(base, locals + alias) == static_cast<uint32_t>(g_names[kGenericAcceptAlias].load())) {
      RequestRestart();
    }
    if (ctx.r5.u32) StoreU32(base, ctx.r5.u32, 1);  // ReturnValue: the answer was handled
    return;
  }
  if (Is(base, function, kResetDefaults) && IsResetAccepted(base, ctx.r4.u32)) {
    // Our rows first; then the script resets the profile settings and redraws the list.
    azura::menu::PcOptionsResetDefaults(CurrentMode(base, ctx.r3.u32));
  }
  // The menu is saving: GetOptionValueAt(Index) returns what the player chose for a row (the
  // slider value or the list index), read from Flash. For our rows it is applied and saved.
  if (Is(base, function, kGetOptionValueAt)) {
    const int option = azura::menu::FindPcOption(
        CurrentMode(base, ctx.r3.u32),
        static_cast<int32_t>(LoadU32(base, LoadU32(base, ctx.r4.u32 + kFrameLocals) + kGetOptionValueAtIndex)));
    if (option >= 0) {
      const uint32_t result = ctx.r5.u32;
      __imp__sub_822DF9E0(ctx, base);
      if (result) {
        const uint32_t bits = LoadU32(base, result);
        float value;
        std::memcpy(&value, &bits, 4);
        azura::menu::PcOptionStoreValue(option, value);
      }
      return;
    }
  }
  if (REXCVAR_GET(script_trace)) {
    const uint32_t frame = ctx.r4.u32;
    const uint32_t function = LoadU32(base, frame + kFrameNode);
    // Functions that Flash (and native code) call through ProcessEvent start here without going
    // through CallFunction, so their first call is logged here too.
    bool first = false;
    {
      std::lock_guard lock(g_seen_mutex);
      first = function != 0 && g_seen.insert(function).second;
    }
    if (first) {
      REXLOG_INFO("[Azura script] {}.{} (event)", ObjectName(base, LoadU32(base, function + kObjectOuter)),
                  ObjectName(base, function));
    }
    if (function && InList(REXCVAR_GET(script_trace_args), ObjectName(base, function))) {
      REXLOG_INFO("[Azura script] call {}", DescribeCall(base, frame));
      const uint32_t result = ctx.r5.u32;
      __imp__sub_822DF9E0(ctx, base);
      // Logged again on return, when the out parameters and the caller's result hold what it
      // produced.
      REXLOG_INFO("[Azura script] return {}", DescribeCall(base, frame, result));
      return;
    }
  }
  __imp__sub_822DF9E0(ctx, base);
}
