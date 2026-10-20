#include "object_tracker.h"

#include <bit>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace ttt2::objects {
namespace {

struct Registry {
  std::mutex mutex;
  std::unordered_map<std::uint32_t, Info> objects;
};

Registry& registry() {
  static Registry r;
  return r;
}

std::string Seconds(std::uint64_t ms) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "t+%.1f s", static_cast<double>(ms) / 1000.0);
  return buf;
}

}  // namespace

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kNotificationEvent: return "notification event";
    case Kind::kSynchronizationEvent: return "synchronization event";
    case Kind::kMutant: return "mutant";
    case Kind::kSemaphore: return "semaphore";
    case Kind::kTimer: return "timer";
    default: return "object";
  }
}

void NoteCreate(std::uint32_t key, Kind kind, std::uint32_t initial, std::uint32_t maximum, std::uint32_t thread_id,
                std::uint32_t lr, std::uint64_t ms) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  Info info;
  info.kind = kind;
  info.created_by = thread_id;
  info.created_lr = lr;
  info.created_ms = ms;
  info.initial = initial;
  info.maximum = maximum;
  r.objects[key] = info;  // a reused handle starts afresh
}

void NoteSignal(std::uint32_t key, std::uint32_t thread_id, std::uint32_t lr, std::uint64_t ms) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  Info& info = r.objects[key];  // objects the host saw no create of (Ke* objects in guest memory) get an entry
  ++info.signals;
  info.signaled_by = thread_id;
  info.signaled_lr = lr;
  info.signaled_ms = ms;
  info.signalers |= std::uint64_t(1) << (thread_id % 64);
}

void NoteClear(std::uint32_t key) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.objects.find(key);
  if (it != r.objects.end()) ++it->second.clears;
}

void NoteClose(std::uint32_t key) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.objects.erase(key);
}

std::optional<Info> Find(std::uint32_t key) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.objects.find(key);
  if (it == r.objects.end()) return std::nullopt;
  return it->second;
}

std::string Describe(const Info& info, std::uint64_t now_ms, std::string (*label)(std::uint32_t),
                     std::string (*where)(std::uint32_t)) {
  (void)now_ms;
  std::string out = KindName(info.kind);
  std::string details;
  if (info.created_by || info.created_ms) {
    details = "created by " + label(info.created_by) + " at " + Seconds(info.created_ms);
    if (info.kind == Kind::kSemaphore) {
      details += ", count " + std::to_string(info.initial) + " of " + std::to_string(info.maximum);
    } else if (info.kind == Kind::kNotificationEvent || info.kind == Kind::kSynchronizationEvent) {
      details += info.initial ? ", initially signaled" : "";
    }
  }
  if (!details.empty()) details += "; ";
  if (info.signals) {
    details += "last signaled by " + label(info.signaled_by) + " from " + where(info.signaled_lr) + " at " +
               Seconds(info.signaled_ms) + ", " + std::to_string(info.signals) +
               (info.signals == 1 ? " signal" : " signals");
    const int setters = std::popcount(info.signalers);
    if (setters > 1) details += " from " + std::to_string(setters) + " threads";
  } else {
    details += "never signaled since the host saw it";
  }
  if (info.clears) details += ", " + std::to_string(info.clears) + (info.clears == 1 ? " clear" : " clears");
  return out + ", " + details;
}

void Reset() {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.objects.clear();
}

}  // namespace ttt2::objects
