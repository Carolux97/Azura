#include "glyph_art.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <unordered_map>

#include "glyph_raster.h"

namespace azura::prompts {

namespace {

// --- Palette ------------------------------------------------------------------------------------

// PC: black silhouettes and amber light.
constexpr Rgb kOmenBlack{9, 6, 4};
constexpr Rgb kOmenFace{24, 15, 8};
constexpr Rgb kAmberHot{255, 228, 150};
constexpr Rgb kAmber{255, 168, 40};
constexpr Rgb kAmberDeep{214, 96, 10};

// PlayStation: dark DualSense plastic, light rims and the four symbol colours.
constexpr Rgb kPsBodyTop{58, 64, 78};
constexpr Rgb kPsBodyBottom{20, 23, 30};
constexpr Rgb kPsRim{176, 190, 214};
constexpr Rgb kPsHole{12, 14, 18};
constexpr Rgb kWhite{246, 248, 252};
constexpr Rgb kInk{6, 5, 4};
constexpr Rgb kPsCross{92, 170, 255};
constexpr Rgb kPsCircle{255, 82, 82};
constexpr Rgb kPsSquare{255, 120, 205};
constexpr Rgb kPsTriangle{40, 220, 140};

ColorFn Solid(Rgb c) {
  return [c](float, float) { return c; };
}

// --- Symbols ------------------------------------------------------------------------------------

const Bitmap& SymbolShift() {
  static const Bitmap b = BitmapFromRows({"....#....", "...###...", "..#####..", ".#######.",
                                          "#########", "..#####..", "..#####..", "..#####.."});
  return b;
}
const Bitmap& SymbolCapsLock() {
  static const Bitmap b = BitmapFromRows({"....#....", "...###...", "..#####..", ".#######.",
                                          "#########", "..#####..", "..#####..", ".........",
                                          "..#####.."});
  return b;
}
const Bitmap& SymbolTab() {
  static const Bitmap b = BitmapFromRows({"......#...##", "......##..##", "#########.##",
                                          "#########.##", "......##..##", "......#...##"});
  return b;
}
const Bitmap& SymbolSpace() {
  static const Bitmap b =
      BitmapFromRows({"##...........##", "##...........##", "###############", "###############"});
  return b;
}
const Bitmap& SymbolEnter() {
  static const Bitmap b = BitmapFromRows({"......##", "......##", "..#...##", ".##...##",
                                          "########", ".##.....", "..#....."});
  return b;
}
const Bitmap& SymbolBackspace() {
  static const Bitmap b =
      BitmapFromRows({"..#.......", ".##.......", "##########", ".##.......", "..#......."});
  return b;
}
const Bitmap& SymbolArrowUp() {
  static const Bitmap b = BitmapFromRows(
      {"...#...", "..###..", ".#####.", "#######", "..###..", "..###..", "..###.."});
  return b;
}
const Bitmap& SymbolArrowDown() {
  static const Bitmap b = BitmapFromRows(
      {"..###..", "..###..", "..###..", "#######", ".#####.", "..###..", "...#..."});
  return b;
}
const Bitmap& SymbolArrowLeft() {
  static const Bitmap b = BitmapFromRows(
      {"...#...", "..##...", ".######", "#######", ".######", "..##...", "...#..."});
  return b;
}
const Bitmap& SymbolArrowRight() {
  static const Bitmap b = BitmapFromRows(
      {"...#...", "...##..", "######.", "#######", "######.", "...##..", "...#..."});
  return b;
}
const Bitmap& SymbolOptions() {
  static const Bitmap b = BitmapFromRows({"#######", ".......", "#######", ".......", "#######"});
  return b;
}
const Bitmap& SymbolMove() {
  static const Bitmap b = BitmapFromRows({"..#..", ".###.", "#.#.#", "#####", "#.#.#", ".###.", "..#.."});
  return b;
}

// --- Key faces ----------------------------------------------------------------------------------

enum class MouseButton : uint8_t { kNone, kLeft, kRight, kMiddle, kBack, kForward };

// How one bindable key is drawn.
struct KeyFace {
  MouseButton mouse = MouseButton::kNone;
  const Bitmap* symbol = nullptr;  // drawn instead of `text` when set
  std::string text;                // legend, in the 5x7 font when it fits, else 3x5
  std::string caption;             // small line above the legend ("NUM" on the numeric keypad)
  bool wide = false;               // drawn as a wider, lower key
  char mini = '?';                 // one 3x5 character for the small key groups
};

std::string Lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

KeyFace TextKey(std::string text, char mini, bool wide = false) {
  KeyFace f;
  f.text = std::move(text);
  f.mini = mini;
  f.wide = wide;
  return f;
}

KeyFace SymbolKey(const Bitmap& symbol, char mini, bool wide) {
  KeyFace f;
  f.symbol = &symbol;
  f.mini = mini;
  f.wide = wide;
  return f;
}

KeyFace MouseKey(MouseButton button) {
  KeyFace f;
  f.mouse = button;
  f.mini = 'M';
  return f;
}

// Every key name the keybind parser accepts (rex::ui::ParseVirtualKey), case-insensitive.
KeyFace DescribeKey(std::string_view name) {
  const std::string key = Lower(name);
  if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    return TextKey(std::string(1, c), c);
  }
  if (key.size() >= 2 && key[0] == 'f' &&
      std::all_of(key.begin() + 1, key.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
    return TextKey("F" + key.substr(1), 'F');
  }
  if (key.rfind("numpad", 0) == 0) {
    const std::string rest = key.substr(6);
    KeyFace f;
    if (rest.size() == 1 && std::isdigit(static_cast<unsigned char>(rest[0]))) {
      f = TextKey(rest, rest[0]);
    } else if (rest == "plus") {
      f = TextKey("+", '+');
    } else if (rest == "minus") {
      f = TextKey("-", '-');
    } else if (rest == "star") {
      f = TextKey("*", '*');
    } else if (rest == "slash") {
      f = TextKey("/", '/');
    } else if (rest == "enter") {
      f = SymbolKey(SymbolEnter(), '_', false);
    } else {
      f = TextKey("?", '?');
    }
    f.caption = "NUM";
    return f;
  }

  static const std::unordered_map<std::string, KeyFace> named = {
      {"backtick", TextKey("`", '`')},
      {"minus", TextKey("-", '-')},
      {"plus", TextKey("+", '+')},
      {"comma", TextKey(",", ',')},
      {"period", TextKey(".", '.')},
      {"semicolon", TextKey(";", ';')},
      {"slash", TextKey("/", '/')},
      {"backslash", TextKey("\\", '\\')},
      {"lbracket", TextKey("[", '[')},
      {"rbracket", TextKey("]", ']')},
      {"quote", TextKey("'", '\'')},
      {"escape", TextKey("ESC", 'E', true)},
      {"return", SymbolKey(SymbolEnter(), '_', true)},
      {"enter", SymbolKey(SymbolEnter(), '_', true)},
      {"space", SymbolKey(SymbolSpace(), '_', true)},
      {"tab", SymbolKey(SymbolTab(), 'T', true)},
      {"backspace", SymbolKey(SymbolBackspace(), 'B', true)},
      {"delete", TextKey("DEL", 'D', true)},
      {"insert", TextKey("INS", 'I', true)},
      {"home", TextKey("HOME", 'H', true)},
      {"end", TextKey("END", 'E', true)},
      {"pageup", TextKey("PGUP", 'P', true)},
      {"pagedown", TextKey("PGDN", 'P', true)},
      {"left", SymbolKey(SymbolArrowLeft(), '<', false)},
      {"right", SymbolKey(SymbolArrowRight(), '>', false)},
      {"up", SymbolKey(SymbolArrowUp(), '^', false)},
      {"down", SymbolKey(SymbolArrowDown(), 'v', false)},
      {"shift", SymbolKey(SymbolShift(), 'S', true)},
      {"control", TextKey("CTRL", 'C', true)},
      {"ctrl", TextKey("CTRL", 'C', true)},
      {"alt", TextKey("ALT", 'A', true)},
      {"printscreen", TextKey("PRT", 'P', true)},
      {"pause", TextKey("PAU", 'P', true)},
      {"capslock", SymbolKey(SymbolCapsLock(), 'C', true)},
      {"numlock", TextKey("NUM", 'N', true)},
      {"scrolllock", TextKey("SCR", 'S', true)},
      {"lmb", MouseKey(MouseButton::kLeft)},
      {"rmb", MouseKey(MouseButton::kRight)},
      {"mmb", MouseKey(MouseButton::kMiddle)},
      {"middlemouse", MouseKey(MouseButton::kMiddle)},
      {"mb4", MouseKey(MouseButton::kBack)},
      {"mouse4", MouseKey(MouseButton::kBack)},
      {"xbutton1", MouseKey(MouseButton::kBack)},
      {"mb5", MouseKey(MouseButton::kForward)},
      {"mouse5", MouseKey(MouseButton::kForward)},
      {"xbutton2", MouseKey(MouseButton::kForward)},
  };
  auto it = named.find(key);
  if (it != named.end()) {
    return it->second;
  }
  return TextKey("?", '?');
}

// --- Omen style helpers -------------------------------------------------------------------------

// Roughens a shape's edge like an ink stain: two octaves of noise added to its distance.
Sdf Ragged(Sdf shape, uint32_t seed, float amplitude) {
  return [shape = std::move(shape), seed, amplitude](float x, float y) {
    return shape(x, y) + amplitude * (ValueNoise(x, y, seed, 2.6f) * 0.7f +
                                      ValueNoise(x, y, seed + 7, 1.2f) * 0.3f);
  };
}

// Adds thin tapered spikes sticking out of the silhouette, like the thorny branches of the art.
Sdf Thorns(Sdf shape, float cx, float cy, uint32_t seed, int count) {
  struct Spike {
    float x1, y1, x2, y2;
  };
  std::vector<Spike> spikes;
  for (int i = 0; i < count; ++i) {
    const float angle = Hash01(i, 3, seed) * 2.0f * std::numbers::pi_v<float>;
    const float dx = std::cos(angle);
    const float dy = std::sin(angle);
    float ex = cx;
    float ey = cy;
    for (int step = 0; step < 80; ++step) {  // walk out to the edge
      ex = cx + dx * step * 0.5f;
      ey = cy + dy * step * 0.5f;
      if (shape(ex, ey) > 0) {
        break;
      }
    }
    const float length = 1.6f + Hash01(i, 9, seed) * 1.8f;
    spikes.push_back({ex - dx * 1.5f, ey - dy * 1.5f, ex + dx * length, ey + dy * length});
  }
  return [shape = std::move(shape), spikes](float x, float y) {
    float d = shape(x, y);
    for (const auto& s : spikes) {
      const float sx = s.x2 - s.x1;
      const float sy = s.y2 - s.y1;
      const float t = std::clamp(((x - s.x1) * sx + (y - s.y1) * sy) / (sx * sx + sy * sy), 0.0f, 1.0f);
      const float half = 0.75f * (1.0f - t) + 0.05f;
      d = std::min(d, std::hypot(x - (s.x1 + t * sx), y - (s.y1 + t * sy)) - half);
    }
    return d;
  };
}

// Amber light spilling around a silhouette: alpha falls off with distance from its edge.
void Backlight(Canvas& canvas, const Sdf& shape, float reach, float strength) {
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) {
      const float d = shape(x + 0.5f, y + 0.5f);
      if (d <= 0) {
        continue;
      }
      const float a = strength * std::exp(-d / (reach * 0.45f));
      if (a > 0.02f) {
        const float t = std::min(1.0f, d / reach);
        canvas.Blend(x, y,
                     Rgb{kAmber.r + (kAmberDeep.r - kAmber.r) * t, kAmber.g + (kAmberDeep.g - kAmber.g) * t,
                         kAmber.b + (kAmberDeep.b - kAmber.b) * t},
                     a);
      }
    }
  }
}

