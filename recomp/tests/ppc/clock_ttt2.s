# License-free PowerPC test input: the kernel clock the game reads straight from memory.
#
# KeTimeStampBundle (xboxkrnl data export 0xAD) is 24 bytes: u64 InterruptTime (100 ns units since
# boot), u64 SystemTime (FILETIME), u32 TickCount (milliseconds since boot). A program imports it like
# any kernel variable: the loader writes its address into the import slot (a type 0 record with no
# thunk), and the code loads the slot, then the fields, without a kernel call. ReXGlue 0.10.0 refreshes
# only TickCount; the host refreshes the other two every millisecond (recomp/src/kernel_clock.cpp).
#
# This program reads the bundle, sleeps 50 ms, reads it again and compares SystemTime with
# KeQuerySystemTime. Then it waits for InterruptTime to change the way a TTT2 thread waits for its
# clock (TTT2 Web documents sub_82ABFDE8: it signals its own synchronization event and waits on it until
# a 64-bit counter changes): NtSignalAndWaitForSingleObjectEx on one auto-reset event, round after round,
# giving up after 1,000,000 rounds. With a frozen clock that loop is the owner's thread with 339 million
# kernel calls in 180 seconds.
#
#   expected results (default):          1 1 1 1 1 1 1
#   with TTT2_KERNEL_CLOCK=runtime:      0 0 0 1 0 0 0
#     0 InterruptTime advanced at least 20 ms over the 50 ms sleep
#     1 SystemTime is a date after 2014 (high word >= 0x01D00000)
#     2 SystemTime advanced at least 20 ms over the sleep
#     3 TickCount advanced at least 20 ms over the sleep (the runtime's own refresh: the control)
#     4 InterruptTime is nonzero and, in milliseconds, within 100 ms of TickCount
#     5 SystemTime is within 1 s of KeQuerySystemTime
#     6 the signal-and-wait loop saw InterruptTime change before giving up
# Assembled with the SDK's bundled binutils (scripts/make-ppc-test-xex.sh). Our own code, no game bytes.
# Kernel ordinals from the public xboxkrnl.exe export table: KeTimeStampBundle 173 (a variable),
# KeDelayExecutionThread 90, KeQuerySystemTime 132, NtCreateEvent 209, NtSignalAndWaitForSingleObjectEx 251.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -192(r1)
	std r22, 96(r1)
	std r23, 104(r1)
	std r24, 112(r1)
	std r25, 120(r1)
	std r26, 128(r1)
	std r27, 136(r1)
	std r28, 144(r1)
	std r29, 152(r1)
	std r30, 160(r1)
	std r31, 168(r1)
	lis r31, results@ha
	addi r31, r31, results@l
	# The bundle's address, as the loader patched it into the import slot.
	lis r30, KeTimeStampBundle_slot@ha
	lwz r30, KeTimeStampBundle_slot@l(r30)
	ld r29, 0(r30)		# InterruptTime
	ld r28, 8(r30)		# SystemTime
	lwz r27, 16(r30)	# TickCount
	# KeDelayExecutionThread(UserMode, FALSE, -50 ms)
	li r3, 1
	li r4, 0
	lis r5, interval@ha
	addi r5, r5, interval@l
	bl KeDelayExecutionThread_thunk
	ld r26, 0(r30)
	ld r25, 8(r30)
	lwz r24, 16(r30)

	# 0: InterruptTime advanced at least 20 ms (200000 units of 100 ns)
	subf r3, r29, r26
	lis r4, 3
	ori r4, r4, 0x0D40
	li r0, 0
	cmpld r3, r4
	blt 1f
	li r0, 1
1:	stw r0, 0(r31)

	# 1: SystemTime is a date after 2014: its high word is at least 0x01D00000
	srdi r3, r25, 32
	lis r4, 0x01D0
	li r0, 0
	cmplw r3, r4
	blt 1f
	li r0, 1
1:	stw r0, 4(r31)

	# 2: SystemTime advanced at least 20 ms
	subf r3, r28, r25
	lis r4, 3
	ori r4, r4, 0x0D40
	li r0, 0
	cmpld r3, r4
	blt 1f
	li r0, 1
