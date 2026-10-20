#include "boot_monitor.h"

#include <rex/image_info.h>
#include <rex/memory.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "frame_monitor.h"
#include "kernel_clock.h"
#include "runtime_exports.h"

namespace ttt2::monitor {
namespace {

using Clock = std::chrono::steady_clock;

const Clock::time_point& StartTime() {
  static const Clock::time_point start = Clock::now();
  return start;
}
// The clock starts with the process (static initialisation), not with the first query.
[[maybe_unused]] const Clock::time_point& kStartAnchor = StartTime();

std::mutex& RecordsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::unique_ptr<ThreadActivity>>& Records() {
  static std::vector<std::unique_ptr<ThreadActivity>> records;
  return records;
}

ThreadActivity* NewRecord() {
  std::uint32_t id = 0;
  if (rex::system::XThread::IsInThread()) {
    if (auto* thread = rex::system::XThread::GetCurrentThread()) id = thread->thread_id();
  }
  auto record = std::make_unique<ThreadActivity>(id);
  ThreadActivity* raw = record.get();
  std::lock_guard<std::mutex> lock(RecordsMutex());
  Records().push_back(std::move(record));
  return raw;
}

// --- recent file events ----------------------------------------------------------------------------

std::mutex& FileEventsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::deque<FileEvent>& FileEvents() {
  static std::deque<FileEvent> events;
  return events;
}

// --- the recompiled image ----------------------------------------------------------------------------

struct ImageMap {
  std::uint32_t code_base = 0;
  std::uint32_t code_end = 0;
  std::vector<std::uint32_t> starts;  // guest function starts, sorted
};

ImageMap& Image() {
  static ImageMap image;
  return image;
}

bool InCode(std::uint32_t address) {
  const auto& image = Image();
  return address >= image.code_base && address < image.code_end;
}

// --- guest memory, read defensively (the watchdog must never fault) -----------------------------------

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

bool Readable(std::uint32_t address, std::uint32_t size) {
  auto* memory = GuestMemory();
  if (!memory || size == 0 || address + (size - 1) < address) return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap || memory->LookupHeap(address + size - 1) != heap) return false;
  return heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = GuestMemory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

std::uint8_t LoadU8(std::uint32_t address) {
  return *GuestMemory()->TranslateVirtual<const std::uint8_t*>(address);
}

// --- naming threads ---------------------------------------------------------------------------------

std::string ThreadLabel(const rex::system::XThread& thread) {
  if (thread.name().empty()) return fmt::format("thread id {}", thread.thread_id());
  return fmt::format("{} (id {})", thread.name(), thread.thread_id());
}

/// Guest KTHREAD address -> label, for the owners of mutants and critical sections.
std::string ThreadLabelForKthread(std::uint32_t kthread) {
  if (!kthread) return "nobody";
  auto* kernel = rex::system::kernel_state();
  if (kernel) {
    for (const auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
      if (thread->guest_object() == kthread) return ThreadLabel(*thread);
    }
  }
  return fmt::format("KTHREAD 0x{:08X} (no such guest thread)", kthread);
}

std::string ThreadLabelForId(std::uint32_t id) {
  if (!id) return "a host thread";
  auto* kernel = rex::system::kernel_state();
  if (kernel) {
    for (const auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
      if (thread->thread_id() == id) return ThreadLabel(*thread);
    }
  }
  return fmt::format("thread id {}", id);
}

// --- naming wait objects ----------------------------------------------------------------------------

std::string DescribeDispatcher(std::uint32_t ptr) {
  if (!Readable(ptr, 0x1C)) return fmt::format("0x{:08X} (unreadable)", ptr);
  const std::uint8_t type = LoadU8(ptr);
  const auto signal = static_cast<std::int32_t>(LoadU32(ptr + 4));
  const char* state = signal ? "signaled" : "not signaled";
  switch (type) {
    case 0:
      return fmt::format("0x{:08X} (notification event, {})", ptr, state);
    case 1:
      return fmt::format("0x{:08X} (synchronization event, {})", ptr, state);
    case 2:
      // KMUTANT: header, mutant list entry, OwnerThread at 0x18. Signal state 1 = free.
      return signal > 0 ? fmt::format("0x{:08X} (mutant, free)", ptr)
                        : fmt::format("0x{:08X} (mutant, owned by {})", ptr, ThreadLabelForKthread(LoadU32(ptr + 0x18)));
    case 5:
      return fmt::format("0x{:08X} (semaphore, count {} of {})", ptr, signal, LoadU32(ptr + 0x10));
    case 6: {
      std::string who = ThreadLabelForKthread(ptr);
      return fmt::format("0x{:08X} (thread {}, {})", ptr, who, signal ? "ended" : "still running");
    }
    case 8:
      return fmt::format("0x{:08X} (notification timer, {})", ptr, state);
    case 9:
      return fmt::format("0x{:08X} (synchronization timer, {})", ptr, state);
    default:
      return fmt::format("0x{:08X} (dispatcher type {}, signal state {})", ptr, type, signal);
  }
}

const char* ObjectTypeName(rex::system::XObject::Type type) {
  using Type = rex::system::XObject::Type;
  switch (type) {
    case Type::Enumerator: return "enumerator";
    case Type::Event: return "event";
    case Type::File: return "file";
    case Type::IOCompletion: return "I/O completion";
    case Type::Module: return "module";
    case Type::Mutant: return "mutant";
    case Type::NotifyListener: return "notification listener";
    case Type::Semaphore: return "semaphore";
    case Type::Session: return "session";
    case Type::Socket: return "socket";
    case Type::SymbolicLink: return "symbolic link";
    case Type::Thread: return "thread";
    case Type::Timer: return "timer";
    default: return "object";
  }
}

std::string DescribeHandle(std::uint32_t handle) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return fmt::format("handle 0x{:08X}", handle);
  auto object = kernel->object_table()->LookupObject<rex::system::XObject>(handle);
  if (!object) return fmt::format("handle 0x{:08X} (no such object)", handle);
  if (object->type() == rex::system::XObject::Type::Thread) {
    auto* thread = static_cast<rex::system::XThread*>(object.get());
    return fmt::format("handle 0x{:08X} (thread {})", handle, ThreadLabel(*thread));
  }
  if (const std::uint32_t guest = object->guest_object()) {
    return fmt::format("handle 0x{:08X} -> {}", handle, DescribeDispatcher(guest));
  }
  return fmt::format("handle 0x{:08X} ({})", handle, ObjectTypeName(object->type()));
}

std::string DescribeCriticalSection(std::uint32_t ptr) {
  if (!Readable(ptr, 0x1C)) return fmt::format("critical section 0x{:08X} (unreadable)", ptr);
  const std::uint32_t owner = LoadU32(ptr + 0x18);
  const auto recursion = static_cast<std::int32_t>(LoadU32(ptr + 0x14));
  if (!owner) return fmt::format("critical section 0x{:08X} (free)", ptr);
  return fmt::format("critical section 0x{:08X} (owned by {}, recursion {})", ptr, ThreadLabelForKthread(owner),
                     recursion);
}

std::string FormatTimeout(std::int64_t timeout) {
  if (timeout == kNoTimeout) return "no timeout";
  if (timeout == 0) return "poll (zero timeout)";
  if (timeout < 0) {
    const double ms = static_cast<double>(-timeout) / 10000.0;
    return fmt::format("timeout {:.1f} ms", ms);
  }
  return "absolute timeout";
}

std::string FormatSeconds(std::uint64_t ms) {
  return fmt::format("{:.1f} s", static_cast<double>(ms) / 1000.0);
}

/// Traced kernel calls a second from which a thread counts as busy (polling): a game thread that waits for its
/// frame or its data makes a few hundred at most.
constexpr std::uint64_t kBusyCallsPerSecond = 50000;

// --- what the watchdog saw on earlier ticks -----------------------------------------------------------

struct Observed {
  std::uint64_t calls = 0;
  std::uint64_t quiet_since_ms = 0;  // first tick at which `calls` had its current value
  std::uint32_t wait_seq = 0;
  std::uint32_t wait_export = ThreadActivity::kNoExport;
  std::uint64_t wait_seen_ms = 0;  // first tick at which this wait was seen
  std::uint64_t calls_last_tick = 0;
  std::array<std::uint32_t, kMaxTrackedExports> per_export{};
  std::array<std::uint32_t, kMaxTrackedExports> per_export_tick{};  // this thread's calls per export, last tick
};

struct ObservedState {
  std::mutex mutex;
  bool ticked = false;
  std::uint64_t last_tick_ms = 0;
  std::uint64_t tick_ms = 0;  // length of the last tick (0 before the second one)
  std::map<const ThreadActivity*, Observed> threads;
  std::array<std::uint64_t, kMaxTrackedExports> per_export_last_tick{};
};

ObservedState& State() {
  static ObservedState state;
  return state;
}

ThreadActivity* FindActivity(std::uint32_t thread_id) {
  if (!thread_id) return nullptr;
  std::lock_guard<std::mutex> lock(RecordsMutex());
  for (const auto& record : Records()) {
    if (record->thread_id == thread_id) return record.get();
  }
  return nullptr;
}

}  // namespace

