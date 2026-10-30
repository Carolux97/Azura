// The notice shown when the game unlocks an achievement, in the style of the project: a dark
// translucent panel with a thin gold line in the bottom right corner of the 16:9 area the game image
// takes in the window, the achievement's icon on the left, "ACHIEVEMENT UNLOCKED" in the player's
// language above the achievement's own name and description, and its gamerscore on the right.
//
// It replaces the SDK's toast (ReXApp::CreateAchievementNotificationDialog). The SDK raises one
// event per unlock, from the guest thread that asked for it; this dialog queues them and shows them
// one at a time on the UI thread. The achievement's name and description come in the player's
// language already: the SDK reads them from the game's own data for the user_language setting.
// Fonts that cannot draw a text (a character outside Barlow and the Noto fonts merged behind it)
// make the dialog show the game's English text for that achievement instead.
//
// It draws on ImGui's foreground draw list, with no window, so it never takes input.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

struct ImFont;

namespace rex {
class Runtime;
}

namespace azura {

class AchievementToast : public rex::ui::AchievementNotificationDialog {
 public:
  // `language` is the Xbox 360 language ID of the user_language setting. The fonts may be null
  // (ImGui's default is used).
  AchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                   rex::Runtime* runtime, uint32_t language, ImFont* title_font,
                   ImFont* body_font);

  // Thread-safe: called from the guest thread that unlocks the achievement.
  void Push(const rex::system::AchievementEvent& event) override;

  // Only while a notice is on screen or queued (fading and sliding need every frame, and the gap
  // between two notices has to end); otherwise the window is repainted only for game frames.
  bool NeedsContinuousPaint() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  using Clock = std::chrono::steady_clock;

  // What the notice writes for one achievement, decided when it starts to show.
  struct Display {
    rex::system::AchievementEvent event;
    std::string heading;
    std::string name;
    std::string description;
    std::string score;
    Clock::time_point shown_since;
  };

  // Fills `display` from the event: the heading in the player's language and the achievement's
  // texts, in English when the fonts cannot draw them.
  void Prepare(Display& display);
  // The game's English name and description of an achievement, from its own data.
  bool EnglishTexts(uint32_t id, std::string& name, std::string& description) const;

  rex::Runtime* runtime_;
  uint32_t language_;
  ImFont* title_font_;
  ImFont* body_font_;
  rex::ui::AchievementIconCache icon_cache_;

  mutable std::mutex mutex_;
  std::deque<rex::system::AchievementEvent> queue_;
  // UI thread only: the notice on screen, and when the next one may start.
  std::optional<Display> current_;
  Clock::time_point next_start_;
};

}  // namespace azura
