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
 *   { status: "notADirectory", folder }          the path is missing, or neither a folder nor a file
 *   { status: "notADiscImage", folder }          a file that is not an Xbox 360 disc image
 *   { status: "missingXex", folder }             no default.xex in it or its subfolders
 *   { status: "missingXexInImage", folder }      a disc image without default.xex
 *   { status: "invalidXex", folder }             default.xex is not an Xbox 360 executable
 * The game is a folder or a disc image (.iso). `folder` is where the executable was found (the
 * disc image itself for one), or the path that was checked when it was not.
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

/**
 * Opens the system file picker for a disc image (.iso) of the game and checks it. Resolves to null
 * when the user cancels.
 */
export const pickGameImage = async () => {
  if (!isNativeApp) return simulatedCheck("~/Games/Gears of War 3.iso");
  return tauri.core.invoke("pick_game_image");
};

/**
 * Calls `onDrop(path)` with the path of a file or folder dropped on the window. Only in the app: a
 * browser gives pages no paths.
 */
export const onPathDropped = (onDrop) => {
  if (!isNativeApp) return;
  tauri.webview.getCurrentWebview().onDragDropEvent((event) => {
    if (event.payload.type === "drop" && event.payload.paths.length === 1) onDrop(event.payload.paths[0]);
  });
};

/** Checks a folder or disc image chosen in an earlier session, or dropped on the window. */
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
 * What this launcher version brings ({ version, items }, from launcher/CHANGELOG.md), or null when
 * the changelog has no section for it. A browser shows a sample.
 */
export const getReleaseNotes = async () =>
  isNativeApp
    ? tauri.core.invoke("get_release_notes")
    : { version: "0.1.0", items: ["Builds the game for this computer from your own copy of Gears of War 3."] };

/**
 * Lists the graphics cards the game can run on: { devices, automatic } (see src-tauri/src/gpus.rs).
 * Empty on macOS and in a browser.
 */
export const listGpus = async () =>
  isNativeApp ? tauri.core.invoke("list_gpus") : { devices: [], automatic: null };

/** Calls `listener` when the chosen graphics card is gone and the game starts with the automatic one. */
export const onGpuMissing = (listener) => {
  if (isNativeApp) return tauri.event.listen("gpu-missing", () => listener());
};

/**
 * State of the runtime (see src-tauri/src/install.rs): { installed, buildFound, updateAvailable,
 * path, source, updateKind }. `source` is "build" (a project build, copied), "kit" (built here) or
 * "none"; `updateKind`, for the kit, "link" (seconds) or "build" (minutes). A browser has none
 * installed and pretends a build is available.
 */
export const getRuntimeStatus = async () =>
  isNativeApp
    ? tauri.core.invoke("get_runtime_status")
    : { installed: false, buildFound: true, updateAvailable: true, path: "" };

/**
 * Installs or updates the runtime: copies the project's build, or builds it from the player kit
 * and the player's default.xex (`source` "kit" in the state), which takes minutes and reports its
 * progress through onRuntimeBuildProgress. Resolves to the new state; rejects with { kind, detail }
 * (see BuildError in src-tauri/src/builder.rs).
 */
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

/**
 * Prepares the shaders of earlier sessions: the runtime makes them with the game suspended and
 * closes. Rejects like launchGame. Its progress arrives through onGameOutput as lines
 * "AZURA_SHADERS <done> <total>" and "AZURA_SHADERS_DONE <total>", and its end through onGameExit.
 */
export const prepareShaders = async () => {
  if (isNativeApp) return tauri.core.invoke("prepare_shaders");
  const total = 1200;
  [0, 300, 700, 1200].forEach((done, index) => {
    setTimeout(
      () => simulated.output.forEach((listener) => listener({ stream: "stdout", line: `AZURA_SHADERS ${done} ${total}` })),
      300 * (index + 1),
    );
  });
  setTimeout(() => simulated.output.forEach((listener) => listener({ stream: "stdout", line: `AZURA_SHADERS_DONE ${total}` })), 1500);
  setTimeout(() => simulated.exit.forEach((listener) => listener({ code: 0, success: true })), 1700);
};

