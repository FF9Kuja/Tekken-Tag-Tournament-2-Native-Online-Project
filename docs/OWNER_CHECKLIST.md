# Owner checklist: identify your copy and produce the patched executable

Everything here runs on **your** PC against **your** files. Nothing from the game is uploaded or committed; the
only things to send back are the text the tools print (title IDs, versions, hashes, counts).

## What you need

* Your Tekken Tag Tournament 2 (USA, Europe) Xbox 360 disc image (`.iso`).
* Optionally your Title Update 3 file: `TU_174Q22P_0000008000000.00000000000O2` (see "Title Update 3" below
  for where it comes from). Without it everything runs on the retail executable 0.0.0.2 ("base-image mode").
* Windows 10/11 with Visual Studio 2022 (Desktop development with C++), git and Python 3, **or** WSL/Ubuntu.

## Status of your copy (2026-10-19)

`ttt2-iso identify` on your disc image, built with Visual Studio 2022: title ID `4E4D0859`, media ID `5D762D06`,
version `0.0.0.2`, normal retail AES-128-CBC encryption, basic compression, and the "Matches" line for the
USA/Europe retail disc (its `default.xex` has the SHA-1 the TTT2Recomp project published, now confirmed by your
copy). That is exactly the image TTT2 Web recompiled and the base every port starts from.

Your first full pipeline run (base-image mode, 2026-10-19) got further: identification and the import analysis
succeeded, and all eight save/restore helper addresses match TTT2 Web's, so its function boundaries apply to your
image. It then stopped at the recompilation: the pipeline handed the executable to the switch-table finder as
extracted from the disc, AES-encrypted and basic-compressed, which the finder and `rexglue codegen` do not read.
That is fixed: step 5c of the pipeline now prepares the plain form of the executable (`ttt2-xexinfo --write-plain`,
verified after writing) and every later step reads that file; a stage that fails now stops the pipeline with a
`CRITICAL:` line instead of running on with missing files. **Rerun the same command** (below). **Not yet done with
your files:** the Title Update 3 steps (you do not have the update yet), the recompilation, the host build and the
first boot; nothing beyond identification and the import/helper analysis is validated on the real game so far.

## No Title Update 3 yet? Use the base-image path

Every step below works without the update: leave `-TitleUpdate` out and the pipeline runs in base-image mode
(the report says `Mode: BASE IMAGE`), skipping steps 3 and 5 and recompiling, building and booting the retail
executable 0.0.0.2. The base game is a complete, playable image (TTT2 Web runs it to menus and fights), so the
first boot does not have to wait for the update. The TTT2 Web function boundaries were produced for this exact
image, so the pipeline's second recompilation pass with them applies ("Layout vs TTT2 Web: 8 of 8"). When you
obtain the update later, rerun the same command with `-TitleUpdate` and the report switches to the TU3 image;
online play will need both players on the same version, which is why TU3 stays the project's target.

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot
```

## Title Update 3: the legitimate way to get it

The update is Namco's copyrighted file and this project never distributes it; the only clean source is Microsoft's
own update service for a game you own:

1. Put the disc in an Xbox 360 that is signed in to Xbox Live (a free account is enough) and start the game. The
   console offers the game's update; accept it. Microsoft's store-closure FAQ says game updates keep being served
   after the July 2024 closure ("Xbox 360 games will still prompt for and install game updates when one is
   available for a game that you've launched", as relayed from support.xbox.com; check the FAQ page
   `support.xbox.com/help/xbox-360/store/xbox-360-marketplace-update` yourself before relying on it). The final
   update for this title is the one that targets 0.0.3.2.
2. Get the file off the console. It is stored on the console's storage under the title's `000B0000` content
   folder (`Content\0000000000000000\4E4D0859\000B0000\`) or, for updates named `TU_...`, in the hard drive's
   `Cache` folder. From an unmodified console the routes are the console's own Storage menu (move the item to a
   USB stick formatted by the console, then read the stick's Xbox 360 partition on the PC with a FATX-aware tool)
   or an Xbox 360 hard-drive transfer cable with such a tool. `ttt2-stfs identify <file>` tells you whether a
   candidate file is the right package ("Identified: Tekken Tag Tournament 2 Title Update 3").
3. Point `-TitleUpdate` at the file, or at a folder holding your candidate files; the pipeline picks the one whose
   `Default.xexp` targets 0.0.3.2.

Downloading the update from third-party archives is redistribution of copyrighted content and is not a route this
project documents.

## The short way: one command

`scripts\owner-pipeline.ps1` runs steps 1-4 (and 5 with `-Codegen`) and writes a text report that holds only
hashes, versions, counts, addresses and the names of the kernel/XAM functions the game imports (each marked with
whether the ReXGlue runtime implements, stubs or lacks it):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_174Q22P_0000008000000.00000000000O2"
# or point -TitleUpdate at the folder that holds your title updates; the TU3 package is picked automatically.
# The whole path to the first boot, with the SDK package downloaded and unzipped to D:\rexglue-sdk (step 6):
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_..." -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot
# the same without Title Update 3 (base-image mode, retail executable 0.0.0.2):
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot
```

