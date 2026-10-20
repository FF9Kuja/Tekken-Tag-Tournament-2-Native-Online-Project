# License-free PowerPC test input: the file-system calls behind the owner's black screen, made through a
# real XEX import table the way the game makes them (see imports_ttt2.s for the table's layout).
#
# The game's loader asks for the disc sector of its archive files (NtQueryInformationFile with
# FileSectorInformation, class 26) and reads by raw sector through the disc device when that works.
# ReXGlue 0.10.0 answers the query with a made-up number and mounts no disc device; the host answers it
# as unsupported, as TTT2 Web does (recomp/src/io_overrides.cpp). This program opens a file of the game
# root, queries its sector and its size, reads it, closes it, then opens the bare device \Device\Cdrom0
# (the open the owner's log shows failing as "ResolvePath(\Device) failed") and asks for the attributes
# of a file that does not exist. The game root must hold io_data.bin, 16 bytes starting with "TTT2".
#
#   expected results (default):          0 0xC0000003 0 16 0 0x54545432 8 0 0xC000000F 0xC000000F
#   with TTT2_SECTOR_QUERY=runtime:      0 0          0 16 0 0x54545432 8 0 0xC000000F 0xC000000F
#     0 NtCreateFile game:\io_data.bin          STATUS_SUCCESS
#     1 sector query (class 26)                 STATUS_INVALID_INFO_CLASS (host) / success (runtime)
#     2 NtQueryInformationFile class 34         STATUS_SUCCESS (the runtime's own answer)
#     3 its end of file                         16
#     4 NtReadFile 8 bytes at offset 0          STATUS_SUCCESS
#     5 the first word read                     "TTT2"
#     6 bytes read (I/O status block)           8
#     7 NtClose                                 STATUS_SUCCESS
#     8 NtCreateFile \Device\Cdrom0             STATUS_NO_SUCH_FILE (no device there)
#     9 NtQueryFullAttributesFile game:\missing.bin   STATUS_NO_SUCH_FILE
# Assemble with the SDK's bundled powerpc-none-elf-as (scripts/make-ppc-test-xex.sh); our own code, no
# game bytes. Kernel ordinals from the public xboxkrnl.exe export table (src/xex/xbox_exports_table.inc).
	.machine "power7"
	.text

	.globl _start
	.type _start,@function
