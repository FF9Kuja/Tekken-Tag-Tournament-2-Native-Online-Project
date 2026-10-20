// Host wrappers of the runtime's blocking and synchronization exports: each records, for the duration of a
// wait, what the calling guest thread waits on (the objects, the timeout, the guest address it was called from),
// so the watchdog's stall dump can say which thread waits for what - and who owns it, for mutants and critical
// sections - instead of only that nothing moves. They also keep:
//  * per thread the shape of its calls (export, call site, objects, statuses: boot_monitor.h CallSignature), which
//    shows a polling loop's structure;
//  * the critical sections each thread holds and the call sites of the enters it has not left (lock_tracker.h);
//  * what each object created by handle is and who signals it (object_tracker.h): the runtime zeroes those
//    objects' guest headers, so a dump that read the header called every event "notification, not signaled".
// The runtime's own function does the waiting. One semantic fix: NtSignalAndWaitForSingleObjectEx's Alertable flag
// (see that wrapper).
//
// Registers follow the Xbox 360 calling convention: arguments in r3..r10, the result in r3.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include "boot_monitor.h"
#include "lock_tracker.h"
#include "object_tracker.h"
#include "runtime_exports.h"

namespace {

using ttt2::monitor::kMaxWaitObjects;
using ttt2::monitor::kNoTimeout;
using ttt2::monitor::NoteCall;
using ttt2::monitor::NoteSignature;
using ttt2::monitor::NoteStatus;
using ttt2::monitor::WaitKind;
using ttt2::monitor::WaitScope;

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = rex::system::kernel_memory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

/// A guest word that may not be mapped (an out pointer the call wrote): 0 when unreadable.
std::uint32_t LoadU32Checked(std::uint32_t address) {
  return address && ttt2::monitor::GuestReadable(address, 4) ? LoadU32(address) : 0;
}

/// A guest LARGE_INTEGER timeout (100 ns units, negative = relative); kNoTimeout for a null pointer.
std::int64_t ReadTimeout(std::uint32_t pointer) {
  if (!pointer) return kNoTimeout;
  return static_cast<std::int64_t>((std::uint64_t(LoadU32(pointer)) << 32) | LoadU32(pointer + 4));
}

/// The first kMaxWaitObjects entries of a guest array of 32-bit words (pointers or handles).
std::array<std::uint32_t, kMaxWaitObjects> ReadObjects(std::uint32_t array, std::uint32_t count) {
  std::array<std::uint32_t, kMaxWaitObjects> objects{};
  for (std::uint32_t i = 0; array && i < count && i < kMaxWaitObjects; ++i) objects[i] = LoadU32(array + 4 * i);
  return objects;
}

std::uint32_t ThreadId() {
  return ttt2::monitor::Current().thread_id;
}

/// Logs a line once per distinct key (call sites, mostly), at most `limit` keys.
bool FirstTime(std::uint64_t key, std::size_t limit = 64) {
  static std::mutex mutex;
  static std::set<std::uint64_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.size() >= limit && !seen.count(key)) return false;
  return seen.insert(key).second;
}

/// The calling thread holds `section` one level deeper: recorded, with its call stack at depths 1, 2, 4, ...
void Entered(const PPCContext& ctx, std::uint32_t section, std::uint32_t lr) {
  const std::uint32_t tid = ThreadId();
  const std::uint64_t now = ttt2::monitor::NowMs();
  const std::uint32_t depth = ttt2::locks::NoteEnter(tid, section, lr, now);
  if (ttt2::locks::SnapshotDepth(depth)) {
    ttt2::locks::NoteStack(tid, section, depth, now, ttt2::monitor::CurrentGuestStack(ctx.r1.u32, lr, 10));
  }
}

/// TTT2_SIGNAL_AND_WAIT_ALERTABLE=runtime keeps the runtime's reading of the arguments (see the wrapper).
bool RuntimeSignalAndWaitAlertable() {
  static const bool runtime = [] {
    const char* value = std::getenv("TTT2_SIGNAL_AND_WAIT_ALERTABLE");
    return value && std::string_view(value) == "runtime";
  }();
  return runtime;
}

}  // namespace

// KeWaitForSingleObject(object r3, wait reason r4, processor mode r5, alertable r6, timeout r7).
TTT2_WRAPPED_EXPORT(KeWaitForSingleObject) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeWaitForSingleObject);
  const std::uint32_t object = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, object);
  {
    WaitScope wait(t, id, WaitKind::kDispatcher, lr, ReadTimeout(ctx.r7.u32), &object, 1);
    TTT2_RUNTIME_ORIGINAL(KeWaitForSingleObject)(ctx, base);
  }
  NoteStatus(sig, ctx.r3.u32);
}

