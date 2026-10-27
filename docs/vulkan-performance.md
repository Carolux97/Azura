# Vulkan backend performance

What was measured, what was changed and why, in the Vulkan backend and the Linux memory code. The
aim: steady frame rates on Linux (Ubuntu, Bazzite) and Windows without light flares showing through
walls. Every change below was measured before and after; the numbers are from the
runs named, and one run proves only what it shows.

Test machines:

- **Ubuntu PC**: RTX 3050 Laptop, 1280x720, NVIDIA 595 driver, Linux 7.0.
- **Bazzite PC**: RTX 5060, Ryzen 7 5800X, Bazzite 44 (Fedora), Linux 7.2.

All runs are Act 1, Chapter 1 with the frame rate not limited (`frame_rate_limit = 150`). Runs of
the same comparison do not follow exactly the same route, so frames are compared by load (draws
per frame), not by session averages.

## 1. How the game waits for the GPU

Unreal Engine 3 tests a box around each object and light flare with an occlusion query and reads
the result in the same frame. The game's render thread calls the query's GetData (guest
`0x82AFA708`) in a loop until it returns ready (guest `0x82961960`); almost all calls return at
once, but about once per frame (call site `0x827C13E8`) the result is not there yet and the thread
spins. In combat on the Ubuntu PC that wait was 12 to 13 ms of a 18 to 20 ms frame (about 68%),
always for an occlusion query (D3D query type 9).

So the frame time is roughly the guest's own CPU work plus the time from the end of the query it
waits for to the moment its result is in guest memory. Most changes below shorten that second
part: the GPU work before the query, the time the command processor takes to record it, and the
time between the GPU counting it and the result being written.

A result can not be made up to avoid the wait: answering "visible" without the GPU's count is what
made flares show through walls (`occlusion_query_no_wait`, reverted in 0.4.4).

## 2. Occlusion results as soon as the GPU counts them

Before: a result was written when the whole submission with its host queries completed. With
`hostQueryReset` (Vulkan 1.2) the query blocks are reset from the CPU when they are taken, so a
host query is available only once the GPU has counted it, and each result is written as soon as
all its host queries are available (`VulkanCommandProcessor::WriteAvailableOcclusionResults`,
`occlusion_query_early_results`, on by default). Details in metal-backend-design.md, section 12.

Ubuntu PC, one run each: median frame 16.9 → 14.6 ms, average 60 → 66 FPS, 1% low 24 → 27 FPS,
same draws and same share of hidden objects.

## 3. 2048 host queries per block (lights flickering on Bazzite)

On Bazzite the light under the upper walkway of the first combat area flickered at a few hertz
with the camera still. The diagnosis run there showed up to 478 guest occlusion queries in one
submission, and blocks held 256 host queries. A guest query whose draws found no host query left
was marked incomplete, and its result was written as visible (`query_occlusion_fake_sample_count`):
frames over 256 queries made that light visible, frames under hid it. (The mechanism is read from
the code and matches the data; the flicker was not traced query by query.)

Blocks now hold 2048 host queries (`kOcclusionQueryBlockSize`), and the incomplete results are
counted per frame (`occlusion_results_incomplete`). On both PCs it stayed at 0, and on Bazzite the
flicker is gone.

## 4. Page protection lookups with PROCMAP_QUERY (Linux stutters)

The GPU emulation watches guest memory it has copied to the GPU: a CPU write to a watched page
faults, and the fault handler (`MMIOHandler`) asks the memory code for the protection of the
faulting page. On Linux, `memory::QueryProtect` read and parsed the whole `/proc/self/maps`
(about 232 KB, thousands of lines) for every fault, while holding the global critical region.

Measured on the Ubuntu PC: about 380 faults per second in gameplay, 436 µs each (182 µs in the
menu), 46 s of 280 s of play spent reading the file, with every other thread that needed the lock
waiting. The game's worker threads (they decompress content while streaming) spent 34% of their CPU
there during stutters.

Now `FindEntryForAddress` and `IsRangeFullyMapped` (`src/core/memory_posix.cpp`) ask the kernel
for the mapping of one address with the `PROCMAP_QUERY` ioctl on `/proc/self/maps` (Linux 6.11+),
1.8 µs per fault. The descriptor is opened once and shared; kernels without the ioctl (ENOTTY or
EINVAL) fall back to parsing the file, as before, for the rest of the run.

Ubuntu PC, gameplay: frames over 33 ms 38.7 → 4.2 per minute, 1% low 22.6 → 32.8 FPS. Bazzite:
under 1 per minute. macOS and Windows have their own lookups and are not affected.

## 5. Guest memory uploads before the render pass

