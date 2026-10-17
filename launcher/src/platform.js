/*
 * Bridge between the interface and the operating system.
 *
 * Inside the Tauri app these functions call the Rust commands in src-tauri/src/lib.rs. Opened in
 * a plain browser (for design work) there is no native side, so they return simulated results (or
 * use localStorage for settings) and the interface keeps working.
 *
 * A folder check is an object with a `status`:
 *   { status: "ok", folder, titleId, mediaId, versionId, versionName, languages }
 *                                                a known build of the game: playable
 *   { status: "unsupportedVersion", folder, titleId, mediaId, languages }
 *                                                the game, in a build with no manifest yet
 *   { status: "wrongGame", folder, titleId }     an Xbox 360 executable of another game
 *   { status: "notADirectory", folder }          the path is missing or not a folder
 *   { status: "missingXex", folder }             no default.xex in it or its subfolders
 *   { status: "invalidXex", folder }             default.xex is not an Xbox 360 executable
 * `folder` is where the executable was found, or the path that was checked when it was not.
 * `languages` lists the engine language codes found in the folder (see gameLanguages.js).
 */

/** Tauri's global API, present only inside the app (app.withGlobalTauri in tauri.conf.json). */
const tauri = globalThis.__TAURI__;

/** True when running inside the Tauri app rather than a browser. */
export const isNativeApp = Boolean(tauri);

/** Result the browser simulation returns for any folder: the World edition (see versions.toml). */
const simulatedCheck = (folder) => ({
  status: "ok",
  folder,
  titleId: "4D5308AB",
  mediaId: "58122AD6",
  versionId: "world",
  versionName: "World",
  languages: ["CHN", "CZE", "FRA", "HUN", "INT", "KOR", "POL", "PTB", "RUS"],
});

/**
 * Opens the system folder picker and checks the chosen folder. Resolves to null when the user
 * cancels.
 */
export const pickGameFolder = async () => {
  if (!isNativeApp) return simulatedCheck("~/Games/Gears of War 3");
  return tauri.core.invoke("pick_game_folder");
};

/** Checks a folder chosen in an earlier session. */
export const checkGameFolder = async (path) => {
  if (!isNativeApp) return simulatedCheck(path);
  return tauri.core.invoke("check_game_folder", { path });
};

/** localStorage key of the settings in the browser simulation. */
const BROWSER_SETTINGS_KEY = "azura.settings";

/**
 * Loads the settings (see src-tauri/src/settings.rs for their shape). In the app they come from
 * azura.toml and launcher.toml; in a browser, from localStorage, which can be unavailable.
 */
export const loadSettings = async () => {
  if (isNativeApp) return tauri.core.invoke("load_settings");
  try {
    return JSON.parse(localStorage.getItem(BROWSER_SETTINGS_KEY) ?? "null");
  } catch {
    return null;
  }
};

/** Saves the settings; in a browser a failure to store them is ignored. */
export const saveSettings = async (settings) => {
  if (isNativeApp) return tauri.core.invoke("save_settings", { settings });
  try {
    localStorage.setItem(BROWSER_SETTINGS_KEY, JSON.stringify(settings));
  } catch {
    // Not stored: the settings still apply until the page closes.
  }
};

/** Describes this Mac ({ chip, memoryGb, arch }); null in a browser, where it is unknown. */
export const getSystemInfo = async () => (isNativeApp ? tauri.core.invoke("get_system_info") : null);

/** Whether the runtime executable is installed ({ installed, path }). A browser has none. */
export const getRuntimeStatus = async () =>
  isNativeApp ? tauri.core.invoke("get_runtime_status") : { installed: false, path: "" };

/** Listeners for the simulated game in a browser; the app uses Tauri events instead. */
const simulated = { output: new Set(), exit: new Set() };

/** Lines the browser simulation prints, to design the log panel without a runtime. */
const SIMULATED_OUTPUT = [
  ["stdout", "azura: loading azura.toml"],
  ["stdout", "azura: GPU plugin xenos (Vulkan on Metal)"],
  ["stderr", "azura: simulated run, no game started"],
];

/**
 * Starts the game. Rejects with { kind, detail } when it cannot start (see runtime.rs:
 * "runtimeMissing", "alreadyRunning", "startFailed"). Its output and end arrive through
 * onGameOutput and onGameExit.
 */
export const launchGame = async () => {
  if (isNativeApp) return tauri.core.invoke("launch_game");
  SIMULATED_OUTPUT.forEach(([stream, line], index) => {
    setTimeout(() => simulated.output.forEach((listener) => listener({ stream, line })), 300 * (index + 1));
  });
  setTimeout(() => simulated.exit.forEach((listener) => listener({ code: 0, success: true })), 1200);
};

/** Calls `listener` with { stream, line } for every line the game prints. */
export const onGameOutput = (listener) => {
  if (isNativeApp) return tauri.event.listen("game-output", (event) => listener(event.payload));
  simulated.output.add(listener);
};

/** Calls `listener` with { code, success } when the game ends. */
export const onGameExit = (listener) => {
  if (isNativeApp) return tauri.event.listen("game-exit", (event) => listener(event.payload));
  simulated.exit.add(listener);
};

/**
 * Asks for a newer launcher version: resolves to { version, currentVersion, notes } or null. It
 * rejects when the update endpoint cannot be reached (offline, or a repository that is still
 * private); a browser has no updater and always resolves to null.
 */
export const checkForUpdate = async () => (isNativeApp ? tauri.core.invoke("check_for_update") : null);

/** Installs the newer version and restarts the launcher; rejects with a message on failure. */
export const installUpdate = async () => tauri.core.invoke("install_update");

/** Calls `listener` with { downloaded, total } while an update downloads. */
export const onUpdateProgress = (listener) => {
  if (isNativeApp) return tauri.event.listen("update-progress", (event) => listener(event.payload));
};

/** Version of the running launcher, from its bundle; `fallback` in a browser. */
export const getAppVersion = async (fallback) => (isNativeApp ? tauri.app.getVersion() : fallback);
