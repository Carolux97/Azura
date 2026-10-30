// PM4 writer trace (see pm4_trace.h).
//
// The D3D device keeps its command buffer write cursor at +48 (the address of the last word
// written: packets are stored with pre-incrementing stores) and the buffer's limit at +56; the
// game reaches the device through the global at 0x83416C5C. Reading the cursor before and after a
// call gives the bytes that call wrote. Calls nest (a D3D function calls helpers that write too),
// so each call is charged only its own bytes: what it wrote minus what the calls inside it wrote.
// When the cursor moves backwards the buffer was handed to the GPU and a new one started; that
// call's bytes are unknown and are counted as a wrap instead.
//
// Callers are the return address in the link register at entry (the call site in the caller,
// plus 4) and one level further up, the return address the caller saved in its own frame (the
// back chain at 0(r1), the saved link register 8 bytes below the frame it points to). Tail calls
// show the caller's caller instead, as on the console.
//
// Each call's packets (everything written between entry and exit, its calls included) are also
// walked and counted per function: the registers that type-0 and type-1 packets write, the
// opcodes of type-3 packets and the constant kind of SET_CONSTANT packets. That profile, with the
// inclusive byte count, tells what a function asks the GPU to do.
//
// Bytes the game writes into the command buffer itself, outside the traced functions (inlined
// D3D code, or data it copies into space a D3D call reserved), show up as the cursor moving
// between two top-level traced calls on the same thread, with no traced call finishing on another
// thread in between (another thread's writes would otherwise be counted); they are charged to the
// return address of the second call, which is in the game function that wrote them.
//
// The results are written every 10 seconds (and replace the previous ones) to the file named by
// the environment variable AZURA_PM4_TRACE_OUT, default pm4_trace.tsv in the working folder. They
// are derived from the game: keep them local (captures/ is ignored by git).

#include "hle_gfx/pm4_trace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace azura::pm4_trace {
namespace {

constexpr uint32_t kDeviceGlobal = 0x83416C5C;
constexpr uint32_t kDeviceCursorOffset = 48;
// Seconds between two writes of the results.
constexpr int64_t kDumpIntervalSeconds = 10;
// Exit looks at the clock only once every this many calls.
constexpr uint32_t kClockCheckEvery = 4096;
// A back chain farther than this above r1 is not trusted as the caller's frame.
constexpr uint32_t kMaxFrameSize = 64 * 1024;

struct FunctionStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> self_bytes{0};
  std::atomic<uint64_t> wraps{0};
  // Bytes written by the call including the calls inside it.
  std::atomic<uint64_t> inclusive_bytes{0};
};

// Kinds of packet profile entries, in the top byte of their key.
enum ProfileKind : uint32_t {
  kRegister = 1,     // a register written by a type-0 or type-1 packet (value: register index)
  kOpcode = 2,       // a type-3 packet (value: opcode)
  kConstantKind = 3, // a SET_CONSTANT packet (value: constant kind, 0 ALU to 4 registers)
};
constexpr uint32_t kPm4SetConstant = 0x2D;
// Packets longer than this are not walked (a misread header would otherwise skip far ahead).
constexpr uint32_t kMaxPacketWords = 0x4000;

struct Frame {
  uint32_t index;
  uint32_t device;
  uint32_t cursor;
  // Bytes written by the calls made inside this one, to subtract from its own total.
  uint64_t child_bytes;
};

std::unique_ptr<FunctionStats[]> g_stats;
std::once_flag g_stats_once;

// (function index, caller return address, caller's caller return address) -> calls.
std::mutex g_callers_mutex;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> g_callers;

// (function index << 32 | kind << 24 | value) -> occurrences.
std::mutex g_profile_mutex;
std::map<uint64_t, uint64_t> g_profile;

std::atomic<uint32_t> g_exit_counter{0};
std::atomic<int64_t> g_last_dump{0};

