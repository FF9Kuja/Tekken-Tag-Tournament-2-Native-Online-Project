# License-free PowerPC test input: a function whose recorded extent ends before its code does.
#
# lock_take enters a critical section, and its .pdata entry (the packer writes one per FUNC symbol, from
# .size) ends right after that call; the instructions after it - leaving the section, counting the round,
# restoring the stack frame and returning - are a function of their own in .pdata, lock_tail, which nothing
# calls. On the console the CPU simply runs on from lock_take into lock_tail. ReXGlue 0.10.0 ends the
# generated lock_take at its recorded end: it returns with the section still held, the stack frame and r31
# not restored, and lock_tail never runs - one leaked recursion level per call, silently (measured: no
# codegen warning). The owner's boots show a critical section held by the main thread at recursion 390
# while two other threads wait for it; TTT2 Web documents functions that end mid-flow in this game.
# Project patch third_party/rexglue-patches/0001 makes the generated code continue into the function that
# starts where the extent ends, as the CPU does, and logs each such site ("Fall-through: ...").
#
# _start calls lock_take five times and then records:
#   results[0]  calls made                                                      5
#   results[1]  the section's recursion count afterwards (word at +0x14)        0   (unpatched: 5)
#   results[2]  1 when the section still has an owner (word at +0x18)           0   (unpatched: 1)
#   results[3]  rounds lock_tail counted                                        5   (unpatched: 0)
# _start reloads every address it needs instead of keeping it in a register, so it records the same
# values whether or not the calls restored the stack pointer and r31. Kernel ordinals from the public
# xboxkrnl.exe export table: RtlEnterCriticalSection 293, RtlInitializeCriticalSection 302,
# RtlLeaveCriticalSection 304. Our own code, no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	lis r3, section@ha
	addi r3, r3, section@l
	bl RtlInitializeCriticalSection_thunk
	li r0, 5
	lis r4, remaining@ha
	stw r0, remaining@l(r4)
.Lround:
	lis r3, section@ha
	addi r3, r3, section@l
	bl lock_take
	lis r4, results@ha
	addi r4, r4, results@l
	lwz r5, 0(r4)
	addi r5, r5, 1
	stw r5, 0(r4)
	lis r4, remaining@ha
	lwz r5, remaining@l(r4)
	addi r5, r5, -1
	stw r5, remaining@l(r4)
	cmpwi r5, 0
	bne .Lround
	lis r4, results@ha
	addi r4, r4, results@l
	lis r6, section@ha
	addi r6, r6, section@l
	lwz r5, 0x14(r6)
	stw r5, 4(r4)
	lwz r5, 0x18(r6)
	li r7, 0
	cmpwi r5, 0
	beq 1f
	li r7, 1
1:	stw r7, 8(r4)
	lis r6, rounds@ha
	lwz r5, rounds@l(r6)
	stw r5, 12(r4)
	li r3, 0
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# lock_take(section r3): its .pdata length ends after the call that enters the section.
	.globl lock_take
	.type lock_take,@function
lock_take:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -32(r1)
	stw r31, 24(r1)
	mr r31, r3
	bl RtlEnterCriticalSection_thunk
	.size lock_take, .-lock_take

# The rest of lock_take's code, with a .pdata entry of its own: leaves the section, counts the round,
# restores the frame and returns to lock_take's caller.
	.globl lock_tail
	.type lock_tail,@function
lock_tail:
	mr r3, r31
	bl RtlLeaveCriticalSection_thunk
	lis r4, rounds@ha
	lwz r5, rounds@l(r4)
	addi r5, r5, 1
	stw r5, rounds@l(r4)
	lwz r31, 24(r1)
	addi r1, r1, 32
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size lock_tail, .-lock_tail

# Import thunks, last in .text (see imports_ttt2.s).
	.balign 4
RtlInitializeCriticalSection_thunk:
	.long 0x0100012E, 0, 0, 0
RtlEnterCriticalSection_thunk:
	.long 0x01000125, 0, 0, 0
RtlLeaveCriticalSection_thunk:
	.long 0x01000130, 0, 0, 0

	.section .rodata
# Import address slots (type 0 records).
	.balign 4
RtlInitializeCriticalSection_slot:	.long 0x0000012E
RtlEnterCriticalSection_slot:		.long 0x00000125
RtlLeaveCriticalSection_slot:		.long 0x00000130

	.section .data
	.balign 8
section:	.space 28
	.balign 4
remaining:	.long 0
rounds:		.long 0
	.globl results
results:	.long 0, 0, 0, 0
