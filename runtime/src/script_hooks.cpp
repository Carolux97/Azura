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
// Online parties (Alboria): with network_mode = online, the Joinable and Invite only party types
// are offered without Xbox LIVE, and choosing one hosts a room on Alboria and creates a System
// Link party, whose traffic the room's relay carries (see "Online parties" below).
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
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
#include <rex/system/xam/alboria.h>
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
REXCVAR_DECLARE(std::string, network_mode);
REXCVAR_DECLARE(uint32_t, user_language);
REXCVAR_DECLARE(bool, menu_remove_unused);
REXCVAR_DECLARE(bool, alboria_live);
REXCVAR_DEFINE_STRING(startup_party, "joinable", "Network",
                      "With network_mode = \"online\", the party the game starts with: the type the "
                      "player last chose in the party options (joinable, invite_only, system_link or "
                      "local). Joinable and Invite only host a room on Alboria; when Alboria does not "
                      "answer the game starts in Local, and this is left as it was")
    .allowed({"joinable", "invite_only", "system_link", "local"});
REXCVAR_DECLARE(double, mnk_run_sensitivity);
REXCVAR_DECLARE(double, mnk_run_turn_limit);

extern "C" void __imp__sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822B0E30(PPCContext& __restrict ctx, uint8_t* base);

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
// Opens the Alboria menu (SetConnectionHandler).
std::function<void()> g_connection_handler;
// Opens the Join by code screen (SetJoinByCodeHandler).
std::function<void()> g_join_by_code_handler;
// A join in a room on Alboria (JoinSystemLinkPartyInRoom, "Join by code" below): the System Link
// browser is to open, and how many searches it may still run (0 when not joining). Searches after
// the first run while the host's party has not answered yet.
constexpr int kJoinSearches = 3;
std::atomic<bool> g_open_browser{false};
std::atomic<int> g_join_searches{0};
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
  kCanAllPlayersPlayOnline,  // GearPC: whether every local player may play on Xbox LIVE
  kCreateLiveParty,          // UIScene_FrontEnd: the Joinable or Invite only party is created
  kDialogItemPress,          // UIScene_FrontEnd: a party type is chosen in the party options
  kOpenPartyOptions,         // UIScene_FrontEnd: the party options open
  kBuildPartyOptions,        // UIScene_FrontEnd: the party options' list is built
  kRefreshManageInviteBar,   // UIScene_WhatsUp: Manage/Invite's button bar is built
  kCanInviteGearsPlayers,    // UIScene_WhatsUp: whether LB (invite Gears friends) is active
  kCanInviteLiveParty,       // UIScene_WhatsUp: whether RB (invite the Xbox LIVE party) is active
  kInviteGearsPlayers,       // UIScene_WhatsUp: LB is pressed
  kInviteLiveParty,          // UIScene_WhatsUp: RB is pressed
  kUpdatePanelTitle,         // UIWidget_SmallPartyPanel: the party panel names the party's type
  kFindOnlineGamesComplete,  // UIScene_SystemLink: the System Link search is over
  kCredits,                  // UIScene_Options.Callback_Credits: Connection, or the credits' button
  kClientMenuSelected,       // UIScene_FrontEnd: a choice in the menu of a player in another's party
  kConditionalCreateParty,   // UIScene_FrontEnd.ConditionalCreatePartySession: the party at start-up
  kGetPartyType,             // GearProfileSettings.GetPartyType: the profile's party type
  kLocalPartyCreated,        // UIScene_FrontEnd.LocalPartyCreated: the party at start-up, without Live
  kClientSetHUD,             // PlayerController.ClientSetHUD: the player's HUD, first set at start-up
  kOnCustomMapChanged,       // UIScene_PregameLobby: the map the host's selection control shows changed
  kApplySensitivityScaling,  // GearPC: scales the look input by the profile's look sensitivity
  kViewAdhesion,             // GearPlayerInput: turns the view towards the target (aim assist)
  kGetAdjustedAimFor,        // GearPC: bends a shot towards the target (aim assist)
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
    "UpdateAchievementProgression", "CanAllPlayersPlayOnline", "CreateLIVEParty",
    "Callback_DialogItemPress", "OpenPartyOptions", "BuildPartyOptions",
    "RefreshButtonBarAndPlayerOptions", "CanInviteGearsPlayers", "CanInviteLivePartyPlayers",
    "InviteGearsPlayers", "InviteLivePartyPlayers", "UpdatePanelTitle", "OnFindOnlineGamesComplete",
    "Callback_Credits",      "Callback_ClientMenuOptionWasSelected", "ConditionalCreatePartySession",
    "GetPartyType",          "LocalPartyCreated",      "ClientSetHUD",       "OnCustomMapChanged",
    "ApplySensitivityScaling", "ViewAdhesion",
    "GetAdjustedAimFor"};

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

// GearPlayerInput.PlayerInput (the one around the engine's), 0 until it first runs.
std::atomic<uint32_t> g_gear_player_input{0};

// Whether `function` is GearPlayerInput.PlayerInput.
bool IsGearPlayerInput(const uint8_t* base, uint32_t function) {
  const uint32_t known = g_gear_player_input.load(std::memory_order_relaxed);
  if (known) return function == known;
  if (!Is(base, function, kPlayerInput) ||
      ObjectName(base, LoadU32(base, function + kObjectOuter)) != "GearPlayerInput") {
    return false;
  }
  g_gear_player_input.store(function, std::memory_order_relaxed);
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

// UBoolProperty::BitMask: the bit of its 32-bit word a bool property is (read from the game: the
// first bool declared in a word is 0x80000000, the next 0x40000000, ...).
constexpr uint32_t kBoolPropertyBitMask = 104;

// A bool property of a class: its word's offset and its bit, or offset -1.
struct BoolField {
  int32_t offset = -1;
  uint32_t mask = 0;
};

// Bool property `name` of objects of class `cls`, found by name the first time. Called with
// g_mouse_look_mutex held.
std::unordered_map<uint64_t, BoolField> g_class_bools;
BoolField CachedBool(const uint8_t* base, uint32_t cls, uint32_t slot, const char* name) {
  const uint64_t key = (static_cast<uint64_t>(cls) << 8) | slot;
  auto it = g_class_bools.find(key);
  if (it != g_class_bools.end()) return it->second;
  BoolField found;
  for (uint32_t owner = cls; owner && found.offset < 0; owner = LoadU32(base, owner + kStructSuper)) {
    for (uint32_t field = LoadU32(base, owner + kStructChildren); field; field = LoadU32(base, field + kFieldNext)) {
      if (ObjectName(base, field) != name) continue;
      const uint32_t mask = LoadU32(base, field + kBoolPropertyBitMask);
      // A bool's mask has exactly one bit set; anything else means the layout is not the one read.
      if (ObjectName(base, LoadU32(base, field + kObjectClass)) == "BoolProperty" && mask && !(mask & (mask - 1))) {
        found = {static_cast<int32_t>(LoadU32(base, field + kPropertyOffset)), mask};
      }
      break;
    }
  }
  return g_class_bools.emplace(key, found).first->second;
}

// The guest user (LocalPlayer.ControllerId) whose PlayerController `controller` is, or -1. Called
// with g_mouse_look_mutex held.
int32_t UserOfController(const uint8_t* base, uint32_t controller) {
  if (!controller) return -1;
  const int32_t player = CachedOffset(base, LoadU32(base, controller + kObjectClass), 3, "Player");
  const uint32_t local_player = player >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(player)) : 0;
  if (!local_player) return -1;
  const int32_t controller_id = CachedOffset(base, LoadU32(base, local_player + kObjectClass), 4, "ControllerId");
  if (controller_id < 0) return -1;
  const uint32_t user = LoadU32(base, local_player + static_cast<uint32_t>(controller_id));
  return user < 4 ? static_cast<int32_t>(user) : -1;
}

// The mouse's share of one player's look input this frame (docs/reverse-engineering.md, Mouse
// look). The engine's PlayerInput.PlayerInput adds aMouseX and aMouseY to aTurn and aLookUp; what
// GearPlayerInput.PlayerInput does afterwards (the profile's look sensitivity, the pitch ratio)
// and GearPC.UpdateRotation (the slow motion division, the conversion to whole rotator units) is
// for the stick. The share is recorded as the engine's function returns (RecordMouseShare), taken
// out before ApplySensitivityScaling (TakeOutMouseShare) and put back as
// GearPlayerInput.PlayerInput returns (PutBackMouseShare), so the mouse turns the view by what
// mouse_look.cpp asked for. Keyed by PlayerInput object.
struct MouseShare {
  int32_t user = -1;
  bool on_mouse = false;   // the player has a keyboard and mouse this frame (ApplyMouseLook)
  float yaw = 0.0f;        // the share of aTurn, in rotator units
  float pitch = 0.0f;      // the share of aLookUp
  bool taken_out = false;  // TakeOutMouseShare ran this frame
  // The fraction of a rotator unit UpdateRotation's conversion of aTurn and aLookUp to the int
  // DeltaRot dropped last frame, added to the next so slow motion is not lost.
  float carry_yaw = 0.0f;
  float carry_pitch = 0.0f;
  // The profile multiplier last taken out, for a frame where it cannot be measured.
  float multiplier = 1.0f;
  // The player camera's view angle this frame (ApplyMouseLookFor), 0 when unknown.
  float view_fov = 0.0f;
  // Whether the player is roadie running, and the turn A or D at full steer gives then, in aTurn's
  // units before the profile multiplier (RecordMouseShare): the mouse never turns faster than that.
  bool running = false;
  float steer_cap = 0.0f;
};
std::unordered_map<uint32_t, MouseShare> g_mouse_shares;  // guarded by g_mouse_look_mutex

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
// Its class (UIScene_FrontEnd), whose functions the hooks run on it.
std::atomic<uint32_t> g_front_end_class{0};
// The controller (ControllerId) that last pressed a button on the main menu's scene: the one that
// chose a main menu option.
std::atomic<int32_t> g_front_end_controller{0};

void ApplyFilmGrain(uint8_t* base, uint32_t controller, uint32_t local_player);
void StoreU32(uint8_t* base, uint32_t address, uint32_t value);

void ApplyMouseLookFor(uint8_t* base, uint32_t player_input) {
  std::lock_guard lock(g_mouse_look_mutex);
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const int32_t mouse_x = CachedOffset(base, input_class, 0, "aMouseX");
  const int32_t mouse_y = CachedOffset(base, input_class, 1, "aMouseY");
  const int32_t sensitivity = CachedOffset(base, input_class, 2, "MouseSensitivity");
  const uint32_t controller = LoadU32(base, player_input + kObjectOuter);
  if (mouse_x < 0 || mouse_y < 0 || sensitivity < 0 || !controller) return;
  const int32_t user_index = UserOfController(base, controller);
  if (user_index < 0) return;
  const uint32_t user = static_cast<uint32_t>(user_index);
  const int32_t player = CachedOffset(base, LoadU32(base, controller + kObjectClass), 3, "Player");
  const uint32_t local_player = LoadU32(base, controller + static_cast<uint32_t>(player));
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
  MouseShare& share = g_mouse_shares[player_input];
  share.user = user_index;
  share.yaw = share.pitch = 0.0f;
  share.taken_out = false;
  share.running = false;
  share.view_fov = target.view_fov;
  share.on_mouse = azura::input::ApplyMouseLook(base, user, target);
  if (share.on_mouse) {
    // SmoothMouse averages the mouse axes with the frames before: the view would trail the mouse.
    const BoolField smoothing = CachedBool(base, input_class, 18, "bEnableMouseSmoothing");
    if (smoothing.offset >= 0) {
      const uint32_t address = player_input + static_cast<uint32_t>(smoothing.offset);
      const uint32_t word = LoadU32(base, address);
      if (word & smoothing.mask) StoreU32(base, address, word & ~smoothing.mask);
    }
  }
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

// --- Mouse share (MouseShare) ---

// GearPawn.SpecialMove while roadie running (GearPlayerInput.AutoPitchCentering tests
// IsDoingSpecialMove(4) for it).
constexpr uint8_t kSpecialMoveRoadieRun = 4;

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadU32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// WorldInfo.TimeDilation of `controller`'s world, 1 when it cannot be read. Called with
// g_mouse_look_mutex held.
float TimeDilationOf(const uint8_t* base, uint32_t controller) {
  const int32_t world_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 7, "WorldInfo");
  const uint32_t world = world_at >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(world_at)) : 0;
  if (!world) return 1.0f;
  const int32_t dilation_at = CachedOffset(base, LoadU32(base, world + kObjectClass), 24, "TimeDilation");
  const float value = dilation_at >= 0 ? LoadF32(base, world + static_cast<uint32_t>(dilation_at)) : 1.0f;
  return value > 0.0f ? value : 1.0f;
}

