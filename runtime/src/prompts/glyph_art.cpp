#include "glyph_art.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
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
constexpr Rgb kPsSymbolGrey{200, 206, 218};

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
// The DualSense's Create (and the DualShock 4's Share): three short rays bursting upwards.
const Bitmap& SymbolCreate() {
  static const Bitmap b = BitmapFromRows({"#..#..#", "#..#..#", ".#.#.#.", ".#.#.#.", "..###.."});
  return b;
}
const Bitmap& SymbolMove() {
  static const Bitmap b = BitmapFromRows({"..#..", ".###.", "#.#.#", "#####", "#.#.#", ".###.", "..#.."});
  return b;
}

// --- Key faces ----------------------------------------------------------------------------------

enum class MouseButton : uint8_t { kNone, kLeft, kRight, kMiddle, kBack, kForward, kWheelUp, kWheelDown };

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
      {"wheelup", MouseKey(MouseButton::kWheelUp)},
      {"wheeldown", MouseKey(MouseButton::kWheelDown)},
  };
  auto it = named.find(key);
  if (it != named.end()) {
    return it->second;
  }
  return TextKey("?", '?');
}

// --- PC style helpers ---------------------------------------------------------------------------

constexpr uint32_t kRaggedSeed = 11;

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
    Bitmap small_text = TextBitmap(text, true);
    if (!small_text.empty() && small_text.width <= max_w) {
      return small_text;
    }
    text.pop_back();
  }
  return TextBitmap("?", true);
}

// Draws a key's lit legend (or legend plus caption) centred on (cx, cy).
void DrawLegend(Canvas& canvas, const KeyFace& face, float cx, float cy, int max_w, int max_h) {
  const bool has_caption = !face.caption.empty();
  const Bitmap caption = has_caption ? TextBitmap(face.caption, true) : Bitmap{};
  const int legend_h = has_caption ? max_h - caption.height - 1 : max_h;
  // Letters and digits in the smooth legend font, as large as the key allows.
  if (!face.symbol && HasLegendFont()) {
    for (float cap = std::min(legend_h * 0.78f, 12.0f); cap >= 5.0f; cap -= 0.5f) {
      const Coverage text = TextCoverage(face.text, cap);
      if (text.empty() || text.width > max_w) continue;
      const float total_h = text.cap_rows + (has_caption ? caption.height + 2 : 0);
      const float top = cy - total_h / 2.0f;
      if (has_caption) {
        DrawBitmap(canvas, caption, cx, top + caption.height / 2.0f, 1, Solid(kAmberDeep));
      }
      // Centred by the capitals; the gradient runs over them too.
      const int x = static_cast<int>(std::lround(cx - text.width / 2.0f));
      const int cap_y = static_cast<int>(std::lround(top + (has_caption ? caption.height + 2 : 0)));
      DrawCoverage(canvas, text, x, cap_y - text.cap_top,
                   VerticalGradient(kAmberHot, kAmber, static_cast<float>(cap_y), static_cast<float>(cap_y + text.cap_rows)));
      return;
    }
  }
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
  DrawBitmap(canvas, legend, cx, legend_cy, scale, VerticalGradient(kAmberHot, kAmber, legend_cy - legend.height * scale / 2.0f, legend_cy + legend.height * scale / 2.0f), bold);
}

// Roughens a shape's edge like an ink stain: two octaves of noise added to its distance.
Sdf Ragged(Sdf shape, uint32_t seed, float amplitude) {
  return [shape = std::move(shape), seed, amplitude](float x, float y) {
    return shape(x, y) + amplitude * (ValueNoise(x, y, seed, 2.6f) * 0.7f +
                                      ValueNoise(x, y, seed + 7, 1.2f) * 0.3f);
  };
}