// Picks the legend bitmap for a key face that has to fit in max_w x max_h.
Bitmap LegendFor(const KeyFace& face, int max_w, int max_h, bool* is_symbol) {
  *is_symbol = face.symbol != nullptr;
  if (face.symbol) {
    return *face.symbol;
  }
  Bitmap big = TextBitmap(face.text, false);
  if (!big.empty() && big.width + 1 <= max_w && big.height <= max_h) {
    return big;
  }
  // 3x5, dropping trailing characters until it fits.
  std::string text = face.text;
  while (!text.empty()) {
    Bitmap small = TextBitmap(text, true);
    if (!small.empty() && small.width <= max_w) {
      return small;
    }
    text.pop_back();
  }
  return TextBitmap("?", true);
}

// Draws a lit legend (or legend plus caption) centred on (cx, cy).
void DrawLitLegend(Canvas& canvas, const KeyFace& face, float cx, float cy, int max_w, int max_h) {
  const bool has_caption = !face.caption.empty();
  const Bitmap caption = has_caption ? TextBitmap(face.caption, true) : Bitmap{};
  const int legend_h = has_caption ? max_h - caption.height - 1 : max_h;
  bool is_symbol = false;
  const Bitmap legend = LegendFor(face, max_w, legend_h, &is_symbol);
  const int scale = FitScale(legend, max_w, legend_h);
  // Bold only thickens single characters: in longer legends it would close the gaps between letters.
  const bool bold = scale == 1 && !is_symbol && face.text.size() == 1 && legend.width + 1 <= max_w;
  const float total_h = legend.height * scale + (has_caption ? caption.height + 1 : 0);
  const float top = cy - total_h / 2.0f;
  if (has_caption) {
    DrawBitmap(canvas, caption, cx, top + caption.height / 2.0f, 1, Solid(kAmberDeep));
  }
  const float legend_cy = top + (has_caption ? caption.height + 1 : 0) + legend.height * scale / 2.0f;
  const float y0 = legend_cy - legend.height * scale / 2.0f;
  DrawBitmap(canvas, legend, cx, legend_cy, scale,
             VerticalGradient(kAmberHot, kAmber, y0, y0 + legend.height * scale), bold);
}