// While `controller`'s pawn is roadie running, GSM_RoadieRun.PreProcessInput replaces aTurn with
// aStrafe * SteeringPct (0 while bDisableSteering): the right stick does not turn, the left one
// steers. Records that the player is running and the turn a full steer gives this frame, as
// PlayerInput.PlayerInput scales it (SteeringPct even while bDisableSteering, at the start of the
// run, so the mouse is not locked then): times 100 * DeltaTime * LookRightScale, then FOVScale
// (GetFOVAngle() * 0.01111 with bEnableFOVScaling), then CompensateFieldOfView's division by
// `scale`. `delta_time` is PlayerInput's parameter, which it divides by TimeDilation first. Called
// with g_mouse_look_mutex held.
void RecordRoadieRun(const uint8_t* base, uint32_t player_input, uint32_t controller, float delta_time, float scale,
                     MouseShare& share) {
  const int32_t pawn_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 27, "MyGearPawn");
  const uint32_t pawn = pawn_at >= 0 ? LoadU32(base, controller + static_cast<uint32_t>(pawn_at)) : 0;
  if (!pawn) return;
  const uint32_t pawn_class = LoadU32(base, pawn + kObjectClass);
  const int32_t special_move_at = CachedOffset(base, pawn_class, 28, "SpecialMove");
  if (special_move_at < 0 || base[pawn + static_cast<uint32_t>(special_move_at)] != kSpecialMoveRoadieRun) return;
  // GearPawn.SpecialMoves, an array of the special move objects by ESpecialMove.
  const int32_t moves_at = CachedOffset(base, pawn_class, 29, "SpecialMoves");
  const uint32_t moves = moves_at >= 0 ? LoadU32(base, pawn + static_cast<uint32_t>(moves_at)) : 0;
  const uint32_t move_count = moves_at >= 0 ? LoadU32(base, pawn + static_cast<uint32_t>(moves_at) + 4) : 0;
  const uint32_t move = moves && kSpecialMoveRoadieRun < move_count ? LoadU32(base, moves + kSpecialMoveRoadieRun * 4) : 0;
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const int32_t look_right = CachedOffset(base, input_class, 32, "LookRightScale");
  const BoolField fov_scaling = CachedBool(base, input_class, 33, "bEnableFOVScaling");
  const int32_t steering = move ? CachedOffset(base, LoadU32(base, move + kObjectClass), 30, "SteeringPct") : -1;
  static std::atomic<bool> logged{false};
  if (!move || steering < 0 || look_right < 0) {
    if (!logged.exchange(true)) {
      REXLOG_WARN("[Azura mouse] Roadie run: cannot find its steering (SpecialMoves {}, SteeringPct {}, "
                  "LookRightScale {}); the mouse turn is not capped",
                  moves_at, steering, look_right);
    }
    return;
  }
  const float steering_pct = LoadF32(base, move + static_cast<uint32_t>(steering));
  const bool fov_scaled = fov_scaling.offset >= 0 &&
                          (LoadU32(base, player_input + static_cast<uint32_t>(fov_scaling.offset)) & fov_scaling.mask) != 0;
  const float fov_scale = fov_scaled && share.view_fov > 0.0f ? share.view_fov * 0.01111f : 1.0f;
  const float dt = delta_time / TimeDilationOf(base, controller);
  share.running = true;
  share.steer_cap = steering_pct * 100.0f * dt *
                    LoadF32(base, player_input + static_cast<uint32_t>(look_right)) * fov_scale / scale;
  if (dt > 0.0f && !logged.exchange(true)) {
    // Rotator units per second to degrees per second, at the profile multiplier last measured.
    const float degrees = share.steer_cap * share.multiplier / dt * 360.0f / 65536.0f;
    REXLOG_INFO("[Azura mouse] Roadie run: SteeringPct {:.3f}, a full A/D steer turns {:.0f} degrees/s, the "
                "mouse at most {:.0f}",
                steering_pct, degrees, degrees * REXCVAR_GET(mnk_run_turn_limit));
  }
}

// As the engine's PlayerInput.PlayerInput returns on `player_input` (after CompensateFieldOfView):
// records how much of aTurn and aLookUp the mouse is. That function adds aMouseX and aMouseY to
// them as AdjustMouseSensitivity left them, then flips them with bInvertTurn and bInvertMouse; with
// bStrafe the mouse's x strafes instead, bLockTurnUntilRelease zeroes aTurn, and
// IsLookInputIgnored (bIgnoreLookInput) zeroes both.
void RecordMouseShare(uint8_t* base, uint32_t player_input, float delta_time) {
  std::lock_guard lock(g_mouse_look_mutex);
  const auto it = g_mouse_shares.find(player_input);
  if (it == g_mouse_shares.end() || !it->second.on_mouse) return;
  MouseShare& share = it->second;
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const int32_t mouse_x = CachedOffset(base, input_class, 0, "aMouseX");
  const int32_t mouse_y = CachedOffset(base, input_class, 1, "aMouseY");
  const int32_t turn = CachedOffset(base, input_class, 9, "aTurn");
  const int32_t look_up = CachedOffset(base, input_class, 10, "aLookUp");
  const int32_t strafe = CachedOffset(base, input_class, 19, "bStrafe");
  const BoolField invert_mouse = CachedBool(base, input_class, 20, "bInvertMouse");
  const BoolField invert_turn = CachedBool(base, input_class, 21, "bInvertTurn");
  const uint32_t controller = LoadU32(base, player_input + kObjectOuter);
  if (!controller || mouse_x < 0 || mouse_y < 0 || turn < 0 || look_up < 0) return;
  const int32_t ignore_look = CachedOffset(base, LoadU32(base, controller + kObjectClass), 22, "bIgnoreLookInput");
  if (ignore_look >= 0 && base[controller + static_cast<uint32_t>(ignore_look)] != 0) return;
  const auto is_set = [&](const BoolField& field) {
    return field.offset >= 0 && (LoadU32(base, player_input + static_cast<uint32_t>(field.offset)) & field.mask) != 0;
  };
  // CompensateFieldOfView divided the whole of aTurn and aLookUp by this.
  const float scale = static_cast<float>(REXCVAR_GET(field_of_view)) / kGameFieldOfView;
  float yaw = LoadF32(base, player_input + static_cast<uint32_t>(mouse_x)) / scale;
  float pitch = LoadF32(base, player_input + static_cast<uint32_t>(mouse_y)) / scale;
  if (is_set(invert_turn)) yaw = -yaw;
  if (is_set(invert_mouse)) pitch = -pitch;
  if ((strafe >= 0 && base[player_input + static_cast<uint32_t>(strafe)] != 0) ||
      LoadF32(base, player_input + static_cast<uint32_t>(turn)) == 0.0f) {
    yaw = 0.0f;
  }
  if (LoadF32(base, player_input + static_cast<uint32_t>(look_up)) == 0.0f) pitch = 0.0f;
  share.yaw = yaw;
  share.pitch = pitch;
  RecordRoadieRun(base, player_input, controller, delta_time, scale, share);
}

// A player's aTurn and aLookUp as GearPC.ApplySensitivityScaling is about to scale them.
struct LookBeforeScaling {
  uint32_t player_input;
  uint32_t turn;     // address of aTurn
  uint32_t look_up;  // address of aLookUp
  float turn_value;
  float look_up_value;
};

// Before GearPC.ApplySensitivityScaling runs on `controller`: its player's look input, when the
// mouse has a share in it this frame.
std::optional<LookBeforeScaling> BeforeSensitivityScaling(const uint8_t* base, uint32_t controller) {
  std::lock_guard lock(g_mouse_look_mutex);
  const int32_t input_at = CachedOffset(base, LoadU32(base, controller + kObjectClass), 23, "PlayerInput");
  if (input_at < 0) return std::nullopt;
  const uint32_t player_input = LoadU32(base, controller + static_cast<uint32_t>(input_at));
  const auto it = g_mouse_shares.find(player_input);
  if (!player_input || it == g_mouse_shares.end()) return std::nullopt;
  const MouseShare& share = it->second;
  if (!share.on_mouse || share.taken_out || (share.yaw == 0.0f && share.pitch == 0.0f)) return std::nullopt;
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const int32_t turn = CachedOffset(base, input_class, 9, "aTurn");
  const int32_t look_up = CachedOffset(base, input_class, 10, "aLookUp");
  if (turn < 0 || look_up < 0) return std::nullopt;
  LookBeforeScaling before{player_input, player_input + static_cast<uint32_t>(turn),
                           player_input + static_cast<uint32_t>(look_up), 0.0f, 0.0f};
  before.turn_value = LoadF32(base, before.turn);
  before.look_up_value = LoadF32(base, before.look_up);
  return before;
}

// After GearPC.ApplySensitivityScaling: takes the mouse's share, as scaled by the profile's look
// sensitivity (GetSensitivityMultiplier, measured as the ratio it scaled the look input by), out of
// aTurn and aLookUp, so the pitch ratio GearPlayerInput.PlayerInput applies next is the stick's
// alone.
void TakeOutMouseShare(uint8_t* base, const LookBeforeScaling& before) {
  std::lock_guard lock(g_mouse_look_mutex);
  MouseShare& share = g_mouse_shares[before.player_input];
  const float turn = LoadF32(base, before.turn);
  const float look_up = LoadF32(base, before.look_up);
  // The axis with the most input gives the ratio most precisely; with neither, the last one.
  constexpr float kMeasurable = 1e-3f;
  float multiplier = share.multiplier;
  if (std::fabs(before.turn_value) >= std::fabs(before.look_up_value) && std::fabs(before.turn_value) > kMeasurable) {
    multiplier = turn / before.turn_value;
  } else if (std::fabs(before.look_up_value) > kMeasurable) {
    multiplier = look_up / before.look_up_value;
  }
  static int logged = 0;
  if (std::fabs(multiplier - share.multiplier) > 1e-3f && logged < 16) {
    ++logged;
    REXLOG_INFO("[Azura mouse] Profile look sensitivity {:.3f}, kept off the mouse (player {})", multiplier,
                share.user + 1);
  }
  share.multiplier = multiplier;
  StoreU32(base, before.turn, FloatBits(turn - share.yaw * multiplier));
  StoreU32(base, before.look_up, FloatBits(look_up - share.pitch * multiplier));
  share.taken_out = true;
}

