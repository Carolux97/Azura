#include "platform_win.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/logging.h>

namespace azura {

namespace {

// Each overlay layer and the variable that disables it, as its manifest declares
// ("disable_environment"), with the value the manifest expects.
struct OverlayLayer {
  const char* layer;
  const char* variable;
};

constexpr OverlayLayer kCrashingLayers[] = {
    {"VK_LAYER_OW_OVERLAY", "DISABLE_VULKAN_OW_OVERLAY_LAYER"},
    {"VK_LAYER_OW_OBS_HOOK", "DISABLE_VULKAN_OW_OBS_CAPTURE"},
    {"VK_LAYER_RTSS", "DISABLE_RTSS_LAYER"},
};

// The layers this run disabled, for LogDisabledVulkanLayers.
std::vector<std::string> disabled_layers;

// The copy of TryCopy, in a call of its own: without -fasync-exceptions clang lets an __except see
// a hardware exception only when it is raised inside a called function.
__declspec(noinline) void CopyBytes(void* destination, const void* source, size_t size) {
  std::memcpy(destination, source, size);
}

}  // namespace

void DisableCrashingVulkanLayers() {
  for (const auto& overlay : kCrashingLayers) {
    if (std::getenv(overlay.variable)) {
      continue;
    }
    // _putenv_s sets the process environment too, which the Vulkan loader reads.
    if (_putenv_s(overlay.variable, "1") == 0) {
      disabled_layers.emplace_back(overlay.layer);
    }
  }
}

void LogDisabledVulkanLayers() {
  for (const auto& layer : disabled_layers) {
    REXLOG_INFO("[Azura] Vulkan overlay layer {} disabled: it closes the game (set its DISABLE_ variable "
                "to 0 to keep it)",
                layer);
  }
}

std::vector<ReadableRange> ReadableRanges(const void* base, size_t size) {
  std::vector<ReadableRange> ranges;
  const auto* start = static_cast<const char*>(base);
  const char* address = start;
  while (address < start + size) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
      break;
    }
    const char* region_end = static_cast<const char*>(info.BaseAddress) + info.RegionSize;
    const bool readable = info.State == MEM_COMMIT && info.Protect != 0 &&
                          !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (readable) {
      const size_t begin = static_cast<size_t>(address - start);
      const size_t end = std::min(static_cast<size_t>(region_end - start), size);
      // VirtualQuery reports neighbouring regions separately when only their attributes differ.
      if (!ranges.empty() && ranges.back().end == begin) {
        ranges.back().end = end;
      } else {
        ranges.push_back({begin, end});
      }
    }
    address = region_end;
  }
  return ranges;
}

bool IsReadable(const void* data, size_t size) {
  const auto ranges = ReadableRanges(data, size);
  return ranges.size() == 1 && ranges.front().begin == 0 && ranges.front().end == size;
}

bool TryCopy(void* destination, const void* source, size_t size) {
  __try {
    CopyBytes(destination, source, size);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

}  // namespace azura
