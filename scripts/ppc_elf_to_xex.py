#!/usr/bin/env python3
"""Wraps a statically linked 32-bit big-endian PowerPC ELF (built with Clang/LLD) into a minimal
XEX2 container around a PE32 image, so that `rexglue codegen` can analyse it.

Purpose: license-free recompiler smoke tests. Real Xbox 360 executables carry a PE32 image with a
`.pdata` exception directory that the SDK's Register phase requires; a plain ELF has none. This
tool lays the ELF's allocated sections out as PE sections at their RVAs (image base 0x82000000),
synthesises `.pdata` from the ELF's FUNC symbols, and writes unencrypted, uncompressed XEX2 headers.

    python3 scripts/ppc_elf_to_xex.py <input.elf> <output.xex> [--title-id 54455354] [--imports FILE]

Link the ELF with: ld.lld -m elf32ppc --image-base=0x82000000 -z separate-code -z max-page-size=0x10000
so that no section lands in the first 64 KiB (reserved for the PE headers).

--imports FILE adds an IMPORT_LIBRARIES header so the image calls kernel exports the way a real
executable does. Each line of FILE names a library and the ELF symbols of its import records:

    xam.xex: XamShowFriendsUI_slot, XamShowFriendsUI_thunk

The program itself lays the records out: a 4-byte slot (type 0) in a data section and a 16-byte
thunk (type 1) at the END of .text, both starting with the record word (type << 24 | ordinal); the
codegen turns `bl thunk` into a call of the host's __imp__<Name> and treats the thunk range as
non-code, as it does for the real game. No game content is involved; the output is entirely
derived from the test program.
"""
import argparse
import struct
import sys

IMAGE_BASE = 0x82000000
PAGE = 0x10000           # the SDK's heap at 0x82000000 uses 64 KiB pages
SECTION_ALIGN = 0x1000

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_STRTAB = 1, 2, 8, 3
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 0x1, 0x2, 0x4
STT_FUNC = 2


