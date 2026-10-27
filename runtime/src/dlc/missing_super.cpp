// Calls to a parent function the disc build does not have. See missing_super.h.

#include "missing_super.h"

#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace azura::dlc {

namespace {

constexpr uint32_t kGNames = 0x8343D148;  // TArray<FNameEntry*>
constexpr uint32_t kObjectOuter = 40;
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kObjectClass = 52;
constexpr uint32_t kFieldNext = 60;
constexpr uint32_t kStructSuper = 64;
constexpr uint32_t kStructChildren = 68;
constexpr uint32_t kFrameNode = 16;  // FFrame: the function running
constexpr uint32_t kFrameCode = 24;  // FFrame: its next bytecode

// Bytecode tokens of the arguments this takes (a local or member variable: the token and the
// property, an 8-byte reference in memory) and of the end of the arguments.
constexpr uint8_t kExLocalVariable = 0x00;
constexpr uint8_t kExInstanceVariable = 0x01;
constexpr uint8_t kExEndFunctionParms = 0x16;
constexpr uint32_t kVariableTokenSize = 1 + 8;

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

bool IsGuestPointer(uint32_t address) { return address >= 0x40000000u && address < 0xA0000000u; }

std::string ObjectName(const uint8_t* base, uint32_t object) {
  if (!IsGuestPointer(object)) return {};
  const uint32_t index = LoadU32(base, object + kObjectName);
  const uint32_t data = LoadU32(base, kGNames);
  if (!data || index >= LoadU32(base, kGNames + 4)) return {};
  const uint32_t entry = LoadU32(base, data + index * 4);
  if (!IsGuestPointer(entry)) return {};
  const bool wide = (LoadU32(base, entry) & 1) != 0;
  std::string text;
  for (uint32_t i = 0; i < 128; ++i) {
    const char c = wide ? static_cast<char>(base[entry + 8 + i * 2 + 1]) : static_cast<char>(base[entry + 8 + i]);
    if (c == 0) break;
    text.push_back(c);
  }
  return text;
}

// The function named `name` in `cls` or its parents, or 0.
uint32_t FindFunction(const uint8_t* base, uint32_t cls, const std::string& name) {
  for (uint32_t c = cls; IsGuestPointer(c); c = LoadU32(base, c + kStructSuper)) {
    for (uint32_t field = LoadU32(base, c + kStructChildren); IsGuestPointer(field);
         field = LoadU32(base, field + kFieldNext)) {
      if (ObjectName(base, field) == name && ObjectName(base, LoadU32(base, field + kObjectClass)) == "Function") {
        return field;
      }
    }
  }
  return 0;
}

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_resolved;  // calling function -> parent function (0: none)

}  // namespace

bool HandleMissingSuper(PPCContext& ctx, uint8_t* base) {
  const uint32_t frame = ctx.r4.u32;
  if (!IsGuestPointer(frame)) return false;
  const uint32_t caller = LoadU32(base, frame + kFrameNode);
  if (!IsGuestPointer(caller)) return false;

  uint32_t parent = 0;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_resolved.find(caller);
    if (it != g_resolved.end()) {
      parent = it->second;
    } else {
      // The caller's class's parents, from the class above the one that declares the caller.
      const uint32_t cls = LoadU32(base, caller + kObjectOuter);
      const std::string name = ObjectName(base, caller);
      parent = FindFunction(base, LoadU32(base, cls + kStructSuper), name);
      g_resolved.emplace(caller, parent);
      REXLOG_INFO("[Azura DLC] {}.{} calls a parent function the game lacks: {}", ObjectName(base, cls), name,
                  parent ? "runs " + ObjectName(base, LoadU32(base, parent + kObjectOuter)) + "." + name
                         : std::string("skipped"));
    }
  }
  if (parent) {
    ctx.r6.u64 = parent;
    return false;
  }

  // No parent function: skip the arguments (plain variables only) and leave the result empty.
  uint32_t code = LoadU32(base, frame + kFrameCode);
  for (int i = 0; i < 16; ++i) {
    const uint8_t token = base[code];
    if (token == kExEndFunctionParms) {
      StoreU32(base, frame + kFrameCode, code + 1);
      if (IsGuestPointer(ctx.r5.u32)) StoreU32(base, ctx.r5.u32, 0);
      return true;
    }
    if (token != kExLocalVariable && token != kExInstanceVariable) break;
    code += kVariableTokenSize;
  }
  REXLOG_ERROR("[Azura DLC] cannot skip the arguments of a call with no function from {}", ObjectName(base, caller));
  return false;
}

}  // namespace azura::dlc
