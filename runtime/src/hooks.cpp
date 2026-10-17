#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

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
