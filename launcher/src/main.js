/*
 * Azura launcher: interface behaviour.
 *
 * Everything that needs the operating system goes through platform.js, which calls the Rust side
 * inside the app and simulates it in a plain browser.
 *
 * Starting the game installs the runtime first when it is missing or a newer build exists, then
 * runs it through platform.js and shows its output in the log panel.
 *
 * Settings are loaded once on start-up, saved shortly after every change, and reloaded when the
 * window regains focus, because the in-game settings menu (F4) writes to the same azura.toml.
 *
 * Text that depends on state (status lines, the launch button, formatted numbers and dates) is
 * written by the render functions below, which run again after every language change.
 */

import { getLocale, getPreference, initI18n, onLocaleChange, setPreference, systemLocale, t } from "./i18n.js";
import { SUPPORTED_LOCALES } from "./locale.js";
import {
  matchPreset,
  PRESET_ORDER,
  presetValues,
  renderSize,
  resolveAutoPreset,
  setDetectedSystem,
  SETTING_KEYS,
} from "./graphics.js";
import {
  checkForUpdate,
  checkGameFolder,
  getAppVersion,
  getRuntimeStatus,
  getSystemInfo,
  installRuntime,
  installUpdate,
  isNativeApp,
  launchGame,
  loadSettings,
  onGameExit,
  onGameOutput,
  onUpdateProgress,
  changeSavesFolder,
  pickGameFolder,
  pickSavesFolder,
  saveSettings,
} from "./platform.js";
import { defaultGameLanguage, gameLanguageName } from "./gameLanguages.js";

const SPORE_COUNT = 26;
/** Launcher version shown in a browser; the app reads its real version (see detectSystem). */
const BROWSER_LAUNCHER_VERSION = "0.1.0";
const RUNTIME_VERSION = "0.10.0";

/** Delay before saving after a change, so a burst of edits writes the files once. */
const SAVE_DELAY_MS = 300;

/** Lines kept in the game log; older ones are dropped so a chatty game cannot grow it forever. */
const MAX_LOG_LINES = 2000;

/** Interface state that outlives a language change. */
const state = {
  // Result of the last folder check (see platform.js), or null before any folder is chosen.
  gameCheck: null,
  // Last game folder that passed the check; the one saved in the settings.
  savedGameFolder: null,
  // Engine code of the game language in the Installation view ("INT", "FRA", ...), and whether the
  // user picked it. Only a user's pick survives a change of game folder; an automatic one is
  // recomputed, so a folder that has the launcher's language gets it preselected.
  gameLanguage: null,
  gameLanguageChosen: false,
  // State of the runtime ({ installed, buildFound, updateAvailable }, see platform.getRuntimeStatus)
  // and whether it is being installed.
  runtime: { installed: false, buildFound: false, updateAvailable: false },
  installingRuntime: false,
  // Folder for the saves (shown in the Installation view; changed only by moving the saves) and
  // the folder waiting for the player to confirm replacing the saves already in it.
  savesFolder: "",
  savesConflict: null,
  // "idle", "starting" (asked to start) or "running" (the process is up).
  game: "idle",
  // Message shown in the log header: a translation key, its values and "ok" / "error" / "".
  logState: null,
  // Newer launcher version ({ version, ... }) when one is published, and the install progress:
  // null (not installing) or a percentage, -1 while the size is unknown.
  update: null,
  updateProgress: null,
  // This Mac ({ chip, memoryGb, arch }), or null when unknown (always in a browser).
  system: null,
  launcherVersion: BROWSER_LAUNCHER_VERSION,
  userName: "",
  fullscreen: false,
  preset: "auto",
  graphics: presetValues("auto"),
};