def read_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise SystemExit("input is not a 32-bit big-endian ELF")
    e_machine = struct.unpack_from(">H", data, 18)[0]
    if e_machine != 20:
        raise SystemExit(f"ELF machine {e_machine} is not PowerPC (20)")
    e_entry, _, e_shoff = struct.unpack_from(">III", data, 24)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", data, 46)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info,
         sh_addralign, sh_entsize) = struct.unpack_from(">10I", data, off)
        sections.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                             offset=sh_offset, size=sh_size, link=sh_link, entsize=sh_entsize))
    shstr = sections[e_shstrndx]
    names = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
    for s in sections:
        end = names.index(b"\0", s["name_off"])
        s["name"] = names[s["name_off"]:end].decode()
    funcs = []
    symbols = []  # every named FUNC/OBJECT/NOTYPE symbol with an address: (address, size, kind, name)
    for s in sections:
        if s["type"] != SHT_SYMTAB:
            continue
        strtab = sections[s["link"]]
        strs = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        for k in range(s["size"] // 16):
            st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack_from(">IIIBBH", data, s["offset"] + k * 16)
            end = strs.index(b"\0", st_name)
            name = strs[st_name:end].decode()
            kind = st_info & 0xF
            if kind == STT_FUNC and st_size > 0:
                funcs.append((st_value, st_size, name))
            if name and st_shndx not in (0, 0xFFF1) and kind in (0, 1, 2):
                symbols.append((st_value, st_size, {0: "NOTYPE", 1: "OBJECT", 2: "FUNC"}[kind], name))
    read_elf.symbols = sorted(symbols)
    return e_entry, sections, sorted(funcs)


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def build_image(entry, sections, funcs):
    alloc = [s for s in sections if s["flags"] & SHF_ALLOC and s["size"] > 0 and s["name"] not in (".comment",)]
    for s in alloc:
        if s["addr"] < IMAGE_BASE + PAGE:
            raise SystemExit(f"section {s['name']} at 0x{s['addr']:08X} overlaps the PE header page; "
                             "link with --image-base=0x82000000 -z separate-code -z max-page-size=0x10000")
    pe_sections = []
    image_end = 0
    for s in alloc:
        rva = s["addr"] - IMAGE_BASE
        if s["flags"] & SHF_EXECINSTR:
            chars, pe_name = 0x60000020, b".text"
        elif s["type"] == SHT_NOBITS:
            chars, pe_name = 0xC0000080, b".bss"
        elif s["flags"] & SHF_WRITE:
            chars, pe_name = 0xC0000040, b".data"
        else:
            chars, pe_name = 0x40000040, b".rdata"
        pe_sections.append(dict(name=pe_name, rva=rva, vsize=s["size"], chars=chars, src=s))
        image_end = max(image_end, rva + s["size"])
    # .pdata after everything else.
    pdata_rva = align(image_end, SECTION_ALIGN)
    pdata = bytearray()
    for addr, size, _ in funcs:
        length_instr = max(1, (size + 3) // 4)
        data = (0 & 0xFF) | ((length_instr & 0x3FFFFF) << 8) | (1 << 30)  # PrologLength 0, 32-bit, no SEH
        pdata += struct.pack(">II", addr, data)
    pe_sections.append(dict(name=b".pdata", rva=pdata_rva, vsize=len(pdata), chars=0x40000040, src=None, raw=bytes(pdata)))
    image_size = align(pdata_rva + len(pdata), PAGE)

    image = bytearray(image_size)
    # DOS header: the loader checks the first dword against "MZ\x90\x00" (e_magic + e_cblp as in
    # every MSVC-linked executable), not just "MZ".
    struct.pack_into("<2sH", image, 0, b"MZ", 0x0090)
    struct.pack_into("<I", image, 0x3C, 0x40)
    nt = 0x40
    struct.pack_into("<4s", image, nt, b"PE\0\0")
    text = next(s for s in pe_sections if s["name"] == b".text")
    # FileHeader: Machine POWERPCBE, sections, timestamp, symtab, nsyms, optional size, characteristics
    struct.pack_into("<HHIIIHH", image, nt + 4, 0x01F2, len(pe_sections), 0, 0, 0, 0xE0, 0x0102)
    opt = nt + 24
    size_of_code = sum(s["vsize"] for s in pe_sections if s["chars"] & 0x20)
    size_of_idata = sum(s["vsize"] for s in pe_sections if s["chars"] & 0x40)
    size_of_udata = sum(s["vsize"] for s in pe_sections if s["chars"] & 0x80)
    struct.pack_into("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", image, opt,
                     0x10B, 8, 0,                       # Magic PE32, linker version
                     size_of_code, size_of_idata, size_of_udata,
                     entry - IMAGE_BASE,                 # AddressOfEntryPoint (RVA)
                     text["rva"], 0,                     # BaseOfCode, BaseOfData
                     IMAGE_BASE, SECTION_ALIGN, SECTION_ALIGN,
                     0, 0, 0, 0, 0, 0,                   # OS / image / subsystem versions
                     0, image_size, SECTION_ALIGN, 0,    # Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum
                     14, 0,                              # Subsystem XBOX, DllCharacteristics
                     0x40000, 0x4000, 0x100000, 0x1000,  # stack / heap reserve & commit
                     0, 16)                              # LoaderFlags, NumberOfRvaAndSizes
    datadir = opt + 96
    struct.pack_into("<II", image, datadir + 3 * 8, pdata_rva, len(pdata))  # EXCEPTION directory
    sect = opt + 0xE0
    for i, s in enumerate(pe_sections):
        raw = s["raw"] if s["src"] is None else None
        raw_size = 0 if (s["src"] is not None and s["src"]["type"] == SHT_NOBITS) else s["vsize"]
        struct.pack_into("<8sIIIIIIHHI", image, sect + i * 40, s["name"].ljust(8, b"\0"), s["vsize"], s["rva"],
                         raw_size, s["rva"] if raw_size else 0, 0, 0, 0, 0, s["chars"])
    if sect + len(pe_sections) * 40 > SECTION_ALIGN:
        raise SystemExit("too many sections for the header page")
    return image, pe_sections, pdata_rva, len(pdata)


def copy_sections(image, pe_sections, elf):
    for s in pe_sections:
        if s["src"] is None:
            image[s["rva"]:s["rva"] + len(s["raw"])] = s["raw"]
        elif s["src"]["type"] != SHT_NOBITS:
            src = s["src"]
            image[s["rva"]:s["rva"] + src["size"]] = elf[src["offset"]:src["offset"] + src["size"]]


def read_imports_file(path):
    """Parses `library: symbol, symbol, ...` lines (# comments) into [(library, [symbols])]."""
    libs = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if ":" not in line:
                raise SystemExit(f"{path}:{lineno}: expected 'library: symbol, symbol'")
            lib, syms = line.split(":", 1)
            names = [n.strip() for n in syms.split(",") if n.strip()]
            if not lib.strip() or not names:
                raise SystemExit(f"{path}:{lineno}: expected 'library: symbol, symbol'")
            libs.append((lib.strip(), names))
    return libs


def build_import_libraries(libs, kernel_version=0x20449700):
    """The IMPORT_LIBRARIES optional header (key 0x000103FF): size, string table (4-byte padded
    names), then one entry per library (size, digest, id, version, version_min, name index, record
    count, record addresses). `libs` is [(name, [record_address, ...])]. The version is the kernel
    build the records were made for (2.0.17559.0, the last Xbox 360 dashboard kernel)."""
    strings = bytearray()
    for name, _ in libs:
        strings += name.encode() + b"\0"
        while len(strings) % 4:
            strings += b"\0"
    body = bytearray()
    for index, (name, records) in enumerate(libs):
        entry = struct.pack(">I", 0x28 + 4 * len(records)) + b"\0" * 0x14
        entry += struct.pack(">IIIHH", 0, kernel_version, kernel_version, index, len(records))
        entry += b"".join(struct.pack(">I", a) for a in records)
        body += entry
    total = 12 + len(strings) + len(body)
    return struct.pack(">III", total, len(strings), len(libs)) + bytes(strings) + bytes(body)


def resolve_imports(libs, symbols):
    """Turns [(library, [symbol names])] into [(library, [addresses])] with the ELF's symbols."""
    by_name = {}
    for addr, _, _, name in symbols:
        by_name.setdefault(name, addr)
    out = []
    for lib, names in libs:
        addrs = []
        for n in names:
            if n not in by_name:
                raise SystemExit(f"import record symbol {n} ({lib}) not found in the ELF")
            addrs.append(by_name[n])
        out.append((lib, addrs))
    return out


def build_xex(image, entry, title_id, pe_sections, import_libs=()):
    # Page descriptors (64 KiB pages): code pages for .text, data for the rest.
    pages = len(image) // PAGE
    text = next(s for s in pe_sections if s["name"] == b".text")
    text_first = text["rva"] // PAGE
    text_last = (text["rva"] + text["vsize"] - 1) // PAGE
    descriptors = []
    for p in range(pages):
        kind = 1 if text_first <= p <= text_last else 2
        if descriptors and descriptors[-1][0] == kind:
            descriptors[-1][1] += 1
        else:
            descriptors.append([kind, 1])
    sec_size = 0x184 + len(descriptors) * 0x18

    opt = []  # (key, inline value or bytes)
    opt.append((0x00010100, entry))                   # ENTRY_POINT
    opt.append((0x00010201, IMAGE_BASE))              # IMAGE_BASE_ADDRESS
    opt.append((0x00010001, IMAGE_BASE))              # ORIGINAL_BASE_ADDRESS
    opt.append((0x00020200, 0x40000))                 # DEFAULT_STACK_SIZE
    opt.append((0x00030000, 0))                       # SYSTEM_FLAGS
    opt.append((0x000003FF, struct.pack(">IHH", 8, 0, 0)))                      # FILE_FORMAT_INFO none/none
    opt.append((0x00040006, struct.pack(">IIIIBBBBI", 0, 0, 0, title_id, 0xFF, 2, 1, 1, 0)))  # EXECUTION_INFO
    opt.append((0x00020104, struct.pack(">IIII", 0, 0, 0, 0)))                  # TLS_INFO
    if import_libs:
        opt.append((0x000103FF, build_import_libraries(import_libs)))           # IMPORT_LIBRARIES
    header = bytearray(0x18 + len(opt) * 8)
    payloads = bytearray()
    entries = []
    for key, val in opt:
        if isinstance(val, int):
            entries.append((key, val))
        else:
            off = len(header) + len(payloads)
            payloads += val
            while len(payloads) % 4:
                payloads += b"\0"
            entries.append((key, off))
    sec_off = len(header) + len(payloads)
    header_size = align(sec_off + sec_size, SECTION_ALIGN)
    out = bytearray(header_size)
    struct.pack_into(">4sIIIII", out, 0, b"XEX2", 0x01, header_size, 0, sec_off, len(entries))
    for i, (key, val) in enumerate(entries):
        struct.pack_into(">II", out, 0x18 + i * 8, key, val)
    out[len(header):len(header) + len(payloads)] = payloads
    s = sec_off
    struct.pack_into(">II", out, s, sec_size, len(image))
    struct.pack_into(">I", out, s + 0x10C, 0)                 # image flags: 64 KiB pages
    struct.pack_into(">I", out, s + 0x110, IMAGE_BASE)
    struct.pack_into(">I", out, s + 0x178, 0xFFFFFFFF)        # region
    struct.pack_into(">I", out, s + 0x17C, 0xFFFFFFFF)        # media types
    struct.pack_into(">I", out, s + 0x180, len(descriptors))
    for i, (kind, count) in enumerate(descriptors):
        struct.pack_into(">I", out, s + 0x184 + i * 0x18, (count << 4) | kind)
    return bytes(out) + bytes(image)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--title-id", default="54455354", help="hex title ID for the execution info (default 'TEST')")
    ap.add_argument("--symbols", help="also write every symbol as 'ADDRESS SIZE KIND NAME' lines to this file")
    ap.add_argument("--imports", metavar="FILE", help="import libraries and their record symbols (see above)")
    args = ap.parse_args()
    with open(args.elf, "rb") as f:
        elf = f.read()
    entry, sections, funcs = read_elf(elf)
    image, pe_sections, pdata_rva, pdata_size = build_image(entry, sections, funcs)
    copy_sections(image, pe_sections, elf)
    import_libs = resolve_imports(read_imports_file(args.imports), read_elf.symbols) if args.imports else []
    xex = build_xex(image, entry, int(args.title_id, 16), pe_sections, import_libs)
    with open(args.out, "wb") as f:
        f.write(xex)
    if args.symbols:
        with open(args.symbols, "w", newline="\n") as f:  # LF on Windows too: shell tools read it
            for addr, size, kind, name in read_elf.symbols:
                f.write(f"0x{addr:08X} {size} {kind} {name}\n")
    print(f"{args.out}: image {len(image)} bytes, {len(pe_sections)} sections, {len(funcs)} functions in .pdata "
          f"(RVA 0x{pdata_rva:X}, {pdata_size} bytes), entry 0x{entry:08X}")
    for addr, size, name in funcs:
        print(f"  0x{addr:08X} {size:6d} {name}")
    for lib, addrs in import_libs:
        print(f"  imports {lib}: {len(addrs)} records at " + ", ".join(f"0x{a:08X}" for a in addrs))


if __name__ == "__main__":
    sys.exit(main())