**Send `work\report.txt`** (the path is printed at the end). Never send the `work\` folder. The steps below are
the same thing by hand; `ttt2-validate.exe "D:\path\TTT2.iso" "D:\path\TU_..." --work work` does steps 2-4 alone.
When a stage the later ones depend on fails, the report ends with a `CRITICAL:` line naming the cause and the
script stops there (the report is still written); fix the cause and rerun the same command.

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
"ReXGlue codegen tool (Windows)" workflow on GitHub (Actions tab, artifact `rexglue-win-amd64`; the
`rexglue-sdk-install-win-amd64` artifact of step 6 holds it too, as `bin\rexglue.exe`), or build it:

```powershell
# needs LLVM 20+ (winget install LLVM.LLVM), CMake, Ninja, Python 3 (winget install Python.Python.3.12) and
# Visual Studio 2022's C++ workload; 30-60 minutes
scripts\fetch-rexglue-sdk.ps1
scripts\build-rexglue-windows.ps1          # prints the path of rexglue.exe
```

**The recompilation image (pipeline step 5c).** The executable as extracted from the disc is AES-encrypted and
basic-compressed (`Encryption: normal`, `Compression: basic` in the report); the switch-table finder and `rexglue
codegen` read only its plain form. The pipeline prepares it with `ttt2-xexinfo --write-plain`: every header is kept
(title ID, versions, imports, page descriptors, the stored keys), only the file-format fields change to encryption
none / compression none, and the decrypted, decompressed PE image follows the headers. The written file is read
back and verified against the input byte for byte (headers and image), the original is never modified, and a
file that fails the check is deleted. The result is `work\default_base_plain.xex` (base-image mode) or
`work\default_tu3_plain.xex` (the patched image, whose patcher output is plain already, so the step only verifies
it). Steps 6 and 7 read that file; the report shows `Plain image: verified` and `Prepared: ...`. By hand:

```powershell
.\out\build\windows-clang-cl\bin\ttt2-xexinfo.exe work\base.xex --write-plain work\default_base_plain.xex
```

Then let the pipeline script do the rest (it copies the prepared image to `recomp\game\default.xex`, runs the
finder on it, runs `rexglue codegen` on `recomp\ttt2_manifest.toml`, and summarises the generated code):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_..." -Codegen -Rexglue "path\to\rexglue.exe"
```

The pipeline also compares your executable's save/restore helper addresses with the ones TTT2 Web published
("Layout vs TTT2 Web: N of 8 helper addresses match" in the report). On a full match it recompiles a second time
with TTT2 Web's 18k explicit function boundaries and writes `work\codegen-ttt2web-REPORT.md` too; send both.

Before recompiling, the pipeline runs the switch-table finder on the prepared executable (step 6a in the report)
and writes `recomp\game\ttt2_switch_tables.toml`, which the codegen manifests include: the recompiler needs these
`[[switch_tables]]` entries because SDK 0.10.0 misses or miscompiles several table shapes on its own
(`docs/RESEARCH.md` 8a). By hand, the same step is (the finder refuses the encrypted file with a message that
names the preparation step):

```powershell
python -I recomp\tools\find_switch_tables.py work\default_base_plain.xex --toml recomp\game\ttt2_switch_tables.toml --json work\switch_tables.json
```

