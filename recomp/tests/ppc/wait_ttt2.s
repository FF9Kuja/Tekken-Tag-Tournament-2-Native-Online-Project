# License-free PowerPC test input: a boot that deadlocks, for the host's stall diagnostics
# (recomp/src/boot_monitor.cpp, wait_overrides.cpp; Ttt2App's watchdog dump).
#
# The main thread initialises a critical section and an event, starts a worker thread and waits (in
# short sleeps) until the worker holds the critical section; then it enters the section itself and
# blocks for good. The worker, holding the section, waits on the event, which nobody sets. Nothing
# moves after that, every thread is alive: what the owner's black screen looks like to the watchdog.
# Its stall dump must name both waits - the main thread's on the critical section, owned by the
# worker, and the worker's on the unsignaled synchronization event - with the guest addresses they
# were called from, and the creation of the worker with its start address. scripts/host-boot-test.sh
# --expect-stall --expect-waits checks that. Kernel ordinals from the public xboxkrnl.exe export table:
# RtlInitializeCriticalSection 302, KeInitializeEvent 112, ExCreateThread 13, KeDelayExecutionThread
# 90, RtlEnterCriticalSection 293, KeWaitForSingleObject 176. Our own code, no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -96(r1)
	# RtlInitializeCriticalSection(&section)
	lis r3, section@ha
	addi r3, r3, section@l
	bl RtlInitializeCriticalSection_thunk
	# KeInitializeEvent(&event, SynchronizationEvent, FALSE)
	lis r3, event@ha
	addi r3, r3, event@l
	li r4, 1
	li r5, 0
	bl KeInitializeEvent_thunk
	# ExCreateThread(&handle, default stack, &thread_id, no XAPI startup, worker, NULL, 0)
	lis r3, handle@ha
	addi r3, r3, handle@l
	li r4, 0
	lis r5, thread_id@ha
	addi r5, r5, thread_id@l
	li r6, 0
	lis r7, worker@ha
	addi r7, r7, worker@l
	li r8, 0
	li r9, 0
	bl ExCreateThread_thunk
	# Until the worker owns the section: KeDelayExecutionThread(UserMode, FALSE, -10 ms)
.Lpoll:
	lis r3, owned@ha
	lwz r3, owned@l(r3)
	cmpwi r3, 0
	bne .Lowned
	li r3, 1
	li r4, 0
	lis r5, interval@ha
	addi r5, r5, interval@l
	bl KeDelayExecutionThread_thunk
	b .Lpoll
.Lowned:
	# RtlEnterCriticalSection(&section): the worker holds it and never leaves it.
	lis r3, section@ha
	addi r3, r3, section@l
	bl RtlEnterCriticalSection_thunk
	li r3, 0
	addi r1, r1, 96
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

	.globl worker
	.type worker,@function
worker:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	# RtlEnterCriticalSection(&section), then tell the main thread
	lis r3, section@ha
	addi r3, r3, section@l
	bl RtlEnterCriticalSection_thunk
	li r4, 1
	lis r3, owned@ha
	stw r4, owned@l(r3)
	# KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL): nobody sets it.
	lis r3, event@ha
	addi r3, r3, event@l
	li r4, 0
	li r5, 0
	li r6, 0
	li r7, 0
	bl KeWaitForSingleObject_thunk
	li r3, 0
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size worker, .-worker

# Import thunks, last in .text (see imports_ttt2.s).
	.balign 4
RtlInitializeCriticalSection_thunk:
	.long 0x0100012E, 0, 0, 0
KeInitializeEvent_thunk:
	.long 0x01000070, 0, 0, 0
ExCreateThread_thunk:
	.long 0x0100000D, 0, 0, 0
KeDelayExecutionThread_thunk:
	.long 0x0100005A, 0, 0, 0
RtlEnterCriticalSection_thunk:
	.long 0x01000125, 0, 0, 0
KeWaitForSingleObject_thunk:
	.long 0x010000B0, 0, 0, 0

	.section .rodata
	.balign 8
# -10 ms as a relative LARGE_INTEGER timeout (100 ns units, negative = relative).
interval:	.long 0xFFFFFFFF, 0xFFFE7960
# Import address slots (type 0 records).
	.balign 4
RtlInitializeCriticalSection_slot:	.long 0x0000012E
KeInitializeEvent_slot:			.long 0x00000070
ExCreateThread_slot:			.long 0x0000000D
KeDelayExecutionThread_slot:		.long 0x0000005A
RtlEnterCriticalSection_slot:		.long 0x00000125
KeWaitForSingleObject_slot:		.long 0x000000B0

	.section .data
	.balign 8
section:	.space 28
	.balign 8
event:		.space 16
handle:		.long 0
thread_id:	.long 0
owned:		.long 0
	.globl results
results:	.long 0
