# Owner checklist: identify your copy and produce the patched executable

Everything here runs on **your** PC against **your** files. Nothing from the game is uploaded or committed; the
only things to send back are the text the tools print (title IDs, versions, hashes, counts).

## What you need

* Your Tekken Tag Tournament 2 (USA, Europe) Xbox 360 disc image (`.iso`).
* Optionally your Title Update 3 file: `TU_174Q22P_0000008000000.00000000000O2` (see "Title Update 3" below
  for where it comes from). Without it everything runs on the retail executable 0.0.0.2 ("base-image mode").
* Windows 10/11 with Visual Studio 2022 (Desktop development with C++), git and Python 3, **or** WSL/Ubuntu.

## Status of your copy (2026-10-20, updated after the third black-screen run)

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
`CRITICAL:` line instead of running on with missing files.

Your second run (with that fix) prepared and verified the plain image, found 596 switch tables, and got ReXGlue
through analysing the whole executable: 49,708 functions sealed, 13 not, and 14 `UnresolvedCall: target not in any
function` errors stopped it. Cause (`docs/RESEARCH.md` 8b): the pinned ReXGlue registers a function for every `bl`
target but never for the target of a plain `b`, and its gap fill misses the code right after a function's last
branch. Thirteen of the 14 targets are small functions only a thunk branches to; TTT2 Web's published table lists
each of them as a function start. The fourteenth is a block ReXGlue cut off its function; TTT2 Web's table has
branch and target in one function. The pipeline now runs codegen through `recomp\tools\resolve_functions.py`,
which adds such a function only after checking the branch in your image and finding the same boundary in TTT2
Web's table (used only because your helper fingerprint matches it 8 of 8), then reruns codegen. On your log it adds
13 entry points and one function boundary, nothing else. When a boot later stops on a call to an unregistered
function (the runtime logs `[FATAL] Call to invalid or unregistered function at guest address ...`), the pipeline
verifies that address the same way, rebuilds and boots again (step 9b; it now repeats by itself, see below).
Your run also reported 18 switch dispatches the finder could not read. Any `bctr` the finder leaves is handled
by ReXGlue's own jump-table detector, which on our test program switches on the wrong register in 6 of its 7
tables (the game would trap there), or becomes an indirect call that traps if the game reaches it. Before your
rerun the finder was rebuilt to follow the dispatch code exactly (`docs/RESEARCH.md` 8c): more shapes (the table
address loaded before the bounds check, a bias folded into the table address, masked or 64-bit index arithmetic,
`add` + `lwz`, two-level tables, `bge` bounds), a complete model of which registers each instruction writes, and a
check that no other path into the dispatch reads a different table. It still never guesses. A new step 6c audits
how the codegen emitted every `bctr` of your executable and writes `work\switch-audit.md`: the finder's tables as
written, every switch only ReXGlue's detector found with a verdict (certain miscompile, consistent, or unverified),
and the dispatches the finder still cannot read, each with its few instructions so the next fix can be exact.
**Rerun the same command** (below). **Not yet done with your files:** the Title Update 3 steps (you do not have the
update yet), a complete recompilation, the host build and the first boot; validated on the real game so far:
identification, the import/helper analysis, the plain image, the switch-table finder and ReXGlue's analysis up to
those 14 errors.

Your latest runs (2026-10-20) compiled the whole game, built `ttt2.exe` and reached native execution. Each boot ran
until the game called a function only a pointer reaches (`[FATAL] Call to invalid or unregistered function at guest
address ...`); step 9b verified the address against TTT2 Web's table, added it and rebuilt. With 21 verified entries
saved, the last run stopped at the old limit of three runs right after adding `0x82D6FC78`. Two changes since
(`docs/RESEARCH.md` 8d):

1. **Step 6 now finds those functions before the game calls them.** Your first codegen log shows 695 functions in
   TTT2 Web's table that ReXGlue never registers, `0x82D6FC78` among them, almost all tiny virtual methods. TTT2 Web
   found them as addresses stored in the game's data (vtables), so the resolver now registers every one whose
   address is in your image's data and whose code passes its checks, compiles once more, and takes out again any
   that codegen fails with. `work\resolve-REPORT.md` gets an "Ahead of time" section with the counts. A function
   without such a pointer is never added this way; the boot step still adds it if the game calls it.
