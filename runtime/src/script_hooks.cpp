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
// To keep the hooks cheap, script functions are matched by FName index; the indices are found by
// scanning new GNames entries every so often until they appear.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "script_hooks.h"

#include <atomic>
#include <functional>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "class_dump.h"
#include "menu/menu_patches.h"
#include "menu/pc_options.h"

REXCVAR_DEFINE_BOOL(script_trace, false, "Azura/Debug",
                    "Log the first call of every UnrealScript function (slow, for reverse engineering)");
REXCVAR_DEFINE_STRING(script_trace_args, "HandleMenuSelection", "Azura/Debug",
                      "With script_trace: comma-separated script functions whose every call is logged "
                      "with its parameters");
REXCVAR_DEFINE_STRING(script_dump_classes, "", "Azura/Debug",
                      "Comma-separated script classes whose functions and properties are logged once loaded");
REXCVAR_DEFINE_BOOL(script_dump_code, false, "Azura/Debug",
                    "With script_dump_classes: also log a rough listing of each function's bytecode");

extern "C" void __imp__sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kGNames = 0x8343D148;     // TArray<FNameEntry*>: data, num, max
constexpr uint32_t kObjectOuter = 40;
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kObjectClass = 52;
constexpr uint32_t kFieldNext = 60;
constexpr uint32_t kStructChildren = 68;
constexpr uint32_t kPropertyFlags = 72;
constexpr uint32_t kPropertyOffset = 84;
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