Measured with GPU timestamps around every kind of work (temporary instrumentation), combat on the
Ubuntu PC: 14.2 ms of GPU per frame, of which game draws 7.3 ms (51%), guest memory uploads 3.1 ms
(22%), texture loads 2.1 ms, render target transfers 1.3 ms, resolves 0.6 ms. 258 of the 318 render
passes per frame ended for an upload: Vulkan allows no copies inside a render pass, so
`VulkanSharedMemory::UploadRanges` ended the pass, copied, and the next draw began a new pass,
with a barrier that drains the GPU each time. Most of them are the vertices the game writes for
the box of each occlusion query (about 100 per frame), and other dynamic vertices: 409 uploads
and 5.8 MB per frame.

The deferred command buffer is only turned into a Vulkan command buffer when the submission ends,
so a place in it can be filled later. When a guest draw render pass begins,
`SubmitBarriersAndEnterRenderTargetCacheRenderPass` reserves a place before it
(`DeferredCommandBuffer::CmdHoistedCopies`). An upload requested while that pass is open goes
there (`VulkanCommandProcessor::HoistSharedMemoryUpload`) instead of ending it, if no draw of the
pass has read those pages yet (`CanHoistSharedMemoryUpload`). When the commands are replayed, a
place with copies becomes one barrier (earlier reads → copy), the copies, and one barrier (copy →
reads), all before the pass begins.

What a draw reads is marked per page after the draw is recorded
(`MarkRenderPassSharedMemoryRead`): every vertex buffer the vertex shader fetches (also those
skipped as already in sync) and the guest index buffer. Each page holds the number of the last
pass that read it, so nothing is cleared when a pass begins. A draw that may write shared memory
(memexport) stops hoisting for the rest of its pass. Hoisting only happens while the shared memory
is only being read (`VulkanSharedMemory::IsOnlyRead`), as the barriers assume, and never in passes
begun for render target transfers.

Example, the depth-only pass with the boxes of about 96 occlusion queries: before, 96 pass ends,
about 192 barriers and 96 passes; now one pass, two barriers and 96 copies before it.

Results, with `vulkan_hoist_uploads` on and off in the same build:

| Draws per frame (Ubuntu PC) | Frame, on | Frame, off | GPU, on | GPU, off |
| --- | --- | --- | --- | --- |
| 1000–2000 | 16.6 ms | 18.3 ms | 10.0 ms | 14.3 ms |
| over 2000 | 21.5 ms | 24.8 ms | 10.4 ms | 15.8 ms |

Pass ends for uploads: 58 → 6 per frame; 369 of 378 uploads hoisted. The Vulkan validation layer
reported nothing in four minutes of menus. No visual difference was seen on either PC.

Risk: a draw that reads shared memory inside the pass without it being marked would see data of
a later draw. Pixel shader vertex fetches are not marked (the draw path does not request them
either). `vulkan_hoist_uploads = false` turns it off.

## 6. Command processor sleeping while the guest waits for a result

When the command processor runs out of commands it spins 500 times calling
`OnWaitingForCommands` (which writes the results the GPU has counted), then waits up to 5 ms for
new commands. A guest spinning on a result sends no commands, so a result counted during that
sleep reached guest memory up to 5 ms late.

Now, while the backend has results to write (`CommandProcessor::HasPendingGuestResults`, true in
Vulkan while occlusion results are pending), the command processor sleeps 0.1 ms between checks
instead. A/B in the same build, Ubuntu PC:

| Draws per frame | 5 ms sleep | 0.1 ms while results are pending |
| --- | --- | --- |
| 300–800 | 14.4 ms (69 FPS) | 12.4 ms (81 FPS) |
| 800–1500 | 15.6 ms (64 FPS) | 13.3 ms (75 FPS) |
| over 1500 | 19.1 ms (52 FPS) | 17.1 ms (59 FPS) |

Same GPU time; the guest's wait per frame went down by about 2 ms. Cost: while a result is
pending, the command processor thread wakes up to 10000 times per second.

On Windows `rex::thread::Sleep` rounds to whole milliseconds, so 0.1 ms becomes `Sleep(0)`: the
thread gives up its time slice and checks again at once. Results arrive as soon as possible there,
but the thread keeps a core busy while the guest waits (about half of each frame in combat). Not
measured on Windows yet; a high-resolution waitable timer would give a real 0.1 ms sleep if the
spinning costs frames on CPUs with few cores.

## 7. Register writes in bulk

CPU samples of the command processor thread ("GPU Commands") in combat: 18.6% of its time went to
PM4 register writes. Type-0 packets wrote their registers one virtual `WriteRegister` call at a
time, and every call looked the register up with `RegisterFile::GetRegisterInfo`, a switch over
the whole register table with a static in each case, only for a debug message (4.4%).

