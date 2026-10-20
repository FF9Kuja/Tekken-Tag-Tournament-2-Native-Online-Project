// Boot monitor: what the game asks the kernel for, recorded by the host's export wrappers
// (io_overrides.cpp, wait_overrides.cpp, gpu_overrides.cpp) and reported by Ttt2App's watchdog.
//
// The owner's first boots end in a black window with every guest thread alive and nothing logged, so
// "where did it stop" has to come from the host: per guest thread the last traced kernel call and the
// blocking wait it is in (API, objects, caller), global counters for the milestones of a boot (files
// opened and read, the GPU ring buffer and interrupt callback, frames presented, audio frames, input
// polls), the recent file activity, and each thread's guest call stack read from its guest stack.
//
// Overhead: a wrapped call costs a thread-local lookup and a few relaxed atomic stores (no clock reads,
// no locks); file events take a mutex (they are rare). Times are derived by the watchdog, which notes
// when it first saw a wait or a quiet thread. Everything recorded is names, addresses and counts.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace rex {
struct PPCImageInfo;
namespace system {
class XThread;
}  // namespace system
}  // namespace rex

namespace ttt2::monitor {

/// Wrapped exports tracked per thread (ids from RegisterWrappedExport; larger ids share the last slot).
inline constexpr std::uint32_t kMaxTrackedExports = 64;
/// Objects of one wait that are recorded (KeWaitForMultipleObjects can name up to 64).
inline constexpr std::uint32_t kMaxWaitObjects = 4;
/// Timeout value for "the guest passed no timeout": the wait lasts until an object is signaled.
inline constexpr std::int64_t kNoTimeout = std::numeric_limits<std::int64_t>::min();

enum class WaitKind : std::uint8_t {
  kNone,
  kDispatcher,       ///< Ke* waits: guest pointers to dispatcher headers (events, semaphores, mutants, ...)
  kHandles,          ///< Nt* waits: object handles
  kCriticalSection,  ///< RtlEnterCriticalSection: a guest pointer to an RTL_CRITICAL_SECTION
  kDelay,            ///< KeDelayExecutionThread: a sleep, no object
};

/// One guest thread's traced kernel activity. Written only by its own thread; read by the watchdog.
struct ThreadActivity {
  explicit ThreadActivity(std::uint32_t id) : thread_id(id) {}

  const std::uint32_t thread_id;  ///< XThread::thread_id, 0 for a host thread
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint32_t> last_export{0};
  std::atomic<std::uint32_t> last_lr{0};  ///< guest return address of the last traced call
  std::array<std::atomic<std::uint32_t>, kMaxTrackedExports> per_export{};

  // The blocking call in progress: wait_export is kNoExport when there is none; wait_seq changes with
  // every wait, so the watchdog can tell a long wait from a series of short ones.
  std::atomic<std::uint32_t> wait_export{kNoExport};
  std::atomic<WaitKind> wait_kind{WaitKind::kNone};
  std::atomic<std::uint32_t> wait_lr{0};
  std::atomic<std::uint32_t> wait_seq{0};
  std::atomic<std::uint32_t> wait_count{0};  ///< objects in the wait (only kMaxWaitObjects recorded)
  std::array<std::atomic<std::uint32_t>, kMaxWaitObjects> wait_objects{};
  std::atomic<std::int64_t> wait_timeout{kNoTimeout};  ///< 100 ns units as passed (negative: relative)

