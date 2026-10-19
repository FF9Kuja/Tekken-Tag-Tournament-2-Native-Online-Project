#include "ttt2_app.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xex_module.h>
#include <rex/system/xthread.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::string version_string(std::uint32_t v) {
  return std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." +
         std::to_string((v >> 8) & 0xFFFF) + "." + std::to_string(v & 0xFF);
}

}  // namespace

Ttt2App::~Ttt2App() {
  StopWatchdog();
}

void Ttt2App::OnConfigurePaths(rex::PathConfig& paths) {
  // The game data root is the player's extracted disc (game: / d:); the update root holds the
  // Title Update 3 data files (update:). When the launcher did not pass an update root, look for an
  // `update` folder next to the game folder, which is where the owner checklist puts it.
  if (paths.update_data_root.empty() && !paths.game_data_root.empty()) {
    const auto sibling = paths.game_data_root.parent_path() / "update";
    std::error_code ec;
    if (std::filesystem::is_directory(sibling, ec)) paths.update_data_root = sibling;
  }
  REXLOG_INFO("ttt2: game data root   {}", paths.game_data_root.string());
  REXLOG_INFO("ttt2: update data root {}", paths.update_data_root.empty()
                                               ? "(none: Title Update 3 data not mounted)"
                                               : paths.update_data_root.string());
  REXLOG_INFO("ttt2: user data root   {}", paths.user_data_root.string());
  REXLOG_INFO("ttt2: cache root       {}", paths.cache_root.string());
}

void Ttt2App::OnPreSetup(rex::RuntimeConfig& config) {
  // The SDK's gpu_plugin cvar defaults to "" (no GPU emulation: VdXxx calls are ignored), which is
  // right for the headless harness but not for a game. Unless the player chose otherwise
  // (--gpu_plugin=<name>, or --gpu_plugin=none; an empty value cannot be given on the command line),
  // load the Xenos plugin the build stages next to the executable (GPU_PLUGINS xenos in
  // recomp/CMakeLists.txt); the SDK's loader only looks there.
  if (config.gpu_plugin == "none") config.gpu_plugin.clear();
  if (!config.graphics && config.gpu_plugin.empty() &&
      rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault) {
    const auto folder = rex::filesystem::GetExecutableFolder();
    bool staged = false;
    for (const char* postfix : {"", "d", "rd"}) {  // the SDK's per-config file name postfixes
#if REX_PLATFORM_WIN32
      const std::string file = std::string("rexgpu-xenos") + postfix + ".dll";
#elif REX_PLATFORM_MAC
      const std::string file = std::string("librexgpu-xenos") + postfix + ".dylib";
#else
      const std::string file = std::string("librexgpu-xenos") + postfix + ".so";
#endif
      std::error_code ec;
      if (std::filesystem::exists(folder / file, ec)) staged = true;
    }
    if (staged) {
      config.gpu_plugin = "xenos";
    } else {
      REXLOG_WARN(
          "ttt2: no GPU plugin next to the executable ({}): running WITHOUT GPU emulation; the game "
          "cannot draw this way (a build stages rexgpu-xenos there; --gpu_plugin=none silences this)",
          folder.string());
    }
  }
  REXLOG_INFO("ttt2: graphics backend {}", config.graphics             ? "provided by the host"
                                           : config.gpu_plugin.empty() ? "none"
                                                                       : config.gpu_plugin);
  if (const char* period = std::getenv("TTT2_WATCHDOG_MS")) {
    watchdog_period_ms_ = static_cast<std::uint32_t>(std::strtoul(period, nullptr, 10));
  }
}

void Ttt2App::OnPostInitLogging() {
  // Every kernel export the runtime only stubs logs "<name> STUB" when called; counting those lines
  // gives the list of system calls the game reaches that the runtime does not implement.
  stubs_ = std::make_shared<ttt2::KernelStubCounter>();
  stubs_->set_level(spdlog::level::warn);
  rex::AddSink(stubs_);
}

void Ttt2App::OnPostSetup() {
  // Installed after the runtime's own handlers (the MMIO handler resolves guest memory faults and
  // returns true), so this one only sees crashes nothing else claims.
  rex::arch::ExceptionHandler::Install(&Ttt2App::OnHostException, this);
  crash_handler_installed_ = true;
}

bool Ttt2App::OnHostException(rex::arch::Exception* ex, void* data) {
  auto* self = static_cast<Ttt2App*>(data);
  if (self && !self->crash_reported_.exchange(true)) self->ReportCrash(ex);
  // Nothing handles this fault. The SDK's POSIX handler then simply returns, so the faulting
  // instruction re-executes and the same fault repeats forever (measured: a misbehaving test
  // program produced 695,000 "Unhandled guest access violation" lines in 15 seconds and rotated
  // the log away); on Windows the process would die with the system's crash dialog. End the process
  // here instead, with the report flushed, so a crash on the owner's machine leaves a log and an
  // exit code (TTT2_CRASH_KEEP_RUNNING=1 keeps the old behaviour for a debugger).
  if (!std::getenv("TTT2_CRASH_KEEP_RUNNING")) {
    REXLOG_CRITICAL("ttt2: exiting with code {} after the host crash", kExitCodeHostCrash);
    rex::FlushLogging();
    std::_Exit(kExitCodeHostCrash);
  }
  return false;  // not handled: the original handler (and the debugger, if any) takes over
}

