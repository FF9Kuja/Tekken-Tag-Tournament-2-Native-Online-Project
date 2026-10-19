# Owner checklist: identify your copy and produce the patched executable

Everything here runs on **your** PC against **your** files. Nothing from the game is uploaded or committed; the
only things to send back are the text the tools print (title IDs, versions, hashes, counts).

## What you need

* Your Tekken Tag Tournament 2 (USA, Europe) Xbox 360 disc image (`.iso`).
* Your Title Update 3 file: `TU_174Q22P_0000008000000.00000000000O2` (from your console's
  `Content\0000000000000000\4E4D0859\000B0000\` folder, or wherever your dump keeps it).
* Windows 10/11 with Visual Studio 2022 (Desktop development with C++) and git, **or** WSL/Ubuntu.

## The short way: one command

`scripts\owner-pipeline.ps1` runs steps 1-4 (and 5 with `-Codegen`) and writes a text report that holds only
hashes, versions, counts, addresses and the names of the kernel/XAM functions the game imports (each marked with
whether the ReXGlue runtime implements, stubs or lacks it):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_174Q22P_0000008000000.00000000000O2"
# or point -TitleUpdate at the folder that holds your title updates; the TU3 package is picked automatically
# add -Codegen once rexglue.exe exists (step 5) to also recompile and summarise the result
```

**Send `work\report.txt`** (the path is printed at the end). Never send the `work\` folder. The steps below are
the same thing by hand; `ttt2-validate.exe "D:\path\TTT2.iso" "D:\path\TU_..." --work work` does steps 2-4 alone.

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

## Step 5 - run the recompiler and report what it found

You need `rexglue.exe` (the ReXGlue codegen tool). Either download it from the latest run of the
"ReXGlue codegen tool (Windows)" workflow on GitHub (Actions tab, artifact `rexglue-win-amd64`), or build it:

```powershell
# needs LLVM 20+ (winget install LLVM.LLVM), CMake, Ninja and Visual Studio 2022's C++ workload; 30-60 minutes
scripts\fetch-rexglue-sdk.ps1
scripts\build-rexglue-windows.ps1          # prints the path of rexglue.exe
```

Then let the pipeline script do the rest (it copies `work\default_tu3.xex` to `recomp\game\default.xex`, runs
`rexglue codegen` on `recomp\ttt2_manifest.toml`, and summarises the generated code):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_..." -Codegen -Rexglue "path\to\rexglue.exe"
```

The pipeline also compares your executable's save/restore helper addresses with the ones TTT2 Web published
("Layout vs TTT2 Web: N of 8 helper addresses match" in the report). On a full match it recompiles a second time
with TTT2 Web's 18k explicit function boundaries and writes `work\codegen-ttt2web-REPORT.md` too; send both.

Also run the switch-table finder on the patched executable; it writes the `[[switch_tables]]` entries the
recompiler needs (SDK 0.10.0 misses or miscompiles several table shapes on its own, see `docs/RESEARCH.md` 8a):

```powershell
python -I recomp\tools\find_switch_tables.py work\default_tu3.xex --toml recomp\ttt2_switch_tables.toml --json work\switch_tables.json
```

**Send `work\report.txt`, `work\codegen-REPORT.md`, `work\codegen.log` and `work\switch_tables.json`.** They
contain function counts, addresses, names and error messages: no game bytes. `recomp\ttt2_switch_tables.toml`
is addresses only too and will be committed once reviewed. The generated C++ in `recomp\generated\default\`
stays on your PC (it is git-ignored and refused by the hygiene check).

## Step 6 - build the native host and attempt the first boot

The host is `ttt2.exe`: your recompiled code plus the ReXGlue runtime, built the way every player will build it.
It needs the SDK installed as a CMake package once (30-60 minutes; `scripts\build-rexglue-windows.ps1 -Install`
does it, or let the pipeline do it with `-InstallSdk`) and the whole disc extracted into a folder, which is the
`game_data_root` the host runs from:

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_..." -Codegen -Rexglue "path\to\rexglue.exe" -BuildHost -InstallSdk -ExtractGame
# by hand instead:
scripts\build-host.ps1 -Game work\default_tu3.xex            # -> recomp\out\build\win-amd64-release\ttt2.exe
.\out\build\windows-clang-cl\bin\ttt2-iso.exe extract-all "D:\path\TTT2.iso" work\game
recomp\out\build\win-amd64-release\ttt2.exe --game_data_root work\game
```

The build folder holds everything the host needs: `ttt2.exe`, `rexruntime.dll` and the Xenos GPU plugin
`rexgpu-xenos.dll`, which the host loads by default (`--gpu_plugin=none` runs without GPU emulation, which is
only useful for log-gathering; the game cannot draw that way). The host exits by itself when the game's main
thread ends, and after a crash it writes the report below and exits with code 3 instead of hanging.

Expect the first boot to fail somewhere: the point is the log. The host writes it to the `logs\` folder next to
`ttt2.exe` (`recomp\out\build\win-amd64-release\logs\`) and prints it to the console: `ttt2: executable title
4E4D0859 version 0.0.3.2 ...`, `ttt2: graphics backend xenos`, an import summary, a guest-thread census every few
seconds, a **possible stall** dump when nothing changes for 15 seconds (every guest thread with its state), a
**kernel stub report** when the main thread exits or you close the window (every system call the game reached
that the runtime only stubs, with counts), and on a crash a **HOST CRASH** block (fault kind and addresses, the
guest thread, the most recent stub calls). **Send the newest file from that `logs\` folder** together with the
triage report the next command writes from it (outcome, crash block, stubs reached with where TTT2 Web implements
them, stalls):

```powershell
python -I recomp\tools\triage_boot_log.py recomp\out\build\win-amd64-release\logs --md work\boot-triage.md --quiet
```

Both contain names, addresses and counts only. Also say what you saw on screen, if anything. Never send
`work\game`, `recomp\game`, `recomp\generated` or `recomp\out`.

## If something fails

Copy the exact error text. The tools never write game data anywhere except where you point `extract` and
`xexpatch`, so a failure leaves nothing to clean up besides the `work\` folder.