// A key of the Gears art: a black silhouette with a slightly ragged edge, rimmed by amber light,
// with a barely visible top. The rim is opaque and crisp rather than a soft glow: the game draws
// its text with a shadow from the glyph's alpha, which turns a glow into a smudge behind the key.
void FillKeyCap(Canvas& canvas, const Sdf& cap, float top, float bottom) {
  const Sdf stain = Ragged(cap, kRaggedSeed, 0.35f);
  canvas.Fill(Inset(stain, -1.1f), VerticalGradient(kAmber, kAmberDeep, top, bottom));
  canvas.Fill(stain, kOmenBlack);
  canvas.Fill(Inset(stain, 1.6f), kOmenFace);
}

// --- PC glyphs ----------------------------------------------------------------------------------

void DrawKey(Canvas& canvas, const KeyFace& face) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_w = w / 2.0f - 1.6f;
  const float key_h = h * (face.wide ? 0.70f : 0.80f);
  const float top = (h - key_h) / 2.0f;
  const float cy = top + key_h / 2.0f;
  FillKeyCap(canvas, RoundedRect(w / 2, cy, half_w, key_h / 2, 3.0f), top, top + key_h);
  const int max_w = static_cast<int>(2 * half_w - 3);
  const int max_h = static_cast<int>(key_h - 4);
  DrawLegend(canvas, face, w / 2, cy, max_w, max_h);
}

// The mouse: a black egg rimmed in amber, with seams of light. `lit` is the button the prompt is
// for; kNone with `look` set draws the movement arrows instead.
void DrawMouse(Canvas& canvas, MouseButton lit, bool look) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const bool side = lit == MouseButton::kBack || lit == MouseButton::kForward;
  // Leave room on the left for the side buttons when they are the prompt.
  const float cx = w / 2.0f + (side ? 1.2f : 0.0f);
  const float cy = h / 2.0f;
  const float half_w = std::min(w / 2.0f - 1.6f - (side ? 1.2f : 0.0f), h * 0.40f);
  const float half_h = h / 2.0f - 1.6f;
  Sdf egg = [=](float x, float y) {
    const float k = half_w * (0.86f + 0.14f * std::clamp((y - (cy - half_h)) / (2 * half_h), 0.0f, 1.0f));
    const float m = std::min(k, half_h);
    return std::hypot((x - cx) / k, (y - cy) / half_h) * m - m;
  };
  FillKeyCap(canvas, egg, cy - half_h, cy + half_h);

  const float split_y = cy - half_h * 0.02f;
  if (lit == MouseButton::kLeft || lit == MouseButton::kRight) {
    const bool left = lit == MouseButton::kLeft;
    canvas.Fill(
        [=](float x, float y) {
          const float side_d = left ? x - (cx - 0.7f) : (cx + 0.7f) - x;
          return std::max({egg(x, y) + 0.4f, y - split_y + 0.6f, side_d});
        },
        VerticalGradient(kAmberHot, kAmber, cy - half_h, split_y));
  }
  // The seams between the buttons and down the middle.
  canvas.Fill([=](float x, float y) { return std::max(egg(x, y) + 0.4f, std::abs(y - split_y) - 0.5f); },
              kAmberDeep);
  canvas.Fill(
      [=](float x, float y) {
        return std::max({egg(x, y) + 0.4f, y - split_y, std::abs(x - cx) - 0.5f});
      },
      kAmberDeep);
  // Wheel.
  Sdf wheel = RoundedRect(cx, cy - half_h * 0.50f, 1.3f, half_h * 0.20f, 1.2f);
  canvas.Fill(Inset(wheel, -0.8f), kOmenBlack);
  const bool wheel_lit = lit == MouseButton::kMiddle || lit == MouseButton::kWheelUp || lit == MouseButton::kWheelDown;
  canvas.Fill(wheel, wheel_lit ? kAmberHot : kAmberDeep);
  // Turning the wheel: an arrow below it, up or down.
  if (lit == MouseButton::kWheelUp || lit == MouseButton::kWheelDown) {
    const Bitmap& arrow = lit == MouseButton::kWheelUp ? SymbolArrowUp() : SymbolArrowDown();
    DrawBitmap(canvas, arrow, cx, cy + half_h * 0.40f, 1, VerticalGradient(kAmberHot, kAmber, 0, static_cast<float>(canvas.height())));
  }
  // Side buttons, on the thumb side: forward above, back below.
  if (side) {
    const float sx = cx - half_w * 0.93f;
    const float fy = cy - half_h * 0.02f + 2.0f;
    const float by = fy + 4.2f;
    canvas.Fill(Inset(RoundedRect(sx, fy, 1.5f, 1.7f, 0.8f), -0.8f), kOmenBlack);
    canvas.Fill(Inset(RoundedRect(sx, by, 1.5f, 1.7f, 0.8f), -0.8f), kOmenBlack);
    canvas.Fill(RoundedRect(sx, fy, 1.5f, 1.7f, 0.8f), lit == MouseButton::kForward ? kAmberHot : kAmberDeep);
    canvas.Fill(RoundedRect(sx, by, 1.5f, 1.7f, 0.8f), lit == MouseButton::kBack ? kAmberHot : kAmberDeep);
  }
  if (look) {
    DrawBitmap(canvas, SymbolMove(), cx, cy + half_h * 0.45f, 1, VerticalGradient(kAmberHot, kAmber, 0, static_cast<float>(canvas.height())));
  }
}