// --- records ------------------------------------------------------------------------------------------

ThreadActivity& Current() {
  thread_local ThreadActivity* record = nullptr;
  if (!record) record = NewRecord();
  return *record;
}

std::vector<ThreadActivity*> AllThreads() {
  std::lock_guard<std::mutex> lock(RecordsMutex());
  std::vector<ThreadActivity*> all;
  all.reserve(Records().size());
  for (const auto& record : Records()) all.push_back(record.get());
  return all;
}

WaitScope::WaitScope(ThreadActivity& t, std::uint32_t export_id, WaitKind kind, std::uint32_t lr,
                     std::int64_t timeout, const std::uint32_t* objects, std::uint32_t count)
    : t_(t) {
  constexpr auto relaxed = std::memory_order_relaxed;
  outer_export_ = t.wait_export.load(relaxed);
  outer_kind_ = t.wait_kind.load(relaxed);
  outer_lr_ = t.wait_lr.load(relaxed);
  outer_count_ = t.wait_count.load(relaxed);
  for (std::uint32_t i = 0; i < kMaxWaitObjects; ++i) outer_objects_[i] = t.wait_objects[i].load(relaxed);
  outer_timeout_ = t.wait_timeout.load(relaxed);

  t.wait_kind.store(kind, relaxed);
  t.wait_lr.store(lr, relaxed);
  t.wait_timeout.store(timeout, relaxed);
  t.wait_count.store(count, relaxed);
  for (std::uint32_t i = 0; i < kMaxWaitObjects; ++i) t.wait_objects[i].store(i < count && objects ? objects[i] : 0, relaxed);
  t.wait_seq.store(t.wait_seq.load(relaxed) + 1, relaxed);
  t.wait_export.store(export_id, std::memory_order_release);
}

