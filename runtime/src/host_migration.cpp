// Host migration (docs/reverse-engineering.md, "Host migration"): the one change to the engine's
// own migration code the online matches need. The rest (turning the migration on, starting it
// early, and what the new host gets back) is in script_hooks.cpp.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, host_migration);

extern "C" void __imp__sub_826E83B8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// WorldInfo's PeerHostMigration.HostMigrationProgress.
constexpr uint32_t kHostMigrationProgress = 1736;
constexpr uint8_t kMigratingAsHost = 2;
constexpr uint8_t kHostReadyToTravel = 5;
// Where the migration tick (sub_8257D1F0) sets MigratingAsHost once MigrateNewHost returns true.
constexpr uint32_t kTickSetsMigratingAsHost = 0x8257D358;

}  // namespace

// AWorldInfo: sets PeerHostMigration.HostMigrationProgress (r3 the WorldInfo, r4 the new state).
// The migration tick reads the state (FindingNewHost), runs MigrateNewHost, and then writes
// MigratingAsHost. When the new host's session has nothing to migrate first, MigrateNewHost
// already went on to PeerTravelAsHost, which set HostReadyToTravel; writing MigratingAsHost over it
// would leave the new host waiting until the migration times out, as only HostReadyToTravel counts
// the travel down. That one write is skipped.
extern "C" void sub_826E83B8(PPCContext& __restrict ctx, uint8_t* base) {
  if (REXCVAR_GET(host_migration) && base[ctx.r3.u32 + kHostMigrationProgress] == kHostReadyToTravel &&
      (ctx.r4.u32 & 0xFF) == kMigratingAsHost && static_cast<uint32_t>(ctx.lr) == kTickSetsMigratingAsHost) {
    REXLOG_INFO("[Azura online] Host migration: the new host keeps its travel countdown");
    return;
  }
  __imp__sub_826E83B8(ctx, base);
}
