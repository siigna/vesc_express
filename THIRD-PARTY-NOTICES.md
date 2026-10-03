# Third-party notices

Code in this repository that was written by other people and vendored in,
rather than depended on. Every file keeps its own copyright notice; this file
exists so the list can be read without walking the tree, and because one of
these licences asks for exactly that.

The provenance audit (`tests/provenance/audit.py` in the firmware tree) answers
a different question — who wrote the content of files *this fork* adds — and it
does not see vendored trees as findings, because their notices are intact. They
are listed here so that is a stated fact rather than an assumption.

## Lua 5.4 — MIT

`main/lua/`, 53 files, about 25,800 lines.

Copyright © 1994–2024 Lua.org, PUC-Rio. The full permission notice is at the
end of `main/lua/lua.h`, which is where the other files point.

Vendored rather than used as a submodule because `luaconf.h` is patched for
Xtensa, which is the part most likely to break on a toolchain bump — the
`target-compile` CI job compiles it for esp32s3 for that reason.

## Clay 0.14 — zlib

`main/display/clay/clay.h`, one file, about 5,100 lines.

Copyright © 2024 Nic Barker. The licence is at the foot of the header.

Its first condition is that the origin of the software must not be
misrepresented and that you must not claim you wrote it; its second is that an
acknowledgment in the product documentation would be appreciated. This file is
that acknowledgment.

## LispBM — GPL-3.0

`main/lispBM/`, 235 files, about 88,200 lines.

Copyright © 2018, 2020–2026 Joel Svensson and contributors. Carried as a
subtree, updated with `tools/lispbm_subtree.py`.

It vendors third-party code of its own, listed in
`main/lispBM/THIRD-PARTY-NOTICES.md` — LibSchrift among others. That file is
upstream's and is not maintained here.

## Everything else

The rest of this repository is the VESC Express firmware, copyright © Benjamin
Vedder and contributors, GPL-3.0, with this fork's changes on top. Individual
files name their authors; `git blame -C -C -C` is the authority when they
disagree with a guess.
