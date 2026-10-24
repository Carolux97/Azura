#include "achievement_export.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

REXCVAR_DEFINE_STRING(export_achievements, "", "Azura",
                      "Write the game's achievements (texts in every language and artwork) to this "
                      "folder and exit (used by the launcher)");

namespace azura {

namespace {

using rex::system::XLanguage;
using rex::system::util::XdbfGameData;
using rex::system::util::XdbfSection;

// The languages an XDBF can hold, with the code the launcher knows them by.
constexpr std::pair<XLanguage, const char*> kLanguages[] = {
    {XLanguage::kEnglish, "en"},     {XLanguage::kJapanese, "ja"},
    {XLanguage::kGerman, "de"},      {XLanguage::kFrench, "fr"},
    {XLanguage::kSpanish, "es"},     {XLanguage::kItalian, "it"},
    {XLanguage::kKorean, "ko"},      {XLanguage::kTChinese, "zh-Hant"},
    {XLanguage::kPortuguese, "pt"},  {XLanguage::kSChinese, "zh-Hans"},
    {XLanguage::kPolish, "pl"},      {XLanguage::kRussian, "ru"},
};

// A string as a JSON string literal. The XDBF strings are UTF-8, which JSON takes as it is.
std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out += char(c);
        }
        break;
    }
  }
  return out + "\"";
}

// Prints the one line the launcher reads, and returns the exit code that goes with it.
int Report(const std::string& json, int exit_code) {
  std::fputs((json + "\n").c_str(), stdout);
  std::fflush(stdout);
  return exit_code;
}

int Fail(const std::string& message) {
  return Report("{\"error\":" + JsonString(message) + "}", 1);
}

// Writes `size` bytes to `path`; false when the file cannot be written.
bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return bool(file);
}

}  // namespace

bool AchievementExportRequested() { return !REXCVAR_GET(export_achievements).empty(); }

int ExportAchievements(const std::filesystem::path& game_data_root) {
  const std::filesystem::path out_dir = REXCVAR_GET(export_achievements);
  // A folder, or a disc image (.iso), which the runtime mounts in place.
  if (game_data_root.empty() || !std::filesystem::exists(game_data_root)) {
    return Fail("game_data_root is neither a folder nor a disc image");
  }

  // The same headless set-up as the SDK's `rexglue init achievements`: tool mode starts the kernel
  // and the file system but no GPU, audio or input, which is all loading the executable needs.
  auto runtime = std::make_unique<rex::Runtime>(game_data_root);
  if (XFAILED(runtime->Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      }))) {
    return Fail("the runtime could not start");
  }
  if (XFAILED(runtime->LoadXexImage("game:\\default.xex"))) {
    return Fail("default.xex could not be loaded");
  }
  const XdbfGameData db = runtime->kernel_state()->title_xdbf();
  if (!db.is_valid()) {
    return Fail("default.xex has no achievement data");
  }

  // Only the languages the game has a string table for: the others would read as empty strings.
  std::vector<std::pair<XLanguage, const char*>> languages;
  for (const auto& language : kLanguages) {
    if (db.GetEntry(XdbfSection::kStringTable, static_cast<uint64_t>(language.first))) {
      languages.push_back(language);
    }
  }

  std::error_code error;
  std::filesystem::create_directories(out_dir / "icons", error);
  if (error) {
    return Fail("cannot create " + out_dir.string() + ": " + error.message());
  }

  // One object per text field, keyed by language: {"en":"...","es":"..."}.
  auto texts = [&](uint16_t string_id) {
    std::string json = "{";
    for (size_t i = 0; i < languages.size(); ++i) {
      json += (i ? "," : "") + std::string("\"") + languages[i].second +
              "\":" + JsonString(db.GetStringTableEntry(languages[i].first, string_id));
    }
    return json + "}";
  };

  const auto achievements = db.GetAchievements();
  std::string json = "{\"titleId\":\"";
  char title_id[9];
  std::snprintf(title_id, sizeof(title_id), "%08X", runtime->kernel_state()->title_id());
  json += std::string(title_id) + "\",\"languages\":[";
  for (size_t i = 0; i < languages.size(); ++i) {
    json += (i ? ",\"" : "\"") + std::string(languages[i].second) + "\"";
  }
  json += "],\"achievements\":[";
  for (size_t i = 0; i < achievements.size(); ++i) {
    const auto& entry = achievements[i];
    const uint32_t image_id = entry.image_id;
    // An achievement without artwork keeps imageId 0, and the launcher shows its generic medal.
    uint32_t written_image = 0;
    if (auto image = db.GetEntry(XdbfSection::kImage, image_id); image && image.size > 0) {
      if (WriteFile(out_dir / "icons" / (std::to_string(image_id) + ".png"), image.buffer,
                    image.size)) {
        written_image = image_id;
      }
    }
    json += (i ? "," : "") + std::string("{\"id\":") + std::to_string(uint16_t(entry.id)) +
            ",\"gamerscore\":" + std::to_string(uint16_t(entry.gamerscore)) +
            ",\"flags\":" + std::to_string(uint32_t(entry.flags)) +
            ",\"imageId\":" + std::to_string(written_image) +
            ",\"label\":" + texts(entry.label_id) +
            ",\"description\":" + texts(entry.description_id) +
            ",\"unachieved\":" + texts(entry.unachieved_id) + "}";
  }
  json += "]}\n";

  if (!WriteFile(out_dir / "achievements.json", json.data(), json.size())) {
    return Fail("cannot write " + (out_dir / "achievements.json").string());
  }
  return Report("{\"achievements\":" + std::to_string(achievements.size()) + "}", 0);
}

}  // namespace azura