WaitScope::~WaitScope() {
  constexpr auto relaxed = std::memory_order_relaxed;
  if (outer_export_ == ThreadActivity::kNoExport) {
    t_.wait_export.store(ThreadActivity::kNoExport, std::memory_order_release);
    return;
  }
  // Back in the outer wait (an APC that waited has returned).
  t_.wait_kind.store(outer_kind_, relaxed);
  t_.wait_lr.store(outer_lr_, relaxed);
  t_.wait_timeout.store(outer_timeout_, relaxed);
  t_.wait_count.store(outer_count_, relaxed);
  for (std::uint32_t i = 0; i < kMaxWaitObjects; ++i) t_.wait_objects[i].store(outer_objects_[i], relaxed);
  t_.wait_seq.store(t_.wait_seq.load(relaxed) + 1, relaxed);
  t_.wait_export.store(outer_export_, std::memory_order_release);
}

Counters& counters() {
  static Counters counters;
  return counters;
}

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - StartTime()).count());
}

// --- file events --------------------------------------------------------------------------------------

void NoteFileEvent(std::string_view api, std::string_view path, std::uint32_t status, std::uint32_t lr) {
  FileEvent event;
  event.ms = NowMs();
  event.thread_id = Current().thread_id;
  event.lr = lr;
  event.status = status;
  event.api = std::string(api);
  event.path = std::string(path);
  std::lock_guard<std::mutex> lock(FileEventsMutex());
  auto& events = FileEvents();
  events.push_back(std::move(event));
  while (events.size() > kRecentFileEvents) events.pop_front();
}