// As GearPlayerInput.PlayerInput returns on `player_input`: puts the mouse's share back into aTurn
// and aLookUp, multiplied by the slow motion factor GearPC.UpdateRotation divides the view's turn
// by, with last frame's leftover fraction of a rotator unit.
void PutBackMouseShare(uint8_t* base, uint32_t player_input) {
  std::lock_guard lock(g_mouse_look_mutex);
  const auto it = g_mouse_shares.find(player_input);
  if (it == g_mouse_shares.end() || !it->second.on_mouse) return;
  MouseShare& share = it->second;
  if (!share.taken_out) {
    share.carry_yaw = share.carry_pitch = 0.0f;
    return;
  }
  const uint32_t input_class = LoadU32(base, player_input + kObjectClass);
  const uint32_t turn = player_input + static_cast<uint32_t>(CachedOffset(base, input_class, 9, "aTurn"));
  const uint32_t look_up = player_input + static_cast<uint32_t>(CachedOffset(base, input_class, 10, "aLookUp"));
  const uint32_t controller = LoadU32(base, player_input + kObjectOuter);
  const float time_dilation = TimeDilationOf(base, controller);
  // While roadie running the mouse turns slower, and never faster sideways than mnk_run_turn_limit
  // times a full A or D steer (with the stick's own steer, if any): what goes beyond is dropped, so
  // a flick cannot turn round.
  const float run = share.running ? static_cast<float>(REXCVAR_GET(mnk_run_sensitivity)) : 1.0f;
  float turn_value = LoadF32(base, turn) + share.yaw * run * time_dilation;
  if (share.running) {
    const float limit = std::max(share.steer_cap * share.multiplier * static_cast<float>(REXCVAR_GET(mnk_run_turn_limit)),
                                 std::fabs(LoadF32(base, turn)));
    if (std::fabs(turn_value) > limit) {
      turn_value = std::copysign(limit, turn_value);
      share.carry_yaw = 0.0f;
    }
  }
  // UpdateRotation turns aTurn and aLookUp into the int DeltaRot, dropping the fraction: kept for
  // the next frame.
  const float yaw = turn_value + share.carry_yaw;
  const float pitch = LoadF32(base, look_up) + share.pitch * run * time_dilation + share.carry_pitch;
  share.carry_yaw = yaw - std::trunc(yaw);
  share.carry_pitch = pitch - std::trunc(pitch);
  StoreU32(base, turn, FloatBits(yaw));
  StoreU32(base, look_up, FloatBits(pitch));
}

// Whether the player of `player_input` aims with the mouse this frame: the aim assist, made for the
// stick, is off for them.
bool AimsWithMouse(const MouseShare& share) {
  return share.on_mouse && (share.yaw != 0.0f || share.pitch != 0.0f || azura::input::PlaysWithMouse(share.user));
}

// The out parameters of GearPlayerInput.ViewAdhesion as it starts.
struct AdhesionBefore {
  uint32_t yaw_at;
  uint32_t pitch_at;
  uint32_t yaw;
  uint32_t pitch;
};

// Before GearPlayerInput.ViewAdhesion runs in `frame` on `player_input`: its out_YawRot and
// out_PitchRot, the view's turn this frame, when its player aims with the mouse.
std::optional<AdhesionBefore> BeforeViewAdhesion(const uint8_t* base, uint32_t player_input, uint32_t frame) {
  {
    std::lock_guard lock(g_mouse_look_mutex);
    const auto it = g_mouse_shares.find(player_input);
    if (it == g_mouse_shares.end() || !AimsWithMouse(it->second)) return std::nullopt;
  }
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const int32_t yaw = ParamOffset(base, function, "out_YawRot");
  const int32_t pitch = ParamOffset(base, function, "out_PitchRot");
  if (yaw < 0 || pitch < 0) return std::nullopt;
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  AdhesionBefore before{locals + static_cast<uint32_t>(yaw), locals + static_cast<uint32_t>(pitch), 0, 0};
  before.yaw = LoadU32(base, before.yaw_at);
  before.pitch = LoadU32(base, before.pitch_at);
  return before;
}

// After GearPlayerInput.ViewAdhesion: gives its out parameters back the turn they came in with, so
// the view is not pulled towards the target. They are copied back to the caller as it returns.
void UndoViewAdhesion(uint8_t* base, const AdhesionBefore& before) {
  static std::atomic<bool> logged{false};
  if ((LoadU32(base, before.yaw_at) != before.yaw || LoadU32(base, before.pitch_at) != before.pitch) &&
      !logged.exchange(true)) {
    REXLOG_INFO("[Azura mouse] View adhesion kept off the mouse (it turned yaw {} -> {}, pitch {} -> {})",
                static_cast<int32_t>(before.yaw), static_cast<int32_t>(LoadU32(base, before.yaw_at)),
                static_cast<int32_t>(before.pitch), static_cast<int32_t>(LoadU32(base, before.pitch_at)));
  }
  StoreU32(base, before.yaw_at, before.yaw);
  StoreU32(base, before.pitch_at, before.pitch);
}

// A weapon's bullet aim assist (AimAssistScale, AimAssistScaleWhileTargeting), zeroed while
// GearPC.GetAdjustedAimFor runs.
struct AimAssistBefore {
  uint32_t scale_at = 0;
  uint32_t targeting_scale_at = 0;
  uint32_t scale = 0;
  uint32_t targeting_scale = 0;
};

// Before GearPC.GetAdjustedAimFor runs in `frame` on `controller`: when its player aims with the
// mouse, zeroes the aim assist of the weapon W it aims, which bends the shot towards the target,
// and returns what it was.
std::optional<AimAssistBefore> SuspendBulletAimAssist(uint8_t* base, uint32_t controller, uint32_t frame) {
  std::lock_guard lock(g_mouse_look_mutex);
  const int32_t user = UserOfController(base, controller);
  if (user < 0 || !azura::input::PlaysWithMouse(static_cast<uint32_t>(user))) return std::nullopt;
  const int32_t weapon_at = ParamOffset(base, LoadU32(base, frame + kFrameNode), "W");
  const uint32_t weapon =
      weapon_at >= 0 ? LoadU32(base, LoadU32(base, frame + kFrameLocals) + static_cast<uint32_t>(weapon_at)) : 0;
  if (!weapon) return std::nullopt;
  const uint32_t weapon_class = LoadU32(base, weapon + kObjectClass);
  const int32_t scale = CachedOffset(base, weapon_class, 25, "AimAssistScale");
  const int32_t targeting_scale = CachedOffset(base, weapon_class, 26, "AimAssistScaleWhileTargeting");
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[Azura mouse] Bullet aim assist kept off the mouse ({}: AimAssistScale at {}, "
                "AimAssistScaleWhileTargeting at {})",
                ObjectName(base, weapon_class), scale, targeting_scale);
  }
  if (scale < 0 || targeting_scale < 0) return std::nullopt;
  AimAssistBefore before{weapon + static_cast<uint32_t>(scale), weapon + static_cast<uint32_t>(targeting_scale)};
  before.scale = LoadU32(base, before.scale_at);
  before.targeting_scale = LoadU32(base, before.targeting_scale_at);
  StoreU32(base, before.scale_at, 0);
  StoreU32(base, before.targeting_scale_at, 0);
  return before;
}

// After GearPC.GetAdjustedAimFor: gives the weapon its aim assist back.
void RestoreBulletAimAssist(uint8_t* base, const AimAssistBefore& before) {
  StoreU32(base, before.scale_at, before.scale);
  StoreU32(base, before.targeting_scale_at, before.targeting_scale);
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
// --- Choosing the map in the pre-game lobby with the keyboard ---
//
// The host chooses the map of a Horde, Versus or custom match with a control made in Flash
// (UIScene_PregameLobby.bIsInCustomMapSelection): the mouse wheel and the arrows change the map,
// which the control reports with OnCustomMapChanged(MapId), and accepting it makes the control call
// OnCustomMapSelected(MapId), which sends the choice to the game and says the host has finished
// (GRI.SubmitMapSelectionByName, GRI.SubmitHostFinishedMapSelection). The control never gets the
// keyboard's Return: in this scene it reaches the script as Start (the pre-game lobby counts as
// play for the keyboard), so there, with the control open, Return calls OnCustomMapSelected itself
// with the map the control last showed.
std::atomic<int32_t> g_custom_map_id{-1};
constexpr uint32_t kCustomMapLocalsSize = 16;  // MapId
uint32_t g_custom_map_locals = 0;

// Whether bool property `name` of `object` is set; false when it is not found.
bool BoolPropertySet(const uint8_t* base, uint32_t object, const char* name) {
  for (uint32_t owner = LoadU32(base, object + kObjectClass); owner; owner = LoadU32(base, owner + kStructSuper)) {
    for (uint32_t field = LoadU32(base, owner + kStructChildren); field; field = LoadU32(base, field + kFieldNext)) {
      if (ObjectName(base, field) != name) continue;
      const uint32_t mask = LoadU32(base, field + kBoolPropertyBitMask);
      if (ObjectName(base, LoadU32(base, field + kObjectClass)) != "BoolProperty" || !mask || (mask & (mask - 1))) {
        return false;
      }
      return (LoadU32(base, object + LoadU32(base, field + kPropertyOffset)) & mask) != 0;
    }
  }
  return false;
}

// Whether Return stays Start in the pre-game lobby scene `scene`: for the host, once the map is chosen
// (not in the map selection control), where Start starts the match (OnHostStartGame, the "Iniciar"
// prompt shows the key that presses Start). Anyone else's, and the map selection's, is A.
// The host is whoever's lobby is the authority (GRI.Role 3, ROLE_Authority).
bool ReturnStartsLobbyMatch(const uint8_t* base, uint32_t scene) {
  if (BoolPropertySet(base, scene, "bIsInCustomMapSelection")) return false;
  const int32_t gri_at = ClassPropertyOffset(base, LoadU32(base, scene + kObjectClass), "GRI");
  const uint32_t gri = gri_at >= 0 ? LoadU32(base, scene + static_cast<uint32_t>(gri_at)) : 0;
  if (!gri) return false;
  const int32_t role_at = ClassPropertyOffset(base, LoadU32(base, gri + kObjectClass), "Role");
  constexpr uint8_t kRoleAuthority = 3;
  return role_at >= 0 && base[gri + static_cast<uint32_t>(role_at)] == kRoleAuthority;
}

// Return reached UIScene_PregameLobby.FilterButtonInput (`ctx`, `function`) as Start: with the map
// selection control open, accepts the map it shows. Returns whether it did.
bool ConfirmCustomMapWithReturn(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const uint32_t cls = LoadU32(base, function + kObjectOuter);
  if (ObjectName(base, cls) != "UIScene_PregameLobby") return false;
  const uint32_t scene = ctx.r3.u32;
  const int32_t map = g_custom_map_id.load();
  if (map < 0 || !BoolPropertySet(base, scene, "bIsInCustomMapSelection")) return false;
  const uint32_t selected = FindClassFunction(base, cls, "OnCustomMapSelected");
  if (!selected) return false;
  if (!g_custom_map_locals) {
    g_custom_map_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kCustomMapLocalsSize);
    if (!g_custom_map_locals) return false;
  }
  std::memset(base + g_custom_map_locals, 0, kCustomMapLocalsSize);
  StoreU32(base, g_custom_map_locals, static_cast<uint32_t>(map));
  g_custom_map_id.store(-1);
  REXLOG_INFO("[Azura menus] Return accepts map {} in the pre-game lobby", map);
  RunInstead(ctx, base, selected, g_custom_map_locals);
  return true;
}

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
    const bool lobby_start = key == StartKey::kReturn && scene == "UIScene_PregameLobby" &&
                             ReturnStartsLobbyMatch(base, ctx.r3.u32);
    const bool keep_start = key == StartKey::kNone || (title_screen && key == StartKey::kReturn) || lobby_start;
    g_start_taken[controller].store(!keep_start);
    if (keep_start) return false;
    if (title_screen) {
      // Escape on the title screen: used up, nothing else.
      if (const uint32_t result = ctx.r5.u32) StoreU32(base, result, 1);
      return true;
    }
    if (key == StartKey::kReturn && ConfirmCustomMapWithReturn(ctx, base, function)) {
      if (const uint32_t result = ctx.r5.u32) StoreU32(base, result, 1);  // the press was handled
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

// Y on the list of categories of the Options is the credits' button (pc_options.cpp): it runs
// Callback_Credits, as choosing the credits did before Connection took their place. RunInstead
// runs it straight in the interpreter, so it does not reach the hook that turns Connection's call
// of the same function into the Alboria menu. Returns whether it did.
bool HandleCreditsButton(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const uint32_t scene = ctx.r3.u32;
  if (ObjectName(base, LoadU32(base, function + kObjectOuter)) != "UIScene_Options") return false;
  if (CurrentMode(base, scene) != kNoCategory) return false;
  const uint32_t locals = LoadU32(base, ctx.r4.u32 + kFrameLocals);
  if (base[locals + kFilterInputEvent] != kInputPressed) return false;
  if (NameText(base, LoadU32(base, locals + kFilterButtonName)) != "XboxTypeS_Y") return false;
  const uint32_t credits = FindClassFunction(base, LoadU32(base, function + kObjectOuter), "Callback_Credits");
  if (!credits) {
    REXLOG_WARN("[Azura options] Callback_Credits not found; the credits button does nothing");
    return false;
  }
  const uint32_t result = ctx.r5.u32;
  REXLOG_INFO("[Azura options] Opening the credits");
  RunInstead(ctx, base, credits);
  if (result) StoreU32(base, result, 1);  // FilterButtonInput's ReturnValue: the press was handled
  return true;
}

// The menu of a player in another's party without its calendar and Marketplace (menu_patches.cpp):
// Callback_ClientMenuOptionWasSelected(SelectedOption, a float) handles its choices by their
// original position (0 Journal, 1 Calendar, 2 Options, 3 Marketplace, 4 Leave party), so the
// positions left (Journal, Options, Leave party) are turned into those before it runs.
void ClientMenuPosition(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(menu_remove_unused)) return;
  constexpr float kOriginal[] = {0.0f, 2.0f, 4.0f};
  const uint32_t at = LoadU32(base, ctx.r4.u32 + kFrameLocals);  // SelectedOption, the first parameter
  uint32_t bits = LoadU32(base, at);
  float shown;
  std::memcpy(&shown, &bits, sizeof(shown));
  const int index = static_cast<int>(shown);
  if (index < 0 || index >= static_cast<int>(std::size(kOriginal))) return;
  std::memcpy(&bits, &kOriginal[index], sizeof(bits));
  StoreU32(base, at, bits);
}

