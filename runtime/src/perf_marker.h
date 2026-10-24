// Performance markers: while perf_log_csv records, the player marks the frame where they noticed a
// hitch, and the mark lands in the CSV's markers column (and a line in the log), so the frame can
// be found afterwards (scripts/gpu_baseline.sh lists the marked frames with their cause).
//
// This registers the key bind_perf_marker (F8 by default). Controllers mark with their Back
// button (View, Create or Share) when the SDK setting perf_marker_button is on, which
// scripts/gpu_baseline.sh turns on for its sessions.

#pragma once

namespace azura {

class PerfMarker {
 public:
  // Registers the key.
  PerfMarker();
  // Removes the key.
  ~PerfMarker();

  PerfMarker(const PerfMarker&) = delete;
  PerfMarker& operator=(const PerfMarker&) = delete;
};

}  // namespace azura