/** Builds the settings object platform.saveSettings stores (see src-tauri/src/settings.rs). */
const currentSettings = () => ({
  gameFolder: state.savedGameFolder,
  userName: state.userName || null,
  fullscreen: state.fullscreen,
  graphics: { ...state.graphics },
  graphicsPreset: state.preset,
  uiLanguage: getPreference(),
  gameLanguage: state.gameLanguage,
  gameLanguageChosen: state.gameLanguageChosen,
  savesFolder: state.savesFolder,
});

/** Pending save timer; null when everything is saved. */
let saveTimer = null;

/** Saves the settings SAVE_DELAY_MS after the last call. */
const persist = () => {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(async () => {
    saveTimer = null;
    try {
      await saveSettings(currentSettings());
    } catch (error) {
      console.error("Saving settings failed:", error);
      toast(t("toast.saveFailed", { detail: String(error) }));
    }
  }, SAVE_DELAY_MS);
};

/** Shows a short message in the bottom-right corner. */
const toast = (() => {
  const el = document.getElementById("toast");
  let timer;
  return (message) => {
    el.textContent = message;
    el.classList.add("is-visible");
    clearTimeout(timer);
    timer = setTimeout(() => el.classList.remove("is-visible"), 2600);
  };
})();

/** Fills the backdrop with glowing spores, each with a random size, speed and path. */
const createSpores = () => {
  const container = document.querySelector(".spores");
  const random = (min, max) => min + Math.random() * (max - min);
  for (let i = 0; i < SPORE_COUNT; i++) {
    const spore = document.createElement("span");
    spore.className = "spore";
    spore.style.left = `${random(0, 100)}%`;
    spore.style.setProperty("--size", `${random(2, 4.5)}px`);
    spore.style.setProperty("--dur", `${random(12, 26)}s`);
    // A negative delay starts each spore mid-flight, so the screen is not empty on load.
    spore.style.setProperty("--delay", `${-random(0, 26)}s`);
    spore.style.setProperty("--dx", `${random(-80, 80)}px`);
    container.appendChild(spore);
  }
};

/** Shows the section called `name` and marks its navigation item as active. */
const showView = (name) => {
  document.querySelectorAll(".nav-item").forEach((item) => item.classList.toggle("is-active", item.dataset.view === name));
  document.querySelectorAll(".view").forEach((view) => view.classList.toggle("is-active", view.dataset.view === name));
};

const setupNavigation = () => {
  document.querySelectorAll(".nav-item").forEach((item) => {
    item.addEventListener("click", () => showView(item.dataset.view));
  });
};

/** True once the chosen folder holds a valid game executable. */
const isInstalled = () => state.gameCheck?.status === "ok";

/** True when Play has to install the runtime (or a newer build of it) before starting. */
const runtimeNeedsInstall = () => state.runtime.buildFound && (!state.runtime.installed || state.runtime.updateAvailable);

/** Translation key describing the runtime, and its status tone ("ok", "warn" or "error"). */
const runtimeState = () => {
  const { installed, buildFound, updateAvailable } = state.runtime;
  if (installed && !updateAvailable) return ["install.runtimeInstalled", "ok"];
  if (installed) return ["install.runtimeUpdate", "warn"];
  if (buildFound) return ["install.runtimeNotInstalled", "warn"];
  return ["install.runtimeNoBuild", "error"];
};

/** Fetches the runtime state; a failure keeps the previous one. */
const refreshRuntime = async () => {
  try {
    state.runtime = await getRuntimeStatus();
  } catch (error) {
    console.error("Checking the runtime failed:", error);
  }
};

/**
 * Writes the state-dependent parts of the Play view. Without game files the main button becomes
 * "Install", so it never offers to start a game that cannot run.
 */