Now type-0 packets that write consecutive registers go through `WriteRegisterRangeFromRing`, the
path the Vulkan backend already writes in bulk for shader constants (one byte-swapped copy and one
check of the constants the current shaders use); other registers still go through `WriteRegister`
one by one in that path. Whether a register is known comes from a table built once
(`IsKnownRegister`).

A/B, Ubuntu PC, combat:

| Draws per frame | Before | After | Command processor CPU per frame |
| --- | --- | --- | --- |
| 800–1500 | 13.2 ms (76 FPS) | 12.5 ms (80 FPS) | 9.6 → 8.2 ms |
| 1500–2500 | 16.0 ms (62 FPS) | 14.4 ms (69 FPS) | 11.8 → 10.2 ms |
| over 2500 | 20.1 ms (50 FPS) | 17.0 ms (59 FPS) | 15.2 → 12.6 ms |

Register write functions: 15.1% → 2.7% of the thread's samples.

## 8. Results on Bazzite

RTX 5060, build with sections 2 to 6 (before section 7), against the build with sections 2 to 4:

| Draws per frame | Before | After | GPU before → after |
| --- | --- | --- | --- |
| 300–800 | 6.8 ms (147 FPS) | 3.6 ms (278 FPS) | 3.9 → 2.4 ms |
| 800–1500 | 8.7 ms (116 FPS) | 7.5 ms (133 FPS) | 5.9 → 3.9 ms |
| 1500–2500 | 12.0 ms (84 FPS) | 10.1 ms (99 FPS) | 7.1 → 4.1 ms |
| over 2500 | 17.8 ms (56 FPS) | 13.4 ms (74 FPS) | 8.3 → 4.3 ms |

1% low 52 → 62 FPS, no incomplete occlusion results, no visual problem reported.

## 9. Where the time goes now, and what is left

Command processor thread in combat (Ubuntu PC, after section 6, before section 7; callers from a
stack scan, approximate):

- 10.4%: the driver recording the Vulkan command buffer when the deferred one is replayed
  (`DeferredCommandBuffer::Execute`), about 1.2 ms per frame in heavy combat. Doing that, and the
  submission, on a thread of its own would overlap it with the recording of the next commands.
  Not done: when the guest waits for a result, the list with its query still has to be recorded
  before the GPU sees it, so the gain is estimated at 0 to 1 ms per frame (an inference), and the
  change touches every place that relies on submission order (the swap and the presenter, waits
  for a submission, cache clears, device loss), where a miss shows as intermittent hangs or lost
  devices.
- Descriptor sets written for each draw (`UpdateBindings`): tried and dropped. Reusing the
  texture and sampler set of the previous draw of the frame when the images and samplers were the
  same (the code's own TODO) reused it in 19% of the draws of a combat run, with no measurable
  change in frame time or command processor CPU (the differences went both ways, within the noise
  between runs). In that run `UpdateBindings` and the driver under it took 4 to 5% of the thread,
  less than the first estimate from the stack scan. Dynamic uniform buffer offsets for the
  constants would save even less, so neither was kept.
- 7.4%: copying the uploaded guest memory (`memcpy` in `UploadRanges`): mostly the data itself.
- 3.7%: protecting watched pages again after uploads (`mprotect`, `EnableAccessCallbacks`).
- 2.7%: looking shaders up by their microcode hash (`LoadShader`, XXH3).

Other known points:

- **Sending work to the GPU in pieces.** Submissions end at the end of each primary buffer the
  guest sends (3 to 4 per frame). Ending one every few hundred draws would let the GPU start while
  the command processor records the rest. Not measured yet.
- **Lights through walls on the Ubuntu PC.** One or two flares were still seen there with all
  results real GPU counts (no incomplete or fake ones). Unexplained: possibly a thin wall lost in
  the half-resolution depth the boxes are tested against, or the game's own behaviour. It needs the
  place and the light identified.
- **Windows PCs with a Ryzen 7 X3D** were reported capped at about 50 FPS; no data yet.

## 10. How it was measured

- Per-frame counters (`perf_log_csv`) and per-thread CPU time (`perf_log_threads`), with F8 marks.
- Temporary instrumentation, not part of the change: GPU timestamps around each kind of GPU work
  (by debug label), the guest's wait for query results (a hook on `0x82961960`), a timeline of
  primary buffers, submissions, GPU execution and guest waits, and a CPU sampler (per-thread CPU
  timers, instruction pointer and nearest own caller on the stack).
- The A/B comparisons switched each change in the same build (`vulkan_hoist_uploads`, and
  temporary switches for sections 4, 6 and 7).
