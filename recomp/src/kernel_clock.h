// KeTimeStampBundle kept current, as the console kernel keeps it.
//
// The bundle (xboxkrnl data export 0xAD, 24 bytes) is a clock the game reads straight from memory,
// without a kernel call: u64 InterruptTime (100 ns units since boot) at +0, u64 SystemTime (FILETIME:
// 100 ns units since 1601) at +8, u32 TickCount (milliseconds since boot) at +16. The console kernel
// refreshes it on every clock interrupt. ReXGlue 0.10.0 allocates it and patches the game's import
// slot, but its 1 ms timer only refreshes TickCount: InterruptTime and SystemTime stay 0 for the whole
// run (third_party/rexglue-sdk/src/src/kernel/xboxkrnl/xboxkrnl_module.cpp).
//
// TTT2 reads them. TTT2Recomp measured it (its network clock, sub_82BEF480, read 0 and a 1 s timeout
// never expired), and TTT2 Web documents a game thread (sub_82ABFDE8) that signals its own event and
// waits on it until a 64-bit counter changes: with a clock that never moves that thread spins forever,
// which is what the owner's log shows (one thread with 339 million kernel calls in 180 s). Both
// reference ports refresh all three fields every millisecond; the host does the same for the two the
// runtime leaves alone, from the runtime's own guest clock (rex::chrono::Clock, which also answers
// KeQuerySystemTime and drives TickCount), so the three fields and the kernel calls agree.
//
// TTT2_KERNEL_CLOCK=runtime keeps the runtime's behaviour, for comparing the two on the owner's machine.
#pragma once

#include <cstdint>
#include <string>

namespace rex {
class Runtime;
}  // namespace rex

namespace ttt2::kernel_clock {

/// The bundle's three fields as the game reads them.
struct Sample {
  std::uint64_t interrupt_time = 0;  ///< 100 ns units since the runtime started
  std::uint64_t system_time = 0;     ///< FILETIME
  std::uint32_t tick_count = 0;      ///< milliseconds since the runtime started
};

/// Starts refreshing InterruptTime and SystemTime every millisecond (after Runtime::Setup, which creates
/// the bundle). Returns the bundle's guest address, or 0 when the runtime has none or
/// TTT2_KERNEL_CLOCK=runtime turned the refresh off. Logs what it did; calling it again does nothing.
std::uint32_t Start(rex::Runtime& runtime);

/// Stops the refresh (before the runtime's memory goes away).
void Stop();

/// True while the host refreshes the bundle.
bool Running();

/// The bundle as the game sees it now (all zero when there is no bundle).
Sample Read();

/// For the watchdog's activity line: "kernel clock 12.3 s (tick count 12.3 s)", or why there is none.
std::string Describe();

}  // namespace ttt2::kernel_clock
