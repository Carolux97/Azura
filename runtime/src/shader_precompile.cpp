#include "shader_precompile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(shader_precompile_only, false, "Azura/Graphics",
                    "Make the stored shaders and pipelines of earlier runs and close, without running the "
                    "game (the launcher's Prepare shaders)");

namespace azura {

namespace {

// How often the progress is read, and how long to wait for the storage to queue anything before
// taking it as empty (the backends queue it from their own threads).
constexpr auto kPoll = std::chrono::milliseconds(200);
constexpr auto kNothingQueued = std::chrono::seconds(8);

}  // namespace

bool ShaderPrecompile::Requested() { return REXCVAR_GET(shader_precompile_only); }

ShaderPrecompile::ShaderPrecompile(rex::Runtime* runtime, rex::system::XThread* thread, std::function<void()> close)
    : runtime_(runtime), thread_(thread), close_(std::move(close)) {
  // The main thread was created suspended and is resumed right after this: one more suspension
  // keeps the game from running while the backend works.
  if (thread_) {
    thread_->Suspend();
  }
  REXLOG_INFO("[Azura shaders] Preparing the stored shaders and pipelines, without running the game");
  watcher_ = std::thread([this] { Watch(); });
}

ShaderPrecompile::~ShaderPrecompile() {
  stop_.store(true);
  if (watcher_.joinable()) {
    watcher_.join();
  }
}

void ShaderPrecompile::Watch() {
  auto* graphics = runtime_ ? runtime_->graphics_system() : nullptr;
  const auto started = std::chrono::steady_clock::now();
  uint64_t last_done = UINT64_MAX;
  uint64_t last_queued = UINT64_MAX;
  rex::system::IGraphicsSystem::ShaderCompilation work;
  while (!stop_.load()) {
    std::this_thread::sleep_for(kPoll);
    work = graphics ? graphics->GetShaderCompilation() : rex::system::IGraphicsSystem::ShaderCompilation{};
    // The totals count from the runtime's start: the storage may have queued (and even finished)
    // its work before this thread first looked.
    if (work.total_done != last_done || work.total_queued != last_queued) {
      last_done = work.total_done;
      last_queued = work.total_queued;
      std::printf("AZURA_SHADERS %llu %llu\n", static_cast<unsigned long long>(work.total_done),
                  static_cast<unsigned long long>(work.total_queued));
      std::fflush(stdout);
    }
    // Done once everything queued is made, or when nothing was queued in time (an empty storage).
    const bool finished = work.total_queued && !work.queued;
    if (finished || (!work.total_queued && std::chrono::steady_clock::now() - started >= kNothingQueued)) {
      break;
    }
  }
  if (stop_.load()) {
    return;
  }
  std::printf("AZURA_SHADERS_DONE %llu\n", static_cast<unsigned long long>(work.total_done));
  std::fflush(stdout);
  REXLOG_INFO("[Azura shaders] {} pipelines prepared, {:.1f} s after the storage was loaded", work.total_done,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  // The game's thread runs again so the title shuts down normally.
  if (thread_) {
    thread_->Resume();
  }
  if (close_) {
    close_();
  }
}

}  // namespace azura