**Send `work\report.txt`, `work\codegen-REPORT.md`, `work\codegen.log` and `work\switch_tables.json`.** They
contain function counts, addresses, names and error messages: no game bytes. `recomp\game\ttt2_switch_tables.toml`
is addresses only too and stays on your PC with the image (git-ignored). The generated C++ in
`recomp\generated\default\` stays on your PC as well (git-ignored and refused by the hygiene check).

## Step 6 - build the native host and attempt the first boot

The host is `ttt2.exe`: your recompiled code plus the ReXGlue runtime, built the way every player will build it.
It needs the SDK as an installed CMake package and the whole disc extracted into a folder, which is the
`game_data_root` the host runs from. The quick way to the package: download the `rexglue-sdk-install-win-amd64`
artifact from a run of the "ReXGlue codegen tool (Windows)" workflow that had `build_host` on (Actions tab; the
runs on `main` after each merge have it), unzip it anywhere, say `D:\rexglue-sdk`, and pass `-SdkInstall
D:\rexglue-sdk` (the pipeline then also takes `rexglue.exe` from its `bin\` folder). The slow way builds it on
your PC (30-60 minutes; `scripts\build-rexglue-windows.ps1 -Install`, or `-InstallSdk` on the pipeline):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -TitleUpdate "D:\path\TU_..." -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot
# (with -InstallSdk -Rexglue "path\to\rexglue.exe" instead of -SdkInstall to build the SDK yourself)
# by hand instead:
scripts\build-host.ps1 -Game work\default_base_plain.xex -SdkInstall D:\rexglue-sdk   # -> recomp\out\build\win-amd64-release\ttt2.exe (the prepared image of step 5c)
.\out\build\windows-clang-cl\bin\ttt2-iso.exe extract-all "D:\path\TTT2.iso" work\game
.\out\build\windows-clang-cl\bin\ttt2-stfs.exe extract-all "D:\path\TU_..." work\update
scripts\run-host.ps1                                          # ttt2.exe --game_data_root work\game, then the triage below
```

`build-host.ps1` runs the switch-table finder of step 5 itself before the build. `-ExtractGame` extracts the disc
as it is: `work\game\default.xex` stays in its encrypted, compressed form, which the host decrypts itself when it
loads it (verified in the SDK workflows: the same host boots on the plain and on the retail form of the test
program). `-ExtractGame` also extracts the title update's own files into `work\update`, which the host mounts as
`update:` because it sits next to `work\game`. `-Boot` (or `scripts\run-host.ps1` by hand) then starts the host on `work\game`, waits until it
ends and runs the triage described below. When the window stays black or freezes, close it with the X button
(the host writes its kernel stub report on the way out) and the script carries on; `-BootTimeout 60` (or
`run-host.ps1 -TimeoutSeconds 60`) closes it for you after 60 seconds.

The build folder holds everything the host needs: `ttt2.exe`, `rexruntime.dll` and the Xenos GPU plugin
`rexgpu-xenos.dll`, which the host loads by default (`--gpu_plugin=none` runs without GPU emulation, which is
only useful for log-gathering; the game cannot draw that way). The host exits by itself when the game's main
thread ends, and after a crash it writes the report below and exits with code 3 instead of hanging.

Expect the first boot to fail somewhere: the point is the log. The host writes it to the `logs\` folder next to
`ttt2.exe` (`recomp\out\build\win-amd64-release\logs\`; the window itself shows nothing useful before the game
draws): `ttt2: executable title
4E4D0859 version 0.0.3.2 ...`, `ttt2: graphics backend xenos`, an import summary, a guest-thread census every few
seconds, a **possible stall** dump when nothing changes for 15 seconds (every guest thread with its state), a
**kernel stub report** when the main thread exits or you close the window (every system call the game reached
that the runtime only stubs, with counts), and on a crash a **HOST CRASH** block (fault kind and addresses, the
guest thread, the most recent stub calls). `run-host.ps1` copies that file to `work\boot-logs\` and writes the
triage report `work\boot-triage.md` (outcome, crash block, stubs reached with where TTT2 Web implements them,
stalls) and the run summary `work\boot-report.txt` (how the host ended, exit code, duration). **Send those three
files.** By hand, the triage is:

```powershell
python -I recomp\tools\triage_boot_log.py recomp\out\build\win-amd64-release\logs --md work\boot-triage.md --quiet
```

All of them contain names, addresses and counts only. Also say what you saw on screen, if anything. Never send
`work\game`, `recomp\game`, `recomp\generated` or `recomp\out`.

## If something fails

Copy the exact error text. A `CRITICAL:` line in the report says which stage stopped the pipeline and why; the
steps above it are complete and their output is in the report. The tools never write game data anywhere except
where you point `extract`, `xexpatch` and `--write-plain`, so a failure leaves nothing to clean up besides the
`work\` folder.