// The direction an arrow key points in (0 up, 1 down, 2 left, 3 right), or -1 for any other key.
int ArrowDirection(const KeyFace& face) {
  if (face.symbol == &SymbolArrowUp()) return 0;
  if (face.symbol == &SymbolArrowDown()) return 1;
  if (face.symbol == &SymbolArrowLeft()) return 2;
  if (face.symbol == &SymbolArrowRight()) return 3;
  return -1;
}

// A filled triangle pointing in `direction` (as ArrowDirection), `half` pixels from its centre to
// its tip: negative inside, the largest of its edges' signed distances.
Sdf Arrowhead(float cx, float cy, float half, int direction) {
  const float dx[4] = {0, 0, -1, 1};
  const float dy[4] = {-1, 1, 0, 0};
  const float fx = dx[direction], fy = dy[direction];  // towards the tip
  const float px = -fy, py = fx;                       // across
  const float tip_x = cx + fx * half, tip_y = cy + fy * half;
  const float back = half * 0.8f;
  const float b1x = cx - fx * back + px * half, b1y = cy - fy * back + py * half;
  const float b2x = cx - fx * back - px * half, b2y = cy - fy * back - py * half;
  // Signed distance to each edge, negative on the triangle's side: the vertices' turn says which
  // side of the edges (taken in order) that is.
  const float turn = (b1x - tip_x) * (b2y - tip_y) - (b1y - tip_y) * (b2x - tip_x);
  const float inward = turn > 0 ? -1.0f : 1.0f;
  auto edge = [inward](float ax, float ay, float bx, float by, float x, float y) {
    const float ex = bx - ax, ey = by - ay;
    return inward * (ex * (y - ay) - ey * (x - ax)) / std::hypot(ex, ey);
  };
  return [=](float x, float y) {
    return std::max({edge(tip_x, tip_y, b1x, b1y, x, y), edge(b1x, b1y, b2x, b2y, x, y),
                     edge(b2x, b2y, tip_x, tip_y, x, y)});
  };
}

// A small key for the WASD-style groups: a key with one 3x5 character, or for an arrow key an
// arrowhead, the same size whichever way it points.
void DrawMiniKey(Canvas& canvas, float x0, float y0, float w, float h, const KeyFace& face) {
  const float cx = x0 + w / 2;
  const float cy = y0 + h / 2;
  FillKeyCap(canvas, RoundedRect(cx, cy, w / 2 - 1.0f, h / 2 - 1.0f, 1.2f), y0, y0 + h);
  if (const int direction = ArrowDirection(face); direction >= 0) {
    const float half = std::max(1.5f, std::min(w, h) / 2 - 1.4f);
    canvas.Fill(Arrowhead(cx, cy, half, direction), VerticalGradient(kAmberHot, kAmber, cy - half, cy + half));
    return;
  }
  DrawBitmap(canvas, TextBitmap(std::string(1, face.mini), true), cx, cy, 1, VerticalGradient(kAmberHot, kAmber, 0, static_cast<float>(canvas.height())));
}

