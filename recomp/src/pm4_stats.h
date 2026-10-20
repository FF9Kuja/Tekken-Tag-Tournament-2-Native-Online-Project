// What a frame asked the GPU to do, counted from the PM4 command stream the game wrote (SDK-free).
//
// The Xbox 360 GPU (Xenos) is fed PM4 packets: type 0 writes consecutive registers, type 1 two registers,
// type 2 is a one-word no-op, type 3 is a command (opcode in bits 8-14, count of the words that follow minus
// one in bits 16-29; bit 0 predicates it on the bin mask). The GPU reads the primary ring buffer up to the write
// pointer the CPU sets (register CP_RB_WPTR, in words), and the indirect buffers its INDIRECT_BUFFER packets
// name (a GPU address and a length). Direct3D writes a frame's commands into its own command buffer and kicks
// them to the GPU with indirect-buffer packets in the ring; VdSwap fills 64 words D3D reserved there with the
// swap packet (XE_SWAP), which ends the frame in the stream. (TTT2's VdSwap buffers lie outside the 32 KB
// ring, in D3D's command buffer: the owner's boot logs, every frame.) A draw is a
// DRAW_INDX or DRAW_INDX_2 packet, and what it does depends on RB_MODECONTROL's EDRAM mode at that point:
// 4 (color and depth) and 5 (depth only) render, 6 (copy) resolves - copies the EDRAM render target to a
// texture in memory, clearing it when RB_COPY_CONTROL says so - and 0 does nothing.
//
// The parser reads packets the way the SDK's command processor executes them
// (third_party/rexglue-sdk/src/src/graphics/command_processor.cpp: a zero word is skipped, predicated packets
// are skipped while bin select & bin mask is 0, SET_CONSTANT type 4 and SET_CONSTANT2 write registers) and
// keeps the registers it needs from one frame to the next. It never fails: what it cannot read is counted.
// Nothing it records is image data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ttt2::pm4 {

// Registers (dword indices) the parser follows.
inline constexpr std::uint32_t kRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
inline constexpr std::uint32_t kRegColorInfo = 0x2001;    // RB_COLOR_INFO
inline constexpr std::uint32_t kRegDepthInfo = 0x2002;    // RB_DEPTH_INFO
inline constexpr std::uint32_t kRegModeControl = 0x2208;  // RB_MODECONTROL
inline constexpr std::uint32_t kRegCopyControl = 0x2318;  // RB_COPY_CONTROL
inline constexpr std::uint32_t kRegCopyDestBase = 0x2319; // RB_COPY_DEST_BASE

// Type 3 opcodes.
inline constexpr std::uint32_t kOpNop = 0x10;
inline constexpr std::uint32_t kOpRegRmw = 0x21;
inline constexpr std::uint32_t kOpDrawIndx = 0x22;
inline constexpr std::uint32_t kOpVizQuery = 0x23;
inline constexpr std::uint32_t kOpWaitForIdle = 0x26;
inline constexpr std::uint32_t kOpImLoad = 0x27;
inline constexpr std::uint32_t kOpImLoadImmediate = 0x2B;
inline constexpr std::uint32_t kOpSetConstant = 0x2D;
inline constexpr std::uint32_t kOpLoadAluConstant = 0x2F;
inline constexpr std::uint32_t kOpDrawIndxBin = 0x34;
inline constexpr std::uint32_t kOpDrawIndx2Bin = 0x35;
inline constexpr std::uint32_t kOpDrawIndx2 = 0x36;
inline constexpr std::uint32_t kOpIndirectBufferPfd = 0x37;
inline constexpr std::uint32_t kOpInvalidateState = 0x3B;
inline constexpr std::uint32_t kOpWaitRegMem = 0x3C;
inline constexpr std::uint32_t kOpMemWrite = 0x3D;
inline constexpr std::uint32_t kOpRegToMem = 0x3E;
inline constexpr std::uint32_t kOpIndirectBuffer = 0x3F;
inline constexpr std::uint32_t kOpCondWrite = 0x45;
inline constexpr std::uint32_t kOpEventWrite = 0x46;
inline constexpr std::uint32_t kOpMeInit = 0x48;
inline constexpr std::uint32_t kOpSetBinMask = 0x50;
inline constexpr std::uint32_t kOpSetBinSelect = 0x51;
inline constexpr std::uint32_t kOpInterrupt = 0x54;
inline constexpr std::uint32_t kOpSetConstant2 = 0x55;
inline constexpr std::uint32_t kOpSetShaderConstants = 0x56;
inline constexpr std::uint32_t kOpEventWriteShd = 0x58;
inline constexpr std::uint32_t kOpEventWriteExt = 0x5A;
inline constexpr std::uint32_t kOpEventWriteZpd = 0x5B;
inline constexpr std::uint32_t kOpContextUpdate = 0x5E;
inline constexpr std::uint32_t kOpSetBinMaskLo = 0x60;
inline constexpr std::uint32_t kOpSetBinMaskHi = 0x61;
inline constexpr std::uint32_t kOpSetBinSelectLo = 0x62;
inline constexpr std::uint32_t kOpSetBinSelectHi = 0x63;
inline constexpr std::uint32_t kOpXeSwap = 0x64;