// KeWaitForMultipleObjects(count r3, objects r4, wait type r5, reason r6, mode r7, alertable r8,
//                          timeout r9, wait blocks r10).
TTT2_WRAPPED_EXPORT(KeWaitForMultipleObjects) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeWaitForMultipleObjects);
  const std::uint32_t count = ctx.r3.u32;
  const auto objects = ReadObjects(ctx.r4.u32, count);
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, objects[0]);
  {
    WaitScope wait(t, id, WaitKind::kDispatcher, lr, ReadTimeout(ctx.r9.u32), objects.data(), count);
    TTT2_RUNTIME_ORIGINAL(KeWaitForMultipleObjects)(ctx, base);
  }
  // STATUS_WAIT_0 + n names the object that satisfied a wait-any: a status, not a failure.
  if (ctx.r3.u32 >= count) NoteStatus(sig, ctx.r3.u32);
}

// NtWaitForSingleObjectEx(handle r3, wait mode r4, alertable r5, timeout r6).
TTT2_WRAPPED_EXPORT(NtWaitForSingleObjectEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtWaitForSingleObjectEx);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle);
  {
    WaitScope wait(t, id, WaitKind::kHandles, lr, ReadTimeout(ctx.r6.u32), &handle, 1);
    TTT2_RUNTIME_ORIGINAL(NtWaitForSingleObjectEx)(ctx, base);
  }
  NoteStatus(sig, ctx.r3.u32);
}

// NtWaitForMultipleObjectsEx(count r3, handles r4, wait type r5, wait mode r6, alertable r7, timeout r8).
TTT2_WRAPPED_EXPORT(NtWaitForMultipleObjectsEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtWaitForMultipleObjectsEx);
  const std::uint32_t count = ctx.r3.u32;
  const auto handles = ReadObjects(ctx.r4.u32, count);
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handles[0]);
  {
    WaitScope wait(t, id, WaitKind::kHandles, lr, ReadTimeout(ctx.r8.u32), handles.data(), count);
    TTT2_RUNTIME_ORIGINAL(NtWaitForMultipleObjectsEx)(ctx, base);
  }
  if (ctx.r3.u32 >= count) NoteStatus(sig, ctx.r3.u32);
}

// NtSignalAndWaitForSingleObjectEx(signal handle r3, wait handle r4, wait mode r5, alertable r6, timeout r7).
//
// The Xbox 360 kernel's Ex waits take the processor mode before the alertable flag (NtWaitForSingleObjectEx:
// handle, mode, alertable, timeout - which the runtime reads right; TTT2 Web documents this one as
// "signal, wait, waitMode, alertable, timeout"). The runtime (as Xenia) reads r5 as Alertable and ignores r6, so a
// call with WaitMode = UserMode (1) - what the game's SignalObjectAndWait passes - waits alertably even when the
// game asked for a non-alertable wait, and a user APC queued to the thread is then delivered inside it, where the
// console never delivers one. The wrapper hands the runtime the game's real Alertable flag (r6) where the runtime
// reads it (r5); TTT2_SIGNAL_AND_WAIT_ALERTABLE=runtime keeps the runtime's reading, for comparison. The first
// call from each call site logs both values.
TTT2_WRAPPED_EXPORT(NtSignalAndWaitForSingleObjectEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtSignalAndWaitForSingleObjectEx);
  const std::uint32_t signal = ctx.r3.u32;
  const std::uint32_t handle = ctx.r4.u32;
  const std::uint32_t wait_mode = ctx.r5.u32;
  const std::uint32_t alertable = ctx.r6.u32;
  if (FirstTime((std::uint64_t(id) << 32) | lr)) {
    REXLOG_INFO(
        "ttt2: NtSignalAndWaitForSingleObjectEx from {}: wait mode {}, alertable {} (the runtime reads the wait mode "
        "as the alertable flag; {})",
        ttt2::monitor::DescribeGuestAddress(lr), wait_mode, alertable,
        RuntimeSignalAndWaitAlertable() ? "TTT2_SIGNAL_AND_WAIT_ALERTABLE=runtime: left as it reads it"
                                        : "the host passes it the alertable flag");
  }
  if (!RuntimeSignalAndWaitAlertable()) ctx.r5.u64 = alertable;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle, signal);
  ttt2::objects::NoteSignal(signal, t.thread_id, lr, ttt2::monitor::NowMs());
  {
    WaitScope wait(t, id, WaitKind::kHandles, lr, ReadTimeout(ctx.r7.u32), &handle, 1);
    TTT2_RUNTIME_ORIGINAL(NtSignalAndWaitForSingleObjectEx)(ctx, base);
  }
  NoteStatus(sig, ctx.r3.u32);
}