// One legend of a group drawn without a key of its own (CompactCross, CompactPair): the key's
// character in the smooth legend font, or in the 5x7 pixel font without it.
struct Legend {
  Bitmap bitmap;
  Coverage coverage;

  bool empty() const { return bitmap.empty() && coverage.empty(); }
  int width() const { return coverage.empty() ? bitmap.width : coverage.width; }
  // The capitals' height: what the group lines up and centres.
  int height() const { return coverage.empty() ? bitmap.height : coverage.cap_rows; }
  // Draws it with the top left of its capitals at whole pixel (x, y).
  void Draw(Canvas& canvas, int x, int y, const ColorFn& color) const {
    if (!coverage.empty()) {
      DrawCoverage(canvas, coverage, x, y - coverage.cap_top, color);
    } else {
      DrawBitmap(canvas, bitmap, x + bitmap.width / 2.0f, y + bitmap.height / 2.0f, 1, color);
    }
  }
};

// The legend of a group's key with capitals `cap_height` tall. Empty when the key has no single
// character to show (a mouse button, a symbol).
Legend GroupLegend(const KeyFace& face, float cap_height) {
  Legend legend;
  if (face.mouse != MouseButton::kNone || face.symbol) return legend;
  const std::string text = face.text.size() == 1 ? face.text : std::string(1, face.mini);
  if (HasLegendFont()) {
    legend.coverage = TextCoverage(text, cap_height);
  } else {
    legend.bitmap = TextBitmap(text, false);
  }
  return legend;
}

// The legends of `keys` with the largest capitals, from `max_cap` down, that pass `fits`; empty
// when a key has no single character or none fits.
std::vector<Legend> FitGroup(const std::vector<std::string>& keys, size_t count, float max_cap,
                             const std::function<bool(const std::vector<Legend>&)>& fits) {
  for (float cap = max_cap; cap >= 5.0f; cap -= 0.5f) {
    std::vector<Legend> legends;
    for (size_t i = 0; i < count; ++i) {
      Legend l = GroupLegend(DescribeKey(i < keys.size() ? keys[i] : "?"), cap);
      if (l.empty()) return {};
      legends.push_back(std::move(l));
    }
    if (fits(legends)) return legends;
    if (!HasLegendFont()) break;  // the pixel font has one size
  }
  return {};
}

// Draws `legends` left to right with `gap` pixels between them, on whole pixels, centred on
// (cx, cy). Returns the left edge of each.
std::vector<int> DrawLegendRow(Canvas& canvas, const std::vector<Legend>& legends, int gap, float cx, float cy) {
  int total = gap * (static_cast<int>(legends.size()) - 1);
  int height = 0;
  for (const Legend& l : legends) {
    total += l.width();
    height = std::max(height, l.height());
  }
  int x = static_cast<int>(std::lround(cx - total / 2.0f));
  const int top = static_cast<int>(std::lround(cy - height / 2.0f));
  const ColorFn lit = VerticalGradient(kAmberHot, kAmber, static_cast<float>(top), static_cast<float>(top + height));
  std::vector<int> lefts;
  for (const Legend& l : legends) {
    l.Draw(canvas, x, top + height - l.height(), lit);
    lefts.push_back(x);
    x += l.width() + gap;
  }
  return lefts;
}

