#include "lock_tracker.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ttt2::locks {
namespace {

constexpr std::size_t kMaxOpenSites = 4096;   // return addresses kept per hold (the depth itself is exact)
constexpr std::uint32_t kMaxSnapshotDepth = 1024;

struct HoldState {
  std::uint32_t section = 0;
  std::uint32_t depth = 0;
  std::uint64_t since_ms = 0;
  std::uint64_t changed_ms = 0;
  std::uint64_t enters = 0;
  std::uint64_t leaves = 0;
  std::vector<std::uint32_t> sites;  // LIFO: the enter sites still open
  std::vector<Hold::Snapshot> snapshots;
};

struct ThreadLocks {
  std::mutex mutex;
  std::vector<HoldState> holds;  // a thread holds few sections at once
  std::uint64_t unmatched_leaves = 0;
};

struct Registry {
  std::mutex mutex;
  std::unordered_map<std::uint32_t, std::unique_ptr<ThreadLocks>> threads;
};

Registry& registry() {
  static Registry r;
  return r;
}

ThreadLocks& For(std::uint32_t thread_id) {
  // Each guest thread runs on its own host thread: cache the last lookup.
  thread_local std::uint32_t cached_id = 0;
  thread_local ThreadLocks* cached = nullptr;
  if (cached && cached_id == thread_id) return *cached;
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto& slot = r.threads[thread_id];
  if (!slot) slot = std::make_unique<ThreadLocks>();
  cached_id = thread_id;
  cached = slot.get();
  return *slot;
}

ThreadLocks* Find(std::uint32_t thread_id) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.threads.find(thread_id);
  return it == r.threads.end() ? nullptr : it->second.get();
}

Hold ToHold(const HoldState& h) {
  Hold out;
  out.section = h.section;
  out.depth = h.depth;
  out.since_ms = h.since_ms;
  out.changed_ms = h.changed_ms;
  out.enters = h.enters;
  out.leaves = h.leaves;
  std::map<std::uint32_t, std::uint32_t> counts;
  for (std::uint32_t lr : h.sites) ++counts[lr];
  out.open_sites.assign(counts.begin(), counts.end());
  std::stable_sort(out.open_sites.begin(), out.open_sites.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  out.snapshots = h.snapshots;
  return out;
}

}  // namespace

bool SnapshotDepth(std::uint32_t depth) {
  return depth >= 1 && depth <= kMaxSnapshotDepth && (depth & (depth - 1)) == 0;
}

std::uint32_t NoteEnter(std::uint32_t thread_id, std::uint32_t section, std::uint32_t lr, std::uint64_t now_ms) {
  ThreadLocks& t = For(thread_id);
  std::lock_guard<std::mutex> lock(t.mutex);
  auto it = std::find_if(t.holds.begin(), t.holds.end(), [&](const HoldState& h) { return h.section == section; });
  if (it == t.holds.end()) {
    HoldState h;
    h.section = section;
    h.since_ms = now_ms;
    t.holds.push_back(std::move(h));
    it = t.holds.end() - 1;
  }
  ++it->depth;
  ++it->enters;
  it->changed_ms = now_ms;
  if (it->sites.size() < kMaxOpenSites) it->sites.push_back(lr);
  return it->depth;
}

void NoteStack(std::uint32_t thread_id, std::uint32_t section, std::uint32_t depth, std::uint64_t now_ms,
               std::vector<std::uint32_t> stack) {
  ThreadLocks& t = For(thread_id);
  std::lock_guard<std::mutex> lock(t.mutex);
  auto it = std::find_if(t.holds.begin(), t.holds.end(), [&](const HoldState& h) { return h.section == section; });
  if (it == t.holds.end()) return;
  it->snapshots.push_back({depth, now_ms, std::move(stack)});
}

void NoteLeave(std::uint32_t thread_id, std::uint32_t section, std::uint64_t now_ms) {
  ThreadLocks& t = For(thread_id);
  std::lock_guard<std::mutex> lock(t.mutex);
  auto it = std::find_if(t.holds.begin(), t.holds.end(), [&](const HoldState& h) { return h.section == section; });
  if (it == t.holds.end() || it->depth == 0) {
    ++t.unmatched_leaves;
    return;
  }
  --it->depth;
  ++it->leaves;
  it->changed_ms = now_ms;
  if (!it->sites.empty() && it->sites.size() > it->depth) it->sites.pop_back();
  // Snapshots of depths no longer held describe enters that were left again.
  while (!it->snapshots.empty() && it->snapshots.back().depth > it->depth) it->snapshots.pop_back();
  if (it->depth == 0) t.holds.erase(it);
}

std::vector<Hold> HoldsOf(std::uint32_t thread_id) {
  std::vector<Hold> out;
  ThreadLocks* t = Find(thread_id);
  if (!t) return out;
  std::lock_guard<std::mutex> lock(t->mutex);
  for (const HoldState& h : t->holds) {
    if (h.depth) out.push_back(ToHold(h));
  }
  std::stable_sort(out.begin(), out.end(), [](const Hold& a, const Hold& b) { return a.depth > b.depth; });
  return out;
}

Hold HoldOf(std::uint32_t thread_id, std::uint32_t section) {
  ThreadLocks* t = Find(thread_id);
  if (!t) return {};
  std::lock_guard<std::mutex> lock(t->mutex);
  for (const HoldState& h : t->holds) {
    if (h.section == section) return ToHold(h);
  }
  return {};
}

std::uint64_t UnmatchedLeaves(std::uint32_t thread_id) {
  ThreadLocks* t = Find(thread_id);
  if (!t) return 0;
  std::lock_guard<std::mutex> lock(t->mutex);
  return t->unmatched_leaves;
}

void Reset() {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  for (auto& [id, t] : r.threads) {
    std::lock_guard<std::mutex> inner(t->mutex);
    t->holds.clear();
    t->unmatched_leaves = 0;
  }
}

}  // namespace ttt2::locks
