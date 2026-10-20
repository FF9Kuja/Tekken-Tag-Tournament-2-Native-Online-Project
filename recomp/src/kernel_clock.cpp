#include "kernel_clock.h"

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/thread.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>

namespace ttt2::kernel_clock {
namespace {

constexpr std::uint16_t kKeTimeStampBundleOrdinal = 0x00AD;

struct State {
  std::mutex mutex;  // Start/Stop
  std::unique_ptr<rex::thread::HighResolutionTimer> timer;
  std::atomic<std::uint8_t*> bundle{nullptr};  // host address of the bundle (null: none)
  std::uint32_t guest_address = 0;
  bool started = false;
  bool disabled = false;  // TTT2_KERNEL_CLOCK=runtime
  bool missing = false;   // the runtime has no bundle
  std::uint64_t last_interrupt = 0;  // only the timer thread touches it
};

State& state() {
  static State s;
  return s;
}

/// InterruptTime and SystemTime from the runtime's guest clock: the system time it reports to
/// KeQuerySystemTime, and the time since its base (the same offset its TickCount is computed from).
void Refresh(std::uint8_t* bundle) {
  auto& s = state();
  const std::uint64_t system = rex::chrono::Clock::QueryGuestSystemTime();
  const std::uint64_t time_base = rex::chrono::Clock::guest_system_time_base();
  // Never backwards (a host wall-clock change with clock_no_scaling, or a new guest time base).
  const std::uint64_t interrupt = std::max(system > time_base ? system - time_base : 0, s.last_interrupt);
  s.last_interrupt = interrupt;
  // Aligned 8-byte stores (the bundle is 32-byte aligned): the game's 64-bit loads see whole values.
  rex::memory::store_and_swap<std::uint64_t>(bundle + 0, interrupt);
  rex::memory::store_and_swap<std::uint64_t>(bundle + 8, system);
  // TickCount (+16) stays the runtime's: two writers of it could make it step back by a millisecond.
}

}  // namespace

std::uint32_t Start(rex::Runtime& runtime) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.started) return s.timer ? s.guest_address : 0;
  s.started = true;
  if (const char* mode = std::getenv("TTT2_KERNEL_CLOCK"); mode && std::string_view(mode) == "runtime") {
    s.disabled = true;
    REXLOG_WARN(
        "ttt2: kernel clock: TTT2_KERNEL_CLOCK=runtime: KeTimeStampBundle keeps the runtime's behaviour (InterruptTime "
        "and SystemTime stay 0, only TickCount moves)");
    return 0;
  }
  auto* resolver = runtime.export_resolver();
  auto* entry = resolver ? resolver->GetExportByOrdinal("xboxkrnl.exe", kKeTimeStampBundleOrdinal) : nullptr;
  if (!entry || entry->type != rex::runtime::Export::Type::kVariable || !entry->variable_ptr || !runtime.memory()) {
    s.missing = true;
    REXLOG_WARN("ttt2: kernel clock: the runtime has no KeTimeStampBundle (xboxkrnl variable 0xAD); not refreshed");
    return 0;
  }
  s.guest_address = entry->variable_ptr;
  auto* bundle = runtime.memory()->TranslateVirtual<std::uint8_t*>(s.guest_address);
  Refresh(bundle);
  s.bundle.store(bundle, std::memory_order_release);
  s.timer = rex::thread::HighResolutionTimer::CreateRepeating(std::chrono::milliseconds(1),
                                                               [bundle]() { Refresh(bundle); });
  REXLOG_INFO(
      "ttt2: kernel clock: KeTimeStampBundle at 0x{:08X}: the host refreshes InterruptTime and SystemTime every 1 ms "
      "(the runtime only refreshes TickCount; the game reads all three from memory)",
      s.guest_address);
  return s.guest_address;
}

void Stop() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.timer.reset();  // disarms the timer; a callback in flight finishes first
}

bool Running() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.timer != nullptr;
}

Sample Read() {
  Sample sample;
  if (const std::uint8_t* bundle = state().bundle.load(std::memory_order_acquire)) {
    sample.interrupt_time = rex::memory::load_and_swap<std::uint64_t>(bundle + 0);
    sample.system_time = rex::memory::load_and_swap<std::uint64_t>(bundle + 8);
    sample.tick_count = rex::memory::load_and_swap<std::uint32_t>(bundle + 16);
  }
  return sample;
}

std::string Describe() {
  auto& s = state();
  bool disabled = false;
  bool missing = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    disabled = s.disabled;
    missing = s.missing;
  }
  if (disabled) return "kernel clock off (TTT2_KERNEL_CLOCK=runtime)";
  if (missing) return "kernel clock missing (no KeTimeStampBundle)";
  if (!s.bundle.load(std::memory_order_acquire)) return "kernel clock not started";
  const Sample now = Read();
  return fmt::format("kernel clock {:.1f} s (tick count {:.1f} s)", static_cast<double>(now.interrupt_time) / 1e7,
                     static_cast<double>(now.tick_count) / 1e3);
}

}  // namespace ttt2::kernel_clock
