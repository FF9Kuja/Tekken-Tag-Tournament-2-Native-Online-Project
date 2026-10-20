#include <doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pm4_stats.h"

using namespace ttt2::pm4;

namespace {

// Synthetic GPU memory: indirect buffers by address.
struct FakeMemory {
  std::map<std::uint32_t, std::vector<std::uint32_t>> buffers;
  int reads = 0;

  ReadFn reader() {
    return [this](std::uint32_t address, std::uint32_t dwords, std::vector<std::uint32_t>& out) {
      ++reads;
      auto it = buffers.find(address);
      if (it == buffers.end() || it->second.size() < dwords) return false;
      out.assign(it->second.begin(), it->second.begin() + dwords);
      return true;
    };
  }
};

std::uint32_t ModeControl(std::uint32_t mode) { return mode; }

// A DRAW_INDX_2 with an auto-index initiator (one word).
void PushDraw2(std::vector<std::uint32_t>& s) {
  s.push_back(Type3(kOpDrawIndx2, 1));
  s.push_back(0x00030004);  // 3 indices, triangle list, auto index
}

// A DRAW_INDX with a viz query word and a DMA initiator plus base/size (four words).
void PushDrawIndx(std::vector<std::uint32_t>& s) {
  s.push_back(Type3(kOpDrawIndx, 4));
  s.push_back(0);
  s.push_back(0x00060004);
  s.push_back(0x1F000000);
  s.push_back(6);
}

}  // namespace

TEST_CASE("pm4: draws count by the EDRAM mode in force") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  s.push_back(Type0(kRegModeControl, 1));
  s.push_back(ModeControl(kEdramColorDepth));
  PushDraw2(s);
  PushDrawIndx(s);
  s.push_back(Type0(kRegModeControl, 1));
  s.push_back(ModeControl(kEdramDepthOnly));
  PushDraw2(s);
  s.push_back(Type0(kRegModeControl, 1));
  s.push_back(ModeControl(kEdramNoOperation));
  PushDraw2(s);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.draws == 3);
  CHECK(stats.draws_skipped == 1);
  CHECK(stats.resolves == 0);
  CHECK(stats.packets == 7);
  CHECK(stats.dwords == s.size());
  CHECK(stats.malformed == 0);
  CHECK_FALSE(stats.truncated);
}

TEST_CASE("pm4: a draw in copy mode is a resolve, clearing when RB_COPY_CONTROL says so") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  // Through SET_CONSTANT type 4 (registers from 0x2000): RB_MODECONTROL = copy.
  s.push_back(Type3(kOpSetConstant, 2));
  s.push_back((4u << 16) | (kRegModeControl - 0x2000));
  s.push_back(kEdramCopy);
  // Type 0, two registers in a row: RB_COPY_CONTROL = color clear, RB_COPY_DEST_BASE = 0x1F400000.
  s.push_back(Type0(kRegCopyControl, 2));
  s.push_back(0x100);
  s.push_back(0x1F400000);
  PushDrawIndx(s);
  s.push_back(Type0(kRegCopyControl, 1));
  s.push_back(0);
  PushDrawIndx(s);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.resolves == 2);
  CHECK(stats.clears == 1);
  CHECK(stats.draws == 0);
  CHECK(stats.last_resolve_dest == 0x1F400000);
  // The mode carries over to the next frame, as the GPU keeps it.
  CHECK(parser.mode_control() == kEdramCopy);
  std::vector<std::uint32_t> next;
  PushDraw2(next);
  FrameStats next_stats;
  parser.Parse(next.data(), next.size(), next_stats);
  CHECK(next_stats.resolves == 1);
}

TEST_CASE("pm4: indirect buffers are followed, nested ones up to the depth limit") {
  FakeMemory memory;
  std::vector<std::uint32_t> inner;
  inner.push_back(Type0(kRegModeControl, 1));
  inner.push_back(kEdramColorDepth);
  PushDraw2(inner);
  PushDraw2(inner);
  memory.buffers[0x00200000] = inner;
  std::vector<std::uint32_t> middle;
  middle.push_back(Type3(kOpIndirectBuffer, 2));
  middle.push_back(0x00200000);
  middle.push_back(static_cast<std::uint32_t>(inner.size()));
  PushDraw2(middle);
  memory.buffers[0x00100000] = middle;
  std::vector<std::uint32_t> ring;
  ring.push_back(Type3(kOpIndirectBufferPfd, 2));
  ring.push_back(0x00100000);
  ring.push_back(static_cast<std::uint32_t>(middle.size()));

  Parser deep(memory.reader(), 1u << 20, 2);
  FrameStats stats;
  deep.Parse(ring.data(), ring.size(), stats);
  CHECK(stats.indirect_buffers == 2);
  CHECK(stats.draws == 3);
  CHECK_FALSE(stats.truncated);

  Parser shallow(memory.reader(), 1u << 20, 1);
  FrameStats shallow_stats;
  shallow.Parse(ring.data(), ring.size(), shallow_stats);
  CHECK(shallow_stats.indirect_buffers == 1);
  CHECK(shallow_stats.draws_skipped == 1);  // the middle buffer's draw: no mode set yet in this parser
  CHECK(shallow_stats.truncated);
}

