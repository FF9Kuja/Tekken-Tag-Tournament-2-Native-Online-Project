// Host wrappers that mark the milestones of a boot: GPU set-up and frames (Vd*), the audio driver
// client and its frames, input polling, system dialogs (XAM UI), and every thread the game creates.
//
// A black window says nothing about how far the game got. These say it in the log: the first call of
// each GPU set-up export with its arguments, the ring buffer and the graphics interrupt callback the game
// registers, the number of frames it hands to the GPU (VdSwap: none means nothing was ever presented,
// rather than black frames), what each frame's command stream asks the GPU to do (frame_monitor.h: draws,
// resolves, clears), and the boot monitor's counters the watchdog prints every tick. Every wrapper calls the
// runtime's own export; nothing here changes what the game gets back.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "boot_monitor.h"
#include "frame_monitor.h"
#include "runtime_exports.h"

namespace {

using ttt2::monitor::counters;
using ttt2::monitor::CurrentThreadLabel;
using ttt2::monitor::DescribeGuestAddress;
using ttt2::monitor::DescribeGuestCode;
using ttt2::monitor::NoteCall;

constexpr std::uint64_t kMaxLoggedContentCalls = 16;
constexpr std::uint64_t kMaxLoggedDialogCalls = 8;

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = rex::system::kernel_memory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

/// The u32 at a guest address the game passed, or 0 when the address is not allocated memory.
std::uint32_t LoadU32Checked(std::uint32_t address) {
  auto* memory = rex::system::kernel_memory();
  if (!memory || !address) return 0;
  auto* heap = memory->LookupHeap(address);
  if (!heap || memory->LookupHeap(address + 3) != heap ||
      heap->QueryRangeAccess(address, address + 3) == rex::memory::PageAccess::kNoAccess) {
    return 0;
  }
  return LoadU32(address);
}

/// VdSwap(buffer r3, fetch r4, r5, r6, r7, front buffer address pointer r8, format pointer r9, color space
/// pointer r10, width pointer and height pointer as the 9th and 10th arguments, on the stack at 0x54 and 0x5C).
ttt2::frames::Swap ReadSwap(const PPCContext& ctx) {
  ttt2::frames::Swap swap;
  swap.buffer = ctx.r3.u32;
  swap.front_buffer = LoadU32Checked(ctx.r8.u32);
  swap.format = LoadU32Checked(ctx.r9.u32);
  swap.width = LoadU32Checked(LoadU32Checked(ctx.r1.u32 + 0x54));
  swap.height = LoadU32Checked(LoadU32Checked(ctx.r1.u32 + 0x5C));
  swap.lr = static_cast<std::uint32_t>(ctx.lr);
  return swap;
}

/// A Win32-style result of the XAM exports.
std::string XamResult(std::uint32_t result) {
  switch (result) {
    case 0: return "ERROR_SUCCESS";
    case 997: return "ERROR_IO_PENDING (completes through its XOVERLAPPED)";
    case 1167: return "ERROR_DEVICE_NOT_CONNECTED";
    case 1223: return "ERROR_CANCELLED";
    case 1317: return "ERROR_NO_SUCH_USER";
    default: return fmt::format("{} (0x{:X})", result, result);
  }
}

void LogFirstGpuCall(const char* name, std::uint32_t lr) {
  REXLOG_INFO("ttt2: gpu: {} first called ({}, from {})", name, CurrentThreadLabel(), DescribeGuestAddress(lr));
}

}  // namespace

