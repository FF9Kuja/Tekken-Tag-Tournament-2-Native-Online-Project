# License-free PowerPC test input: a program that faults on purpose. Its entry point stores a marker
# and then writes to guest address 0x10, in the unmapped null page, so the host takes an access
# violation that no runtime handler claims. Ttt2App::OnHostException must then log the HOST CRASH
# block and end the process with exit code 3 (recomp/src/ttt2_app.cpp); without that, SDK 0.10.0's
# signal handler would re-execute the store forever. scripts/host-boot-test.sh --expect-crash boots
# the host on this image and checks for the report and the exit code. Deterministic on its own, unlike a
# wrong jump table, so it stays a crash whatever the switch-table finder does.
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
	li r4, 0x10
	stw r3, 0(r4)			# write to the null page: unhandled guest access violation
	li r3, 2
	stw r3, 0(r31)			# never reached
	blr
	.size _start, .-_start

	.section .data
	.balign 4
	.globl results
results:	.long 0
