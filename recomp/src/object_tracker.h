// What the game's dispatcher objects are and who signals them (SDK-free).
//
// The runtime gives an object created by handle (NtCreateEvent, NtCreateMutant, NtCreateSemaphore) a guest header
// that it zeroes and never updates, so a dump that reads the header calls every such event a "notification event,
// not signaled" whatever it is (measured with the pinned SDK: XEvent::Initialize never writes it; the owner's
// dumps show thread 19's synchronization event that way). The host's wrappers record here what the game created
// (the kind and initial state from the create call's arguments) and every signal (set, pulse, release, the signal
// half of a signal-and-wait: who, from where, how often), keyed by handle - or by guest address for objects the
// game keeps in its own memory (Ke* calls). A closed handle is forgotten.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ttt2::objects {

enum class Kind : std::uint8_t {
  kUnknown,
  kNotificationEvent,    ///< manual reset (event type 0)
  kSynchronizationEvent, ///< auto reset (event type 1)
  kMutant,
  kSemaphore,
  kTimer,
};

const char* KindName(Kind kind);

struct Info {
  Kind kind = Kind::kUnknown;
  std::uint32_t created_by = 0;   ///< thread id
  std::uint32_t created_lr = 0;
  std::uint64_t created_ms = 0;
  std::uint32_t initial = 0;      ///< initial state (events), initial count (semaphores), initial owner (mutants)
  std::uint32_t maximum = 0;      ///< semaphores
  std::uint64_t signals = 0;      ///< sets, pulses, releases, signal-and-wait signals
  std::uint64_t clears = 0;
  std::uint32_t signaled_by = 0;  ///< thread id of the last signal (0: never signaled)
  std::uint32_t signaled_lr = 0;
  std::uint64_t signaled_ms = 0;
  std::uint64_t signalers = 0;    ///< bit (id mod 64) per thread that signaled it, to tell one setter from several
};

void NoteCreate(std::uint32_t key, Kind kind, std::uint32_t initial, std::uint32_t maximum, std::uint32_t thread_id,
                std::uint32_t lr, std::uint64_t ms);
void NoteSignal(std::uint32_t key, std::uint32_t thread_id, std::uint32_t lr, std::uint64_t ms);
void NoteClear(std::uint32_t key);
void NoteClose(std::uint32_t key);
std::optional<Info> Find(std::uint32_t key);

/// "synchronization event, created by thread id 6 at t+0.3 s; last signaled by thread id 19 from 0x82BCA6DC at
/// t+180.0 s, 329085374 signals"; `label` names thread ids, `where` names guest addresses.
std::string Describe(const Info& info, std::uint64_t now_ms, std::string (*label)(std::uint32_t),
                     std::string (*where)(std::uint32_t));

void Reset();

}  // namespace ttt2::objects
