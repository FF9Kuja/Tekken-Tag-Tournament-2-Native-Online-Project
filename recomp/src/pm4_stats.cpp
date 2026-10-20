#include "pm4_stats.h"

#include <algorithm>
#include <utility>

namespace ttt2::pm4 {

bool OpcodeHandledBySdk(std::uint32_t opcode) {
  // The cases of CommandProcessor::ExecutePacketType3 in SDK 0.10.0.
  switch (opcode) {
    case kOpMeInit:
    case kOpNop:
    case kOpInterrupt:
    case kOpXeSwap:
    case kOpIndirectBuffer:
    case kOpIndirectBufferPfd:
    case kOpWaitRegMem:
    case kOpRegRmw:
    case kOpRegToMem:
    case kOpMemWrite:
    case kOpCondWrite:
    case kOpEventWrite:
    case kOpEventWriteShd:
    case kOpEventWriteExt:
    case kOpEventWriteZpd:
    case kOpDrawIndx:
    case kOpDrawIndx2:
    case kOpSetConstant:
    case kOpSetConstant2:
    case kOpLoadAluConstant:
    case kOpSetShaderConstants:
    case kOpImLoad:
    case kOpImLoadImmediate:
    case kOpInvalidateState:
    case kOpVizQuery:
    case kOpSetBinMaskLo:
    case kOpSetBinMaskHi:
    case kOpSetBinSelectLo:
    case kOpSetBinSelectHi:
    case kOpSetBinMask:
    case kOpSetBinSelect:
    case kOpContextUpdate:
    case kOpWaitForIdle:
      return true;
    default:
      return false;
  }
}

void FrameStats::Add(const FrameStats& o) {
  dwords += o.dwords;
  packets += o.packets;
  draws += o.draws;
  draws_skipped += o.draws_skipped;
  resolves += o.resolves;
  clears += o.clears;
  binned_draws += o.binned_draws;
  predicated_skips += o.predicated_skips;
  render_target_changes += o.render_target_changes;
  indirect_buffers += o.indirect_buffers;
  swaps += o.swaps;
  unhandled += o.unhandled;
  unreadable += o.unreadable;
  malformed += o.malformed;
  truncated = truncated || o.truncated;
  if (o.resolves) last_resolve_dest = o.last_resolve_dest;
  for (std::uint32_t op : o.unhandled_opcodes) {
    if (unhandled_opcodes.size() < 8 &&
        std::find(unhandled_opcodes.begin(), unhandled_opcodes.end(), op) == unhandled_opcodes.end()) {
      unhandled_opcodes.push_back(op);
    }
  }
}

Parser::Parser(ReadFn read, std::uint32_t max_dwords, int max_depth)
    : read_(std::move(read)), max_dwords_(max_dwords), max_depth_(max_depth) {}

void Parser::Parse(const std::uint32_t* words, std::size_t count, FrameStats& stats) {
  std::uint32_t budget = max_dwords_;
  ParseBuffer(words, count, stats, 0, budget);
}

void Parser::WriteRegister(std::uint32_t index, std::uint32_t value, FrameStats& stats) {
  const auto target_change = [&](std::uint32_t& reg) {
    if (reg != value) {
      reg = value;
      ++stats.render_target_changes;
    }
  };
  switch (index) {
    case kRegModeControl: mode_control_ = value; break;
    case kRegCopyControl: copy_control_ = value; break;
    case kRegCopyDestBase: copy_dest_base_ = value; break;
    case kRegSurfaceInfo: target_change(surface_info_); break;
    case kRegColorInfo: target_change(color_info_); break;
    case kRegDepthInfo: target_change(depth_info_); break;
    default: break;
  }
}

void Parser::Draw(std::uint32_t /*opcode*/, FrameStats& stats) {
  switch (mode_control_ & 7) {
    case kEdramCopy:
      ++stats.resolves;
      if (copy_control_ & 0x300) ++stats.clears;  // color_clear_enable (bit 8), depth_clear_enable (bit 9)
      stats.last_resolve_dest = copy_dest_base_;
      break;
    case kEdramColorDepth:
    case kEdramDepthOnly:
      ++stats.draws;
      break;
    default:
      ++stats.draws_skipped;
      break;
  }
}

void Parser::ParseBuffer(const std::uint32_t* words, std::size_t count, FrameStats& stats, int depth,
                         std::uint32_t& budget) {
  std::size_t i = 0;
  while (i < count) {
    if (budget == 0) {
      stats.truncated = true;
      return;
    }
    const std::uint32_t packet = words[i++];
    --budget;
    ++stats.dwords;
    if (packet == 0) continue;  // the SDK skips a zero word
    ++stats.packets;
    const std::size_t left = count - i;
    switch (packet >> 30) {
      case 0: {
        const std::uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        if (left < n || budget < n) {
          if (left < n) ++stats.malformed; else stats.truncated = true;
          return;
        }
        const std::uint32_t base = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 1;
        for (std::uint32_t m = 0; m < n; ++m) WriteRegister(one_register ? base : base + m, words[i + m], stats);
        i += n;
        budget -= n;
        stats.dwords += n;
        break;
      }
      case 1: {
        if (left < 2 || budget < 2) {
          if (left < 2) ++stats.malformed; else stats.truncated = true;
          return;
        }
        WriteRegister(packet & 0x7FF, words[i], stats);
        WriteRegister((packet >> 11) & 0x7FF, words[i + 1], stats);
        i += 2;
        budget -= 2;
        stats.dwords += 2;
        break;
      }
      case 2:
        break;
      default: {
        const std::uint32_t opcode = (packet >> 8) & 0x7F;
        const std::uint32_t n = ((packet >> 16) & 0x3FFF) + 1;
        if (left < n || budget < n) {
          if (left < n) ++stats.malformed; else stats.truncated = true;
          return;
        }
        const std::uint32_t* body = words + i;
        i += n;
        budget -= n;
        stats.dwords += n;
        if ((packet & 1) && ((bin_select_ & bin_mask_) == 0 || opcode == kOpXeSwap)) {
          ++stats.predicated_skips;
          break;
        }
        switch (opcode) {
          case kOpIndirectBuffer:
          case kOpIndirectBufferPfd: {
            if (n < 2) {
              ++stats.malformed;
              break;
            }
            const std::uint32_t address = body[0];
            const std::uint32_t length = body[1] & 0xFFFFF;
            if (!length) break;
            if (depth >= max_depth_ || length > budget) {
              stats.truncated = true;
              break;
            }
            std::vector<std::uint32_t> buffer;
            if (!read_ || !read_(address, length, buffer) || buffer.size() != length) {
              ++stats.unreadable;
              break;
            }
            ++stats.indirect_buffers;
            ParseBuffer(buffer.data(), buffer.size(), stats, depth + 1, budget);
            break;
          }
          case kOpDrawIndx:
          case kOpDrawIndx2:
            Draw(opcode, stats);
            break;
          case kOpDrawIndxBin:
          case kOpDrawIndx2Bin:
            ++stats.binned_draws;
            ++stats.unhandled;
            if (stats.unhandled_opcodes.size() < 8 &&
                std::find(stats.unhandled_opcodes.begin(), stats.unhandled_opcodes.end(), opcode) ==
                    stats.unhandled_opcodes.end()) {
              stats.unhandled_opcodes.push_back(opcode);
            }
            break;
          case kOpSetConstant: {
            // offset_type: register index in bits 0-10, type in bits 16-23 (4: registers from 0x2000).
            const std::uint32_t index = body[0] & 0x7FF;
            if (((body[0] >> 16) & 0xFF) == 4) {
              for (std::uint32_t k = 1; k < n; ++k) WriteRegister(0x2000 + index + (k - 1), body[k], stats);
            }
            break;
          }
          case kOpSetConstant2:
          case kOpSetShaderConstants: {
            const std::uint32_t index = body[0] & 0xFFFF;
            for (std::uint32_t k = 1; k < n; ++k) WriteRegister(index + (k - 1), body[k], stats);
            break;
          }
          case kOpLoadAluConstant: {
            // address, offset_type, size: registers (type 4) loaded from memory.
            if (n < 3 || ((body[1] >> 16) & 0xFF) != 4) break;
            const std::uint32_t size = body[2] & 0xFFF;
            std::vector<std::uint32_t> values;
            if (size && read_ && read_(body[0] & 0x3FFFFFFF, size, values) && values.size() == size) {
              for (std::uint32_t k = 0; k < size; ++k) WriteRegister(0x2000 + (body[1] & 0x7FF) + k, values[k], stats);
            } else if (size) {
              ++stats.unreadable;
            }
            break;
          }
          case kOpXeSwap:
            ++stats.swaps;
            break;
          case kOpSetBinMaskLo: bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | body[0]; break;
          case kOpSetBinMaskHi: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (std::uint64_t(body[0]) << 32); break;
          case kOpSetBinSelectLo: bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | body[0]; break;
          case kOpSetBinSelectHi: bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (std::uint64_t(body[0]) << 32); break;
          case kOpSetBinMask:
            if (n >= 2) bin_mask_ = (std::uint64_t(body[0]) << 32) | body[1];
            break;
          case kOpSetBinSelect:
            if (n >= 2) bin_select_ = (std::uint64_t(body[0]) << 32) | body[1];
            break;
          default:
            if (!OpcodeHandledBySdk(opcode)) {
              ++stats.unhandled;
              if (stats.unhandled_opcodes.size() < 8 &&
                  std::find(stats.unhandled_opcodes.begin(), stats.unhandled_opcodes.end(), opcode) ==
                      stats.unhandled_opcodes.end()) {
                stats.unhandled_opcodes.push_back(opcode);
              }
            }
            break;
        }
        break;
      }
    }
  }
}

std::vector<RingSpan> RingSpans(std::uint32_t ring_bytes, std::uint32_t from, std::uint32_t to) {
  std::vector<RingSpan> spans;
  if (!ring_bytes || from >= ring_bytes || to > ring_bytes || from == to) return spans;
  if (from < to) {
    spans.push_back({from, to - from});
  } else {
    spans.push_back({from, ring_bytes - from});
    if (to) spans.push_back({0, to});
  }
  return spans;
}

std::string Describe(const FrameStats& s) {
  std::string out;
  const auto part = [&out](std::uint32_t n, const char* one, const char* many) {
    if (!n) return;
    if (!out.empty()) out += ", ";
    out += std::to_string(n) + " " + (n == 1 ? one : many);
  };
  part(s.draws, "draw", "draws");
  if (s.resolves) {
    if (!out.empty()) out += ", ";
    out += std::to_string(s.resolves) + (s.resolves == 1 ? " resolve" : " resolves");
    if (s.clears) out += " (" + std::to_string(s.clears) + " clearing)";
  }
  part(s.draws_skipped, "draw with EDRAM mode off", "draws with EDRAM mode off");
  part(s.binned_draws, "binned draw", "binned draws");
  part(s.render_target_changes, "render target change", "render target changes");
  part(s.indirect_buffers, "indirect buffer", "indirect buffers");
  if (s.unhandled) {
    if (!out.empty()) out += ", ";
    out += std::to_string(s.unhandled) + " packet" + (s.unhandled == 1 ? "" : "s") + " the GPU emulation skips (opcode";
    out += s.unhandled_opcodes.size() == 1 ? " " : "s ";
    for (std::size_t k = 0; k < s.unhandled_opcodes.size(); ++k) {
      char hex[8];
      static constexpr char kDigits[] = "0123456789ABCDEF";
      hex[0] = '0';
      hex[1] = 'x';
      hex[2] = kDigits[(s.unhandled_opcodes[k] >> 4) & 0xF];
      hex[3] = kDigits[s.unhandled_opcodes[k] & 0xF];
      hex[4] = '\0';
      out += (k ? ", " : "") + std::string(hex);
    }
    out += ")";
  }
  part(s.unreadable, "unreadable buffer", "unreadable buffers");
  part(s.malformed, "cut-off packet", "cut-off packets");
  if (s.truncated) out += std::string(out.empty() ? "" : ", ") + "parse limit reached";
  if (!out.empty()) out += ", ";
  out += std::to_string(s.packets) + (s.packets == 1 ? " packet" : " packets");
  return out;
}

}  // namespace ttt2::pm4