// --- PC glyphs ----------------------------------------------------------------------------------

void DrawOmenKey(Canvas& canvas, const KeyFace& face, uint32_t seed) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_w = w / 2.0f - 2.2f;
  const float key_h = h * (face.wide ? 0.74f : 0.84f);
  const float top = (h - key_h) / 2.0f;
  const float cy = top + key_h / 2.0f;
  Sdf blot = Thorns(Ragged(RoundedRect(w / 2, cy, half_w, key_h / 2, 3.5f), seed, 0.9f), w / 2, cy, seed, 3);
  Backlight(canvas, blot, 4.0f, 1.0f);
  canvas.Fill(blot, kOmenBlack);
  // A barely visible key top, so the stain still reads as a key.
  canvas.Fill(Ragged(RoundedRect(w / 2, cy - 1, half_w - 2.2f, key_h / 2 - 2.6f, 2.0f), seed + 3, 0.4f),
              kOmenFace);
  const int max_w = static_cast<int>(2 * (half_w - 1.1f));
  const int max_h = static_cast<int>(key_h - 5);
  DrawLitLegend(canvas, face, w / 2, cy - 1, max_w, max_h);
}

// The mouse: an egg-shaped silhouette with seams of light. `lit` is the button the prompt is for;
// kNone with `look` set draws the movement arrows instead.
void DrawOmenMouse(Canvas& canvas, MouseButton lit, bool look, uint32_t seed) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const bool side = lit == MouseButton::kBack || lit == MouseButton::kForward;
  // Leave room on the left for the side buttons when they are the prompt.
  const float cx = w / 2.0f + (side ? 1.2f : 0.0f);
  const float cy = h / 2.0f;
  const float half_w = std::min(w / 2.0f - 1.9f - (side ? 1.2f : 0.0f), h * 0.40f);
  const float half_h = h / 2.0f - 1.8f;
  Sdf egg = [=](float x, float y) {
    const float k = half_w * (0.86f + 0.14f * std::clamp((y - (cy - half_h)) / (2 * half_h), 0.0f, 1.0f));
    const float m = std::min(k, half_h);
    return std::hypot((x - cx) / k, (y - cy) / half_h) * m - m;
  };
  Sdf blot = Thorns(Ragged(egg, seed, 0.9f), cx, cy, seed, 2);
  Backlight(canvas, blot, 4.0f, 1.0f);
  canvas.Fill(blot, kOmenBlack);

  const float split_y = cy - half_h * 0.02f;
  const ColorFn lit_grad = VerticalGradient(kAmberHot, kAmber, cy - half_h, split_y);
  if (lit == MouseButton::kLeft || lit == MouseButton::kRight) {
    const bool left = lit == MouseButton::kLeft;
    canvas.Fill(
        [=](float x, float y) {
          const float side_d = left ? x - (cx - 0.7f) : (cx + 0.7f) - x;
          return std::max({egg(x, y) + 1.6f, y - split_y + 0.6f, side_d});
        },
        lit_grad);
  }
  // Light leaking through the seams.
  canvas.Fill([=](float x, float y) { return std::max(egg(x, y) + 1.4f, std::abs(y - split_y) - 0.45f); },
              kAmberDeep);
  canvas.Fill(
      [=](float x, float y) {
        return std::max({egg(x, y) + 1.4f, y - split_y, std::abs(x - cx) - 0.45f});
      },
      kAmberDeep);
  // Wheel.
  Sdf wheel = RoundedRect(cx, cy - half_h * 0.50f, 1.3f, half_h * 0.20f, 1.2f);
  canvas.Fill(Inset(wheel, -0.7f), kOmenBlack);
  canvas.Fill(wheel, lit == MouseButton::kMiddle ? kAmberHot : kAmberDeep);
  // Side buttons, on the thumb side: forward above, back below.
  if (side) {
    const float sx = cx - half_w * 0.93f;
    const float fy = cy - half_h * 0.02f + 2.0f;
    const float by = fy + 4.2f;
    canvas.Fill(Inset(RoundedRect(sx, fy, 1.5f, 1.7f, 0.8f), -0.7f), kOmenBlack);
    canvas.Fill(Inset(RoundedRect(sx, by, 1.5f, 1.7f, 0.8f), -0.7f), kOmenBlack);
    canvas.Fill(RoundedRect(sx, fy, 1.5f, 1.7f, 0.8f), lit == MouseButton::kForward ? kAmberHot : kAmberDeep);
    canvas.Fill(RoundedRect(sx, by, 1.5f, 1.7f, 0.8f), lit == MouseButton::kBack ? kAmberHot : kAmberDeep);
  }
  if (look) {
    const Bitmap& move = SymbolMove();
    const float my = cy + half_h * 0.45f;
    DrawBitmap(canvas, move, cx, my, 1, VerticalGradient(kAmberHot, kAmber, my - 3, my + 3));
  }
}

