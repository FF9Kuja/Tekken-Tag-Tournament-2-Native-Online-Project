// Host wrappers that only record what the game's calls did, for the boot diagnostics: exceptions the game raises
// (thread names, C++ throws), debug breaks and bug checks, and memory calls the runtime refuses.
//
// Exceptions. The game names its threads with the MSVC SetThreadName exception (0x406D1388; TTT2 Web logs names
// such as nuscMain and nuscFileAsyncRequest), which the runtime applies to the thread with the id the game passes;
// the owner's dumps list every thread as "(unnamed)", so the wrapper logs each name with its thread id and whether
// the runtime found that thread. A C++ throw (0xE06D7363) or any other code is logged with its call site: the
// runtime cannot unwind the guest stack, so it returns into the code after the throw - which on the console never
// runs (TTT2 Web aborts there, TTT2Recomp logs and goes on).
//
// Memory. An allocation the runtime refuses can send the game down an error path no console run takes; each
// failure is logged with its arguments (the first ones) and counted.
//
// Registers follow the Xbox 360 calling convention: arguments in r3..r10, the result in r3.
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <fmt/format.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "boot_monitor.h"
#include "runtime_exports.h"

namespace {

using ttt2::monitor::NoteCall;

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = rex::system::kernel_memory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

std::uint32_t LoadU32Checked(std::uint32_t address) {
  return address && ttt2::monitor::GuestReadable(address, 4) ? LoadU32(address) : 0;
}

/// A NUL-terminated guest string of printable characters (at most `max`), "" when unreadable.
std::string GuestString(std::uint32_t address, std::size_t max = 64) {
  std::string out;
  for (std::size_t i = 0; i < max; ++i) {
    if (!ttt2::monitor::GuestReadable(address + static_cast<std::uint32_t>(i), 1)) break;
    const char c = *rex::system::kernel_memory()->TranslateVirtual<const char*>(address + static_cast<std::uint32_t>(i));
    if (!c) break;
    out += (c >= 32 && c < 127) ? c : '?';
  }
  return out;
}

std::atomic<std::uint64_t> g_other_exceptions{0};
std::atomic<std::uint64_t> g_memory_logged{0};

/// The name of the first type a C++ throw can be caught as (MSVC ThrowInfo -> CatchableTypeArray -> CatchableType ->
/// TypeDescriptor), "" when any link is unreadable.
std::string ThrownTypeName(std::uint32_t throw_info) {
  const std::uint32_t array = LoadU32Checked(throw_info + 0xC);
  const std::uint32_t first = array ? LoadU32Checked(array + 4) : 0;
  const std::uint32_t type = first ? LoadU32Checked(first + 4) : 0;
  return type ? GuestString(type + 8) : std::string();
}

void NoteMemoryFailure(const std::string& what, std::uint32_t status, std::uint32_t lr) {
  ttt2::monitor::counters().memory_failures.fetch_add(1, std::memory_order_relaxed);
  if (g_memory_logged.fetch_add(1, std::memory_order_relaxed) < 24) {
    REXLOG_WARN("ttt2: memory: {} -> {} from {} on {}", what, ttt2::monitor::FormatStatus(status),
                ttt2::monitor::DescribeGuestAddress(lr), ttt2::monitor::CurrentThreadLabel());
  }
}

}  // namespace