// --- Content updates and online play ----------------------------------------------------------
//
// A content update (RAAM's Shadow's, mounted as update data) replaces GearGame.xxx with a version
// whose net objects are the base game's, at the same indices, plus new ones at the end (verified:
// the 86564 objects both load sit at the same index in both; the update's 74 extra ones follow
// index 89730). The update keeps the package's GUID and its single generation, now of 89804 net
// objects, so two players with and without it number every package after GearGame differently, and
// what one sends the other resolves to the wrong object or to None.
//
// Unreal Engine 3 already negotiates this through generations: UPackageMap::Compute (guest
// 0x822C88B8) gives each package the net object count of the older of the two sides' generations.
// So the updated GearGame is given the base game's count as a first generation and its own as a
// second: with a player without the update both sides count the base game's objects, and the
// update's own ones are not sent; between two players with it, everything is.
struct ContentUpdate {
  const char* package;
  uint32_t guid[4];
  uint32_t base_count;    // the base game's net objects
  uint32_t update_count;  // the updated package's
};
constexpr ContentUpdate kContentUpdates[] = {
    {"GearGame", {0x4365494C, 0x471BBA26, 0x06C75190, 0x7AD22C91}, 89730, 89804},
};
constexpr uint32_t kPackageGuid = 92;         // UPackage: its GUID (16 bytes)
constexpr uint32_t kPackageGenerations = 128;  // UPackage: TArray<INT> net object count per generation

// Gives `package`, when it is an updated one of kContentUpdates with a single generation, the base
// game's generation first. The array is grown with the game's own TArray resize (guest 0x822B0E30:
// r3 the array, r4 its new capacity, r5 the element size), on the caller's context.
void SplitContentUpdateGenerations(PPCContext& ctx, uint8_t* base, uint32_t package) {
  for (const ContentUpdate& update : kContentUpdates) {
    if (ObjectName(base, package) != update.package) continue;
    bool same_guid = true;
    for (int i = 0; i < 4; ++i) same_guid &= LoadU32(base, package + kPackageGuid + i * 4) == update.guid[i];
    const uint32_t array = package + kPackageGenerations;
    if (!same_guid || LoadU32(base, array + 4) != 1 || LoadU32(base, LoadU32(base, array)) != update.update_count) return;
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;
    ctx.r3.u64 = array;
    ctx.r4.u64 = 2;
    ctx.r5.u64 = 4;
    __imp__sub_822B0E30(ctx, base);
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    const uint32_t data = LoadU32(base, array);
    if (!data) return;
    StoreU32(base, data, update.base_count);
    StoreU32(base, data + 4, update.update_count);
    StoreU32(base, array + 4, 2);
    StoreU32(base, array + 8, 2);
    REXLOG_INFO("[Azura online] {} is a content update: generations {} (base game) and {}", update.package,
                update.base_count, update.update_count);
    return;
  }
}

// Splits the content updates' generations once, at the first ClientSetHUD: the game has loaded its
// script packages by then, and no connection has a package map yet. The packages are found through
// the player controller's class, which GearGame holds.
void SplitContentUpdatesOnce(PPCContext& ctx, uint8_t* base) {
  static std::atomic<bool> split{false};
  if (split.exchange(true)) return;
  uint32_t package = LoadU32(base, ctx.r3.u32 + kObjectClass);
  for (uint32_t outer = LoadU32(base, package + kObjectOuter); outer; outer = LoadU32(base, outer + kObjectOuter)) {
    package = outer;
  }
  SplitContentUpdateGenerations(ctx, base, package);
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

void SetConnectionHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_connection_handler = std::move(handler);
}

void JoinSystemLinkPartyInRoom() {
  // With Live parties on Alboria the SDK hands the host's session to the game as an accepted
  // invite, and the game joins the party itself.
  if (REXCVAR_GET(alboria_live)) return;
  g_join_searches.store(kJoinSearches + 1);
  g_open_browser.store(true);
}

void SetJoinByCodeHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_join_by_code_handler = std::move(handler);
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

// --- Online parties (Alboria) ---
//
// The party options offer Joinable and Invite only when GearPC.CanAllPlayersPlayOnline() says
// every local player may play on Xbox LIVE (docs/reverse-engineering.md, "Party types, System
// Link and Manage/Invite"). With network_mode = online it says yes. Choosing one of them runs
// UIScene_FrontEnd.Callback_DialogItemPress, which checks the Xbox LIVE sign-in (natively, out of
// reach of these hooks) and shows the Gold message: while Alboria answers (CheckService, asked as
// the options open) the choice skips those checks and goes, as the script would after them, to
// TransitionToLIVEParty with the type pending. When Alboria does not answer, or refused the
// sign-in, the script runs and its message says online play is not possible (menu_patches.cpp),
// and the room overlay names a refusal. TransitionToLIVEParty destroys the party and runs
// CreateLIVEParty, which hosts a room on Alboria and runs CreateSystemLinkParty instead: the party
// is a System Link one, and its local network is the room's relay (rex/system/xam/alboria.h). The
// host's address is the room's first one from the start, so the session the party creates right
// away already has it. Choosing System Link or Local leaves the room.
//
// The party is a System Link one, so the party options would tick System Link and the party panel
// name it: while a room is hosted, both compare the party type against the one chosen instead
// (their byte constants are changed for that one call). In Manage/Invite (UIScene_WhatsUp), the
// Xbox LIVE friends' Join (Y) and Invite (X) leave the button bar, kept for Alboria's own friends
// later; LB ("Show ID" / "Hide ID") shows or hides the room's code next to the Current players
// heading ("(LOBBY ID: ****-****)"), hidden whenever the scene opens, so a stream never shows it
// by chance; RB joins a friend's room by its code, only from a party already on Alboria (the
// Local and System Link parties stay off it).

// Players a room takes: the most a Versus private match has.
constexpr int kOnlineRoomMembers = 10;
// The party type of the online party (kFriendsOnlyType or kInviteRequiredType; -1 none yet): the
// one the player chose when hosting, or the room's when joined by code. The party options and the
// party panel show it while the room lasts.
std::atomic<int> g_online_party_type{-1};
// Whether the room's code shows next to Manage/Invite's Current players heading (LB toggles it).
std::atomic<bool> g_show_room_code{false};
// UIScene_FrontEnd.PendingPartyType, Callback_DialogItemPress's list positions, and the party types
// (EGearVersusPartyType) they stand for.
constexpr uint32_t kFrontEndPendingPartyType = 1009;
constexpr int32_t kJoinableItem = 0;
constexpr int32_t kInviteOnlyItem = 1;
// The two others (positions as BuildPartyOptions fills the list; not checked on screen yet).
constexpr int32_t kSystemLinkItem = 2;
constexpr int32_t kLocalItem = 3;
constexpr int kInviteRequiredType = 0;
constexpr int kFriendsOnlyType = 1;  // "Joinable" in the party options
constexpr int kSystemLinkType = 3;
constexpr int kLocalPartyType = 4;
// Guest memory for the locals of UIScene_FrontEnd.TransitionToLIVEParty, larger than they are.
constexpr uint32_t kTransitionLocalsSize = 64;
uint32_t g_transition_locals = 0;
// Guest memory for the locals of UIScene_FrontEnd.CreateSystemLinkParty, larger than they are.
constexpr uint32_t kSystemLinkPartyLocalsSize = 64;
uint32_t g_system_link_party_locals = 0;

// Online play through the runtime's System Link parties on Alboria. With alboria_live (work in
// progress) the game's own Xbox Live parties are Alboria's instead, and these hooks stay out of it.
bool OnlineMode() {
  return REXCVAR_GET(network_mode) == "online" && !REXCVAR_GET(alboria_live);
}

// Answers CanAllPlayersPlayOnline with yes when online. The frame is about to run it; returns
// whether it was answered.
// The party the game creates when it leaves the title screen (UIScene_FrontEnd.
// ConditionalCreatePartySession, docs/reverse-engineering.md "The party at start-up"). Without an
// Xbox Live sign-in the game always makes it Local (LocalPartyCreated); with online play on, the
// runtime makes it the one startup_party names instead while the function runs: a room's party
// (kBootOnline) or a System Link one (kBootSystemLink), both created as choosing them does.
enum BootParty { kBootNone, kBootOnline, kBootSystemLink, kBootLocal };
std::atomic<int> g_boot_party{kBootNone};
// The party type the profile answers outside a room (GetPartyType), when the party the game
// started with is not the profile's: System Link or Local from startup_party, or Local forced
// because Alboria did not answer or cannot be played on. -1 once the player chooses a type in the
// party options.
std::atomic<int> g_startup_party_type{-1};
// The sign-in refusal the game stays in Local for, from the moment the start-up party fell back to
// Local or the player asked for a room, until the room overlay takes it (TakeRefusalNotice).
// Guarded by g_refusal_notice_mutex.
std::mutex g_refusal_notice_mutex;
std::string g_refusal_notice;

// Has the room overlay show `refusal` from now on.
void NoticeRefusal(const std::string& refusal) {
  std::lock_guard lock(g_refusal_notice_mutex);
  g_refusal_notice = refusal;
}

namespace azura::script {
std::string TakeRefusalNotice() {
  std::lock_guard lock(g_refusal_notice_mutex);
  return std::exchange(g_refusal_notice, {});
}
}  // namespace azura::script

// A room failed (alboria::TakeFailedRoom), so the party the game creates next is Local instead of
// another room that would most likely fail the same way, until the player chooses Joinable or
// Invite only again.
std::atomic<bool> g_room_failed{false};

bool AnswerCanPlayOnline(PPCContext& ctx, uint8_t* base) {
  if (!OnlineMode()) return false;
  if (ctx.r5.u32) StoreU32(base, ctx.r5.u32, 1);  // ReturnValue
  return true;
}

