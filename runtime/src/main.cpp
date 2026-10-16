// Gears3-Azura runtime entry point.
//
// REX_DEFINE_APP registers AzuraApp as the application the SDK's SDL entry point creates. The
// generated init header (written by `rexglue codegen`) provides PPCImageConfig, the description of
// the recompiled game image that AzuraApp hands to the runtime.

#include "generated/default/azura_init.h"

#include "azura_app.h"

REX_DEFINE_APP(azura, AzuraApp::Create)