// Exports whose first call is a boot milestone worth one log line; their arguments are not needed.
#define TTT2_GPU_FIRST_CALL(name)                                           \
  TTT2_WRAPPED_EXPORT(name) {                                               \
    const auto lr = static_cast<std::uint32_t>(ctx.lr);                     \
    NoteCall(TTT2_EXPORT_ID(name), lr);                                     \
    static std::atomic_flag once = ATOMIC_FLAG_INIT;                        \
    if (!once.test_and_set()) LogFirstGpuCall(#name, lr);                   \
    TTT2_RUNTIME_ORIGINAL(name)(ctx, base);                                 \
  }

// --- GPU ---------------------------------------------------------------------------------------------

TTT2_GPU_FIRST_CALL(VdQueryVideoMode)
TTT2_GPU_FIRST_CALL(VdQueryVideoFlags)
TTT2_GPU_FIRST_CALL(VdGetCurrentDisplayInformation)
TTT2_GPU_FIRST_CALL(VdGetCurrentDisplayGamma)
TTT2_GPU_FIRST_CALL(VdSetDisplayMode)
TTT2_GPU_FIRST_CALL(VdSetDisplayModeOverride)
TTT2_GPU_FIRST_CALL(VdGetGraphicsAsicID)
TTT2_GPU_FIRST_CALL(VdEnableDisableClockGating)
TTT2_GPU_FIRST_CALL(VdGetSystemCommandBuffer)
TTT2_GPU_FIRST_CALL(VdSetSystemCommandBufferGpuIdentifierAddress)
TTT2_GPU_FIRST_CALL(VdInitializeScalerCommandBuffer)
TTT2_GPU_FIRST_CALL(VdRetrainEDRAM)
TTT2_GPU_FIRST_CALL(VdRetrainEDRAMWorker)
TTT2_GPU_FIRST_CALL(VdIsHSIOTrainingSucceeded)
TTT2_GPU_FIRST_CALL(VdCallGraphicsNotificationRoutines)
TTT2_GPU_FIRST_CALL(VdPersistDisplay)
TTT2_GPU_FIRST_CALL(VdShutdownEngines)

#undef TTT2_GPU_FIRST_CALL

// VdInitializeEngines(r3, callback r4, argument r5, PFP microcode r6, ME microcode r7).
TTT2_WRAPPED_EXPORT(VdInitializeEngines) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(VdInitializeEngines), lr);
  TTT2_RUNTIME_ORIGINAL(VdInitializeEngines)(ctx, base);
  counters().engines_initialized.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("ttt2: gpu: VdInitializeEngines -> {} ({}, from {})", ctx.r3.u32, CurrentThreadLabel(),
              DescribeGuestAddress(lr));
}

// VdSetGraphicsInterruptCallback(callback r3, user data r4): the game's vblank/GPU interrupt handler.
TTT2_WRAPPED_EXPORT(VdSetGraphicsInterruptCallback) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(VdSetGraphicsInterruptCallback), lr);
  const std::uint32_t callback = ctx.r3.u32;
  const std::uint32_t data = ctx.r4.u32;
  TTT2_RUNTIME_ORIGINAL(VdSetGraphicsInterruptCallback)(ctx, base);
  counters().interrupt_callback.store(callback, std::memory_order_relaxed);
  REXLOG_INFO("ttt2: gpu: graphics interrupt callback {} (data 0x{:08X}) registered ({}, from {})",
              DescribeGuestCode(callback), data, CurrentThreadLabel(), DescribeGuestAddress(lr));
}

// VdInitializeRingBuffer(physical address r3, log2 size r4): the GPU command ring.
TTT2_WRAPPED_EXPORT(VdInitializeRingBuffer) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(VdInitializeRingBuffer), lr);
  const std::uint32_t ring = ctx.r3.u32;
  const std::uint32_t size_log2 = ctx.r4.u32;
  TTT2_RUNTIME_ORIGINAL(VdInitializeRingBuffer)(ctx, base);
  ttt2::frames::NoteRingBuffer(ring, size_log2);
  counters().ring_buffer.store(ring ? ring : 1, std::memory_order_relaxed);
  counters().ring_buffer_size_log2.store(size_log2, std::memory_order_relaxed);
  REXLOG_INFO("ttt2: gpu: ring buffer at 0x{:08X}, size 2^{} ({}, from {})", ring, size_log2, CurrentThreadLabel(),
              DescribeGuestAddress(lr));
}

// VdEnableRingBufferRPtrWriteBack(address r3, log2 block size r4).
TTT2_WRAPPED_EXPORT(VdEnableRingBufferRPtrWriteBack) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(VdEnableRingBufferRPtrWriteBack), lr);
  const std::uint32_t address = ctx.r3.u32;
  const std::uint32_t block_log2 = ctx.r4.u32;
  TTT2_RUNTIME_ORIGINAL(VdEnableRingBufferRPtrWriteBack)(ctx, base);
  REXLOG_INFO("ttt2: gpu: ring read pointer write-back at 0x{:08X}, block 2^{} ({})", address, block_log2,
              CurrentThreadLabel());
}

