// RAAM's Shadow's achievements on the disc build. See dlc_achievements.h.

#include "dlc_achievements.h"

#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/achievements.h>
#include <rex/system/flags.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

namespace azura::dlc {
namespace {

using rex::system::XLanguage;
using rex::system::util::XdbfGameData;
using rex::system::util::XdbfSection;

// Where the game keeps downloadable content in the saves folder: no profile, the game's title ID,
// Marketplace content.
const std::filesystem::path kContentDir = std::filesystem::path("0000000000000000") / "4D5308AB" / "00000002";
// A file only RAAM's Shadow has: its first chapter's level.
const std::filesystem::path kRaamsShadowMarker = std::filesystem::path("GearGame") / "CookedXbox360" / "SP_Bank_P.xxx";

constexpr int32_t kFirstChapter = 31;
constexpr int32_t kLastChapter = 35;
constexpr uint32_t kWelcomeToZeta = 56;            // the add-on in a standard campaign
constexpr uint32_t kSomeFriendlyCompetition = 57;  // the add-on in an arcade campaign
constexpr uint32_t kDeathFromAbove = 61;           // Hammer of Dawn kills from the satellite
constexpr int32_t kDeathFromAboveKills = 50;

// What the unlocks need, as kept in its file: one line per key, its values separated by spaces.
//   standard 31 32 ...   chapters completed in a standard campaign
//   arcade 31 ...        chapters completed in an arcade campaign
//   death_from_above 12  satellite kills
struct Progress {
  std::set<int32_t> standard, arcade;
  int32_t death_from_above = 0;
};

std::mutex g_mutex;
bool g_registered = false;  // RAAM's Shadow is installed and its achievements are registered
std::filesystem::path g_progress_path;
Progress g_progress;

void LoadProgress() {
  std::ifstream in(g_progress_path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key;
    words >> key;
    int32_t value = 0;
    while (words >> value) {
      if (key == "standard") g_progress.standard.insert(value);
      if (key == "arcade") g_progress.arcade.insert(value);
      if (key == "death_from_above") g_progress.death_from_above = value;
    }
  }
}

void SaveProgress() {
  std::error_code error;
  std::filesystem::create_directories(g_progress_path.parent_path(), error);
  std::ofstream out(g_progress_path, std::ios::trunc);
  auto chapters = [&](const char* key, const std::set<int32_t>& set) {
    out << key;
    for (int32_t chapter : set) out << ' ' << chapter;
    out << '\n';
  };
  chapters("standard", g_progress.standard);
  chapters("arcade", g_progress.arcade);
  out << "death_from_above " << g_progress.death_from_above << '\n';
  if (!out) REXLOG_WARN("[Azura achievements] cannot write {}", g_progress_path.string());
}

bool AllChapters(const std::set<int32_t>& chapters) {
  for (int32_t chapter = kFirstChapter; chapter <= kLastChapter; ++chapter) {
    if (!chapters.count(chapter)) return false;
  }
  return true;
}

void Unlock(uint32_t id) {
  if (rex::system::UnlockAchievement(id)) REXLOG_INFO("[Azura achievements] RAAM's Shadow achievement {} unlocked", id);
}

}  // namespace

void RegisterDlcAchievements(const std::filesystem::path& user_data_root, const std::filesystem::path& cache_root) {
  if (user_data_root.empty()) return;
  std::error_code error;
  std::filesystem::path package;
  for (const auto& entry : std::filesystem::directory_iterator(user_data_root / kContentDir, error)) {
    if (std::filesystem::is_regular_file(entry.path() / kRaamsShadowMarker, error)) {
      package = entry.path();
      break;
    }
  }
  if (package.empty()) return;

  std::ifstream in(package / "spa.bin", std::ios::binary);
  const std::vector<uint8_t> spa((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const XdbfGameData db(spa.empty() ? nullptr : spa.data(), spa.size());
  if (!db.is_valid()) {
    REXLOG_WARN("[Azura achievements] RAAM's Shadow has no readable achievement data ({})", (package / "spa.bin").string());
    return;
  }
  const XLanguage language = db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
  const std::filesystem::path icons = cache_root / "achievement_icons" / "raams-shadow";
  std::filesystem::create_directories(icons, error);
  int registered = 0;
  for (const auto& entry : db.GetAchievements()) {
    const uint32_t id = entry.id;
    if (id < kRaamsShadowFirstAchievement || id > kRaamsShadowLastAchievement) continue;
    rex::system::AchievementInfo info;
    info.id = id;
    info.label = db.GetStringTableEntry(language, entry.label_id);
    info.description = db.GetStringTableEntry(language, entry.description_id);
    info.unachieved_description = db.GetStringTableEntry(language, entry.unachieved_id);
    info.gamerscore = entry.gamerscore;
    info.flags = entry.flags;
    // The artwork is in the package's database, not the executable's: the notice reads it from a file.
    if (const auto image = db.GetEntry(XdbfSection::kImage, entry.image_id); image && image.size > 0) {
      const std::filesystem::path icon = icons / (std::to_string(uint32_t(entry.image_id)) + ".png");
      std::ofstream(icon, std::ios::binary | std::ios::trunc)
          .write(reinterpret_cast<const char*>(image.buffer), std::streamsize(image.size));
      info.icon_path = icon.string();
    }
    if (rex::system::RegisterAchievement(std::move(info))) ++registered;
  }

  std::lock_guard lock(g_mutex);
  g_registered = registered > 0;
  g_progress_path = user_data_root / "achievements" / "4D5308AB-raams-shadow.txt";
  LoadProgress();
  REXLOG_INFO("[Azura achievements] RAAM's Shadow: {} achievements registered", registered);
}

void OnChapterCompleted(int32_t chapter, bool arcade) {
  if (chapter < kFirstChapter || chapter > kLastChapter) return;
  std::lock_guard lock(g_mutex);
  if (!g_registered) return;
  std::set<int32_t>& chapters = arcade ? g_progress.arcade : g_progress.standard;
  if (!chapters.insert(chapter).second) return;
  SaveProgress();
  REXLOG_INFO("[Azura achievements] RAAM's Shadow chapter {} completed ({} campaign)", chapter,
              arcade ? "arcade" : "standard");
  if (AllChapters(chapters)) Unlock(arcade ? kSomeFriendlyCompetition : kWelcomeToZeta);
}

void OnAchievementProgress(uint32_t achievement, int32_t count) {
  if (achievement != kDeathFromAbove || count <= 0) return;
  std::lock_guard lock(g_mutex);
  if (!g_registered) return;
  g_progress.death_from_above += count;
  SaveProgress();
  if (g_progress.death_from_above >= kDeathFromAboveKills) Unlock(kDeathFromAbove);
}

}  // namespace azura::dlc