const renderPlay = () => {
  const status = document.getElementById("status-game");
  status.dataset.state = isInstalled() ? "ok" : "warn";
  let detail = "status.noFolder";
  if (isInstalled()) detail = "status.xexFound";
  else if (state.gameCheck) detail = `install.${state.gameCheck.status}`;
  status.querySelector(".status-detail").textContent = t(detail, state.gameCheck ?? {});

  const runtime = document.getElementById("status-runtime");
  const [, tone] = runtimeState();
  runtime.dataset.state = tone === "ok" ? "ok" : "warn";
  let runtimeDetail = `ReXGlue ${RUNTIME_VERSION} · mac-arm64`;
  if (!state.runtime.buildFound && !state.runtime.installed) runtimeDetail = t("status.runtimeMissing");
  else if (!state.runtime.installed) runtimeDetail = t("status.runtimeNotInstalled");
  else if (state.runtime.updateAvailable) runtimeDetail = t("status.runtimeUpdate");
  runtime.querySelector(".status-detail").textContent = runtimeDetail;

  const player = document.getElementById("status-player");
  if (player) {
    const name = state.userName.trim() || t("install.defaultUserName");
    document.getElementById("status-player-detail").textContent = name;
  }

  const button = document.getElementById("launch");
  button.classList.toggle("is-install", !isInstalled());
  button.disabled = state.game !== "idle" || state.installingRuntime;
  let label = "play.install";
  let sub = "play.installSub";
  if (isInstalled()) {
    label = { idle: "play.launch", starting: "play.launching", running: "play.running" }[state.game];
    sub = state.game === "idle" && runtimeNeedsInstall() ? "play.runtimeWillInstall" : "play.mode";
  }
  button.querySelector(".btn-launch-label").textContent = t(label);
  button.querySelector(".btn-launch-sub").textContent = t(sub);
  renderLogState();

  // "Apple M4 · 24 GB"; a browser cannot tell, so it names the platform only.
  const { chip, memoryGb } = state.system ?? {};
  const machine = [chip ?? "Apple Silicon", memoryGb ? `${memoryGb} GB` : null].filter(Boolean).join(" · ");
  document.getElementById("status-gpu-detail").textContent = t("status.gpuValue", { machine });

  const date = document.querySelector(".news-item time");
  date.textContent = new Intl.DateTimeFormat(getLocale(), { dateStyle: "medium" }).format(
    new Date(`${date.dateTime}T12:00:00`),
  );

  document.getElementById("launcher-version").textContent = t("rail.launcherVersion", { version: state.launcherVersion });
  document.getElementById("runtime-version").textContent = t("rail.runtimeVersion", { version: RUNTIME_VERSION });
};

/** Writes the log header message (kept as a key, so it follows language changes). */
const renderLogState = () => {
  const el = document.getElementById("game-log-state");
  const log = state.logState;
  el.textContent = log ? t(log.key, log.vars) : "";
  el.dataset.state = log?.tone ?? "";
};

/** Sets the log header message and shows the panel. */
const setLogState = (key, tone = "", vars = {}) => {
  state.logState = { key, tone, vars };
  document.getElementById("game-log").hidden = false;
  renderLogState();
};

/** Appends one line of game output, dropping the oldest beyond MAX_LOG_LINES. */
const appendLogLine = ({ stream, line }) => {
  const body = document.getElementById("game-log-body");
  // Follow the end only if the user has not scrolled up to read something.
  const atEnd = body.scrollTop + body.clientHeight >= body.scrollHeight - 4;
  const row = document.createElement("span");
  row.className = stream;
  row.textContent = `${line}\n`;
  body.appendChild(row);
  while (body.childElementCount > MAX_LOG_LINES) body.firstElementChild.remove();
  if (atEnd) body.scrollTop = body.scrollHeight;
};

/** Translation key and values for a launch error from platform.launchGame. */
const launchErrorMessage = (error) => {
  switch (error?.kind) {
    case "runtimeMissing":
      return ["log.runtimeMissing", {}];
    case "alreadyRunning":
      return ["log.alreadyRunning", {}];
    case "savesUnavailable":
      return ["log.savesUnavailable", { detail: error?.detail ?? "" }];
    default:
      return ["log.startFailed", { detail: error?.detail ?? String(error) }];
  }
};

