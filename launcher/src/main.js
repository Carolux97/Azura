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
  RESOLUTION_SCALES,
  choosableGpus,
  gpuChoiceVisible,
  gpuLabel,
  renderSize,
  resolutionValue,
  resolveAutoScale,
  scaleOfResolution,
  selectedGpu,
  setDetectedSystem,
} from "./graphics.js";
import {
  checkForUpdate,
  checkGameFolder,
  getAppVersion,
  getRuntimeStatus,
  getSystemInfo,
  getReleaseNotes,
  installRuntime,
  installUpdate,
  isNativeApp,
  launchGame,
  listGpus,
  loadSettings,
  onGameExit,
  onGameOutput,
  onGameRestart,
  onGpuMissing,
  onUpdateProgress,
  changeSavesFolder,
  createSaveBackup,
  getAchievements,
  getCompilerStatus,
  installCompiler,
  onRuntimeBuildProgress,
  onGamepadInput,
  showBuildLog,
  getSaveStatus,
  openSavesFolder,
  pickGameFolder,
  pickGameImage,
  onPathDropped,
  pickSavesFolder,
  repairSaves,
  resetSaves,
  restoreSaveBackup,
  saveSettings,
} from "./platform.js";
import { defaultGameLanguage, gameLanguageName } from "./gameLanguages.js";
import { setupGamepad } from "./gamepadNav.js";
import { TEXT_RULES } from "./textRules.js";
import { achievementView } from "./achievements.js";
import { createBuildTracker } from "./buildProgress.js";
import { LOCK_ICON, completedBanner, renderAchievementsPage } from "./achievementsPage.js";

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
  // The kit build's last progress ({ phase, done, total }), or null when no build runs.
  buildProgress: null,
  // The runtime build from the kit the Play view's card shows: its tracker (buildProgress.js) and,
  // once it failed, why ({ key, vars, log }); null when there is none to show.
  build: null,
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
  fullscreen: true,
  // Render resolution (render_resolution, also in the game's Video menu): "auto" (follows this Mac)
  // or the render size the user chose ("2560x1440").
  renderResolution: "auto",
  // State of the saved games (see saves.rs), or null until it is read.
  saveStatus: null,
  // The save action waiting for the player's confirmation: { action: "reset" } or
  // { action: "restore", backup }, or null.
  savesPending: null,
  // What the Achievements section shows (see platform.getAchievements), { status: "loading" }
  // while it is first read, or null before that. The view and the filter the player chose; the view
  // is saved with the settings.
  achievements: null,
  achievementsView: "list",
  achievementsFilter: "all",
  // What this launcher version brings ({ version, items }, see platform.getReleaseNotes), and the
  // version whose notes the player dismissed (saved with the settings).
  releaseNotes: null,
  seenReleaseNotes: null,
  // Whether the player opened the dismissed notes again from the rail (not saved).
  releaseNotesReopened: false,
  // The C++ compiler the runtime is built with (see platform.getCompilerStatus), or null while it is
  // first checked; and what the launcher is doing about it: "" (nothing), "checking", "installing"
  // (the package manager runs) or "waitingForApple" (Apple's install window is open).
  compiler: null,
  compilerAction: "",
  // Graphics card chosen by the player (its UUID), or null for the game's own choice.
  gpu: null,
  // The graphics cards the runtime lists (see src-tauri/src/gpus.rs), once asked for.
  gpus: { devices: [], automatic: null },
};

