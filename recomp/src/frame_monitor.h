// Per-frame GPU diagnostics: at every VdSwap, what the game's command stream for that frame asked the GPU to
// do (draws, resolves, clears, render target changes, indirect buffers, packets the GPU emulation skips:
// pm4_stats.h), and what it presents (the front buffer's address, size and format).
//
// The owner's boots present frames (VdSwap 60 times a second) and the window stays black. These counts tell a
// frame that asks for nothing to be drawn (the game shows a black screen of its own, for instance while it
// waits for data) from frames full of draws that come out black (a GPU emulation problem). The stream is read
// on the game's own thread right after VdSwap, from guest physical memory (only allocated pages are read), the
// way the GPU reads it: with the GPU emulation loaded, the primary ring buffer up to the write pointer the game
// last gave the GPU (register CP_RB_WPTR) and the indirect buffers it names - Direct3D's own command buffer,
// where TTT2's VdSwap buffers lie (outside the 32 KB ring: the owner's boot logs) - split into frames at the
// swap packets VdSwap writes. Without a GPU emulation (the headless harness) only a ring that holds VdSwap's
// buffer itself can be read, between one swap and the next. A frame read by neither is counted as not
// followed: its draws are unknown, not zero. TTT2_FRAME_STATS=0 turns it off.
//
// The pixel check (TTT2_PIXEL_CHECK=1) also samples the front buffer in guest memory: how many distinct values
// and which value is most common, nothing else. The GPU emulation renders on the host GPU and only copies
// results back to guest memory with --readback_resolve (fast, some or full; the check turns on "fast" when it is
// off), so without readback the guest copy is not the image shown.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ttt2::frames {

/// VdInitializeRingBuffer(physical address, log2 of the size in 8-byte units): the primary ring buffer.
void NoteRingBuffer(std::uint32_t physical, std::uint32_t size_log2);

/// Where the ring's write pointer (CP_RB_WPTR, in words) can be read: set by the host when the GPU emulation's
/// registers are readable; std::nullopt from the function while they are not.
void SetWritePointerSource(std::function<std::optional<std::uint32_t>()> source);

/// VdSwap's arguments, read before the runtime's VdSwap ran (guest addresses and values).
struct Swap {
  std::uint32_t buffer = 0;       ///< virtual address of the 64 words VdSwap fills (in the ring or D3D's command buffer)
  std::uint32_t front_buffer = 0; ///< virtual address of the front buffer (0: unknown)
  std::uint32_t format = 0;       ///< texture format (6: k_8_8_8_8, 54: k_2_10_10_10_AS_16_16_16_16)
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t lr = 0;           ///< guest return address
};

/// Called after the runtime's VdSwap: counts the frame's commands (and samples the front buffer when the pixel
/// check is on).
void NoteSwap(const Swap& swap);

/// After the runtime is set up: turns the pixel check on when TTT2_PIXEL_CHECK=1 (with readback of resolves,
/// when it is off) and logs what it did.
void ConfigurePixelCheck();

/// For the watchdog's activity line: "frames parsed 600 (0 with draws, 0 with resolves; 0 draws, ...)".
std::string Describe();

}  // namespace ttt2::frames