// RB_MODECONTROL EDRAM modes.
inline constexpr std::uint32_t kEdramNoOperation = 0;
inline constexpr std::uint32_t kEdramColorDepth = 4;
inline constexpr std::uint32_t kEdramDepthOnly = 5;
inline constexpr std::uint32_t kEdramCopy = 6;

/// True for the type 3 opcodes the SDK's command processor executes; the others it logs as unimplemented and skips.
bool OpcodeHandledBySdk(std::uint32_t opcode);

/// Packet-building helpers (tests and synthetic streams).
inline constexpr std::uint32_t Type0(std::uint32_t index, std::uint32_t count) {
  return (((count - 1) & 0x3FFF) << 16) | (index & 0x7FFF);
}
inline constexpr std::uint32_t Type3(std::uint32_t opcode, std::uint32_t count, bool predicate = false) {
  return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8) | (predicate ? 1u : 0u);
}

/// What one stretch of the command stream (one frame, in practice) contained.
struct FrameStats {
  std::uint32_t dwords = 0;            ///< words read (ring and indirect buffers)
  std::uint32_t packets = 0;
  std::uint32_t draws = 0;             ///< draw packets that render (EDRAM mode color+depth or depth only)
  std::uint32_t draws_skipped = 0;     ///< draw packets with EDRAM mode "no operation" (or another non-rendering mode)
  std::uint32_t resolves = 0;          ///< draw packets in copy mode: the render target copied to memory
  std::uint32_t clears = 0;            ///< resolves that also clear color and/or depth
  std::uint32_t binned_draws = 0;      ///< DRAW_INDX_BIN / DRAW_INDX_2_BIN (the SDK does not execute them)
  std::uint32_t predicated_skips = 0;  ///< predicated packets skipped (bin select & mask = 0), as the SDK skips them
  std::uint32_t render_target_changes = 0;  ///< writes that changed RB_SURFACE_INFO, RB_COLOR_INFO or RB_DEPTH_INFO
  std::uint32_t indirect_buffers = 0;  ///< indirect buffers followed
  std::uint32_t swaps = 0;             ///< XE_SWAP packets
  std::uint32_t unhandled = 0;         ///< type 3 packets the SDK does not execute (opcodes in unhandled_opcodes)
  std::uint32_t unreadable = 0;        ///< indirect buffers that could not be read (not in allocated memory)
  std::uint32_t malformed = 0;         ///< packets longer than what was left of their buffer
  bool truncated = false;              ///< stopped at the word limit or the indirect-buffer depth
  std::uint32_t last_resolve_dest = 0; ///< RB_COPY_DEST_BASE of the last resolve (a GPU address)
  std::vector<std::uint32_t> unhandled_opcodes;  ///< distinct, in order of appearance (at most 8)

  void Add(const FrameStats& other);
};

/// Reads `dwords` words at GPU (physical) address `address` into `out` (host order, already byte-swapped);
/// false when any of them is not readable.
using ReadFn = std::function<bool(std::uint32_t address, std::uint32_t dwords, std::vector<std::uint32_t>& out)>;