std::vector<FileEvent> RecentFileEvents() {
  std::lock_guard<std::mutex> lock(FileEventsMutex());
  return {FileEvents().begin(), FileEvents().end()};
}

std::string_view StatusName(std::uint32_t status) {
  switch (status) {
    case 0x00000000: return "STATUS_SUCCESS";
    case 0x00000080: return "STATUS_ABANDONED";
    case 0x00000101: return "STATUS_ALERTED";
    case 0x00000102: return "STATUS_TIMEOUT";
    case 0x00000103: return "STATUS_PENDING";
    case 0x000000C0: return "STATUS_USER_APC";
    case 0x80000006: return "STATUS_NO_MORE_FILES";
    case 0xC0000002: return "STATUS_NOT_IMPLEMENTED";
    case 0xC0000003: return "STATUS_INVALID_INFO_CLASS";
    case 0xC0000004: return "STATUS_INFO_LENGTH_MISMATCH";
    case 0xC0000005: return "STATUS_ACCESS_VIOLATION";
    case 0xC0000008: return "STATUS_INVALID_HANDLE";
    case 0xC000000D: return "STATUS_INVALID_PARAMETER";
    case 0xC000000E: return "STATUS_NO_SUCH_DEVICE";
    case 0xC000000F: return "STATUS_NO_SUCH_FILE";
    case 0xC0000010: return "STATUS_INVALID_DEVICE_REQUEST";
    case 0xC0000011: return "STATUS_END_OF_FILE";
    case 0xC0000013: return "STATUS_NO_MEDIA_IN_DEVICE";
    case 0xC0000017: return "STATUS_NO_MEMORY";
    case 0xC0000022: return "STATUS_ACCESS_DENIED";
    case 0xC0000033: return "STATUS_OBJECT_NAME_INVALID";
    case 0xC0000034: return "STATUS_OBJECT_NAME_NOT_FOUND";
    case 0xC0000035: return "STATUS_OBJECT_NAME_COLLISION";
    case 0xC000003A: return "STATUS_OBJECT_PATH_NOT_FOUND";
    case 0xC000009A: return "STATUS_INSUFFICIENT_RESOURCES";
    case 0xC00000BA: return "STATUS_FILE_IS_A_DIRECTORY";
    case 0xC00000BB: return "STATUS_NOT_SUPPORTED";
    default: return {};
  }
}

std::string FormatStatus(std::uint32_t status) {
  const auto name = StatusName(status);
  if (name.empty()) return fmt::format("0x{:08X}", status);
  return fmt::format("0x{:08X} ({})", status, name);
}

std::string CurrentThreadLabel() {
  if (rex::system::XThread::IsInThread()) {
    if (auto* thread = rex::system::XThread::GetCurrentThread()) return ThreadLabel(*thread);
  }
  return "a host thread";
}

// --- image --------------------------------------------------------------------------------------------

void SetImage(const rex::PPCImageInfo& image) {
  auto& map = Image();
  map.code_base = image.code_base;
  map.code_end = image.code_base + image.code_size;
  map.starts.clear();
  for (const PPCFuncMapping* m = image.func_mappings; m && m->host; ++m) {
    map.starts.push_back(static_cast<std::uint32_t>(m->guest));
  }
  std::sort(map.starts.begin(), map.starts.end());
  map.starts.erase(std::unique(map.starts.begin(), map.starts.end()), map.starts.end());
}

namespace {

/// `address` named by the function holding `lookup` (the image's function starts are sorted).
std::string NameInFunction(std::uint32_t address, std::uint32_t lookup) {
  const auto& map = Image();
  if (!InCode(address) || !InCode(lookup) || map.starts.empty()) return fmt::format("0x{:08X}", address);
  auto it = std::upper_bound(map.starts.begin(), map.starts.end(), lookup);
  if (it == map.starts.begin()) return fmt::format("0x{:08X}", address);
  const std::uint32_t start = *(it - 1);
  if (address == start) return fmt::format("0x{:08X} (sub_{:08X})", address, start);
  return fmt::format("0x{:08X} (sub_{:08X}+0x{:X})", address, start, address - start);
}

}  // namespace

std::string DescribeGuestAddress(std::uint32_t address) {
  // A return address: the call that put it there is the instruction before it, which can be the last
  // instruction of its function (a call that does not return), so the function holding address - 4.
  return NameInFunction(address, address - 4);
}