// A small key for the WASD-style groups: a ragged black block with one 3x5 character.
void DrawMiniKey(Canvas& canvas, float x0, float y0, float w, float h, const KeyFace& face, uint32_t seed) {
  const float cx = x0 + w / 2;
  const float cy = y0 + h / 2;
  canvas.Fill(Ragged(RoundedRect(cx, cy, w / 2 - 0.3f, h / 2 - 0.3f, 1.4f), seed, 0.45f), kOmenBlack);
  const Bitmap legend = TextBitmap(std::string(1, face.mini), true);
  DrawBitmap(canvas, legend, cx, cy, 1, VerticalGradient(kAmberHot, kAmber, cy - 2.5f, cy + 2.5f));
}

void DrawCross(Canvas& canvas, const std::vector<std::string>& keys, uint32_t seed) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cell_w = std::floor(w / 3.0f);
  const float cell_h = std::min(cell_w + 2.0f, std::floor((h - 2.0f) / 2.0f));
  const float x0 = (w - 3 * cell_w) / 2.0f;
  const float y0 = std::floor((h - (2 * cell_h + 1)) / 2.0f);
  struct Cell {
    float x, y;
  };
  // keys = {up, left, down, right}
  const Cell cells[4] = {{x0 + cell_w, y0}, {x0, y0 + cell_h + 1}, {x0 + cell_w, y0 + cell_h + 1},
                         {x0 + 2 * cell_w, y0 + cell_h + 1}};
  Sdf all = [=](float x, float y) {
    float d = 1e9f;
    for (const auto& c : cells) {
      d = std::min(d, RoundedRect(c.x + cell_w / 2, c.y + cell_h / 2, cell_w / 2, cell_h / 2, 1.4f)(x, y));
    }
    return d;
  };
  Backlight(canvas, all, 2.0f, 0.9f);
  for (int i = 0; i < 4; ++i) {
    const KeyFace face = DescribeKey(i < static_cast<int>(keys.size()) ? keys[i] : "?");
    DrawMiniKey(canvas, cells[i].x, cells[i].y, cell_w, cell_h, face, seed + i * 11);
  }
}

