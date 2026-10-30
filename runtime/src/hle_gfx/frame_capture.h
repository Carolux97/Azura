// Frame captures for comparing the graphics paths (see frame_capture.cpp).

#pragma once

namespace rex::ui {
class Presenter;
}

namespace azura::hle_gfx {

// Starts saving the game's output periodically when AZURA_CAPTURE_DIR is set; otherwise does
// nothing.
void StartFrameCapture(rex::ui::Presenter* presenter);

}  // namespace azura::hle_gfx
