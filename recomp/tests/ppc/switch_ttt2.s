# License-free PowerPC test input: jump-table dispatch sequences scheduled the way Tekken Tag
# Tournament 2's compiler schedules them (TTT2 Web, CC0: "lis, rlwinm, addi, lwzx" instead of
# XenonAnalyse's "lis, addi, rlwinm, lwzx"), plus the other shapes that project's finder
# handles: a conditional-return bounds check (bgtlr), an aliased index (mr), an index that is
# recomputed after the bounds check, byte- and halfword-offset tables, interleaved unrelated
# instructions, an "ori" low half, a leaf function without a .pdata entry, and a genuine
# vtable-style indirect call that must NOT be taken for a switch.
#
# scripts/codegen-smoke-test.sh records how many of these the SDK recognises on its own, then
# checks that with recomp/tools/find_switch_tables.py's [[switch_tables]] all of them become C++
# switches. recomp/tests/ppc/exec_switch.s executes the same shapes natively to prove correctness. Assemble with the SDK's bundled
# powerpc-none-elf-as (-a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128); our own code,
# no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
# Entry point. Runs every dispatch once (plus two default paths and the vtable call), stores the
# returned values in `results` (.data) and returns their sum in r3, so a native run can check them:
#   expected results: 20 102 201 301 401 500 600 701 900 800 0 9   (sum 4535)
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	lis r31, results@ha
	addi r31, r31, results@l
	li r3, 5
	bl sw_abs_ttt2_order
	stw r3, 0(r31)
	li r3, 2
	bl sw_abs_bgtlr
	stw r3, 4(r31)
	li r3, 3
	bl sw_abs_mr_alias
	stw r3, 8(r31)
	li r4, 4
	bl sw_abs_recompute
	stw r3, 12(r31)
	li r3, 1
	bl sw_byte_offset
	stw r3, 16(r31)
	li r3, 6
	bl sw_half_offset
	stw r3, 20(r31)
	li r3, 0
	bl sw_abs_interleaved
	stw r3, 24(r31)
	li r3, 7
	bl sw_abs_ori_leaf
	stw r3, 28(r31)
	li r3, 2
	bl sw_abs_table_in_ra
	stw r3, 32(r31)
	lis r3, vtable_obj@ha
	addi r3, r3, vtable_obj@l
	bl call_vtable
	stw r3, 36(r31)
	li r3, 8
	bl sw_abs_ttt2_order
	stw r3, 40(r31)
	li r3, 9
	bl sw_abs_bgtlr
	stw r3, 44(r31)
	li r3, 0
	li r4, 0
.Lsum:	lwzx r5, r4, r31
	add r3, r3, r5
	addi r4, r4, 4
	cmplwi r4, 48
	blt .Lsum
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# V1: absolute table, TTT2 scheduling (lis, rlwinm, addi, lwzx), bgt to a default label.
	.globl sw_abs_ttt2_order
	.type sw_abs_ttt2_order,@function
sw_abs_ttt2_order:
	cmplwi cr6, r3, 7
	bgt cr6, .L1_def
	lis r11, tbl1@ha
	rlwinm r10, r3, 2, 0, 29
	addi r11, r11, tbl1@l
	lwzx r11, r10, r11
	mtctr r11
	bctr
.L1_c0:	li r3, 10
	blr
.L1_c1:	li r3, 20
	blr
.L1_c2:	li r3, 30
	blr
.L1_c3:	li r3, 40
	blr
.L1_def:	li r3, 0
	blr
	.size sw_abs_ttt2_order, .-sw_abs_ttt2_order

# V2: bounds check that returns instead of branching (bgtlr), 5 cases.
	.globl sw_abs_bgtlr
	.type sw_abs_bgtlr,@function
sw_abs_bgtlr:
	cmplwi r3, 4
	bgtlr
	lis r11, tbl2@ha
	rlwinm r10, r3, 2, 0, 29
	addi r11, r11, tbl2@l
	lwzx r11, r10, r11
	mtctr r11
	bctr
.L2_c0:	li r3, 100
	blr
.L2_c1:	li r3, 101
	blr
.L2_c2:	li r3, 102
	blr
	.size sw_abs_bgtlr, .-sw_abs_bgtlr

# V3: the compare runs on a copy of the index (mr), the dispatch on the original, 6 cases.
	.globl sw_abs_mr_alias
	.type sw_abs_mr_alias,@function
sw_abs_mr_alias:
	mr r10, r3
	cmplwi r10, 5
	bgt .L3_def
	lis r11, tbl3@ha
	rlwinm r9, r3, 2, 0, 29
	addi r11, r11, tbl3@l
	lwzx r11, r9, r11
	mtctr r11
	bctr
.L3_c0:	li r3, 200
	blr
.L3_c1:	li r3, 201
	blr
.L3_def:	li r3, 0
	blr
	.size sw_abs_mr_alias, .-sw_abs_mr_alias

# V4: biased index recomputed after the bounds check (addi r11, r4, -3 twice), 4 cases.
	.globl sw_abs_recompute
	.type sw_abs_recompute,@function
sw_abs_recompute:
	addi r11, r4, -3
	cmplwi r11, 3
	bgt .L4_def
	lis r12, tbl4@ha
	addi r11, r4, -3
	rlwinm r11, r11, 2, 0, 29
	addi r12, r12, tbl4@l
	lwzx r12, r11, r12
	mtctr r12
	bctr
.L4_c0:	li r3, 300
	blr
.L4_c1:	li r3, 301
	blr
.L4_def:	li r3, 0
	blr
	.size sw_abs_recompute, .-sw_abs_recompute

# V5: byte-offset table, entries shifted by 2 and added to a base label, 4 cases.
	.globl sw_byte_offset
	.type sw_byte_offset,@function
