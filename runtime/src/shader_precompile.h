// Shader precompilation only: with the `shader_precompile_only` setting, the runtime starts as
// usual, so the GPU backend loads the shader storage of earlier runs (cache_root/shaders) and makes
// every stored shader and pipeline in the background, but the game itself stays suspended. The
// progress goes to standard output, one line per change, for the launcher:
//
//   AZURA_SHADERS <done> <total>
//   AZURA_SHADERS_DONE <total>
//
// and the runtime closes once nothing is left. The drivers keep what they compiled in their own
// caches (Metal's, Mesa's or NVIDIA's on Linux), so the game's own preload at its next start, and
// its frames while playing, no longer wait for that work. The launcher runs it before playing
// (launcher/src-tauri/src/runtime.rs, "prepare shaders").

#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace rex {
class Runtime;
namespace system {
class XThread;
}
}  // namespace rex

namespace azura {

class ShaderPrecompile {
 public:
  // Whether the setting asks for this mode.
  static bool Requested();

  // Suspends the game's main thread `thread` (just launched) and follows the background work of
  // `runtime`'s graphics system; `close` runs, from the watching thread, once it is done.
  ShaderPrecompile(rex::Runtime* runtime, rex::system::XThread* thread, std::function<void()> close);
  ~ShaderPrecompile();

  ShaderPrecompile(const ShaderPrecompile&) = delete;
  ShaderPrecompile& operator=(const ShaderPrecompile&) = delete;

 private:
  void Watch();

  rex::Runtime* runtime_;
  rex::system::XThread* thread_;
  std::function<void()> close_;
  std::atomic<bool> stop_{false};
  std::thread watcher_;
};

}  // namespace azura
