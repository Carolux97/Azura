#include "perf_marker.h"

#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ui/keybinds.h>

namespace azura {

namespace {

constexpr const char* kBindName = "bind_perf_marker";

}  // namespace

PerfMarker::PerfMarker() {
  rex::ui::RegisterBind(kBindName, "F8", "Mark this frame in the perf_log_csv CSV", [] {
    rex::perf::IncrementCounter(rex::perf::CounterId::kMarkers);
    REXLOG_INFO("[Azura] Performance marker (key)");
  });
}

PerfMarker::~PerfMarker() { rex::ui::UnregisterBind(kBindName); }

}  // namespace azura