// A party type chosen in the party options, about to be handled by Callback_DialogItemPress on the
// front end scene in `ctx`. Returns whether it was handled here instead.
bool ChooseOnlinePartyType(PPCContext& ctx, uint8_t* base, uint32_t dialog_item_press) {
  if (!OnlineMode()) return false;
  namespace alboria = rex::system::xam::alboria;
  const int32_t item = SmallParam(base, dialog_item_press, ctx.r4.u32, "Idx");
  // The player's choice is the party the game starts with next time (startup_party). A Local the
  // runtime forced at start-up ends here too.
  const char* chosen = item == kJoinableItem     ? "joinable"
                       : item == kInviteOnlyItem ? "invite_only"
                       : item == kSystemLinkItem ? "system_link"
                       : item == kLocalItem      ? "local"
                                                 : nullptr;
  if (chosen) {
    g_startup_party_type.store(-1);
    if (REXCVAR_GET(startup_party) != chosen) azura::menu::StoreControlsSetting("startup_party", chosen, true);
  }
  if (item != kJoinableItem && item != kInviteOnlyItem) {
    // System Link or Local: the party leaves Alboria.
    if (alboria::InRoom()) {
      REXLOG_INFO("[Azura online] Leaving the Alboria room for another party type");
      alboria::Leave();
    }
    return false;
  }
  if (!alboria::ServiceReachable()) {
    // The script's message, which with online play on says online play on Alboria is not possible.
    alboria::CheckService();
    return false;
  }
  if (const std::string refusal = alboria::SignInRefusal(); !refusal.empty()) {
    // Alboria refused the sign-in (checked again as the party options opened): a room would fail
    // the same way. The party stays as it is; the script's message says why in general, and the
    // room overlay the refusal itself.
    REXLOG_INFO("[Azura online] Alboria refused the sign-in ({}): no room", refusal);
    NoticeRefusal(refusal);
    return false;
  }
  // The player asks for a room again: one that fails sends the party back to Local once more.
  g_room_failed.store(false);
  const uint32_t cls = LoadU32(base, dialog_item_press + kObjectOuter);
  const uint32_t transition = FindClassFunction(base, cls, "TransitionToLIVEParty");
  const uint32_t close_options = FindClassFunction(base, cls, "ClosePartyOptions");
  if (!transition || !close_options) {
    REXLOG_WARN("[Azura online] TransitionToLIVEParty or ClosePartyOptions not found");
    return false;
  }
  if (!g_transition_locals) {
    g_transition_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kTransitionLocalsSize);
    if (!g_transition_locals) return false;
  }
  std::memset(base + g_transition_locals, 0, kTransitionLocalsSize);
  const auto status = alboria::GetStatus();
  const int type = item == kJoinableItem ? kFriendsOnlyType : kInviteRequiredType;
  if (status.host && alboria::InRoom()) {
    // Already hosting a room: the party stays, with the type chosen now, which Alboria passes on to
    // the room's members and to who joins later.
    g_online_party_type.store(type);
    alboria::SetPartyType(type == kInviteRequiredType ? alboria::PartyType::kInviteOnly : alboria::PartyType::kJoinable);
    RunInstead(ctx, base, close_options, g_transition_locals);
    return true;
  }
  base[ctx.r3.u32 + kFrontEndPendingPartyType] = static_cast<uint8_t>(type);
  g_online_party_type.store(type);
  g_show_room_code.store(false);
  RunInstead(ctx, base, transition, g_transition_locals);
  return true;
}

// Runs `function` (in `ctx`, about to run) seeing the chosen online party type where the party is
// System Link, while the player is in a room (hosting it, or joined by code): the party options'
// BuildPartyOptions, which ticks the party's type, and the party panel's UpdatePanelTitle, which
// names it. Both compare
// PartyGRI.CurrPartyType with a byte constant for each type (docs/reverse-engineering.md, "Party
// options"): for this call the System Link one can never match and the chosen type's one is
// System Link's. Returns whether it ran.
// The online party type a room's party shows (kFriendsOnlyType or kInviteRequiredType), or -1 out
// of a room or before one is chosen: the host's chosen type, and for a member the room's type as
// Alboria last told it, which follows the host's changes.
int ShownOnlinePartyType() {
  namespace alboria = rex::system::xam::alboria;
  const int chosen = g_online_party_type.load();
  if (chosen < 0 || !alboria::InRoom()) return -1;
  if (const auto status = alboria::GetStatus(); !status.host && status.state == alboria::State::kConnected) {
    return status.party_type == alboria::PartyType::kInviteOnly ? kInviteRequiredType : kFriendsOnlyType;
  }
  return chosen;
}

// The party panel (UIWidget_SmallPartyPanel) whose UpdatePanelTitle ran last, and the online party
// type its title names (-1 when it named no room's type).
std::atomic<uint32_t> g_party_panel{0};
std::atomic<int> g_party_panel_title_type{-1};

bool RunWithOnlinePartyType(PPCContext& ctx, uint8_t* base, uint32_t function) {
  const int chosen = ShownOnlinePartyType();
  if (chosen < 0) return false;
  const uint32_t code = LoadU32(base, function + kStructScript);
  const uint32_t size = LoadU32(base, function + kStructScript + 4);
  // Each `CurrPartyType == <byte>`, compared as integers: the property's 8-byte reference, the
  // byte-to-int cast (0x38 0x3A), then the byte constant token.
  constexpr uint8_t kTokenPrimitiveCast = 0x38;
  constexpr uint8_t kCastByteToInt = 0x3A;
  std::vector<std::pair<uint32_t, uint8_t>> changed;
  for (uint32_t i = 0; i + 12 <= size; ++i) {
    if (LoadU32(base, code + i) != 0 || base[code + i + 8] != kTokenPrimitiveCast ||
        base[code + i + 9] != kCastByteToInt || base[code + i + 10] != kTokenByteConst) {
      continue;
    }
    if (ObjectName(base, LoadU32(base, code + i + 4)) != "CurrPartyType") continue;
    const uint32_t at = code + i + 11;
    if (base[at] == kSystemLinkType) {
      changed.emplace_back(at, base[at]);
      base[at] = 0xFF;
    } else if (base[at] == chosen) {
      changed.emplace_back(at, base[at]);
      base[at] = kSystemLinkType;
    }
  }
  if (changed.size() != 2) {
    for (const auto& [at, value] : changed) base[at] = value;
    REXLOG_WARN("[Azura online] {} is not as expected ({} party type constants changed); it shows System Link",
                ObjectName(base, function), changed.size());
    return false;
  }
  __imp__sub_822DF9E0(ctx, base);
  for (const auto& [at, value] : changed) base[at] = value;
  if (Is(base, function, kUpdatePanelTitle)) g_party_panel_title_type.store(chosen);
  return true;
}

// --- Manage/Invite ---

// UIScene_WhatsUp.ButtonList (array<byte> at +896) and the button kinds that leave it online: the
// Xbox LIVE friends' Invite (X) and Join (Y).
constexpr uint32_t kManageInviteButtonList = 896;
constexpr uint8_t kInviteButton = 3;
constexpr uint8_t kJoinButton = 4;
// Guest memory for the locals of UIScene_WhatsUp.RefreshButtonBarAndPlayerOptions, larger than
// they are. Cleared before each run, as the script expects: it appends the bar to its ButtonString
// local.
constexpr uint32_t kButtonBarLocalsSize = 128;
uint32_t g_button_bar_locals = 0;
// The scene's heading as the game has it (UIScene_WhatsUp.CurrentGameTitle), before ours; how to
// hand a new one to the party list (from WidgetInitialized); and the properties our bytecode sets.
std::mutex g_heading_mutex;
std::u16string g_players_heading;
std::vector<uint8_t> g_heading_refresh;
uint32_t g_heading_property = 0;
uint32_t g_lb_text_property = 0;
std::map<std::u16string, uint32_t> g_heading_code;  // guest bytecode for each heading and button

// Texts in the player's language (Xbox 360 language IDs), upper case as the game's.
struct ManageInviteTexts {
  const char16_t* lobby_id;
  const char16_t* show_id;
  const char16_t* hide_id;
};

ManageInviteTexts ManageInviteTextsFor(uint32_t language) {
  switch (language) {
    case 2: return {u"ロビー ID", u"IDを表示", u"IDを非表示"};
    case 3: return {u"LOBBY-ID", u"ID ANZEIGEN", u"ID AUSBLENDEN"};
    case 4: return {u"ID DU SALON", u"AFFICHER L'ID", u"MASQUER L'ID"};
    case 5: return {u"ID DE SALA", u"MOSTRAR ID", u"OCULTAR ID"};
    case 6: return {u"ID STANZA", u"MOSTRA ID", u"NASCONDI ID"};
    case 7: return {u"로비 ID", u"ID 표시", u"ID 숨기기"};
    case 8: return {u"大廳 ID", u"顯示 ID", u"隱藏 ID"};
    case 9: return {u"ID DA SALA", u"MOSTRAR ID", u"OCULTAR ID"};
    case 10: return {u"大厅 ID", u"显示 ID", u"隐藏 ID"};
    case 11: return {u"ID POKOJU", u"POKAŻ ID", u"UKRYJ ID"};
    case 12: return {u"ID ЛОББИ", u"ПОКАЗАТЬ ID", u"СКРЫТЬ ID"};
    default: return {u"LOBBY ID", u"SHOW ID", u"HIDE ID"};
  }
}

std::u16string Utf16(const std::string& ascii) {
  return std::u16string(ascii.begin(), ascii.end());
}

// The game's heading (read once from `scene`'s CurrentGameTitle, a UTF-16 FString), with the
// room's code, or stars, after it while the player is in a room.
std::u16string PlayersHeading(const uint8_t* base, uint32_t scene, uint32_t heading_property) {
  std::lock_guard lock(g_heading_mutex);
  if (g_players_heading.empty()) {
    const uint32_t at = scene + LoadU32(base, heading_property + kPropertyOffset);
    const uint32_t data = LoadU32(base, at);
    const uint32_t count = LoadU32(base, at + 4);
    for (uint32_t i = 0; data && i + 1 < count && i < 128; ++i) {
      g_players_heading += static_cast<char16_t>(base[data + i * 2] << 8 | base[data + i * 2 + 1]);
    }
  }
  const auto status = rex::system::xam::alboria::GetStatus();
  if (status.state != rex::system::xam::alboria::State::kConnected || status.code.empty()) return g_players_heading;
  const std::string code = g_show_room_code.load() ? status.code : std::string("****-****");
  return g_players_heading + u" <font color='#E3A63B'>(" +
         ManageInviteTextsFor(REXCVAR_GET(user_language)).lobby_id + u": " + Utf16(code) + u")</font>";
}

// LB's text (UIScene_WhatsUp.InviteGearsButtonActive): the button's controller font character,
// which the button prompts replace, and "Show ID" or "Hide ID".
std::u16string ShowIdButton() {
  const ManageInviteTexts texts = ManageInviteTextsFor(REXCVAR_GET(user_language));
  return std::u16string(u"<Font:$ControllerFont>_</Font> ") + (g_show_room_code.load() ? texts.hide_id : texts.show_id);
}

// Guest bytecode that sets the scene's CurrentGameTitle to `heading` and LB's text to `button`
// and, with `refresh`, hands the heading to the party list (`refresh` is the script's own
// `PartyList.SetString("HeaderHtml", CurrentGameTitle)`, copied from WidgetInitialized). Built
// once for each.
uint32_t HeadingCode(uint8_t* base, const std::u16string& heading, const std::u16string& button,
                     const std::vector<uint8_t>& refresh) {
  std::lock_guard lock(g_heading_mutex);
  if (!g_heading_property || !g_lb_text_property) return 0;
  const std::u16string key = heading + u'\0' + button + (refresh.empty() ? u"" : u"\1");
  if (const auto found = g_heading_code.find(key); found != g_heading_code.end()) return found->second;
  std::vector<uint8_t> code;
  auto u32 = [&code](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) code.push_back(static_cast<uint8_t>(v >> shift));
  };
  auto let = [&](uint32_t property, const std::u16string& text) {
    constexpr uint8_t kTokenInstanceVariable = 0x01;
    code.push_back(kTokenLet);
    code.push_back(kTokenInstanceVariable);
    u32(0);
    u32(property);
    // UTF-16, big-endian like the rest of the console's bytecode, zero-terminated.
    code.push_back(kTokenUnicodeStringConst);
    for (char16_t c : text) {
      code.push_back(static_cast<uint8_t>(c >> 8));
      code.push_back(static_cast<uint8_t>(c));
    }
    code.push_back(0);
    code.push_back(0);
  };
  let(g_heading_property, heading);
  let(g_lb_text_property, button);
  code.insert(code.end(), refresh.begin(), refresh.end());
  code.push_back(kTokenReturn);
  code.push_back(kTokenNothing);
  code.push_back(kTokenEndOfScript);
  const uint32_t address = rex::system::kernel_state()->memory()->SystemHeapAlloc(static_cast<uint32_t>(code.size()));
  if (!address) return 0;
  std::memcpy(base + address, code.data(), code.size());
  g_heading_code.emplace(key, address);
  return address;
}

