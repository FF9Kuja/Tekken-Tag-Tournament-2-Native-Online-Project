// ttt2 - the native host application for Tekken Tag Tournament 2 (ReXGlue SDK, rex::ReXApp).
//
// Milestone 4 work in progress. What exists: the lifecycle hooks that identify the loaded
// executable, log the content paths and backend selection, run a guest-thread watchdog that
// reports the thread population periodically, and count the kernel stubs the game calls (the first
// boot diagnostics both reference ports needed). Hooks for data patches, netplay and settings come
// later. Nothing here is game data.

#pragma once

#include <rex/exception_handler.h>
#include <rex/image_info.h>
#include <rex/rex_app.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "boot_monitor.h"
#include "stub_counter.h"

/// Layout of the recompiled image, defined by the generated ttt2_init.cpp.
extern const rex::PPCImageInfo PPCImageConfig;

class Ttt2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Ttt2App() override;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ttt2App>(new Ttt2App(ctx, "ttt2", PPCImageConfig));
  }

  /// The executable this host was recompiled from: Tekken Tag Tournament 2 with Title Update 3.
  static constexpr std::uint32_t kExpectedTitleId = 0x4E4D0859u;
  static constexpr std::uint32_t kExpectedVersion = 0x00000302u;  // 0.0.3.2, Title Update 3
  static constexpr std::uint32_t kRetailBaseVersion = 0x00000002u;  // 0.0.0.2, the retail disc
  /// Process exit code after an unhandled host exception (the HOST CRASH block is in the log).
  static constexpr int kExitCodeHostCrash = 3;
  /// Process exit code after the runtime aborted (REX_FATAL: its "[FATAL] ..." line is in the log, e.g.
  /// a call to an address no recompiled function starts at).
  static constexpr int kExitCodeFatal = 4;

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostInitLogging() override;
  void OnPostSetup() override;
  void OnPostLoadXexImage() override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  bool OnWindowCloseRequested() override;
  void OnShutdown() override;

 private:
  void StartWatchdog();
  void StopWatchdog();
  void WatchdogTick();
  /// Every guest thread's state (census line, traced wait, kernel calls, guest stack) and the
  /// process-wide trace (busiest calls, recent file activity), under `header`; `warn` logs at warning level.
  void LogThreadDump(const std::string& header, bool warn);
  /// What the host can read of the guest GPU emulation (vblank counter, ring write pointer).
  ttt2::monitor::GpuState ReadGpuState();
  void LogStubReport(const char* when);
  /// Last-chance host exception handler (rex::arch::ExceptionHandler): logs what is known about the
  /// crash (kind, host PC, fault address, guest thread, recent stub calls), flushes the log, and
  /// ends the process with kExitCodeHostCrash (the SDK would otherwise re-execute the fault forever).
  static bool OnHostException(rex::arch::Exception* ex, void* data);
  /// SIGABRT handler: REX_FATAL logs its reason and calls abort(), which on Windows would show the C
  /// runtime's "abort() has been called" box and the error-reporting dialog and leave an unattended boot
  /// hanging. Logs that the runtime aborted, flushes the log and ends the process with kExitCodeFatal.
  static void OnAbort(int signal);
  void ReportCrash(rex::arch::Exception* ex);

  std::shared_ptr<ttt2::KernelStubCounter> stubs_;
  bool crash_handler_installed_ = false;
  std::atomic<bool> crash_reported_{false};
  rex::system::XThread* main_thread_ = nullptr;
  std::thread watchdog_;
  std::atomic<bool> watchdog_stop_{false};
  std::uint32_t watchdog_period_ms_ = 5000;
  // Stall detection: the census (threads, running threads), the stub-call total and the boot monitor's
  // progress counters (files opened, bytes read, frames presented, threads created) seen last tick, and
  // how many consecutive ticks they stayed identical while the main thread was running.
  std::size_t last_thread_count_ = 0;
  std::size_t last_running_count_ = 0;
  std::uint64_t last_stub_total_ = 0;
  std::uint64_t last_progress_ = 0;
  unsigned unchanged_ticks_ = 0;
  // The GPU plugin the runtime was set up with ("xenos" is the one whose state the host can read).
  std::string gpu_plugin_;
  // One dump at a time (the watchdog, the window-close path and a guest-thread exit can overlap).
  std::mutex dump_mutex_;
};
