#include "frame_monitor.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "boot_monitor.h"
#include "pm4_stats.h"

namespace ttt2::frames {
namespace {

constexpr std::uint32_t kSwapBlockBytes = 64 * 4;  // the words VdSwap fills in the ring
constexpr std::uint64_t kLoggedFrames = 3;          // frames described one by one at the start

bool Enabled() {
  static const bool on = [] {
    const char* value = std::getenv("TTT2_FRAME_STATS");
    return !(value && std::string_view(value) == "0");
  }();
  return on;
}

rex::memory::Memory* GuestMemory() {
  return rex::system::kernel_memory();
}

/// `dwords` words of guest physical memory at `address`, byte-swapped; false unless every page is allocated.
bool ReadPhysical(std::uint32_t address, std::uint32_t dwords, std::vector<std::uint32_t>& out) {
  auto* memory = GuestMemory();
  if (!memory || !dwords) return false;
  address &= 0x1FFFFFFF;
  const std::uint64_t end = std::uint64_t(address) + std::uint64_t(dwords) * 4;
  if (end > 0x20000000) return false;
  auto* heap = memory->GetPhysicalHeap();
  if (!heap || heap->QueryRangeAccess(address, static_cast<std::uint32_t>(end - 1)) ==
                   rex::memory::PageAccess::kNoAccess) {
    return false;
  }
  const auto* p = memory->TranslatePhysical<const std::uint8_t*>(address);
  out.resize(dwords);
  for (std::uint32_t i = 0; i < dwords; ++i) {
    const std::uint8_t* w = p + 4 * std::size_t(i);
    out[i] = (std::uint32_t(w[0]) << 24) | (std::uint32_t(w[1]) << 16) | (std::uint32_t(w[2]) << 8) | w[3];
  }
  return true;
}

/// True when [address, address + bytes) is allocated guest virtual memory.
bool VirtualReadable(std::uint32_t address, std::uint64_t bytes) {
  auto* memory = GuestMemory();
  if (!memory || !bytes || std::uint64_t(address) + bytes > 0x100000000ull) return false;
  const auto last = static_cast<std::uint32_t>(address + bytes - 1);
  auto* heap = memory->LookupHeap(address);
  if (!heap || memory->LookupHeap(last) != heap) return false;
  return heap->QueryRangeAccess(address, last) != rex::memory::PageAccess::kNoAccess;
}

std::string FormatName(std::uint32_t format) {
  switch (format) {
    case 6: return "k_8_8_8_8";
    case 54: return "k_2_10_10_10_AS_16_16_16_16";
    default: return fmt::format("format {}", format);
  }
}

struct State {
  std::mutex mutex;
  // The primary ring buffer (physical) and where the next frame's commands start in it.
  bool ring_known = false;
  std::uint32_t ring_base = 0;
  std::uint32_t ring_bytes = 0;
  std::uint32_t next_offset = 0;
  std::unique_ptr<pm4::Parser> parser;
  std::vector<std::uint32_t> words;
  std::vector<std::uint32_t> part;
  // Totals.
  std::uint64_t frames = 0;
  std::uint64_t frames_with_draws = 0;
  std::uint64_t frames_with_resolves = 0;
  std::uint64_t draws = 0;
  std::uint64_t resolves = 0;
  std::uint64_t clears = 0;
  std::uint64_t unhandled = 0;
  std::uint64_t outside_ring = 0;
  std::uint32_t max_draws = 0;
  pm4::FrameStats last;
  bool have_last = false;
  // The front buffer as last presented.
  std::uint32_t front_buffer = 0;
  std::uint32_t front_width = 0;
  std::uint32_t front_height = 0;
  std::uint32_t front_format = 0;
  // One-time log lines.
  bool logged_first_draws = false;
  bool logged_unhandled = false;
  bool logged_outside = false;
  bool logged_no_ring = false;
  // The pixel check.
  bool pixel_check = false;
  bool readback = false;
};

State& state() {
  static State s;
  return s;
}

bool PixelCheckFrame(std::uint64_t frame) {
  return frame <= 3 || frame == 10 || frame == 30 || frame == 60 || frame == 120 || frame == 300 || frame % 600 == 0;
}

/// Samples the front buffer: distinct values and the most common one (no image data is kept).
void SamplePixels(State& s, const Swap& swap, std::uint64_t frame) {
  if (!swap.front_buffer || !swap.width || !swap.height || swap.width > 8192 || swap.height > 8192) return;
  const std::uint64_t pixels = std::uint64_t(swap.width) * swap.height;
  if (!VirtualReadable(swap.front_buffer, pixels * 4)) {
    REXLOG_INFO("ttt2: gpu: pixel check frame {}: front buffer at 0x{:08X} ({}x{}) is not readable guest memory", frame,
                swap.front_buffer, swap.width, swap.height);
    return;
  }
  const auto* p = GuestMemory()->TranslateVirtual<const std::uint8_t*>(swap.front_buffer);
  const std::uint32_t samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(pixels, 4096));
  std::vector<std::pair<std::uint32_t, std::uint32_t>> values;  // value, count (the first 64 distinct)
  bool more = false;
  std::uint32_t zero = 0;
  for (std::uint32_t i = 0; i < samples; ++i) {
    const std::uint8_t* w = p + 4 * (i * pixels / samples);
    const std::uint32_t v = (std::uint32_t(w[0]) << 24) | (std::uint32_t(w[1]) << 16) | (std::uint32_t(w[2]) << 8) | w[3];
    if (!v) ++zero;
    auto it = std::find_if(values.begin(), values.end(), [v](const auto& e) { return e.first == v; });
    if (it != values.end()) {
      ++it->second;
    } else if (values.size() < 64) {
      values.emplace_back(v, 1);
    } else {
      more = true;
    }
  }
  const auto top = std::max_element(values.begin(), values.end(),
                                    [](const auto& a, const auto& b) { return a.second < b.second; });
  REXLOG_INFO(
      "ttt2: gpu: pixel check frame {}: front buffer {}x{} {} at 0x{:08X}: {} samples, {}{} distinct value{}, most "
      "common 0x{:08X} ({:.0f}%), {:.0f}% zero{}",
      frame, swap.width, swap.height, FormatName(swap.format), swap.front_buffer, samples, more ? "more than " : "",
      values.size(), values.size() == 1 && !more ? "" : "s", top != values.end() ? top->first : 0u,
      top != values.end() ? 100.0 * top->second / samples : 0.0, 100.0 * zero / samples,
      s.readback ? "" : " (resolves are not read back: the guest copy is not what the window shows)");
}

}  // namespace

