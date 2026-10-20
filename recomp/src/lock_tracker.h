// Who holds which critical section, and from where they entered it (SDK-free).
//
// The owner's boots show a critical section (0x40000618, in the 64 KB-page heap where the game's process heap
// lives) held by the main thread at recursion 390 for the whole run, while the audio engine's thread and a loader
// thread wait for it. The section's own fields say who owns it and how deep, not how it got there. This tracker,
// fed by the host's wrappers of RtlEnterCriticalSection, RtlTryEnterCriticalSection and RtlLeaveCriticalSection,
// keeps per thread the sections it holds: the depth it entered them to, when, and the guest call site of every
// enter still open (a stack of return addresses), plus a guest call stack at the enters that took the depth to
// 1, 2, 4, 8, ... 1024. The stall dump then names the call sites that entered a section without leaving it.
//
// Cost: a per-thread mutex (taken by its own thread on every enter and leave, and by the watchdog while it reads)
// and a few vector operations; the owner's boots make a few hundred critical-section calls a second.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ttt2::locks {

/// What one thread's hold of one section looks like now.
struct Hold {
  std::uint32_t section = 0;
  std::uint32_t depth = 0;            ///< enters minus leaves seen by the wrappers
  std::uint64_t since_ms = 0;         ///< when the depth went from 0 to 1
  std::uint64_t changed_ms = 0;       ///< the last enter or leave
  std::uint64_t enters = 0;           ///< enters since since_ms
  std::uint64_t leaves = 0;           ///< leaves since since_ms
  /// Return addresses of the enters still open, grouped: (address, count), most frequent first.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> open_sites;
  /// Guest call stacks (innermost first) of the enters that took the depth to 1, 2, 4, 8, ...: (depth, ms, stack).
  struct Snapshot {
    std::uint32_t depth = 0;
    std::uint64_t ms = 0;
    std::vector<std::uint32_t> stack;
  };
  std::vector<Snapshot> snapshots;
};

/// True when the enter that takes a hold to `depth` should record a call stack (1, 2, 4, ... 1024).
bool SnapshotDepth(std::uint32_t depth);

/// The calling thread entered `section` (the runtime's function returned owning it), from guest address `lr`.
/// Returns the hold's new depth; when SnapshotDepth(depth), the caller passes the stack to NoteStack.
std::uint32_t NoteEnter(std::uint32_t thread_id, std::uint32_t section, std::uint32_t lr, std::uint64_t now_ms);

/// The guest call stack of the enter that took the calling thread's hold of `section` to `depth`.
void NoteStack(std::uint32_t thread_id, std::uint32_t section, std::uint32_t depth, std::uint64_t now_ms,
               std::vector<std::uint32_t> stack);

/// The calling thread left `section` once. A leave of a section the tracker saw no enter of is counted.
void NoteLeave(std::uint32_t thread_id, std::uint32_t section, std::uint64_t now_ms);

/// The sections `thread_id` holds now (depth >= 1), deepest first.
std::vector<Hold> HoldsOf(std::uint32_t thread_id);

/// `thread_id`'s hold of `section`, if any (depth 0 when none).
Hold HoldOf(std::uint32_t thread_id, std::uint32_t section);

/// Leaves seen without a matching enter on that thread (the runtime's or another thread's enter), per thread.
std::uint64_t UnmatchedLeaves(std::uint32_t thread_id);

/// Forgets everything (tests).
void Reset();

}  // namespace ttt2::locks
