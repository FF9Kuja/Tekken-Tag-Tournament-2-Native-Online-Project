# License-free PowerPC test input: switch dispatch shapes that recomp/tools/find_switch_tables.py
# follows beyond switch_ttt2.s, because its symbolic execution only uses exact facts:
#   S1 the table address set before the bounds check       S6 clrldi / sldi (64-bit forms)
#   S2 a `bge` bounds check (max entries, not max + 1)      S7 a byte index table into a jump table
#   S3 a bias folded into the table address                  S8 signed halfword offsets (lhax), cases
#   S4 a byte index masked and scaled (clrlslwi)                before and after the dispatch
#   S5 `add` then a D-form `lwz`                              S9 the index range from a mask (clrlwi)
# and S10, a dispatch another path enters with a different table: the finder must report it, not
# guess (never called here). scripts/codegen-smoke-test.sh recompiles this with the finder's
# [[switch_tables]], checks the audit (recomp/tools/audit_switches.py) and, with --exec, runs it.
# Assemble with the SDK's bundled powerpc-none-elf-as; our own code, no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
# Runs the dispatches, stores the returned values at `results` and returns their sum:
#   expected results: 13 24 32 44 51 62 71 80 83 92 0   (sum 552)
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -64(r1)
	lis r31, results@ha
	addi r31, r31, results@l
	li r3, 3
	bl sh_early_table
	stw r3, 0(r31)
	li r3, 4
	bl sh_bge
	stw r3, 4(r31)
	li r3, 12
	bl sh_bias_fold
	stw r3, 8(r31)
	lis r3, byte_in@ha
	addi r3, r3, byte_in@l
	bl sh_byte_clrlslwi
	stw r3, 12(r31)
	li r3, 1
	bl sh_add_lwz
	stw r3, 16(r31)
	li r3, 2
	bl sh_sldi_clrldi
	stw r3, 20(r31)
	li r3, 7
	bl sh_two_level
	stw r3, 24(r31)
	li r3, 0
	bl sh_signed_half
	stw r3, 28(r31)
	li r3, 3
	bl sh_signed_half
	stw r3, 32(r31)
	li r3, 6
	bl sh_mask
	stw r3, 36(r31)
	li r3, 9
	bl sh_bge
	stw r3, 40(r31)
	li r3, 0
	li r4, 0
.Lsum:	lwzx r5, r4, r31
	add r3, r3, r5
	addi r4, r4, 4
	cmplwi r4, 44
	blt .Lsum
	addi r1, r1, 64
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# S1: the table address is loaded before the bounds check, 5 entries.
	.globl sh_early_table
	.type sh_early_table,@function
sh_early_table:
	lis r11, tbl_s1@ha
	addi r11, r11, tbl_s1@l
	cmplwi cr6, r3, 4
	bgt cr6, .LS1_def
	rlwinm r10, r3, 2, 0, 29
	lwzx r11, r10, r11
	mtctr r11
	bctr
.LS1_c0:	li r3, 10
	blr
.LS1_c1:	li r3, 11
	blr
.LS1_c2:	li r3, 12
	blr
.LS1_c3:	li r3, 13
	blr
.LS1_def:	li r3, 0
	blr
	.size sh_early_table, .-sh_early_table

# S2: `bge` against the number of cases, 5 entries (an index of 5 or more takes the default).
	.globl sh_bge
	.type sh_bge,@function
sh_bge:
	cmplwi cr7, r3, 5
	bge cr7, .LS2_def
	lis r11, tbl_s2@ha
	slwi r10, r3, 2
	addi r11, r11, tbl_s2@l
	lwzx r11, r11, r10
	mtctr r11
	bctr
.LS2_c0:	li r3, 20
	blr
.LS2_c1:	li r3, 21
	blr
.LS2_c2:	li r3, 22
	blr
.LS2_c3:	li r3, 23
	blr
.LS2_c4:	li r3, 24
	blr
.LS2_def:	li r3, 0
	blr
	.size sh_bge, .-sh_bge

