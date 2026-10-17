#include "glyph_raster.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace azura::prompts {

// --- Canvas -------------------------------------------------------------------------------------

Canvas::Canvas(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width * height)) {}

void Canvas::Blend(int x, int y, const Rgb& color, float alpha) {
  if (alpha <= 0.0f || x < 0 || y < 0 || x >= width_ || y >= height_) {
    return;
  }
  alpha = std::min(alpha, 1.0f);
  auto& p = pixels_[static_cast<size_t>(y * width_ + x)];
  const float out_a = alpha + p[3] * (1.0f - alpha);
  if (out_a <= 0.0f) {
    return;
  }
  const float src[3] = {color.r, color.g, color.b};
  for (int i = 0; i < 3; ++i) {
    p[i] = (src[i] * alpha + p[i] * p[3] * (1.0f - alpha)) / out_a;
  }
  p[3] = out_a;
}

void Canvas::Fill(const Sdf& shape, const ColorFn& color) {
  constexpr int kSamples = 4;
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      int covered = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          if (shape(x + (sx + 0.5f) / kSamples, y + (sy + 0.5f) / kSamples) < 0.0f) {
            ++covered;
          }
        }
      }
      if (covered) {
        Blend(x, y, color(x + 0.5f, y + 0.5f), covered / float(kSamples * kSamples));
      }
    }
  }
}

void Canvas::Fill(const Sdf& shape, const Rgb& color) {
  Fill(shape, [color](float, float) { return color; });
}

std::vector<uint8_t> Canvas::ToRgba8() const {
  std::vector<uint8_t> out(pixels_.size() * 4);
  for (size_t i = 0; i < pixels_.size(); ++i) {
    for (int c = 0; c < 4; ++c) {
      const float v = c == 3 ? pixels_[i][3] * 255.0f : pixels_[i][c];
      out[i * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    }
  }
  return out;
}

// --- Shapes -------------------------------------------------------------------------------------

Sdf RoundedRect(float cx, float cy, float half_w, float half_h, float radius) {
  return [=](float x, float y) {
    const float qx = std::abs(x - cx) - (half_w - radius);
    const float qy = std::abs(y - cy) - (half_h - radius);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) -
           radius;
  };
}

Sdf Circle(float cx, float cy, float radius) {
  return [=](float x, float y) { return std::hypot(x - cx, y - cy) - radius; };
}

Sdf Ring(float cx, float cy, float radius, float half_thickness) {
  return [=](float x, float y) { return std::abs(std::hypot(x - cx, y - cy) - radius) - half_thickness; };
}

