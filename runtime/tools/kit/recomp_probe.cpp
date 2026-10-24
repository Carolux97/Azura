// Stands in for a file of generated code, so the player kit can learn how generated code compiles
// without generating any: scripts/build-kit.sh records the command CMake uses for this file (see
// azura_kit_recomp in runtime/CMakeLists.txt) and compiles it with the kit, to check the command
// against the headers the kit ships. It is never part of the runtime. It includes the SDK headers
// the generated precompiled header includes (the code generator's pch_h.inja).

#include <rex/chrono/clock.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>
#include <rex/system/mmio_handler.h>
#include <rex/thread/mutex.h>
#include <tracy/Tracy.hpp>

void AzuraKitRecompProbe(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
}
