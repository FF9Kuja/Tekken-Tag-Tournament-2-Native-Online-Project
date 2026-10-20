# License-free PowerPC test input: functions that only a pointer in data reaches and that ReXGlue
# 0.10.0 folds into another function, which is how most of the calls to unregistered functions on
# the owner's first boots arise (0x82D6FC78 and the others: TTT2 Web's table lists ~690 functions
# that ReXGlue never registers, mostly 4- to 32-byte virtual methods of vtables without RTTI).
#
# A branch from a registered function reaches each target below, so ReXGlue's analysis makes the
# target an internal label of that function instead of a function: codegen succeeds, and the call
# through the pointer then ends in the runtime's "[FATAL] Call to invalid or unregistered function at
# guest address ...". The run traps on D1 first; resolved from the boot log alone, the program
# would need one rebuild per shape. recomp/tools/resolve_functions.py codegen --indirect registers
# both ahead of time instead (the program's boundary table, pointer_ttt2.functions, stands in for TTT2
# Web's), and ReXGlue then emits the branches as (conditional) tail calls; measured with the pinned SDK:
#   D1 cond_target: cond_split's `beq` reaches it; called through ptr_table (.rdata)
#   D2 over_target: jump_over's forward `b` across mid_leaf reaches it; called through cb_table (.data),
#      as a callback in writable data would be
# The table also lists three functions the resolver must NOT register ahead of time (negative controls;
# a wrong or unprovable entry of a reference would look like these):
#   N1 fold_only: folded into jump_twice like D2, but no word in the data points at it: no evidence
#   N2 after_call: a data word points at it, but the bl before it falls through into it: code inside
#      twice_caller, not an entry
#   N3 handler_label: a data word points at it, but it lies inside _start's .pdata range (an
#      exception-handler label is code of its function)
# Our own code, no game bytes. Assemble with the SDK's bundled powerpc-none-elf-as
# (scripts/make-ppc-test-xex.sh); scripts/codegen-smoke-test.sh runs it.
#
# Expected results: 50 6 57 205 204 1004 62   (sum 1588)
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	stw r31, 60(r1)
	lis r31, results@ha
	addi r31, r31, results@l
	li r3, 0
	bl cond_split           # 0: the conditional tail branch to cond_target: 0 + 50 = 50
	stw r3, 0(r31)
	li r3, 5
	bl cond_split           # 5 + 1 = 6
	stw r3, 4(r31)
	lis r11, ptr_table@ha
	lwz r11, ptr_table@l(r11)
	mtctr r11
	li r3, 7
	bctrl                   # D1 through the pointer: cond_target(7) = 57
	stw r3, 8(r31)
	li r3, 3
	bl jump_over            # 3 + 2, then over_target: + 200 = 205
	stw r3, 12(r31)
	lis r11, cb_table@ha
	lwz r11, cb_table@l(r11)
	mtctr r11
	li r3, 4
	bctrl                   # D2 through the pointer: over_target(4) = 204
	stw r3, 16(r31)
	li r3, 1
	bl jump_twice           # 1 + 3, then fold_only: + 1000 = 1004
	stw r3, 20(r31)
	li r3, 2
	bl twice_caller         # 2 + 30 + 30 = 62
	stw r3, 24(r31)
	lwz r3, 0(r31)
	lwz r4, 4(r31)
	add r3, r3, r4
	lwz r4, 8(r31)
	add r3, r3, r4
	lwz r4, 12(r31)
	add r3, r3, r4
	lwz r4, 16(r31)
	add r3, r3, r4
	lwz r4, 20(r31)
	add r3, r3, r4
	lwz r4, 24(r31)
	add r3, r3, r4          # return the sum (1588)
	lwz r31, 60(r1)
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
# N3: inside _start's .pdata range, after its blr; never executed.
handler_label:
	li r3, -1
	blr
handler_end:
	.size _start, .-_start

cond_split:
	cmpwi r3, 0
	beq cond_target
	addi r3, r3, 1
	blr
# D1
cond_target:
	addi r3, r3, 50
	blr

jump_over:
	addi r3, r3, 2
	b over_target
mid_leaf:
	addi r3, r3, 30
	blr
# D2
over_target:
	addi r3, r3, 200
	blr

jump_twice:
	addi r3, r3, 3
	b fold_only
twice_caller:
	mflr r12
	stw r12, -8(r1)
	stwu r1, -32(r1)
	bl mid_leaf
# N2: the bl above falls through into it.
after_call:
	bl mid_leaf
	addi r1, r1, 32
	lwz r12, -8(r1)
	mtlr r12
	blr
twice_end:
# N1: only jump_twice's branch reaches it.
fold_only:
	addi r3, r3, 1000
	blr
fold_end:

	.section .rodata
	.align 2
ptr_table:	.long cond_target, after_call, handler_label

	.section .data
	.align 2
cb_table:	.long over_target
	.globl results
results:	.long 0, 0, 0, 0, 0, 0, 0