TEST_CASE("pm4: an indirect buffer outside readable memory is counted, not followed") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> ring;
  ring.push_back(Type3(kOpIndirectBuffer, 2));
  ring.push_back(0x0BAD0000);
  ring.push_back(16);
  PushDraw2(ring);
  FrameStats stats;
  parser.Parse(ring.data(), ring.size(), stats);
  CHECK(stats.unreadable == 1);
  CHECK(stats.indirect_buffers == 0);
  CHECK(stats.draws_skipped == 1);
  CHECK(stats.packets == 2);
}

TEST_CASE("pm4: zero words, type 1 and type 2 packets, swaps and the predicate") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  s.push_back(0);           // skipped like the SDK does: not a packet
  s.push_back(0x80000000);  // type 2: no-op
  // Type 1: two registers (indices 11 bits each): RB_SURFACE_INFO's low index is out of type 1's
  // reach (0x2000 > 0x7FF), so it writes two unrelated registers.
  s.push_back((1u << 30) | (0x10u << 11) | 0x11u);
  s.push_back(1);
  s.push_back(2);
  // A predicated draw while bin select & mask are nonzero runs; with the mask cleared it is skipped.
  s.push_back(Type0(kRegModeControl, 1));
  s.push_back(kEdramColorDepth);
  s.push_back(Type3(kOpDrawIndx2, 1, true));
  s.push_back(0x00030004);
  s.push_back(Type3(kOpSetBinMask, 2));
  s.push_back(0);
  s.push_back(0);
  s.push_back(Type3(kOpDrawIndx2, 1, true));
  s.push_back(0x00030004);
  s.push_back(Type3(kOpXeSwap, 4));
  s.push_back(0x53574150);
  s.push_back(0x1F000000);
  s.push_back(1280);
  s.push_back(720);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.packets == 7);
  CHECK(stats.draws == 1);
  CHECK(stats.predicated_skips == 1);
  CHECK(stats.swaps == 1);
  CHECK(stats.dwords == s.size());
}

TEST_CASE("pm4: render target changes count only real changes") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  s.push_back(Type0(kRegSurfaceInfo, 3));  // surface, color, depth info
  s.push_back(1280);
  s.push_back(0x00000000);
  s.push_back(0x00010000);
  s.push_back(Type0(kRegColorInfo, 1));
  s.push_back(0x00000000);  // same value: no change
  s.push_back(Type0(kRegColorInfo, 1));
  s.push_back(0x00005000);  // a different EDRAM base: a change
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.render_target_changes == 3);  // surface 0 -> 1280, depth 0 -> 0x10000, color 0 -> 0x5000
}

TEST_CASE("pm4: packets the SDK does not execute are listed by opcode") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  s.push_back(Type3(kOpDrawIndxBin, 2));
  s.push_back(0);
  s.push_back(0);
  s.push_back(Type3(0x25, 1));  // SET_STATE
  s.push_back(0);
  s.push_back(Type3(0x25, 1));
  s.push_back(0);
  s.push_back(Type3(kOpNop, 3));  // handled: skipped words
  s.push_back(0xCDCDCDCD);
  s.push_back(0xCDCDCDCD);
  s.push_back(0xCDCDCDCD);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.binned_draws == 1);
  CHECK(stats.unhandled == 3);
  REQUIRE(stats.unhandled_opcodes.size() == 2);
  CHECK(stats.unhandled_opcodes[0] == kOpDrawIndxBin);
  CHECK(stats.unhandled_opcodes[1] == 0x25);
  CHECK(OpcodeHandledBySdk(kOpNop));
  CHECK_FALSE(OpcodeHandledBySdk(0x25));
  const std::string text = Describe(stats);
  CHECK(text.find("3 packets the GPU emulation skips (opcodes 0x34, 0x25)") != std::string::npos);
  CHECK(text.find("1 binned draw") != std::string::npos);
}

TEST_CASE("pm4: a packet longer than its buffer is counted and parsing stops") {
  FakeMemory memory;
  Parser parser(memory.reader());
  std::vector<std::uint32_t> s;
  PushDraw2(s);
  s.push_back(Type3(kOpNop, 8));
  s.push_back(0);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.malformed == 1);
  CHECK(stats.draws_skipped == 1);
}