// "Class.Function(param=value, ...)" for a frame about to run.
std::string DescribeCall(const uint8_t* base, uint32_t frame) {
  const uint32_t function = LoadU32(base, frame + kFrameNode);
  const uint32_t locals = LoadU32(base, frame + kFrameLocals);
  std::string text = ObjectName(base, LoadU32(base, function + kObjectOuter)) + "." + ObjectName(base, function) + "(";
  bool first = true;
  for (uint32_t prop = LoadU32(base, function + kStructChildren); prop; prop = LoadU32(base, prop + kFieldNext)) {
    const uint64_t flags = (uint64_t(LoadU32(base, prop + kPropertyFlags)) << 32) | LoadU32(base, prop + kPropertyFlags + 4);
    if (!(flags & kPropertyParm)) break;  // parameters come first; locals follow
    const std::string type = ObjectName(base, LoadU32(base, prop + kObjectClass));
    text += (first ? "" : ", ") + ObjectName(base, prop) + "=" + ParamValue(base, type, locals + LoadU32(base, prop + kPropertyOffset));
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

// --- Quit Game ---

std::mutex g_quit_mutex;
std::function<void()> g_quit_handler;
std::function<void()> g_restart_handler;
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
  kResetAcceptAlias,         // button alias of the Defaults message box's Accept button
  kGenericAcceptAlias,       // button alias of the game's plain Accept button
  kBuildCampaignURL,         // GearMenuGame: builds travel URL for campaign
  kServerTravel,             // WorldInfo: travels to a new level
  kProcessServerTravel,      // GameInfo: processes server travel URL
  kWatchedNameCount,
};
constexpr const char* kWatchedNames[kWatchedNameCount] = {
    "HandleMenuSelection",   "PopulateOptionsList",    "GetOptionValueAt",
    "OnConfirm_ResetDefaults", "FilterButtonInput",    "Callback_VideoOptions",
    "CloseVideoOptionsScene", "Callback_MenuOptionSelected", "Callback_ReturnToMain",
    "ResetAccept",           "GenericAccept",
    "BuildCampaignURL",      "ServerTravel",           "ProcessServerTravel"};

std::atomic<int32_t> g_names[kWatchedNameCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
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
constexpr uint32_t kPopulateRow = 0;                // PopulateOptionsList local I (row index)
constexpr uint32_t kPopulateSliderValue = 172;      // PopulateOptionsList local Slider_Value
constexpr uint32_t kPopulateListIndex = 176;        // PopulateOptionsList local OptionList_Index
constexpr uint32_t kGetOptionValueAtIndex = 0;      // GetOptionValueAt parameter Index

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

// Callback_MenuOptionSelected(SelectedOption) starts with
//   if (SelectedOption == 5 || SelectedOption == 1 || SelectedOption == 7) return;
// because those positions (storage location, Video, Credits) opened another screen instead of a
// list. Video and Keyboard and mouse (at the storage location's position) are lists now, so:
//   - in `SelectedOption == 5` the 5 (a one-byte int constant) becomes 255, not a position;
//   - in `SelectedOption == 1` the one (EX_IntOne, one byte) becomes EX_Self, also one byte: it
//     evaluates to the scene's own address, which is never a position.
// Both comparisons are then always false, and the bytecode keeps its size and its jump offsets.
// The ones patched are those of this condition (from `SelectedOption == 5` on), so an earlier one
// that only picks a sound is left alone. Done once.
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
    if (!after_five && base[code + i + kOperand] == kTokenIntConstByte &&
        base[code + i + kOperand + 1] == azura::menu::kKeyboardMouseCategory) {
      base[code + i + kOperand + 1] = 0xFF;
      after_five = true;
      continue;
    }
    if (after_five && base[code + i + kOperand] == kTokenIntOne &&
        base[code + i + kOperand + 1] == kTokenEndFunctionParms) {
      base[code + i + kOperand] = kTokenSelf;
      REXLOG_INFO("[Azura options] Video and Keyboard and mouse now show their lists");
      return;
    }
  }
  REXLOG_WARN("[Azura options] Callback_MenuOptionSelected does not skip 5 and 1 as expected; those stay empty");
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

void RequestRestart() {
  std::function<void()> handler;
  {
    std::lock_guard lock(g_quit_mutex);
    handler = g_restart_handler;
  }
  REXLOG_INFO("[Azura options] Restarting the game to apply Video settings");
  if (handler) handler();
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

// Table of persistent maps for each of the 31 campaign chapters (from Xbox360-GearGame.ini)
constexpr const char* const kChapterMaps[31] = {
    "sp_nightmare_p",         // 0: Prólogo
    "sp_ravens_p",            // 1: Acto 1, Cap 1
    "sp_ravens2_p",           // 2: Acto 1, Cap 2
    "sp_hanover_p",           // 3: Acto 1, Cap 3
    "sp_warehouse_p",         // 4: Acto 1, Cap 4
    "sp_stadium_p",           // 5: Acto 1, Cap 5
    "sp_bridge_p",            // 6: Acto 1, Cap 6
    "sp_wreckage_p",          // 7: Acto 2, Cap 1
    "sp_badlands_p",          // 8: Acto 2, Cap 2
    "sp_badlands_p",          // 9: Acto 2, Cap 3
    "sp_badlands2_p",         // 10: Acto 2, Cap 4
    "sp_badlands2_p",         // 11: Acto 2, Cap 5
    "sp_blimp1_p",            // 12: Acto 2, Cap 6
    "sp_blimp2_p",            // 13: Acto 2, Cap 7
    "sp_fort_p",              // 14: Acto 3, Cap 1
    "sp_fort_p",              // 15: Acto 3, Cap 2
    "sp_convoy_p",            // 16: Acto 3, Cap 3
    "sp_mercy_p",             // 17: Acto 3, Cap 4
    "sp_mercy_p",             // 18: Acto 3, Cap 5
    "sp_char_p",              // 19: Acto 4, Cap 1
    "sp_char_p",              // 20: Acto 4, Cap 2
    "sp_char_p",              // 21: Acto 4, Cap 3
    "SP_Oldshipyard_Intro_P", // 22: Acto 4, Cap 4
    "sp_oldshipyard_p",       // 23: Acto 4, Cap 5
    "sp_maelstrom_p",         // 24: Acto 4, Cap 6
    "sp_coast_p",             // 25: Acto 5, Cap 1
    "sp_coast_p",             // 26: Acto 5, Cap 2
    "SP_Azura_Intro_P",       // 27: Acto 5, Cap 3
    "sp_azura2_p",            // 28: Acto 5, Cap 4
    "sp_tower_p",             // 29: Acto 5, Cap 5
    "SP_Rooftop_P",           // 30: Acto 5, Cap 6
};

// Several chapters are sections of the same persistent map; duplicate entries are intentional.
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

// Intercepts campaign and menu travel URLs to prevent infinite socket/network stalls
// and replaces GearGame_P with the chapter's real persistent level map from kChapterMaps.
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

  auto write_url = [&](const std::string& value) {
    if (value.size() >= max) {
      REXLOG_WARN("[Azura travel] Rewritten URL needs {} TCHARs, but capacity is {}; URL left unchanged",
                  value.size() + 1, max);
      return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
      base[data + i * 2] = 0;
      base[data + i * 2 + 1] = static_cast<uint8_t>(value[i]);
    }
    base[data + value.size() * 2] = 0;
    base[data + value.size() * 2 + 1] = 0;
    StoreU32(base, fstring_addr + 4, static_cast<uint32_t>(value.size() + 1));
    return true;
  };

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

  int chapter = -1;
  size_t ch_pos = lower_url.find("chapter=");
  if (ch_pos != std::string::npos) {
    size_t num_start = ch_pos + 8;
    size_t num_end = lower_url.find_first_not_of("0123456789", num_start);
    std::string ch_str = lower_url.substr(num_start, num_end == std::string::npos ? std::string::npos : num_end - num_start);
    if (!ch_str.empty()) {
      try {
        chapter = std::stoi(ch_str);
      } catch (...) {}
    }
  }

  if (chapter < 0 || chapter >= 31) {
    REXLOG_WARN("[Azura campaign] GearGame_P requested but invalid chapter {}", chapter);
    return false;
  }

  const char* real_map = kChapterMaps[chapter];
  size_t qpos = url.find('?');
  std::string new_url = std::string(real_map) + (qpos != std::string::npos ? url.substr(qpos) : "");

  // This hook has no verified single-player/co-op discriminator; stripping listen may affect co-op.
  StripListenParam(new_url);

  uint32_t new_count = static_cast<uint32_t>(new_url.size() + 1);

  // If it still doesn't fit, strip package prefix "geargamecontent." from game class
  if (new_count > max) {
    size_t pkg_pos = new_url.find("geargamecontent.");
    if (pkg_pos != std::string::npos) {
      new_url.erase(pkg_pos, 16);
    }
    new_count = static_cast<uint32_t>(new_url.size() + 1);
  }

  // If it still doesn't fit, strip redundant ?chapter=X
  if (new_count > max) {
    size_t ch_p = new_url.find("?chapter=");
    if (ch_p == std::string::npos) ch_p = new_url.find("&chapter=");
    if (ch_p != std::string::npos) {
      size_t next_q = new_url.find_first_of("?&", ch_p + 1);
      new_url.erase(ch_p, next_q == std::string::npos ? std::string::npos : next_q - ch_p);
    }
    new_count = static_cast<uint32_t>(new_url.size() + 1);
  }

  // If it still doesn't fit, strip ?loadcheckpoint=0
  if (new_count > max) {
    size_t lc_pos = new_url.find("?loadcheckpoint=0");
    if (lc_pos == std::string::npos) lc_pos = new_url.find("&loadcheckpoint=0");
    if (lc_pos != std::string::npos) {
      new_url.erase(lc_pos, 17);
    }
    new_count = static_cast<uint32_t>(new_url.size() + 1);
  }

  // Final fallback: just the persistent map name to ensure it never loads empty GearGame_P
  if (new_count > max) {
    new_url = real_map;
    new_count = static_cast<uint32_t>(new_url.size() + 1);
  }

  if (new_count > max) {
    REXLOG_WARN("[Azura campaign] Rewritten URL for chapter {} needs {} TCHARs, but capacity is {}; URL left unchanged",
                chapter, new_count, max);
    return false;
  }

  REXLOG_INFO("[Azura campaign] Remapped campaign map for chapter {}: '{}' -> '{}' (buffer count={}, max={})",
              chapter, url, new_url, count, max);

  return write_url(new_url);
}

}  // namespace

