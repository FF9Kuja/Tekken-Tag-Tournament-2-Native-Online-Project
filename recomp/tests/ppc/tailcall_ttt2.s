# License-free PowerPC test input: the two function-boundary shapes behind the "target not in any
# function" errors ReXGlue 0.10.0 reported on Tekken Tag Tournament 2's retail executable (owner's
# codegen log, 2026-10-19: 14 UnresolvedCall errors, 13 unsealed functions).
#
#  A. A thunk (adjustor-style `addi r3,r3,N; b target`) tail-branches to a leaf function that no
#     .pdata entry, `bl` or function pointer names. ReXGlue registers `bl` targets as functions but
#     never `b` targets, so only its gap fill could cover the leaf; gap fill skips the whole code run
#     that starts at the function in front of it (whose last instruction branches past the leaf to a
#     target that is not a function yet), so the leaf ends up in no function.
#  B. A function reached only through a pointer in data (vtable-style) keeps a rarely taken block
#     after its blr, and that block branches back into the body. ReXGlue does not register the
#     function (no .pdata, no bl, no RTTI); gap fill registers the out-of-line block as a function
#     of its own, whose backward branch then targets the uncovered body.
#  C. A second pointer-called leaf follows that block. No direct branch reaches it, so codegen passes,
#     but no function covers it either: the indirect call at run time ends in the runtime's
#     "[FATAL] Call to invalid or unregistered function at guest address ..." (the kind of failure a
#     first boot of the game can show for leaf virtual functions in vtables without RTTI).
#
# A and B reproduce with the pinned SDK as codegen errors, C as the runtime trap once A and B are
# resolved. recomp/tools/resolve_functions.py turns each into a verified [functions] entry (A: the
# leaf's entry point; B: the whole function, so the branch is internal again; C, from the boot log:
# the called leaf's entry point) and scripts/codegen-smoke-test.sh runs the result natively. The true
# boundaries, as a control-flow walk of this program gives them, are in tailcall_ttt2.functions (the
# stand-in for TTT2 Web's published table). Our own code, no game bytes. Assemble with the SDK's
# bundled powerpc-none-elf-as (scripts/make-ppc-test-xex.sh).
#
# Expected results: 1002 112 24 406 35   (sum 1579)
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
	li r3, 1
	bl fwd_merge            # 1 + 1 + 1000 = 1002 (fwd_merge jumps over leaf_tail to merged_tail)
	stw r3, 0(r31)
	li r3, 2
	bl adjustor_thunk       # 2 + 10, then tail branch to leaf_tail: + 100 = 112
	stw r3, 4(r31)
	li r3, 3
	bl tail_to_gapfill      # 3 + 1, then tail branch back to gapfill_leaf: + 20 = 24
	stw r3, 8(r31)
	lis r11, ptr_table@ha
	lwz r11, ptr_table@l(r11)
	mtctr r11
	li r3, 4
	bctrl                   # ptr_func(4) = 4 + 300 + 2 + 100 = 406
	stw r3, 12(r31)
	lis r11, ptr_table@ha
	addi r11, r11, ptr_table@l
	lwz r11, 4(r11)
	mtctr r11
	li r3, 5
	bctrl                   # ptr_leaf(5) = 5 * 7 = 35
	stw r3, 16(r31)
	lwz r3, 0(r31)
	lwz r4, 4(r31)
	add r3, r3, r4
	lwz r4, 8(r31)
	add r3, r3, r4
	lwz r4, 12(r31)
	add r3, r3, r4
	lwz r4, 16(r31)
	add r3, r3, r4          # return the sum (1579)
	lwz r31, 60(r1)
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# Leaf without .pdata, reached only through tail_to_gapfill's backward branch. It follows _start's
# blr, so ReXGlue's gap fill registers it; tail_to_gapfill's branch then resolves.
gapfill_leaf:
	addi r3, r3, 20
	blr

# Leaf reached by bl. Its unconditional branch jumps forward over leaf_tail; ReXGlue follows it as an
# internal branch (same code region, no prologue at the target), so merged_tail becomes part of this
# function and the code in between (leaf_tail) is in none.
fwd_merge:
	addi r3, r3, 1
	b merged_tail

# Shape A's target: a leaf only adjustor_thunk branches to.
leaf_tail:
	addi r3, r3, 100
	blr

merged_tail:
	addi r3, r3, 1000
	blr

# Shape A's thunk (reached by bl). Its branch goes backward, which ReXGlue classifies as a tail
# call; leaf_tail is not a function, so the thunk cannot be sealed:
#   b leaf_tail from adjustor_thunk+4 - target not in any function
adjustor_thunk:
	addi r3, r3, 10
	b leaf_tail

# Reached by bl; its backward tail branch targets gapfill_leaf, which is not a function when gap fill
# splits the code into runs, so the run starting here is not split after it, and gap fill skips the
# whole run (it starts at a known function), ptr_func included.
tail_to_gapfill:
	addi r3, r3, 1
	b gapfill_leaf

# Shape B: called only through ptr_table (.rodata). Odd counters take the out-of-line block after the
# blr, which branches back into the loop:
#   b ptr_back from ptr_ool+8 - target not in any function
ptr_func:
	li r4, 0
ptr_loop:
	andi. r5, r3, 1
	bne ptr_ool
	add r4, r4, r3
ptr_back:
	addi r3, r3, -1
	cmpwi r3, 0
	bne ptr_loop
	mr r3, r4
	blr
ptr_ool:
	mulli r5, r3, 100
	add r4, r4, r5
	b ptr_back
ptr_end:

# Shape C: called only through ptr_table+4. Gap fill's run that would cover it starts at ptr_ool,
# which lies inside ptr_func once B is resolved, so the whole run is skipped.
ptr_leaf:
	mulli r3, r3, 7
	blr
ptr_leaf_end:

	.section .rodata
	.align 2
ptr_table:	.long ptr_func, ptr_leaf

	.section .data
	.align 2
	.globl results
results:	.long 0, 0, 0, 0, 0
