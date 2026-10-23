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

/**
 * State of the runtime (see src-tauri/src/install.rs): { installed, buildFound, updateAvailable,
 * path }. A browser has none installed and pretends a build is available.
 */
export const getRuntimeStatus = async () =>
  isNativeApp
    ? tauri.core.invoke("get_runtime_status")
    : { installed: false, buildFound: true, updateAvailable: true, path: "" };

/** Installs or updates the runtime from the project's build; resolves to the new state. */
export const installRuntime = async () =>
  isNativeApp
    ? tauri.core.invoke("install_runtime")
    : { installed: true, buildFound: true, updateAvailable: false, path: "" };

/** Opens the folder picker for the saves; resolves to the path, or null when cancelled. */
export const pickSavesFolder = async () =>
  isNativeApp ? tauri.core.invoke("pick_saves_folder") : "~/Documents/Games Saves/Gears3-Azura";

/**
 * Moves the saves to `folder` and makes it the saves folder. Rejects with { kind, detail }:
 * "destinationHasSaves" (call again with `replace` to replace them), "nestedFolders" or "io".
 */
export const changeSavesFolder = async (folder, replace = false) =>
  isNativeApp ? tauri.core.invoke("change_saves_folder", { folder, replace }) : folder;

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
 * "runtimeMissing", "alreadyRunning", "startFailed"). Its output, restarts and end arrive
 * through onGameOutput, onGameRestart and onGameExit.
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
 * Calls `listener` when the game closes to start again (to apply settings that only take effect
 * at start-up): the launcher starts it at once, so the game is still running. The browser
 * simulation never restarts.
 */
export const onGameRestart = (listener) => {
  if (isNativeApp) return tauri.event.listen("game-restart", () => listener());
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

/** Checks the status of saved games and corruptions. */
export const getSaveStatus = async () => {
  if (isNativeApp) return tauri.core.invoke("get_save_status");
  return {
    hasSaves: false,
    checkpointCount: 0,
    isCorrupted: false,
    status: "empty",
    message: "Modo simulación en navegador",
    savePath: "~/.local/share/azura/B13EBABEBABEBABE/4D5308AB",
    lastSaved: null,
    backups: [],
  };
};

/** Repairs orphan/corrupted save headers to prevent the corrupt save dialog. */
export const repairSaves = async () => {
  if (isNativeApp) return tauri.core.invoke("repair_saves");
  return "Reparación simulada completada.";
};

/** Clears all saves to start fresh, with an automatic backup first. */
export const resetSaves = async () => {
  if (isNativeApp) return tauri.core.invoke("reset_saves");
  return "Guardado restablecido con éxito (simulado).";
};

/** Creates a backup of the current saves. */
export const createSaveBackup = async (label) => {
  if (isNativeApp) return tauri.core.invoke("create_save_backup", { label });
  return { id: "backup_simulado", date: "Ahora", sizeBytes: 1024 };
};

/** Restores a selected backup. */
export const restoreSaveBackup = async (backupId) => {
  if (isNativeApp) return tauri.core.invoke("restore_save_backup", { backupId });
  return "Copia restaurada con éxito (simulado).";
};

/** Opens the save directory in the file manager. */
export const openSavesFolder = async () => {
  if (isNativeApp) return tauri.core.invoke("open_saves_folder");
};


/**
 * The player's achievements for the Achievements section (see src-tauri/src/achievements.rs), for
 * the game in `gameFolder`, with their texts in `gameLanguage` (an engine code), else in
 * `uiLocale`, else in English. Resolves to
 *   { status: "ok", achievements, unlockedCount, earnedGamerscore, totalGamerscore, completedAt }
 * or { status: "noGame" }, { status: "noRuntime" }, { status: "unavailable", detail }.
 * The browser simulation sends 50 placeholder achievements with a few unlocked and one hidden.
 */
export const getAchievements = async ({ gameFolder, gameLanguage, uiLocale }) => {
  if (isNativeApp) return tauri.core.invoke("get_achievements", { gameFolder, gameLanguage, uiLocale });
  if (!gameFolder) return { status: "noGame" };
  const unlockedAt = Date.parse("2026-10-21T21:16:05Z");
  const achievements = Array.from({ length: 50 }, (_, index) => {
    const id = index + 1;
    const unlocked = id <= 3;
    const hidden = id === 19;
    return {
      id,
      name: hidden ? null : `Achievement ${id}`,
      description: hidden ? null : unlocked ? "Simulated achievement, unlocked." : "Simulated achievement, still locked.",
      gamerscore: hidden ? null : 20,
      hidden,
      unlocked,
      unlockedAt: unlocked ? unlockedAt + id * 3_600_000 : null,
      icon: null,
    };
  });
  return { status: "ok", achievements, unlockedCount: 3, earnedGamerscore: 60, totalGamerscore: 1000, completedAt: null };
};