class Parser {
 public:
  /// Indirect buffers are read through `read`; at most `max_dwords` words are parsed per Parse call
  /// (indirect buffers included) and indirect buffers are followed `max_depth` levels deep.
  explicit Parser(ReadFn read, std::uint32_t max_dwords = 4u << 20, int max_depth = 2);

  /// Parses `count` words (host order) as a command buffer, adding to `stats`.
  void Parse(const std::uint32_t* words, std::size_t count, FrameStats& stats);

  /// Called at every XE_SWAP packet (after counting it) with the stats being added to; the handler may take
  /// them as a finished frame and reset them, and the parse goes on adding to the same object. Empty: none.
  using SwapFn = std::function<void(FrameStats& stats)>;
  void SetSwapHandler(SwapFn handler) { on_swap_ = std::move(handler); }

  // The register state carried from one Parse to the next (as the GPU keeps it).
  std::uint32_t mode_control() const { return mode_control_; }
  std::uint32_t copy_control() const { return copy_control_; }

 private:
  void ParseBuffer(const std::uint32_t* words, std::size_t count, FrameStats& stats, int depth, std::uint32_t& budget);
  void WriteRegister(std::uint32_t index, std::uint32_t value, FrameStats& stats);
  void Draw(std::uint32_t opcode, FrameStats& stats);

  ReadFn read_;
  SwapFn on_swap_;
  std::uint32_t max_dwords_;
  int max_depth_;
  std::uint32_t mode_control_ = 0;
  std::uint32_t copy_control_ = 0;
  std::uint32_t copy_dest_base_ = 0;
  std::uint32_t surface_info_ = 0;
  std::uint32_t color_info_ = 0;
  std::uint32_t depth_info_ = 0;
  std::uint64_t bin_select_ = 0xFFFFFFFFull;
  std::uint64_t bin_mask_ = 0xFFFFFFFFull;
};

/// One or two byte ranges of a ring buffer of `ring_bytes` bytes between offsets `from` (inclusive) and
/// `to` (exclusive), wrapping at the end: what was written from one position to the next.
struct RingSpan {
  std::uint32_t offset = 0;
  std::uint32_t bytes = 0;
};
std::vector<RingSpan> RingSpans(std::uint32_t ring_bytes, std::uint32_t from, std::uint32_t to);

/// Follows a ring buffer the way the GPU reads it. At each sample of the ring's write pointer (CP_RB_WPTR, a
/// word index) the words written since the previous sample are parsed, with the indirect buffers they name, and
/// split into frames at the XE_SWAP packets. D3D kicks a frame's commands in one or more stretches (a command
/// buffer segment when it fills, the rest at Present), so what follows the last swap is kept and continues into
/// the next sample's frame. The CPU writes whole packets before it moves the write pointer, so a sample never
/// ends inside a packet; packets may wrap around the ring's end. More than a whole ring written between two
/// samples cannot be told from less: the caller compares the swaps found with the frames presented.
class RingFollower {
 public:
  using FrameFn = std::function<void(const FrameStats& frame)>;
  /// The ring: GPU (physical) base address and size in words; `start` is where the GPU starts reading (0 after
  /// VdInitializeRingBuffer).
  RingFollower(ReadFn read, std::uint32_t base, std::uint32_t dwords, std::uint32_t start = 0);

  /// Parses the words from the previous position up to `write_pointer` (taken modulo the ring size) and calls
  /// `on_frame` for every frame a swap packet completes. False when nothing new was written.
  bool Advance(std::uint32_t write_pointer, const FrameFn& on_frame);

  std::uint32_t position() const { return position_; }
  /// The frame in progress: what was read after the last swap packet.
  const FrameStats& pending() const { return pending_; }
  std::uint64_t words_read() const { return words_read_; }
  std::uint64_t frames() const { return frames_; }

 private:
  ReadFn read_;
  Parser parser_;
  std::uint32_t base_;
  std::uint32_t dwords_;
  std::uint32_t position_;
  FrameStats pending_;
  std::vector<std::uint32_t> words_;
  std::vector<std::uint32_t> part_;
  std::uint64_t words_read_ = 0;
  std::uint64_t frames_ = 0;
};

/// "3 draws, 1 resolve (1 clearing), 2 indirect buffers, 120 packets" - the parts that are not zero.
std::string Describe(const FrameStats& stats);

}  // namespace ttt2::pm4
