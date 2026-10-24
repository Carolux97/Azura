// Gears3-Azura runtime entry point.
//
// REX_DEFINE_APP registers AzuraApp as the application the SDK's SDL entry point creates.
// AzuraApp hands the runtime PPCImageConfig, the description of the recompiled game image, which
// the generated code defines (see azura_app.h).

#include "azura_app.h"

REX_DEFINE_APP(azura, AzuraApp::Create)