thread_local std::vector<Frame> t_stack;
// The last traced call to return, on any thread: its thread's token (high half) and the cursor
// (low half).
std::atomic<uint64_t> g_last_exit{0};
std::atomic<uint32_t> g_next_thread_token{1};
thread_local const uint32_t t_thread_token = g_next_thread_token.fetch_add(1);
thread_local uint32_t t_last_device = 0;

// Return address of a top-level traced call -> bytes the game wrote since the previous one.
std::mutex g_gaps_mutex;
std::map<uint32_t, uint64_t> g_gaps;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

// Counts the packets in [first, last] (guest addresses of whole words) for function index.
void ProfilePackets(uint32_t index, const uint8_t* base, uint32_t first, uint32_t last) {
  std::map<uint32_t, uint64_t> counts;
  uint32_t at = first;
  while (at <= last) {
    const uint32_t header = Load32(base, at);
    const uint32_t type = header >> 30;
    uint32_t words = 1;
    if (type == 0) {
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      const uint32_t reg = header & 0x7FFF;
      const bool one_register = header & 0x8000;
      for (uint32_t i = 0; i < count && i < kMaxPacketWords; ++i) {
        ++counts[kRegister << 24 | (one_register ? reg : reg + i)];
      }
      words += count;
    } else if (type == 1) {
      ++counts[kRegister << 24 | (header & 0x7FF)];
      ++counts[kRegister << 24 | ((header >> 11) & 0x7FF)];
      words += 2;
    } else if (type == 3) {
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      const uint32_t opcode = (header >> 8) & 0x7F;
      ++counts[kOpcode << 24 | opcode];
      if (opcode == kPm4SetConstant && at + 4 <= last) {
        const uint32_t kind = (Load32(base, at + 4) >> 16) & 0xFF;
        ++counts[kConstantKind << 24 | kind];
      }
      words += count;
    }
    if (words > kMaxPacketWords) {
      break;
    }
    at += words * 4;
  }
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  for (const auto& [key, n] : counts) {
    g_profile[uint64_t{index} << 32 | key] += n;
  }
}

FunctionStats* Stats() {
  std::call_once(g_stats_once, [] {
    g_stats = std::make_unique<FunctionStats[]>(kTracedFunctionCount);
    REXLOG_INFO("[Azura PM4 trace] Tracing {} guest functions", kTracedFunctionCount);
  });
  return g_stats.get();
}