// VdSwap(...): a frame handed to the display. The count tells "black frames" from "no frames", the frame
// monitor what the frame asked the GPU to draw.
TTT2_WRAPPED_EXPORT(VdSwap) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(VdSwap), lr);
  const ttt2::frames::Swap swap = ReadSwap(ctx);
  TTT2_RUNTIME_ORIGINAL(VdSwap)(ctx, base);
  if (counters().frames_presented.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("ttt2: gpu: first frame presented (VdSwap, {}, from {})", CurrentThreadLabel(), DescribeGuestAddress(lr));
  }
  ttt2::frames::NoteSwap(swap);
}

// --- audio ---------------------------------------------------------------------------------------------

// XAudioRegisterRenderDriverClient(callback r3 -> {function, argument}, driver out r4).
TTT2_WRAPPED_EXPORT(XAudioRegisterRenderDriverClient) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XAudioRegisterRenderDriverClient), lr);
  const std::uint32_t callback_ptr = ctx.r3.u32;
  const std::uint32_t function = callback_ptr ? LoadU32(callback_ptr) : 0;
  const std::uint32_t argument = callback_ptr ? LoadU32(callback_ptr + 4) : 0;
  TTT2_RUNTIME_ORIGINAL(XAudioRegisterRenderDriverClient)(ctx, base);
  const std::uint32_t status = ctx.r3.u32;
  if ((status & 0x80000000u) == 0) counters().audio_clients.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("ttt2: audio: render driver client {} (argument 0x{:08X}) -> {} ({})", DescribeGuestCode(function),
              argument, ttt2::monitor::FormatStatus(status), CurrentThreadLabel());
}

// XAudioUnregisterRenderDriverClient(driver r3).
TTT2_WRAPPED_EXPORT(XAudioUnregisterRenderDriverClient) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XAudioUnregisterRenderDriverClient), lr);
  const std::uint32_t driver = ctx.r3.u32;
  TTT2_RUNTIME_ORIGINAL(XAudioUnregisterRenderDriverClient)(ctx, base);
  REXLOG_INFO("ttt2: audio: render driver client 0x{:08X} unregistered ({})", driver, CurrentThreadLabel());
}

// XAudioSubmitRenderDriverFrame(driver r3, samples r4): one audio frame.
TTT2_WRAPPED_EXPORT(XAudioSubmitRenderDriverFrame) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XAudioSubmitRenderDriverFrame), lr);
  TTT2_RUNTIME_ORIGINAL(XAudioSubmitRenderDriverFrame)(ctx, base);
  if (counters().audio_frames.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("ttt2: audio: first frame submitted ({}, from {})", CurrentThreadLabel(), DescribeGuestAddress(lr));
  }
}

// --- input -----------------------------------------------------------------------------------------------

// XamInputGetState(user r3, flags r4, state r5): the game polls its controllers once its loop runs.
TTT2_WRAPPED_EXPORT(XamInputGetState) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XamInputGetState), lr);
  const std::uint32_t user = ctx.r3.u32;
  TTT2_RUNTIME_ORIGINAL(XamInputGetState)(ctx, base);
  if (counters().input_polls.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("ttt2: input: first controller poll (user {}) -> {} ({}, from {})", user, XamResult(ctx.r3.u32),
                CurrentThreadLabel(), DescribeGuestAddress(lr));
  }
}

// --- system dialogs and content (XAM) --------------------------------------------------------------------

// A dialog the game waits for (through its XOVERLAPPED) would stop it as surely as a deadlock. The first
// calls of each are logged (a game that polls one in a loop would otherwise flood the log).
#define TTT2_XAM_DIALOG(name)                                                                                \
  TTT2_WRAPPED_EXPORT(name) {                                                                                \
    const auto lr = static_cast<std::uint32_t>(ctx.lr);                                                      \
    NoteCall(TTT2_EXPORT_ID(name), lr);                                                                      \
    TTT2_RUNTIME_ORIGINAL(name)(ctx, base);                                                                  \
    static std::atomic<std::uint64_t> logged{0};                                                             \
    const std::uint64_t n = logged.fetch_add(1, std::memory_order_relaxed) + 1;                              \
    if (n <= kMaxLoggedDialogCalls) {                                                                        \
      REXLOG_INFO("ttt2: xam: {} -> {} ({}, from {}){}", #name, XamResult(ctx.r3.u32), CurrentThreadLabel(), \
                  DescribeGuestAddress(lr), n == kMaxLoggedDialogCalls ? "; further calls are not logged" : ""); \
    }                                                                                                        \
  }

