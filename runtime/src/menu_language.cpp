#include "menu_language.h"

#include <rex/cvar.h>

REXCVAR_DECLARE(uint32_t, user_language);
REXCVAR_DECLARE(uint32_t, user_country);

namespace azura {

uint32_t MenuLanguage() {
  const uint32_t language = REXCVAR_GET(user_language);
  if (language != 1) return language;
  switch (REXCVAR_GET(user_country)) {
    case 23: return kLanguageCzech;
    case 42: return kLanguageHungarian;
    default: return language;
  }
}

}  // namespace azura