// A WASD-style group in a narrow slot, where a key each would leave a legend of three pixels: two
// keys as on a keyboard, up alone on top and a wide one below with the other three characters,
// each with its own rim. False when they do not fit (the caller draws a key each).
bool CompactCross(Canvas& canvas, const std::vector<std::string>& keys) {
  const int w = canvas.width();
  const int h = canvas.height();
  constexpr float kPad = 2.0f;  // from a key's characters to its rim, vertically
  constexpr float kKeysGap = 2.0f;
  const float bottom_half_w = w / 2.0f - 1.2f;
  // keys = {up, left, down, right}: each one character to show.
  std::string chars;
  for (int i = 0; i < 4; ++i) {
    const KeyFace face = DescribeKey(i < static_cast<int>(keys.size()) ? keys[i] : "?");
    if (face.mouse != MouseButton::kNone || face.symbol) return false;
    chars += face.text.size() == 1 ? face.text : std::string(1, face.mini);
  }
  // The bottom row decides the size, set as one word so the font spaces it: it has to fit the
  // slot's width. Up is a size larger, alone on its key. Without the font, the 5x7 pixel font.
  Legend up, bottom;
  for (float cap = 7.0f; cap >= 4.5f; cap -= 0.5f) {
    if (HasLegendFont()) {
      bottom.coverage = TextCoverage(chars.substr(1), cap);
      up.coverage = TextCoverage(chars.substr(0, 1), cap + 1.0f);
    } else {
      bottom.bitmap = TextBitmap(chars.substr(1), false);
      up.bitmap = TextBitmap(chars.substr(0, 1), false);
    }
    // The antialiased fringe of the outer letters may touch the key's black edge, not its rim.
    const bool fits = !bottom.empty() && !up.empty() && bottom.width() <= w - 1 &&
                      up.height() + bottom.height() + 4 * kPad + kKeysGap + 2 <= h;
    if (fits) break;
    if (!HasLegendFont()) return false;  // the pixel font has one size
    bottom = {};
  }
  if (bottom.empty()) return false;
  const float top_key_h = up.height() + 2 * kPad;
  const float bottom_key_h = bottom.height() + 2 * kPad;
  const float top = (h - (top_key_h + kKeysGap + bottom_key_h)) / 2.0f;
  const float cx = w / 2.0f;
  const float top_cy = top + top_key_h / 2.0f;
  const float bottom_cy = top + top_key_h + kKeysGap + bottom_key_h / 2.0f;
  FillKeyCap(canvas, RoundedRect(cx, top_cy, up.width() / 2.0f + 1.8f, top_key_h / 2.0f, 2.0f), top, top + top_key_h);
  FillKeyCap(canvas, RoundedRect(cx, bottom_cy, bottom_half_w, bottom_key_h / 2.0f, 2.0f), bottom_cy - bottom_key_h / 2.0f,
             bottom_cy + bottom_key_h / 2.0f);
  DrawLegendRow(canvas, {up}, 0, cx, top_cy);
  DrawLegendRow(canvas, {bottom}, 0, cx, bottom_cy);
  return true;
}

void DrawCross(Canvas& canvas, const std::vector<std::string>& keys) {
  if (CompactCross(canvas, keys)) return;
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cell_w = std::floor(w / 3.0f);
  const float cell_h = std::min(cell_w + 2.0f, std::floor((h - 2.0f) / 2.0f));
  const float x0 = (w - 3 * cell_w) / 2.0f;
  const float y0 = std::floor((h - (2 * cell_h + 1)) / 2.0f);
  // keys = {up, left, down, right}
  const float cells[4][2] = {{x0 + cell_w, y0}, {x0, y0 + cell_h + 1}, {x0 + cell_w, y0 + cell_h + 1},
                             {x0 + 2 * cell_w, y0 + cell_h + 1}};
  for (int i = 0; i < 4; ++i) {
    const KeyFace face = DescribeKey(i < static_cast<int>(keys.size()) ? keys[i] : "?");
    DrawMiniKey(canvas, cells[i][0], cells[i][1], cell_w, cell_h, face);
  }
}

