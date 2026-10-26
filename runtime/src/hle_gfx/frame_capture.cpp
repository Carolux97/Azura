// Frame captures for comparing the graphics paths (docs/hle-graphics/02-design.md, phase 4): with
// the environment variable AZURA_CAPTURE_DIR set, a background thread saves the game's output
// image there every AZURA_CAPTURE_EVERY seconds (default 5), as binary PPM files named after the
// seconds since start. The images are the game's: keep them local (captures/ is ignored by git).

#include "hle_gfx/frame_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

namespace azura::hle_gfx {

namespace {

bool WritePpm(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) {
    return false;
  }
  std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
  std::string row(size_t(image.width) * 3, '\0');
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* source = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3 + 0] = char(source[x * 4 + 0]);
      row[x * 3 + 1] = char(source[x * 4 + 1]);
      row[x * 3 + 2] = char(source[x * 4 + 2]);
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  return true;
}

}  // namespace

void StartFrameCapture(rex::ui::Presenter* presenter) {
  const char* dir = std::getenv("AZURA_CAPTURE_DIR");
  if (!presenter || !dir || !*dir) {
    return;
  }
  const char* every = std::getenv("AZURA_CAPTURE_EVERY");
  const int every_seconds = std::max(1, every ? std::atoi(every) : 5);
  std::filesystem::path out_dir(dir);
  std::error_code error;
  std::filesystem::create_directories(out_dir, error);
  REXLOG_INFO("[Azura capture] Saving the game's output to {} every {} s", out_dir.string(),
              every_seconds);
  std::thread([presenter, out_dir, every_seconds] {
    for (int seconds = every_seconds;; seconds += every_seconds) {
      std::this_thread::sleep_for(std::chrono::seconds(every_seconds));
      rex::ui::RawImage image;
      if (presenter->CaptureGuestOutput(image) && image.width && image.height) {
        WritePpm(out_dir / fmt::format("frame_{:04d}.ppm", seconds), image);
      }
    }
  }).detach();
}

}  // namespace azura::hle_gfx