void Ttt2App::ReportCrash(rex::arch::Exception* ex) {
  // Diagnostics for the owner's first boots: a crash without this is a dead end. Everything logged
  // here is addresses, names and counts.
  using rex::arch::Exception;
  std::string kind;
  switch (ex->code()) {
    case Exception::Code::kAccessViolation:
      kind = std::string(ex->access_violation_operation() == Exception::AccessViolationOperation::kWrite
                             ? "write"
                         : ex->access_violation_operation() == Exception::AccessViolationOperation::kRead
                             ? "read"
                             : "access") +
             " fault at host address " + std::to_string(ex->fault_address());
      break;
    case Exception::Code::kIllegalInstruction:
      kind = "illegal instruction (a REX_FATAL / trap in recompiled code, or a jump into data)";
      break;
    case Exception::Code::kInvalidException:
      kind = "unknown exception";
      break;
  }
  REXLOG_CRITICAL("ttt2: HOST CRASH: {} at host pc {:#x}", kind, ex->pc());
  if (ex->code() == Exception::Code::kAccessViolation) {
    REXLOG_CRITICAL("ttt2:   fault address {:#x}", ex->fault_address());
  }
  if (rex::system::XThread::IsInThread()) {
    auto* thread = rex::system::XThread::GetCurrentThread();
    REXLOG_CRITICAL("ttt2:   on guest thread {} (id {})",
                    thread && !thread->name().empty() ? thread->name() : "?",
                    thread ? thread->thread_id() : 0u);
  } else {
    REXLOG_CRITICAL("ttt2:   on a host thread (not a guest thread)");
  }
  if (stubs_) {
    std::string recent;
    for (const auto& name : stubs_->Recent()) recent += (recent.empty() ? "" : ", ") + name;
    REXLOG_CRITICAL("ttt2:   kernel stubs so far: {}", stubs_->Summary(5));
    REXLOG_CRITICAL("ttt2:   most recent stub calls: {}", recent.empty() ? "none" : recent);
  }
  rex::FlushLogging();
}

void Ttt2App::OnPostLoadXexImage() {
  auto module = runtime()->kernel_state()->GetExecutableModule();
  if (!module) {
    REXLOG_WARN("ttt2: no executable module after loading the XEX");
    return;
  }
  const auto* xex = module->xex_module();
  if (const auto* info = xex->opt_execution_info()) {
    const std::uint32_t title_id = info->title_id;
    const std::uint32_t version = info->version_value;
    const std::uint32_t base_version = info->base_version_value;
    REXLOG_INFO("ttt2: executable title {:08X} version {} (base {}), media {:08X}", title_id,
                version_string(version), version_string(base_version),
                static_cast<std::uint32_t>(info->media_id));
    if (title_id != kExpectedTitleId) {
      REXLOG_WARN(
          "ttt2: this executable is not Tekken Tag Tournament 2 (title {:08X}); the recompiled code will not "
          "match it",
          title_id);
    } else if (version == kRetailBaseVersion) {
      REXLOG_INFO(
          "ttt2: base retail executable {} (no Title Update 3 applied): the recompiled code must come from this "
          "same image (docs/OWNER_CHECKLIST.md, base-image path)",
          version_string(version));
    } else if (version != kExpectedVersion) {
      REXLOG_WARN(
          "ttt2: executable version {} is neither Title Update 3 ({}) nor the retail base ({}); the recompiled "
          "code must come from exactly this image",
          version_string(version), version_string(kExpectedVersion), version_string(kRetailBaseVersion));
    }
  } else {
    REXLOG_WARN("ttt2: the executable has no execution info header");
  }
  REXLOG_INFO("ttt2: image base {:08X} size {:X} entry {:08X}", xex->base_address(), xex->image_size(),
              xex->entry_point());
  if (const auto* libs = xex->import_libraries()) {
    for (const auto& lib : *libs) {
      REXLOG_INFO("ttt2: imports {} entries from {}", lib.imports.size(), lib.name);
    }
  }
}

void Ttt2App::OnPostLaunchModule(rex::system::XThread* thread) {
  main_thread_ = thread;
  REXLOG_INFO("ttt2: main guest thread created (id {}), starting", thread ? thread->thread_id() : 0u);
  StartWatchdog();
}

void Ttt2App::OnGuestThreadExit(rex::system::XThread* thread) {
  REXLOG_INFO("ttt2: main guest thread exited (id {})", thread ? thread->thread_id() : 0u);
  WatchdogTick();
  StopWatchdog();
  LogStubReport("main thread exit");
  // The title is over (on a console the dashboard would come back). ReXApp would now quit its UI
  // loop and tear the runtime down, but SDK 0.10.0's teardown deadlocks here: the mouse/keyboard
  // input driver's destructor calls CallInUIThreadSynchronous from the UI thread after the loop has
  // ended (measured with the license-free test image: the process never exits). The SDK's own
  // window-close path hard-exits for the same reason, so do the same: flush the log and end.
  REXLOG_INFO("ttt2: guest program ended; exiting with code 0");
  rex::FlushLogging();
  std::_Exit(0);
}