sw_byte_offset:
	cmplwi r3, 3
	bgt .L5_def
	lis r11, tbl5@ha
	lis r9, .L5_base@ha
	addi r11, r11, tbl5@l
	lbzx r10, r3, r11
	addi r9, r9, .L5_base@l
	rlwinm r10, r10, 2, 0, 29
	add r10, r9, r10
	mtctr r10
	bctr
.L5_base:
.L5_c0:	li r3, 400
	blr
.L5_c1:	li r3, 401
	blr
.L5_c2:	li r3, 402
	blr
.L5_def:	li r3, 0
	blr
	.size sw_byte_offset, .-sw_byte_offset

# V6: halfword-offset table added to a base label, 8 cases.
	.globl sw_half_offset
	.type sw_half_offset,@function
sw_half_offset:
	cmplwi r3, 7
	bgt .L6_def
	lis r11, tbl6@ha
	rlwinm r10, r3, 1, 0, 30
	addi r11, r11, tbl6@l
	lhzx r10, r10, r11
	lis r9, .L6_base@ha
	addi r9, r9, .L6_base@l
	add r10, r9, r10
	mtctr r10
	bctr
.L6_base:
.L6_c0:	li r3, 500
	blr
.L6_c1:	li r3, 501
	blr
.L6_def:	li r3, 0
	blr
	.size sw_half_offset, .-sw_half_offset

# V7: unrelated instructions interleaved with the dispatch sequence, compare on cr7, 3 cases.
	.globl sw_abs_interleaved
	.type sw_abs_interleaved,@function
sw_abs_interleaved:
	cmplwi cr7, r3, 2
	addi r5, r5, 1
	bgt cr7, .L7_def
	lis r11, tbl7@ha
	addi r6, r6, 4
	rlwinm r10, r3, 2, 0, 29
	ori r7, r7, 1
	addi r11, r11, tbl7@l
	lwzx r11, r10, r11
	mr r8, r5
	mtctr r11
	bctr
.L7_c0:	li r3, 600
	blr
.L7_c1:	li r3, 601
	blr
.L7_c2:	li r3, 602
	blr
.L7_def:	li r3, 0
	blr
	.size sw_abs_interleaved, .-sw_abs_interleaved

# V8: table address formed with ori instead of addi, in a leaf function WITHOUT a .pdata
# entry (no .size directive, so the packer emits none); 8 cases. The recompiler must find the
# function through the call from _start and size it past the table dispatch on its own.
	.globl sw_abs_ori_leaf
	.type sw_abs_ori_leaf,@function
sw_abs_ori_leaf:
	cmplwi r3, 7
	bgt .L8_def
	lis r11, tbl8@h
	rlwinm r10, r3, 2, 0, 29
	ori r11, r11, tbl8@l
	lwzx r11, r10, r11
	mtctr r11
	bctr
.L8_c0:	li r3, 700
	blr
.L8_c1:	li r3, 701
	blr
.L8_def:	li r3, 0
	blr
	.long 0, 0

# V9: like V1 but with the lwzx operands the other way round (table in rA, scaled index in rB),
# the order ReXGlue's detector assumes; 4 cases. Both orders are legal compiler output.
	.globl sw_abs_table_in_ra
	.type sw_abs_table_in_ra,@function
sw_abs_table_in_ra:
	cmplwi r3, 3
	bgt .L9_def
	lis r11, tbl9@ha
	rlwinm r10, r3, 2, 0, 29
	addi r11, r11, tbl9@l
	lwzx r11, r11, r10
	mtctr r11
	bctr
.L9_c0:	li r3, 900
	blr
.L9_c1:	li r3, 901
	blr
.L9_def:	li r3, 0
	blr
	.size sw_abs_table_in_ra, .-sw_abs_table_in_ra

# Not a switch: a vtable-style indirect call through a pointer loaded from the object.
	.globl call_vtable
	.type call_vtable,@function
call_vtable:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -32(r1)
	lwz r11, 0(r3)
	lwz r11, 4(r11)
	mtctr r11
	bctrl
	addi r1, r1, 32
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size call_vtable, .-call_vtable

	.globl vtable_method
	.type vtable_method,@function
vtable_method:
	li r3, 800
	blr
	.size vtable_method, .-vtable_method

	.section .rodata
	.align 2
tbl1:	.long .L1_c0, .L1_c1, .L1_c2, .L1_c3, .L1_c0, .L1_c1, .L1_c2, .L1_c3
tbl2:	.long .L2_c0, .L2_c1, .L2_c2, .L2_c0, .L2_c1
tbl3:	.long .L3_c0, .L3_c1, .L3_c0, .L3_c1, .L3_c0, .L3_c1
tbl4:	.long .L4_c0, .L4_c1, .L4_c0, .L4_c1
tbl5:	.byte (.L5_c0-.L5_base)>>2, (.L5_c1-.L5_base)>>2, (.L5_c2-.L5_base)>>2, (.L5_c0-.L5_base)>>2
	.align 2
tbl6:	.short .L6_c0-.L6_base, .L6_c1-.L6_base, .L6_c0-.L6_base, .L6_c1-.L6_base
	.short .L6_c0-.L6_base, .L6_c1-.L6_base, .L6_c0-.L6_base, .L6_c1-.L6_base
tbl7:	.long .L7_c0, .L7_c1, .L7_c2
tbl8:	.long .L8_c0, .L8_c1, .L8_c0, .L8_c1, .L8_c0, .L8_c1, .L8_c0, .L8_c1
tbl9:	.long .L9_c0, .L9_c1, .L9_c0, .L9_c1
	.align 2
vtable:	.long 0, vtable_method
vtable_obj:	.long vtable

	.section .data
	.align 2
	.globl results
results:	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
