#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include "platform_win.h"
#endif
#include "button_prompts.h"

#include <algorithm>
#include <utility>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/pad_remap.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "glyph_art.h"
#include "glyph_raster.h"
#include "input/player_assignment.h"
#include "script_hooks.h"
#include "xenos_dxt5.h"

REXCVAR_DEFINE_STRING(controller_icons, "auto", "Input",
                      "Button prompts: auto (follows the device last used), xbox, playstation, pc");

namespace azura::prompts {

namespace {

// --- The textures and their button slots --------------------------------------------------------

struct Slot {
  uint16_t u, v, w, h;  // pixel rectangle in the texture
  PadButton button;
};

struct AtlasDef {
  const char* name;
  uint32_t byte_size;  // bytes the texture occupies in memory, tiling padding included
  uint16_t width;
  uint16_t height;
  uint16_t pitch_blocks;  // row pitch of the tiled data, in 4x4 blocks
  // Fingerprint: FNV-1a 64 of the 16-byte blocks at these offsets of the original texture.
  std::array<uint32_t, 4> fingerprint_offsets;
  uint64_t fingerprint_hash;
  std::span<const Slot> slots;
  // HUD icons: the game packs other pieces (the dots of a "hold" prompt, tick marks) right against
  // its round buttons, inside the corners of their slots, so only the button's disc is ours there.
  bool round_buttons = false;
};

using B = PadButton;

// Button font used for prompts inside HUD text and menus (identical in every language package).
constexpr Slot kFontSlots[] = {
    {201, 55, 24, 26, B::kA},           {227, 55, 26, 26, B::kB},
    {196, 109, 24, 26, B::kX},          {222, 109, 24, 26, B::kY},
    {33, 136, 22, 26, B::kLeftTrigger}, {204, 190, 22, 26, B::kLeftTrigger},
    {57, 136, 22, 26, B::kRightTrigger}, {228, 190, 22, 26, B::kRightTrigger},
    {81, 136, 27, 26, B::kLeftShoulder}, {0, 217, 27, 26, B::kLeftShoulder},
    {110, 136, 27, 26, B::kRightShoulder}, {29, 217, 27, 26, B::kRightShoulder},
    {149, 55, 28, 26, B::kStart},       {75, 55, 28, 26, B::kStart},
    {45, 55, 28, 26, B::kBack},         {105, 55, 28, 26, B::kBack},
    {203, 82, 18, 26, B::kLeftStick},   {223, 82, 18, 26, B::kLeftStick},
    {0, 82, 18, 26, B::kRightStick},    {142, 163, 29, 26, B::kLeftStickPress},
    {80, 163, 29, 26, B::kRightStickPress}, {204, 163, 24, 26, B::kDpadAll},
    {216, 28, 20, 26, B::kDpadUp},      {230, 163, 24, 26, B::kDpadUp},
    {0, 55, 18, 26, B::kDpadDown},      {78, 190, 24, 26, B::kDpadDown},
    {20, 55, 23, 26, B::kDpadLeft},     {0, 190, 24, 26, B::kDpadLeft},
    {196, 28, 18, 26, B::kDpadRight},   {26, 190, 24, 26, B::kDpadRight},
    {156, 190, 24, 26, B::kDpadUpDown}, {180, 190, 23, 26, B::kDpadLeftRight},
};

constexpr Slot kActionIcons2Slots[] = {
    {99, 3, 26, 26, B::kB},           {100, 34, 24, 24, B::kB},
    {7, 210, 24, 24, B::kY},          {47, 210, 24, 24, B::kY},
    {4, 132, 28, 30, B::kLeftTrigger}, {53, 134, 25, 28, B::kRightTrigger},
    {6, 172, 24, 26, B::kLeftTrigger}, {52, 170, 28, 30, B::kRightTrigger},
};

// Context action icons (the stick icons there carry a "press" arrow drawn outside the slot).
constexpr Slot kWarfareSlots[] = {
    {180, 337, 24, 24, B::kY}, {394, 423, 24, 26, B::kY}, {217, 331, 25, 25, B::kA},
    {254, 332, 23, 24, B::kA}, {302, 317, 24, 24, B::kB}, {333, 317, 24, 24, B::kX},
    {379, 318, 24, 24, B::kX}, {429, 449, 29, 29, B::kRightStickPress},
    {466, 449, 29, 29, B::kRightStickPress},
};

constexpr Slot kDirASlots[] = {{0, 0, 32, 32, B::kA}};
constexpr Slot kDirBSlots[] = {{0, 0, 32, 32, B::kB}};
constexpr Slot kDirXSlots[] = {{0, 0, 32, 32, B::kX}};
constexpr Slot kDirYSlots[] = {{0, 0, 32, 32, B::kY}};

const AtlasDef kAtlases[] = {
    {"Xbox360_18pt", 65536, 256, 256, 64, {0x800, 0x5D00, 0x9540, 0xF7A0}, 0x4F275066BCC4C36Cull, kFontSlots},
    {"HUD_ActionIcons2", 65536, 256, 256, 64, {0xC0, 0x4CA0, 0xAE30, 0xFFF0}, 0xA7A6CDBA24A68498ull,
     kActionIcons2Slots, true},
    {"WarfareHUD_ActionIcons", 262144, 512, 512, 128, {0x8A0, 0x18AA0, 0x2B7F0, 0x3F7F0},
     0x104FCC5D6BCC684Eull, kWarfareSlots, true},
    {"HUD_Dir_ButtonA", 16384, 32, 32, 32, {0x110, 0x500, 0x1030, 0x1620}, 0xD1391F5D1B926434ull, kDirASlots},
    {"HUD_Dir_ButtonB", 16384, 32, 32, 32, {0x110, 0x430, 0x1020, 0x1620}, 0x3A3B7F982BE5036Full, kDirBSlots},
    {"HUD_Dir_ButtonX", 16384, 32, 32, 32, {0x110, 0x500, 0x1030, 0x1620}, 0x918B0905E5B3D6C7ull, kDirXSlots},
    {"HUD_Dir_ButtonY", 16384, 32, 32, 32, {0x110, 0x430, 0x1030, 0x1620}, 0xEF796ADEBC67F707ull, kDirYSlots},
};

constexpr uint32_t kPhysicalSize = 0x20000000;   // 512 MB of guest physical memory
constexpr uint32_t kPhysicalView = 0xA0000000;   // guest virtual view of all of it
constexpr uint32_t kTextureAlignment = 0x1000;   // textures start on 4 KB boundaries

uint64_t Fnv1a(const uint8_t* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

bool FingerprintMatches(const uint8_t* texture, const AtlasDef& atlas) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t offset : atlas.fingerprint_offsets) {
    hash = Fnv1a(texture + offset, 16, hash);
  }
  return hash == atlas.fingerprint_hash;
}

// --- Prompt style and bindings ------------------------------------------------------------------

enum class Style : uint8_t { kXbox, kPlayStation, kPc };

// The keybind settings a PC prompt can show, in a fixed order.
enum Bind : int {
  kBindA, kBindB, kBindX, kBindY, kBindLeftTrigger, kBindRightTrigger, kBindLeftShoulder,
  kBindRightShoulder, kBindStart, kBindBack, kBindLStickUp, kBindLStickDown, kBindLStickLeft,
  kBindLStickRight, kBindLStickPress, kBindRStickUp, kBindRStickDown, kBindRStickLeft,
  kBindRStickRight, kBindRStickPress, kBindDpadUp, kBindDpadDown, kBindDpadLeft, kBindDpadRight,
  kBindCount
};

constexpr const char* kBindCvars[kBindCount] = {
    "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_trigger",
    "keybind_right_trigger", "keybind_left_shoulder", "keybind_right_shoulder", "keybind_start",
    "keybind_back", "keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left",
    "keybind_lstick_right", "keybind_lstick_press", "keybind_rstick_up", "keybind_rstick_down",
    "keybind_rstick_left", "keybind_rstick_right", "keybind_rstick_press", "keybind_dpad_up",
    "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right",
};

struct Bindings {
  std::array<std::string, kBindCount> keys;  // primary key of each bind
  bool mouse_look = true;                    // the right stick comes from the mouse
};

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

PcPrompt PromptFor(PadButton button, const Bindings& b) {
  auto single = [&](Bind bind) { return PcPrompt{PcPrompt::Layout::kSingle, {b.keys[bind]}}; };
  switch (button) {
    case B::kA: return single(kBindA);
    case B::kB: return single(kBindB);
    case B::kX: return single(kBindX);
    case B::kY: return single(kBindY);
    case B::kLeftTrigger: return single(kBindLeftTrigger);
    case B::kRightTrigger: return single(kBindRightTrigger);
    case B::kLeftShoulder: return single(kBindLeftShoulder);
    case B::kRightShoulder: return single(kBindRightShoulder);
    case B::kStart: return single(kBindStart);
    case B::kBack: return single(kBindBack);
    case B::kLeftStick:
      return {PcPrompt::Layout::kCross,
              {b.keys[kBindLStickUp], b.keys[kBindLStickLeft], b.keys[kBindLStickDown], b.keys[kBindLStickRight]}};
    case B::kRightStick:
      if (b.mouse_look) return {PcPrompt::Layout::kMouseLook, {}};
      return {PcPrompt::Layout::kCross,
              {b.keys[kBindRStickUp], b.keys[kBindRStickLeft], b.keys[kBindRStickDown], b.keys[kBindRStickRight]}};
    case B::kLeftStickPress: return single(kBindLStickPress);
    case B::kRightStickPress: return single(kBindRStickPress);
    case B::kDpadAll:
      return {PcPrompt::Layout::kCross,
              {b.keys[kBindDpadUp], b.keys[kBindDpadLeft], b.keys[kBindDpadDown], b.keys[kBindDpadRight]}};
    case B::kDpadUp: return single(kBindDpadUp);
    case B::kDpadDown: return single(kBindDpadDown);
    case B::kDpadLeft: return single(kBindDpadLeft);
    case B::kDpadRight: return single(kBindDpadRight);
    case B::kDpadUpDown: return {PcPrompt::Layout::kPair, {b.keys[kBindDpadUp], b.keys[kBindDpadDown]}};
    case B::kDpadLeftRight: return {PcPrompt::Layout::kPair, {b.keys[kBindDpadLeft], b.keys[kBindDpadRight]}};
  }
  return single(kBindA);
}

// Buttons whose game art is a disc: the face buttons and the stick presses.
bool IsRound(PadButton button) {
  switch (button) {
    case B::kA:
    case B::kB:
    case B::kX:
    case B::kY:
    case B::kLeftStickPress:
    case B::kRightStickPress:
      return true;
    default:
      return false;
  }
}

// Alpha above which a pixel outside a round button's disc is a piece of the game's own art.
constexpr uint8_t kForeignAlpha = 16;
// How far past a round button's slot, in pixels, its disc and shadow are cleared.
constexpr float kDiscMargin = 1.5f;

// --- The player's controller layout ---------------------------------------------------------------

// The layout's control of a prompt's button; kCount for the sticks and the D-pad, which it does not
// move.
rex::input::PadControl ControlOf(PadButton button) {
  using C = rex::input::PadControl;
  switch (button) {
    case B::kA: return C::kA;
    case B::kB: return C::kB;
    case B::kX: return C::kX;
    case B::kY: return C::kY;
    case B::kLeftTrigger: return C::kLeftTrigger;
    case B::kRightTrigger: return C::kRightTrigger;
    case B::kLeftShoulder: return C::kLeftShoulder;
    case B::kRightShoulder: return C::kRightShoulder;
    case B::kStart: return C::kStart;
    case B::kBack: return C::kBack;
    case B::kLeftStickPress: return C::kLeftThumb;
    case B::kRightStickPress: return C::kRightThumb;
    default: return C::kCount;
  }
}

// The button a prompt for `button` shows with the player's own layout: the physical control that
// acts as it (rex/input/pad_remap.h). The button itself when nothing does, or with no layout.
PadButton PhysicalButton(PadButton button, const rex::input::PadLayout& layout) {
  const auto control = ControlOf(button);
  if (control == rex::input::PadControl::kCount) return button;
  for (size_t i = 0; i < rex::input::kPadControlCount; ++i) {
    if (layout[i] != control) continue;
    const auto physical = static_cast<rex::input::PadControl>(i);
    for (int b = 0; b <= static_cast<int>(B::kRightStickPress); ++b) {
      if (ControlOf(static_cast<PadButton>(b)) == physical) return static_cast<PadButton>(b);
    }
  }
  return button;
}

// --- Texture bookkeeping ------------------------------------------------------------------------

// One copy of a texture found in guest memory.
struct Instance {
  uint32_t physical = 0;
  std::vector<Block> original;  // the blocks we replace, as the game loaded them
  std::vector<Block> written;   // what we last wrote (empty: still the original)
  std::string written_key;      // style and bindings `written` was drawn for
};

struct AtlasState {
  const AtlasDef* def = nullptr;
  std::vector<uint32_t> block_offsets;  // byte offsets of every block a slot touches
  std::vector<std::pair<uint16_t, uint16_t>> block_coords;  // their (x, y) in blocks
  std::vector<Instance> instances;
};

}  // namespace