/**
 * The main button either opens the Installation view (no game files yet) or starts the game. The
 * game's output fills the log panel; its end re-enables the button.
 */
const setupLaunch = () => {
  document.getElementById("launch").addEventListener("click", async () => {
    if (!isInstalled()) {
      showView("install");
      document.getElementById("pick-game").focus();
      return;
    }
    document.getElementById("game-log-body").replaceChildren();
    state.game = "starting";
    renderPlay();
    if (runtimeNeedsInstall() && !(await runRuntimeInstall())) {
      state.game = "idle";
      renderPlay();
      return;
    }
    setLogState("log.starting");
    renderPlay();
    try {
      await launchGame();
      state.game = "running";
      setLogState("log.running");
    } catch (error) {
      state.game = "idle";
      const [key, vars] = launchErrorMessage(error);
      setLogState(key, "error", vars);
    }
    renderPlay();
  });

  onGameOutput(appendLogLine);
  onGameExit(({ code, success }) => {
    state.game = "idle";
    if (success) setLogState("log.exitOk", "ok");
    else if (code == null) setLogState("log.exitSignal", "error");
    else setLogState("log.exitError", "error", { code });
    renderPlay();
  });

  document.getElementById("game-log-hide").addEventListener("click", () => {
    document.getElementById("game-log").hidden = true;
  });
};

/**
 * Installs or updates the runtime, reporting progress in the log panel. Resolves to whether it
 * succeeded.
 */
const runRuntimeInstall = async () => {
  state.installingRuntime = true;
  setLogState("log.installingRuntime");
  renderInstall();
  renderPlay();
  try {
    state.runtime = await installRuntime();
    toast(t("toast.runtimeInstalled"));
    return true;
  } catch (error) {
    setLogState("log.installFailed", "error", { detail: String(error) });
    return false;
  } finally {
    state.installingRuntime = false;
    renderInstall();
    renderPlay();
  }
};

/** Writes the folder path and the result of its check into the Installation view. */
const renderInstall = () => {
  const check = state.gameCheck;
  const pathField = document.getElementById("game-path");
  pathField.value = check?.folder ?? "";
  // Long paths are cut off in the field; the tooltip shows them whole.
  pathField.title = pathField.value;

  const playerField = document.getElementById("player-name");
  if (playerField && playerField.value !== state.userName) {
    playerField.value = state.userName;
  }

  const [runtimeKey, runtimeTone] = runtimeState();
  const runtimeLine = document.getElementById("runtime-check");
  runtimeLine.dataset.state = runtimeTone;
  runtimeLine.textContent = t(runtimeKey);
  const runtimeButton = document.getElementById("install-runtime");
  runtimeButton.hidden = !state.runtime.buildFound;
  runtimeButton.disabled = state.installingRuntime || state.game !== "idle" || !runtimeNeedsInstall();
  runtimeButton.textContent = t(
    state.installingRuntime ? "install.installingRuntime" : state.runtime.installed ? "install.updateRuntime" : "install.installRuntime",
  );

  const savesField = document.getElementById("saves-path");
  savesField.value = state.savesFolder;
  savesField.title = state.savesFolder;
  document.getElementById("saves-conflict").hidden = !state.savesConflict;
  if (state.savesConflict) {
    document.getElementById("saves-conflict-text").textContent = t("install.savesConflict", { folder: state.savesConflict });
  }

  const line = document.getElementById("game-check");
  line.hidden = !check;
  if (!check) return;
  line.dataset.state = check.status === "ok" ? "ok" : "error";
  // Every check field (titleId, mediaId, versionName, ...) is available to the message.
  line.textContent = t(`install.${check.status}`, check);
};

/**
 * Fills the game language selector with the languages found in the game folder. Without a folder
 * (or with one that has no language files) it shows a single disabled hint instead.
 */