// KeDelayExecutionThread(processor mode r3, alertable r4, interval r5): a sleep.
TTT2_WRAPPED_EXPORT(KeDelayExecutionThread) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeDelayExecutionThread);
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, 0);
  {
    WaitScope wait(t, id, WaitKind::kDelay, lr, ReadTimeout(ctx.r5.u32), nullptr, 0);
    TTT2_RUNTIME_ORIGINAL(KeDelayExecutionThread)(ctx, base);
  }
  NoteStatus(sig, ctx.r3.u32);
}

// RtlEnterCriticalSection(critical section r3): blocks while another thread owns it.
TTT2_WRAPPED_EXPORT(RtlEnterCriticalSection) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(RtlEnterCriticalSection);
  const std::uint32_t section = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  NoteSignature(t, id, lr, section);
  {
    WaitScope wait(t, id, WaitKind::kCriticalSection, lr, kNoTimeout, &section, 1);
    TTT2_RUNTIME_ORIGINAL(RtlEnterCriticalSection)(ctx, base);
  }
  Entered(ctx, section, lr);
}

// RtlTryEnterCriticalSection(critical section r3) -> 1 when the section was entered.
TTT2_WRAPPED_EXPORT(RtlTryEnterCriticalSection) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(RtlTryEnterCriticalSection);
  const std::uint32_t section = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, section);
  TTT2_RUNTIME_ORIGINAL(RtlTryEnterCriticalSection)(ctx, base);
  if (ctx.r3.u32 == 1) {
    Entered(ctx, section, lr);
  } else {
    NoteStatus(sig, 0x00000102u);  // not entered: counted like a timeout
  }
}

// RtlLeaveCriticalSection(critical section r3).
TTT2_WRAPPED_EXPORT(RtlLeaveCriticalSection) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(RtlLeaveCriticalSection);
  const std::uint32_t section = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  NoteSignature(t, id, lr, section);
  TTT2_RUNTIME_ORIGINAL(RtlLeaveCriticalSection)(ctx, base);
  ttt2::locks::NoteLeave(t.thread_id, section, ttt2::monitor::NowMs());
}

// --- objects created by handle, and their signals ---------------------------------------------------------

// NtCreateEvent(handle out r3, object attributes r4, event type r5 (0 notification, 1 synchronization), initial r6).
TTT2_WRAPPED_EXPORT(NtCreateEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t out = ctx.r3.u32;
  const std::uint32_t type = ctx.r5.u32;
  const std::uint32_t initial = ctx.r6.u32;
  auto& t = NoteCall(TTT2_EXPORT_ID(NtCreateEvent), lr);
  TTT2_RUNTIME_ORIGINAL(NtCreateEvent)(ctx, base);
  if (ctx.r3.u32 == 0 || ctx.r3.u32 == 0x40000000u) {  // STATUS_SUCCESS, STATUS_OBJECT_NAME_EXISTS
    if (const std::uint32_t handle = LoadU32Checked(out)) {
      ttt2::objects::NoteCreate(handle,
                                type ? ttt2::objects::Kind::kSynchronizationEvent : ttt2::objects::Kind::kNotificationEvent,
                                initial ? 1 : 0, 0, t.thread_id, lr, ttt2::monitor::NowMs());
    }
  }
}

// NtCreateMutant(handle out r3, object attributes r4, initial owner r5).
TTT2_WRAPPED_EXPORT(NtCreateMutant) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t out = ctx.r3.u32;
  const std::uint32_t owner = ctx.r5.u32;
  auto& t = NoteCall(TTT2_EXPORT_ID(NtCreateMutant), lr);
  TTT2_RUNTIME_ORIGINAL(NtCreateMutant)(ctx, base);
  if (ctx.r3.u32 == 0 || ctx.r3.u32 == 0x40000000u) {
    if (const std::uint32_t handle = LoadU32Checked(out)) {
      ttt2::objects::NoteCreate(handle, ttt2::objects::Kind::kMutant, owner ? 1 : 0, 0, t.thread_id, lr,
                                ttt2::monitor::NowMs());
    }
  }
}

// NtCreateSemaphore(handle out r3, object attributes r4, initial count r5, maximum count r6).
TTT2_WRAPPED_EXPORT(NtCreateSemaphore) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t out = ctx.r3.u32;
  const std::uint32_t count = ctx.r5.u32;
  const std::uint32_t maximum = ctx.r6.u32;
  auto& t = NoteCall(TTT2_EXPORT_ID(NtCreateSemaphore), lr);
  TTT2_RUNTIME_ORIGINAL(NtCreateSemaphore)(ctx, base);
  if (ctx.r3.u32 == 0 || ctx.r3.u32 == 0x40000000u) {
    if (const std::uint32_t handle = LoadU32Checked(out)) {
      ttt2::objects::NoteCreate(handle, ttt2::objects::Kind::kSemaphore, count, maximum, t.thread_id, lr,
                                ttt2::monitor::NowMs());
    }
  }
}

