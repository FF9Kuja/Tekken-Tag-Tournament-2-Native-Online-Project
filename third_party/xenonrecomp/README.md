# third_party/xenonrecomp

Pinned checkout location for [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT). Only its
`XenonUtils` XEX patcher and the two helper libraries it needs are compiled into `ttt2-xexpatch`:

| Component | License | Use |
|---|---|---|
| `XenonUtils/xex_patcher.cpp`, `xex.h`, `memory_mapped_file.cpp` | MIT (XenonRecomp); patcher derived from Xenia Canary (BSD-3) | applies `.xexp` delta patches |
| `thirdparty/libmspack` (`lzxd.c`) | LGPL-2.1 | LZX delta decompression; this project's source is published, so relinking terms are met |
| `thirdparty/tiny-AES-c` | Unlicense (public domain) | AES for the patcher's key handling |
| `thirdparty/TinySHA1` | MIT | digest checks |

The GPL PowerPC disassembler that XenonRecomp also bundles is **not** compiled or linked.

`scripts/fetch-xenonrecomp.sh` clones the pinned commit with just those submodules; the checkout lives in
`src/` here and is git-ignored.