// The bytes of `PartyList.SetString("HeaderHtml", CurrentGameTitle)` in WidgetInitialized's
// bytecode: from the context token (0x19) whose object is the PartyList instance variable, through
// the call, the "HeaderHtml" string and the CurrentGameTitle instance variable, to the end of the
// parameters. Empty when the bytecode is not as expected.
std::vector<uint8_t> HeadingRefreshCall(const uint8_t* base, uint32_t widget_initialized) {
  constexpr uint8_t kTokenContext = 0x19;
  constexpr uint8_t kTokenInstanceVariable = 0x01;
  const uint32_t code = LoadU32(base, widget_initialized + kStructScript);
  const uint32_t size = LoadU32(base, widget_initialized + kStructScript + 4);
  const int64_t text = FindStringConst(base, code, size, "HeaderHtml");
  if (text < 0) return {};
  // After the string: its terminator, CurrentGameTitle (token and 8-byte reference), a marker for
  // each optional parameter left out, and the end of the parameters.
  const uint32_t after = static_cast<uint32_t>(text) + sizeof("HeaderHtml");
  if (after + 9 >= size || base[code + after] != kTokenInstanceVariable) return {};
  uint32_t end = after + 9;
  while (end < size && end < after + 13 && base[code + end] != kTokenEndFunctionParms) ++end;
  if (end >= size || base[code + end] != kTokenEndFunctionParms) return {};
  ++end;
  for (int64_t start = text - 2; start >= 0 && start >= text - 64; --start) {
    if (base[code + start] == kTokenContext && base[code + start + 1] == kTokenInstanceVariable &&
        LoadU32(base, code + start + 2) == 0 && ObjectName(base, LoadU32(base, code + start + 6)) == "PartyList") {
      return std::vector<uint8_t>(base + code + start, base + code + end);
    }
  }
  return {};
}

// Runs `code` on the frame in `ctx` in place of its function's own bytecode, then puts it back.
void RunCode(PPCContext& ctx, uint8_t* base, uint32_t code) {
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_code = LoadU32(base, frame + kFrameCode);
  StoreU32(base, frame + kFrameCode, code);
  __imp__sub_822DF9E0(ctx, base);
  StoreU32(base, frame + kFrameCode, own_code);
}

// Before UIScene_WhatsUp.WidgetInitialized sets up a widget: the code is hidden again (each time
// the scene opens), the heading gets the stars and LB "Show ID". Also remembers what refreshing
// them later needs.
void PrepareManageInviteHeading(PPCContext& ctx, uint8_t* base, uint32_t widget_initialized) {
  const uint32_t cls = LoadU32(base, widget_initialized + kObjectOuter);
  {
    std::lock_guard lock(g_heading_mutex);
    if (!g_heading_property) g_heading_property = FindClassFunction(base, cls, "CurrentGameTitle");
    if (!g_lb_text_property) g_lb_text_property = FindClassFunction(base, cls, "InviteGearsButtonActive");
    if (g_heading_refresh.empty()) g_heading_refresh = HeadingRefreshCall(base, widget_initialized);
  }
  g_show_room_code.store(false);
  if (const uint32_t code = HeadingCode(base, PlayersHeading(base, ctx.r3.u32, g_heading_property), ShowIdButton(), {})) {
    RunCode(ctx, base, code);
  }
}

// LB in Manage/Invite (InviteGearsPlayers, in `ctx` about to run): shows or hides the room's code
// in the heading, and LB's text and the button bar follow. The scene runs it twice for one press,
// so a second run within a moment is the same press.
void ToggleRoomCode(PPCContext& ctx, uint8_t* base, uint32_t invite_gears_players) {
  using Clock = std::chrono::steady_clock;
  static std::atomic<int64_t> last_toggle{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  if (now - last_toggle.load() < 300) return;
  last_toggle.store(now);
  g_show_room_code.store(!g_show_room_code.load());
  std::vector<uint8_t> refresh;
  {
    std::lock_guard lock(g_heading_mutex);
    refresh = g_heading_refresh;
  }
  const uint32_t refresh_bar =
      FindClassFunction(base, LoadU32(base, invite_gears_players + kObjectOuter), "RefreshButtonBarAndPlayerOptions");
  const uint32_t code =
      refresh.empty() ? 0 : HeadingCode(base, PlayersHeading(base, ctx.r3.u32, g_heading_property), ShowIdButton(), refresh);
  if (!code || !refresh_bar) {
    REXLOG_WARN("[Azura online] Manage/Invite's heading cannot be refreshed");
    return;
  }
  RunCode(ctx, base, code);
  if (!g_button_bar_locals) {
    g_button_bar_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kButtonBarLocalsSize);
    if (!g_button_bar_locals) return;
  }
  std::memset(base + g_button_bar_locals, 0, kButtonBarLocalsSize);
  RunInstead(ctx, base, refresh_bar, g_button_bar_locals);
}

// --- Join by code ---
//
// RB in Manage/Invite closes the scene (its ActionScript_CloseView) and opens the Join by code
// screen. Once the player is in the room, the front end's AttemptSystemLinkTransition opens the
// System Link browser, as choosing System Link in the party options does; it runs on the next
// player input frame, borrowing that frame for the front end scene. When the browser's search ends
// (OnFindOnlineGamesComplete) with a party, the first one (the room's host, the only one there) is
// joined through Callback_OnJoinSelected(0), as choosing it in the list does.

// UIScene_SystemLink.SearchResults (array at +1056).
constexpr uint32_t kSystemLinkSearchResults = 1056;
// Guest memory for the locals of the functions run here (ActionScript_CloseView,
// AttemptSystemLinkTransition, Callback_OnJoinSelected, RefreshSearch), larger than any of them.
constexpr uint32_t kJoinLocalsSize = 64;
uint32_t g_join_locals = 0;

uint32_t JoinLocals(uint8_t* base) {
  if (!g_join_locals) g_join_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kJoinLocalsSize);
  if (g_join_locals) std::memset(base + g_join_locals, 0, kJoinLocalsSize);
  return g_join_locals;
}

// RB in Manage/Invite (InviteLivePartyPlayers, in `ctx` about to run).
// Manage/Invite while the Join by code screen is open over it, and its function that closes it: it
// stays open under the screen, so going back returns to it, and closes once the code joins a room
// (CloseManageInviteForJoin). 0 when it closed by itself in the meantime (its OnClose ran).
std::atomic<uint32_t> g_manage_invite_scene{0};
std::atomic<uint32_t> g_manage_invite_close_view{0};

void OpenJoinByCode(PPCContext& ctx, uint8_t* base, uint32_t invite_live_party) {
  g_manage_invite_close_view.store(
      FindClassFunction(base, LoadU32(base, invite_live_party + kObjectOuter), "ActionScript_CloseView"));
  g_manage_invite_scene.store(ctx.r3.u32);
  std::function<void()> handler;
  {
    std::lock_guard lock(g_quit_mutex);
    handler = g_join_by_code_handler;
  }
  REXLOG_INFO("[Azura online] Opening Join by code");
  if (handler) handler();
}

// On a player input frame (`ctx`): closes Manage/Invite, left open under the Join by code screen,
// before the System Link browser opens for the room the code joined. Only while it is still the
// scene it was (its class is checked too, in case its object was reused).
void CloseManageInviteForJoin(PPCContext& ctx, uint8_t* base) {
  const uint32_t scene = g_manage_invite_scene.exchange(0);
  const uint32_t close_view = g_manage_invite_close_view.load();
  if (!scene || !close_view || !JoinLocals(base)) return;
  if (ObjectName(base, LoadU32(base, scene + kObjectClass)) != "UIScene_WhatsUp") return;
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, scene);
  ctx.r3.u64 = scene;
  RunInstead(ctx, base, close_view, g_join_locals);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
}

// On a player input frame (`ctx`, about to run PlayerInput): opens the System Link browser on the
// front end scene when a join asked for it.
void OpenSystemLinkBrowserForJoin(PPCContext& ctx, uint8_t* base) {
  const uint32_t front_end = g_front_end_scene.load(std::memory_order_relaxed);
  if (!front_end || !g_open_browser.exchange(false)) return;
  CloseManageInviteForJoin(ctx, base);
  // The party joined by code is the host's room on Alboria: it shows the room's type (Joinable or
  // Invite only), never System Link, which is the party underneath.
  g_online_party_type.store(rex::system::xam::alboria::GetStatus().party_type ==
                                    rex::system::xam::alboria::PartyType::kInviteOnly
                                ? kInviteRequiredType
                                : kFriendsOnlyType);
  g_show_room_code.store(false);
  const uint32_t front_end_class = g_front_end_class.load(std::memory_order_relaxed);
  const uint32_t attempt =
      front_end_class ? FindClassFunction(base, front_end_class, "AttemptSystemLinkTransition") : 0;
  if (!attempt || !JoinLocals(base)) {
    REXLOG_WARN("[Azura online] AttemptSystemLinkTransition not found; the System Link browser does not open");
    g_join_searches.store(0);
    return;
  }
  REXLOG_INFO("[Azura online] Opening the System Link browser to join the room's party");
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, front_end);
  ctx.r3.u64 = front_end;
  RunInstead(ctx, base, attempt, g_join_locals);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
}

// --- A room that ended under its member, or one that failed ---
//
// A player who joined another's party (an Alboria room) is in that host's party. When the room ends
// without the player leaving (the host closed it or left, the player was removed, or the relay went
// silent), the party is gone, and the game would only notice once its own connection times out. What
// the game itself does when it loses the host (GearPC.ClientWasKicked, among others) is
// GearPC.ReturnToMainMenu(bReturnToStartScreen = false, bClearMenuHistory = true): it leaves the
// party, goes back to the main menu and, with no room, creates the player's own party there as at
// start-up.
//
// A room that failed (alboria::TakeFailedRoom: hosting or joining did not get it connected, or the
// hosted room ended) leaves the player in a System Link party with no room behind it, where nobody
// can join and the menus still offer what only a room has. The same return to the main menu
// replaces it, with g_room_failed set so that the party created there is Local (CreateStartupParty);
// the room overlay tells why the room failed.
//
// Either way the player then gets the game's message box of an invite that failed, which with
// online play on says the room on Alboria failed and how to try again (menu_patches.cpp): GearPC's
// NotifyInviteFailed, on the first input frame in the main menu (g_room_message_pending), so that a
// return from a match, which closes every message box, does not take it away.
//
// ReturnToMainMenu travels to the front end's map again, as leaving a match does, and the front end
// then starts over at its title screen. A player already in the main menu stays there instead, and
// gets a Local party the way choosing Local in the party options makes one: the front end's
// DestroyPartyForLocal destroys the party, then its CreateLocalParty runs LocalPartyCreated. The
// profile's party type answers Local (g_startup_party_type) until the player chooses another, or
// the party would show the System Link type the room's party had.
constexpr uint32_t kReturnToMainMenuLocalsSize = 64;  // bReturnToStartScreen, bClearMenuHistory, locals
uint32_t g_return_to_menu_locals = 0;
// The message box of a room that failed or ended is still to be shown in the main menu.
std::atomic<bool> g_room_message_pending{false};

// The function `name` of the player controller's class or one it extends; 0 when none has it.
uint32_t FindControllerFunction(uint8_t* base, uint32_t controller, const char* name) {
  uint32_t function = 0;
  for (uint32_t cls = controller ? LoadU32(base, controller + kObjectClass) : 0; cls && !function;
       cls = LoadU32(base, cls + kStructSuper)) {
    function = FindClassFunction(base, cls, name);
  }
  return function;
}