const renderGameLanguages = () => {
  const select = document.getElementById("game-language");
  const languages = state.gameCheck?.languages ?? [];
  select.replaceChildren();
  select.disabled = languages.length === 0;
  if (select.disabled) {
    select.add(new Option(t("install.chooseFolderFirst"), ""));
    return;
  }
  if (!state.gameLanguageChosen || !languages.includes(state.gameLanguage)) {
    state.gameLanguage = defaultGameLanguage(languages, getLocale());
    state.gameLanguageChosen = false;
  }
  for (const code of languages) {
    select.add(new Option(gameLanguageName(code), code));
  }
  select.value = state.gameLanguage;
};

/** Stores a folder check, saves the folder when it is valid and refreshes the views. */
const applyGameCheck = (check) => {
  state.gameCheck = check;
  if (check.status === "ok" && check.folder !== state.savedGameFolder) {
    state.savedGameFolder = check.folder;
    persist();
  }
  renderInstall();
  renderGameLanguages();
  renderPlay();
};

/**
 * Wires the folder picker. A failed check keeps the previous valid folder in the settings, so a
 * wrong pick never makes the launcher forget a working installation on its next start.
 */
const setupInstall = () => {
  const button = document.getElementById("pick-game");
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      const check = await pickGameFolder();
      if (!check) return; // The user cancelled the picker.
      applyGameCheck(check);
      if (!isNativeApp) toast(t("toast.pick"));
    } catch (error) {
      console.error("Folder picker failed:", error);
    } finally {
      button.disabled = false;
    }
  });

  document.getElementById("install-runtime").addEventListener("click", () => runRuntimeInstall());

  // Moving the saves: a folder that already holds saves needs a second, explicit confirmation.
  const moveSaves = async (folder, replace) => {
    try {
      state.savesFolder = await changeSavesFolder(folder, replace);
      state.savesConflict = null;
      toast(t("toast.savesMoved"));
    } catch (error) {
      state.savesConflict = null;
      if (error?.kind === "destinationHasSaves") state.savesConflict = folder;
      else if (error?.kind === "nestedFolders") toast(t("toast.savesNested"));
      else toast(t("toast.savesFailed", { detail: error?.detail ?? String(error) }));
    }
    renderInstall();
  };
  document.getElementById("pick-saves").addEventListener("click", async () => {
    const folder = await pickSavesFolder();
    if (folder && folder !== state.savesFolder) await moveSaves(folder, false);
  });
  document.getElementById("saves-replace").addEventListener("click", () => moveSaves(state.savesConflict, true));
  document.getElementById("saves-cancel").addEventListener("click", () => {
    state.savesConflict = null;
    renderInstall();
  });

  const playerField = document.getElementById("player-name");
  if (playerField) {
    playerField.addEventListener("input", () => {
      if (playerField.value.length > 15) {
        playerField.value = playerField.value.slice(0, 15);
      }
      state.userName = playerField.value;
      renderPlay();
      persist();
    });
  }
};

/** Checks the folder saved in an earlier session, if any, and shows the result. */
const restoreGameFolder = async (folder) => {
  if (!folder) return;
  state.savedGameFolder = folder;
  try {
    applyGameCheck(await checkGameFolder(folder));
  } catch (error) {
    console.error("Folder check failed:", error);
  }
};

/**
 * Takes the graphics settings as stored. A value missing from the file comes from the saved
 * preset; the preset shown is then recomputed, so values changed in game (F4) show as "Custom".
 */
const applyStoredGraphics = (settings) => {
  const saved = PRESET_ORDER.includes(settings.graphicsPreset) ? settings.graphicsPreset : "auto";
  const stored = Object.fromEntries(
    Object.entries(settings.graphics ?? {}).filter(([key, value]) => SETTING_KEYS.includes(key) && value != null),
  );
  state.graphics = { ...presetValues(saved), ...stored };
  state.preset = matchPreset(state.graphics, saved);
  state.fullscreen = Boolean(settings.fullscreen);
};