std::string DescribeGuestCode(std::uint32_t address) {
  return NameInFunction(address, address);
}

std::vector<std::uint32_t> GuestStack(rex::system::XThread& thread, std::size_t max_frames) {
  std::vector<std::uint32_t> frames;
  auto* state = thread.thread_state();
  if (!state || !GuestMemory() || !thread.guest_object()) return frames;
  const PPCContext* ctx = state->context();
  if (!ctx) return frames;
  const auto* kthread = thread.guest_object<rex::system::X_KTHREAD>();
  const std::uint32_t stack_high = kthread ? static_cast<std::uint32_t>(kthread->stack_base) : 0;
  const std::uint32_t stack_low = kthread ? static_cast<std::uint32_t>(kthread->stack_limit) : 0;
  // A racy read of another thread's registers: good enough for a diagnostic, never dereferenced
  // outside the thread's own stack.
  const auto lr = static_cast<std::uint32_t>(ctx->lr);
  std::uint32_t sp = ctx->r1.u32;
  if (InCode(lr)) frames.push_back(lr);
  if (stack_low >= stack_high) return frames;
  for (std::size_t depth = 0; depth < 64 && frames.size() < max_frames; ++depth) {
    if (sp < stack_low || sp + 4 > stack_high || (sp & 3)) break;
    const std::uint32_t back = LoadU32(sp);
    // The back chain points to the caller's frame, higher up the stack.
    if (back <= sp || back > stack_high || (back & 3) || back - 8 < stack_low) break;
    const std::uint32_t ret = LoadU32(back - 8);
    if (InCode(ret) && (frames.empty() || frames.back() != ret)) frames.push_back(ret);
    sp = back;
  }
  return frames;
}

// --- watchdog side --------------------------------------------------------------------------------------

void Observe(std::uint64_t now_ms) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::array<std::uint64_t, kMaxTrackedExports> tick{};
  for (ThreadActivity* t : AllThreads()) {
    auto [it, inserted] = state.threads.try_emplace(t);
    Observed& seen = it->second;
    const std::uint64_t calls = t->calls.load(std::memory_order_relaxed);
    if (inserted) seen.quiet_since_ms = now_ms;
    seen.calls_last_tick = calls - seen.calls;
    if (calls != seen.calls) seen.quiet_since_ms = now_ms;
    seen.calls = calls;
    for (std::uint32_t i = 0; i < kMaxTrackedExports; ++i) {
      const std::uint32_t now_count = t->per_export[i].load(std::memory_order_relaxed);
      const auto delta = static_cast<std::uint32_t>(now_count - seen.per_export[i]);
      seen.per_export_tick[i] = inserted ? 0 : delta;
      tick[i] += delta;
      seen.per_export[i] = now_count;
    }
    const std::uint32_t wait_export = t->wait_export.load(std::memory_order_acquire);
    const std::uint32_t wait_seq = t->wait_seq.load(std::memory_order_relaxed);
    if (wait_export != seen.wait_export || wait_seq != seen.wait_seq) seen.wait_seen_ms = now_ms;
    seen.wait_export = wait_export;
    seen.wait_seq = wait_seq;
  }
  state.per_export_last_tick = tick;
  state.tick_ms = state.ticked && now_ms > state.last_tick_ms ? now_ms - state.last_tick_ms : 0;
  state.last_tick_ms = now_ms;
  state.ticked = true;
}

std::uint64_t ProgressFingerprint() {
  const auto& c = counters();
  // All four only grow, so their sum changes exactly when one of them does.
  return c.files_opened.load(std::memory_order_relaxed) + c.bytes_read.load(std::memory_order_relaxed) +
         c.frames_presented.load(std::memory_order_relaxed) + c.threads_created.load(std::memory_order_relaxed);
}

