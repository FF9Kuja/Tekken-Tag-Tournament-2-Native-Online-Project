#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "lock_tracker.h"
#include "object_tracker.h"

using namespace ttt2;

TEST_CASE("locks: a section entered without leaving keeps its open enter sites and the stacks of its deepest enters") {
  locks::Reset();
  // Thread 6 enters the heap lock from the allocator 390 times and leaves 0 times (the owner's dumps), with one
  // balanced enter/leave pair from another site in between.
  const std::uint32_t heap = 0x40000618;
  std::uint32_t depth = 0;
  std::vector<std::uint32_t> snapshot_depths;
  for (std::uint64_t i = 0; i < 390; ++i) {
    depth = locks::NoteEnter(6, heap, 0x82BD6888, 1000 + i);
    if (locks::SnapshotDepth(depth)) {
      snapshot_depths.push_back(depth);
      locks::NoteStack(6, heap, depth, 1000 + i, {0x82BD6888, 0x82BD7FAC, 0x82552570});
    }
    if (i == 100) {
      locks::NoteEnter(6, heap, 0x82AAD844, 1100);
      locks::NoteLeave(6, heap, 1101);
    }
  }
  CHECK(depth == 390);
  CHECK(snapshot_depths == std::vector<std::uint32_t>{1, 2, 4, 8, 16, 32, 64, 128, 256});
  const locks::Hold hold = locks::HoldOf(6, heap);
  CHECK(hold.depth == 390);
  CHECK(hold.since_ms == 1000);
  CHECK(hold.enters == 391);
  CHECK(hold.leaves == 1);
  REQUIRE(hold.open_sites.size() == 1);
  CHECK(hold.open_sites[0] == std::pair<std::uint32_t, std::uint32_t>{0x82BD6888, 390});
  CHECK(hold.snapshots.size() == 9);
  CHECK(hold.snapshots.back().depth == 256);
  CHECK(hold.snapshots.back().stack.size() == 3);
  CHECK(locks::HoldsOf(6).size() == 1);
  CHECK(locks::HoldsOf(12).empty());
}

TEST_CASE("locks: balanced enters leave nothing behind; a leave without an enter is counted") {
  locks::Reset();
  const std::uint32_t cs = 0x8309F45C;
  CHECK(locks::NoteEnter(12, cs, 0x82BFAB04, 10) == 1);
  CHECK(locks::NoteEnter(12, cs, 0x82BFA888, 11) == 2);
  locks::NoteStack(12, cs, 2, 11, {0x82BFA888});
  locks::NoteLeave(12, cs, 12);
  CHECK(locks::HoldOf(12, cs).depth == 1);
  CHECK(locks::HoldOf(12, cs).snapshots.size() == 0);  // the depth-2 snapshot went with its enter
  locks::NoteLeave(12, cs, 13);
  CHECK(locks::HoldOf(12, cs).depth == 0);
  CHECK(locks::HoldsOf(12).empty());
  locks::NoteLeave(12, cs, 14);
  CHECK(locks::UnmatchedLeaves(12) == 1);
  // Two sections at once, reported deepest first.
  locks::NoteEnter(14, 0x1000, 0x82000010, 20);
  locks::NoteEnter(14, 0x2000, 0x82000020, 21);
  locks::NoteEnter(14, 0x2000, 0x82000020, 22);
  const auto holds = locks::HoldsOf(14);
  REQUIRE(holds.size() == 2);
  CHECK(holds[0].section == 0x2000);
  CHECK(holds[1].section == 0x1000);
}

namespace {
std::string Label(std::uint32_t id) { return "thread id " + std::to_string(id); }
std::string Where(std::uint32_t lr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", lr);
  return buf;
}
}  // namespace

TEST_CASE("objects: the kind comes from the create call, signals name their thread and site, a close forgets") {
  objects::Reset();
  objects::NoteCreate(0xF8000108, objects::Kind::kSynchronizationEvent, 0, 0, 6, 0x82BDA100, 300);
  for (int i = 0; i < 1000; ++i) objects::NoteSignal(0xF8000108, 19, 0x82BCA6DC, 180000);
  objects::NoteSignal(0xF8000108, 6, 0x82AAD844, 179000);  // an earlier signal from another thread, logged later
  auto info = objects::Find(0xF8000108);
  REQUIRE(info.has_value());
  CHECK(info->kind == objects::Kind::kSynchronizationEvent);
  CHECK(info->signals == 1001);
  CHECK(info->signaled_by == 6);
  const std::string text = objects::Describe(*info, 180100, &Label, &Where);
  CHECK(text == "synchronization event, created by thread id 6 at t+0.3 s; last signaled by thread id 6 from 0x82AAD844 at "
                "t+179.0 s, 1001 signals from 2 threads");

  objects::NoteCreate(0xF8000110, objects::Kind::kSemaphore, 0, 32, 6, 0x82BDA200, 400);
  CHECK(objects::Describe(*objects::Find(0xF8000110), 500, &Label, &Where) ==
        "semaphore, created by thread id 6 at t+0.4 s, count 0 of 32; never signaled since the host saw it");
  objects::NoteClose(0xF8000110);
  CHECK_FALSE(objects::Find(0xF8000110).has_value());
  // A Ke* object in guest memory: no create seen, the kind stays unknown, signals still count.
  objects::NoteSignal(0x40047D4C, 24, 0x8271CB10, 700);
  CHECK(objects::Find(0x40047D4C)->kind == objects::Kind::kUnknown);
  CHECK(objects::Find(0x40047D4C)->signals == 1);
}