void DrawPair(Canvas& canvas, const std::vector<std::string>& keys, uint32_t seed) {
  const int w = canvas.width();
  const int h = canvas.height();
  const int half = w / 2;
  for (int i = 0; i < 2; ++i) {
    Canvas part(half, h);
    const KeyFace face = DescribeKey(i < static_cast<int>(keys.size()) ? keys[i] : "?");
    if (face.mouse != MouseButton::kNone) {
      DrawOmenMouse(part, face.mouse, false, seed + i * 5);
    } else {
      DrawOmenKey(part, face, seed + i * 5);
    }
    const auto rgba = part.ToRgba8();
    const int ox = i == 0 ? 0 : w - half;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < half; ++x) {
        const uint8_t* p = &rgba[static_cast<size_t>((y * half + x) * 4)];
        canvas.Blend(ox + x, y, Rgb{float(p[0]), float(p[1]), float(p[2])}, p[3] / 255.0f);
      }
    }
  }
}

// --- PlayStation glyphs -------------------------------------------------------------------------

void PsLabel(Canvas& canvas, std::string_view label, float cx, float cy, int max_w, int max_h) {
  Bitmap rows = TextBitmap(label, false);
  if (rows.empty() || rows.width > max_w) {
    rows = TextBitmap(label, true);
  }
  const int scale = FitScale(rows, max_w, max_h);
  DrawBitmap(canvas, rows, cx, cy, scale, Solid(kWhite), scale == 1 && rows.width + 1 <= max_w, &kInk);
}

