// Hooks on the game's D3D calls that the HLE graphics path needs to see (docs/hle-graphics/
// 01-function-map.md has the evidence for each role). Every hook calls the original function;
// with gfx = hle it also writes a marker (rex/graphics/hle/markers.h) into the device's command
// buffer, before the call for the start of a clear, a resolve or a frame, after it for the rest.
//
// The device (r3 in every call) keeps its command buffer write cursor at +48, pointing at the
// last word written, and the buffer's limit at +56. Like the library's own writers, a marker
// checks the cursor against the limit once, has the library hand the buffer to the GPU
// (sub_82AF58B8, which returns the new cursor) when it is past it, and then stores its words with
// pre-incrementing stores; the limit leaves room for packets larger than a marker.

#include "hle_gfx/hle_gfx.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include <rex/graphics/hle/markers.h>
#include <rex/hook.h>

extern "C" {
void __imp__sub_82AECC98(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82AED028(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82AF1D50(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82AF34A8(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82AE9F60(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82AF65D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82AF58B8(PPCContext& __restrict ctx, uint8_t* base);
}

namespace azura::hle_gfx {

namespace {

using rex::graphics::hle::kMarkerMagic;
using rex::graphics::hle::kMaxMarkerWords;
using rex::graphics::hle::MarkerEvent;

constexpr uint32_t kCursorOffset = 48;
constexpr uint32_t kLimitOffset = 56;
// A PM4 type-3 NOP header, its payload word count minus one in bits 29-16.
constexpr uint32_t kNopHeader = 0xC0001000;
// Words of a surface header the markers carry: its RB_COLOR_INFO or RB_DEPTH_INFO value, and the
// word after it.
constexpr uint32_t kSurfaceInfoOffset = 28;
constexpr uint32_t kSurfaceInfo2Offset = 32;

std::atomic<uint32_t> g_frame{0};

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// Writes a marker with the event and its arguments at the device's cursor.
void WriteMarker(PPCContext& ctx, uint8_t* base, uint32_t device, MarkerEvent event,
                 std::initializer_list<uint32_t> args) {
  const uint32_t payload = 2 + static_cast<uint32_t>(args.size());
  if (payload > kMaxMarkerWords) {
    return;
  }
  uint32_t cursor = Load32(base, device + kCursorOffset);
  if (cursor > Load32(base, device + kLimitOffset)) {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = device;
    sub_82AF58B8(frame.ctx, base);
    cursor = Load32(base, device + kCursorOffset);
  }
  Store32(base, cursor += 4, kNopHeader | (payload - 1) << 16);
  Store32(base, cursor += 4, kMarkerMagic);
  Store32(base, cursor += 4, static_cast<uint32_t>(event));
  for (uint32_t arg : args) {
    Store32(base, cursor += 4, arg);
  }
  Store32(base, device + kCursorOffset, cursor);
}

// A word of a guest structure (a surface's header, a clear's colour), or zero without one.
uint32_t SurfaceWord(const uint8_t* base, uint32_t surface, uint32_t offset) {
  return surface ? Load32(base, surface + offset) : 0;
}

}  // namespace

bool Enabled() {
  static const bool enabled = REXCVAR_GET(gfx) == "hle";
  return enabled;
}

}  // namespace azura::hle_gfx

using azura::hle_gfx::Enabled;
using azura::hle_gfx::MarkerEvent;
using azura::hle_gfx::SurfaceWord;
using azura::hle_gfx::WriteMarker;

// SetRenderTarget(device, index, surface).
REX_HOOK_RAW(sub_82AECC98) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t surface = ctx.r5.u32;
  __imp__sub_82AECC98(ctx, base);
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kSetRenderTarget,
                {index, surface, SurfaceWord(base, surface, azura::hle_gfx::kSurfaceInfoOffset),
                 SurfaceWord(base, surface, azura::hle_gfx::kSurfaceInfo2Offset)});
  }
}

// SetDepthStencilSurface(device, surface).
REX_HOOK_RAW(sub_82AED028) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t surface = ctx.r4.u32;
  __imp__sub_82AED028(ctx, base);
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kSetDepthStencil,
                {surface, SurfaceWord(base, surface, azura::hle_gfx::kSurfaceInfoOffset),
                 SurfaceWord(base, surface, azura::hle_gfx::kSurfaceInfo2Offset)});
  }
}

// ClearF(device, flags, rectangle, colour, depth, stencil): the packets between the two markers
// are the guest's clear. The colour is four floats (red, green, blue, alpha) at r6.
REX_HOOK_RAW(sub_82AF1D50) {
  const uint32_t device = ctx.r3.u32;
  if (Enabled()) {
    const float depth = static_cast<float>(ctx.f1.f64);
    uint32_t depth_bits;
    std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
    const uint32_t color = ctx.r6.u32;
    WriteMarker(ctx, base, device, MarkerEvent::kClearBegin,
                {ctx.r4.u32, ctx.r5.u32, color, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, depth_bits,
                 azura::hle_gfx::SurfaceWord(base, color, 0),
                 azura::hle_gfx::SurfaceWord(base, color, 4),
                 azura::hle_gfx::SurfaceWord(base, color, 8),
                 azura::hle_gfx::SurfaceWord(base, color, 12)});
  }
  __imp__sub_82AF1D50(ctx, base);
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kClearEnd, {});
  }
}

// Resolve: the packets between the two markers are the guest's resolve.
REX_HOOK_RAW(sub_82AF34A8) {
  const uint32_t device = ctx.r3.u32;
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kResolveBegin,
                {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                 ctx.r10.u32});
  }
  __imp__sub_82AF34A8(ctx, base);
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kResolveEnd, {});
  }
}

// Start of a frame (resets the tiling bins).
REX_HOOK_RAW(sub_82AE9F60) {
  const uint32_t device = ctx.r3.u32;
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kFrameBegin,
                {azura::hle_gfx::g_frame.fetch_add(1, std::memory_order_relaxed) + 1});
  }
  __imp__sub_82AE9F60(ctx, base);
}

// End of a frame (fence and GPU interrupt).
REX_HOOK_RAW(sub_82AF65D8) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_82AF65D8(ctx, base);
  if (Enabled()) {
    WriteMarker(ctx, base, device, MarkerEvent::kFrameEnd,
                {azura::hle_gfx::g_frame.load(std::memory_order_relaxed)});
  }
}