TTT2_XAM_DIALOG(XamShowSigninUI)
TTT2_XAM_DIALOG(XamShowMessageBoxUI)
TTT2_XAM_DIALOG(XamShowMessageBoxUIEx)
TTT2_XAM_DIALOG(XamShowKeyboardUI)
TTT2_XAM_DIALOG(XamShowDeviceSelectorUI)

#undef TTT2_XAM_DIALOG

// XamContentCreateEx(user r3, root name r4, content data r5, flags r6, ...): save data and DLC.
TTT2_WRAPPED_EXPORT(XamContentCreateEx) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XamContentCreateEx), lr);
  const std::uint32_t user = ctx.r3.u32;
  const std::uint32_t flags = ctx.r6.u32;
  TTT2_RUNTIME_ORIGINAL(XamContentCreateEx)(ctx, base);
  static std::atomic<std::uint64_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedContentCalls) {
    REXLOG_INFO("ttt2: xam: XamContentCreateEx (user {}, flags 0x{:X}) -> {} ({}, from {})", user, flags,
                XamResult(ctx.r3.u32), CurrentThreadLabel(), DescribeGuestAddress(lr));
  }
}

// XamNotifyCreateListener(mask r3, version r4): the game listens for system notifications.
TTT2_WRAPPED_EXPORT(XamNotifyCreateListener) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(XamNotifyCreateListener), lr);
  const std::uint64_t mask = ctx.r3.u64;
  TTT2_RUNTIME_ORIGINAL(XamNotifyCreateListener)(ctx, base);
  REXLOG_INFO("ttt2: xam: notification listener (mask 0x{:X}) -> handle 0x{:08X} ({})", mask, ctx.r3.u32,
              CurrentThreadLabel());
}

// --- threads ------------------------------------------------------------------------------------------------

// ExCreateThread(handle out r3, stack size r4, thread id out r5, XAPI startup r6, start r7, context r8,
//                flags r9). The start address ties a thread to the code its stack shows later.
TTT2_WRAPPED_EXPORT(ExCreateThread) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  NoteCall(TTT2_EXPORT_ID(ExCreateThread), lr);
  const std::uint32_t handle_ptr = ctx.r3.u32;
  const std::uint32_t xapi_startup = ctx.r6.u32;
  const std::uint32_t start = ctx.r7.u32;
  const std::uint32_t context = ctx.r8.u32;
  const std::uint32_t flags = ctx.r9.u32;
  TTT2_RUNTIME_ORIGINAL(ExCreateThread)(ctx, base);
  const std::uint32_t status = ctx.r3.u32;
  if (status & 0x80000000u) {
    REXLOG_WARN("ttt2: thread creation failed -> {} (start {}, {}, from {})", ttt2::monitor::FormatStatus(status),
                DescribeGuestCode(start), CurrentThreadLabel(), DescribeGuestAddress(lr));
    return;
  }
  counters().threads_created.fetch_add(1, std::memory_order_relaxed);
  std::uint32_t id = 0;
  if (handle_ptr) {
    if (auto* kernel = rex::system::kernel_state()) {
      if (auto thread = kernel->object_table()->LookupObject<rex::system::XThread>(LoadU32(handle_ptr))) {
        id = thread->thread_id();
      }
    }
  }
  REXLOG_INFO("ttt2: thread created: id {}, start {}{}, context 0x{:08X}{} ({}, from {})", id,
              DescribeGuestCode(start),
              xapi_startup ? fmt::format(" (through XAPI startup {})", DescribeGuestCode(xapi_startup)) : std::string(),
              context, (flags & 1) ? ", suspended" : "", CurrentThreadLabel(), DescribeGuestAddress(lr));
}