std::string ActivityLine(const GpuState& gpu) {
  const auto& c = counters();
  constexpr auto relaxed = std::memory_order_relaxed;
  std::uint64_t calls = 0;
  for (ThreadActivity* t : AllThreads()) calls += t->calls.load(relaxed);
  const double mb = static_cast<double>(c.bytes_read.load(relaxed)) / (1024.0 * 1024.0);
  std::string gpu_part = fmt::format(
      "gpu: engines {}, ring buffer {}, interrupt callback {}", c.engines_initialized.load(relaxed) ? "initialized" : "not initialized",
      c.ring_buffer.load(relaxed) ? fmt::format("0x{:08X}", c.ring_buffer.load(relaxed)) : std::string("not set up"),
      c.interrupt_callback.load(relaxed) ? fmt::format("0x{:08X}", c.interrupt_callback.load(relaxed))
                                         : std::string("none"));
  if (gpu.available) {
    gpu_part += fmt::format(", vblanks {}, ring write pointer 0x{:X}", gpu.vblanks, gpu.ring_write_pointer);
  } else {
    gpu_part += ", no GPU emulation";
  }
  // "10760 (0 with draws, 0 with resolves; draws 0, ...; front buffer 1280x720)": what the frames asked for.
  std::string frames = std::to_string(c.frames_presented.load(relaxed));
  if (const std::string detail = ttt2::frames::Describe(); !detail.empty()) frames += " (" + detail + ")";
  return fmt::format(
      "activity: files {} opened, {} failed ({} device paths); read {:.1f} MB in {} reads ({} failed); sector "
      "queries {}; missing names {}; threads created {}; {}; frames presented {}; audio clients {}, audio frames {}; "
      "input polls {}; traced kernel calls {}; {}",
      c.files_opened.load(relaxed), c.open_failures.load(relaxed), c.device_open_failures.load(relaxed), mb,
      c.reads.load(relaxed), c.read_failures.load(relaxed), c.sector_queries.load(relaxed),
      c.not_found_answers.load(relaxed), c.threads_created.load(relaxed), gpu_part, frames, c.audio_clients.load(relaxed),
      c.audio_frames.load(relaxed), c.input_polls.load(relaxed), calls, kernel_clock::Describe());
}