// Two keys in a narrow slot: one key with both characters and a dim slash between them, as two
// keys of one prompt. False when they do not fit (the caller draws a key each).
bool CompactPair(Canvas& canvas, const std::vector<std::string>& keys) {
  const int w = canvas.width();
  const int h = canvas.height();
  constexpr int kGap = 1;
  const std::vector<Legend> legends = FitGroup(keys, 2, 10.0f, [&](const std::vector<Legend>& l) {
    const int slash_w = std::max(3, l[0].height() / 3);
    return l[0].width() + slash_w + l[1].width() + 2 * kGap + 6 <= w && std::max(l[0].height(), l[1].height()) + 6 <= h;
  });
  if (legends.empty()) return false;
  const int row_h = std::max(legends[0].height(), legends[1].height());
  const int slash_w = std::max(3, row_h / 3);
  const int total = legends[0].width() + slash_w + legends[1].width() + 2 * kGap;
  const float cx = w / 2.0f;
  const float cy = h / 2.0f;
  const float half_h = row_h / 2.0f + 2.0f;
  FillKeyCap(canvas, RoundedRect(cx, cy, total / 2.0f + 2.0f, half_h, 2.5f), cy - half_h, cy + half_h);
  const int left = static_cast<int>(std::lround(cx - total / 2.0f));
  const int top = static_cast<int>(std::lround(cy - row_h / 2.0f));
  const ColorFn lit = VerticalGradient(kAmberHot, kAmber, static_cast<float>(top), static_cast<float>(top + row_h));
  legends[0].Draw(canvas, left, top + row_h - legends[0].height(), lit);
  legends[1].Draw(canvas, left + legends[0].width() + slash_w + 2 * kGap, top + row_h - legends[1].height(), lit);
  // The slash dimmer than the legends, so it reads as a separator and not as a key.
  const float sx = left + legends[0].width() + kGap + slash_w / 2.0f;
  canvas.Fill(Segment(sx + slash_w / 2.0f - 0.5f, top + 0.5f, sx - slash_w / 2.0f + 0.5f, top + row_h - 0.5f, 0.55f),
              kAmberDeep);
  return true;
}

