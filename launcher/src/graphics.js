/*
 * Graphics settings the launcher sets before the game starts, kept free of DOM access so they can be
 * unit tested with Node. Everything else (vsync, anti-aliasing, motion blur, filtering, image
 * scaling, full screen) is in the game's Options menu.
 *
 * Settings written to the runtime (cvars, read at start-up):
 *   resolutionScale  draw_resolution_scale_x / _y    (1 = the console's 1280x720)
 *   textureMemory    texture_cache_memory_limit_soft (MB; the hard limit is twice this value),
 *                    derived from the resolution: a larger render needs more texture memory.
 *
 * The automatic resolution and the memory sizes are a first proposal, to be tuned by measuring
 * frame rates on real Apple Silicon hardware.
 */

/** Base render size of the game on the console; resolutionScale multiplies it. */
export const BASE_RENDER_SIZE = { width: 1280, height: 720 };

/** Resolution scales the launcher offers, in order. */
export const RESOLUTION_SCALES = [1, 2, 3];

/**
 * Resolution scale "Automatic" stands for on a machine described by `system` (see
 * src-tauri/src/system.rs: { chip, memoryGb, arch }):
 *   - Pro, Max and Ultra chips with at least 16 GB of memory: 2 (more GPU cores and memory
 *     bandwidth);
 *   - anything else, or an unknown machine: 1, the console's own resolution.
 */
export const autoResolutionScale = (system) => {
  if (!system || (system.arch && system.arch !== "aarch64")) return 1;
  if (system.memoryGb != null && system.memoryGb < 16) return 1;
  return /\b(Pro|Max|Ultra)\b/.test(system.chip ?? "") ? 2 : 1;
};

/** Texture memory (MB) for a resolution scale. */
export const textureMemoryFor = (scale) => (scale >= 2 ? 768 : 384);

/** Machine the launcher runs on, once detected; null until then (and in a browser). */
let detectedSystem = null;

/** Records the detected machine, so "Automatic" resolves for it from then on. */
export const setDetectedSystem = (system) => {
  detectedSystem = system;
};

/** Resolution scale "Automatic" stands for on this machine. */
export const resolveAutoScale = () => autoResolutionScale(detectedSystem);

/** Render size that a resolution scale produces, for display ("2560 × 1440"). */
export const renderSize = (scale) => ({
  width: BASE_RENDER_SIZE.width * scale,
  height: BASE_RENDER_SIZE.height * scale,
});