TEST_CASE("pm4: the word limit truncates a frame") {
  FakeMemory memory;
  Parser parser(memory.reader(), 6);
  std::vector<std::uint32_t> s;
  for (int k = 0; k < 4; ++k) PushDraw2(s);
  FrameStats stats;
  parser.Parse(s.data(), s.size(), stats);
  CHECK(stats.truncated);
  CHECK(stats.draws_skipped == 3);
  CHECK(stats.dwords == 6);
}

TEST_CASE("pm4: ring spans between two positions, wrapping at the end") {
  auto spans = RingSpans(0x1000, 0x100, 0x400);
  REQUIRE(spans.size() == 1);
  CHECK(spans[0].offset == 0x100);
  CHECK(spans[0].bytes == 0x300);
  spans = RingSpans(0x1000, 0xF00, 0x200);
  REQUIRE(spans.size() == 2);
  CHECK(spans[0].offset == 0xF00);
  CHECK(spans[0].bytes == 0x100);
  CHECK(spans[1].offset == 0);
  CHECK(spans[1].bytes == 0x200);
  spans = RingSpans(0x1000, 0xF00, 0);
  REQUIRE(spans.size() == 1);
  CHECK(spans[0].bytes == 0x100);
  CHECK(RingSpans(0x1000, 0x200, 0x200).empty());
  CHECK(RingSpans(0x1000, 0x1000, 0x200).empty());  // out of range
  CHECK(RingSpans(0, 0, 0).empty());
}

TEST_CASE("pm4: frame stats add up and describe themselves") {
  FrameStats a;
  a.draws = 2;
  a.resolves = 1;
  a.clears = 1;
  a.packets = 10;
  a.unhandled = 1;
  a.unhandled_opcodes = {0x25};
  FrameStats b;
  b.draws = 1;
  b.packets = 3;
  b.indirect_buffers = 1;
  b.unhandled = 1;
  b.unhandled_opcodes = {0x25};
  a.Add(b);
  CHECK(a.draws == 3);
  CHECK(a.packets == 13);
  CHECK(a.unhandled_opcodes.size() == 1);
  CHECK(Describe(a) ==
        "3 draws, 1 resolve (1 clearing), 1 indirect buffer, 2 packets the GPU emulation skips (opcode 0x25), 13 packets");
  CHECK(Describe(FrameStats{}) == "0 packets");
}

namespace {

// A ring of `dwords` words at GPU address `base` in FakeMemory, written the way D3D does: whole packets at the
// write position, wrapping at the end, the write pointer moved after each batch.
struct FakeRing {
  FakeMemory& memory;
  std::uint32_t base;
  std::uint32_t dwords;
  std::uint32_t write = 0;

  FakeRing(FakeMemory& m, std::uint32_t b, std::uint32_t n) : memory(m), base(b), dwords(n) {
    // Readable as one buffer from any offset: register every word address with the rest of the ring.
    Sync();
  }
  void Sync() {
    // FakeMemory serves exact (address, length) reads from buffers keyed by start address: give every word
    // offset its own view of the ring from there to the end.
    std::vector<std::uint32_t> all(ring.begin(), ring.end());
    for (std::uint32_t i = 0; i < dwords; ++i) {
      memory.buffers[base + i * 4] = std::vector<std::uint32_t>(all.begin() + i, all.end());
    }
  }
  void Put(const std::vector<std::uint32_t>& packets) {
    for (std::uint32_t w : packets) {
      ring[write] = w;
      write = (write + 1) % dwords;
    }
    Sync();
  }
  std::vector<std::uint32_t> ring = std::vector<std::uint32_t>(64, 0);
};

std::vector<std::uint32_t> IndirectBuffer(std::uint32_t address, std::uint32_t dwords) {
  return {Type3(kOpIndirectBuffer, 2), address, dwords};
}

std::vector<std::uint32_t> SwapPacket() {
  // VdSwap's 64 words: XE_SWAP with 63 words of body ('SWAP', front buffer, width, height, zeros).
  std::vector<std::uint32_t> s(64, 0);
  s[0] = Type3(kOpXeSwap, 63);
  s[1] = 0x53574150;
  return s;
}

}  // namespace