// NtSetEvent(handle r3, previous state out r4).
TTT2_WRAPPED_EXPORT(NtSetEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtSetEvent);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle);
  ttt2::objects::NoteSignal(handle, t.thread_id, lr, ttt2::monitor::NowMs());
  TTT2_RUNTIME_ORIGINAL(NtSetEvent)(ctx, base);
  NoteStatus(sig, ctx.r3.u32);
}

// NtPulseEvent(handle r3, previous state out r4).
TTT2_WRAPPED_EXPORT(NtPulseEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtPulseEvent);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle);
  ttt2::objects::NoteSignal(handle, t.thread_id, lr, ttt2::monitor::NowMs());
  TTT2_RUNTIME_ORIGINAL(NtPulseEvent)(ctx, base);
  NoteStatus(sig, ctx.r3.u32);
}

// NtClearEvent(handle r3).
TTT2_WRAPPED_EXPORT(NtClearEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtClearEvent);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  NoteSignature(t, id, lr, handle);
  ttt2::objects::NoteClear(handle);
  TTT2_RUNTIME_ORIGINAL(NtClearEvent)(ctx, base);
}

// NtReleaseMutant(handle r3, previous count out r4).
TTT2_WRAPPED_EXPORT(NtReleaseMutant) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtReleaseMutant);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle);
  ttt2::objects::NoteSignal(handle, t.thread_id, lr, ttt2::monitor::NowMs());
  TTT2_RUNTIME_ORIGINAL(NtReleaseMutant)(ctx, base);
  NoteStatus(sig, ctx.r3.u32);
}

// NtReleaseSemaphore(handle r3, release count r4, previous count out r5).
TTT2_WRAPPED_EXPORT(NtReleaseSemaphore) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtReleaseSemaphore);
  const std::uint32_t handle = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  auto& sig = NoteSignature(t, id, lr, handle);
  ttt2::objects::NoteSignal(handle, t.thread_id, lr, ttt2::monitor::NowMs());
  TTT2_RUNTIME_ORIGINAL(NtReleaseSemaphore)(ctx, base);
  NoteStatus(sig, ctx.r3.u32);
}

// KeSetEvent(event r3 (a guest dispatcher header), increment r4, wait r5).
TTT2_WRAPPED_EXPORT(KeSetEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeSetEvent);
  const std::uint32_t event = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  NoteSignature(t, id, lr, event);
  ttt2::objects::NoteSignal(event, t.thread_id, lr, ttt2::monitor::NowMs());
  TTT2_RUNTIME_ORIGINAL(KeSetEvent)(ctx, base);
}

// KeResetEvent(event r3).
TTT2_WRAPPED_EXPORT(KeResetEvent) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeResetEvent);
  const std::uint32_t event = ctx.r3.u32;
  auto& t = NoteCall(id, lr);
  NoteSignature(t, id, lr, event);
  ttt2::objects::NoteClear(event);
  TTT2_RUNTIME_ORIGINAL(KeResetEvent)(ctx, base);
}

// NtClose(handle r3): a closed handle's record is dropped (handles are reused).
TTT2_WRAPPED_EXPORT(NtClose) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t handle = ctx.r3.u32;
  NoteCall(TTT2_EXPORT_ID(NtClose), lr);
  TTT2_RUNTIME_ORIGINAL(NtClose)(ctx, base);
  if (ctx.r3.u32 == 0) ttt2::objects::NoteClose(handle);
}

// NtQueueApcThread(thread handle r3, routine r4, context r5, argument 1 r6, argument 2 r7). Neither reference port
// delivers user APCs at all (both answer this call without queueing anything) and the game runs there; the runtime
// queues them, to be delivered at the thread's next alertable wait. Each one is counted and the first ones logged.
TTT2_WRAPPED_EXPORT(NtQueueApcThread) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t thread = ctx.r3.u32;
  const std::uint32_t routine = ctx.r4.u32;
  const std::uint32_t context = ctx.r5.u32;
  NoteCall(TTT2_EXPORT_ID(NtQueueApcThread), lr);
  const std::uint64_t n = ttt2::monitor::counters().apcs_queued.fetch_add(1, std::memory_order_relaxed) + 1;
  TTT2_RUNTIME_ORIGINAL(NtQueueApcThread)(ctx, base);
  if (n <= 16) {
    REXLOG_INFO("ttt2: APC {} queued to handle 0x{:08X}: routine {} (context 0x{:08X}) -> {} from {} on {}", n, thread,
                ttt2::monitor::DescribeGuestCode(routine), context, ttt2::monitor::FormatStatus(ctx.r3.u32),
                ttt2::monitor::DescribeGuestAddress(lr), ttt2::monitor::CurrentThreadLabel());
  }
}
