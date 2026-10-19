# Owner checklist: identify your copy and produce the patched executable

Everything here runs on **your** PC against **your** files. Nothing from the game is uploaded or committed; the
only things to send back are the text the tools print (title IDs, versions, hashes, counts).

## What you need

* Your Tekken Tag Tournament 2 (USA, Europe) Xbox 360 disc image (`.iso`).
* Your Title Update 3 file: `TU_174Q22P_0000008000000.00000000000O2` (from your console's
  `Content\0000000000000000\4E4D0859\000B0000\` folder, or wherever your dump keeps it).
* Windows 10/11 with Visual Studio 2022 (Desktop development with C++) and git, **or** WSL/Ubuntu.

## Step 1 - build the tools (once)

Windows (PowerShell, in the repository folder):

```powershell
scripts\build.ps1 -Preset windows-msvc
# tools land in out\build\windows-msvc\bin\Release\
```

WSL / Ubuntu:

```sh
scripts/setup-ubuntu.sh && scripts/build.sh linux-clang
# tools land in out/build/linux-clang/bin/
```

## Step 2 - identify the disc image

```powershell
.\out\build\windows-msvc\bin\Release\ttt2-iso.exe identify "D:\path\to\TTT2.iso" --image-hash
```

It prints the partition layout, the title ID (expected `4E4D0859 Tekken Tag Tournament 2`), the version
(expected `0.0.0.2` for the retail disc), the SHA-1/SHA-256 of `default.xex` and of its decrypted image.
**Send the printed block.** If it says "Matches: Tekken Tag Tournament 2 (USA, Europe) retail disc ...", your copy
is the one the existing ports were built from.

## Step 3 - identify the title update

```powershell
.\out\build\windows-msvc\bin\Release\ttt2-stfs.exe identify "D:\path\to\TU_174Q22P_0000008000000.00000000000O2"
```

Expected: content type `0x000B0000 (title update)`, title ID `4E4D0859`, a `Default.xexp` whose patch versions are
`0.0.0.2 -> 0.0.3.2` and the line "Identified: Tekken Tag Tournament 2 Title Update 3". **Send the printed block.**

## Step 4 - produce the patched executable (needs Clang)

The patch tool compiles only with Clang. Two options:

**Option A - Windows with the Clang tools of Visual Studio** (install the "C++ Clang tools for Windows" component):

```powershell
scripts\fetch-xenonrecomp.ps1
scripts\build.ps1 -Preset windows-clang-cl
mkdir work
.\out\build\windows-clang-cl\bin\ttt2-iso.exe extract "D:\path\to\TTT2.iso" default.xex work\base.xex
.\out\build\windows-clang-cl\bin\ttt2-stfs.exe extract "D:\path\to\TU_174Q22P_0000008000000.00000000000O2" Default.xexp work\Default.xexp
.\out\build\windows-clang-cl\bin\ttt2-xexpatch.exe work\base.xex work\Default.xexp work\default_tu3.xex
.\out\build\windows-clang-cl\bin\ttt2-xexinfo.exe work\default_tu3.xex --image-hash
```

**Option B - WSL / Ubuntu:**

```sh
scripts/fetch-xenonrecomp.sh && scripts/build.sh linux-clang
mkdir -p work
out/build/linux-clang/bin/ttt2-iso extract /mnt/d/path/to/TTT2.iso default.xex work/base.xex
out/build/linux-clang/bin/ttt2-stfs extract "/mnt/d/path/to/TU_174Q22P_0000008000000.00000000000O2" Default.xexp work/Default.xexp
out/build/linux-clang/bin/ttt2-xexpatch work/base.xex work/Default.xexp work/default_tu3.xex
out/build/linux-clang/bin/ttt2-xexinfo work/default_tu3.xex --image-hash
```

Expected: `output: ... version 0.0.3.2`, `image: PE image, N bytes, SHA-1 ...`. **Send the printed lines.** The
`work\` folder holds your game files; `.xex`/`.xexp` are git-ignored, but keep it out of any commit anyway.

## Step 5 - (next milestone) run the recompiler

Once the hashes above are confirmed, the next instructions will cover fetching the ReXGlue SDK
(`scripts\fetch-rexglue-sdk.ps1`), building its codegen tool, and running `rexglue codegen` on
`work\default_tu3.xex` through `recomp\ttt2_manifest.toml`. The generated code stays on your PC; the report to
send back will again be counts and logs only.

## If something fails

Copy the exact error text. The tools never write game data anywhere except where you point `extract` and
`xexpatch`, so a failure leaves nothing to clean up besides the `work\` folder.