/** Bytes of the stored pipeline descriptions, to tell new ones from those already prepared. */
export const shaderStorageBytes = async () => {
  if (isNativeApp) return tauri.core.invoke("shader_storage_bytes");
  return 4096;
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

/**
 * The state of the game's saves (see SaveStatus in saves.rs): `status` is "ok", "corrupted" or
 * "empty", dates are milliseconds since 1970 and backups come newest first. The interface words it.
 */
export const getSaveStatus = async () => {
  if (isNativeApp) return tauri.core.invoke("get_save_status");
  const now = Date.now();
  return {
    hasSaves: true,
    checkpointCount: 3,
    isCorrupted: false,
    status: "ok",
    savePath: "~/.local/share/azura/B13EBABEBABEBABE/4D5308AB",
    lastSavedMs: now - 3_600_000,
    backups: [
      { id: "backup_preview_manual", createdMs: now - 86_400_000, kind: "manual", sizeBytes: 2_621_440 },
      { id: "backup_preview_auto_before_repair", createdMs: now - 172_800_000, kind: "beforeRepair", sizeBytes: 1_048_576 },
    ],
  };
};

/**
 * Removes broken checkpoint headers (backing the saves up first when there are any) and resolves
 * to how many were removed. Errors reject with { kind, detail }, as SavesError in saves.rs.
 */
export const repairSaves = async () => (isNativeApp ? tauri.core.invoke("repair_saves") : 0);

/** Backs the saves up and clears them for a new campaign. Rejects with { kind, detail }. */
export const resetSaves = async () => {
  if (isNativeApp) return tauri.core.invoke("reset_saves");
};

/** Backs the current saves up and resolves to the new backup. Rejects with { kind, detail }. */
export const createSaveBackup = async () => {
  if (isNativeApp) return tauri.core.invoke("create_save_backup");
  return { id: "backup_preview_manual", createdMs: Date.now(), kind: "manual", sizeBytes: 2_621_440 };
};

/** Replaces the current saves with the backup `backupId`. Rejects with { kind, detail }. */
export const restoreSaveBackup = async (backupId) => {
  if (isNativeApp) return tauri.core.invoke("restore_save_backup", { backupId });
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

/**
 * Whether there is a working C++ compiler to build the runtime with (see src-tauri/src/toolchain.rs):
 *   { status: "ready", version }
 *   { status: "missing", found, problem, install: { kind, manager?, command? }, manual, installerRunning }
 * `install.kind` is "appleCommandLineTools", "packageManager" or "manual". The browser simulation
 * reports Apple's compiler as installed.
 */
export const getCompilerStatus = async () =>
  isNativeApp ? tauri.core.invoke("get_compiler_status") : { status: "ready", version: "Apple clang 21.0.0" };

/**
 * Installs the compiler: on macOS it opens Apple's dialog and resolves at once (check the status
 * again until it is ready); on Linux it resolves once the package manager is done. Rejects with a
 * message when it cannot install it.
 */
export const installCompiler = async () => (isNativeApp ? tauri.core.invoke("install_compiler") : undefined);

/**
 * Calls `listener` with { phase, done, total } while the runtime is built from the kit: phase is
 * "preparing", "translating", "compiling" (done of total files), "linking" or "installing".
 */
/**
 * Calls `listener({ action, pressed, family })` for every controller action the app reads
 * (src-tauri/src/gamepad.rs). Only in the app.
 */
export const onGamepadInput = (listener) => {
  if (isNativeApp) return tauri.event.listen("gamepad-input", (event) => listener(event.payload));
};

export const onRuntimeBuildProgress = (listener) => {
  if (isNativeApp) return tauri.event.listen("runtime-build-progress", (event) => listener(event.payload));
};

/** Shows a build log (the path a failed build reported) in the system's file manager. */
export const showBuildLog = async (path) => (isNativeApp ? tauri.core.invoke("show_build_log", { path }) : undefined);
