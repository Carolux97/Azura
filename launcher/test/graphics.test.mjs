/*
 * Tests for the launcher's graphics presets.
 *
 * Run with: npm test (from launcher/)
 */

import { test } from "node:test";
import assert from "node:assert/strict";

import { PRESETS, autoPresetFor, matchPreset, presetValues, renderSize, resolveAutoPreset } from "../src/graphics.js";

test("every fixed preset defines the same settings", () => {
  const keys = Object.keys(PRESETS.medium).sort();
  for (const [name, values] of Object.entries(PRESETS)) {
    assert.deepEqual(Object.keys(values).sort(), keys, name);
  }
});

test("fixed presets differ from each other, so a match is never ambiguous", () => {
  const serialized = Object.values(PRESETS).map((values) => JSON.stringify(values));
  assert.equal(new Set(serialized).size, serialized.length);
});

test("auto resolves to the values of a fixed preset", () => {
  assert.deepEqual(presetValues("auto"), PRESETS[resolveAutoPreset()]);
});

test("presetValues returns a copy, so editing it never changes the preset", () => {
  const values = presetValues("high");
  values.resolutionScale = 3;
  assert.equal(PRESETS.high.resolutionScale, 2);
});

test("unchanged values keep the current preset, including auto", () => {
  assert.equal(matchPreset(presetValues("high"), "high"), "high");
  assert.equal(matchPreset(presetValues("auto"), "auto"), "auto");
});

test("editing one value turns the preset into custom", () => {
  const edited = { ...presetValues("medium"), anisotropic: 5 };
  assert.equal(matchPreset(edited, "medium"), "custom");
  assert.equal(matchPreset(edited, "auto"), "custom");
});

test("editing values back to a preset's values selects that preset", () => {
  assert.equal(matchPreset({ ...PRESETS.low }, "custom"), "low");
});

test("render size multiplies the console's 1280x720", () => {
  assert.deepEqual(renderSize(1), { width: 1280, height: 720 });
  assert.deepEqual(renderSize(2), { width: 2560, height: 1440 });
  assert.deepEqual(renderSize(3), { width: 3840, height: 2160 });
});

test("auto picks a preset from the machine", () => {
  assert.equal(autoPresetFor({ chip: "Apple M4", memoryGb: 24, arch: "aarch64" }), "medium");
  assert.equal(autoPresetFor({ chip: "Apple M3 Pro", memoryGb: 18, arch: "aarch64" }), "high");
  assert.equal(autoPresetFor({ chip: "Apple M2 Max", memoryGb: 32, arch: "aarch64" }), "high");
  assert.equal(autoPresetFor({ chip: "Apple M1", memoryGb: 8, arch: "aarch64" }), "low");
  assert.equal(autoPresetFor({ chip: "Intel(R) Core(TM) i7", memoryGb: 32, arch: "x86_64" }), "low");
});

test("auto falls back to medium when the machine is unknown", () => {
  assert.equal(autoPresetFor(null), "medium");
  assert.equal(autoPresetFor({ chip: null, memoryGb: null, arch: "aarch64" }), "medium");
});

test("a memory-starved Pro chip still gets the low preset", () => {
  assert.equal(autoPresetFor({ chip: "Apple M1 Pro", memoryGb: 8, arch: "aarch64" }), "low");
});
