// Button prompts that follow the player's device and key bindings.
//
// Gears of War 3 draws its button prompts from seven Xbox 360 textures: the button font atlas
// (Xbox360_18pt), two HUD action icon atlases and the four HUD_Dir_Button* icons. ButtonPrompts
// runs a background thread that:
//   1. finds those textures in guest physical memory by fingerprint (a hash of four of their
//      blocks), and keeps a copy of the original blocks it is going to replace;
//   2. picks the prompt style: the original Xbox art, PlayStation glyphs, or keyboard and mouse
//      glyphs drawn for the keys currently bound (the `controller_icons` setting, "auto" by
//      default, follows the device the player last used);
//   3. draws every button slot in that style, compresses it to DXT5, writes it over the texture
//      and tells the GPU texture cache to upload it again.
// It redraws only when the style, a binding or a texture changes. Nothing from the game's files
// is stored in the source: textures are recognised by hash and the originals live only in memory.

#pragma once

#include <filesystem>
#include <memory>

namespace rex {
class Runtime;
}

namespace azura::prompts {

class ButtonPrompts {
 public:
  // Starts the background thread. `runtime` must outlive this object. `legend_font` is the
  // TrueType font the keyboard glyphs' letters and digits are drawn with (Barlow); without it they
  // use pixel fonts.
  ButtonPrompts(rex::Runtime* runtime, const std::filesystem::path& legend_font);
  // Stops and joins the thread. Textures keep the last prompts written.
  ~ButtonPrompts();

  ButtonPrompts(const ButtonPrompts&) = delete;
  ButtonPrompts& operator=(const ButtonPrompts&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azura::prompts
