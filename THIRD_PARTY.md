# Third-party software and data

This repository contains E1 Doom's original camera platform code and patches, not copies of its upstream engines or game data. `tools/fetch-sources` retrieves the exact revisions recorded in `sources.lock`.

## Woof

E1 Doom is based on [Woof 15.3.0](https://github.com/fabiangreffrath/woof/tree/9f124c6269926f90231592ea1bd2c39358ad17c9). Woof is predominantly GPL-2.0-or-later and contains GPL-3.0-or-later files plus components and data under compatible MIT, BSD, CC0, CC-BY, and other licenses. Its complete copyright and license inventory is in the fetched checkout's `README.md`; its GPL text is in `COPYING`.

`patches/woof/e1-doom.patch` adds the camera build shims, startup/attract-loop and quit behavior, ARM span-rendering changes, and the fixed-rate/fast-path OPL2 work. Because the resulting executable includes Woof's GPL-3.0-or-later video code, E1 Doom distributes the combined work under GPL-3.0-or-later.

## RP2040 Doom OPL renderer

The optimized OPL2 path uses four files from [rp2040-doom](https://github.com/kilograham/rp2040-doom/tree/29a453c980918a03e40fc8b69b024e7a3bdb5dc2): `opl/emu8950.c`, `opl/emu8950.h`, `opl/slot_render.cpp`, and `opl/slot_render.h`. Upstream describes its modified emu8950-derived code as retaining the MIT license and its new RP2040-specific code as BSD-3-Clause. The original attribution and SPDX identifier remain in `emu8950.c`; the license texts are in `LICENSES/MIT.txt` and `LICENSES/BSD-3-Clause.txt`.

`patches/rp2040-doom-opl/e1-doom.patch` contains E1 Doom's additional fixed-rate and skip-path optimizations.

## Doom IWAD

No Doom WAD is included. The build accepts a user-supplied IWAD through `IWAD=/path/to/file.wad`. The reference configuration was tested with the official Doom v1.9 shareware IWAD (SHA-256 `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`). The WAD remains subject to id Software's terms; it is not covered by this repository's GPL license.

## Reolink/Novatek interfaces

No camera firmware, extracted library, vendor sample, or Novatek HDAL header is included. Set `HDAL_INCLUDE_DIR` to headers you obtained separately. Those inputs remain under their respective owners' terms.