void DrawPsFace(Canvas& canvas, PadButton button) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cx = w / 2;
  const float cy = h / 2;
  const float r = std::min(w, h) / 2 - 0.6f;
  canvas.Fill(Circle(cx, cy, r), VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - r, cy + r));
  canvas.Fill(Ring(cx, cy, r - 0.6f, 0.6f), kPsHole);
  const float s = r * 0.50f;                    // symbol half-size
  const float t = std::max(1.05f, r * 0.13f);  // stroke half-width
  switch (button) {
    case PadButton::kA:
      canvas.Fill(Union({Segment(cx - s, cy - s, cx + s, cy + s, t), Segment(cx + s, cy - s, cx - s, cy + s, t)}),
                  kPsCross);
      break;
    case PadButton::kB:
      canvas.Fill(Ring(cx, cy, s * 0.98f, t), kPsCircle);
      break;
    case PadButton::kX: {
      const float q = s * 0.86f;
      canvas.Fill(Shell(RoundedRect(cx, cy, q, q, 0.8f), t), kPsSquare);
      break;
    }
    default: {
      const float a = s * 1.1f;
      const float x1 = cx, y1 = cy - a * 0.95f, x2 = cx + a, y2 = cy + a * 0.72f, x3 = cx - a, y3 = y2;
      canvas.Fill(Union({Segment(x1, y1, x2, y2, t), Segment(x2, y2, x3, y3, t), Segment(x3, y3, x1, y1, t)}),
                  kPsTriangle);
      break;
    }
  }
}

void DrawPsBumper(Canvas& canvas, std::string_view label) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_h = h * 0.30f;
  const float cy = h * 0.56f;
  Sdf pill = RoundedRect(w / 2, cy, w / 2 - 0.6f, half_h, half_h * 0.75f);
  canvas.Fill(pill, VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - half_h, cy + half_h));
  canvas.Fill(Shell(Inset(pill, 0.6f), 0.6f), kPsRim);
  PsLabel(canvas, label, w / 2, cy, static_cast<int>(w - 6), static_cast<int>(2 * half_h - 3));
}

