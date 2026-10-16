/*
 * Game languages, as Unreal Engine 3 names them in the game files (Coalesced_INT.bin,
 * Coalesced_FRA.bin, ...). Kept free of DOM access so it can be unit tested with Node.
 *
 * The launcher does not decide which languages exist: the native side lists the Coalesced bundles
 * found in the chosen game folder, and this module only names them and picks a default.
 */

/** Name of each engine language code, written in that language. */
const LANGUAGE_NAMES = {
  INT: "English",
  FRA: "Français",
  DEU: "Deutsch",
  ITA: "Italiano",
  ESN: "Español (España)",
  ESM: "Español (México)",
  PTB: "Português (Brasil)",
  RUS: "Русский",
  POL: "Polski",
  CZE: "Čeština",
  HUN: "Magyar",
  JPN: "日本語",
  KOR: "한국어",
  CHN: "中文",
};

/** Engine code that matches each launcher locale, used to preselect the game language. */
const LOCALE_TO_GAME_LANGUAGE = {
  en: "INT",
  fr: "FRA",
  de: "DEU",
  it: "ITA",
  es: "ESN",
  pt: "PTB",
  ru: "RUS",
  pl: "POL",
  ja: "JPN",
  ko: "KOR",
  "zh-Hant": "CHN",
  cs: "CZE",
  hu: "HUN",
};

/** Engine code the game falls back to; every edition ships it. */
export const DEFAULT_GAME_LANGUAGE = "INT";

/** Display name of an engine language code; unknown codes are shown as they are. */
export const gameLanguageName = (code) => LANGUAGE_NAMES[code] ?? code;

/**
 * Picks the language to preselect from those available in the game folder: the one matching the
 * launcher's language when the folder has it, otherwise English, otherwise the first available.
 * Returns null when the folder lists none.
 */
export const defaultGameLanguage = (available, launcherLocale) => {
  if (!available?.length) return null;
  const preferred = LOCALE_TO_GAME_LANGUAGE[launcherLocale];
  if (preferred && available.includes(preferred)) return preferred;
  if (available.includes(DEFAULT_GAME_LANGUAGE)) return DEFAULT_GAME_LANGUAGE;
  return available[0];
};
