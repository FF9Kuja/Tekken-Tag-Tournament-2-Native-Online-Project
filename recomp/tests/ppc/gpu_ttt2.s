# License-free PowerPC test input: three frames of GPU commands handed to VdSwap, for the host's frame
# monitor (recomp/src/frame_monitor.cpp, pm4_stats.cpp).
#
# The program allocates physical memory for a 4 KB primary ring buffer and for a block holding an indirect
# buffer, VdSwap's arguments and a 16x16 front buffer, registers the ring (VdInitializeRingBuffer), then writes
# three frames of PM4 packets into it the way Direct3D does - commands, then 64 words for VdSwap to fill - and
# presents each:
#   frame 1: RB_MODECONTROL = color+depth, RB_SURFACE_INFO/RB_COLOR_INFO, a DRAW_INDX_2, an INDIRECT_BUFFER
#            holding two more DRAW_INDX_2, RB_MODECONTROL = copy with RB_COPY_CONTROL = color clear and a
#            DRAW_INDX (a resolve that clears), and a SET_STATE packet the GPU emulation does not execute;
#            the host must count "3 draws, 1 resolve (1 clearing), 1 render target change, 1 indirect buffer,
#            1 packet the GPU emulation skips (opcode 0x25), 10 packets (26 words of commands)"
#   frame 2: RB_MODECONTROL = no operation, a DRAW_INDX_2, and an INDIRECT_BUFFER naming a GPU address with no
#            memory behind it, which the host must count rather than read: "1 draw with EDRAM mode off, 1
#            unreadable buffer, 3 packets (7 words of commands)"
#   frame 3: a NOP up to the end of the ring, RB_MODECONTROL = color+depth and two DRAW_INDX_2 of which the
#            second wraps to the start of the ring: "2 draws, 4 packets (869 words of commands)"
# 192 of the front buffer's 256 pixels hold 0x11223344, for the pixel check (TTT2_PIXEL_CHECK=1).
#
#   expected results: 1 1 (the ring's and the block's physical addresses are not 0)
# No GPU emulation is needed (the headless harness has none): VdSwap only fills its 64 words. Kernel ordinals
# from the public xboxkrnl.exe export table: MmAllocatePhysicalMemoryEx 186, MmGetPhysicalAddress 190,
# VdInitializeRingBuffer 451, VdSwap 603. Our own code, no game bytes.
	.machine "power7"

	.section .rodata
	.balign 4
# Frame 1 (22 words). Type 0 header: (count - 1) << 16 | register; type 3: 0xC0000000 | (count - 1) << 16 | opcode << 8.
frame1:
	.long 0x00002208, 4			# RB_MODECONTROL = color + depth
	.long 0x00012000, 16, 0			# RB_SURFACE_INFO = 16, RB_COLOR_INFO = 0
	.long 0xC0003600, 0x00030004		# DRAW_INDX_2: 3 auto indices, triangle list
frame1_ib:
	.long 0xC0013F00, 0, 4			# INDIRECT_BUFFER: GPU address (patched), 4 words
	.long 0x00002208, 6			# RB_MODECONTROL = copy
	.long 0x00012318, 0x100			# RB_COPY_CONTROL = color clear, RB_COPY_DEST_BASE =
frame1_dest:
	.long 0					#   the front buffer's GPU address (patched)
	.long 0xC0032200, 0, 0x00060004, 0, 0	# DRAW_INDX: in copy mode, a resolve
	.long 0xC0002500, 0			# SET_STATE: not executed by the GPU emulation
frame1_end:
	.if (frame1_end - frame1) != 88
	.error "frame 1 must be 22 words (the ring offsets below depend on it)"
	.endif
# The indirect buffer (4 words): two more draws.
ib:
	.long 0xC0003600, 0x00030004, 0xC0003600, 0x00030004
# Frame 2 (7 words).
frame2:
	.long 0x00002208, 0			# RB_MODECONTROL = no operation
	.long 0xC0003600, 0x00030004		# DRAW_INDX_2: does nothing in this mode
	.long 0xC0013F00, 0x0BAD0000, 4		# INDIRECT_BUFFER: no memory is allocated at that GPU address
# Frame 3's last 6 words, written at ring word 1020: the last two wrap to words 0 and 1.
frame3:
	.long 0x00002208, 4			# RB_MODECONTROL = color + depth
	.long 0xC0003600, 0x00030004		# DRAW_INDX_2
	.long 0xC0003600, 0x00030004		# DRAW_INDX_2 (at ring words 0 and 1)

# Import address slots (type 0 records).
	.balign 4
MmAllocatePhysicalMemoryEx_slot:	.long 0x000000BA
MmGetPhysicalAddress_slot:		.long 0x000000BE
VdInitializeRingBuffer_slot:		.long 0x000001C3
VdSwap_slot:				.long 0x0000025B

	.section .data
	.balign 4
	.globl results
results:	.long 0, 0

