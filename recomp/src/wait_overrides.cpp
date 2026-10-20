// Host wrappers of the runtime's blocking exports: each records, for the duration of the call, what the
// calling guest thread waits on (the objects, the timeout, the guest address it was called from), so
// the watchdog's stall dump can say which thread waits for what - and who owns it, for mutants and
// critical sections - instead of only that nothing moves. The runtime's own function does the waiting.
//
// Registers follow the Xbox 360 calling convention: arguments in r3..r10, the result in r3.
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <array>
#include <cstdint>

#include "boot_monitor.h"
#include "runtime_exports.h"

namespace {

using ttt2::monitor::kMaxWaitObjects;
using ttt2::monitor::kNoTimeout;
using ttt2::monitor::NoteCall;
using ttt2::monitor::WaitKind;
using ttt2::monitor::WaitScope;

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = rex::system::kernel_memory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
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

}  // namespace

// KeWaitForSingleObject(object r3, wait reason r4, processor mode r5, alertable r6, timeout r7).
TTT2_WRAPPED_EXPORT(KeWaitForSingleObject) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeWaitForSingleObject);
  const std::uint32_t object = ctx.r3.u32;
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kDispatcher, lr, ReadTimeout(ctx.r7.u32), &object, 1);
  TTT2_RUNTIME_ORIGINAL(KeWaitForSingleObject)(ctx, base);
}

// KeWaitForMultipleObjects(count r3, objects r4, wait type r5, reason r6, mode r7, alertable r8,
//                          timeout r9, wait blocks r10).
TTT2_WRAPPED_EXPORT(KeWaitForMultipleObjects) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeWaitForMultipleObjects);
  const std::uint32_t count = ctx.r3.u32;
  const auto objects = ReadObjects(ctx.r4.u32, count);
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kDispatcher, lr, ReadTimeout(ctx.r9.u32), objects.data(), count);
  TTT2_RUNTIME_ORIGINAL(KeWaitForMultipleObjects)(ctx, base);
}

// NtWaitForSingleObjectEx(handle r3, wait mode r4, alertable r5, timeout r6).
TTT2_WRAPPED_EXPORT(NtWaitForSingleObjectEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtWaitForSingleObjectEx);
  const std::uint32_t handle = ctx.r3.u32;
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kHandles, lr, ReadTimeout(ctx.r6.u32), &handle, 1);
  TTT2_RUNTIME_ORIGINAL(NtWaitForSingleObjectEx)(ctx, base);
}

// NtWaitForMultipleObjectsEx(count r3, handles r4, wait type r5, wait mode r6, alertable r7, timeout r8).
TTT2_WRAPPED_EXPORT(NtWaitForMultipleObjectsEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtWaitForMultipleObjectsEx);
  const std::uint32_t count = ctx.r3.u32;
  const auto handles = ReadObjects(ctx.r4.u32, count);
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kHandles, lr, ReadTimeout(ctx.r8.u32), handles.data(), count);
  TTT2_RUNTIME_ORIGINAL(NtWaitForMultipleObjectsEx)(ctx, base);
}

// NtSignalAndWaitForSingleObjectEx(signal handle r3, wait handle r4, alertable r5, r6, timeout r7).
TTT2_WRAPPED_EXPORT(NtSignalAndWaitForSingleObjectEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(NtSignalAndWaitForSingleObjectEx);
  const std::uint32_t handle = ctx.r4.u32;
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kHandles, lr, ReadTimeout(ctx.r7.u32), &handle, 1);
  TTT2_RUNTIME_ORIGINAL(NtSignalAndWaitForSingleObjectEx)(ctx, base);
}

// KeDelayExecutionThread(processor mode r3, alertable r4, interval r5): a sleep.
TTT2_WRAPPED_EXPORT(KeDelayExecutionThread) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(KeDelayExecutionThread);
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kDelay, lr, ReadTimeout(ctx.r5.u32), nullptr, 0);
  TTT2_RUNTIME_ORIGINAL(KeDelayExecutionThread)(ctx, base);
}

// RtlEnterCriticalSection(critical section r3): blocks while another thread owns it.
TTT2_WRAPPED_EXPORT(RtlEnterCriticalSection) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const auto id = TTT2_EXPORT_ID(RtlEnterCriticalSection);
  const std::uint32_t section = ctx.r3.u32;
  WaitScope wait(NoteCall(id, lr), id, WaitKind::kCriticalSection, lr, kNoTimeout, &section, 1);
  TTT2_RUNTIME_ORIGINAL(RtlEnterCriticalSection)(ctx, base);
}