Sdf Segment(float x1, float y1, float x2, float y2, float half_thickness) {
  return [=](float x, float y) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len2 = dx * dx + dy * dy;
    const float t =
        len2 > 0.0f ? std::clamp(((x - x1) * dx + (y - y1) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(x - (x1 + t * dx), y - (y1 + t * dy)) - half_thickness;
  };
}

Sdf Union(std::initializer_list<Sdf> shapes) {
  std::vector<Sdf> list(shapes);
  return [list](float x, float y) {
    float d = 1e9f;
    for (const auto& s : list) {
      d = std::min(d, s(x, y));
    }
    return d;
  };
}

Sdf Inset(Sdf shape, float by) {
  return [shape = std::move(shape), by](float x, float y) { return shape(x, y) + by; };
}

Sdf Shell(Sdf shape, float half_thickness) {
  return [shape = std::move(shape), half_thickness](float x, float y) {
    return std::abs(shape(x, y)) - half_thickness;
  };
}

ColorFn VerticalGradient(Rgb top, Rgb bottom, float y0, float y1) {
  return [=](float, float y) {
    const float t = std::clamp((y - y0) / std::max(1e-6f, y1 - y0), 0.0f, 1.0f);
    return Rgb{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
               top.b + (bottom.b - top.b) * t};
  };
}

float Hash01(int ix, int iy, uint32_t seed) {
  uint32_t n = static_cast<uint32_t>(ix) * 374761393u + static_cast<uint32_t>(iy) * 668265263u +
               seed * 2147483647u;
  n = (n ^ (n >> 13)) * 1274126177u;
  return float((n ^ (n >> 16)) & 0xFFFFu) / 65535.0f;
}

float ValueNoise(float x, float y, uint32_t seed, float scale) {
  x /= scale;
  y /= scale;
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - ix;
  const float fy = y - iy;
  const float sx = fx * fx * (3 - 2 * fx);
  const float sy = fy * fy * (3 - 2 * fy);
  const float a = Hash01(ix, iy, seed);
  const float b = Hash01(ix + 1, iy, seed);
  const float c = Hash01(ix, iy + 1, seed);
  const float d = Hash01(ix + 1, iy + 1, seed);
  return ((a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy) * 2.0f - 1.0f;
}

// --- Pixel art ----------------------------------------------------------------------------------

Bitmap BitmapFromRows(std::initializer_list<std::string_view> rows) {
  Bitmap b;
  b.height = static_cast<int>(rows.size());
  for (auto row : rows) {
    b.width = std::max(b.width, static_cast<int>(row.size()));
  }
  b.bits.assign(static_cast<size_t>(b.width * b.height), 0);
  int y = 0;
  for (auto row : rows) {
    for (int x = 0; x < static_cast<int>(row.size()); ++x) {
      b.bits[static_cast<size_t>(y * b.width + x)] = row[x] == '#' ? 1 : 0;
    }
    ++y;
  }
  return b;
}

namespace {

// 5x7 font: letters, digits and the punctuation printed on keyboard keys.
const std::unordered_map<char, Bitmap>& LargeFont() {
  static const std::unordered_map<char, Bitmap> font = {
      {'A', BitmapFromRows({".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"})},
      {'B', BitmapFromRows({"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."})},
      {'C', BitmapFromRows({".####", "#....", "#....", "#....", "#....", "#....", ".####"})},
      {'D', BitmapFromRows({"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."})},
      {'E', BitmapFromRows({"#####", "#....", "#....", "####.", "#....", "#....", "#####"})},
      {'F', BitmapFromRows({"#####", "#....", "#....", "####.", "#....", "#....", "#...."})},
      {'G', BitmapFromRows({".####", "#....", "#....", "#.###", "#...#", "#...#", ".###."})},
      {'H', BitmapFromRows({"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"})},
      {'I', BitmapFromRows({"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"})},
      {'J', BitmapFromRows({"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."})},
      {'K', BitmapFromRows({"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"})},
      {'L', BitmapFromRows({"#....", "#....", "#....", "#....", "#....", "#....", "#####"})},
      {'M', BitmapFromRows({"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"})},
      {'N', BitmapFromRows({"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"})},
      {'O', BitmapFromRows({".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."})},
      {'P', BitmapFromRows({"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."})},
      {'Q', BitmapFromRows({".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"})},
      {'R', BitmapFromRows({"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"})},
      {'S', BitmapFromRows({".####", "#....", "#....", ".###.", "....#", "....#", "####."})},
      {'T', BitmapFromRows({"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."})},
      {'U', BitmapFromRows({"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."})},
      {'V', BitmapFromRows({"#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.."})},
      {'W', BitmapFromRows({"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"})},
      {'X', BitmapFromRows({"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"})},
      {'Y', BitmapFromRows({"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."})},
      {'Z', BitmapFromRows({"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"})},
      {'0', BitmapFromRows({".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."})},
      {'1', BitmapFromRows({"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."})},
      {'2', BitmapFromRows({".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"})},
      {'3', BitmapFromRows({"####.", "....#", "....#", ".###.", "....#", "....#", "####."})},
      {'4', BitmapFromRows({"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."})},
      {'5', BitmapFromRows({"#####", "#....", "####.", "....#", "....#", "#...#", ".###."})},
      {'6', BitmapFromRows({".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."})},
      {'7', BitmapFromRows({"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."})},
      {'8', BitmapFromRows({".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."})},
      {'9', BitmapFromRows({".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."})},
      {'`', BitmapFromRows({".#...", "..#..", "...#.", ".....", ".....", ".....", "....."})},
      {'-', BitmapFromRows({".....", ".....", ".....", "#####", ".....", ".....", "....."})},
      {'+', BitmapFromRows({".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."})},
      {'=', BitmapFromRows({".....", ".....", "#####", ".....", "#####", ".....", "....."})},
      {',', BitmapFromRows({".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."})},
      {'.', BitmapFromRows({".....", ".....", ".....", ".....", ".....", ".##..", ".##.."})},
      {';', BitmapFromRows({".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."})},
      {'/', BitmapFromRows({"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."})},
      {'\\', BitmapFromRows({"#....", "#....", ".#...", "..#..", "...#.", "....#", "....#"})},
      {'[', BitmapFromRows({".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."})},
      {']', BitmapFromRows({".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."})},
      {'\'', BitmapFromRows({"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."})},
      {'*', BitmapFromRows({".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."})},
      {'?', BitmapFromRows({".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."})},
  };
  return font;
}

// 3x5 font, for legends that do not fit at 5x7.
const std::unordered_map<char, Bitmap>& SmallFont() {
  static const std::unordered_map<char, Bitmap> font = {
      {'A', BitmapFromRows({".#.", "#.#", "###", "#.#", "#.#"})},
      {'B', BitmapFromRows({"##.", "#.#", "##.", "#.#", "##."})},
      {'C', BitmapFromRows({".##", "#..", "#..", "#..", ".##"})},
      {'D', BitmapFromRows({"##.", "#.#", "#.#", "#.#", "##."})},
      {'E', BitmapFromRows({"###", "#..", "##.", "#..", "###"})},
      {'F', BitmapFromRows({"###", "#..", "##.", "#..", "#.."})},
      {'G', BitmapFromRows({".##", "#..", "#.#", "#.#", ".##"})},
      {'H', BitmapFromRows({"#.#", "#.#", "###", "#.#", "#.#"})},
      {'I', BitmapFromRows({"###", ".#.", ".#.", ".#.", "###"})},
      {'J', BitmapFromRows({"..#", "..#", "..#", "#.#", ".#."})},
      {'K', BitmapFromRows({"#.#", "#.#", "##.", "#.#", "#.#"})},
      {'L', BitmapFromRows({"#..", "#..", "#..", "#..", "###"})},
      {'M', BitmapFromRows({"#.#", "###", "###", "#.#", "#.#"})},
      {'N', BitmapFromRows({"##.", "#.#", "#.#", "#.#", "#.#"})},
      {'O', BitmapFromRows({".#.", "#.#", "#.#", "#.#", ".#."})},
      {'P', BitmapFromRows({"##.", "#.#", "##.", "#..", "#.."})},
      {'Q', BitmapFromRows({".#.", "#.#", "#.#", "##.", ".##"})},
      {'R', BitmapFromRows({"##.", "#.#", "##.", "#.#", "#.#"})},
      {'S', BitmapFromRows({".##", "#..", ".#.", "..#", "##."})},
      {'T', BitmapFromRows({"###", ".#.", ".#.", ".#.", ".#."})},
      {'U', BitmapFromRows({"#.#", "#.#", "#.#", "#.#", "###"})},
      {'V', BitmapFromRows({"#.#", "#.#", "#.#", "#.#", ".#."})},
      {'W', BitmapFromRows({"#.#", "#.#", "###", "###", "#.#"})},
      {'X', BitmapFromRows({"#.#", "#.#", ".#.", "#.#", "#.#"})},
      {'Y', BitmapFromRows({"#.#", "#.#", ".#.", ".#.", ".#."})},
      {'Z', BitmapFromRows({"###", "..#", ".#.", "#..", "###"})},
      {'0', BitmapFromRows({"###", "#.#", "#.#", "#.#", "###"})},
      {'1', BitmapFromRows({".#.", "##.", ".#.", ".#.", "###"})},
      {'2', BitmapFromRows({"##.", "..#", ".#.", "#..", "###"})},
      {'3', BitmapFromRows({"##.", "..#", ".#.", "..#", "##."})},
      {'4', BitmapFromRows({"#.#", "#.#", "###", "..#", "..#"})},
      {'5', BitmapFromRows({"###", "#..", "##.", "..#", "##."})},
      {'6', BitmapFromRows({".##", "#..", "###", "#.#", "###"})},
      {'7', BitmapFromRows({"###", "..#", ".#.", ".#.", ".#."})},
      {'8', BitmapFromRows({"###", "#.#", "###", "#.#", "###"})},
      {'9', BitmapFromRows({"###", "#.#", "###", "..#", "##."})},
      {'-', BitmapFromRows({"...", "...", "###", "...", "..."})},
      {'+', BitmapFromRows({"...", ".#.", "###", ".#.", "..."})},
      {'=', BitmapFromRows({"...", "###", "...", "###", "..."})},
      {'*', BitmapFromRows({"...", "#.#", ".#.", "#.#", "..."})},
      {'/', BitmapFromRows({"..#", "..#", ".#.", "#..", "#.."})},
      {'.', BitmapFromRows({"...", "...", "...", "...", ".#."})},
      {',', BitmapFromRows({"...", "...", "...", ".#.", "#.."})},
      {';', BitmapFromRows({"...", ".#.", "...", ".#.", "#.."})},
      {'\'', BitmapFromRows({".#.", ".#.", "...", "...", "..."})},
      {'`', BitmapFromRows({"#..", ".#.", "...", "...", "..."})},
      {'[', BitmapFromRows({"##.", "#..", "#..", "#..", "##."})},
      {']', BitmapFromRows({".##", "..#", "..#", "..#", ".##"})},
      {'\\', BitmapFromRows({"#..", "#..", ".#.", "..#", "..#"})},
      {'?', BitmapFromRows({"##.", "..#", ".#.", "...", ".#."})},
      // Arrows for the mini key clusters.
      {'^', BitmapFromRows({"...", ".#.", "###", "...", "..."})},
      {'v', BitmapFromRows({"...", "###", ".#.", "...", "..."})},
      {'<', BitmapFromRows({"..#", ".##", "###", ".##", "..#"})},
      {'>', BitmapFromRows({"#..", "##.", "###", "##.", "#.."})},
      {'_', BitmapFromRows({"...", "...", "...", "#.#", "###"})},
  };
  return font;
}

}  // namespace

Bitmap TextBitmap(std::string_view text, bool small) {
  const auto& font = small ? SmallFont() : LargeFont();
  std::vector<const Bitmap*> glyphs;
  int width = 0;
  int height = 0;
  for (char c : text) {
    auto it = font.find(c);
    if (it == font.end()) {
      return {};
    }
    glyphs.push_back(&it->second);
    width += it->second.width + (glyphs.size() > 1 ? 1 : 0);
    height = std::max(height, it->second.height);
  }
  Bitmap out;
  out.width = width;
  out.height = height;
  out.bits.assign(static_cast<size_t>(width * height), 0);
  int x0 = 0;
  for (const Bitmap* g : glyphs) {
    for (int y = 0; y < g->height; ++y) {
      for (int x = 0; x < g->width; ++x) {
        out.bits[static_cast<size_t>(y * width + x0 + x)] = g->bits[static_cast<size_t>(y * g->width + x)];
      }
    }
    x0 += g->width + 1;
  }
  return out;
}

int FitScale(const Bitmap& bitmap, int max_w, int max_h) {
  if (bitmap.empty()) {
    return 1;
  }
  return std::max(1, std::min(max_w / bitmap.width, max_h / bitmap.height));
}

void DrawBitmap(Canvas& canvas, const Bitmap& bitmap, float cx, float cy, int scale,
                const ColorFn& color, bool bold, const Rgb* shadow) {
  if (bitmap.empty()) {
    return;
  }
  const int extra = bold ? 1 : 0;
  const int w = bitmap.width * scale + extra;
  const int h = bitmap.height * scale;
  const int x0 = static_cast<int>(std::lround(cx - w / 2.0f));
  const int y0 = static_cast<int>(std::lround(cy - h / 2.0f));
  for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
    const int off = pass == 0 ? 1 : 0;
    for (int y = 0; y < bitmap.height; ++y) {
      for (int x = 0; x < bitmap.width; ++x) {
        if (!bitmap.at(x, y)) {
          continue;
        }
        for (int dy = 0; dy < scale; ++dy) {
          for (int dx = 0; dx < scale + extra; ++dx) {
            const int px = x0 + x * scale + dx + off;
            const int py = y0 + y * scale + dy + off;
            canvas.Blend(px, py, pass == 0 ? *shadow : color(px + 0.5f, py + 0.5f), 1.0f);
          }
        }
      }
    }
  }
}

}  // namespace azura::prompts