TEST_CASE("pm4: the ring follower reads up to the write pointer, follows indirect buffers and ends frames at swaps") {
  FakeMemory memory;
  // D3D's command buffer: frame 1 renders two draws, frame 2 one draw and a resolve; each ends with VdSwap's packet.
  std::vector<std::uint32_t> frame1{Type0(kRegModeControl, 1), kEdramColorDepth};
  PushDraw2(frame1);
  PushDraw2(frame1);
  const auto swap = SwapPacket();
  frame1.insert(frame1.end(), swap.begin(), swap.end());
  std::vector<std::uint32_t> frame2;
  PushDraw2(frame2);
  frame2.push_back(Type0(kRegModeControl, 1));
  frame2.push_back(kEdramCopy);
  PushDrawIndx(frame2);
  frame2.insert(frame2.end(), swap.begin(), swap.end());
  memory.buffers[0x100000] = frame1;
  memory.buffers[0x200000] = frame2;

  FakeRing ring(memory, 0x0BFE8000, 64);
  RingFollower follower(memory.reader(), ring.base, ring.dwords);
  std::vector<FrameStats> frames;
  const auto collect = [&](const FrameStats& f) { frames.push_back(f); };

  CHECK_FALSE(follower.Advance(0, collect));  // nothing written yet
  ring.Put(IndirectBuffer(0x100000, static_cast<std::uint32_t>(frame1.size())));
  CHECK(follower.Advance(ring.write, collect));
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].draws == 2);
  CHECK(frames[0].swaps == 1);
  CHECK(frames[0].indirect_buffers == 1);
  CHECK(follower.position() == 3);

  ring.Put(IndirectBuffer(0x200000, static_cast<std::uint32_t>(frame2.size())));
  CHECK(follower.Advance(ring.write, collect));
  REQUIRE(frames.size() == 2);
  CHECK(frames[1].draws == 1);
  CHECK(frames[1].resolves == 1);
  CHECK(follower.frames() == 2);
  CHECK(follower.words_read() == 6);
}

TEST_CASE("pm4: a frame kicked in several stretches continues across samples, and packets wrap at the ring's end") {
  FakeMemory memory;
  std::vector<std::uint32_t> part1{Type0(kRegModeControl, 1), kEdramColorDepth};
  PushDraw2(part1);
  std::vector<std::uint32_t> part2;
  PushDraw2(part2);
  PushDraw2(part2);
  const auto swap = SwapPacket();
  part2.insert(part2.end(), swap.begin(), swap.end());
  memory.buffers[0x300000] = part1;
  memory.buffers[0x300100] = part2;

  FakeRing ring(memory, 0x0BFE8000, 64);
  ring.write = 62;  // the next packet starts two words before the end of the ring and wraps
  RingFollower follower(memory.reader(), ring.base, ring.dwords, 62);
  std::vector<FrameStats> frames;
  const auto collect = [&](const FrameStats& f) { frames.push_back(f); };

  ring.Put(IndirectBuffer(0x300000, static_cast<std::uint32_t>(part1.size())));  // words 62, 63, 0
  CHECK(follower.Advance(ring.write, collect));
  CHECK(frames.empty());                     // no swap yet: the frame is still open
  CHECK(follower.pending().draws == 1);
  CHECK(follower.position() == 1);

  ring.Put(IndirectBuffer(0x300100, static_cast<std::uint32_t>(part2.size())));
  CHECK(follower.Advance(ring.write + ring.dwords, collect));  // taken modulo the ring size
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].draws == 3);                // both stretches in one frame
  CHECK(frames[0].indirect_buffers == 2);
  CHECK(follower.pending().draws == 0);       // a new frame starts after the swap
}

TEST_CASE("pm4: the ring follower counts what it cannot read and keeps the register state between samples") {
  FakeMemory memory;
  RingFollower unreadable(memory.reader(), 0x0BFE8000, 64);
  CHECK(unreadable.Advance(10, nullptr));     // no memory behind the ring: counted, not parsed
  CHECK(unreadable.pending().unreadable == 1);
  CHECK(unreadable.pending().packets == 0);

  FakeRing ring(memory, 0x0BFE8000, 64);
  RingFollower follower(memory.reader(), ring.base, ring.dwords);
  std::vector<FrameStats> frames;
  const auto collect = [&](const FrameStats& f) { frames.push_back(f); };
  ring.Put({Type0(kRegModeControl, 1), kEdramCopy});  // sample 1: copy mode set
  CHECK(follower.Advance(ring.write, collect));
  std::vector<std::uint32_t> draw;
  PushDraw2(draw);
  ring.Put(draw);                                      // sample 2: a draw, in the mode set before
  CHECK(follower.Advance(ring.write, collect));
  ring.Put({Type3(kOpXeSwap, 1), 0x53574150});         // sample 3: the swap ends the frame
  CHECK(follower.Advance(ring.write, collect));
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].resolves == 1);
  CHECK(frames[0].draws == 0);
  CHECK_FALSE(follower.Advance(follower.position(), collect));  // same position: nothing new
}