// --- Engine -------------------------------------------------------------------------------------

class ButtonPrompts::Impl {
 public:
  explicit Impl(rex::Runtime* runtime) : runtime_(runtime) {
    for (const auto& def : kAtlases) {
      AtlasState state;
      state.def = &def;
      // Every block under a slot, with one pixel of margin: the margin is cleared so no edge of
      // the original art survives around the new glyph.
      std::vector<bool> touched(static_cast<size_t>((def.width / 4) * (def.height / 4)), false);
      for (const Slot& s : def.slots) {
        const int x0 = std::max(0, s.u - 1) / 4, y0 = std::max(0, s.v - 1) / 4;
        const int x1 = std::min<int>(def.width - 1, s.u + s.w) / 4;
        const int y1 = std::min<int>(def.height - 1, s.v + s.h) / 4;
        for (int by = y0; by <= y1; ++by)
          for (int bx = x0; bx <= x1; ++bx) touched[static_cast<size_t>(by * (def.width / 4) + bx)] = true;
      }
      for (int by = 0; by < def.height / 4; ++by) {
        for (int bx = 0; bx < def.width / 4; ++bx) {
          if (touched[static_cast<size_t>(by * (def.width / 4) + bx)]) {
            state.block_coords.emplace_back(bx, by);
            state.block_offsets.push_back(TiledBlockOffset(bx, by, def.pitch_blocks));
          }
        }
      }
      atlases_.push_back(std::move(state));
    }
    thread_ = std::thread([this] { Run(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    using namespace std::chrono_literals;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      lock.unlock();
      Tick();
      lock.lock();
      wake_.wait_for(lock, 250ms, [this] { return stopping_; });
    }
  }

  Style CurrentStyle() const {
    const std::string setting = Lower(rex::cvar::GetFlagByName("controller_icons"));
    if (setting == "xbox") return Style::kXbox;
    if (setting == "playstation") return Style::kPlayStation;
    if (setting == "pc") return Style::kPc;
    // auto: follow the device the player last pressed something on.
    auto* input = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    const auto kind = input ? input->ActiveKind(0) : rex::input::ActiveDeviceKind::kNone;
    switch (kind) {
      case rex::input::ActiveDeviceKind::kKeyboardMouse:
        return Style::kPc;
      case rex::input::ActiveDeviceKind::kPlayStation:
        return Style::kPlayStation;
      case rex::input::ActiveDeviceKind::kNone:
        return rex::cvar::GetFlagByName("mnk_mode") == "true" ? Style::kPc : Style::kXbox;
      default:
        return Style::kXbox;
    }
  }

  // The model of player 1's PlayStation controller: a DualShock 4 when that is the one they have,
  // otherwise (a DualSense, or none) a DualSense.
  static PsModel CurrentPsModel() {
    const auto* assignment = azura::input::InstalledPlayerAssignment();
    if (!assignment) return PsModel::kDualSense;
    for (const auto& slot : assignment->Slots()) {
      if (slot.keyboard || slot.player != 0 ||
          slot.device.controller_subtype != rex::input::ControllerSubtype::kPlayStation) {
        continue;
      }
      std::string name = Lower(slot.device.name);
      if (name.find("dualsense") == std::string::npos) return PsModel::kDualShock4;
    }
    return PsModel::kDualSense;
  }

  static Bindings CurrentBindings() {
    Bindings b;
    for (int i = 0; i < kBindCount; ++i) {
      b.keys[i] = PrimaryKeyName(rex::cvar::GetFlagByName(kBindCvars[i]));
    }
    // Buttons with no key of their own still have the driver's fixed ones (kFixedBinds in the
    // SDK's keyboard and mouse driver): Escape is Start, the arrow keys the D-pad.
    constexpr std::pair<Bind, const char*> kFixed[] = {
        {kBindStart, "Return"}, {kBindB, "Backspace"},
        {kBindDpadUp, "Up"}, {kBindDpadDown, "Down"}, {kBindDpadLeft, "Left"}, {kBindDpadRight, "Right"},
    };
    for (const auto& [bind, key] : kFixed) {
      if (b.keys[bind].empty()) {
        b.keys[bind] = key;
      }
    }
    // In the menus Return accepts and Escape goes back (script_hooks.cpp turns them into A and B
    // there), so their prompts show those keys; on the title screen Return is Start; in play, the
    // actions' own keys.
    switch (azura::script::WhereInMenus()) {
      case azura::script::Menus::kTitleScreen:
        b.keys[kBindA] = "Return";
        b.keys[kBindStart] = "Return";
        break;
      case azura::script::Menus::kMenu:
        b.keys[kBindA] = "Return";
        b.keys[kBindB] = "Escape";
        b.keys[kBindStart] = "Return";
        break;
      case azura::script::Menus::kPlaying:
        break;
    }
    b.mouse_look = rex::cvar::GetFlagByName("mnk_mouse") != "false";
    return b;
  }

  void Tick() {
    auto* memory = runtime_->memory();
    uint8_t* membase = memory ? memory->physical_membase() : nullptr;
    if (!membase) {
      return;
    }

    const Style style = CurrentStyle();
    Bindings bindings;
    std::string key = std::to_string(static_cast<int>(style));
    // A controller's prompts follow the player's own layout in play; the menus keep the
    // controller's own buttons, as the layout applies only in play (rex/input/pad_remap.h).
    const bool playing = azura::script::WhereInMenus() == azura::script::Menus::kPlaying;
    const rex::input::PadLayout layout =
        playing ? rex::input::CurrentPadLayout() : rex::input::IdentityPadLayout();
    const bool remapped = layout != rex::input::IdentityPadLayout();
    if (style != Style::kPc && remapped) key += "|" + rex::input::FormatPadLayout(layout);
    if (style == Style::kPc) {
      bindings = CurrentBindings();
      for (const auto& k : bindings.keys) key += "|" + k;
      key += bindings.mouse_look ? "|look" : "|keys";
    }
    // Player 1's PlayStation controller: the DualSense's glyphs are grey, the DualShock 4's coloured.
    const PsModel ps_model = CurrentPsModel();
    if (style == Style::kPlayStation && ps_model == PsModel::kDualShock4) key += "|ds4";
    if (style != last_style_) {
      static constexpr const char* kNames[] = {"Xbox", "PlayStation", "keyboard and mouse"};
      REXLOG_INFO("[Azura prompts] Button prompts: {}", kNames[static_cast<int>(style)]);
      last_style_ = style;
    }

    Validate(membase);
    MaybeScan(membase);

    for (auto& atlas : atlases_) {
      if (atlas.instances.empty()) continue;
      std::vector<Block> blocks;  // drawn once per atlas, written to every copy
      for (auto& inst : atlas.instances) {
        if (inst.written_key == key) continue;
        if (blocks.empty()) {
          blocks = style == Style::kXbox && !remapped ? inst.original : Draw(atlas, inst.original, style, bindings, layout, ps_model);
        }
        Write(memory, membase, atlas, inst.physical, blocks);
        inst.written = blocks;
        inst.written_key = key;
      }
    }
  }

  // Drops copies that are gone, and takes a fresh original when the game reloaded one.
  void Validate(const uint8_t* membase) {
    for (auto& atlas : atlases_) {
      auto& list = atlas.instances;
      for (auto it = list.begin(); it != list.end();) {
        const uint8_t* tex = membase + it->physical;
#if defined(__linux__)
        unsigned char vec[1] = {0};
        if (mincore(const_cast<void*>(static_cast<const void*>(tex)), 4096, vec) != 0 || !(vec[0] & 1)) {
          it = list.erase(it);
          continue;
        }
#elif defined(_WIN32)
        // Memory the game freed may have gone back to Windows, and reading it would end the game.
        if (!azura::IsReadable(tex, atlas.def->byte_size)) {
          it = list.erase(it);
          continue;
        }
#endif
        if (!it->written.empty() && Matches(tex, atlas, it->written)) {
          ++it;
          continue;
        }
        if (FingerprintMatches(tex, *atlas.def)) {
          if (!it->written.empty()) {  // reloaded by the game: our blocks were overwritten
            it->original = Read(tex, atlas);
            it->written.clear();
            it->written_key.clear();
          }
          ++it;
          continue;
        }
        REXLOG_INFO("[Azura prompts] {} at 0x{:08X} is gone", atlas.def->name, it->physical);
        it = list.erase(it);
      }
    }
  }

  // Scans guest physical memory for the textures: often while some are missing, rarely otherwise
  // (the game may load another copy when a level changes).
  void MaybeScan(const uint8_t* membase) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const bool missing = std::any_of(atlases_.begin(), atlases_.end(),
                                     [](const AtlasState& a) { return a.instances.empty(); });
    const auto since = now - last_scan_;
    if (!(missing && since >= 2s) && since < 20s) {
      return;
    }
    last_scan_ = now;
#if defined(__linux__)
    constexpr size_t kPageCount = kPhysicalSize / kTextureAlignment;
    std::vector<unsigned char> mincore_vec(kPageCount, 0);
    const bool mincore_ok = (mincore(const_cast<void*>(static_cast<const void*>(membase)), kPhysicalSize, mincore_vec.data()) == 0);
#elif defined(_WIN32)
    // Windows commits guest memory as the game allocates it, and reading memory that is only
    // reserved ends the game: only textures that lie wholly in committed memory are looked at.
    const auto readable = azura::ReadableRanges(membase, kPhysicalSize);
    size_t range = 0;
#endif
    for (uint32_t page = 0; page < kPhysicalSize; page += kTextureAlignment) {
#if defined(__linux__)
      if (mincore_ok && !(mincore_vec[page / kTextureAlignment] & 1)) {
        continue;
      }
#elif defined(_WIN32)
      while (range < readable.size() && readable[range].end <= page) ++range;
      if (range == readable.size()) break;
      if (page < readable[range].begin) continue;
#endif
      for (auto& atlas : atlases_) {
        if (page + atlas.def->byte_size > kPhysicalSize) continue;
#if defined(_WIN32)
        if (page + atlas.def->byte_size > readable[range].end) continue;
#endif
        const uint8_t* tex = membase + page;
        if (!FingerprintMatches(tex, *atlas.def)) continue;
        auto& list = atlas.instances;
        if (std::any_of(list.begin(), list.end(), [&](const Instance& i) { return i.physical == page; })) continue;
        Instance inst;
        inst.physical = page;
        inst.original = Read(tex, atlas);
        list.push_back(std::move(inst));
        REXLOG_INFO("[Azura prompts] Found {} at physical 0x{:08X}", atlas.def->name, page);
      }
    }
  }

  static std::vector<Block> Read(const uint8_t* tex, const AtlasState& atlas) {
    std::vector<Block> blocks(atlas.block_offsets.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
      std::memcpy(blocks[i].data(), tex + atlas.block_offsets[i], 16);
    }
    return blocks;
  }

  static bool Matches(const uint8_t* tex, const AtlasState& atlas, const std::vector<Block>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (std::memcmp(tex + atlas.block_offsets[i], blocks[i].data(), 16) != 0) return false;
    }
    return true;
  }

