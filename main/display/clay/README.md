# Clay

Vendored, unmodified.

- Upstream: https://github.com/nicbarker/clay
- Commit: `e6cc36941ab2af5d81107617039d6f527a1c660b` (2026-05-20)
- Licence: zlib. The full notice is at the bottom of `clay.h`; leave it there.

Clay computes layout and nothing else: it emits a sorted list of rectangles,
text runs, borders and images, and drawing them is the caller's job. It has no
dependencies, links no standard library, and allocates only the arena it is
handed at init.

`../clay_layout.c` is the VESC side: it walks a LispBM description, drives
Clay, and returns the command list to lisp, which draws it with the existing
`img-*` and `disp-render` primitives.

## Upgrading

The walker calls `Clay__OpenElement`, `Clay__ConfigureOpenElement`,
`Clay__OpenTextElement` and `Clay__CloseElement` directly, because the
`CLAY()` macros are for layouts written in C and a data-driven walker cannot
use them. The double underscore is upstream's way of saying "not the public
contract", so an upgrade is a deliberate act: bump the file, re-run the
golden-image tests, and update the commit hash above.