extern "C" void sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base) {
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

}  // namespace azura::script

extern "C" void sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base) {
  if (g_names_missing.load(std::memory_order_relaxed) > 0 &&
      (g_calls.fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) {
    ScanNewNames(base);
  }
  const uint32_t function = LoadU32(base, ctx.r4.u32 + kFrameNode);
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
  const int quit_option = azura::menu::MainMenuQuitOption();
  if (quit_option >= 0 && Is(base, function, kHandleMenuSelection) && IsQuitSelection(base, ctx.r4.u32, quit_option)) {
    RequestQuit();
    return;  // the script would not know the option and would leave the menu waiting
  }
  if (Is(base, function, kMenuOptionSelected)) LetListsShow(base, function);
  if (Is(base, function, kReturnToMain)) LetVideoPlayLeaveSound(base, function);
  // Closing the calibration screen runs Callback_ReturnToMain, which puts the script back on the
  // list of categories: right when the screen was opened from there, as in the game. Opened from
  // the Video list, Flash goes back to that list instead, so the script must stay on it too: that
  // one Callback_ReturnToMain is skipped, and the selection stays on the calibration row.
  if (Is(base, function, kCloseVideoOptionsScene) && g_calibration_open.exchange(false)) {
    g_returning_from_calibration.store(true);
  }
  if (Is(base, function, kReturnToMain) && g_returning_from_calibration.exchange(false)) return;
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
  if (Is(base, function, kFilterButtonInput) && HandleOptionsButton(ctx, base, function)) return;
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
    }
  }
  __imp__sub_822DF9E0(ctx, base);
}
