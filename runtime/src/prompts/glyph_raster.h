// Small software rasterizer for the button prompt glyphs.
//
// Glyphs are drawn at the size the game shows them (about 18 to 32 px), so everything here works
// in pixel units:
//   - shapes are signed distance functions (negative inside), sampled 4x4 per pixel for smooth
//     edges;
//   - letters and digits are drawn in a TrueType font (Barlow), antialiased, on whole pixels;
//     symbols are pixel art drawn as a smooth field, so their diagonals do not come out stepped;
//   - colours are straight (not premultiplied) RGB in 0..255 with a separate alpha.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace azura::prompts {

struct Rgb {
  float r = 0;
  float g = 0;
  float b = 0;
};

// Signed distance in pixels at a point: negative inside the shape, positive outside.
using Sdf = std::function<float(float x, float y)>;
// Colour at a point, for gradients.
using ColorFn = std::function<Rgb(float x, float y)>;

// RGBA float image with "over" compositing.
class Canvas {
 public:
  Canvas(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }

  // Composites `color` over pixel (x, y) with coverage `alpha`. Out-of-range pixels are ignored.
  void Blend(int x, int y, const Rgb& color, float alpha);

  // Fills where `shape` is negative, with 4x4 supersampled coverage.
  void Fill(const Sdf& shape, const ColorFn& color);
  void Fill(const Sdf& shape, const Rgb& color);

  // Straight-alpha RGBA8, row by row.
  std::vector<uint8_t> ToRgba8() const;

 private:
  int width_;
  int height_;
  std::vector<std::array<float, 4>> pixels_;
};

// --- Shapes -------------------------------------------------------------------------------------

Sdf RoundedRect(float cx, float cy, float half_w, float half_h, float radius);
Sdf Circle(float cx, float cy, float radius);
Sdf Ring(float cx, float cy, float radius, float half_thickness);
Sdf Segment(float x1, float y1, float x2, float y2, float half_thickness);
Sdf Union(std::initializer_list<Sdf> shapes);
// Grows (negative `by`) or shrinks (positive `by`) a shape.
Sdf Inset(Sdf shape, float by);
// The band of width 2 * half_thickness centred on a shape's edge.
Sdf Shell(Sdf shape, float half_thickness);

ColorFn VerticalGradient(Rgb top, Rgb bottom, float y0, float y1);

// Smooth value noise in [-1, 1] with features about `scale` pixels wide.
float ValueNoise(float x, float y, uint32_t seed, float scale);
// Deterministic hash in [0, 1].
float Hash01(int ix, int iy, uint32_t seed);

// --- Pixel art ----------------------------------------------------------------------------------

struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bits;  // width * height, 1 = lit

  bool empty() const { return width == 0; }
  bool at(int x, int y) const { return bits[y * width + x] != 0; }
};

// Builds a bitmap from rows of '#' (lit) and '.' (unlit).
Bitmap BitmapFromRows(std::initializer_list<std::string_view> rows);

// Renders `text` with the 5x7 font (`small` = false) or the 3x5 font, one pixel between letters.
// Returns an empty bitmap when a character has no glyph in that font.
Bitmap TextBitmap(std::string_view text, bool small);

// Largest whole scale at which `bitmap` fits in max_w x max_h (at least 1).
int FitScale(const Bitmap& bitmap, int max_w, int max_h);

// --- Smooth text --------------------------------------------------------------------------------

// Loads the TrueType font (Barlow) that letters and digits are drawn with, antialiased. Until it is
// loaded, or when it cannot be, legends use the pixel fonts above. Call it before drawing.
bool LoadLegendFont(const std::string& path);
bool HasLegendFont();

// Coverage of a text drawn in the legend font: width x height values in 0..1. The capitals take
// `cap_rows` rows from `cap_top`; what reaches past them (the tail of a Q, a comma, a bracket) is
// above or below, so a text is centred by its capitals.
struct Coverage {
  int width = 0;
  int height = 0;
  int cap_top = 0;
  int cap_rows = 0;
  std::vector<float> alpha;

  bool empty() const { return width == 0; }
};

// `text` in the legend font with capitals `cap_height` pixels tall. Empty without the font.
Coverage TextCoverage(std::string_view text, float cap_height);

// Draws `coverage` with its top left at whole pixel (x, y).
void DrawCoverage(Canvas& canvas, const Coverage& coverage, int x, int y, const ColorFn& color);

// Draws `bitmap` centred on (cx, cy) at a whole `scale`, its edges antialiased (a bilinear field of
// its pixels, so diagonals and corners are smooth). `bold` thickens its strokes by about a pixel,
// for legends drawn at scale 1. `shadow`, when set, is drawn 1 px down-right first.
void DrawBitmap(Canvas& canvas, const Bitmap& bitmap, float cx, float cy, int scale,
                const ColorFn& color, bool bold = false, const Rgb* shadow = nullptr);

}  // namespace azura::prompts
