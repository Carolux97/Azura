// Research into the engine's host migration (research_host_migration, docs/reverse-engineering.md,
// "Host migration"): logging around the native functions it goes through, found in the recompiled
// code. Each replaces the recompiled function's weak symbol, logs, and runs the original
// (__imp__). Temporary: it goes once the migration is understood.

#include <chrono>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, research_host_migration);

#define AZURA_ORIGINAL(name) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base)
AZURA_ORIGINAL(sub_826E83B8);
AZURA_ORIGINAL(sub_826E8260);
AZURA_ORIGINAL(sub_824A97A8);
AZURA_ORIGINAL(sub_82986FA0);
AZURA_ORIGINAL(sub_82987140);
AZURA_ORIGINAL(sub_82987248);
AZURA_ORIGINAL(sub_82B0DB28);
AZURA_ORIGINAL(sub_826E36C8);
AZURA_ORIGINAL(sub_8257D1F0);
AZURA_ORIGINAL(sub_829A83B8);
AZURA_ORIGINAL(sub_82451BD8);
AZURA_ORIGINAL(sub_82AA01A8);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// An FString's text (UTF-16BE), '?' for what is not ASCII.
std::string FString(const uint8_t* base, uint32_t address) {
  if (!address) return "(none)";
  const uint32_t data = Load32(base, address);
  const uint32_t count = Load32(base, address + 4);
  std::string text;
  for (uint32_t i = 0; data && i + 1 < count && i < 200; ++i) {
    const uint16_t w = static_cast<uint16_t>(base[data + i * 2] << 8 | base[data + i * 2 + 1]);
    text.push_back(w < 128 ? static_cast<char>(w) : '?');
  }
  return text;
}

// WorldInfo's PeerHostMigration: progress +1736, elapsed +1740, travel countdown +1744, URL +1748.
std::string MigrationState(const uint8_t* base, uint32_t world) {
  if (!world) return "(no WorldInfo)";
  return fmt::format("progress {}, elapsed {:.2f}, countdown {:.2f}, URL \"{}\"", base[world + 1736],
                     LoadFloat(base, world + 1740), LoadFloat(base, world + 1744), FString(base, world + 1748));
}

bool On() {
  return REXCVAR_GET(research_host_migration);
}

// A function that may run every frame: logged on its first call, then at most once a second.
bool Throttled(int64_t& last) {
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (last && now - last < 1'000'000'000) return false;
  last = now;
  return true;
}

}  // namespace

// Where the host migration tick (sub_8257D1F0) sets MigratingAsHost (2) after MigrateNewHost returns true.
constexpr uint32_t kTickSetsMigratingAsHost = 0x8257D358;

// AWorldInfo: sets PeerHostMigration.HostMigrationProgress (r3 the WorldInfo, r4 the new state).
// The tick reads the state (FindingNewHost, 1), runs MigrateNewHost, and then writes MigratingAsHost. On
// the path without an online session migration, MigrateNewHost already reached PeerTravelAsHost, which
// set HostReadyToTravel (5); writing 2 over it leaves the new host waiting forever, since only state 5
// counts the travel down. That one write is skipped, so the countdown runs and the new host travels.
extern "C" void sub_826E83B8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint8_t current = base[ctx.r3.u32 + 1736];
  const uint8_t next = ctx.r4.u32 & 0xFF;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  if (On()) {
    REXLOG_INFO("[Azura migration] state {} -> {} (called from {:08X}); {}", current, next, caller,
                MigrationState(base, ctx.r3.u32));
    if (current == 5 && next == 2 && caller == kTickSetsMigratingAsHost) {
      REXLOG_INFO("[Azura migration] kept HostReadyToTravel: the travel countdown runs");
      return;
    }
  }
  __imp__sub_826E83B8(ctx, base);
}

// AWorldInfo::BeginHostMigration.
extern "C" void sub_826E8260(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t world = ctx.r3.u32;
  const uint8_t before = base[world + 1736];
  __imp__sub_826E8260(ctx, base);
  // The game repeats the call on every frame while a migration waits: logged when the state changes.
  if (On() && before != base[world + 1736]) {
    REXLOG_INFO("[Azura migration] BeginHostMigration -> {}; {}", ctx.r3.u32, MigrationState(base, world));
  }
}

