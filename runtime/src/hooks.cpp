#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/system/flags.h>
#include <utf8cpp/utf8.h>
#include <string>
#include <string_view>
#include <iterator>

REXCVAR_DEFINE_STRING(user_name, "User", "Kernel", "User name / gamertag");

extern "C" void __imp__XGetLanguage(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t lang = REXCVAR_GET(user_language);
  if (lang == 0) {
    lang = 1;  // Default: English (1)
  }
  REXLOG_INFO("[Azura Hooks] XGetLanguage() invoked -> returning language ID {}", lang);
  ctx.r3.u64 = lang;
}

extern "C" void __imp__XamGetLanguage(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t lang = REXCVAR_GET(user_language);
  if (lang == 0) {
    lang = 1;  // Default: English (1)
  }
  REXLOG_INFO("[Azura Hooks] XamGetLanguage() invoked -> returning language ID {}", lang);
  ctx.r3.u64 = lang;
}

extern "C" void __imp__XamGetLocale(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t country = REXCVAR_GET(user_country);
  if (country == 0) {
    uint32_t lang = REXCVAR_GET(user_language);
    if (lang == 5) {
      country = 31;  // Spain (31)
    } else {
      country = 103; // US (103)
    }
  }
  REXLOG_INFO("[Azura Hooks] XamGetLocale() invoked -> returning Country ID {}", country);
  ctx.r3.u64 = country;
}

extern "C" void __imp__XGetGameRegion(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  REXLOG_INFO("[Azura Hooks] XGetGameRegion() invoked -> returning Region mask 0x00000200");
  ctx.r3.u64 = 0x00000200;  // XEX_REGION_EUROPE
}

extern "C" void __imp__XexCheckExecutablePrivilege(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t priv = ctx.r3.u32;
  REXLOG_INFO("[Azura Hooks] XexCheckExecutablePrivilege({}) -> granting privilege (1)", priv);
  ctx.r3.u64 = 1;
}

// Strong symbol instantiations to override throwing implementations from librexruntime.so
template utf8::utfchar32_t utf8::next<const char*>(const char*&, const char*);
template std::back_insert_iterator<std::u16string> utf8::utf8to16<std::back_insert_iterator<std::u16string>, const char*>(const char*, const char*, std::back_insert_iterator<std::u16string>);
template std::back_insert_iterator<std::string> utf8::utf16to8<const char16_t*, std::back_insert_iterator<std::string>>(const char16_t*, const char16_t*, std::back_insert_iterator<std::string>);

namespace rex::string {
std::u16string to_utf16(const std::string_view source) {
  std::u16string result;
  result.reserve(source.size());
  const char* it = source.data();
  const char* end = it + source.size();
  while (it < end) {
    uint8_t c = static_cast<uint8_t>(*it);
    if (c < 0x80) {
      result.push_back(char16_t(c));
      it++;
    } else {
      char32_t cp = utf8::next(it, end);
      if (cp <= 0xFFFF) {
        result.push_back(char16_t(cp));
      } else {
        result.push_back(char16_t((cp >> 10) + 0xD7C0));
        result.push_back(char16_t((cp & 0x3FF) + 0xDC00));
      }
    }
  }
  return result;
}

std::string to_utf8(const std::u16string_view source) {
  std::string result;
  result.reserve(source.size() * 2);
  utf8::utf16to8(source.data(), source.data() + source.size(), std::back_inserter(result));
  return result;
}
}

