# License-free PowerPC test input: a boot that deadlocks, for the host's stall diagnostics
# (recomp/src/boot_monitor.cpp, wait_overrides.cpp; Ttt2App's watchdog dump).
#
# The main thread initialises a critical section and an event, starts a worker thread and a spinner
# thread and waits (in short sleeps) until the worker holds the critical section; then it enters the
# section itself and blocks for good. The worker, holding the section, waits on the event, which nobody
# sets. The spinner does what TTT2 Web documents for the game's sub_82ABFDE8 and the owner's log shows as
# one thread with 339 million kernel calls: it signals its own synchronization event and waits on it
# (NtSignalAndWaitForSingleObjectEx), round after round, until a 64-bit counter changes - which nothing
# here changes. No boot progress after that, every thread alive: what the owner's black screen looks like
# to the watchdog. Its stall dump must name both waits - the main thread's on the critical section, owned
# by the worker, and the worker's on the unsignaled synchronization event - with the guest addresses they
# were called from, the creation of the threads with their start addresses, and the spinner as busy
# (calls a second, its busiest call). scripts/host-boot-test.sh --expect-stall --expect-waits checks that.
# Kernel ordinals from the public xboxkrnl.exe export table: RtlInitializeCriticalSection 302,
# KeInitializeEvent 112, ExCreateThread 13, KeDelayExecutionThread 90, RtlEnterCriticalSection 293,
# KeWaitForSingleObject 176, NtCreateEvent 209, NtSignalAndWaitForSingleObjectEx 251. Our own code, no game
# bytes.
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
	# ExCreateThread(&handle2, default stack, &thread_id2, no XAPI startup, spinner, NULL, 0)
	lis r3, handle2@ha
	addi r3, r3, handle2@l
	li r4, 0
	lis r5, thread_id2@ha
	addi r5, r5, thread_id2@l
	li r6, 0
	lis r7, spinner@ha
	addi r7, r7, spinner@l
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

	.globl spinner
	.type spinner,@function
spinner:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	stw r31, 56(r1)
	# NtCreateEvent(&spin_event, NULL, SynchronizationEvent, FALSE)
	lis r3, spin_event@ha
	addi r3, r3, spin_event@l
	li r4, 0
	li r5, 1
	li r6, 0
	bl NtCreateEvent_thunk
	lis r31, spin_event@ha
	lwz r31, spin_event@l(r31)
	# NtSignalAndWaitForSingleObjectEx(event, event, FALSE, 0, NULL) until the 64-bit counter changes.
.Lspin:
	mr r3, r31
	mr r4, r31
	li r5, 0
	li r6, 0
	li r7, 0
	bl NtSignalAndWaitForSingleObjectEx_thunk
	lis r3, counter@ha
	ld r4, counter@l(r3)
	cmpdi r4, 0
	beq .Lspin
	li r3, 0
	lwz r31, 56(r1)
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size spinner, .-spinner

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
NtCreateEvent_thunk:
	.long 0x010000D1, 0, 0, 0
NtSignalAndWaitForSingleObjectEx_thunk:
	.long 0x010000FB, 0, 0, 0

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
NtCreateEvent_slot:			.long 0x000000D1
NtSignalAndWaitForSingleObjectEx_slot:	.long 0x000000FB

	.section .data
	.balign 8
section:	.space 28
	.balign 8
event:		.space 16
handle:		.long 0
thread_id:	.long 0
handle2:	.long 0
thread_id2:	.long 0
spin_event:	.long 0
owned:		.long 0
	.balign 8
counter:	.long 0, 0
	.globl results
results:	.long 0