void DrawPair(Canvas& canvas, const std::vector<std::string>& keys) {
  if (CompactPair(canvas, keys)) return;
  const int w = canvas.width();
  const int h = canvas.height();
  const int half = w / 2;
  for (int i = 0; i < 2; ++i) {
    Canvas part(half, h);
    const KeyFace face = DescribeKey(i < static_cast<int>(keys.size()) ? keys[i] : "?");
    if (face.mouse != MouseButton::kNone) {
      DrawMouse(part, face.mouse, false);
    } else {
      DrawKey(part, face);
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

// A PlayStation label in the smooth legend font: white with a dark shadow, its capitals centred on
// (cx, cy), as large as fits. False without the font.
bool PsSmoothLabel(Canvas& canvas, std::string_view label, float cx, float cy, int max_w, int max_h) {
  if (!HasLegendFont()) return false;
  for (float cap = std::min(max_h * 0.9f, 12.0f); cap >= 5.0f; cap -= 0.5f) {
    const Coverage text = TextCoverage(label, cap);
    if (text.empty() || text.width > max_w) continue;
    const int x = static_cast<int>(std::lround(cx - text.width / 2.0f));
    const int y = static_cast<int>(std::lround(cy - text.cap_rows / 2.0f)) - text.cap_top;
    DrawCoverage(canvas, text, x + 1, y + 1, Solid(kInk));
    DrawCoverage(canvas, text, x, y, Solid(kWhite));
    return true;
  }
  return false;
}

void PsLabel(Canvas& canvas, std::string_view label, float cx, float cy, int max_w, int max_h) {
  if (PsSmoothLabel(canvas, label, cx, cy, max_w, max_h)) return;
  Bitmap rows = TextBitmap(label, false);
  if (rows.empty() || rows.width > max_w) {
    rows = TextBitmap(label, true);
  }
  const int scale = FitScale(rows, max_w, max_h);
  DrawBitmap(canvas, rows, cx, cy, scale, Solid(kWhite), scale == 1 && rows.width + 1 <= max_w, &kInk);
}

void DrawPsFace(Canvas& canvas, PadButton button, PsModel model) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float cx = w / 2;
  const float cy = h / 2;
  const float r = std::min(w, h) / 2 - 0.6f;
  canvas.Fill(Circle(cx, cy, r), VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - r, cy + r));
  canvas.Fill(Ring(cx, cy, r - 0.6f, 0.6f), kPsHole);
  const float s = r * 0.50f;                    // symbol half-size
  const float t = std::max(1.05f, r * 0.13f);  // stroke half-width
  const bool grey = model == PsModel::kDualSense;
  switch (button) {
    case PadButton::kA:
      canvas.Fill(Union({Segment(cx - s, cy - s, cx + s, cy + s, t), Segment(cx + s, cy - s, cx - s, cy + s, t)}),
                  grey ? kPsSymbolGrey : kPsCross);
      break;
    case PadButton::kB:
      canvas.Fill(Ring(cx, cy, s * 0.98f, t), grey ? kPsSymbolGrey : kPsCircle);
      break;
    case PadButton::kX: {
      const float q = s * 0.86f;
      canvas.Fill(Shell(RoundedRect(cx, cy, q, q, 0.8f), t), grey ? kPsSymbolGrey : kPsSquare);
      break;
    }
    default: {
      const float a = s * 1.1f;
      const float x1 = cx, y1 = cy - a * 0.95f, x2 = cx + a, y2 = cy + a * 0.72f, x3 = cx - a, y3 = y2;
      canvas.Fill(Union({Segment(x1, y1, x2, y2, t), Segment(x2, y2, x3, y3, t), Segment(x3, y3, x1, y1, t)}),
                  grey ? kPsSymbolGrey : kPsTriangle);
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
  if (!PsSmoothLabel(canvas, std::string(1, side), cx, cy, static_cast<int>(cap * 1.4f), static_cast<int>(cap * 1.1f))) {
    DrawBitmap(canvas, TextBitmap(std::string(1, side), false), cx, cy, 1, Solid(kWhite), true, &kInk);
  }
}

// Options (Start) and Create or Share (Back): small pills with their symbols, as on the controller.
void DrawPsMenuButton(Canvas& canvas, const Bitmap& symbol) {
  const float w = static_cast<float>(canvas.width());
  const float h = static_cast<float>(canvas.height());
  const float half_h = h * 0.30f;
  const float cy = h / 2;
  Sdf pill = RoundedRect(w / 2, cy, w / 2 - 0.6f, half_h, half_h * 0.6f);
  canvas.Fill(pill, VerticalGradient(kPsBodyTop, kPsBodyBottom, cy - half_h, cy + half_h));
  canvas.Fill(Shell(Inset(pill, 0.6f), 0.6f), kPsRim);
  DrawBitmap(canvas, symbol, w / 2, cy, 1, Solid(kWhite), false, &kInk);
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

std::vector<uint8_t> DrawPlayStationGlyph(PadButton button, int width, int height, PsModel model) {
  Canvas canvas(width, height);
  switch (button) {
    case PadButton::kA:
    case PadButton::kB:
    case PadButton::kX:
    case PadButton::kY:
      DrawPsFace(canvas, button, model);
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
      DrawPsMenuButton(canvas, SymbolOptions());
      break;
    case PadButton::kBack:
      DrawPsMenuButton(canvas, SymbolCreate());
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

std::vector<uint8_t> DrawPcGlyph(const PcPrompt& prompt, int width, int height) {
  Canvas canvas(width, height);
  switch (prompt.layout) {
    case PcPrompt::Layout::kSingle: {
      const KeyFace face = DescribeKey(prompt.keys.empty() ? "?" : prompt.keys[0]);
      if (face.mouse != MouseButton::kNone) {
        DrawMouse(canvas, face.mouse, false);
      } else {
        DrawKey(canvas, face);
      }
      break;
    }
    case PcPrompt::Layout::kMouseLook:
      DrawMouse(canvas, MouseButton::kNone, true);
      break;
    case PcPrompt::Layout::kCross:
      DrawCross(canvas, prompt.keys);
      break;
    case PcPrompt::Layout::kPair:
      DrawPair(canvas, prompt.keys);
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
