#include "system_settings.h"

#include <array>
#include <atomic>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, depth_of_field);
REXCVAR_DECLARE(bool, dynamic_shadows);

namespace azura::system_settings {

namespace {

// A setting the options switch: its fields in the engine's settings object. The object holds some
// settings more than once (a main block, a short block the renderer reads some of them from, and a
// second copy of the main block), and a setting is turned off in every copy.
struct Setting {
  bool (*enabled)();
  std::array<uint32_t, 2> offsets;
};

// Offsets verified by starting the game with each [SystemSettings] value changed and reading the
// object (docs/reverse-engineering.md, "Engine settings (SystemSettings)").
const std::array<Setting, 2> kSettings = {{
    // DepthOfField: the main block and its copy.
    {[] { return REXCVAR_GET(depth_of_field); }, {64, 1156}},
    // DynamicShadows: the shadow settings and the short block.
    {[] { return REXCVAR_GET(dynamic_shadows); }, {888, 1044}},
}};

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

std::atomic<uint32_t> g_object{0};

// The game's values of a setting's fields, taken when the option turns it off, to put back when
// the option turns it on again. Guarded by g_mutex.
struct Saved {
  bool off = false;
  std::array<uint32_t, 2> values{};
};
std::mutex g_mutex;
std::array<Saved, 2> g_saved;

}  // namespace

void NoteSettingsObject(uint32_t address) {
  uint32_t expected = 0;
  if (g_object.compare_exchange_strong(expected, address)) {
    REXLOG_INFO("[Azura options] Engine settings object at {:08X}", address);
  }
}

void Apply(uint8_t* base) {
  const uint32_t object = g_object.load(std::memory_order_relaxed);
  if (!object) return;
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const Setting& setting = kSettings[i];
    Saved& saved = g_saved[i];
    if (!setting.enabled()) {
      if (!saved.off) {
        for (size_t j = 0; j < setting.offsets.size(); ++j) {
          saved.values[j] = LoadU32(base, object + setting.offsets[j]);
        }
        saved.off = true;
      }
      // Every frame, in case the engine writes its settings again (a settings profile it applies).
      for (uint32_t offset : setting.offsets) StoreU32(base, object + offset, 0);
    } else if (saved.off) {
      for (size_t j = 0; j < setting.offsets.size(); ++j) {
        StoreU32(base, object + setting.offsets[j], saved.values[j]);
      }
      saved.off = false;
    }
  }
}

}  // namespace azura::system_settings
