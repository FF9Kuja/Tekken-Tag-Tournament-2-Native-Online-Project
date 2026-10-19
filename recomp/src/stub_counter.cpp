#include "stub_counter.h"

#include <algorithm>

#include "stub_name.hpp"

namespace ttt2 {

void KernelStubCounter::sink_it_(const spdlog::details::log_msg& msg) {
  const std::string name = stub_name(std::string_view(msg.payload.data(), msg.payload.size()));
  if (name.empty()) return;
  std::lock_guard lock(hits_mutex_);
  recent_[total_ % kRecent] = name;
  ++total_;
  auto it = hits_.find(name);
  if (it == hits_.end()) it = hits_.emplace(name, Hit{name, 0, total_}).first;
  ++it->second.calls;
}

std::vector<std::string> KernelStubCounter::Recent() const {
  std::lock_guard lock(hits_mutex_);
  std::vector<std::string> out;
  const std::uint64_t count = total_ < kRecent ? total_ : kRecent;
  for (std::uint64_t i = 0; i < count; ++i) out.push_back(recent_[(total_ - count + i) % kRecent]);
  return out;
}

std::vector<KernelStubCounter::Hit> KernelStubCounter::Snapshot() const {
  std::vector<Hit> out;
  {
    std::lock_guard lock(hits_mutex_);
    out.reserve(hits_.size());
    for (const auto& [_, hit] : hits_) out.push_back(hit);
  }
  std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
    return a.calls != b.calls ? a.calls > b.calls : a.first_seen < b.first_seen;
  });
  return out;
}

std::uint64_t KernelStubCounter::total_calls() const {
  std::lock_guard lock(hits_mutex_);
  return total_;
}

std::string KernelStubCounter::Summary(std::size_t top) const {
  const auto hits = Snapshot();
  std::string s = std::to_string(total_calls()) + " calls to " + std::to_string(hits.size()) + " stubs";
  if (hits.empty()) return s;
  s += "; top:";
  for (std::size_t i = 0; i < hits.size() && i < top; ++i) {
    s += (i ? ", " : " ") + hits[i].name + " x" + std::to_string(hits[i].calls);
  }
  if (hits.size() > top) s += ", +" + std::to_string(hits.size() - top) + " more";
  return s;
}

}  // namespace ttt2
