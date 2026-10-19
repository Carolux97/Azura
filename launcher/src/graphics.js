/*
 * Graphics presets for the launcher, kept free of DOM access so they can be unit tested with Node.
 *
 * Every setting maps to a ReXGlue runtime setting (cvar) that the runtime reads at start-up, which
 * is why they belong in the launcher and not in the in-game menu (anti-aliasing, which also applies
 * at start-up, is in the game's Options menu instead, next to the other Video settings):
 *   resolutionScale  draw_resolution_scale_x / _y   (1 = the console's 1280x720)
 *   anisotropic      anisotropic_override           (0 off, 1 1x, 2 2x, 3 4x, 4 8x, 5 16x)
 *   upscaling        present_effect                 ("bilinear", "cas", "fsr")
 *   textureMemory    texture_cache_memory_limit_soft (MB); the hard limit is twice this value
 *
 * The preset values are a first proposal. They have to be tuned by measuring frame rates on real
 * Apple Silicon hardware once the game runs.
 */

/** Values of each fixed preset. "auto" is not here: it resolves to one of these at launch. */
export const PRESETS = {
  low: { resolutionScale: 1, anisotropic: 3, upscaling: "cas", textureMemory: 512 },
  medium: { resolutionScale: 1, anisotropic: 4, upscaling: "cas", textureMemory: 768 },
  high: { resolutionScale: 2, anisotropic: 5, upscaling: "cas", textureMemory: 1024 },
};

/** Preset names in the order the selector shows them. "custom" is never chosen directly. */
export const PRESET_ORDER = ["low", "medium", "high", "auto"];

/** Base render size of the game on the console; resolutionScale multiplies it. */
export const BASE_RENDER_SIZE = { width: 1280, height: 720 };

/** The settings a preset defines, in a stable order for comparison and display. */
export const SETTING_KEYS = Object.keys(PRESETS.medium);

/**
 * Picks the fixed preset "auto" stands for on a machine described by `system` (see
 * src-tauri/src/system.rs: { chip, memoryGb, arch }), or medium when nothing is known.
 *
 * The rule is a first proposal, to be tuned with frame rates measured on real hardware:
 *   - Intel Macs, or less than 16 GB of memory: low;
 *   - Pro, Max and Ultra chips: high (more GPU cores and memory bandwidth);
 *   - any other Apple Silicon chip: medium.
 */
export const autoPresetFor = (system) => {
  if (!system) return "medium";
  if (system.arch && system.arch !== "aarch64") return "low";
  if (system.memoryGb != null && system.memoryGb < 16) return "low";
  if (/\b(Pro|Max|Ultra)\b/.test(system.chip ?? "")) return "high";
  return "medium";
};

/** Machine the launcher runs on, once detected; null until then (and in a browser). */
let detectedSystem = null;

/** Records the detected machine, so "auto" resolves for it from then on. */
export const setDetectedSystem = (system) => {
  detectedSystem = system;
};

/** The fixed preset "auto" stands for on this machine. */
export const resolveAutoPreset = () => autoPresetFor(detectedSystem);

/** Returns the setting values of a preset name, resolving "auto" first. */
export const presetValues = (preset) => ({ ...PRESETS[preset === "auto" ? resolveAutoPreset() : preset] });

/** True when two settings objects hold the same value for every preset setting. */
const sameSettings = (a, b) => SETTING_KEYS.every((key) => a[key] === b[key]);

/**
 * Names the preset that `settings` corresponds to after the user edited one value.
 *
 * Auto stays selected while the values still equal what auto resolved to, so opening the advanced
 * block and touching nothing never turns it into "custom". Otherwise the first fixed preset with
 * identical values wins, and anything else is "custom".
 */
export const matchPreset = (settings, currentPreset) => {
  if (currentPreset === "auto" && sameSettings(settings, presetValues("auto"))) return "auto";
  return Object.keys(PRESETS).find((name) => sameSettings(settings, PRESETS[name])) ?? "custom";
};

/** Render size that a resolution scale produces, for display ("2560 × 1440"). */
export const renderSize = (scale) => {
  if (scale === "1080p" || scale === 1.5 || scale === "1.5") {
    return { width: 1920, height: 1080 };
  }
  return {
    width: BASE_RENDER_SIZE.width * Number(scale),
    height: BASE_RENDER_SIZE.height * Number(scale),
  };
};
