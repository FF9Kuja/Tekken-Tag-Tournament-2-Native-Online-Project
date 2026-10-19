# License-free PowerPC test input: every instruction in recomp/ttt2_instructions.txt (the
# instructions Tekken Tag Tournament 2 needs beyond XenonRecomp's original set, per the CC0 TTT2
# Web project) in straight-line functions, so `rexglue codegen` has to translate each one.
# scripts/codegen-smoke-test.sh expects exactly two REX_UNIMPLEMENTED sites (macchwu, maclhwu:
# the SDK decodes them but has no builder; in TTT2 they only occur in data mistaken for code)
# and none once `[[invalid_instructions]]` covers them.
#
# Four instructions are written as raw words because the SDK's bundled binutils 2.24 rejects
# them (operand table limited to v0-v7 for vrlw128/vsel128; the 405 multiply-accumulates are
# not in its tables). Encodings follow the SDK's disassembler tables (thirdparty/disasm):
#   vrlw128 v7,v8,v9      VX128(6,80)  with VD128/VA128/VB128 fields   -> 0x18E84850
#   vsel128 v10,v11,v12   VX128(5,848)                                   -> 0x154B6350
#   macchwu r3,r4,r5      XO(4,140,0,0)                                  -> 0x10642918
#   maclhwu r3,r4,r5      XO(4,396,0,0)                                  -> 0x10642B18
# Our own code; no game bytes.
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	bl integer_ops
	bl update_form_loads_stores
	bl branch_and_cr_ops
	bl vmx_compare_minmax
	bl vmx_pack_shift
	bl vmx128_ops
	bl data_as_code_traps
	b .
	.size _start, .-_start

	.globl integer_ops
	.type integer_ops,@function
integer_ops:
	addc r3, r4, r5
	addme r3, r4
	subfze r3, r4
	eqv r3, r4, r5
	mulhd r3, r4, r5
	mulhdu r3, r4, r5
	dcbst 0, r3
	blr
	.size integer_ops, .-integer_ops

	.globl update_form_loads_stores
	.type update_form_loads_stores,@function
update_form_loads_stores:
	lbzux r3, r4, r5
	ldux r3, r4, r5
	lhau r3, 2(r4)
	lhzu r3, 2(r4)
	lhzux r3, r4, r5
	lwzux r3, r4, r5
	lfdu f1, 8(r4)
	lfsu f1, 4(r4)
	lfsux f1, r4, r5
	lvehx v1, r4, r5
	stbux r3, r4, r5
	stdux r3, r4, r5
	sthu r3, 2(r4)
	sthux r3, r4, r5
	stfdu f1, 8(r4)
	stfsu f1, 4(r4)
	stfsux f1, r4, r5
	blr
	.size update_form_loads_stores, .-update_form_loads_stores

	.globl branch_and_cr_ops
	.type branch_and_cr_ops,@function
branch_and_cr_ops:
	cror 0, 1, 2
	crorc 3, 4, 5
	li r3, 2
	mtctr r3
.Lloop1:
	bc 8, 2, .Lloop1
	li r3, 2
	mtctr r3
.Lloop2:
	bc 2, 2, .Lloop2
	blr
	.size branch_and_cr_ops, .-branch_and_cr_ops

	.globl vmx_compare_minmax
	.type vmx_compare_minmax,@function
vmx_compare_minmax:
	vaddsbs v1, v2, v3
	vaddsws v1, v2, v3
	vsubshs v1, v2, v3
	vsububm v1, v2, v3
	vsubuwm v1, v2, v3
	vsubuws v1, v2, v3
	vandc v1, v2, v3
	vnor v1, v2, v3
	vavguh v1, v2, v3
	vcmpequh v1, v2, v3
	vcmpgtsh v1, v2, v3
	vcmpgtsw v1, v2, v3
	vcmpgtuw v1, v2, v3
	vmaxsh v1, v2, v3
	vmaxub v1, v2, v3
	vminsh v1, v2, v3
	vminub v1, v2, v3
	vctuxs v1, v2, 3
	blr
	.size vmx_compare_minmax, .-vmx_compare_minmax

	.globl vmx_pack_shift
	.type vmx_pack_shift,@function
vmx_pack_shift:
	vpkshss v1, v2, v3
	vpkswss v1, v2, v3
	vpkuhum v1, v2, v3
	vrlh v1, v2, v3
	vsl v1, v2, v3
	vslh v1, v2, v3
	vsrab v1, v2, v3
	vsrah v1, v2, v3
	vsrh v1, v2, v3
	vspltish v1, 5
	blr
	.size vmx_pack_shift, .-vmx_pack_shift

	.globl vmx128_ops
	.type vmx128_ops,@function
vmx128_ops:
	vcfpuxws128 v1, v2, 3
	vnor128 v4, v5, v6
	vpkshss128 v1, v2, v3
	vpkswss128 v1, v2, v3
	vpkswus128 v1, v2, v3
	vpkuhus128 v1, v2, v3
	.long 0x18E84850
	.long 0x154B6350
	blr
	.size vmx128_ops, .-vmx128_ops

	.globl data_as_code_traps
	.type data_as_code_traps,@function
data_as_code_traps:
	.long 0x10642918
	.long 0x10642B18
	blr
	.size data_as_code_traps, .-data_as_code_traps