// APlayerController::PeerTravelAsHost(TravelCountdownTimer f1, URL r5); r3+212 is its WorldInfo.
extern "C" void sub_824A97A8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t controller = ctx.r3.u32;
  if (On()) {
    REXLOG_INFO("[Azura migration] PeerTravelAsHost(countdown {:.2f}, URL \"{}\"), WorldInfo {:08X}", ctx.f1.f64,
                FString(base, ctx.r5.u32), Load32(base, controller + 212));
  }
  __imp__sub_824A97A8(ctx, base);
  if (On()) REXLOG_INFO("[Azura migration] after PeerTravelAsHost: {}", MigrationState(base, Load32(base, controller + 212)));
}

// UOnlineSubsystemLive::MigrateOnlineGame(HostingPlayerNum r4, SessionName r5).
extern "C" void sub_82986FA0(PPCContext& __restrict ctx, uint8_t* base) {
  if (On()) REXLOG_INFO("[Azura migration] MigrateOnlineGame(player {}, session name {:08X})", ctx.r4.u32, ctx.r5.u32);
  __imp__sub_82986FA0(ctx, base);
  if (On()) REXLOG_INFO("[Azura migration] MigrateOnlineGame -> {}", ctx.r3.u32);
}

// UOnlineSubsystemLive::JoinMigratedOnlineGame.
extern "C" void sub_82987140(PPCContext& __restrict ctx, uint8_t* base) {
  if (On()) REXLOG_INFO("[Azura migration] JoinMigratedOnlineGame(player {}, {:08X}, {:08X})", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_82987140(ctx, base);
  if (On()) REXLOG_INFO("[Azura migration] JoinMigratedOnlineGame -> {}", ctx.r3.u32);
}

// The session migration helper both use (r6: 1 hosting, 0 joining).
extern "C" void sub_82987248(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t hosting = ctx.r6.u32;
  if (On()) REXLOG_INFO("[Azura migration] session migration ({})", hosting ? "as host" : "as client");
  __imp__sub_82987248(ctx, base);
  if (On()) REXLOG_INFO("[Azura migration] session migration -> {}", ctx.r3.u32);
}

// XSessionMigrateHost's wrapper (XamSessionRefObjByHandle, then XMsgStartIORequest 0x000B001E).
extern "C" void sub_82B0DB28(PPCContext& __restrict ctx, uint8_t* base) {
  if (On()) REXLOG_INFO("[Azura migration] XSessionMigrateHost(handle {:08X}, user {})", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82B0DB28(ctx, base);
  if (On()) REXLOG_INFO("[Azura migration] XSessionMigrateHost -> {}", ctx.r3.u32);
}

// The handler of the peers' control messages (r5: the message type).
extern "C" void sub_826E36C8(PPCContext& __restrict ctx, uint8_t* base) {
  if (On()) REXLOG_INFO("[Azura migration] peer control message {} (connection {:08X})", ctx.r5.u32 & 0xFF, ctx.r4.u32);
  __imp__sub_826E36C8(ctx, base);
}

// The four functions that use PeerHostMigration's travel countdown (+1744); r3 is logged as is.
#define AZURA_COUNTDOWN_USER(name)                                                                 \
  extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {                                \
    static int64_t last = 0;                                                                       \
    if (On() && Throttled(last)) {                                                                 \
      REXLOG_INFO("[Azura migration] " #name "(r3 {:08X}, r4 {:08X}) from {:08X}", ctx.r3.u32, ctx.r4.u32, \
                  static_cast<uint32_t>(ctx.lr));                                                  \
    }                                                                                              \
    __imp__##name(ctx, base);                                                                      \
  }
AZURA_COUNTDOWN_USER(sub_8257D1F0)
AZURA_COUNTDOWN_USER(sub_829A83B8)
AZURA_COUNTDOWN_USER(sub_82451BD8)
AZURA_COUNTDOWN_USER(sub_82AA01A8)