// Whether player 1 is in the front end's main menu, past its title screen.
bool InMainMenu(uint8_t* base) {
  const uint32_t scene = g_front_end_scene.load(std::memory_order_relaxed);
  return g_front_end.load(std::memory_order_relaxed) && scene &&
         !(LoadU32(base, scene + kFrontEndFlags) & kOnTitleScreenBit);
}

// On a player input frame (`ctx`, about to run PlayerInput) in the main menu: shows the message box
// of a room that failed or ended, when one is pending, with GearPC.NotifyInviteFailed.
void ShowRoomMessage(PPCContext& ctx, uint8_t* base) {
  if (!g_room_message_pending.load() || !InMainMenu(base) || !g_return_to_menu_locals) return;
  g_room_message_pending.store(false);
  const uint32_t controller = LoadU32(base, ctx.r3.u32 + kObjectOuter);
  const uint32_t notify = FindControllerFunction(base, controller, "NotifyInviteFailed");
  if (!notify) {
    REXLOG_WARN("[Azura online] NotifyInviteFailed not found; the room's message box is not shown");
    return;
  }
  REXLOG_INFO("[Azura online] Showing the room's message box");
  std::memset(base + g_return_to_menu_locals, 0, kReturnToMainMenuLocalsSize);
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, controller);
  ctx.r3.u64 = controller;
  RunInstead(ctx, base, notify, g_return_to_menu_locals);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
}

// Guest memory for the locals of UIWidget_SmallPartyPanel.UpdatePanelTitle, larger than they are.
constexpr uint32_t kPanelTitleLocalsSize = 256;
uint32_t g_panel_title_locals = 0;

// On a player input frame (`ctx`, about to run PlayerInput) in the front end's main menu: names the
// room's type again in the party panel when it changed since its title last named it. A room's
// party stays System Link in the game (CurrPartyType 3), so switching between Joinable and Invite
// only, or the host doing so for a member, changes nothing the panel follows: the runtime runs its
// UpdatePanelTitle, seeing the type the room has now (RunWithOnlinePartyType).
void RefreshPartyPanelTitle(PPCContext& ctx, uint8_t* base) {
  const int shown = ShownOnlinePartyType();
  if (shown < 0 || shown == g_party_panel_title_type.load() || !InMainMenu(base)) return;
  const uint32_t panel = g_party_panel.load();
  if (!panel) return;
  const uint32_t panel_class = LoadU32(base, panel + kObjectClass);
  if (ObjectName(base, panel_class) != "UIWidget_SmallPartyPanel") {
    g_party_panel.store(0);
    return;
  }
  const uint32_t update_panel_title = FindClassFunction(base, panel_class, "UpdatePanelTitle");
  if (!update_panel_title) return;
  if (!g_panel_title_locals) {
    g_panel_title_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kPanelTitleLocalsSize);
    if (!g_panel_title_locals) return;
  }
  std::memset(base + g_panel_title_locals, 0, kPanelTitleLocalsSize);
  const uint32_t frame = ctx.r4.u32;
  const uint32_t node = LoadU32(base, frame + kFrameNode);
  const uint32_t code = LoadU32(base, frame + kFrameCode);
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, panel);
  StoreU32(base, frame + kFrameNode, update_panel_title);
  StoreU32(base, frame + kFrameCode, LoadU32(base, update_panel_title + kStructScript));
  StoreU32(base, frame + kFrameLocals, g_panel_title_locals);
  ctx.r3.u64 = panel;
  if (!RunWithOnlinePartyType(ctx, base, update_panel_title)) g_party_panel_title_type.store(shown);
  StoreU32(base, frame + kFrameObject, own_object);
  StoreU32(base, frame + kFrameNode, node);
  StoreU32(base, frame + kFrameCode, code);
  StoreU32(base, frame + kFrameLocals, locals);
  ctx.r3.u64 = own_r3;
}

// On a player input frame (`ctx`, about to run PlayerInput) in the front end's main menu: runs
// UIScene_FrontEnd.DestroyPartyForLocal and then CreateLocalParty on its scene, with
// g_return_to_menu_locals (zeroed) as their locals. Returns whether it did; not in the front end, or
// on its title screen, it does not.
bool DestroyPartyInFrontEnd(PPCContext& ctx, uint8_t* base, bool failed) {
  const uint32_t scene = g_front_end_scene.load(std::memory_order_relaxed);
  const uint32_t front_end_class = g_front_end_class.load(std::memory_order_relaxed);
  if (!InMainMenu(base) || !front_end_class) return false;
  const uint32_t destroy = FindClassFunction(base, front_end_class, "DestroyPartyForLocal");
  const uint32_t create = FindClassFunction(base, front_end_class, "CreateLocalParty");
  if (!destroy || !create) {
    REXLOG_WARN("[Azura online] DestroyPartyForLocal or CreateLocalParty not found; back to the main menu instead");
    return false;
  }
  g_startup_party_type.store(kLocalPartyType);
  REXLOG_INFO("[Azura online] {}: the party in the main menu becomes Local",
              failed ? "The room failed" : "The room ended");
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, scene);
  ctx.r3.u64 = scene;
  RunInstead(ctx, base, destroy, g_return_to_menu_locals);
  std::memset(base + g_return_to_menu_locals, 0, kReturnToMainMenuLocalsSize);
  RunInstead(ctx, base, create, g_return_to_menu_locals);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
  return true;
}

// On a player input frame (`ctx`, about to run PlayerInput): sends the player back to the main menu
// when the room it was a member of ended, or when a room failed.
void ReturnToMainMenuWhenRoomLost(PPCContext& ctx, uint8_t* base) {
  namespace alboria = rex::system::xam::alboria;
  const bool failed = alboria::TakeFailedRoom();
  if (failed) {
    g_room_failed.store(true);
  } else if (!alboria::TakeLostRoom()) {
    return;
  }
  g_room_message_pending.store(true);
  const uint32_t player_input = ctx.r3.u32;
  const uint32_t controller = LoadU32(base, player_input + kObjectOuter);
  const uint32_t return_to_menu = FindControllerFunction(base, controller, "ReturnToMainMenu");
  if (!return_to_menu) {
    REXLOG_WARN("[Azura online] The room ended, but ReturnToMainMenu was not found on the player controller");
    return;
  }
  if (!g_return_to_menu_locals) {
    g_return_to_menu_locals = rex::system::kernel_state()->memory()->SystemHeapAlloc(kReturnToMainMenuLocalsSize);
    if (!g_return_to_menu_locals) return;
  }
  std::memset(base + g_return_to_menu_locals, 0, kReturnToMainMenuLocalsSize);
  if (DestroyPartyInFrontEnd(ctx, base, failed)) return;
  StoreU32(base, g_return_to_menu_locals + 4, 1);  // bClearMenuHistory; bReturnToStartScreen stays 0
  REXLOG_INFO("[Azura online] {}: back to the main menu", failed ? "The room failed" : "The room ended");
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, controller);
  ctx.r3.u64 = controller;
  RunInstead(ctx, base, return_to_menu, g_return_to_menu_locals);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
}

// UIScene_SystemLink.OnFindOnlineGamesComplete has just run (`ctx`): joins the first party found,
// or searches again.
void JoinFoundParty(PPCContext& ctx, uint8_t* base, uint32_t find_complete) {
  if (g_join_searches.load() <= 0) return;
  const uint32_t scene = ctx.r3.u32;
  const uint32_t cls = LoadU32(base, find_complete + kObjectOuter);
  const uint32_t results = LoadU32(base, scene + kSystemLinkSearchResults + 4);
  if (results > 0) {
    g_join_searches.store(0);
    const uint32_t join_selected = FindClassFunction(base, cls, "Callback_OnJoinSelected");
    if (!join_selected || !JoinLocals(base)) return;
    REXLOG_INFO("[Azura online] Joining the room's party ({} found)", results);
    // SelectedIndex, the first parameter, stays 0.
    RunInstead(ctx, base, join_selected, g_join_locals);
    return;
  }
  if (g_join_searches.fetch_sub(1) > 1) {
    const uint32_t refresh = FindClassFunction(base, cls, "RefreshSearch");
    if (refresh && JoinLocals(base)) {
      REXLOG_INFO("[Azura online] No party in the room yet; searching again");
      RunInstead(ctx, base, refresh, g_join_locals);
    }
    return;
  }
  REXLOG_WARN("[Azura online] No party found in the room; leaving it");
  rex::system::xam::alboria::Leave();
}

// Removes the Xbox LIVE friends' Invite and Join from Manage/Invite's button list (scene in
// `scene`), before the bar is built from it.
void TrimManageInviteButtons(uint8_t* base, uint32_t scene) {
  const uint32_t data = LoadU32(base, scene + kManageInviteButtonList);
  const uint32_t count = LoadU32(base, scene + kManageInviteButtonList + 4);
  if (!data || count > 64) return;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t button = base[data + i];
    if (button != kInviteButton && button != kJoinButton) base[data + kept++] = button;
  }
  if (kept != count) StoreU32(base, scene + kManageInviteButtonList + 4, kept);
}

// Hosts a room and creates a System Link party where CreateLIVEParty was about to run. Returns
// whether it did.
// Runs UIScene_FrontEnd.CreateSystemLinkParty where `front_end_function` (a function of the same
// class, in `ctx`) was about to run, after hosting a room on Alboria when `host_room`, of the type
// in g_online_party_type. Returns whether it did.
bool RunSystemLinkParty(PPCContext& ctx, uint8_t* base, uint32_t front_end_function, bool host_room) {
  const uint32_t create_system_link_party =
      FindClassFunction(base, LoadU32(base, front_end_function + kObjectOuter), "CreateSystemLinkParty");
  if (!create_system_link_party) {
    REXLOG_WARN("[Azura online] CreateSystemLinkParty not found; the party is left to the game");
    return false;
  }
  if (!g_system_link_party_locals) {
    g_system_link_party_locals =
        rex::system::kernel_state()->memory()->SystemHeapAlloc(kSystemLinkPartyLocalsSize);
    if (!g_system_link_party_locals) return false;
  }
  std::memset(base + g_system_link_party_locals, 0, kSystemLinkPartyLocalsSize);
  if (host_room) {
    const bool invite_only = g_online_party_type.load() == kInviteRequiredType;
    REXLOG_INFO("[Azura online] Hosting an {} party on Alboria", invite_only ? "Invite only" : "Joinable");
    rex::system::xam::alboria::HostGame(rex::system::kernel_state()->title_id(), kOnlineRoomMembers,
                                        invite_only ? rex::system::xam::alboria::PartyType::kInviteOnly
                                                    : rex::system::xam::alboria::PartyType::kJoinable);
  }
  RunInstead(ctx, base, create_system_link_party, g_system_link_party_locals);
  return true;
}

// Hosts a room and creates a System Link party where CreateLIVEParty was about to run.
bool CreateOnlineParty(PPCContext& ctx, uint8_t* base, uint32_t create_live_party) {
  if (!OnlineMode()) return false;
  return RunSystemLinkParty(ctx, base, create_live_party, true);
}

