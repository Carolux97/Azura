#include "xenos_dxt5.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace azura::prompts {

namespace {

// The 360 stores each 16-bit word of the block big-endian; swapping every byte pair converts
// between that and the standard little-endian layout (in both directions).
Block SwapWords(const Block& in) {
  Block out;
  for (size_t i = 0; i < 16; i += 2) {
    out[i] = in[i + 1];
    out[i + 1] = in[i];
  }
  return out;
}

std::array<float, 3> From565(uint16_t c) {
  return {float(((c >> 11) & 31) * 255 / 31), float(((c >> 5) & 63) * 255 / 63), float((c & 31) * 255 / 31)};
}

uint16_t To565(float r, float g, float b) {
  auto q = [](float v, int max) {
    return static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround(v * max / 255.0f)), 0, max));
  };
  return static_cast<uint16_t>((q(r, 31) << 11) | (q(g, 63) << 5) | q(b, 31));
}

}  // namespace

uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks) {
  // Xenos 2D tiling for 16-byte elements (bytes-per-block log2 = 4), as in Xenia's texture_util.
  constexpr uint32_t kBppLog2 = 4;
  const uint32_t pitch = (pitch_blocks + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (kBppLog2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << kBppLog2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

std::array<Rgba, 16> DecodeDxt5(const Block& xenos_block) {
  const Block b = SwapWords(xenos_block);
  std::array<uint8_t, 8> alphas{};
  alphas[0] = b[0];
  alphas[1] = b[1];
  if (b[0] > b[1]) {
    for (int i = 1; i < 7; ++i) alphas[i + 1] = static_cast<uint8_t>(((7 - i) * b[0] + i * b[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) alphas[i + 1] = static_cast<uint8_t>(((5 - i) * b[0] + i * b[1]) / 5);
    alphas[6] = 0;
    alphas[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);

  const uint16_t c0 = static_cast<uint16_t>(b[8] | (b[9] << 8));
  const uint16_t c1 = static_cast<uint16_t>(b[10] | (b[11] << 8));
  const auto e0 = From565(c0);
  const auto e1 = From565(c1);
  std::array<std::array<float, 3>, 4> colors{};
  colors[0] = e0;
  colors[1] = e1;
  for (int k = 0; k < 3; ++k) {
    if (c0 > c1) {
      colors[2][k] = std::floor((2 * e0[k] + e1[k]) / 3);
      colors[3][k] = std::floor((e0[k] + 2 * e1[k]) / 3);
    } else {
      colors[2][k] = std::floor((e0[k] + e1[k]) / 2);
      colors[3][k] = 0;
    }
  }
  const uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);

  std::array<Rgba, 16> out{};
  for (int i = 0; i < 16; ++i) {
    const auto& c = colors[(cbits >> (2 * i)) & 3];
    out[i] = {static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]), static_cast<uint8_t>(c[2]),
              alphas[(abits >> (3 * i)) & 7]};
  }
  return out;
}

Block EncodeDxt5(const std::array<Rgba, 16>& px) {
  Block b{};

  // Alpha: endpoints at the block's extremes, eight interpolated levels.
  uint8_t amin = 255, amax = 0;
  for (const auto& p : px) {
    amin = std::min(amin, p[3]);
    amax = std::max(amax, p[3]);
  }
  b[0] = amax;
  b[1] = amin;
  std::array<int, 8> apal{};
  apal[0] = amax;
  apal[1] = amin;
  for (int i = 1; i < 7; ++i) apal[i + 1] = ((7 - i) * amax + i * amin) / 7;
  uint64_t abits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    for (int k = 1; k < 8; ++k) {
      if (std::abs(px[i][3] - apal[k]) < std::abs(px[i][3] - apal[best])) best = k;
    }
    abits |= uint64_t(best) << (3 * i);
  }
  for (int i = 0; i < 6; ++i) b[2 + i] = static_cast<uint8_t>(abits >> (8 * i));

  // Colour: endpoints along the principal axis of the visible pixels.
  std::array<std::array<float, 3>, 16> pts{};
  int n = 0;
  for (const auto& p : px) {
    if (p[3] > 8) pts[n++] = {float(p[0]), float(p[1]), float(p[2])};
  }
  if (n == 0) {
    for (const auto& p : px) pts[n++] = {float(p[0]), float(p[1]), float(p[2])};
  }
  std::array<float, 3> mean{};
  for (int i = 0; i < n; ++i)
    for (int k = 0; k < 3; ++k) mean[k] += pts[i][k] / n;
  float cov[3][3] = {};
  for (int i = 0; i < n; ++i)
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) cov[r][c] += (pts[i][r] - mean[r]) * (pts[i][c] - mean[c]);
  std::array<float, 3> axis{1, 1, 1};
  for (int it = 0; it < 8; ++it) {  // power iteration
    std::array<float, 3> next{};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) next[r] += cov[r][c] * axis[c];
    const float len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
    if (len < 1e-6f) break;
    for (int k = 0; k < 3; ++k) axis[k] = next[k] / len;
  }
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < n; ++i) {
    const float t = (pts[i][0] - mean[0]) * axis[0] + (pts[i][1] - mean[1]) * axis[1] + (pts[i][2] - mean[2]) * axis[2];
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  uint16_t c0 = To565(mean[0] + axis[0] * tmax, mean[1] + axis[1] * tmax, mean[2] + axis[2] * tmax);
  uint16_t c1 = To565(mean[0] + axis[0] * tmin, mean[1] + axis[1] * tmin, mean[2] + axis[2] * tmin);
  // Four-colour mode needs c0 > c1.
  if (c0 < c1) std::swap(c0, c1);
  if (c0 == c1) {
    if (c0 < 0xFFFF) ++c0; else --c1;
  }
  const auto e0 = From565(c0);
  const auto e1 = From565(c1);
  std::array<std::array<float, 3>, 4> pal{};
  pal[0] = e0;
  pal[1] = e1;
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = std::floor((2 * e0[k] + e1[k]) / 3);
    pal[3][k] = std::floor((e0[k] + 2 * e1[k]) / 3);
  }
  uint32_t cbits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    float best_d = 1e9f;
    for (int k = 0; k < 4; ++k) {
      float d = 0;
      for (int c = 0; c < 3; ++c) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
      if (d < best_d) {
        best_d = d;
        best = k;
      }
    }
    cbits |= uint32_t(best) << (2 * i);
  }
  b[8] = static_cast<uint8_t>(c0);
  b[9] = static_cast<uint8_t>(c0 >> 8);
  b[10] = static_cast<uint8_t>(c1);
  b[11] = static_cast<uint8_t>(c1 >> 8);
  for (int i = 0; i < 4; ++i) b[12 + i] = static_cast<uint8_t>(cbits >> (8 * i));
  return SwapWords(b);
}

}  // namespace azura::prompts