2. **The boot loop repeats by itself** until a run ends some other way: up to `-BootRounds` runs (default 10, at
   most 50) within `-BootMinutes` (default 480), stopping early on no progress, a repeated trap, less than 5 GB free
   or a failed rebuild. Step 9c of the report lists every run and why the loop stopped.

Your 21 entries are kept (the file is tied to your executable's SHA-1), and a rerun after any stop continues from
them. If something new breaks with the ahead-of-time entries, `-NoIndirect` runs without them for comparison.

Your next run (2026-10-20, checkout `0d1d012`) got all the way: 348 verified entries (327 ahead of time), codegen
without unresolved calls, `ttt2.exe` built, 78 files extracted, and the game started 28 threads without reaching a
single kernel stub. The window then stayed black until the pipeline closed it; the watchdog saw nothing change after
15 s, and the log held one error, `ResolvePath(\Device) failed - device not found`. What that error is
(`docs/RESEARCH.md` 8e): the game opened the disc drive device itself (`\Device\Cdrom0`); ReXGlue mounts your
extracted folder as the hard-disk partition and has no disc device, so that open fails. A game does that to read its
archives by raw disc sector, and TTT2's loader takes that path when the system tells it which sector a file starts
at: ReXGlue answers that question with a made-up number, TTT2 Web (which runs the game to its menus) answers "not
supported", and the loader then reads files by name. **The host now answers like TTT2 Web.** That is the likely cause,
not yet proven on your copy, so the host also records much more about a boot (the boot monitor below): which files
the game opens and which fail, whether it set up the GPU and presented any frame, and for every thread what it waits
on, who holds it and where in the game's code it was called. Next command (`git pull` first; it rebuilds `ttt2.exe`
with the change and boots it, closing the window after 180 s):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot -BootTimeout 180
```

Send `work\report.txt`, `work\boot-triage.md`, `work\boot-report.txt` and the log in `work\boot-logs\`. Watch the
window: whether anything appears (a logo, a loading screen, any colour other than black), and after how many seconds;
whether a dialog box appears; whether the window still responds. `$env:TTT2_SECTOR_QUERY = "runtime"` before the
command brings back the runtime's old answer, if we ever need to compare the two (only when asked).

Your latest run (2026-10-20, `main` at `33f43c8`) presented 10,760 frames, all black, read only 2 files in 180 s,
opened `\Device\Image` (not the disc drive; the game never asked for a disc sector), and one thread made 339.5
million system calls. What that points at (`docs/RESEARCH.md` 8f): the game reads a clock straight from memory
(`KeTimeStampBundle`), and ReXGlue never advances two of its three parts. Both working reference ports keep them
running, one of them measured the game waiting forever on that clock, and the other documents a game thread that loops
until it moves - your busy thread. **The host now keeps that clock running.** `\Device\Image` is a device the game
tries on every boot; neither working port has it either, so the host leaves it unmapped and answers the way the
console does ("no such name"). The host also records what every frame asks the graphics chip to draw, which tells a
black screen the game shows on its own (frames with nothing drawn, for instance while it waits for data) from frames
that are drawn but come out black, and names any thread that spins. Next command, the same as last time (`git pull`
first):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot -BootTimeout 180
```