std::vector<std::string> DescribeThread(rex::system::XThread& thread, std::uint64_t now_ms) {
  std::vector<std::string> lines;
  ThreadActivity* t = FindActivity(thread.thread_id());
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const Observed* seen = nullptr;
  if (t) {
    auto it = state.threads.find(t);
    if (it != state.threads.end()) seen = &it->second;
  }

  // The wait it is in.
  if (!t) {
    lines.push_back("    wait: not traced (this thread has made no traced kernel call)");
  } else if (const std::uint32_t wait = t->wait_export.load(std::memory_order_acquire); wait == ThreadActivity::kNoExport) {
    lines.push_back("    wait: none (running guest code, or blocked outside the traced kernel calls)");
  } else {
    const WaitKind kind = t->wait_kind.load(std::memory_order_relaxed);
    const std::uint32_t count = t->wait_count.load(std::memory_order_relaxed);
    std::string objects;
    for (std::uint32_t i = 0; i < count && i < kMaxWaitObjects; ++i) {
      const std::uint32_t object = t->wait_objects[i].load(std::memory_order_relaxed);
      if (!objects.empty()) objects += ", ";
      switch (kind) {
        case WaitKind::kDispatcher: objects += DescribeDispatcher(object); break;
        case WaitKind::kHandles: objects += DescribeHandle(object); break;
        case WaitKind::kCriticalSection: objects += DescribeCriticalSection(object); break;
        default: break;
      }
    }
    if (count > kMaxWaitObjects) objects += fmt::format(" and {} more", count - kMaxWaitObjects);
    std::string what = WrappedExportName(wait);
    if (kind != WaitKind::kDelay) what += objects.empty() ? std::string(" (no object)") : " on " + objects;
    std::string how_long;
    if (seen && seen->wait_export == wait && seen->wait_seq == t->wait_seq.load(std::memory_order_relaxed)) {
      how_long = ", for at least " + FormatSeconds(now_ms - seen->wait_seen_ms);
    }
    lines.push_back(fmt::format("    wait: {}, {}{}, called from {}", what,
                                FormatTimeout(t->wait_timeout.load(std::memory_order_relaxed)), how_long,
                                DescribeGuestAddress(t->wait_lr.load(std::memory_order_relaxed))));
  }

  // Its traced kernel calls: how many in the last tick and which, in total, the last one.
  if (t) {
    const std::uint64_t calls = t->calls.load(std::memory_order_relaxed);
    const std::uint32_t last_lr = t->last_lr.load(std::memory_order_relaxed);
    std::string recent;
    std::string mostly;
    if (seen) {
      std::vector<std::pair<std::uint32_t, std::uint32_t>> busiest;  // calls, export id
      for (std::uint32_t i = 0; i < kMaxTrackedExports; ++i) {
        if (seen->per_export_tick[i]) busiest.emplace_back(seen->per_export_tick[i], i);
      }
      std::sort(busiest.rbegin(), busiest.rend());
      std::string top;
      for (std::size_t i = 0; i < busiest.size() && i < 3; ++i) {
        top += fmt::format("{}{} {}", i ? ", " : "", WrappedExportName(busiest[i].second), busiest[i].first);
      }
      if (!busiest.empty()) mostly = WrappedExportName(busiest[0].second);
      recent = fmt::format("{} in the last tick{}, ", seen->calls_last_tick, top.empty() ? "" : " (" + top + ")");
      if (seen->calls == calls && now_ms > seen->quiet_since_ms) {
        recent += fmt::format("none for at least {}, ", FormatSeconds(now_ms - seen->quiet_since_ms));
      }
    }
    lines.push_back(fmt::format("    kernel calls: {}{} in total; last {} from {}", recent, calls,
                                WrappedExportName(t->last_export.load(std::memory_order_relaxed)),
                                DescribeGuestAddress(last_lr)));
    // A thread that keeps calling the kernel without waiting for long: a polling loop. TTT2 Web documents one
    // (sub_82ABFDE8 signals its own event and waits on it until a 64-bit counter changes) that spins forever
    // when the kernel clock stands still.
    if (seen && state.tick_ms) {
      const std::uint64_t per_second = seen->calls_last_tick * 1000 / state.tick_ms;
      if (per_second >= kBusyCallsPerSecond) {
        lines.push_back(fmt::format(
            "    busy: {} traced kernel calls a second over the last {} (mostly {}), last from {}: a loop that keeps "
            "calling the kernel without waiting for long",
            per_second, FormatSeconds(state.tick_ms), mostly.empty() ? "?" : mostly, DescribeGuestAddress(last_lr)));
      }
    }
  }

  // Its guest call stack.
  const auto frames = GuestStack(thread, 12);
  if (frames.empty()) {
    lines.push_back("    guest stack: (no frames: the thread has not called into the game's code yet, or its stack is not readable)");
  } else {
    std::string stack;
    for (std::uint32_t frame : frames) {
      if (!stack.empty()) stack += " <- ";
      stack += DescribeGuestAddress(frame);
    }
    lines.push_back("    guest stack: " + stack);
  }
  return lines;
}

std::vector<std::string> DescribeProcess(std::uint64_t now_ms) {
  std::vector<std::string> lines;
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.ticked) {
      std::vector<std::pair<std::uint64_t, std::uint32_t>> busiest;
      for (std::uint32_t i = 0; i < kMaxTrackedExports; ++i) {
        if (state.per_export_last_tick[i]) busiest.emplace_back(state.per_export_last_tick[i], i);
      }
      std::sort(busiest.rbegin(), busiest.rend());
      std::string text;
      for (std::size_t i = 0; i < busiest.size() && i < 8; ++i) {
        if (!text.empty()) text += ", ";
        text += fmt::format("{} {}", WrappedExportName(busiest[i].second), busiest[i].first);
      }
      lines.push_back(text.empty() ? "  traced kernel calls in the last tick: none"
                                   : "  traced kernel calls in the last tick: " + text);
    }
  }
  const auto events = RecentFileEvents();
  if (events.empty()) {
    lines.push_back("  recent file activity: none");
  } else {
    lines.push_back(fmt::format("  recent file activity (last {}, newest last):", events.size()));
    for (const auto& e : events) {
      lines.push_back(fmt::format("    t+{} {} '{}' -> {}, {}, from {}", FormatSeconds(e.ms), e.api, e.path,
                                  FormatStatus(e.status), ThreadLabelForId(e.thread_id), DescribeGuestAddress(e.lr)));
    }
  }
  (void)now_ms;
  return lines;
}

}  // namespace ttt2::monitor