// RtlRaiseException(exception record r3): code at +0, flags +4, address +0xC, parameter count +0x10, parameters
// from +0x14.
TTT2_WRAPPED_EXPORT(RtlRaiseException) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t record = ctx.r3.u32;
  NoteCall(TTT2_EXPORT_ID(RtlRaiseException), lr);
  auto& c = ttt2::monitor::counters();
  c.exceptions_raised.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t code = LoadU32Checked(record);
  if (code == 0x406D1388u) {
    // THREADNAME_INFO in the parameters: type 0x1000, name, thread id (-1: the caller), flags.
    const std::uint32_t name_ptr = LoadU32Checked(record + 0x14 + 4);
    const std::uint32_t thread_id = LoadU32Checked(record + 0x14 + 8);
    const std::string name = name_ptr ? GuestString(name_ptr) : std::string();
    TTT2_RUNTIME_ORIGINAL(RtlRaiseException)(ctx, base);
    std::string applied = "no thread with that id";
    if (auto* kernel = rex::system::kernel_state()) {
      rex::system::object_ref<rex::system::XThread> thread;
      if (thread_id == 0xFFFFFFFFu) {
        thread = rex::system::retain_object(rex::system::XThread::GetCurrentThread());
      } else {
        thread = kernel->GetThreadByID(thread_id);
      }
      if (thread) {
        applied = thread->name() == name ? fmt::format("thread id {} is named so now", thread->thread_id())
                                         : fmt::format("thread id {} is still called '{}'", thread->thread_id(), thread->name());
      }
    }
    REXLOG_INFO("ttt2: thread name '{}' for thread id {}: {}; set from {} on {}", name,
                thread_id == 0xFFFFFFFFu ? std::string("-1: the caller") : std::to_string(thread_id), applied,
                ttt2::monitor::DescribeGuestAddress(lr), ttt2::monitor::CurrentThreadLabel());
    return;
  }
  if (code == 0xE06D7363u) {
    const std::uint64_t n = c.cpp_exceptions.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) {
      const std::string type = ThrownTypeName(LoadU32Checked(record + 0x14 + 8));
      REXLOG_WARN(
          "ttt2: C++ exception {} thrown ({}) from {} on {}: the runtime cannot unwind the guest stack, so the code after "
          "the throw runs next, which on the console never does",
          n, type.empty() ? std::string("type unreadable") : "type " + type, ttt2::monitor::DescribeGuestAddress(lr),
          ttt2::monitor::CurrentThreadLabel());
    }
  } else if (g_other_exceptions.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("ttt2: exception 0x{:08X} raised (flags 0x{:X}, address 0x{:08X}) from {} on {}", code,
                LoadU32Checked(record + 4), LoadU32Checked(record + 0xC), ttt2::monitor::DescribeGuestAddress(lr),
                ttt2::monitor::CurrentThreadLabel());
  }
  TTT2_RUNTIME_ORIGINAL(RtlRaiseException)(ctx, base);
}

// DbgBreakPoint(): a debug break, where an assertion of the game's would stop under a debugger.
TTT2_WRAPPED_EXPORT(DbgBreakPoint) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(DbgBreakPoint), lr);
  const std::uint64_t n = ttt2::monitor::counters().debug_breaks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16) {
    REXLOG_WARN("ttt2: debug break {} from {} on {}", n, ttt2::monitor::DescribeGuestAddress(lr),
                ttt2::monitor::CurrentThreadLabel());
  }
  TTT2_RUNTIME_ORIGINAL(DbgBreakPoint)(ctx, base);
}

// KeBugCheckEx(code r3, parameters r4..r7) / KeBugCheck(code r3): the console stops here.
TTT2_WRAPPED_EXPORT(KeBugCheckEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(KeBugCheckEx), lr);
  ttt2::monitor::counters().debug_breaks.fetch_add(1, std::memory_order_relaxed);
  REXLOG_ERROR("ttt2: bug check 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}) from {} on {}", ctx.r3.u32, ctx.r4.u32,
               ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ttt2::monitor::DescribeGuestAddress(lr),
               ttt2::monitor::CurrentThreadLabel());
  TTT2_RUNTIME_ORIGINAL(KeBugCheckEx)(ctx, base);
}

TTT2_WRAPPED_EXPORT(KeBugCheck) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(KeBugCheck), lr);
  ttt2::monitor::counters().debug_breaks.fetch_add(1, std::memory_order_relaxed);
  REXLOG_ERROR("ttt2: bug check 0x{:08X} from {} on {}", ctx.r3.u32, ttt2::monitor::DescribeGuestAddress(lr),
               ttt2::monitor::CurrentThreadLabel());
  TTT2_RUNTIME_ORIGINAL(KeBugCheck)(ctx, base);
}

