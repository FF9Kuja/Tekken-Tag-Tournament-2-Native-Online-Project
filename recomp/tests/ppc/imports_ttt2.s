# License-free PowerPC test input: kernel imports called the way a real Xbox 360 executable calls
# them. The XEX2 import table (built by scripts/ppc_elf_to_xex.py from imports_ttt2.imports) lists
# one 4-byte slot (type 0) per import in .rodata and one 16-byte thunk (type 1) at the end of
# .text, both starting with the record word (type << 24 | ordinal); the code calls `bl thunk`.
# `rexglue codegen` resolves the ordinals to __imp__<Name> calls into the host, exactly as for the
# game, so a native run of this program proves the host's kernel-override mechanism
# (recomp/src/kernel_overrides.cpp): two XAM calls the runtime only stubs must return TTT2 Web's
# values, and an implemented kernel export must work alongside them.
#   expected results: 0 1317 5   (sum 1322)
#     XamShowFriendsUI            -> ERROR_SUCCESS (host override)
#     XamUserCreateStatsEnumerator-> ERROR_NO_SUCH_USER = 1317 (host override)
#     RtlCompareMemory(a, b, 8)   -> 5 matching bytes (SDK runtime)
# Assemble with the SDK's bundled powerpc-none-elf-as (-a32 -be -mregnames -mpower7 -maltivec
# -mvsx -mvmx128); our own code, no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	lis r31, results@ha
	addi r31, r31, results@l
	# XamShowFriendsUI(user 0): the runtime's stub would leave r3 untouched (r3 = 0x5A5A5A5A here).
	lis r3, 0x5A5A
	ori r3, r3, 0x5A5A
	bl XamShowFriendsUI_thunk
	stw r3, 0(r31)
	# XamUserCreateStatsEnumerator(title, user, count, flags, specs, buffer size, handle out).
	li r3, 0
	li r4, 0
	li r5, 1
	li r6, 0
	li r7, 0
	li r8, 0
	li r9, 0
	bl XamUserCreateStatsEnumerator_thunk
	stw r3, 4(r31)
	# RtlCompareMemory(buf_a, buf_b, 8): 5 bytes match.
	lis r3, buf_a@ha
	addi r3, r3, buf_a@l
	lis r4, buf_b@ha
	addi r4, r4, buf_b@l
	li r5, 8
	bl RtlCompareMemory_thunk
	stw r3, 8(r31)
	# Return the sum.
	lwz r3, 0(r31)
	lwz r4, 4(r31)
	add r3, r3, r4
	lwz r4, 8(r31)
	add r3, r3, r4
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# Import thunk table, last in .text as in a real executable (the codegen treats everything from the
# first thunk to the end of the section as non-code). Word 0 is the import record the loader reads;
# the console's loader would patch the thunk into a jump through the slot. Plain labels, not
# functions: no .pdata entries. Ordinals from the public xboxkrnl.exe / xam.xex export tables.
	.balign 4
XamShowFriendsUI_thunk:
	.long 0x010002BF, 0, 0, 0		# xam.xex 703
XamUserCreateStatsEnumerator_thunk:
	.long 0x010002F7, 0, 0, 0		# xam.xex 759
RtlCompareMemory_thunk:
	.long 0x0100011A, 0, 0, 0		# xboxkrnl.exe 282

	.section .rodata
	.balign 4
buf_a:	.byte 1, 2, 3, 4, 5, 6, 7, 8
buf_b:	.byte 1, 2, 3, 4, 5, 9, 9, 9
# Import address slots (type 0 records), one word per import.
	.balign 4
XamShowFriendsUI_slot:			.long 0x000002BF
XamUserCreateStatsEnumerator_slot:	.long 0x000002F7
RtlCompareMemory_slot:			.long 0x0000011A

	.section .data
	.balign 4
	.globl results
results:	.long 0, 0, 0