void Dump() {
  const char* path = std::getenv("AZURA_PM4_TRACE_OUT");
  if (!path || !*path) {
    path = "pm4_trace.tsv";
  }
  FILE* file = std::fopen(path, "w");
  if (!file) {
    REXLOG_WARN("[Azura PM4 trace] Could not write {}", path);
    return;
  }
  FunctionStats* stats = Stats();
  std::fprintf(file, "# functions\nfunction\tcalls\tself_bytes\twraps\tinclusive_bytes\n");
  for (size_t i = 0; i < kTracedFunctionCount; ++i) {
    const uint64_t calls = stats[i].calls.load(std::memory_order_relaxed);
    if (calls) {
      std::fprintf(file, "%08X\t%llu\t%llu\t%llu\t%llu\n", kTracedFunctions[i],
                   static_cast<unsigned long long>(calls),
                   static_cast<unsigned long long>(stats[i].self_bytes.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(stats[i].wraps.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(stats[i].inclusive_bytes.load(std::memory_order_relaxed)));
    }
  }
  std::fprintf(file, "# callers\nfunction\treturn_address\tcaller_return_address\tcalls\n");
  {
    std::lock_guard<std::mutex> lock(g_callers_mutex);
    for (const auto& [key, calls] : g_callers) {
      std::fprintf(file, "%08X\t%08X\t%08X\t%llu\n", kTracedFunctions[std::get<0>(key)],
                   std::get<1>(key), std::get<2>(key), static_cast<unsigned long long>(calls));
    }
  }
  std::fprintf(file, "# game_writes\nreturn_address\tbytes\n");
  {
    std::lock_guard<std::mutex> lock(g_gaps_mutex);
    for (const auto& [return_address, bytes] : g_gaps) {
      std::fprintf(file, "%08X\t%llu\n", return_address, static_cast<unsigned long long>(bytes));
    }
  }
  std::fprintf(file, "# packets\nfunction\tkind\tvalue\tcount\n");
  {
    static const char* const kKindNames[] = {"", "register", "opcode", "constant_kind"};
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    for (const auto& [key, n] : g_profile) {
      const uint32_t index = static_cast<uint32_t>(key >> 32);
      const uint32_t kind = static_cast<uint32_t>(key >> 24) & 0xFF;
      std::fprintf(file, "%08X\t%s\t%X\t%llu\n", kTracedFunctions[index],
                   kind < 4 ? kKindNames[kind] : "?", static_cast<uint32_t>(key) & 0xFFFFFF,
                   static_cast<unsigned long long>(n));
    }
  }
  std::fclose(file);
}

}  // namespace

void Enter(uint32_t index, PPCContext& ctx, uint8_t* base) {
  Stats()[index].calls.fetch_add(1, std::memory_order_relaxed);

  const uint32_t return_address = static_cast<uint32_t>(ctx.lr);
  uint32_t caller_return_address = 0;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t back_chain = Load32(base, sp);
  if (back_chain > sp && back_chain - sp <= kMaxFrameSize) {
    caller_return_address = Load32(base, back_chain - 8);
  }
  {
    std::lock_guard<std::mutex> lock(g_callers_mutex);
    ++g_callers[{index, return_address, caller_return_address}];
  }

  const uint32_t device = Load32(base, kDeviceGlobal);
  const uint32_t cursor = device ? Load32(base, device + kDeviceCursorOffset) : 0;
  const uint64_t last_exit = g_last_exit.load(std::memory_order_relaxed);
  const uint32_t last_cursor = static_cast<uint32_t>(last_exit);
  if (t_stack.empty() && device && device == t_last_device && (last_exit >> 32) == t_thread_token &&
      cursor > last_cursor) {
    std::lock_guard<std::mutex> lock(g_gaps_mutex);
    g_gaps[return_address] += cursor - last_cursor;
  }
  t_stack.push_back({index, device, cursor, 0});
}

void Exit(uint32_t index, uint8_t* base) {
  if (t_stack.empty() || t_stack.back().index != index) {
    // Unbalanced (a call that never returned through its pass-through); start over.
    t_stack.clear();
    return;
  }
  const Frame frame = t_stack.back();
  t_stack.pop_back();

  FunctionStats& stats = Stats()[index];
  const uint32_t device = Load32(base, kDeviceGlobal);
  t_last_device = device;
  g_last_exit.store(uint64_t{t_thread_token} << 32 | (device ? Load32(base, device + kDeviceCursorOffset) : 0),
                    std::memory_order_relaxed);
  if (frame.device && device == frame.device) {
    const uint32_t cursor = Load32(base, device + kDeviceCursorOffset);
    if (cursor >= frame.cursor) {
      const uint64_t written = cursor - frame.cursor;
      const uint64_t self = written >= frame.child_bytes ? written - frame.child_bytes : 0;
      stats.self_bytes.fetch_add(self, std::memory_order_relaxed);
      stats.inclusive_bytes.fetch_add(written, std::memory_order_relaxed);
      if (written) {
        ProfilePackets(index, base, frame.cursor + 4, cursor);
      }
      if (!t_stack.empty()) {
        t_stack.back().child_bytes += written;
      }
    } else {
      stats.wraps.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (g_exit_counter.fetch_add(1, std::memory_order_relaxed) % kClockCheckEvery == 0) {
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    int64_t last = g_last_dump.load(std::memory_order_relaxed);
    if (last == 0) {
      g_last_dump.compare_exchange_strong(last, now);
    } else if (now - last >= kDumpIntervalSeconds && g_last_dump.compare_exchange_strong(last, now)) {
      Dump();
    }
  }
}

}  // namespace azura::pm4_trace
