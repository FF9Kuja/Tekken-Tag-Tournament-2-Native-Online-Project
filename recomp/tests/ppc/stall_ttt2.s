# License-free PowerPC test input: a program that never finishes. Its entry point stores a marker and
# then spins in a tight loop with no system calls, which is what a boot that waits forever looks like
# to the host's watchdog: the guest-thread census and the kernel stub total stop changing while the
# main thread is still running, so after three ticks Ttt2App logs a "possible stall" dump with every
# guest thread's state (recomp/src/ttt2_app.cpp, WatchdogTick). scripts/host-boot-test.sh --expect-stall
# boots the host on this image with a 1 s watchdog period and checks for that dump.
# Assemble with the SDK's bundled powerpc-none-elf-as (-a32 -be -mregnames -mpower7 -maltivec
# -mvsx -mvmx128); our own code, no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	lis r31, results@ha
	addi r31, r31, results@l
	li r3, 1
	stw r3, 0(r31)			# marker: the program started
	li r4, 0
.Lspin:
	addi r4, r4, 1			# count laps so the loop is not empty
	stw r4, 4(r31)
	b .Lspin
	.size _start, .-_start

	.section .data
	.balign 4
	.globl results
results:	.long 0, 0