# S3: the compare runs on r3 - 10, the dispatch indexes with r3 and a table address 40 bytes lower.
	.globl sh_bias_fold
	.type sh_bias_fold,@function
sh_bias_fold:
	addi r11, r3, -10
	cmplwi cr6, r11, 3
	bgt cr6, .LS3_def
	lis r12, (tbl_s3-40)@ha
	slwi r10, r3, 2
	addi r12, r12, (tbl_s3-40)@l
	lwzx r12, r10, r12
	mtctr r12
	bctr
.LS3_c0:	li r3, 30
	blr
.LS3_c1:	li r3, 31
	blr
.LS3_c2:	li r3, 32
	blr
.LS3_c3:	li r3, 33
	blr
.LS3_def:	li r3, 0
	blr
	.size sh_bias_fold, .-sh_bias_fold

# S4: a byte read through the argument, bounds-checked, then masked and scaled in one rlwinm.
	.globl sh_byte_clrlslwi
	.type sh_byte_clrlslwi,@function
sh_byte_clrlslwi:
	lbz r11, 0(r3)
	cmplwi cr6, r11, 5
	bgt cr6, .LS4_def
	lis r12, tbl_s4@ha
	rlwinm r10, r11, 2, 22, 29
	addi r12, r12, tbl_s4@l
	lwzx r12, r10, r12
	mtctr r12
	bctr
.LS4_c0:	li r3, 40
	blr
.LS4_c1:	li r3, 41
	blr
.LS4_c4:	li r3, 44
	blr
.LS4_c5:	li r3, 45
	blr
.LS4_def:	li r3, 0
	blr
	.size sh_byte_clrlslwi, .-sh_byte_clrlslwi

# S5: the entry address is computed with `add`, then loaded with a D-form lwz.
	.globl sh_add_lwz
	.type sh_add_lwz,@function
sh_add_lwz:
	cmplwi cr6, r3, 2
	bgt cr6, .LS5_def
	slwi r10, r3, 2
	lis r11, tbl_s5@ha
	addi r11, r11, tbl_s5@l
	add r10, r10, r11
	lwz r11, 0(r10)
	mtctr r11
	bctr
.LS5_c0:	li r3, 50
	blr
.LS5_c1:	li r3, 51
	blr
.LS5_c2:	li r3, 52
	blr
.LS5_def:	li r3, 0
	blr
	.size sh_add_lwz, .-sh_add_lwz

# S6: the index zero-extended with clrldi and scaled with sldi.
	.globl sh_sldi_clrldi
	.type sh_sldi_clrldi,@function
sh_sldi_clrldi:
	clrldi r9, r3, 32
	cmplwi cr6, r9, 3
	bgt cr6, .LS6_def
	sldi r10, r9, 2
	lis r11, tbl_s6@ha
	addi r11, r11, tbl_s6@l
	lwzx r11, r11, r10
	mtctr r11
	bctr
.LS6_c0:	li r3, 60
	blr
.LS6_c1:	li r3, 61
	blr
.LS6_c2:	li r3, 62
	blr
.LS6_c3:	li r3, 63
	blr
.LS6_def:	li r3, 0
	blr
	.size sh_sldi_clrldi, .-sh_sldi_clrldi

# S7: a byte index table selects an entry of a jump table (10 cases, 3 targets).
	.globl sh_two_level
	.type sh_two_level,@function
sh_two_level:
	cmplwi cr6, r3, 9
	bgt cr6, .LS7_def
	lis r11, idx_s7@ha
	addi r11, r11, idx_s7@l
	lbzx r11, r3, r11
	lis r10, jt_s7@ha
	slwi r11, r11, 2
	addi r10, r10, jt_s7@l
	lwzx r11, r10, r11
	mtctr r11
	bctr
.LS7_c0:	li r3, 70
	blr
.LS7_c1:	li r3, 71
	blr
.LS7_c2:	li r3, 72
	blr
