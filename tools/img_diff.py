#!/usr/bin/env python3
"""Compare two rendered images, one of which may be a PPM.

    img_diff.py golden.png rendered.ppm [--tol N] [--out diff.png]

Exists because the two dashes render through different code: the lisp views
through the lispBM repl, the Lua ones through main/script/test/render_host.
The goldens in dash_common/test were produced by the first, so checking a
port against them needs a comparison that reports *how* different rather than
just whether, and a tolerance that was measured instead of assumed.

Reports the worst per-channel difference, how many pixels differ at all, and
how many differ by more than the tolerance. The last number is the one to
gate on.

Measured, so the default tolerance of zero is not optimism: the same scene --
a filled rectangle and antialiased text through a four-entry palette --
rendered by the lispBM repl and by render_host came out pixel identical,
80000 of 80000, worst per-channel difference zero. The two agree because they
share tinygfx and read the same prepared glyph bitmaps, and because a lisp
colour list of (0 1 2 3) and a Lua base index of 1 mean the same thing.

So a difference against a golden is a real difference. If a tolerance ever
becomes necessary, something changed in one renderer and not the other, and
that is worth finding rather than absorbing.

To reproduce the calibration:

    repl -H 400000 -M 4000000 -s cal.lisp --terminate    # writes a png
    render_host cal.luapkg cal.ppm 400 200               # writes a ppm
    img_diff.py cal.png cal.ppm
"""

import argparse
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()

    # P6 with three whitespace-separated fields, then one byte of whitespace.
    if not data.startswith(b'P6'):
        raise ValueError('%s is not a P6 ppm' % path)

    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1

    w, h, maxval = fields
    if maxval != 255:
        raise ValueError('%s has maxval %d, only 255 is handled' % (path, maxval))

    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError('%s is truncated: %d bytes of pixel data, expected %d'
                         % (path, len(px), w * h * 3))
    return w, h, px


def read_png(path):
    """Enough PNG to read what the repl writes: 8-bit RGB or RGBA, no
    interlace. Hand-rolled rather than pulling in pillow, so this runs
    wherever python3 does."""
    with open(path, 'rb') as f:
        data = f.read()

    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('%s is not a png' % path)

    pos = 8
    idat = bytearray()
    w = h = depth = ctype = None

    while pos < len(data):
        length, ctag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length

        if ctag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8:
                raise ValueError('%s is %d bit, only 8 is handled' % (path, depth))
            if ctype not in (2, 6):
                raise ValueError('%s has colour type %d, only 2 and 6 are handled'
                                 % (path, ctype))
            if interlace:
                raise ValueError('%s is interlaced' % path)
        elif ctag == b'IDAT':
            idat += body
        elif ctag == b'IEND':
            break

    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = w * nch

    # Undo the per-scanline filters.
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride

        if ftype == 1:
            for x in range(nch, stride):
                line[x] = (line[x] + line[x - nch]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                left = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        elif ftype != 0:
            raise ValueError('%s uses filter %d' % (path, ftype))

        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * nch:x * nch + 3]

        prev = line

    return w, h, bytes(out)


def load(path):
    if path.lower().endswith('.ppm'):
        return read_ppm(path)
    return read_png(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('reference')
    ap.add_argument('rendered')
    ap.add_argument('--tol', type=int, default=0,
                    help='per-channel difference treated as equal (default 0)')
    ap.add_argument('--max-bad', type=int, default=0,
                    help='how many pixels may exceed the tolerance (default 0)')
    args = ap.parse_args()

    aw, ah, a = load(args.reference)
    bw, bh, b = load(args.rendered)

    if (aw, ah) != (bw, bh):
        print('size differs: reference %dx%d, rendered %dx%d' % (aw, ah, bw, bh))
        return 1

    worst = 0
    differing = 0
    over_tol = 0
    first_bad = None

    for i in range(0, len(a), 3):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        if d:
            differing += 1
            if d > worst:
                worst = d
            if d > args.tol:
                over_tol += 1
                if first_bad is None:
                    p = i // 3
                    first_bad = (p % aw, p // aw,
                                 tuple(a[i:i + 3]), tuple(b[i:i + 3]))

    total = aw * ah
    print('%dx%d, %d pixels' % (aw, ah, total))
    print('differing at all: %d (%.3f%%)' % (differing, 100.0 * differing / total))
    print('worst per-channel difference: %d' % worst)
    print('over tolerance %d: %d (allowed %d)' % (args.tol, over_tol, args.max_bad))
    if first_bad:
        print('first over tolerance: (%d,%d) reference %s rendered %s' % first_bad)

    return 0 if over_tol <= args.max_bad else 1


if __name__ == '__main__':
    sys.exit(main())
