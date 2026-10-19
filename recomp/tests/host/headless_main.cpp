// Headless host for recompiled test programs: no window, no GPU, no audio.
//
// Loads a license-free XEX2 test image into a tool-mode ReXGlue Runtime (the same runtime the
// game host uses, minus presentation), launches its entry point on a guest thread, waits for the
// thread to exit and reports the 32-bit values the program stored at a known guest address. The
// smoke test compares them with the values the program must produce, which proves - rather than
// assumes - that the recompiled jump tables and instructions behave correctly on this toolchain.
//
//   headless_host --game-root DIR --xex NAME.xex [--results 0xADDR:COUNT] [--expect v,v,...]
//                 [--max-stub-calls N] [--timeout-ms 10000] [--log-level warn]
//
// The kernel stubs the program reaches ("<name> STUB" warnings of the runtime) are counted and
// reported; --max-stub-calls makes more than N of them a failure.
//
// Exit codes: 0 results as expected (or no expectation given), 1 mismatch or too many stub calls,
// 2 setup failure, 3 the guest thread did not finish in time.

#include TTT2_GUEST_INIT_HEADER

#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stub_counter.h"

namespace {

struct Options {
  std::string game_root;
  std::string xex;
  std::uint32_t results_address = 0;
  unsigned results_count = 0;
  std::vector<std::uint32_t> expect;
  unsigned timeout_ms = 10000;
  std::uint64_t max_stub_calls = std::numeric_limits<std::uint64_t>::max();
  std::string log_level = "warn";
};

void usage() {
  std::fprintf(
      stderr,
      "usage: headless_host --game-root DIR --xex NAME.xex [--results 0xADDR:COUNT]\n"
      "                     [--expect v,v,...] [--max-stub-calls N] [--timeout-ms N] [--log-level LEVEL]\n");
}

bool parse(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--game-root") {
      const char* v = value();
      if (!v) return false;
      opt.game_root = v;
    } else if (a == "--xex") {
      const char* v = value();
      if (!v) return false;
      opt.xex = v;
    } else if (a == "--results") {
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      opt.results_address = static_cast<std::uint32_t>(std::strtoul(v, &end, 16));
      if (!end || *end != ':') return false;
      opt.results_count = static_cast<unsigned>(std::strtoul(end + 1, nullptr, 10));
    } else if (a == "--expect") {
      const char* v = value();
      if (!v) return false;
      std::string s(v);
      std::size_t pos = 0;
      while (pos <= s.size()) {
        const std::size_t comma = s.find(',', pos);
        const std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty())
          opt.expect.push_back(static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 0)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (a == "--max-stub-calls") {
      const char* v = value();
      if (!v) return false;
      opt.max_stub_calls = std::strtoull(v, nullptr, 10);
    } else if (a == "--timeout-ms") {
      const char* v = value();
      if (!v) return false;
      opt.timeout_ms = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (a == "--log-level") {
      const char* v = value();
      if (!v) return false;
      opt.log_level = v;
    } else {
      return false;
    }
  }
  return !opt.game_root.empty() && !opt.xex.empty();
}

std::uint32_t load_be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

namespace {

// SDK 0.10.0 declares BuildLogConfig(log_file, level, categories); its development branch dropped the log_file
// parameter (the path moved into LogConfig). These overloads are constrained on the signature the SDK in use
// declares and their bodies are dependent, so only the matching one is instantiated: the harness builds against
// the pin and against a snapshot under evaluation (docs/decisions/0002-sdk-pin-and-snapshot-evaluation.md).
template <class Level, class Categories>
  requires requires(const Level& l, const Categories& c) { rex::BuildLogConfig(l, c); }
rex::LogConfig BuildLogConfigCompat(const Level& level, const Categories& categories) {
  return rex::BuildLogConfig(level, categories);
}

template <class Level, class Categories>
  requires(!requires(const Level& l, const Categories& c) { rex::BuildLogConfig(l, c); })
rex::LogConfig BuildLogConfigCompat(const Level& level, const Categories& categories) {
  return rex::BuildLogConfig(nullptr, level, categories);
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible if the guest hangs and we are killed
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }

