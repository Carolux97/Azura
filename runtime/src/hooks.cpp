#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/system/flags.h>
#include <dlfcn.h>

#include "perf_overlay.h"

extern "C" void __imp__XGetLanguage(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t lang = REXCVAR_GET(user_language);
  if (lang == 0) {
    lang = 5;  // Default: Spanish (5)
  }
  REXLOG_INFO("[Azura Hooks] XGetLanguage() invoked -> returning language ID {}", lang);
  ctx.r3.u64 = lang;
}

extern "C" void __imp__XamGetLanguage(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t lang = REXCVAR_GET(user_language);
  if (lang == 0) {
    lang = 5;  // Default: Spanish (5)
  }
  REXLOG_INFO("[Azura Hooks] XamGetLanguage() invoked -> returning language ID {}", lang);
  ctx.r3.u64 = lang;
}

extern "C" void __imp__VdSwap(PPCContext& __restrict ctx, uint8_t* base) {
  static void (*real_VdSwap)(PPCContext& __restrict, uint8_t*) = nullptr;
  if (!real_VdSwap) {
    real_VdSwap = reinterpret_cast<void (*)(PPCContext& __restrict, uint8_t*)>(
        dlsym(RTLD_NEXT, "__imp__VdSwap"));
  }
  azura::RecordGuestFrame();
  if (real_VdSwap) {
    real_VdSwap(ctx, base);
  }
}
