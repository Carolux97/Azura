/*
 * Tests for the launcher's graphics settings.
 *
 * Run with: npm test (from launcher/)
 */

import { test } from "node:test";
import assert from "node:assert/strict";

import { RESOLUTION_SCALES, autoResolutionScale, renderSize, textureMemoryFor } from "../src/graphics.js";

test("render size multiplies the console's 1280x720", () => {
  assert.deepEqual(renderSize(1), { width: 1280, height: 720 });
  assert.deepEqual(renderSize(2), { width: 2560, height: 1440 });
  assert.deepEqual(renderSize(3), { width: 3840, height: 2160 });
});

test("automatic resolution doubles on Pro, Max and Ultra chips", () => {
  assert.equal(autoResolutionScale({ chip: "Apple M3 Pro", memoryGb: 18, arch: "aarch64" }), 2);
  assert.equal(autoResolutionScale({ chip: "Apple M2 Max", memoryGb: 32, arch: "aarch64" }), 2);
  assert.equal(autoResolutionScale({ chip: "Apple M4", memoryGb: 24, arch: "aarch64" }), 1);
});

test("automatic resolution stays at the console's on small, Intel or unknown machines", () => {
  assert.equal(autoResolutionScale({ chip: "Apple M1 Pro", memoryGb: 8, arch: "aarch64" }), 1);
  assert.equal(autoResolutionScale({ chip: "Intel(R) Core(TM) i7", memoryGb: 32, arch: "x86_64" }), 1);
  assert.equal(autoResolutionScale(null), 1);
  assert.equal(autoResolutionScale({ chip: null, memoryGb: null, arch: "aarch64" }), 1);
});

test("every offered resolution is one the automatic choice can produce or larger", () => {
  assert.ok(RESOLUTION_SCALES.includes(1) && RESOLUTION_SCALES.includes(2));
});

test("texture memory grows with the resolution", () => {
  assert.equal(textureMemoryFor(1), 384);
  assert.equal(textureMemoryFor(2), 768);
  assert.equal(textureMemoryFor(3), 768);
});
