// ttt2 - the native host application for Tekken Tag Tournament 2 (ReXGlue SDK, rex::ReXApp).
//
// Milestone 4 work in progress. What exists: the lifecycle hooks that identify the loaded
// executable, log the content paths and backend selection, run a guest-thread watchdog that
// reports the thread population periodically, and count the kernel stubs the game calls (the first
// boot diagnostics both reference ports needed). Hooks for data patches, netplay and settings come
// later. Nothing here is game data.

#pragma once

#include <rex/image_info.h>
#include <rex/rex_app.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

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
  static constexpr std::uint32_t kExpectedVersion = 0x00000302u;  // 0.0.3.2

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostInitLogging() override;
  void OnPostLoadXexImage() override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  void OnShutdown() override;

 private:
  void StartWatchdog();
  void StopWatchdog();
  void WatchdogTick();
  void LogStubReport(const char* when);

  std::shared_ptr<ttt2::KernelStubCounter> stubs_;
  rex::system::XThread* main_thread_ = nullptr;
  std::thread watchdog_;
  std::atomic<bool> watchdog_stop_{false};
  std::uint32_t watchdog_period_ms_ = 5000;
};