.LS7_def:	li r3, 0
	blr
	.size sh_two_level, .-sh_two_level

# S8: signed halfword offsets from the dispatch code; two cases sit before it.
	.globl sh_signed_half
	.type sh_signed_half,@function
sh_signed_half:
	b .LS8_start
.LS8_c0:	li r3, 80
	blr
.LS8_c1:	li r3, 81
	blr
.LS8_start:
	cmplwi cr6, r3, 3
	bgt cr6, .LS8_def
	lis r11, tbl_s8@ha
	slwi r10, r3, 1
	addi r11, r11, tbl_s8@l
	lhax r11, r11, r10
	lis r12, .LS8_start@ha
	addi r12, r12, .LS8_start@l
	add r11, r11, r12
	mtctr r11
	bctr
.LS8_c2:	li r3, 82
	blr
.LS8_c3:	li r3, 83
	blr
.LS8_def:	li r3, 0
	blr
	.size sh_signed_half, .-sh_signed_half

# S9: no compare; the index range is the mask (r3 & 3), scaled in place.
	.globl sh_mask
	.type sh_mask,@function
sh_mask:
	clrlwi r11, r3, 30
	lis r12, tbl_s9@ha
	slwi r11, r11, 2
	addi r12, r12, tbl_s9@l
	lwzx r12, r12, r11
	mtctr r12
	bctr
.LS9_c0:	li r3, 90
	blr
.LS9_c1:	li r3, 91
	blr
.LS9_c2:	li r3, 92
	blr
.LS9_c3:	li r3, 93
	blr
	.size sh_mask, .-sh_mask

# S10 (never called): .LS10_other enters the dispatch after the table address was set, with another
# table. A table for this bctr would be wrong on that path, so the finder must report the site.
	.globl sh_shared_tail
	.type sh_shared_tail,@function
sh_shared_tail:
	cmplwi cr6, r3, 3
	bgt cr6, .LS10_def
	lis r11, tbl_s10@ha
	addi r11, r11, tbl_s10@l
.LS10_join:
	slwi r10, r3, 2
	lwzx r11, r11, r10
	mtctr r11
	bctr
.LS10_c0:	li r3, 100
	blr
.LS10_c1:	li r3, 101
	blr
.LS10_def:	li r3, 0
	blr
.LS10_other:
	lis r11, tbl_s10b@ha
	addi r11, r11, tbl_s10b@l
	li r3, 1
	b .LS10_join
	.size sh_shared_tail, .-sh_shared_tail

	.section .rodata
	.balign 4
tbl_s1:	.long .LS1_c0, .LS1_c1, .LS1_c2, .LS1_c3, .LS1_c1
tbl_s2:	.long .LS2_c0, .LS2_c1, .LS2_c2, .LS2_c3, .LS2_c4
tbl_s3:	.long .LS3_c0, .LS3_c1, .LS3_c2, .LS3_c3
tbl_s4:	.long .LS4_c0, .LS4_c1, .LS4_c0, .LS4_c1, .LS4_c4, .LS4_c5
tbl_s5:	.long .LS5_c0, .LS5_c1, .LS5_c2
tbl_s6:	.long .LS6_c0, .LS6_c1, .LS6_c2, .LS6_c3
jt_s7:	.long .LS7_c0, .LS7_c1, .LS7_c2
tbl_s8:	.short .LS8_c0-.LS8_start, .LS8_c1-.LS8_start, .LS8_c2-.LS8_start, .LS8_c3-.LS8_start
	.balign 4
tbl_s9:	.long .LS9_c0, .LS9_c1, .LS9_c2, .LS9_c3
tbl_s10:	.long .LS10_c0, .LS10_c1, .LS10_c0, .LS10_c1
tbl_s10b:	.long .LS10_c1, .LS10_c0, .LS10_c1, .LS10_c0
idx_s7:	.byte 0, 1, 2, 0, 2, 2, 0, 1, 1, 0
byte_in:	.byte 4

	.section .data
	.balign 4
	.globl results
results:	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