/** Reads a control's value with the type of the setting it edits (numbers stay numbers). */
const readSetting = (control) => {
  const value = control.value;
  return typeof state.graphics[control.dataset.setting] === "number" ? Number(value) : value;
};

/** Writes the current graphics state into the preset selector and every setting control. */
const renderGraphics = () => {
  document.querySelectorAll("#preset [data-preset]").forEach((button) => {
    button.setAttribute("aria-checked", String(button.dataset.preset === state.preset));
  });
  document.querySelector('#preset [data-preset="custom"]').hidden = state.preset !== "custom";

  const note = document.getElementById("preset-note");
  note.textContent =
    state.preset === "auto" ? t("graphics.autoResolved", { preset: t(`graphics.${resolveAutoPreset()}`) }) : "";

  for (const key of SETTING_KEYS) {
    const select = document.querySelector(`[data-setting="${key}"]`);
    const value = String(state.graphics[key]);
    // A value set outside the launcher (in game, or by hand) may not be one of the options; it
    // is added as it is rather than shown as an empty field.
    if (![...select.options].some((option) => option.value === value)) {
      select.add(new Option(value, value));
    }
    select.value = value;
  }
  document.getElementById("display-mode").value = state.fullscreen ? "fullscreen" : "windowed";

  // Render resolution options show the real size each scale produces ("2560 × 1440 (2×)").
  document.querySelectorAll("#resolution-scale option").forEach((option) => {
    const scale = Number(option.value);
    const { width, height } = renderSize(scale);
    const size = `${width} × ${height}`;
    option.textContent = scale === 1 ? `${size} · ${t("graphics.original")}` : `${size} (${scale}×)`;
  });
};

/**
 * Wires the preset selector and the setting controls. Choosing a preset copies its values into
 * every control; editing a control re-checks which preset (if any) the values still match, so a
 * hand-tuned combination shows as "Custom".
 */
const setupGraphics = () => {
  document.querySelectorAll("#preset [data-preset]").forEach((button) => {
    button.addEventListener("click", () => {
      state.preset = button.dataset.preset;
      state.graphics = presetValues(state.preset);
      renderGraphics();
      persist();
    });
  });

  document.querySelectorAll("[data-setting]").forEach((control) => {
    control.addEventListener("change", () => {
      state.graphics = { ...state.graphics, [control.dataset.setting]: readSetting(control) };
      state.preset = matchPreset(state.graphics, state.preset);
      renderGraphics();
      persist();
    });
  });

  const displayMode = document.getElementById("display-mode");
  displayMode.addEventListener("change", () => {
    state.fullscreen = displayMode.value === "fullscreen";
    persist();
  });
};

/** Adds one <option> per supported language to a <select>, labelled with its own name. */
const fillLanguageOptions = (select) => {
  for (const { code, name } of SUPPORTED_LOCALES) {
    select.add(new Option(name, code));
  }
};

/**
 * Builds both language selectors. It runs before the translations load, because the language
 * change listener renders into them. The launcher selector starts with "System (...)", which
 * follows the system language.
 */
const setupLanguages = () => {
  const uiSelect = document.getElementById("ui-language");
  uiSelect.add(new Option("", "auto"));
  fillLanguageOptions(uiSelect);
  uiSelect.addEventListener("change", async () => {
    await setPreference(uiSelect.value);
    persist();
  });

  const gameSelect = document.getElementById("game-language");
  gameSelect.addEventListener("change", () => {
    state.gameLanguage = gameSelect.value;
    state.gameLanguageChosen = true;
    persist();
  });
};

/** Updates the "System (...)" option so it names the language the system resolves to. */
const renderLanguageOptions = () => {
  const systemName = SUPPORTED_LOCALES.find(({ code }) => code === systemLocale())?.name ?? "";
  document.querySelector('#ui-language option[value="auto"]').textContent = t("rail.systemLanguage", {
    name: systemName,
  });
};

