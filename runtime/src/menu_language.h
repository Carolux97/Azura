#pragma once

#include <cstdint>

namespace azura {

// The language our own menus are shown in: the Xbox 360 language ID of the user_language setting,
// or one of the two below. The console has no Czech or Hungarian language, only those countries:
// set to English in the Czech Republic or Hungary, the game shows its Czech or Hungarian texts
// (CZE, HUN), and so do our menus.
constexpr uint32_t kLanguageCzech = 100;
constexpr uint32_t kLanguageHungarian = 101;

// user_language, or kLanguageCzech / kLanguageHungarian for English with user_country set to the
// Czech Republic (23) or Hungary (42), the IDs of the SDK's country table.
uint32_t MenuLanguage();

}  // namespace azura
