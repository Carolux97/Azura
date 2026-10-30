// Frame planner of the HLE graphics path (docs/hle-graphics/02-design.md): receives the markers the
// D3D hooks write (hooks.cpp), on the command processor thread, in stream order.
//
// For now it checks the marker stream: it counts each event, logs the first markers in full and
// a summary every 600 frames, and warns when a clear or resolve end arrives without its start
// (which would mean markers are lost or reordered).

#include "hle_gfx/hle_gfx.h"

#include <array>
#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/graphics/hle/markers.h>
#include <rex/logging.h>

namespace azura::hle_gfx {

namespace {

using rex::graphics::hle::MarkerEvent;

constexpr uint32_t kEventCount = 9;
constexpr uint32_t kLoggedMarkers = 40;
constexpr uint32_t kSummaryEveryFrames = 600;

const char* EventName(uint32_t event) {
  switch (static_cast<MarkerEvent>(event)) {
    case MarkerEvent::kSetRenderTarget:
      return "SetRenderTarget";
    case MarkerEvent::kSetDepthStencil:
      return "SetDepthStencil";
    case MarkerEvent::kClearBegin:
      return "ClearBegin";
    case MarkerEvent::kClearEnd:
      return "ClearEnd";
    case MarkerEvent::kResolveBegin:
      return "ResolveBegin";
    case MarkerEvent::kResolveEnd:
      return "ResolveEnd";
    case MarkerEvent::kFrameBegin:
      return "FrameBegin";
    case MarkerEvent::kFrameEnd:
      return "FrameEnd";
  }
  return "Unknown";
}

// State of the command processor thread; only the marker handler touches it.
struct Planner {
  std::array<uint64_t, kEventCount> counts{};
  uint32_t logged = 0;
  bool in_clear = false;
  bool in_resolve = false;
  uint64_t unmatched = 0;
};

Planner g_planner;

void OnMarker(const uint32_t* words, uint32_t count, void* user) {
  Planner& planner = *static_cast<Planner*>(user);
  const uint32_t event = words[1];
  if (event < kEventCount) {
    ++planner.counts[event];
  }
  if (planner.logged < kLoggedMarkers) {
    ++planner.logged;
    std::string args;
    for (uint32_t i = 2; i < count; ++i) {
      args += fmt::format(" {:08X}", words[i]);
    }
    REXLOG_INFO("[Azura HLE] marker {}{}", EventName(event), args);
  }
  switch (static_cast<MarkerEvent>(event)) {
    case MarkerEvent::kClearBegin:
      planner.in_clear = true;
      break;
    case MarkerEvent::kClearEnd:
      planner.unmatched += !planner.in_clear;
      planner.in_clear = false;
      break;
    case MarkerEvent::kResolveBegin:
      planner.in_resolve = true;
      break;
    case MarkerEvent::kResolveEnd:
      planner.unmatched += !planner.in_resolve;
      planner.in_resolve = false;
      break;
    case MarkerEvent::kFrameEnd:
      if (count > 2 && words[2] % kSummaryEveryFrames == 0) {
        const auto& c = planner.counts;
        REXLOG_INFO(
            "[Azura HLE] frame {}: render targets {}, depth {}, clears {}/{}, resolves {}/{}, "
            "frames {}/{}, unmatched ends {}",
            words[2], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], planner.unmatched);
      }
      break;
    default:
      break;
  }
}

}  // namespace

void Install() {
  if (Enabled()) {
    rex::graphics::hle::SetMarkerHandler(OnMarker, &g_planner);
    REXLOG_INFO("[Azura HLE] HLE graphics path on (gfx = hle)");
  }
}

}  // namespace azura::hle_gfx
