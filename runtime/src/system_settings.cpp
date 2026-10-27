#include "system_settings.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(bool, depth_of_field);
REXCVAR_DECLARE(bool, dynamic_shadows);
REXCVAR_DECLARE(bool, ambient_occlusion);
REXCVAR_DECLARE(bool, bloom);
REXCVAR_DECLARE(bool, lens_flares);

namespace azura::system_settings {

namespace {

// A setting the options switch: its fields in the engine's settings object. The object holds some
// settings more than once (a main block, a short block the renderer reads some of them from, and a
// second copy of the main block), and a setting is turned off in every copy.
struct Setting {
  bool (*enabled)();
  std::vector<uint32_t> offsets;
};

// Offsets verified by starting the game with each [SystemSettings] value changed and reading the
// object (docs/reverse-engineering.md, "Engine settings (SystemSettings)"). The game reads depth of
// field (+64), ambient occlusion (+1040), bloom (+72) and lens flares (+96) as it draws; those are
// the reads the community patches for this game replace with 0.
const std::array<Setting, 5> kSettings = {{
    {[] { return REXCVAR_GET(depth_of_field); }, {64, 1156}},
    {[] { return REXCVAR_GET(dynamic_shadows); }, {888, 1044}},
    {[] { return REXCVAR_GET(ambient_occlusion); }, {68, 1040, 1160}},
    {[] { return REXCVAR_GET(bloom); }, {72, 1164}},
    {[] { return REXCVAR_GET(lens_flares); }, {96, 1188}},
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
  std::vector<uint32_t> values;
};
std::mutex g_mutex;
std::array<Saved, kSettings.size()> g_saved;

}  // namespace

void Apply(uint8_t* base) {
  const uint32_t object = g_object.load(std::memory_order_relaxed);
  if (!object) return;
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < kSettings.size(); ++i) {
    const Setting& setting = kSettings[i];
    Saved& saved = g_saved[i];
    if (!setting.enabled()) {
      if (!saved.off) {
        saved.values.clear();
        for (uint32_t offset : setting.offsets) saved.values.push_back(LoadU32(base, object + offset));
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

// Mid-function hook (manifest) right after the game reads its ambient occlusion setting from the
// engine's settings object: r10 holds the object's address, in every edition.
void AzuraNoteEngineSettings(PPCRegister& r10) {
  uint32_t expected = 0;
  if (azura::system_settings::g_object.compare_exchange_strong(expected, r10.u32)) {
    REXLOG_INFO("[Azura options] Engine settings object at {:08X}", r10.u32);
  }
}