// NtAllocateVirtualMemory(base in/out r3, size in/out r4, allocation type r5, protect r6, debug r7).
TTT2_WRAPPED_EXPORT(NtAllocateVirtualMemory) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t base_ptr = ctx.r3.u32;
  const std::uint32_t size_ptr = ctx.r4.u32;
  const std::uint32_t type = ctx.r5.u32;
  const std::uint32_t protect = ctx.r6.u32;
  const std::uint32_t want_base = LoadU32Checked(base_ptr);
  const std::uint32_t want_size = LoadU32Checked(size_ptr);
  NoteCall(TTT2_EXPORT_ID(NtAllocateVirtualMemory), lr);
  TTT2_RUNTIME_ORIGINAL(NtAllocateVirtualMemory)(ctx, base);
  if (ctx.r3.u32) {
    NoteMemoryFailure(fmt::format("NtAllocateVirtualMemory(base 0x{:08X}, size 0x{:X}, type 0x{:X}, protect 0x{:X})",
                                  want_base, want_size, type, protect),
                      ctx.r3.u32, lr);
  }
}

// NtFreeVirtualMemory(base in/out r3, size in/out r4, free type r5, debug r6).
TTT2_WRAPPED_EXPORT(NtFreeVirtualMemory) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t want_base = LoadU32Checked(ctx.r3.u32);
  const std::uint32_t want_size = LoadU32Checked(ctx.r4.u32);
  const std::uint32_t type = ctx.r5.u32;
  NoteCall(TTT2_EXPORT_ID(NtFreeVirtualMemory), lr);
  TTT2_RUNTIME_ORIGINAL(NtFreeVirtualMemory)(ctx, base);
  if (ctx.r3.u32) {
    NoteMemoryFailure(fmt::format("NtFreeVirtualMemory(base 0x{:08X}, size 0x{:X}, type 0x{:X})", want_base, want_size, type),
                      ctx.r3.u32, lr);
  }
}

// MmAllocatePhysicalMemoryEx(flags r3, size r4, protect r5, min address r6, max address r7, alignment r8) -> address.
TTT2_WRAPPED_EXPORT(MmAllocatePhysicalMemoryEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t size = ctx.r4.u32;
  const std::uint32_t protect = ctx.r5.u32;
  const std::uint32_t low = ctx.r6.u32;
  const std::uint32_t high = ctx.r7.u32;
  const std::uint32_t align = ctx.r8.u32;
  NoteCall(TTT2_EXPORT_ID(MmAllocatePhysicalMemoryEx), lr);
  TTT2_RUNTIME_ORIGINAL(MmAllocatePhysicalMemoryEx)(ctx, base);
  if (!ctx.r3.u32) {
    NoteMemoryFailure(fmt::format("MmAllocatePhysicalMemoryEx(size 0x{:X}, protect 0x{:X}, range 0x{:08X}-0x{:08X}, "
                                  "alignment 0x{:X}) returned no memory",
                                  size, protect, low, high, align),
                      0xC0000017u, lr);  // reported as STATUS_NO_MEMORY
  }
}

// ExAllocatePool(size r3) -> address.
TTT2_WRAPPED_EXPORT(ExAllocatePool) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t size = ctx.r3.u32;
  NoteCall(TTT2_EXPORT_ID(ExAllocatePool), lr);
  TTT2_RUNTIME_ORIGINAL(ExAllocatePool)(ctx, base);
  if (!ctx.r3.u32) {
    NoteMemoryFailure(fmt::format("ExAllocatePool(size 0x{:X}) returned no memory", size), 0xC0000017u, lr);
  }
}

// ExAllocatePoolTypeWithTag(size r3, tag r4, pool type r5) -> address.
TTT2_WRAPPED_EXPORT(ExAllocatePoolTypeWithTag) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  const std::uint32_t size = ctx.r3.u32;
  const std::uint32_t tag = ctx.r4.u32;
  NoteCall(TTT2_EXPORT_ID(ExAllocatePoolTypeWithTag), lr);
  TTT2_RUNTIME_ORIGINAL(ExAllocatePoolTypeWithTag)(ctx, base);
  if (!ctx.r3.u32) {
    NoteMemoryFailure(fmt::format("ExAllocatePoolTypeWithTag(size 0x{:X}, tag 0x{:08X}) returned no memory", size, tag),
                      0xC0000017u, lr);
  }
}