bool Ttt2App::OnWindowCloseRequested() {
  // Closing the window (the X button, or a quit signal, which SDL turns into the same request) is
  // how the owner's first boots will end: a black or frozen window. ReXApp's close path terminates
  // the title and hard-exits the process without OnShutdown, so the full kernel stub report is
  // written here, while the counts are still there; the watchdog ticks only carry the top five.
  LogStubReport("window close");
  rex::FlushLogging();
  return true;
}

void Ttt2App::OnShutdown() {
  StopWatchdog();
  if (crash_handler_installed_) {
    rex::arch::ExceptionHandler::Uninstall(&Ttt2App::OnHostException, this);
    crash_handler_installed_ = false;
  }
  LogStubReport("shutdown");
  if (stubs_) {
    rex::RemoveSink(stubs_);
    stubs_.reset();
  }
}

void Ttt2App::LogStubReport(const char* when) {
  // The complete list, most-called first: what Milestone 4 has to implement next. Names and counts.
  if (!stubs_) return;
  const auto hits = stubs_->Snapshot();
  REXLOG_INFO("ttt2: kernel stub report at {}: {} calls to {} distinct stubs", when, stubs_->total_calls(),
              hits.size());
  constexpr std::size_t kMaxLines = 200;
  for (std::size_t i = 0; i < hits.size() && i < kMaxLines; ++i) {
    REXLOG_INFO("ttt2:   {:<48} {:>9} calls (first seen as stub call #{})", hits[i].name, hits[i].calls,
                hits[i].first_seen);
  }
  if (hits.size() > kMaxLines) REXLOG_INFO("ttt2:   ... {} more", hits.size() - kMaxLines);
}

void Ttt2App::StartWatchdog() {
  if (watchdog_period_ms_ == 0 || watchdog_.joinable()) return;
  watchdog_stop_ = false;
  watchdog_ = std::thread([this] {
    while (!watchdog_stop_) {
      for (std::uint32_t waited = 0; waited < watchdog_period_ms_ && !watchdog_stop_; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!watchdog_stop_) WatchdogTick();
    }
  });
}

void Ttt2App::StopWatchdog() {
  watchdog_stop_ = true;
  if (watchdog_.joinable()) watchdog_.join();
}

void Ttt2App::WatchdogTick() {
  // Periodic census of the guest threads: TTT2 starts about twenty (nuscMain, nuscFileAsyncRequest,
  // ... per TTT2 Web), so a boot that stalls shows up here as a frozen population long before
  // anything is drawn. Names and counts only.
  auto* rt = runtime();
  if (!rt || !rt->kernel_state()) return;
  auto threads = rt->kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
  std::size_t running = 0;
  std::string names;
  for (const auto& t : threads) {
    if (t->is_running()) ++running;
    if (names.size() < 400) {
      if (!names.empty()) names += ", ";
      names += t->name().empty() ? ("#" + std::to_string(t->thread_id())) : t->name();
    }
  }
  const bool main_running = main_thread_ && main_thread_->is_running();
  const std::uint64_t stub_total = stubs_ ? stubs_->total_calls() : 0;
  REXLOG_INFO("ttt2: watchdog: {} guest threads ({} running); main thread {}: {}; kernel stubs: {}",
              threads.size(), running, main_running ? "running" : "not running", names,
              stubs_ ? stubs_->Summary(5) : std::string("not counted"));

  // Stall heuristic: nothing changed for three ticks while the game is supposedly running. A boot
  // that waits forever on an event, a vblank or a stubbed call looks exactly like this; the
  // per-thread dump says who is waiting on what.
  const bool unchanged = threads.size() == last_thread_count_ && running == last_running_count_ &&
                         stub_total == last_stub_total_;
  unchanged_ticks_ = unchanged ? unchanged_ticks_ + 1 : 0;
  last_thread_count_ = threads.size();
  last_running_count_ = running;
  last_stub_total_ = stub_total;
  if (main_running && unchanged_ticks_ == 3) {
    REXLOG_WARN("ttt2: possible stall: thread census and stub calls unchanged for {} ms; per-thread state:",
                3u * watchdog_period_ms_);
    for (const auto& t : threads) {
      const auto* k = t->guest_object<rex::system::X_KTHREAD>();
      REXLOG_WARN("ttt2:   {:<32} id {:<6} {} priority {} suspend {} wait_result {:#x}",
                  t->name().empty() ? "(unnamed)" : t->name(), t->thread_id(),
                  t->is_running() ? "running" : "stopped", t->priority(), t->suspend_count(),
                  k ? static_cast<std::uint32_t>(k->wait_result) : 0u);
    }
  }
}