  // Draws every slot of `atlas` in `style` over the original blocks and returns the new blocks.
  // The atlas with our glyphs: PlayStation's or the keyboard's drawn in every slot, or the game's
  // own Xbox glyphs moved between slots for the player's controller layout.
  static std::vector<Block> Draw(const AtlasState& atlas, const std::vector<Block>& original, Style style,
                                 const Bindings& bindings, const rex::input::PadLayout& layout, PsModel ps_model) {
    const AtlasDef& def = *atlas.def;
    const int w = def.width;
    const int h = def.height;
    std::vector<Rgba> image(static_cast<size_t>(w * h), Rgba{0, 0, 0, 0});
    for (size_t i = 0; i < original.size(); ++i) {
      const auto px = DecodeDxt5(original[i]);
      const auto [bx, by] = atlas.block_coords[i];
      for (int p = 0; p < 16; ++p) image[static_cast<size_t>((by * 4 + p / 4) * w + bx * 4 + p % 4)] = px[p];
    }
    if (style == Style::kXbox) {
      // Each slot gets the game's glyph of the button that acts as its button, scaled to the slot
      // when the two slots differ in size. A button with no slot in this atlas stays as it is.
      const std::vector<Rgba> game = image;
      for (const Slot& slot : def.slots) {
        const PadButton shown = PhysicalButton(slot.button, layout);
        if (shown == slot.button) continue;
        const auto from = std::find_if(def.slots.begin(), def.slots.end(), [&](const Slot& s) { return s.button == shown; });
        if (from == def.slots.end()) continue;
        for (int y = 0; y < slot.h; ++y)
          for (int x = 0; x < slot.w; ++x) {
            const int sx = from->u + x * from->w / slot.w;
            const int sy = from->v + y * from->h / slot.h;
            image[static_cast<size_t>((slot.v + y) * w + slot.u + x)] = game[static_cast<size_t>(sy * w + sx)];
          }
      }
    }
    for (size_t s = 0; s < def.slots.size() && style != Style::kXbox; ++s) {
      const Slot& slot = def.slots[s];
      // A round button among the HUD icons: the game's own pieces in the slot's corners, outside
      // the button's disc, stay as they are and our glyph is drawn around them.
      const bool round = def.round_buttons && IsRound(slot.button);
      // The button's disc with its shadow, which reaches a pixel or two past the slot.
      auto in_disc = [&](int x, int y) {
        const float dx = (x + 0.5f - (slot.u + slot.w / 2.0f)) / (slot.w / 2.0f + kDiscMargin);
        const float dy = (y + 0.5f - (slot.v + slot.h / 2.0f)) / (slot.h / 2.0f + kDiscMargin);
        return dx * dx + dy * dy <= 1.0f;
      };
      auto foreign = [&](int x, int y) {
        return round && !in_disc(x, y) && image[static_cast<size_t>(y * w + x)][3] > kForeignAlpha;
      };
      for (int y = std::max(0, slot.v - 1); y < std::min(h, slot.v + slot.h + 1); ++y)
        for (int x = std::max(0, slot.u - 1); x < std::min(w, slot.u + slot.w + 1); ++x) {
          if (!round || in_disc(x, y)) image[static_cast<size_t>(y * w + x)] = Rgba{0, 0, 0, 0};
        }
      const std::vector<uint8_t> glyph =
          style == Style::kPlayStation
              ? DrawPlayStationGlyph(PhysicalButton(slot.button, layout), slot.w, slot.h, ps_model)
              : DrawPcGlyph(PromptFor(slot.button, bindings), slot.w, slot.h);
      for (int y = 0; y < slot.h; ++y)
        for (int x = 0; x < slot.w; ++x) {
          if (foreign(slot.u + x, slot.v + y)) continue;
          const uint8_t* g = &glyph[static_cast<size_t>((y * slot.w + x) * 4)];
          image[static_cast<size_t>((slot.v + y) * w + slot.u + x)] = Rgba{g[0], g[1], g[2], g[3]};
        }
    }
    std::vector<Block> blocks(original.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
      const auto [bx, by] = atlas.block_coords[i];
      std::array<Rgba, 16> px;
      for (int p = 0; p < 16; ++p) px[p] = image[static_cast<size_t>((by * 4 + p / 4) * w + bx * 4 + p % 4)];
      blocks[i] = EncodeDxt5(px);
    }
    return blocks;
  }

