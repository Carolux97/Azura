// Checks on the engine's render command queue (Unreal Engine 3 FRingBuffer).
//
// Game threads enqueue render commands into a ring buffer (guest 0x8343F018: +0 start, +4 end,
// +8 write pointer, +16 read pointer): they construct the command, then publish it by moving the
// write pointer, with no barrier in between. The rendering thread (sub_823F4238) reads the write
// pointer and then runs each command through its vtable, again with no barrier. On the Xbox 360's
// in-order cores that is safe; on ARM64 either side may reorder its accesses, and the rendering
// thread can find a command whose vtable is not written yet. It then calls address 0 or garbage
// ("Call to invalid or unregistered function ... called from 0x823F42E8"), which happens mostly
// while a Bink movie plays, the heaviest user of the queue.
//
// AzuraCheckRenderCommand runs right before the rendering thread reads a command's vtable
// (midasm hook at 0x823F42D8 in the manifest). Its acquire fence orders that read, and everything
// the command reads, after the read of the write pointer: that is the fix, since the reordering
// that crashes is on the reading side (the evidence is in docs/reverse-engineering.md). As a
// safety net, a command whose vtable still does not look valid is waited for (the producer's
// writes arrive within microseconds) and logged as a warning instead of crashing; that warning
// would mean the writing side reorders too.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {

// The game's code and read-only data, where vtables and the functions they point to live.
constexpr uint32_t kImageStart = 0x82000000;
constexpr uint32_t kImageEnd = 0x84000000;

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + address));
}

bool InImage(uint32_t address) {
  return address >= kImageStart && address < kImageEnd;
}

// Whether the command at `command` has a vtable whose Execute entry is a function of the game.
bool HasValidVtable(const uint8_t* base, uint32_t command) {
  const uint32_t vtable = LoadU32(base, command);
  return InImage(vtable) && InImage(LoadU32(base, vtable + 4));
}

}  // namespace

// Called with the guest's r18: the address of the command about to run.
void AzuraCheckRenderCommand(PPCRegister& r18) {
  const uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t command = r18.u32;
  if (HasValidVtable(base, command)) return;

  const uint32_t seen = LoadU32(base, command);
  const auto start = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < 2000; ++attempt) {
    if (HasValidVtable(base, command)) {
      const auto waited =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
      REXLOG_WARN("[Azura render] Command at 0x{:08X} had vtable 0x{:08X}; valid (0x{:08X}) after {} us", command,
                  seen, LoadU32(base, command), waited);
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
  REXLOG_ERROR("[Azura render] Command at 0x{:08X} has vtable 0x{:08X}, still invalid after 2000 tries", command,
               LoadU32(base, command));
}
