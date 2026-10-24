// "Disable Ambient Occlusion", the Xenia Canary community fix for Gears of War 3 (game-patches
// repository, "4D5308AB - Gears of War 3.patch.toml"), applied on Linux.
//
// The fix replaces the load `lwz r11,1040(r10)` with `li r11,0` at two addresses, 0x82762C54 (in
// sub_82762C30) and 0x827C9628 (in sub_827C9610), which turns the game's ambient occlusion off.
// The manifest has a midasm hook right after each of those loads (after_instruction), and
// AzuraDisableAmbientOcclusion clears r11 there: the code that follows sees 0, as with the fix.
// The load itself still runs and reads what the game reads, so it is harmless.
//
// On macOS ambient occlusion stays as the game draws it.

#include <rex/ppc/context.h>

// Called with the guest's r11, just loaded at one of the two addresses.
void AzuraDisableAmbientOcclusion(PPCRegister& r11) {
#if defined(__linux__)
  r11.u64 = 0;
#else
  (void)r11;
#endif
}