/** Builds the settings object platform.saveSettings stores (see src-tauri/src/settings.rs). */
const currentSettings = () => ({
  gameFolder: state.savedGameFolder,
  userName: state.userName.trimEnd() || null,
  fullscreen: state.fullscreen,
  graphics: { renderResolution: state.renderResolution },
  uiLanguage: getPreference(),
  gameLanguage: state.gameLanguage,
  gameLanguageChosen: state.gameLanguageChosen,
  achievementsView: state.achievementsView,
  seenReleaseNotes: state.seenReleaseNotes,
  gpu: state.gpu,
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
  if (name === "saves") {
    refreshAndRenderSaves();
  }
  if (name === "achievements") {
    refreshAchievements();
  }
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
  const { installed, buildFound, updateAvailable, source, updateKind } = state.runtime;
  if (installed && !updateAvailable) return ["install.runtimeInstalled", "ok"];
  // Built here from the kit: say what an update takes, seconds or minutes.
  if (source === "kit" && !installed) return ["install.runtimeNotBuilt", "warn"];
  if (source === "kit" && updateKind === "link") return ["install.runtimeUpdateLink", "warn"];
  if (source === "kit") return ["install.runtimeUpdateBuild", "warn"];
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
/**
 * The "What's new" panel: this version's notes until the player dismisses them, and again when
 * they reopen them from the rail's link (shown while the panel is closed). The notes are in English
 * (launcher/CHANGELOG.md); the title around them is translated.
 */
const renderReleaseNotes = () => {
  const panel = document.getElementById("release-notes");
  const notes = state.releaseNotes;
  const dismissed = Boolean(notes) && notes.version === state.seenReleaseNotes;
  panel.hidden = !notes || (dismissed && !state.releaseNotesReopened);
  document.getElementById("release-notes-show").hidden = !notes || !panel.hidden;
  if (panel.hidden) return;
  document.getElementById("release-notes-title").textContent = t("releaseNotes.title", { version: notes.version });
  document.getElementById("release-notes-list").replaceChildren(
    ...notes.items.map((item) => {
      const line = document.createElement("li");
      line.textContent = item;
      return line;
    }),
  );
};

const setupReleaseNotes = async () => {
  document.getElementById("release-notes-dismiss").addEventListener("click", () => {
    state.seenReleaseNotes = state.releaseNotes?.version ?? null;
    state.releaseNotesReopened = false;
    persist();
    renderReleaseNotes();
  });
  document.getElementById("release-notes-show").addEventListener("click", () => {
    state.releaseNotesReopened = true;
    showView("play");
    renderReleaseNotes();
    document.getElementById("release-notes").scrollIntoView({ block: "nearest" });
  });
  try {
    state.releaseNotes = await getReleaseNotes();
  } catch (error) {
    console.error("Release notes could not be read:", error);
  }
  renderReleaseNotes();
};

/**
 * The decorative animations run only while someone can see them: any of them makes WebKit redraw
 * the window every frame, about a whole CPU core, which the game needs (on a Steam Deck above all).
 * They pause while the game starts or runs and while the window is in the background or hidden,
 * and carry on from where they were when it comes back.
 */
let windowFocused = document.hasFocus();
const syncMotion = () => {
  const paused = state.game !== "idle" || !windowFocused || document.hidden;
  document.documentElement.classList.toggle("motion-paused", paused);
};
window.addEventListener("focus", () => {
  windowFocused = true;
  syncMotion();
});
window.addEventListener("blur", () => {
  windowFocused = false;
  syncMotion();
});
document.addEventListener("visibilitychange", syncMotion);

const renderPlay = () => {
  syncMotion();
  const status = document.getElementById("status-game");
  status.dataset.state = isInstalled() ? "ok" : "warn";
  let detail = "status.noFolder";
  if (isInstalled()) detail = "status.xexFound";
  else if (state.gameCheck) detail = `install.${state.gameCheck.status}`;
  status.querySelector(".status-detail").textContent = t(detail, state.gameCheck ?? {});

  const compilerLine = document.getElementById("status-compiler");
  compilerLine.dataset.state = state.compiler?.status === "ready" ? "ok" : "warn";
  compilerLine.querySelector(".status-detail").textContent =
    state.compiler?.status === "ready" ? state.compiler.version : t(state.compiler ? "status.compilerMissing" : "status.compilerChecking");

  const runtime = document.getElementById("status-runtime");
  const [, tone] = runtimeState();
  const building = state.installingRuntime && state.runtime.source === "kit";
  runtime.dataset.state = building ? "progress" : tone === "ok" ? "ok" : "warn";
  const system = state.system;
  // The heading names the system the launcher runs on (macOS until it is known, as in a browser).
  const linux = system?.os === "linux";
  document.getElementById("play-eyebrow").textContent = t("play.eyebrow", { platform: linux ? "Linux" : "macOS" });
  document.getElementById("play-lede").textContent = t("play.lede", { target: linux ? "Linux" : "Apple Silicon" });
  const platformName = system?.os === "linux"
    ? `Linux ${system.arch}`
    : system?.os === "macos"
      ? `macOS ${system.arch}`
      : system?.arch ? `mac-${system.arch}` : "mac-arm64";
  let runtimeDetail = `ReXGlue ${RUNTIME_VERSION} · ${platformName}`;
  if (!state.runtime.buildFound && !state.runtime.installed) runtimeDetail = t("status.runtimeMissing");
  else if (!state.runtime.installed) runtimeDetail = t("status.runtimeNotInstalled");
  else if (state.runtime.updateAvailable) runtimeDetail = t("status.runtimeUpdate");
  if (state.runtime.source === "kit" && !state.runtime.installed) runtimeDetail = t("status.runtimeNotBuilt");
  else if (state.runtime.source === "kit" && state.runtime.updateAvailable) runtimeDetail = t("status.runtimeUpdateKit");
  if (building) runtimeDetail = t("status.runtimeBuilding");
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
  const fromKit = state.runtime.source === "kit";
  if (isInstalled() && building) {
    label = "play.building";
    sub = "play.buildingSub";
  } else if (isInstalled() && fromKit && state.game === "idle" && !state.runtime.installed) {
    // From the kit the button installs first, and says so; it turns to Play once installed.
    label = "play.installRuntime";
    sub = "play.installRuntimeSub";
  } else if (isInstalled() && fromKit && state.game === "idle" && state.runtime.updateAvailable) {
    label = "play.update";
    sub = state.runtime.updateKind === "link" ? "play.updateLinkSub" : "play.updateBuildSub";
  } else if (isInstalled()) {
    label = { idle: "play.launch", starting: "play.launching", running: "play.running" }[state.game];
    sub = state.game === "idle" && runtimeNeedsInstall() ? "play.runtimeWillInstall" : "play.mode";
  }
  button.querySelector(".btn-launch-label").textContent = t(label);
  button.querySelector(".btn-launch-sub").textContent = t(sub);
  renderLogState();

  // macOS: the chip is the GPU too, "Apple M4 · 24 GB · Metal". Linux: the processor
  // and memory ("AMD Ryzen 7 · 32 GB") and the graphics cards apart. A browser cannot tell, so it
  // names the platform only.
  const { chip, memoryGb, gpus = [] } = state.system ?? {};
  const memory = memoryGb ? `${memoryGb} GB` : null;
  document.getElementById("status-cpu").hidden = !linux;
  if (linux) {
    document.getElementById("status-cpu-detail").textContent = [chip ?? "Linux PC", memory].filter(Boolean).join(" · ");
    document.getElementById("status-gpu-detail").textContent = gpus.length
      ? t("status.gpuValueLinux", { gpu: gpus.join(" + ") })
      : t("status.gpuUnknown");
  } else {
    const machine = [chip ?? "Apple Silicon", memory].filter(Boolean).join(" · ");
    document.getElementById("status-gpu-detail").textContent = t("status.gpuValue", { machine });
  }

  renderReleaseNotes();
  renderBuildCard();

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
    // Building from the kit takes minutes: the button then only builds, and turns to Play once the
    // build is done, so the game does not start while the player may have stepped away. A project
    // build installs in a second, and Play goes on to start the game.
    if (runtimeNeedsInstall() && state.runtime.source === "kit") {
      await runRuntimeInstall();
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
  onGameRestart(() => {
    state.game = "running";
    setLogState("log.restarting");
    renderPlay();
  });
  onGameExit(({ code, success }) => {
    state.game = "idle";
    if (success) setLogState("log.exitOk", "ok");
    else if (code == null) setLogState("log.exitSignal", "error");
    else setLogState("log.exitError", "error", { code });
    renderPlay();
    // The game may have unlocked achievements.
    refreshAchievements();
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
  const fromKit = state.runtime.source === "kit";
  state.installingRuntime = true;
  state.buildProgress = null;
  if (fromKit) {
    // A build from the kit shows its own card with a bar on Play, not the game log.
    state.build = { tracker: createBuildTracker(), failed: null };
    state.build.tracker.update({ phase: "preparing" }, Date.now());
    startBuildTicker();
  } else {
    setLogState("log.installingRuntime");
  }
  renderInstall();
  renderPlay();
  try {
    state.runtime = await installRuntime();
    state.build = null;
    toast(t("toast.runtimeInstalled"));
    // The runtime reads the achievements, so they can show now.
    refreshAchievements();
    refreshGpus();
    return true;
  } catch (error) {
    const [key, vars] = buildErrorMessage(error);
    if (fromKit) state.build = { ...state.build, failed: { key, vars, log: error?.detail?.log ?? null } };
    else setLogState(key, "error", vars);
    if (error?.kind === "noCompiler") refreshCompiler();
    return false;
  } finally {
    state.installingRuntime = false;
    state.buildProgress = null;
    renderInstall();
    renderPlay();
  }
};

/** Redraws the build's bar and time left every second while a build runs (the time moves on even
 * when no file finishes). */
let buildTicker = null;
const startBuildTicker = () => {
  clearInterval(buildTicker);
  buildTicker = setInterval(() => {
    if (!state.installingRuntime) {
      clearInterval(buildTicker);
      buildTicker = null;
    }
    renderBuildCard();
  }, 1000);
};

/** "1 min 10 s", "45 s": a duration in the reader's language. */
const formatDuration = (ms) => {
  const seconds = Math.max(1, Math.round(ms / 1000));
  const unit = (value, name) => new Intl.NumberFormat(getLocale(), { style: "unit", unit: name, unitDisplay: "short" }).format(value);
  if (seconds < 60) return unit(seconds, "second");
  const minutes = Math.floor(seconds / 60);
  const rest = seconds % 60;
  return rest ? `${unit(minutes, "minute")} ${unit(rest, "second")}` : unit(minutes, "minute");
};

/**
 * Writes the build card on Play (the step, the bar, the percentage and the time left, or why it
 * failed with a retry and the log) and the bar in the Installation view.
 */
const renderBuildCard = () => {
  const build = state.build;
  const card = document.getElementById("build-card");
  const installBar = document.getElementById("install-build-bar");
  card.hidden = !build;
  installBar.hidden = !build || Boolean(build.failed);
  if (!build) return;
  const now = Date.now();
  const tracker = build.tracker;
  const failed = build.failed;
  const percent = tracker.percent(now);
  card.classList.toggle("is-failed", Boolean(failed));
  document.getElementById("build-card-title").textContent = t(failed ? "build.failedTitle" : "build.title");
  document.getElementById("build-card-percent").textContent = failed
    ? ""
    : new Intl.NumberFormat(getLocale(), { style: "percent" }).format(percent / 100);
  document.getElementById("build-card-bar").style.width = `${percent}%`;
  installBar.firstElementChild.style.width = `${percent}%`;
  let detail;
  if (failed) {
    detail = t(failed.key, failed.vars);
  } else {
    const progress = state.buildProgress ?? { phase: tracker.phase, done: 0, total: 0 };
    const step = t(BUILD_PHASE_KEYS[progress.phase] ?? "log.buildPreparing", { done: progress.done, total: progress.total });
    const remaining = tracker.remainingMs(now);
    detail = `${step} · ${remaining === null ? t("build.estimating") : t("build.remaining", { time: formatDuration(remaining) })}`;
  }
  document.getElementById("build-card-detail").textContent = detail;
  document.getElementById("build-card-actions").hidden = !failed;
  document.getElementById("build-card-log").hidden = !failed?.log;
};

/** Translation key of each step of a kit build, for the log header and the Installation view. */
const BUILD_PHASE_KEYS = {
  preparing: "log.buildPreparing",
  translating: "log.buildTranslating",
  compiling: "log.buildCompiling",
  linking: "log.buildLinking",
  installing: "log.buildInstalling",
};

/** Translation key and values for a failed runtime install or build (see platform.installRuntime). */
const buildErrorMessage = (error) => {
  const detail = error?.detail;
  switch (error?.kind) {
    case "noCompiler":
      return ["log.buildNoCompiler", {}];
    case "noKit":
      return ["log.buildNoKit", {}];
    case "wrongKit":
      return ["log.buildWrongKit", { detail: detail?.reason ?? "" }];
    case "unsupportedGame":
      return ["log.buildUnsupportedGame", {}];
    case "notEnoughSpace":
      return ["log.buildNoSpace", { needed: detail?.neededMb ?? 0, free: detail?.freeMb ?? 0 }];
    case "stepFailed":
      return ["log.buildStepFailed", {
        step: t(`log.buildStep.${detail?.phase ?? "preparing"}`),
        summary: detail?.summary ?? "",
        log: detail?.log ?? "",
      }];
    case "alreadyRunning":
      return ["log.buildBusy", {}];
    default:
      return ["log.installFailed", { detail: typeof detail === "string" ? detail : String(error) }];
  }
};

/** Shows a kit build's progress in the log header and the Installation view. */
const setupBuildProgress = () => {
  onRuntimeBuildProgress?.((progress) => {
    state.buildProgress = progress;
    state.build?.tracker.update(progress, Date.now());
    renderBuildCard();
    renderInstall();
  });
  document.getElementById("build-card-retry").addEventListener("click", () => {
    state.build = null;
    renderBuildCard();
    runRuntimeInstall();
  });
  document.getElementById("build-card-log").addEventListener("click", () => {
    if (state.build?.failed?.log) showBuildLog(state.build.failed.log).catch((error) => console.error(error));
  });
};

/** How often the status is checked while Apple's install window is open, and for how long. */
const APPLE_TOOLS_POLL_MS = 5000;
const APPLE_TOOLS_POLL_LIMIT_MS = 60 * 60 * 1000;

/** Writes the compiler's state into its Installation panel: what was found, and how to install it. */
const renderCompiler = () => {
  const status = state.compiler;
  const line = document.getElementById("compiler-check");
  const install = document.getElementById("install-compiler");
  const manual = document.getElementById("compiler-manual");
  const action = state.compilerAction;
  const ready = status?.status === "ready";
  line.dataset.state = ready ? "ok" : status ? "error" : "";
  if (action === "waitingForApple") line.textContent = t("install.compilerWaitingApple");
  else if (action === "installing") line.textContent = t("install.compilerInstalling");
  else if (!status || action === "checking") line.textContent = t("install.compilerChecking");
  else if (ready) line.textContent = t("install.compilerReady", { version: status.version });
  else if (status.found) line.textContent = t("install.compilerUnusable", { found: status.found, problem: status.problem ?? "" });
  else line.textContent = t("install.compilerMissing");

  const kind = status?.install?.kind;
  install.hidden = ready || !status || kind === "manual";
  install.disabled = action !== "";
  install.textContent = t(kind === "appleCommandLineTools" ? "install.compilerInstallApple" : "install.compilerInstallPackage");
  document.getElementById("recheck-compiler").disabled = action !== "";
  manual.hidden = ready || !status?.manual;
  document.getElementById("compiler-manual-command").textContent = status?.manual ?? "";
};

/** Checks the compiler again and redraws what shows it. */
const refreshCompiler = async () => {
  try {
    state.compiler = await getCompilerStatus();
  } catch (error) {
    console.error("Checking the compiler failed:", error);
  }
  renderCompiler();
  renderPlay();
};

/**
 * Wires the compiler panel. Installing on macOS opens Apple's window and returns at once, so the
 * status is checked every few seconds until the tools are there or Apple's installer is closed
 * without installing them; on Linux the package manager runs to the end first.
 */
const setupCompiler = () => {
  document.getElementById("recheck-compiler").addEventListener("click", async () => {
    state.compilerAction = "checking";
    renderCompiler();
    await refreshCompiler();
    state.compilerAction = "";
    renderCompiler();
  });

  document.getElementById("install-compiler").addEventListener("click", async () => {
    const apple = state.compiler?.install?.kind === "appleCommandLineTools";
    state.compilerAction = apple ? "waitingForApple" : "installing";
    renderCompiler();
    try {
      await installCompiler();
      if (apple) {
        // Until the tools are there, or Apple's installer has closed without installing them.
        const start = Date.now();
        while (Date.now() - start < APPLE_TOOLS_POLL_LIMIT_MS) {
          await new Promise((resolve) => setTimeout(resolve, APPLE_TOOLS_POLL_MS));
          state.compiler = await getCompilerStatus();
          if (state.compiler.status === "ready" || !state.compiler.installerRunning) break;
        }
      } else {
        state.compiler = await getCompilerStatus();
      }
      if (state.compiler?.status === "ready") toast(t("toast.compilerInstalled", { version: state.compiler.version }));
    } catch (error) {
      toast(t("toast.compilerInstallFailed", { detail: String(error) }));
      await refreshCompiler();
    } finally {
      state.compilerAction = "";
      renderCompiler();
      renderPlay();
    }
  });
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
  const progress = state.buildProgress;
  runtimeLine.textContent = state.installingRuntime && progress
    ? t(BUILD_PHASE_KEYS[progress.phase] ?? "log.buildPreparing", { done: progress.done, total: progress.total })
    : t(runtimeKey);
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
  refreshAchievements();
};

/**
 * Wires the game pickers: a folder, a disc image (.iso), or either dropped on the Installation
 * view. A failed check keeps the previous valid game in the settings, so a wrong pick never makes
 * the launcher forget a working installation on its next start.
 */
const setupInstall = () => {
  const buttons = [document.getElementById("pick-game"), document.getElementById("pick-game-image")];
  const choose = async (pick) => {
    buttons.forEach((button) => (button.disabled = true));
    try {
      const check = await pick();
      if (!check) return; // The user cancelled the picker.
      applyGameCheck(check);
      if (!isNativeApp) toast(t("toast.pick"));
    } catch (error) {
      console.error("Game picker failed:", error);
    } finally {
      buttons.forEach((button) => (button.disabled = false));
    }
  };
  buttons[0].addEventListener("click", () => choose(pickGameFolder));
  buttons[1].addEventListener("click", () => choose(pickGameImage));
  onPathDropped((path) => {
    if (document.querySelector('.view[data-view="install"]').classList.contains("is-active")) {
      choose(() => checkGameFolder(path));
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
    // Only what a gamertag may hold stays (textRules.js), however it was typed or pasted.
    playerField.addEventListener("input", () => {
      const clean = TEXT_RULES.gamertag.clean(playerField.value);
      if (clean !== playerField.value) playerField.value = clean;
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

/** Takes the graphics settings as stored. */
const applyStoredGraphics = (settings) => {
  // A value the game's menu does not offer (set by hand) reads as Automatic, as in the runtime.
  const stored = settings.graphics?.renderResolution;
  state.renderResolution = scaleOfResolution(stored) ? stored : "auto";
  state.fullscreen = Boolean(settings.fullscreen);
  state.gpu = settings.gpu ?? null;
};

/** Text of a resolution scale: "2560 × 1440 (2×)", or "1280 × 720 · original" for 1. */
const resolutionLabel = (scale) => {
  const { width, height } = renderSize(scale);
  const size = `${width} × ${height}`;
  return scale === 1 ? `${size} · ${t("graphics.original")}` : `${size} (${scale}×)`;
};

/** Writes the current graphics state into the controls. */
const renderGraphics = () => {
  const select = document.getElementById("resolution-scale");
  select.replaceChildren();
  select.add(new Option(t("graphics.resolutionAuto", { size: resolutionLabel(resolveAutoScale()) }), "auto"));
  for (const scale of RESOLUTION_SCALES) select.add(new Option(resolutionLabel(scale), resolutionValue(scale)));
  select.value = state.renderResolution;
  document.getElementById("display-mode").value = state.fullscreen ? "fullscreen" : "windowed";
  renderGpuChoice();
};

/** The graphics card choice, shown only when the computer has two or more cards. */
const renderGpuChoice = () => {
  const field = document.getElementById("gpu-field");
  field.hidden = !gpuChoiceVisible(state.gpus);
  if (field.hidden) return;
  const kindLabels = { discrete: t("graphics.gpuDiscrete"), integrated: t("graphics.gpuIntegrated") };
  const gpus = choosableGpus(state.gpus);
  const automatic = gpus.find((gpu) => gpu.uuid === state.gpus.automatic);
  const select = document.getElementById("gpu-choice");
  select.replaceChildren();
  select.add(
    new Option(
      t("graphics.gpuAuto", { name: automatic ? gpuLabel(automatic, kindLabels) : "…" }),
      "auto",
    ),
  );
  for (const gpu of gpus) select.add(new Option(gpuLabel(gpu, kindLabels), gpu.uuid));
  select.value = selectedGpu(state.gpu, state.gpus);
};

/** Asks the runtime for the graphics cards (it must be installed), then shows the choice. */
const refreshGpus = async () => {
  if (!state.runtime?.installed) return;
  try {
    state.gpus = await listGpus();
  } catch (error) {
    console.error("Listing the graphics cards failed:", error);
    state.gpus = { devices: [], automatic: null };
  }
  renderGpuChoice();
};

/** Wires the graphics controls; every change is saved. */
const setupGraphics = () => {
  const resolution = document.getElementById("resolution-scale");
  resolution.addEventListener("change", () => {
    state.renderResolution = resolution.value;
    persist();
  });

  const displayMode = document.getElementById("display-mode");
  displayMode.addEventListener("change", () => {
    state.fullscreen = displayMode.value === "fullscreen";
    persist();
  });

  const gpu = document.getElementById("gpu-choice");
  gpu.addEventListener("change", () => {
    state.gpu = gpu.value === "auto" ? null : gpu.value;
    persist();
  });
  onGpuMissing(() => toast(t("toast.gpuMissing")));
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
    refreshAchievements();
  });

  const gameSelect = document.getElementById("game-language");
  gameSelect.addEventListener("change", () => {
    state.gameLanguage = gameSelect.value;
    state.gameLanguageChosen = true;
    persist();
    refreshAchievements();
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

/** "24 oct 2026, 12:05": a moment in the reader's language and time zone. */
const formatDateTime = (ms) => new Intl.DateTimeFormat(getLocale(), { dateStyle: "medium", timeStyle: "short" }).format(new Date(ms));

/** "2,5 MB", "640 kB": a size in the reader's language, in the largest unit that keeps it above 1. */
const formatSize = (bytes) => {
  const units = ["byte", "kilobyte", "megabyte", "gigabyte"];
  let value = bytes;
  let index = 0;
  while (value >= 1024 && index < units.length - 1) {
    value /= 1024;
    index += 1;
  }
  const format = new Intl.NumberFormat(getLocale(), { style: "unit", unit: units[index], unitDisplay: "short", maximumFractionDigits: 1 });
  return format.format(value);
};

/** Translation keys for why a backup was made (BackupKind in saves.rs). */
const BACKUP_KIND_KEYS = {
  manual: "saves.kindManual",
  beforeRepair: "saves.kindBeforeRepair",
  beforeReset: "saves.kindBeforeReset",
};

/** The message for a failed save operation, from its { kind, detail } (SavesError in saves.rs). */
const savesErrorMessage = (error) => {
  if (error?.kind === "backupMissing") return t("saves.errorBackupMissing");
  return t("saves.errorFailed", { detail: error?.detail ?? String(error) });
};

/**
 * Renders the save manager: the status, its metrics and detail, the folder, the backups and, when
 * the player asked for an action that replaces saves, the confirmation waiting for an answer.
 */
const renderSaves = () => {
  const s = state.saveStatus;
  if (!s) return;

  const badgeState = s.status === "ok" ? "ok" : s.status === "corrupted" ? "warn" : "";
  document.getElementById("save-status-badge").dataset.state = badgeState;
  const statusKey = s.status === "ok" ? "Ok" : s.status === "corrupted" ? "Corrupted" : "Empty";
  document.getElementById("save-status-text").textContent = t(`saves.state${statusKey}`);
  document.getElementById("save-detail-msg").textContent = t(`saves.detail${statusKey}`);
  document.getElementById("save-checkpoints-count").textContent = new Intl.NumberFormat(getLocale()).format(s.checkpointCount);
  document.getElementById("save-last-modified").textContent = s.lastSavedMs ? formatDateTime(s.lastSavedMs) : "—";
  document.getElementById("save-path").value = s.savePath;

  const backups = document.getElementById("backups-list");
  backups.replaceChildren();
  if (!s.backups?.length) {
    const empty = document.createElement("p");
    empty.className = "help";
    empty.textContent = t("saves.noBackups");
    backups.append(empty);
  }
  for (const backup of s.backups ?? []) {
    const item = document.createElement("div");
    item.className = "backup-item";
    const info = document.createElement("div");
    const title = document.createElement("div");
    title.className = "backup-item-title";
    title.textContent = t(BACKUP_KIND_KEYS[backup.kind] ?? "saves.kindOther");
    const meta = document.createElement("div");
    meta.className = "backup-item-meta";
    meta.textContent = [backup.createdMs ? formatDateTime(backup.createdMs) : backup.id, formatSize(backup.sizeBytes)].join(" · ");
    info.append(title, meta);
    const restore = document.createElement("button");
    restore.className = "btn btn-sm btn-restore-backup";
    restore.dataset.id = backup.id;
    restore.textContent = t("saves.restore");
    item.append(info, restore);
    backups.append(item);
  }

  renderSavesConfirm();
};

/** Shows the confirmation of the action in state.savesPending, or hides it when there is none. */
const renderSavesConfirm = () => {
  const pending = state.savesPending;
  const box = document.getElementById("saves-confirm");
  box.hidden = !pending;
  if (!pending) return;
  const backup = pending.backup;
  const when = backup?.createdMs ? formatDateTime(backup.createdMs) : backup?.id;
  document.getElementById("saves-confirm-text").textContent =
    pending.action === "reset" ? t("saves.confirmReset") : t("saves.confirmRestore", { date: when });
  document.getElementById("saves-confirm-yes").textContent = t(pending.action === "reset" ? "saves.resetConfirm" : "saves.restore");
};

const refreshAndRenderSaves = async () => {
  try {
    state.saveStatus = await getSaveStatus();
    renderSaves();
  } catch (error) {
    console.error("Failed to load save status:", error);
  }
};

/**
 * Wires the save manager. Resetting and restoring replace the player's saves, so they first ask
 * for confirmation inside the view (state.savesPending), which the gamepad can answer too: its
 * Back button presses the cancel button (data-pad-back).
 */
const setupSaves = () => {
  /** Runs a save operation, then shows `success` (or why it failed) and the new state. */
  const run = async (operation, success) => {
    try {
      const result = await operation();
      toast(success(result));
    } catch (error) {
      console.error("Save operation failed:", error);
      toast(savesErrorMessage(error));
    }
    await refreshAndRenderSaves();
  };

  /** Asks for confirmation of `pending` and moves the focus to its safe answer. */
  const ask = (pending) => {
    state.savesPending = pending;
    renderSavesConfirm();
    document.getElementById("saves-confirm-no").focus();
  };

  document.getElementById("btn-repair-saves").addEventListener("click", () =>
    run(repairSaves, (removed) => (removed > 0 ? t("saves.repairedToast", { count: removed }) : t("saves.repairedNone"))),
  );
  document.getElementById("btn-reset-saves").addEventListener("click", () => ask({ action: "reset" }));
  document.getElementById("btn-backup-saves").addEventListener("click", () => run(createSaveBackup, () => t("saves.backupToast")));
  document.getElementById("open-saves-dir").addEventListener("click", () =>
    openSavesFolder().catch((error) => console.error("Open folder failed:", error)),
  );
  document.getElementById("backups-list").addEventListener("click", (event) => {
    const button = event.target.closest(".btn-restore-backup");
    if (!button) return;
    const backup = state.saveStatus?.backups?.find((b) => b.id === button.dataset.id);
    ask({ action: "restore", backup: backup ?? { id: button.dataset.id } });
  });

  document.getElementById("saves-confirm-no").addEventListener("click", () => {
    state.savesPending = null;
    renderSavesConfirm();
  });
  document.getElementById("saves-confirm-yes").addEventListener("click", () => {
    const pending = state.savesPending;
    state.savesPending = null;
    renderSavesConfirm();
    if (pending?.action === "reset") run(resetSaves, () => t("saves.resetToast"));
    else if (pending?.action === "restore") run(() => restoreSaveBackup(pending.backup.id), () => t("saves.restoreToast"));
  });
};

/** True while the Achievements section is the one on screen. */
const achievementsShown = () => document.querySelector('.view[data-view="achievements"]')?.classList.contains("is-active");

/**
 * Writes the Achievements section, the lock mark of its navigation entry (until the achievements
 * can be read) and the view selector, which shows only with the achievements.
 */
const renderAchievements = () => {
  const data = state.achievements;
  const ok = renderAchievementsPage(
    document.getElementById("achievements-body"),
    data,
    state.achievementsView,
    state.achievementsFilter,
    {
      t,
      locale: getLocale(),
      onFilter: (filter) => {
        state.achievementsFilter = filter;
        renderAchievements();
      },
      onGoToInstall: () => showView("install"),
    },
  );
  const banner = completedBanner(data, { t, locale: getLocale() });
  document.getElementById("achievements-banner").replaceChildren(...(banner ? [banner] : []));
  document.querySelector(".view-achievements").classList.toggle("is-earned", Boolean(banner));

  const views = document.getElementById("achievements-views");
  views.hidden = !ok;
  views.querySelectorAll("button").forEach((button) => {
    const on = button.dataset.mode === state.achievementsView;
    button.classList.toggle("is-on", on);
    button.setAttribute("aria-pressed", String(on));
  });
  const eyebrow = ok ? `achievements.view${state.achievementsView[0].toUpperCase()}${state.achievementsView.slice(1)}` : "achievements.locked";
  document.getElementById("achievements-eyebrow").textContent = t(eyebrow);

  const nav = document.getElementById("nav-achievements");
  nav.classList.toggle("is-locked", !ok);
  nav.querySelector(".lock")?.remove();
  if (!ok) nav.insertAdjacentHTML("beforeend", LOCK_ICON);
};

/** Achievements request in flight, and whether another was asked for meanwhile. */
let achievementsRequest = null;
let achievementsAgain = false;

/**
 * Reads the achievements again (the unlocks change while the game runs, and the game folder or
 * language may have changed) and redraws the section. A call while a read is in flight reads once
 * more after it, so the last change always shows. The first read of a game build has the runtime
 * read them from the game, so a loading card shows until then.
 */
const refreshAchievements = () => {
  if (achievementsRequest) {
    achievementsAgain = true;
    return achievementsRequest;
  }
  achievementsRequest = (async () => {
    do {
      achievementsAgain = false;
      if (state.achievements?.status !== "ok") {
        state.achievements = { status: "loading" };
        renderAchievements();
      }
      try {
        state.achievements = await getAchievements({
          gameFolder: isInstalled() ? state.savedGameFolder : null,
          gameLanguage: state.gameLanguage,
          uiLocale: getLocale(),
        });
      } catch (error) {
        console.error("Reading the achievements failed:", error);
        state.achievements = { status: "unavailable", detail: String(error) };
      }
      renderAchievements();
    } while (achievementsAgain);
    achievementsRequest = null;
  })();
  return achievementsRequest;
};

/** Wires the view selector; the chosen view is saved with the settings. */
const setupAchievements = () => {
  document.querySelectorAll("#achievements-views button").forEach((button) => {
    button.addEventListener("click", () => {
      state.achievementsView = button.dataset.mode;
      renderAchievements();
      persist();
    });
  });
};

const renderAll = () => {
  renderPlay();
  renderInstall();
  renderGameLanguages();
  renderGraphics();
  renderLanguageOptions();
  renderUpdate();
  renderSaves();
  renderAchievements();
  renderCompiler();
};

/**
 * Refreshes the runtime status and reloads the settings when the window regains focus, to show
 * what the game changed (full screen, in its Options menu). Settings are not reloaded while a save is pending, so an
 * edit that is about to be written is never replaced.
 */
const setupReloadOnFocus = () => {
  window.addEventListener("focus", async () => {
    try {
      // The runtime may have been rebuilt or removed while the window was in the background.
      await refreshRuntime();
      renderInstall();
      renderPlay();
      if (document.querySelector('.view[data-view="saves"]')?.classList.contains("is-active")) {
        await refreshAndRenderSaves();
      }
      if (achievementsShown()) await refreshAchievements();
      if (saveTimer) return;
      const settings = await loadSettings();
      if (!settings) return;
      applyStoredGraphics(settings);
      state.userName = TEXT_RULES.gamertag.clean(settings.userName ?? "");
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
// The controller belongs to the game while it runs.
setupGamepad({ showView, blocked: () => state.game !== "idle", t, onLocaleChange, onInput: onGamepadInput });
setupLaunch();
setupInstall();
setupGraphics();
setupLanguages();
setupSaves();
setupAchievements();
setupCompiler();
setupBuildProgress();
setupReloadOnFocus();

/**
 * Detects this Mac and the launcher version; a failure leaves them unknown, and "Automatic"
 * then resolves to the console's resolution.
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

// The machine is detected before the settings load, because "Automatic" resolves from it.
await detectSystem();
await refreshRuntime();
// In the background: it compiles a small program, and nothing below waits for it.
refreshCompiler();
const settings = await loadSavedSettings();
state.savesFolder = settings.savesFolder ?? "";
applyStoredGraphics(settings);
state.userName = TEXT_RULES.gamertag.clean(settings.userName ?? "");
state.gameLanguage = settings.gameLanguage ?? null;
state.gameLanguageChosen = Boolean(settings.gameLanguageChosen);
state.achievementsView = achievementView(settings.achievementsView);
state.seenReleaseNotes = settings.seenReleaseNotes ?? null;
onLocaleChange(renderAll);
await initI18n(settings.uiLanguage);
document.getElementById("ui-language").value = getPreference();
await restoreGameFolder(settings.gameFolder);
// Without a valid folder restoreGameFolder reads nothing; the section still needs its locked page.
if (!state.achievements) refreshAchievements();
await refreshAndRenderSaves();
setupUpdates();
setupReleaseNotes();
// Running the runtime to list the cards takes a moment: the rest of the window is ready first.
refreshGpus();