  // Writes the blocks and makes the GPU texture cache drop its copy of the texture. The cache
  // write-protects the pages it uploaded from; the first notification lifts that, the second makes
  // sure an upload that slipped in between is redone with the new data.
  static void Write(rex::memory::Memory* memory, uint8_t* membase, const AtlasState& atlas, uint32_t physical,
                    const std::vector<Block>& blocks) {
    auto notify = [&] {
      memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             kPhysicalView + physical, atlas.def->byte_size, true, false, true);
    };
    notify();
    for (size_t i = 0; i < blocks.size(); ++i) {
      std::memcpy(membase + physical + atlas.block_offsets[i], blocks[i].data(), 16);
    }
    notify();
  }

  rex::Runtime* runtime_;
  std::vector<AtlasState> atlases_;
  Style last_style_ = Style::kXbox;
  std::chrono::steady_clock::time_point last_scan_{};

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

ButtonPrompts::ButtonPrompts(rex::Runtime* runtime, const std::filesystem::path& legend_font) {
  if (!LoadLegendFont(legend_font.string())) {
    REXLOG_WARN("[Azura prompts] {} could not be loaded; keyboard glyphs use pixel fonts", legend_font.string());
  }
  impl_ = std::make_unique<Impl>(runtime);
}

ButtonPrompts::~ButtonPrompts() = default;

}  // namespace azura::prompts