void DrawPsTrigger(Canvas& canvas, std::string_view label) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_w = w / 2 - 0.6f;
  const float top = 1.0f;
  const float bottom = h - 1.5f;
  const float dome_cy = top + half_w * 0.9f;
  Sdf body = Union({RoundedRect(w / 2, (dome_cy + bottom) / 2, half_w, (bottom - dome_cy) / 2, 2.5f),
                    [=](float x, float y) {
                      return std::hypot((x - w / 2) / half_w, (y - dome_cy) / (dome_cy - top)) * half_w - half_w;
                    }});
  canvas.Fill(body, VerticalGradient(kPsBodyTop, kPsBodyBottom, top, bottom));
  canvas.Fill(Shell(Inset(body, 0.6f), 0.6f), kPsRim);
  PsLabel(canvas, label, w / 2, (top + bottom) / 2 + 2, static_cast<int>(w - 6), static_cast<int>(h * 0.5f));
}

// LS/RS: direction notches around the cap. L3/R3: the cap pushed in (double ring).
void DrawPsStick(Canvas& canvas, char side, bool pressed) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cx = w / 2;
  const float cy = h / 2;
  const float r = std::min(w, h) / 2 - 0.6f;
  canvas.Fill(Circle(cx, cy, r), kPsHole);
  const float cap = r - (pressed ? 1.4f : 2.2f);
  canvas.Fill(Circle(cx, cy, cap), VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - cap, cy + cap));
  if (pressed) {
    canvas.Fill(Ring(cx, cy, cap - 0.4f, 0.6f), kWhite);
    canvas.Fill(Ring(cx, cy, cap - 2.6f, 0.5f), kPsRim);
  } else {
    canvas.Fill(Ring(cx, cy, cap, 0.5f), kPsRim);
    for (int i = 0; i < 4; ++i) {
      const float a = i * std::numbers::pi_v<float> / 2;
      canvas.Fill(Circle(cx + std::cos(a) * (r - 0.9f), cy + std::sin(a) * (r - 0.9f), 0.9f), kWhite);
    }
  }
  DrawBitmap(canvas, TextBitmap(std::string(1, side), false), cx, cy, 1, Solid(kWhite), true, &kInk);
}

void DrawPsOptions(Canvas& canvas) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_h = h * 0.30f;
  const float cy = h / 2;
  Sdf pill = RoundedRect(w / 2, cy, w / 2 - 0.6f, half_h, half_h * 0.6f);
  canvas.Fill(pill, VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - half_h, cy + half_h));
  canvas.Fill(Shell(Inset(pill, 0.6f), 0.6f), kPsRim);
  DrawBitmap(canvas, SymbolOptions(), w / 2, cy, 1, Solid(kWhite), false, &kInk);
}

void DrawPsTouchpad(Canvas& canvas) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_h = h * 0.32f;
  const float cy = h / 2;
  Sdf pad = RoundedRect(w / 2, cy, w / 2 - 0.6f, half_h, 3.5f);
  canvas.Fill(pad, VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - half_h, cy + half_h));
  canvas.Fill(Shell(Inset(pad, 0.6f), 0.6f), kPsRim);
  // A fingertip pressing the pad.
  canvas.Fill(Circle(w / 2, cy, 2.2f), kWhite);
  canvas.Fill(Ring(w / 2, cy, 3.8f, 0.5f), kPsRim);
}