void NoteRingBuffer(std::uint32_t physical, std::uint32_t size_log2) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (size_log2 > 26) return;  // more than 512 MB: not a ring the GPU could have
  s.ring_base = physical & 0x1FFFFFFF;
  s.ring_bytes = 1u << (size_log2 + 3);
  s.next_offset = 0;  // the command processor starts at the beginning
  s.ring_known = true;
}

void NoteSwap(const Swap& swap) {
  if (!Enabled()) return;
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const std::uint64_t frame = ++s.frames;

  if (swap.width != s.front_width || swap.height != s.front_height || swap.format != s.front_format ||
      swap.front_buffer != s.front_buffer) {
    REXLOG_INFO("ttt2: gpu: front buffer {}x{} {} at 0x{:08X} (frame {})", swap.width, swap.height,
                FormatName(swap.format), swap.front_buffer, frame);
    s.front_width = swap.width;
    s.front_height = swap.height;
    s.front_format = swap.format;
    s.front_buffer = swap.front_buffer;
  }
  if (s.pixel_check && PixelCheckFrame(frame)) SamplePixels(s, swap, frame);

  auto* memory = GuestMemory();
  if (!s.ring_known || !memory) {
    if (!s.logged_no_ring) {
      s.logged_no_ring = true;
      REXLOG_INFO("ttt2: gpu: frame {} presented before any ring buffer was set up: its commands are not counted", frame);
    }
    return;
  }
  const std::uint32_t physical = memory->GetPhysicalAddress(swap.buffer);
  if (physical == std::numeric_limits<std::uint32_t>::max() || (physical & 0x1FFFFFFF) < s.ring_base ||
      (physical & 0x1FFFFFFF) - s.ring_base >= s.ring_bytes) {
    ++s.outside_ring;
    if (!s.logged_outside) {
      s.logged_outside = true;
      REXLOG_WARN(
          "ttt2: gpu: frame {}: VdSwap's buffer 0x{:08X} (physical 0x{:08X}) is not in the ring buffer at 0x{:08X} "
          "({} bytes): the frame's commands are not counted",
          frame, swap.buffer, physical, s.ring_base, s.ring_bytes);
    }
    return;
  }
  const std::uint32_t offset = (physical & 0x1FFFFFFF) - s.ring_base;

  if (!s.parser) s.parser = std::make_unique<pm4::Parser>(&ReadPhysical);
  pm4::FrameStats stats;
  s.words.clear();
  for (const auto& span : pm4::RingSpans(s.ring_bytes, s.next_offset, offset)) {
    if (!ReadPhysical(s.ring_base + span.offset, span.bytes / 4, s.part)) {
      ++stats.unreadable;
      continue;
    }
    s.words.insert(s.words.end(), s.part.begin(), s.part.end());
  }
  s.parser->Parse(s.words.data(), s.words.size(), stats);
  s.next_offset = (offset + kSwapBlockBytes) % s.ring_bytes;

  if (stats.draws) ++s.frames_with_draws;
  if (stats.resolves) ++s.frames_with_resolves;
  s.draws += stats.draws;
  s.resolves += stats.resolves;
  s.clears += stats.clears;
  s.unhandled += stats.unhandled;
  s.max_draws = std::max(s.max_draws, stats.draws);
  s.last = stats;
  s.have_last = true;

  if (frame <= kLoggedFrames) {
    REXLOG_INFO("ttt2: gpu: frame {}: {} ({} words of commands)", frame, pm4::Describe(stats), stats.dwords);
  }
  if (!s.logged_first_draws && (stats.draws || stats.resolves)) {
    s.logged_first_draws = true;
    REXLOG_INFO("ttt2: gpu: first frame with draws: frame {}: {} ({}, from {})", frame, pm4::Describe(stats),
                monitor::CurrentThreadLabel(), monitor::DescribeGuestAddress(swap.lr));
  }
  if (!s.logged_unhandled && stats.unhandled) {
    s.logged_unhandled = true;
    REXLOG_WARN(
        "ttt2: gpu: frame {} holds packets the GPU emulation does not execute (it logs them as \"Unimplemented GPU "
        "OPCODE\" and skips them): {}",
        frame, pm4::Describe(stats));
  }
}