  rex::InitLoggingEarly();
  const std::map<std::string, std::string> no_categories;
  auto log_config = BuildLogConfigCompat(opt.log_level, no_categories);
  log_config.log_to_console = true;
  rex::InitLogging(log_config);
  // Count the "<name> STUB" warnings the runtime logs for kernel exports it does not implement.
  auto stubs = std::make_shared<ttt2::KernelStubCounter>();
  stubs->set_level(spdlog::level::warn);
  rex::AddSink(stubs);

  std::printf("headless_host: project %s, image base 0x%08X size 0x%X, code 0x%08X+0x%X\n",
              TTT2_GUEST_PROJECT_NAME, PPCImageConfig.image_base, PPCImageConfig.image_size,
              PPCImageConfig.code_base, PPCImageConfig.code_size);

  rex::Runtime runtime(std::filesystem::path(opt.game_root));
  rex::RuntimeConfig config;
  config.kernel_init = rex::kernel::InitializeKernel;
  config.tool_mode = true;  // no GPU, no presentation
  const rex::X_STATUS setup = runtime.Setup(PPCImageConfig, std::move(config));
  if (XFAILED(setup)) {
    std::fprintf(stderr, "headless_host: runtime setup failed (status 0x%08X)\n", setup);
    return 2;
  }
  std::printf("headless_host: runtime ready (tool mode)\n");
  const std::string vfs_path = "game:\\" + opt.xex;
  const rex::X_STATUS loaded = runtime.LoadXexImage(vfs_path);
  if (XFAILED(loaded)) {
    std::fprintf(stderr, "headless_host: loading %s failed (status 0x%08X)\n", vfs_path.c_str(), loaded);
    return 2;
  }

  std::printf("headless_host: image loaded, launching the module\n");
  auto thread = runtime.LaunchModule();
  if (!thread) {
    std::fprintf(stderr, "headless_host: the module did not launch\n");
    return 2;
  }
  // The guest thread marks its kernel object signaled when it exits (XThread::Exit).
  auto* kthread = thread->guest_object<rex::system::X_KTHREAD>();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.timeout_ms);
  while (kthread->header.signal_state == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (kthread->header.signal_state == 0) {
    std::fprintf(stderr, "headless_host: the guest thread is still running after %u ms\n", opt.timeout_ms);
    return 3;
  }
  const std::uint32_t exit_status = kthread->exit_status;
  std::printf("headless_host: guest main thread exited, status 0x%08X (%u)\n", exit_status, exit_status);

  int rc = 0;
  if (opt.results_count) {
    const std::uint8_t* base = runtime.virtual_membase();
    for (unsigned i = 0; i < opt.results_count; ++i) {
      const std::uint32_t v = load_be32(base + opt.results_address + 4u * i);
      const bool checked = i < opt.expect.size();
      const bool ok = !checked || opt.expect[i] == v;
      std::printf("result[%2u] = %10u (0x%08X)%s\n", i, v, v, checked ? (ok ? "  ok" : "  MISMATCH") : "");
      if (checked && !ok) rc = 1;
    }
    if (!opt.expect.empty() && opt.expect.size() != opt.results_count) {
      std::fprintf(stderr, "headless_host: %zu expected values for %u results\n", opt.expect.size(),
                   opt.results_count);
      rc = 1;
    }
  }
  std::printf("headless_host: kernel stubs: %s\n", stubs->Summary().c_str());
  if (stubs->total_calls() > opt.max_stub_calls) {
    std::fprintf(stderr, "headless_host: %llu stub calls, at most %llu allowed\n",
                 static_cast<unsigned long long>(stubs->total_calls()),
                 static_cast<unsigned long long>(opt.max_stub_calls));
    rc = 1;
  }
  std::printf("headless_host: %s\n", rc == 0 ? "PASS" : "FAIL");
  std::fflush(stdout);
  // The guest thread has exited; the process ends here. Runtime teardown with live kernel
  // state is the app's job (ReXApp::OnDestroy); this harness only needs the results.
  std::_Exit(rc);
}
