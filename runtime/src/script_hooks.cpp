// Hooks into the game's UnrealScript interpreter (Unreal Engine 3, recompiled).
//
// The recompiled engine calls every script function through UObject::CallFunction (guest
// 0x822DF4D0: r3 = object, r4 = FFrame& stack, r5 = result, r6 = UFunction*). Each generated
// function is a weak symbol that forwards to __imp__<name>, so defining sub_822DF4D0 here
// replaces it and __imp__sub_822DF4D0 still runs the original.
//
// Script names are FNames: an index into the global name table GNames (a TArray of FNameEntry*
// at guest 0x8343D148). An FNameEntry holds (index << 1 | unicode flag) at +0 and the characters
// at +8; a UObject holds its Outer at +40 and its FName at +44.
//
// The body of a script function runs in UObject::ProcessInternal (guest 0x822DF9E0: r3 = object,
// r4 = FFrame*, r5 = result). By then CallFunction has evaluated the parameters into the frame's
// locals. FFrame: +16 Node (UFunction*), +20 Object, +24 Code, +28 Locals. A function's
// parameters are its first child properties (UStruct children at +68, UField::Next at +60), each
// with PropertyFlags (64-bit) at +72 and its offset in the locals at +84.
//
// With `script_trace` on, the first call of every script function is logged as Class.Function,
// and every call of the functions listed in `script_trace_args` is logged with its parameters.
// That is how menu and gameplay functions, and what they receive, are identified.
//
// Quit Game: the main menu calls UIScene_FrontEnd.HandleMenuSelection(ParentScreen,
// SelectedOption, bIsSimulating, ...) when an option is chosen. When it is the Quit Game option
// added by menu_patches, the quit handler runs instead of the script, which would not know the
// option. To keep the hook cheap, script functions are matched by FName index; the index of
// "HandleMenuSelection" is found by scanning new GNames entries every so often until it appears.

#include <algorithm>
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

#include "menu/menu_patches.h"

REXCVAR_DEFINE_BOOL(script_trace, false, "Azura/Debug",
                    "Log the first call of every UnrealScript function (slow, for reverse engineering)");
REXCVAR_DEFINE_STRING(script_trace_args, "HandleMenuSelection", "Azura/Debug",
                      "With script_trace: comma-separated script functions whose every call is logged "
                      "with its parameters");

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
std::atomic<bool> g_quit_requested{false};

std::atomic<int32_t> g_handle_menu_selection{-1};  // FName index, once known
std::atomic<uint32_t> g_names_scanned{0};
std::atomic<uint32_t> g_calls{0};
std::mutex g_scan_mutex;

// Looks through the GNames entries added since the last scan for "HandleMenuSelection".
void ScanNewNames(const uint8_t* base) {
  std::unique_lock lock(g_scan_mutex, std::try_to_lock);
  if (!lock) return;
  const uint32_t count = LoadU32(base, kGNames + 4);
  for (uint32_t i = g_names_scanned.load(); i < count; ++i) {
    if (NameText(base, i) == "HandleMenuSelection") {
      g_handle_menu_selection.store(static_cast<int32_t>(i));
      break;
    }
  }
  g_names_scanned.store(count);
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

}  // namespace

extern "C" void sub_822DF4D0(PPCContext& __restrict ctx, uint8_t* base) {
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

}  // namespace azura::script

extern "C" void sub_822DF9E0(PPCContext& __restrict ctx, uint8_t* base) {
  const int quit_option = azura::menu::MainMenuQuitOption();
  if (quit_option >= 0) {
    const int32_t target = g_handle_menu_selection.load(std::memory_order_relaxed);
    if (target < 0) {
      if ((g_calls.fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) ScanNewNames(base);
    } else {
      const uint32_t function = LoadU32(base, ctx.r4.u32 + kFrameNode);
      if (function && LoadU32(base, function + kObjectName) == static_cast<uint32_t>(target) &&
          IsQuitSelection(base, ctx.r4.u32, quit_option)) {
        RequestQuit();
        return;  // the script would not know the option and would leave the menu waiting
      }
    }
  }
  if (REXCVAR_GET(script_trace)) {
    const uint32_t frame = ctx.r4.u32;
    const uint32_t function = LoadU32(base, frame + kFrameNode);
    if (function && InList(REXCVAR_GET(script_trace_args), ObjectName(base, function))) {
      REXLOG_INFO("[Azura script] call {}", DescribeCall(base, frame));
    }
  }
  __imp__sub_822DF9E0(ctx, base);
}