# Ring layout in words: frame 1 at 0..21, its swap at 22..85; frame 2 at 86..92, its swap at 93..156;
# frame 3 from 157 (a NOP whose 862 words run to 1019), 1020..1023 and 0..1, its swap at 2..65.
	.text
	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -128(r1)
	stw r26, 96(r1)
	stw r27, 100(r1)
	stw r28, 104(r1)
	stw r29, 108(r1)
	stw r30, 112(r1)
	stw r31, 116(r1)
	# ring = MmAllocatePhysicalMemoryEx(0, 64 KB, PAGE_READWRITE | MEM_LARGE_PAGES, 0, 0xFFFFFFFF, 0)
	li r3, 0
	lis r4, 1
	lis r5, 0x2000
	ori r5, r5, 4
	li r6, 0
	li r7, -1
	li r8, 0
	bl MmAllocatePhysicalMemoryEx_thunk
	mr r31, r3
	# block = MmAllocatePhysicalMemoryEx(the same)
	li r3, 0
	lis r4, 1
	lis r5, 0x2000
	ori r5, r5, 4
	li r6, 0
	li r7, -1
	li r8, 0
	bl MmAllocatePhysicalMemoryEx_thunk
	mr r29, r3
	# Their physical (GPU) addresses.
	mr r3, r31
	bl MmGetPhysicalAddress_thunk
	mr r30, r3
	mr r3, r29
	bl MmGetPhysicalAddress_thunk
	mr r28, r3
	# results: 1 when each physical address is not 0
	lis r27, results@ha
	addi r27, r27, results@l
	li r0, 0
	cmpwi r30, 0
	beq 1f
	li r0, 1
1:	stw r0, 0(r27)
	li r0, 0
	cmpwi r28, 0
	beq 1f
	li r0, 1
1:	stw r0, 4(r27)
	# VdInitializeRingBuffer(ring physical address, 9): 1 << (9 + 3) = 4 KB.
	mr r3, r30
	li r4, 9
	bl VdInitializeRingBuffer_thunk

	# The block: the indirect buffer at +0, the fetch constant at +0x100, VdSwap's value cells at +0x200
	# (front buffer address, format k_8_8_8_8 = 6, color space 0, width 16, height 16), the front buffer at +0x1000.
	addi r26, r29, 0x1000
	stw r26, 0x200(r29)
	li r0, 6
	stw r0, 0x204(r29)
	li r0, 0
	stw r0, 0x208(r29)
	li r0, 16
	stw r0, 0x20C(r29)
	stw r0, 0x210(r29)
	# Fetch constant: dword 1 = front buffer address | format, dword 2 = (width - 1) | (height - 1) << 13.
	ori r0, r26, 6
	stw r0, 0x104(r29)
	lis r0, 1
	ori r0, r0, 0xE00F
	stw r0, 0x108(r29)
	# 192 of the 256 front buffer pixels = 0x11223344 (the block is zeroed when allocated).
	lis r0, 0x1122
	ori r0, r0, 0x3344
	li r4, 192
	mtctr r4
	mr r4, r26
1:	stw r0, 0(r4)
	addi r4, r4, 4
	bdnz 1b
	# The indirect buffer.
	mr r3, r29
	lis r4, ib@ha
	addi r4, r4, ib@l
	li r5, 4
	bl copy_words

	# Frame 1 at ring word 0, its GPU addresses patched in; VdSwap's 64 words at word 22.
	mr r3, r31
	lis r4, frame1@ha
	addi r4, r4, frame1@l
	li r5, 22
	bl copy_words
	stw r28, (frame1_ib - frame1 + 4)(r31)
	addi r0, r28, 0x1000
	stw r0, (frame1_dest - frame1)(r31)
	addi r3, r31, 22 * 4
	bl swap

	# Frame 2 at word 86, its swap at 93.
	addi r3, r31, 86 * 4
	lis r4, frame2@ha
	addi r4, r4, frame2@l
	li r5, 7
	bl copy_words
	addi r3, r31, 93 * 4
	bl swap

	# Frame 3: a NOP at word 157 over the 862 words up to 1019, then 4 words at 1020 and 2 at 0; its swap at 2.
	lis r0, 0xC35D
	ori r0, r0, 0x1000
	stw r0, 157 * 4(r31)
	addi r3, r31, 1020 * 4
	lis r4, frame3@ha
	addi r4, r4, frame3@l
	li r5, 4
	bl copy_words
	mr r3, r31
	lis r4, frame3@ha
	addi r4, r4, frame3@l
	addi r4, r4, 16
	li r5, 2
	bl copy_words
	addi r3, r31, 2 * 4
	bl swap

	li r3, 0
	lwz r26, 96(r1)
	lwz r27, 100(r1)
	lwz r28, 104(r1)
	lwz r29, 108(r1)
	lwz r30, 112(r1)
	lwz r31, 116(r1)
	addi r1, r1, 128
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# copy_words(destination r3, source r4, count r5): copies count words.
	.globl copy_words
	.type copy_words,@function
copy_words:
	mtctr r5
1:	lwz r0, 0(r4)
	stw r0, 0(r3)
	addi r4, r4, 4
	addi r3, r3, 4
	bdnz 1b
	blr
	.size copy_words, .-copy_words

# swap(buffer r3): VdSwap(buffer, fetch constant, 0, 0, 0, &front buffer address, &format, &color space,
# &width, &height) with the cells in the block (r29); the 9th and 10th arguments go on the stack.
	.globl swap
	.type swap,@function
swap:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -112(r1)
	addi r4, r29, 0x100
	li r5, 0
	li r6, 0
	li r7, 0
	addi r8, r29, 0x200
	addi r9, r29, 0x204
	addi r10, r29, 0x208
	addi r0, r29, 0x20C
	stw r0, 0x54(r1)
	addi r0, r29, 0x210
	stw r0, 0x5C(r1)
	bl VdSwap_thunk
	addi r1, r1, 112
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size swap, .-swap

# Import thunks, last in .text (see imports_ttt2.s).
	.balign 4
MmAllocatePhysicalMemoryEx_thunk:
	.long 0x010000BA, 0, 0, 0
MmGetPhysicalAddress_thunk:
	.long 0x010000BE, 0, 0, 0
VdInitializeRingBuffer_thunk:
	.long 0x010001C3, 0, 0, 0
VdSwap_thunk:
	.long 0x0100025B, 0, 0, 0