_start:
	mflr r0
	stw r0, -8(r1)
	stwu r1, -128(r1)
	stw r31, 120(r1)
	lis r31, results@ha
	addi r31, r31, results@l

	# 0: NtCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &oa_data, &iosb, NULL, 0, FILE_SHARE_READ,
	#    FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT). The ninth argument goes on the stack (0x54(r1)).
	li r0, 0x20
	stw r0, 0x54(r1)
	lis r3, handle@ha
	addi r3, r3, handle@l
	lis r4, 0x8010
	lis r5, oa_data@ha
	addi r5, r5, oa_data@l
	lis r6, iosb@ha
	addi r6, r6, iosb@l
	li r7, 0
	li r8, 0
	li r9, 1
	li r10, 1
	bl NtCreateFile_thunk
	stw r3, 0(r31)

	# 1: NtQueryInformationFile(handle, &iosb, &info, 4, FileSectorInformation)
	lis r3, handle@ha
	lwz r3, handle@l(r3)
	lis r4, iosb@ha
	addi r4, r4, iosb@l
	lis r5, info@ha
	addi r5, r5, info@l
	li r6, 4
	li r7, 26
	bl NtQueryInformationFile_thunk
	stw r3, 4(r31)

	# 2, 3: NtQueryInformationFile(handle, &iosb, &info, 56, FileNetworkOpenInformation); end of file
	lis r3, handle@ha
	lwz r3, handle@l(r3)
	lis r4, iosb@ha
	addi r4, r4, iosb@l
	lis r5, info@ha
	addi r5, r5, info@l
	li r6, 56
	li r7, 34
	bl NtQueryInformationFile_thunk
	stw r3, 8(r31)
	lis r3, info@ha
	addi r3, r3, info@l
	lwz r4, 0x2C(r3)
	stw r4, 12(r31)

	# 4, 5, 6: NtReadFile(handle, NULL, NULL, NULL, &iosb, &buffer, 8, &offset)
	lis r3, handle@ha
	lwz r3, handle@l(r3)
	li r4, 0
	li r5, 0
	li r6, 0
	lis r7, iosb@ha
	addi r7, r7, iosb@l
	lis r8, buffer@ha
	addi r8, r8, buffer@l
	li r9, 8
	lis r10, offset@ha
	addi r10, r10, offset@l
	bl NtReadFile_thunk
	stw r3, 16(r31)
	lis r3, buffer@ha
	lwz r4, buffer@l(r3)
	stw r4, 20(r31)
	lis r3, iosb@ha
	addi r3, r3, iosb@l
	lwz r4, 4(r3)
	stw r4, 24(r31)

	# 7: NtClose(handle)
	lis r3, handle@ha
	lwz r3, handle@l(r3)
	bl NtClose_thunk
	stw r3, 28(r31)

	# 8: NtCreateFile(&handle2, ..., &oa_device, ...): the disc device itself
	li r0, 0x20
	stw r0, 0x54(r1)
	lis r3, handle2@ha
	addi r3, r3, handle2@l
	lis r4, 0x8010
	lis r5, oa_device@ha
	addi r5, r5, oa_device@l
	lis r6, iosb@ha
	addi r6, r6, iosb@l
	li r7, 0
	li r8, 0
	li r9, 1
	li r10, 1
	bl NtCreateFile_thunk
	stw r3, 32(r31)

	# 9: NtQueryFullAttributesFile(&oa_missing, &info)
	lis r3, oa_missing@ha
	addi r3, r3, oa_missing@l
	lis r4, info@ha
	addi r4, r4, info@l
	bl NtQueryFullAttributesFile_thunk
	stw r3, 36(r31)

	li r3, 0
	lwz r31, 120(r1)
	addi r1, r1, 128
	lwz r0, -8(r1)
	mtlr r0
	blr
	.size _start, .-_start

# Import thunks, last in .text (see imports_ttt2.s). Ordinals: NtCreateFile 210, NtQueryInformationFile
# 232, NtReadFile 240, NtClose 207, NtQueryFullAttributesFile 231.
	.balign 4
NtCreateFile_thunk:
	.long 0x010000D2, 0, 0, 0
NtQueryInformationFile_thunk:
	.long 0x010000E8, 0, 0, 0
NtReadFile_thunk:
	.long 0x010000F0, 0, 0, 0
NtClose_thunk:
	.long 0x010000CF, 0, 0, 0
NtQueryFullAttributesFile_thunk:
	.long 0x010000E7, 0, 0, 0

	.section .rodata
	.balign 4
name_data:	.ascii "game:\\io_data.bin"
name_data_end:
name_device:	.ascii "\\Device\\Cdrom0"
name_device_end:
name_missing:	.ascii "game:\\missing.bin"
name_missing_end:
	.balign 4
# ANSI_STRING { u16 length, u16 maximum length, u32 buffer }
ansi_data:	.short name_data_end - name_data, name_data_end - name_data
		.long name_data
ansi_device:	.short name_device_end - name_device, name_device_end - name_device
		.long name_device
ansi_missing:	.short name_missing_end - name_missing, name_missing_end - name_missing
		.long name_missing
# OBJECT_ATTRIBUTES { u32 root directory, u32 name, u32 attributes (OBJ_CASE_INSENSITIVE) }
oa_data:	.long 0, ansi_data, 0x40
oa_device:	.long 0, ansi_device, 0x40
oa_missing:	.long 0, ansi_missing, 0x40
# Import address slots (type 0 records).
	.balign 4
NtCreateFile_slot:		.long 0x000000D2
NtQueryInformationFile_slot:	.long 0x000000E8
NtReadFile_slot:		.long 0x000000F0
NtClose_slot:			.long 0x000000CF
NtQueryFullAttributesFile_slot:	.long 0x000000E7

	.section .data
	.balign 8
offset:		.long 0, 0
iosb:		.long 0, 0
handle:		.long 0
handle2:	.long 0
buffer:		.long 0, 0, 0, 0
info:		.space 64
	.globl results
results:	.long 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