// Runs ConditionalCreatePartySession (`ctx`, about to run) for the party at start-up (BootParty):
// the one startup_party names, which the player chose last time, Joinable the first time. A room's
// party (Joinable or Invite only) when Alboria answered the start-up check and accepted the
// sign-in; Local when it did not answer, refused the sign-in (alboria::SignInRefusal: no account
// linked, banned, an old client) or a room failed (g_room_failed), which startup_party does not
// keep.
void CreateStartupParty(PPCContext& ctx, uint8_t* base) {
  namespace alboria = rex::system::xam::alboria;
  const std::string wanted = REXCVAR_GET(startup_party);
  int boot = kBootOnline;
  if (wanted == "system_link") {
    boot = kBootSystemLink;
    g_startup_party_type.store(kSystemLinkType);
  } else if (wanted == "local") {
    boot = kBootLocal;
    g_startup_party_type.store(kLocalPartyType);
  } else if (!alboria::ServiceReachable()) {
    boot = kBootLocal;
    g_startup_party_type.store(kLocalPartyType);
    REXLOG_INFO("[Azura online] Alboria did not answer: starting in Local");
  } else if (const std::string refusal = alboria::SignInRefusal(); !refusal.empty()) {
    boot = kBootLocal;
    g_startup_party_type.store(kLocalPartyType);
    REXLOG_INFO("[Azura online] Alboria refused the sign-in ({}): starting in Local", refusal);
    NoticeRefusal(refusal);
  } else if (g_room_failed.load()) {
    boot = kBootLocal;
    g_startup_party_type.store(kLocalPartyType);
    REXLOG_INFO("[Azura online] The last room failed: starting in Local");
  } else {
    g_online_party_type.store(wanted == "invite_only" ? kInviteRequiredType : kFriendsOnlyType);
    g_show_room_code.store(false);
  }
  REXLOG_INFO("[Azura online] Startup party: {} ({})", wanted,
              boot == kBootOnline ? "Alboria" : boot == kBootSystemLink ? "System Link" : "Local");
  g_boot_party.store(boot);
  __imp__sub_822DF9E0(ctx, base);
  g_boot_party.store(kBootNone);
}

// The party startup_party names, when it is not Local, still to be created in the main menu
// (kBootOnline or kBootSystemLink; kBootNone when there is none), and whether it is being created.
std::atomic<int> g_pending_startup_party{kBootNone};
std::atomic<bool> g_creating_startup_party{false};

// LocalPartyCreated (`ctx`, about to run) at start-up, when the party startup_party names is not
// Local: the Local party is created as the game does, and the one startup_party names is created
// over it in the main menu (CreatePendingStartupParty). Here, at the end of the failed Xbox LIVE
// party's OnCreatePartyComplete, CreateSystemLinkParty sets the party type but never reaches
// CreateParty, so the party would have no session and nobody could join its room. Never runs in
// LocalPartyCreated's place: returns false.
bool ReplaceStartupLocalParty(PPCContext& ctx, uint8_t* base, uint32_t local_party_created) {
  const int boot = g_boot_party.load();
  if (boot == kBootOnline || boot == kBootSystemLink) g_pending_startup_party.store(boot);
  return false;
}

// On a player input frame (`ctx`, about to run PlayerInput) in the front end's main menu: creates
// the party startup_party names over the Local one the game started with, as choosing it in the
// party options does: a room on Alboria and its System Link party (kBootOnline), or a plain System
// Link party (kBootSystemLink), with UIScene_FrontEnd.CreateSystemLinkParty on the front end's
// scene. Its ConditionalCreatePartySession then goes to the game, not to CreateStartupParty.
void CreatePendingStartupParty(PPCContext& ctx, uint8_t* base) {
  const int boot = g_pending_startup_party.load(std::memory_order_relaxed);
  if (boot == kBootNone || !InMainMenu(base)) return;
  g_pending_startup_party.store(kBootNone);
  const uint32_t scene = g_front_end_scene.load(std::memory_order_relaxed);
  const uint32_t front_end_class = g_front_end_class.load(std::memory_order_relaxed);
  const uint32_t create_system_link_party =
      front_end_class ? FindClassFunction(base, front_end_class, "CreateSystemLinkParty") : 0;
  if (!create_system_link_party) {
    REXLOG_WARN("[Azura online] CreateSystemLinkParty not found; the party stays Local");
    return;
  }
  REXLOG_INFO("[Azura online] Creating the start-up party in the main menu");
  const uint32_t frame = ctx.r4.u32;
  const uint32_t own_object = LoadU32(base, frame + kFrameObject);
  const uint64_t own_r3 = ctx.r3.u64;
  StoreU32(base, frame + kFrameObject, scene);
  ctx.r3.u64 = scene;
  g_creating_startup_party.store(true);
  RunSystemLinkParty(ctx, base, create_system_link_party, boot == kBootOnline);
  g_creating_startup_party.store(false);
  StoreU32(base, frame + kFrameObject, own_object);
  ctx.r3.u64 = own_r3;
}

// GearProfileSettings.GetPartyType (`ctx`, about to run), with online play on: the party the game
// started with when it is not the profile's (g_startup_party_type: System Link, or Local), until
// the player chooses one; otherwise the profile's. The game copies it into the party's
// CurrPartyType once the party exists (PartySuccessfullyCreatedNotify). A room's party is left as
// the System Link party it is: the game treats any other type as an Xbox Live party (private
// matches then need a Live session and do not start), and RunWithOnlinePartyType only changes what
// the party panel and options show.
void AnswerPartyType(PPCContext& ctx, uint8_t* base) {
  const uint32_t result = ctx.r5.u32;
  __imp__sub_822DF9E0(ctx, base);
  const int startup = g_startup_party_type.load();
  if (result && startup >= 0 && !rex::system::xam::alboria::InRoom()) base[result] = static_cast<uint8_t>(startup);
}

extern "C" void sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base) {
  if (g_names_missing.load(std::memory_order_relaxed) > 0 &&
      (g_calls.fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) {
    ScanNewNames(base);
  }
  const uint32_t function = LoadU32(base, ctx.r4.u32 + kFrameNode);
  // Before it runs: it reads aMouseX and aMouseY.
  if (function && IsEnginePlayerInput(base, function)) {
    if (g_open_browser.load(std::memory_order_relaxed)) OpenSystemLinkBrowserForJoin(ctx, base);
    ReturnToMainMenuWhenRoomLost(ctx, base);
    CreatePendingStartupParty(ctx, base);
    ShowRoomMessage(ctx, base);
    RefreshPartyPanelTitle(ctx, base);
    const uint32_t player_input = ctx.r3.u32;
    const int32_t delta_time_at = ParamOffset(base, function, "DeltaTime");
    const float delta_time =
        delta_time_at >= 0
            ? LoadF32(base, LoadU32(base, ctx.r4.u32 + kFrameLocals) + static_cast<uint32_t>(delta_time_at))
            : 0.0f;
    ApplyMouseLookFor(base, player_input);
    __imp__sub_822DF9E0(ctx, base);
    // After it: it has scaled the look input by the field of view and added the mouse to it.
    CompensateFieldOfView(base, player_input);
    RecordMouseShare(base, player_input, delta_time);
    return;
  }
  // The mouse's share of the look input is kept out of what the game does to it for the stick
  // (MouseShare), and the aim assist off for a player on the mouse.
  if (function && IsGearPlayerInput(base, function)) {
    __imp__sub_822DF9E0(ctx, base);
    PutBackMouseShare(base, ctx.r3.u32);
    return;
  }
  if (Is(base, function, kApplySensitivityScaling)) {
    const auto before = BeforeSensitivityScaling(base, ctx.r3.u32);
    __imp__sub_822DF9E0(ctx, base);
    if (before) TakeOutMouseShare(base, *before);
    return;
  }
  if (Is(base, function, kViewAdhesion)) {
    const auto before = BeforeViewAdhesion(base, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_822DF9E0(ctx, base);
    if (before) UndoViewAdhesion(base, *before);
    return;
  }
  if (Is(base, function, kGetAdjustedAimFor)) {
    const auto before = SuspendBulletAimAssist(base, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_822DF9E0(ctx, base);
    if (before) RestoreBulletAimAssist(base, *before);
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
  if (Is(base, function, kConditionalCreateParty) && OnlineMode() &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd" &&
      !rex::system::xam::alboria::InRoom() && !g_creating_startup_party.load()) {
    CreateStartupParty(ctx, base);
    return;
  }
  if (Is(base, function, kGetPartyType) && OnlineMode() &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "GearProfileSettings") {
    AnswerPartyType(ctx, base);
    return;
  }
  if (Is(base, function, kLocalPartyCreated) && ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd" &&
      ReplaceStartupLocalParty(ctx, base, function)) {
    return;
  }
  if (Is(base, function, kCanAllPlayersPlayOnline) && AnswerCanPlayOnline(ctx, base)) return;
  if (Is(base, function, kFindOnlineGamesComplete) && g_join_searches.load(std::memory_order_relaxed) > 0 &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_SystemLink") {
    __imp__sub_822DF9E0(ctx, base);
    JoinFoundParty(ctx, base, function);
    return;
  }
  if (Is(base, function, kOpenPartyOptions) && OnlineMode()) rex::system::xam::alboria::CheckService();
  if (Is(base, function, kUpdatePanelTitle)) {
    g_party_panel.store(LoadU32(base, ctx.r4.u32 + kFrameObject));
    g_party_panel_title_type.store(-1);
  }
  if ((Is(base, function, kBuildPartyOptions) || Is(base, function, kUpdatePanelTitle)) && OnlineMode() &&
      RunWithOnlinePartyType(ctx, base, function)) {
    return;
  }
  // Manage/Invite with online play on.
  // Manage/Invite's code and Join by code, with either kind of party on Alboria.
  if (REXCVAR_GET(network_mode) == "online" &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_WhatsUp") {
    namespace alboria = rex::system::xam::alboria;
    if (Is(base, function, kWidgetInitialized)) PrepareManageInviteHeading(ctx, base, function);
    if (Is(base, function, kRefreshManageInviteBar)) TrimManageInviteButtons(base, ctx.r3.u32);
    if (Is(base, function, kCanInviteGearsPlayers)) {
      if (ctx.r5.u32) StoreU32(base, ctx.r5.u32, alboria::Connected() ? 1 : 0);
      return;
    }
    // Joining by code moves an Alboria party (Joinable or Invite only) to a friend's room; the
    // Local and System Link parties stay off Alboria.
    if (Is(base, function, kCanInviteLiveParty)) {
      const bool can_join = alboria::ServiceReachable() && alboria::Connected();
      if (ctx.r5.u32) StoreU32(base, ctx.r5.u32, can_join ? 1 : 0);
      return;
    }
    if (Is(base, function, kInviteGearsPlayers)) {
      if (alboria::Connected()) ToggleRoomCode(ctx, base, function);
      return;
    }
    if (Is(base, function, kInviteLiveParty)) {
      // Greyed out (above) the button still reaches here: it does nothing then.
      if (alboria::ServiceReachable() && alboria::Connected()) OpenJoinByCode(ctx, base, function);
      return;
    }
  }
  if (Is(base, function, kDialogItemPress) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd" &&
      ChooseOnlinePartyType(ctx, base, function)) {
    return;
  }
  if (Is(base, function, kCreateLiveParty) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd" &&
      CreateOnlineParty(ctx, base, function)) {
    return;
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
  // Manage/Invite closed while the Join by code screen was open over it: nothing to close later.
  if (Is(base, function, kOnClose)) {
    uint32_t scene = ctx.r3.u32;
    g_manage_invite_scene.compare_exchange_strong(scene, 0);
  }
  if ((Is(base, function, kWidgetInitialized) || Is(base, function, kFilterButtonInput)) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd") {
    g_front_end_scene.store(ctx.r3.u32, std::memory_order_relaxed);
    g_front_end_class.store(LoadU32(base, function + kObjectOuter), std::memory_order_relaxed);
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
  if (Is(base, function, kFilterButtonInput) && HandleCreditsButton(ctx, base, function)) return;
  if (Is(base, function, kClientSetHUD)) SplitContentUpdatesOnce(ctx, base);
  if (Is(base, function, kOnCustomMapChanged)) {
    g_custom_map_id.store(SmallParam(base, function, ctx.r4.u32, "MapId"));
  }
  if (Is(base, function, kClientMenuSelected) &&
      ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_FrontEnd") {
    ClientMenuPosition(ctx, base);
  }
  // Connection, at the credits' place, runs Callback_Credits: it opens the Alboria menu. (The
  // credits' own button runs the function without passing here: HandleCreditsButton.)
  if (Is(base, function, kCredits) && ObjectName(base, LoadU32(base, function + kObjectOuter)) == "UIScene_Options") {
    std::function<void()> handler;
    {
      std::lock_guard lock(g_quit_mutex);
      handler = g_connection_handler;
    }
    REXLOG_INFO("[Azura options] Opening the Alboria menu");
    if (handler) handler();
    return;
  }
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