  static constexpr std::uint32_t kNoExport = 0xFFFFFFFFu;
};

/// The calling thread's record (created on first use, never freed).
ThreadActivity& Current();

/// Every record created so far.
std::vector<ThreadActivity*> AllThreads();

/// Records a traced call of export `id` from guest address `lr` on the calling thread.
inline ThreadActivity& NoteCall(std::uint32_t id, std::uint32_t lr) {
  ThreadActivity& t = Current();
  // Only this thread writes these: plain load + store, no locked instructions.
  t.calls.store(t.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  auto& slot = t.per_export[id < kMaxTrackedExports ? id : kMaxTrackedExports - 1];
  slot.store(slot.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  t.last_export.store(id, std::memory_order_relaxed);
  t.last_lr.store(lr, std::memory_order_relaxed);
  return t;
}

/// Marks a blocking call for the stall dump while it runs; restores an outer wait on exit (a wait can
/// run guest code - APCs - that waits again).
class WaitScope {
 public:
  WaitScope(ThreadActivity& t, std::uint32_t export_id, WaitKind kind, std::uint32_t lr,
            std::int64_t timeout, const std::uint32_t* objects, std::uint32_t count);
  ~WaitScope();
  WaitScope(const WaitScope&) = delete;
  WaitScope& operator=(const WaitScope&) = delete;

 private:
  ThreadActivity& t_;
  std::uint32_t outer_export_;
  WaitKind outer_kind_;
  std::uint32_t outer_lr_;
  std::uint32_t outer_count_;
  std::array<std::uint32_t, kMaxWaitObjects> outer_objects_;
  std::int64_t outer_timeout_;
};

/// Boot milestones and volumes, process-wide.
struct Counters {
  std::atomic<std::uint64_t> files_opened{0};
  std::atomic<std::uint64_t> open_failures{0};
  std::atomic<std::uint64_t> device_open_failures{0};  ///< bare \Device\<name> paths that failed to open
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> read_failures{0};
  std::atomic<std::uint64_t> bytes_read{0};
  std::atomic<std::uint64_t> sector_queries{0};  ///< FileSectorInformation queries answered unsupported
  std::atomic<std::uint64_t> attribute_failures{0};  ///< NtQueryFullAttributesFile misses
  std::atomic<std::uint64_t> threads_created{0};
  std::atomic<std::uint64_t> frames_presented{0};  ///< VdSwap calls
  std::atomic<std::uint64_t> audio_frames{0};      ///< XAudioSubmitRenderDriverFrame calls
  std::atomic<std::uint64_t> input_polls{0};       ///< XamInputGetState calls
  std::atomic<std::uint32_t> engines_initialized{0};  ///< VdInitializeEngines calls
  std::atomic<std::uint32_t> ring_buffer{0};          ///< VdInitializeRingBuffer base (0: not yet)
  std::atomic<std::uint32_t> ring_buffer_size_log2{0};
  std::atomic<std::uint32_t> interrupt_callback{0};   ///< VdSetGraphicsInterruptCallback (0: none)
  std::atomic<std::uint32_t> audio_clients{0};        ///< XAudioRegisterRenderDriverClient successes
};

Counters& counters();

/// Milliseconds since the monitor started (process start, in practice).
std::uint64_t NowMs();

/// A file-system event kept for the stall dump (the last kRecentFileEvents of them).
struct FileEvent {
  std::uint64_t ms = 0;
  std::uint32_t thread_id = 0;
  std::uint32_t lr = 0;
  std::uint32_t status = 0;
  std::string api;
  std::string path;
};
inline constexpr std::size_t kRecentFileEvents = 12;

void NoteFileEvent(std::string_view api, std::string_view path, std::uint32_t status, std::uint32_t lr);
std::vector<FileEvent> RecentFileEvents();

/// NTSTATUS name for the values the I/O and wait paths return ("" when unknown).
std::string_view StatusName(std::uint32_t status);
/// "0xC000000F (STATUS_NO_SUCH_FILE)" or "0x00001234".
std::string FormatStatus(std::uint32_t status);

/// Name of the calling guest thread ("nuscMain (id 7)", "thread id 9", "host thread").
std::string CurrentThreadLabel();

/// The recompiled image, for naming guest addresses (the start of the function holding an address).
void SetImage(const rex::PPCImageInfo& image);
/// A return address as "0x8234ABCC (sub_8234AB00+0xCC)": the function holding the call before it.
/// The bare address when the image is unknown or it is not code.
std::string DescribeGuestAddress(std::uint32_t address);
/// A code address that is not a return address (a function or thread start), named the same way.
std::string DescribeGuestCode(std::uint32_t address);

/// Return addresses on a guest thread's stack, innermost first: its link register, then the saved
/// return address of every frame on the back chain (the Xbox 360 ABI keeps it at -8 from the caller's
/// stack pointer). Only addresses inside the thread's stack are read.
std::vector<std::uint32_t> GuestStack(rex::system::XThread& thread, std::size_t max_frames);

/// State of the guest GPU emulation the host can read (Ttt2App fills it from the graphics system).
struct GpuState {
  bool available = false;
  std::uint32_t vblanks = 0;              ///< the command processor's vblank counter
  std::uint32_t ring_write_pointer = 0;   ///< CP_RB_WPTR as last written by the game
};

/// Called by the watchdog every tick: notes per thread whether it made traced calls and whether it is
/// in the same wait as on the previous tick, and the per-export call counts of the tick.
void Observe(std::uint64_t now_ms);

/// The counters that only move while the boot makes progress (files opened, bytes read, frames
/// presented, threads created); a stall is a run of ticks where this stays the same.
std::uint64_t ProgressFingerprint();

/// The watchdog tick's activity line (without the "ttt2: " prefix).
std::string ActivityLine(const GpuState& gpu);

/// Detail lines for one guest thread in a stall dump: its wait, its traced calls, its guest stack.
std::vector<std::string> DescribeThread(rex::system::XThread& thread, std::uint64_t now_ms);

/// Process-wide lines of a stall dump: busiest traced calls of the last tick, recent file activity.
std::vector<std::string> DescribeProcess(std::uint64_t now_ms);

}  // namespace ttt2::monitor