void ConfigurePixelCheck() {
  auto& s = state();
  const char* value = std::getenv("TTT2_PIXEL_CHECK");
  if (!value || std::string_view(value) != "1") return;
  std::string readback = rex::cvar::GetFlagByName("readback_resolve");
  if (readback.empty()) {
    REXLOG_WARN(
        "ttt2: gpu: pixel check requested (TTT2_PIXEL_CHECK=1) but the GPU emulation is not loaded (no readback_resolve "
        "option): nothing renders into the front buffer, the check reads what guest memory holds");
  } else if (readback == "none" && rex::cvar::SetFlagByName("readback_resolve", "fast")) {
    readback = "fast";
    REXLOG_INFO("ttt2: gpu: pixel check: readback_resolve set to fast so the frames reach guest memory (one frame late)");
  }
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pixel_check = true;
  s.readback = !readback.empty() && readback != "none";
  REXLOG_INFO("ttt2: gpu: pixel check on: the front buffer is sampled at frames 1-3, 10, 30, 60, 120, 300 and every 600th "
              "(readback_resolve {})",
              readback.empty() ? "unavailable" : readback);
}

std::string Describe() {
  if (!Enabled()) return "frame stats off (TTT2_FRAME_STATS=0)";
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.frames) return {};
  std::string out = fmt::format("{} with draws, {} with resolves; draws {}, resolves {}, clears {}",
                                s.frames_with_draws, s.frames_with_resolves, s.draws, s.resolves, s.clears);
  if (s.max_draws) out += fmt::format(", at most {} draws in a frame", s.max_draws);
  if (s.unhandled) out += fmt::format(", {} packet{} the GPU emulation skips", s.unhandled, s.unhandled == 1 ? "" : "s");
  if (s.outside_ring) out += fmt::format(", {} swap{} outside the ring", s.outside_ring, s.outside_ring == 1 ? "" : "s");
  if (s.have_last) out += "; last frame: " + pm4::Describe(s.last);
  if (s.front_width) out += fmt::format("; front buffer {}x{}", s.front_width, s.front_height);
  return out;
}

}  // namespace ttt2::frames