1:	stw r0, 8(r31)

	# 3: TickCount advanced at least 20 ms (the runtime refreshes it on its own)
	subf r3, r27, r24
	li r0, 0
	cmplwi r3, 20
	blt 1f
	li r0, 1
1:	stw r0, 12(r31)

	# 4: InterruptTime is nonzero and, in milliseconds, within 100 ms of TickCount
	li r0, 0
	cmpldi r26, 0
	beq 1f
	li r4, 10000
	divdu r3, r26, r4
	subf r3, r24, r3
	cmpdi r3, 0
	bge 2f
	neg r3, r3
2:	cmpldi r3, 100
	bgt 1f
	li r0, 1
1:	stw r0, 16(r31)

	# 5: SystemTime within 1 s (10,000,000 units) of KeQuerySystemTime(&now)
	lis r3, now@ha
	addi r3, r3, now@l
	bl KeQuerySystemTime_thunk
	lis r3, now@ha
	ld r3, now@l(r3)
	ld r4, 8(r30)
	subf r3, r4, r3
	cmpdi r3, 0
	bge 2f
	neg r3, r3
2:	lis r4, 0x0098
	ori r4, r4, 0x9680
	li r0, 0
	cmpld r3, r4
	bgt 1f
	li r0, 1
1:	stw r0, 20(r31)

	# 6: wait for InterruptTime to change: NtCreateEvent(&event_handle, NULL, SynchronizationEvent, FALSE),
	#    then NtSignalAndWaitForSingleObjectEx(event, event, FALSE, 0, NULL) until the counter differs
	#    from the value read before the loop, at most 1,000,000 (0xF4240) rounds.
	lis r3, event_handle@ha
	addi r3, r3, event_handle@l
	li r4, 0
	li r5, 1
	li r6, 0
	bl NtCreateEvent_thunk
	lis r22, event_handle@ha
	lwz r22, event_handle@l(r22)
	ld r29, 0(r30)
	li r23, 0
.Lspin:
	mr r3, r22
	mr r4, r22
	li r5, 0
	li r6, 0
	li r7, 0
	bl NtSignalAndWaitForSingleObjectEx_thunk
	ld r3, 0(r30)
	cmpld r3, r29
	bne .Lchanged
	addi r23, r23, 1
	lis r4, 0x000F
	ori r4, r4, 0x4240
	cmplw r23, r4
	blt .Lspin
	li r0, 0
	b .Lspun
.Lchanged:
	li r0, 1
.Lspun:
	stw r0, 24(r31)

	li r3, 0
	ld r22, 96(r1)
	ld r23, 104(r1)
	ld r24, 112(r1)
	ld r25, 120(r1)
	ld r26, 128(r1)
	ld r27, 136(r1)
	ld r28, 144(r1)
	ld r29, 152(r1)
	ld r30, 160(r1)
	ld r31, 168(r1)
	addi r1, r1, 192
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# Import thunks, last in .text (see imports_ttt2.s). KeTimeStampBundle is a variable: a slot, no thunk.
	.balign 4
KeDelayExecutionThread_thunk:
	.long 0x0100005A, 0, 0, 0
KeQuerySystemTime_thunk:
	.long 0x01000084, 0, 0, 0
NtCreateEvent_thunk:
	.long 0x010000D1, 0, 0, 0
NtSignalAndWaitForSingleObjectEx_thunk:
	.long 0x010000FB, 0, 0, 0

	.section .rodata
	.balign 8
# -50 ms as a relative LARGE_INTEGER timeout (100 ns units, negative = relative).
interval:	.long 0xFFFFFFFF, 0xFFF85EE0
# Import address slots (type 0 records); the loader overwrites KeTimeStampBundle's with the bundle's address.
	.balign 4
KeTimeStampBundle_slot:			.long 0x000000AD
KeDelayExecutionThread_slot:		.long 0x0000005A
KeQuerySystemTime_slot:			.long 0x00000084
NtCreateEvent_slot:			.long 0x000000D1
NtSignalAndWaitForSingleObjectEx_slot:	.long 0x000000FB

	.section .data
	.balign 8
now:		.long 0, 0
event_handle:	.long 0
	.globl results
results:	.long 0, 0, 0, 0, 0, 0, 0