Send the same four files: `work\report.txt`, `work\boot-triage.md`, `work\boot-report.txt` and the newest log in
`work\boot-logs\`. Watch the window for anything other than black (a logo, a loading screen, a colour) and when it
appears. In `boot-triage.md`, the "Boot progress" table now has a "Kernel clock" row (it should count up to about the
run's length) and the frames row says how many frames had draws; "What to do next" names any thread that still spins.
Only if asked: `$env:TTT2_PIXEL_CHECK = "1"` before the command samples the pictures the game presents (it turns on
the graphics emulation's copy-back, which is slower); `$env:TTT2_KERNEL_CLOCK = "runtime"` or
`$env:TTT2_NOT_FOUND_STATUS = "runtime"` bring back the runtime's old behaviour for a comparison.

Your latest run (2026-10-20, `main` at `8cadd71`) kept the clock running (180 s) but stayed black and silent. What it
shows (`docs/RESEARCH.md` 8g): the main thread holds the game's memory-allocation lock and never lets go - 390 levels
deep, the same number as the run before - so the loader thread and the sound engine, which both need memory, wait for
it forever. The busy thread 19 is a loop the game also runs on the working ports; it still finishes its work twice per
frame, so it is not what holds the boot. And the "0 draws" was not a measurement: the game builds its frames in a buffer
the frame monitor did not read, so it could not see them at all. Three changes:

1. **A recompiler fix for the most likely cause of the stuck lock.** When one of the game's functions runs on past
   its recorded size, the recompiled code returned early and skipped the rest - for instance the release of a lock.
   TTT2 Web found and fixed the same thing in its recompiler, and the C runtime releases its memory lock in this kind
   of code, so it fits; it is not yet confirmed on your copy. Our fix is a patch to
   the SDK's code generator, so **you need the new SDK package once**: on GitHub open the Actions tab, the
   "ReXGlue codegen tool (Windows)" workflow, the newest run on `main` (after this change was merged), download its
   `rexglue-sdk-install-win-amd64` artifact and unzip it over `D:\rexglue-sdk` (replace the old files). The pipeline
   checks this and stops with these instructions if the old package is still there.
2. **The frame monitor reads the frames the way the graphics chip does**, so draws are counted for real, and the report
   says how many frames it could follow.
3. **More evidence for whatever is left:** for every lock a thread holds, where each still-open entry came from; what
   each event really is and who signals it; each of the first file reads; every exception, debug break and failed
   memory call the game makes. A closed-by-timeout run is no longer reported as closed by you.

Next command (`git pull` first, then the SDK package above):

```powershell
scripts\owner-pipeline.ps1 -Iso "D:\path\TTT2.iso" -Codegen -BuildHost -SdkInstall D:\rexglue-sdk -ExtractGame -Boot -BootTimeout 180
```

Send `work\report.txt`, `work\boot-triage.md`, `work\boot-report.txt`, the newest log in `work\boot-logs\` and
`work\codegen-REPORT.md` (its "Fall-through at a function's end" section counts the places the fix changed). Watch
the window for anything other than black and for sound. Only if asked: `$env:TTT2_SIGNAL_AND_WAIT_ALERTABLE =
"runtime"` brings back the runtime's reading of one wait argument (`docs/RESEARCH.md` 8g); `-AllowUnpatchedSdk` runs
the pipeline with an old SDK package for a comparison.

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
finder on it, runs `rexglue codegen` on `recomp\ttt2_manifest.toml` through `recomp\tools\resolve_functions.py`,
and summarises the generated code):

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

**Function boundaries ReXGlue misses (step 6b).** ReXGlue 0.10.0 never makes the target of a plain `b` a function,
so a small function that only a thunk branches to, or a block cut off its function, makes codegen stop with
`UnresolvedCall: ... target not in any function`. `resolve_functions.py` reads each such error, checks in the image
that the instruction really branches there, and looks the target up in TTT2 Web's table (only when your image's
helper fingerprint matches it, as yours does): a function starting at the target becomes an entry point, a branch
inside one function makes that function's boundary an entry. The entries go to `recomp\game\ttt2_resolved_functions.toml`
(addresses only, git-ignored, tied to your image's SHA-1) and codegen runs again. Anything it cannot verify stops
the pipeline with the reason; it never guesses. Its report is `work\resolve-REPORT.md` (each entry with its
evidence); the codegen logs of failed rounds are kept as `work\codegen.round<N>.log`. By hand, on a failed log:

```powershell
python -I recomp\tools\resolve_functions.py verify --image recomp\game\default.xex --log work\codegen.log --reference recomp\ttt2_functions.ttt2web.toml --report work\resolve-REPORT.md
```

**Switch dispatch audit (step 6c).** After codegen, `recomp\tools\audit_switches.py` rebuilds the address of every
`bctr` in the generated code and compares it with the finder's report. The report gets the summary: the finder's
tables emitted as written (anything else is a `PROBLEM:` line), the switches only ReXGlue's own detector found with
a verdict each (a certain miscompile traps when the game reaches it; the summary names how many), and how many
dispatches the finder could not read (each becomes an indirect call that traps if reached, or an unverified SDK
switch). `work\switch-audit.md` adds, for each of those, the few instructions of its dispatch code. When a boot
later stops on a call into the middle of a function, step 9b names an unread dispatch in that function: the address
is then most likely one of its case labels, not a missing function. By hand:

```powershell
python -I recomp\tools\audit_switches.py --image recomp\game\default.xex --generated recomp\generated\default --finder work\switch_tables.json --md work\switch-audit.md
```

**Send `work\report.txt`, `work\resolve-REPORT.md`, `work\codegen-REPORT.md`, `work\codegen.log`,
`work\switch_tables.json` and `work\switch-audit.md`.** They contain function counts, addresses, names and error
messages; `switch-audit.md` and `switch_tables.json` also list the few disassembled instructions of each dispatch
the finder could not read (for its analysis; they stay private between us and are never committed). `recomp\game\ttt2_switch_tables.toml`
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

**The first build of the whole game takes a while.** ReXGlue writes the game's roughly 50,000 functions as about 300
C++ files of up to 1 MB, and Clang compiles them with optimisation; expect tens of minutes on a fast PC, more on a
slower one, and keep a few GB of disk space free (the report prints the free space before the build and how long
codegen and the build took). Ninja compiles about one file per CPU thread at a time; if the build fails because the
compiler ran out of memory (the report then prints a `HINT:` line), rerun the same command with `-BuildJobs 4` (or
`-BuildJobs 2`). Later builds only recompile what changed.

The build folder holds everything the host needs: `ttt2.exe`, `rexruntime.dll` and the Xenos GPU plugin
`rexgpu-xenos.dll`, which the host loads by default (`--gpu_plugin=none` runs without GPU emulation, which is
only useful for log-gathering; the game cannot draw that way). The host exits by itself when the game's main
thread ends, and after a crash it writes the report below and exits with code 3 instead of hanging. When the
runtime gives up (a `[FATAL] ...` line in the log, for instance a call to an address no recompiled function starts
at), the host logs that and exits with code 4 instead of showing the "abort() has been called" box.

**Calls to functions ReXGlue never registered (steps 6b, 9b, 9c).** Functions that only a pointer reaches (vtables
without RTTI, callback tables) pass codegen unregistered; the first call to one ends the run with
`[FATAL] Call to invalid or unregistered function at guest address X`. Step 6b registers the ones it can prove ahead
of time (their address is in your image's data and their code passes the checks; the resolve report's "Ahead of
time" section counts them by reason). The boot loop handles the rest: the triage names X, `resolve_functions.py
boot` adds X as an entry when your image has code there and TTT2 Web's table lists a function starting there, and
(with `-Codegen -BuildHost`) the pipeline recompiles, rebuilds and boots again by itself. It stops, keeping every
entry, when a run ends some other way, when a run's log gives nothing new, when the rebuilt host traps at an address
already added (the entry did not take effect), after `-BootRounds` runs (default 10, at most 50), before a rebuild
that would exceed `-BootMinutes` (default 480), below `-MinFreeGb` (default 5) GB free, or when a rebuild fails;
step 9c says which, and rerunning the same command continues. The runs after a rebuild close the game after
`-BootTimeout` seconds (300 if you gave 0). An X inside a function is a jump into its middle, usually a jump table
the finder did not recognise: that is reported for us to fix, never turned into an entry. Each run's report is
`work\resolve-boot-<N>.md`; send them with the triage. `-NoIndirect` drops the ahead-of-time entries (the others
stay) if you want to compare.

Expect the first boot to fail somewhere: the point is the log. The host writes it to the `logs\` folder next to
`ttt2.exe` (`recomp\out\build\win-amd64-release\logs\`; the window itself shows nothing useful before the game
draws): `ttt2: executable title
4E4D0859 version 0.0.3.2 ...`, `ttt2: graphics backend xenos`, an import summary, a guest-thread census every few
seconds with an **activity** line (files opened and failed, data read, the GPU set-up, frames presented, audio
frames, controller polls), every file the game opens and every open that fails (with its status and where in the
game's code it was called), the GPU, audio, input and dialog milestones, every thread the game creates, a **possible
stall** dump when nothing moves for 15 seconds and again after 60 (every guest thread with the call it waits in, the
objects it waits on and who owns them, and its call stack in the game's code), the same dump when you close the
window, a **kernel stub report** when the main thread exits or you close the window (every system call the game
reached that the runtime only stubs, with counts), and on a crash a **HOST CRASH** block (fault kind and addresses,
the guest thread, the most recent stub calls). `run-host.ps1` copies that file to `work\boot-logs\` and writes the
triage report `work\boot-triage.md` (outcome, boot progress, file access, milestones, crash block, stubs reached
with where TTT2 Web implements them, stalls with every thread's wait and stack, deadlocks and wait chains) and the
run summary `work\boot-report.txt` (how the host ended, exit code, duration). **Send those three files and the log
itself.** By hand, the triage is:

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