void DrawPsDpad(Canvas& canvas, bool up, bool down, bool left, bool right) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cx = w / 2;
  const float cy = h / 2;
  const float a = std::min(w, h) / 2 - 0.6f;
  const float t = a * 0.36f;
  Sdf arms = Union({RoundedRect(cx, cy, a, t, 1.5f), RoundedRect(cx, cy, t, a, 1.5f)});
  canvas.Fill(arms, VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - a, cy + a));
  canvas.Fill(Shell(Inset(arms, 0.6f), 0.6f), kPsRim);
  if (up) canvas.Fill(RoundedRect(cx, cy - a * 0.62f, t - 1.4f, a * 0.30f, 1), kWhite);
  if (down) canvas.Fill(RoundedRect(cx, cy + a * 0.62f, t - 1.4f, a * 0.30f, 1), kWhite);
  if (left) canvas.Fill(RoundedRect(cx - a * 0.62f, cy, a * 0.30f, t - 1.4f, 1), kWhite);
  if (right) canvas.Fill(RoundedRect(cx + a * 0.62f, cy, a * 0.30f, t - 1.4f, 1), kWhite);
}

}  // namespace

std::vector<uint8_t> DrawPlayStationGlyph(PadButton button, int width, int height) {
  Canvas canvas(width, height);
  switch (button) {
    case PadButton::kA:
    case PadButton::kB:
    case PadButton::kX:
    case PadButton::kY:
      DrawPsFace(canvas, button);
      break;
    case PadButton::kLeftTrigger:
      DrawPsTrigger(canvas, "L2");
      break;
    case PadButton::kRightTrigger:
      DrawPsTrigger(canvas, "R2");
      break;
    case PadButton::kLeftShoulder:
      DrawPsBumper(canvas, "L1");
      break;
    case PadButton::kRightShoulder:
      DrawPsBumper(canvas, "R1");
      break;
    case PadButton::kStart:
      DrawPsOptions(canvas);
      break;
    case PadButton::kBack:
      DrawPsTouchpad(canvas);
      break;
    case PadButton::kLeftStick:
      DrawPsStick(canvas, 'L', false);
      break;
    case PadButton::kRightStick:
      DrawPsStick(canvas, 'R', false);
      break;
    case PadButton::kLeftStickPress:
      DrawPsStick(canvas, 'L', true);
      break;
    case PadButton::kRightStickPress:
      DrawPsStick(canvas, 'R', true);
      break;
    case PadButton::kDpadAll:
      DrawPsDpad(canvas, true, true, true, true);
      break;
    case PadButton::kDpadUp:
      DrawPsDpad(canvas, true, false, false, false);
      break;
    case PadButton::kDpadDown:
      DrawPsDpad(canvas, false, true, false, false);
      break;
    case PadButton::kDpadLeft:
      DrawPsDpad(canvas, false, false, true, false);
      break;
    case PadButton::kDpadRight:
      DrawPsDpad(canvas, false, false, false, true);
      break;
    case PadButton::kDpadUpDown:
      DrawPsDpad(canvas, true, true, false, false);
      break;
    case PadButton::kDpadLeftRight:
      DrawPsDpad(canvas, false, false, true, true);
      break;
  }
  return canvas.ToRgba8();
}

std::vector<uint8_t> DrawPcGlyph(const PcPrompt& prompt, int width, int height, uint32_t seed) {
  Canvas canvas(width, height);
  switch (prompt.layout) {
    case PcPrompt::Layout::kSingle: {
      const KeyFace face = DescribeKey(prompt.keys.empty() ? "?" : prompt.keys[0]);
      if (face.mouse != MouseButton::kNone) {
        DrawOmenMouse(canvas, face.mouse, false, seed);
      } else {
        DrawOmenKey(canvas, face, seed);
      }
      break;
    }
    case PcPrompt::Layout::kMouseLook:
      DrawOmenMouse(canvas, MouseButton::kNone, true, seed);
      break;
    case PcPrompt::Layout::kCross:
      DrawCross(canvas, prompt.keys, seed);
      break;
    case PcPrompt::Layout::kPair:
      DrawPair(canvas, prompt.keys, seed);
      break;
  }
  return canvas.ToRgba8();
}

std::string PrimaryKeyName(std::string_view bind_value) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  std::string_view first = bind_value.substr(0, bind_value.find(','));
  first = trim(first);
  // "Shift+W" -> "W": the key is the last part; the parts before it are modifiers.
  const size_t plus = first.rfind('+');
  if (plus != std::string_view::npos && plus + 1 < first.size()) {
    first = trim(first.substr(plus + 1));
  }
  return std::string(first);
}

}  // namespace azura::prompts
