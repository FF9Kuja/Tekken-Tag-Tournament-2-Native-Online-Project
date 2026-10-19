// Counts the kernel/XAM stubs the guest calls, from the SDK's own log lines.
//
// Every export the ReXGlue runtime declares with REX_EXPORT_STUB logs "<__imp__Name> STUB" at warn
// level on the kernel log category when the game calls it (rex/hook.h). Attached as a spdlog sink
// to that category, this counter turns the stream into the list that matters for Milestone 4: which
// system calls the game actually reaches that the runtime does not implement, how often, and in
// which order they first appeared. Names and counts only; nothing from the game.
#pragma once

#include <rex/logging/sink.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ttt2 {

class KernelStubCounter : public spdlog::sinks::base_sink<std::mutex> {
 public:
  struct Hit {
    std::string name;  ///< export name without the __imp__ prefix
    std::uint64_t calls = 0;
    std::uint64_t first_seen = 0;  ///< ordinal of the first call among all stub calls
  };

  /// All stubs hit so far, most-called first (ties: first seen first).
  std::vector<Hit> Snapshot() const;
  std::uint64_t total_calls() const;

  /// One line for the log: "N calls to M stubs; top: A x3, B x1" (at most `top` names).
  std::string Summary(std::size_t top = 8) const;

  /// The most recent stub calls, oldest first (at most kRecent): what the game did right before a crash.
  static constexpr std::size_t kRecent = 16;
  std::vector<std::string> Recent() const;

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override;
  void flush_() override {}

 private:
  mutable std::mutex hits_mutex_;
  std::unordered_map<std::string, Hit> hits_;
  std::uint64_t total_ = 0;
  std::string recent_[kRecent];  ///< ring of the last stub names, written at total_ % kRecent
};

}  // namespace ttt2