/** Writes the update button: hidden, "Update to X", or the download progress. */
const renderUpdate = () => {
  const button = document.getElementById("update-button");
  button.hidden = !state.update;
  if (!state.update) return;
  button.disabled = state.updateProgress !== null;
  if (state.updateProgress === null) {
    button.textContent = t("update.available", { version: state.update.version });
  } else if (state.updateProgress < 0) {
    button.textContent = t("update.downloading", { percent: "…" });
  } else {
    const percent = new Intl.NumberFormat(getLocale(), { style: "percent" }).format(state.updateProgress / 100);
    button.textContent = t("update.downloading", { percent });
  }
};

/**
 * Checks for a newer launcher in the background and offers it in the sidebar. An unreachable
 * endpoint is expected while the repository is private (and when offline), so it is only logged.
 * Installing restarts the launcher; only a failure comes back here.
 */
const setupUpdates = () => {
  onUpdateProgress(({ downloaded, total }) => {
    state.updateProgress = total ? Math.min(100, Math.round((downloaded / total) * 100)) : -1;
    renderUpdate();
  });

  document.getElementById("update-button").addEventListener("click", async () => {
    state.updateProgress = -1;
    renderUpdate();
    try {
      await installUpdate();
    } catch (error) {
      console.error("Installing the update failed:", error);
      state.updateProgress = null;
      renderUpdate();
      toast(t("update.failed"));
    }
  });

  checkForUpdate()
    .then((update) => {
      state.update = update;
      renderUpdate();
    })
    .catch((error) => console.info("No update information:", error));
};

const renderAll = () => {
  renderPlay();
  renderInstall();
  renderGameLanguages();
  renderGraphics();
  renderLanguageOptions();
  renderUpdate();
};

/**
 * Refreshes the runtime status and reloads the settings when the window regains focus, to show
 * what the in-game menu (F4) changed. Settings are not reloaded while a save is pending, so an
 * edit that is about to be written is never replaced.
 */
const setupReloadOnFocus = () => {
  window.addEventListener("focus", async () => {
    try {
      // The runtime may have been rebuilt or removed while the window was in the background.
      await refreshRuntime();
      renderInstall();
      renderPlay();
      if (saveTimer) return;
      const settings = await loadSettings();
      if (!settings) return;
      applyStoredGraphics(settings);
      state.userName = (settings.userName ?? "").slice(0, 15);
      renderGraphics();
      renderInstall();
      renderPlay();
    } catch (error) {
      console.error("Reloading settings failed:", error);
    }
  });
};

/** Loads the saved settings; a failure (for example a hand-edited, invalid file) starts empty. */
const loadSavedSettings = async () => {
  try {
    return (await loadSettings()) ?? {};
  } catch (error) {
    console.error("Loading settings failed:", error);
    return {};
  }
};

createSpores();
setupNavigation();
setupLaunch();
setupInstall();
setupGraphics();
setupLanguages();
setupReloadOnFocus();

/**
 * Detects this Mac and the launcher version; a failure leaves them unknown, and "Auto" then
 * resolves to medium.
 */
const detectSystem = async () => {
  try {
    state.system = await getSystemInfo();
    state.launcherVersion = await getAppVersion(BROWSER_LAUNCHER_VERSION);
  } catch (error) {
    console.error("Detecting the system failed:", error);
  }
  setDetectedSystem(state.system);
};

// The machine is detected before the settings load, because "Auto" resolves from it.
await detectSystem();
await refreshRuntime();
const settings = await loadSavedSettings();
state.savesFolder = settings.savesFolder ?? "";
applyStoredGraphics(settings);
state.userName = (settings.userName ?? "").slice(0, 15);
state.gameLanguage = settings.gameLanguage ?? null;
state.gameLanguageChosen = Boolean(settings.gameLanguageChosen);
onLocaleChange(renderAll);
await initI18n(settings.uiLanguage);
document.getElementById("ui-language").value = getPreference();
await restoreGameFolder(settings.gameFolder);
setupUpdates();
