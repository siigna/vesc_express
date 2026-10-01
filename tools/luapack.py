#!/usr/bin/env python3
"""Pack a Lua script and its required modules into a VESC script container.

This is the Lua counterpart of VESC Tool's lisp import bundling
(CodeLoader::lispPackImports). VESC Tool walks `(import "file" 'tag)` lines
and appends each file to the blob; this walks `require "mod"` calls and does
the same, producing a byte-identical container format so the existing upload,
erase, run and REPL commands carry it unchanged.

Container layout, big endian throughout:

    uint32  size of everything after the crc field
    uint16  crc16-ccitt over everything after the crc field
    uint16  flags               bit 0: 0 = LispBM, 1 = Lua
    char[]  source, NUL terminated
    uint16  number of imports
    per import:
        char[]  name, NUL terminated
        int32   payload offset, relative to the start of the source
        int32   payload length

The flags word is why a single package store can serve both engines: VESC Tool
has always written zero there and the firmware never read it, so bit 0 marks
the language without a new command id or a change to the container.

With --lisp it packs a LispBM script instead, walking (import "path" 'sym)
lines and keying the table by symbol. That is enough to package a real lisp
project from the command line; imports that come from VESC Tool's package
archive (pkg@://...) cannot be resolved and are reported.

Usage:
    luapack.py main.lua -o main.luapkg
    luapack.py main.lua --print-imports
    luapack.py --selftest
"""

import argparse
import os
import re
import struct
import sys

HEADER_SIZE = 8
FLAG_LANG_LISP = 0x0000
FLAG_LANG_LUA = 0x0001
MAX_IMPORTS = 499          # The firmware refuses a count of 500 or more.
MAX_BLOB = 512 * 1024 - 6  # ESP32 script partition, less the size and crc.


def crc16(data: bytes) -> int:
    """CRC16-CCITT, the same polynomial and seed the packet protocol uses."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# require("x"), require('x') and require "x" / require 'x'. Deliberately not a
# Lua parser: a build tool that silently disagrees with the interpreter about
# what a program means is worse than one with an obvious limitation, so
# anything dynamic is reported rather than guessed at.
_REQUIRE = re.compile(r'''(?<![\w.:])require\s*(?:\(\s*)?["']([^"'\n]+)["']''')
_REQUIRE_DYNAMIC = re.compile(r'''(?<![\w.:])require\s*(?:\(\s*)?[^"'\s)]''')


def scan_source(src: str):
    """Return (comment-blanked source, spans of string literals).

    Comments are blanked so a commented-out require is not bundled. String
    literals are *kept*, because the require matcher needs to see the quoted
    module name -- but their spans are returned so a match inside a string can
    be discarded. Without that, an error message mentioning the word require
    is read as code; the end-to-end fixture in main/script/test tripped exactly
    that.

    Blanking replaces with spaces rather than deleting, so offsets and
    therefore reported line numbers stay correct.
    """
    out = []
    spans = []
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == '--':
            # Long comment --[[ ... ]] or --[==[ ... ]==]
            m = re.match(r'--\[(=*)\[', src[i:])
            if m:
                close = ']' + m.group(1) + ']'
                end = src.find(close, i)
                end = n if end < 0 else end + len(close)
            else:
                end = src.find('\n', i)
                end = n if end < 0 else end
            out.append(''.join(' ' if c != '\n' else '\n' for c in src[i:end]))
            i = end
            continue
        if src[i] in '"\'':
            quote = src[i]
            j = i + 1
            while j < n and src[j] != quote:
                if src[j] == '\\':
                    j += 1
                j += 1
            j = min(j + 1, n)
            # Kept verbatim, but recorded so matches inside can be dropped.
            spans.append((i, j))
            out.append(src[i:j])
            i = j
            continue
        out.append(src[i])
        i += 1
    return ''.join(out), spans


def _in_string(pos: int, spans) -> bool:
    """True if pos falls inside a string literal.

    The opening quote itself counts as outside, so that a match starting at
    `require "x"` -- whose argument is a string -- is not discarded, while a
    match that begins within quoted text is.
    """
    for start, end in spans:
        if start < pos < end:
            return True
    return False


def find_requires(src: str):
    """Module names required by this source, in order of first appearance."""
    cleaned, spans = scan_source(src)
    names = []
    for m in _REQUIRE.finditer(cleaned):
        if _in_string(m.start(), spans):
            continue
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def find_dynamic_requires(src: str):
    """Requires whose argument is not a literal, which cannot be bundled."""
    cleaned, spans = scan_source(src)
    literal = {m.start() for m in _REQUIRE.finditer(cleaned)}
    return [m.start() for m in _REQUIRE_DYNAMIC.finditer(cleaned)
            if m.start() not in literal and not _in_string(m.start(), spans)]


def module_path(base_dir: str, name: str) -> str:
    """Resolve a module name the way Lua's default path would, relative to the
    main script: dots become directory separators."""
    rel = name.replace('.', os.sep)
    for candidate in (rel + '.lua', os.path.join(rel, 'init.lua')):
        path = os.path.join(base_dir, candidate)
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(
        "module '%s' required but not found (looked for %s.lua and %s/init.lua "
        "under %s)" % (name, rel, rel, base_dir))


# (import "path" 'symbol) -- the lisp form. The import table is keyed by the
# SYMBOL, not the path: the firmware's ext_import compares the table entry
# against the name of the destination symbol. That is also what makes
# --asset work for a lisp script.
_LISP_IMPORT = re.compile(r"""\(\s*import\s+"([^"]+)"\s+'([^\s)]+)\s*\)""")


def collect_lisp(main_path: str, verbose=False, import_root=None):
    """Read a lisp script and the files it imports.

    Not recursive: lisp imports bind a symbol to a file's bytes, and the
    importing script decides when to evaluate it, so an imported file's own
    imports are its business rather than something to resolve here.
    """
    base_dir = import_root_dir(main_path, import_root)
    with open(main_path, 'r', encoding='utf-8') as f:
        src = f.read()

    order = []
    payloads = {}
    warnings = []

    for path, sym in _LISP_IMPORT.findall(src):
        if sym in payloads:
            warnings.append("symbol %s imported more than once; keeping the "
                            "first" % sym)
            continue

        if path.startswith('pkg@') or path.startswith('pkg::'):
            # These resolve out of VESC Tool's downloaded package archive,
            # which is not available here. Reported rather than guessed at:
            # the script will fail at the point it evaluates that symbol, and
            # knowing which one is the difference between a quick fix and a
            # hunt.
            warnings.append("%s imports %s from the package archive, which "
                            "cannot be resolved here. Supply it with "
                            "--asset %s=<file> (the import table is keyed by "
                            "symbol, so that is all it takes)" % (sym, path, sym))
            continue

        full = os.path.normpath(os.path.join(base_dir, path))
        if not os.path.isfile(full):
            raise FileNotFoundError("%s imports %s, which is not there "
                                    "(looked at %s)" % (sym, path, full))

        with open(full, 'rb') as f:
            payloads[sym] = f.read()
        order.append(sym)
        if verbose:
            print("  import   %-24s %-44s %d bytes"
                  % (sym, os.path.relpath(full, base_dir), len(payloads[sym])),
                  file=sys.stderr)

    return src, order, payloads, warnings


def read_assets(specs, verbose=False):
    """Read --asset NAME=PATH pairs into the import table.

    Binary data has nowhere else to live: the board has no filesystem, and the
    container is the only thing that travels with a script. A prepared font or
    an icon goes here and the script reads it back with vesc.asset(name).
    """
    assets = {}
    order = []
    for spec in specs or []:
        if '=' not in spec:
            raise ValueError("--asset wants NAME=PATH, got %r" % spec)
        name, path = spec.split('=', 1)
        name = name.strip()
        if not name:
            raise ValueError("--asset has an empty name: %r" % spec)
        if name in assets:
            raise ValueError("--asset %s given twice" % name)
        with open(path, 'rb') as f:
            assets[name] = f.read()
        order.append(name)
        if verbose:
            print("  asset    %-24s %d bytes" % (name, len(assets[name])),
                  file=sys.stderr)
    return order, assets


def import_root_dir(main_path: str, import_root=None) -> str:
    """Where a relative import resolves from.

    Normally the main script's own directory, which is what VESC Tool does.
    --import-root overrides it so a modified copy of a script can be packed
    without moving it into the tree it imports from -- the case that comes up
    when testing a change to somebody else's package.
    """
    if import_root:
        root = os.path.abspath(import_root)
        if not os.path.isdir(root):
            raise NotADirectoryError("--import-root %s is not a directory"
                                     % import_root)
        return root
    return os.path.dirname(os.path.abspath(main_path)) or '.'


def collect(main_path: str, verbose=False, import_root=None):
    """Read the main script and every module reachable through require.

    Transitive, so a module may require another, and cycles terminate because
    a name already collected is not visited again -- the same property that
    makes require() itself safe at runtime.
    """
    base_dir = import_root_dir(main_path, import_root)
    with open(main_path, 'r', encoding='utf-8') as f:
        main_src = f.read()

    modules = {}
    order = []
    queue = list(find_requires(main_src))
    sources = {'': main_src}

    while queue:
        name = queue.pop(0)
        if name in modules:
            continue
        path = module_path(base_dir, name)
        with open(path, 'r', encoding='utf-8') as f:
            src = f.read()
        modules[name] = src
        order.append(name)
        sources[name] = src
        if verbose:
            print("  bundling %-24s %s" % (name, os.path.relpath(path, base_dir)),
                  file=sys.stderr)
        for dep in find_requires(src):
            if dep not in modules:
                queue.append(dep)

    warnings = []
    for name, src in sources.items():
        for off in find_dynamic_requires(src):
            line = src.count('\n', 0, off) + 1
            where = name if name else os.path.basename(main_path)
            warnings.append("%s:%d: require with a non-literal argument cannot "
                            "be bundled and will fail at runtime" % (where, line))

    return main_src, order, modules, warnings


def build(main_src: str, order, modules, lang_flag=FLAG_LANG_LUA) -> bytes:
    """Assemble the container, byte for byte as CodeLoader::lispPackImports does.

    Three details here are not decoration, and getting any of them wrong
    produces a container that uploads cleanly and then misbehaves:

      - Every payload gets a NUL appended. VESC Tool's comment is "pad with 0
        in case it is a text file", and it matters: an imported lisp file is
        handed to read-eval-program as a shared array, and without a
        terminator the reader runs off the end. That showed up as a read_error
        partway through a file that was perfectly well-formed.

      - Payload offsets are aligned to four bytes, "in case this is loaded as
        code", with the padding inserted into the stream.

      - The stored size field is two less than the counted region's length.

    The offsets are relative to the start of the source, which is where the
    firmware's pointer begins.
    """
    if len(order) > MAX_IMPORTS:
        raise ValueError("%d entries exceeds the %d the firmware accepts"
                         % (len(order), MAX_IMPORTS))

    def as_bytes(v):
        # Modules and lisp imports arrive as text, assets as bytes.
        return v if isinstance(v, bytes) else v.encode('utf-8')

    src_bytes = main_src.encode('utf-8')
    if b'\0' in src_bytes:
        raise ValueError("the script contains a NUL byte, which terminates "
                         "the source field")

    body = bytearray()
    body += struct.pack('>H', lang_flag)
    body += src_bytes + b'\0'
    body += struct.pack('>H', len(order))

    # Every payload carries a trailing NUL, and its recorded length includes it.
    payloads = {name: as_bytes(modules[name]) + b'\0' for name in order}

    table_size = sum(len(name.encode('utf-8')) + 9 for name in order)

    # Mirrors `file_offset = vb.size() + file_table_size - 2` once the count
    # has been written.
    file_offset = len(body) + table_size - 2

    for name in order:
        while file_offset % 4 != 0:
            file_offset += 1
        body += name.encode('utf-8') + b'\0'
        body += struct.pack('>i', file_offset)
        body += struct.pack('>i', len(payloads[name]))
        file_offset += len(payloads[name])

    for name in order:
        while (len(body) - 2) % 4 != 0:
            body += b'\0'
        body += payloads[name]

    # The size field is len(body) - 2, not len(body): that is what VESC Tool
    # writes, and the firmware's code_check validates size and crc together,
    # so being two bytes out makes the engine run nothing at all, silently.
    blob = (struct.pack('>I', len(body) - 2)
            + struct.pack('>H', crc16(bytes(body)))
            + bytes(body))

    if len(blob) > MAX_BLOB:
        raise ValueError("packed script is %d bytes, over the %d the script "
                         "partition holds" % (len(blob), MAX_BLOB))
    return blob


def selftest() -> int:
    """Checks that do not need a board, run by CI.

    The container layout is duplicated between this file and
    main/script/script_pack.c, so the round-trip test in the C host tests and
    these have to agree. Both are run by the same workflow for that reason.
    """
    import tempfile
    checks = failures = 0

    def ok(what, cond):
        nonlocal checks, failures
        checks += 1
        if not cond:
            failures += 1
            print("FAIL %s" % what)

    # crc16 against a known value from the protocol implementation.
    ok("crc16 of empty input", crc16(b'') == 0)
    ok("crc16 is order dependent", crc16(b'\x01\x02') != crc16(b'\x02\x01'))

    # require discovery, including the forms and the things that must not match.
    ok("double quotes", find_requires('require("a")') == ['a'])
    ok("single quotes", find_requires("require('a')") == ['a'])
    ok("no parens", find_requires('require "a"') == ['a'])
    ok("no parens single", find_requires("require 'a'") == ['a'])
    ok("whitespace", find_requires('require  (  "a" )') == ['a'])
    ok("dotted name", find_requires('require("pkg.mod")') == ['pkg.mod'])
    ok("several, deduplicated",
       find_requires('require("a") require("b") require("a")') == ['a', 'b'])
    ok("line comment ignored", find_requires('-- require("a")\n') == [])
    ok("block comment ignored", find_requires('--[[ require("a") ]]') == [])
    ok("long bracket comment ignored",
       find_requires('--[==[ require("a") ]==]') == [])
    ok("method call not matched", find_requires('obj:require("a")') == [])
    ok("field access not matched", find_requires('t.require("a")') == [])
    ok("prefixed identifier not matched", find_requires('prerequire("a")') == [])
    # A require inside a string literal is text, not code. The end-to-end
    # fixture's assert message ("transitive require did not load") made this
    # a real false positive rather than a hypothetical one.
    ok("require inside a double-quoted string ignored",
       find_requires('assert(x, "call require(\'a\') first")') == [])
    ok("require inside a single-quoted string ignored",
       find_requires("error('use require(\"a\")')") == [])
    ok("word require inside a message ignored",
       find_requires('assert(y, "transitive require did not load")') == [])
    ok("dynamic require inside a string ignored",
       find_dynamic_requires('print("require(name)")') == [])
    ok("code after a string still matched",
       find_requires('print("hi") require("a")') == ['a'])

    # A dynamic require is reported rather than silently dropped.
    ok("dynamic require flagged", len(find_dynamic_requires('require(name)')) == 1)
    ok("literal require not flagged", len(find_dynamic_requires('require("a")')) == 0)

    # Round trip through a real directory.
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, 'main.lua'), 'w') as f:
            f.write('local m = require("mod")\nlocal n = require("pkg.sub")\nprint(m, n)\n')
        with open(os.path.join(d, 'mod.lua'), 'w') as f:
            f.write('return {val = 7}\n')
        os.makedirs(os.path.join(d, 'pkg'))
        with open(os.path.join(d, 'pkg', 'sub.lua'), 'w') as f:
            f.write('local m = require("mod")\nreturn {v = m.val}\n')

        main_src, order, modules, warnings = collect(os.path.join(d, 'main.lua'))
        ok("transitive modules collected", set(order) == {'mod', 'pkg.sub'})
        ok("no warnings for literal requires", warnings == [])

        blob = build(main_src, order, modules)

        # Parse it back the way the firmware does, to catch a layout mistake
        # here rather than on a board.
        size = struct.unpack('>I', blob[0:4])[0]
        crc = struct.unpack('>H', blob[4:6])[0]
        # Mirrors CodeLoader exactly: the counted region starts at the flags
        # word and the figure stored is two less than its length. The previous
        # expectation here encoded the same off-by-two as the writer, so the
        # selftest agreed with the bug.
        ok("size field is len(body) - 2", size == len(blob) - 6 - 2)
        ok("crc field matches", crc == crc16(blob[6:]))

        flags = struct.unpack('>H', blob[6:8])[0]
        ok("flags mark lua", flags & 1 == FLAG_LANG_LUA)

        base = blob[HEADER_SIZE:]
        nul = base.index(b'\0')
        ok("source round trips", base[:nul].decode() == main_src)

        ind = nul + 1
        count = struct.unpack('>H', base[ind:ind + 2])[0]
        ind += 2
        ok("import count", count == 2)

        found = {}
        offsets_seen = []
        for _ in range(count):
            end = base.index(b'\0', ind)
            name = base[ind:end].decode()
            ind = end + 1
            off, length = struct.unpack('>ii', base[ind:ind + 8])
            ind += 8
            offsets_seen.append(off)
            found[name] = base[off:off + length].decode()

        # Payloads carry a trailing NUL, as VESC Tool writes them.
        ok("mod payload", found.get('mod') == modules['mod'] + '\0')
        ok("pkg.sub payload", found.get('pkg.sub') == modules['pkg.sub'] + '\0')
        ok("payload offsets are 4-byte aligned",
           all(o % 4 == 0 for o in offsets_seen))

        # --import-root resolves imports away from the script's own
        # directory. Packed from /tmp against the fixture as root, the same
        # modules must be found.
        import shutil
        with tempfile.TemporaryDirectory() as elsewhere:
            moved = os.path.join(elsewhere, 'main.lua')
            shutil.copy(os.path.join(d, 'main.lua'), moved)
            _, root_order, _, _ = collect(moved, import_root=d)
            ok("import-root finds modules outside the script's directory",
               set(root_order) == {'mod', 'pkg.sub'})
            try:
                collect(moved)
                ok("without import-root the same pack fails", False)
            except OSError:
                ok("without import-root the same pack fails", True)
        try:
            import_root_dir(os.path.join(d, 'main.lua'),
                            os.path.join(d, 'not-a-dir'))
            ok("bad import-root rejected", False)
        except NotADirectoryError:
            ok("bad import-root rejected", True)

        # A missing module is an error with a useful message, not a traceback.
        with open(os.path.join(d, 'bad.lua'), 'w') as f:
            f.write('require("nope")\n')
        try:
            collect(os.path.join(d, 'bad.lua'))
            ok("missing module raises", False)
        except FileNotFoundError as e:
            ok("missing module raises", 'nope' in str(e))

    # A NUL in the source would truncate the field.
    try:
        build('print("a\0b")', [], {})
        ok("NUL in source refused", False)
    except ValueError:
        ok("NUL in source refused", True)

    print("\n%d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('script', nargs='?', help='main .lua file')
    ap.add_argument('-o', '--output', help='output container (default: <script>.luapkg)')
    ap.add_argument('--print-imports', action='store_true',
                    help='list the modules that would be bundled, then exit')
    ap.add_argument('--asset', action='append', metavar='NAME=PATH',
                    help='bundle a binary file, readable as vesc.asset(NAME). '
                         'Repeatable.')
    ap.add_argument('--import-root', metavar='DIR',
                    help="resolve relative imports from DIR instead of the "
                         "script's own directory, so a script can be packed "
                         'from outside the tree it imports from')
    ap.add_argument('--lisp', action='store_true',
                    help='mark the container as LispBM instead of Lua')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true', help='run built-in checks')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.script:
        ap.error('a script is required unless --selftest is given')

    try:
        if args.lisp:
            main_src, order, modules, warnings = collect_lisp(
                args.script, args.verbose, args.import_root)
        else:
            main_src, order, modules, warnings = collect(
                args.script, args.verbose, args.import_root)
    except OSError as e:
        # A missing module or a bad --import-root is a user mistake; the
        # messages say which file and where it was looked for, so a traceback
        # on top of that is noise.
        print("error: %s" % e, file=sys.stderr)
        return 1

    try:
        asset_order, assets = read_assets(args.asset, args.verbose)
    except (ValueError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    # An import the walker could not resolve is not a problem if --asset
    # supplies it, so drop those warnings rather than telling the user to do
    # what they have already done.
    warnings = [w for w in warnings
                if not any(w.startswith(a + " imports ") for a in asset_order)]

    for name in asset_order:
        if name in modules:
            print("error: asset %s collides with a bundled module" % name,
                  file=sys.stderr)
            return 1
    order = order + asset_order
    modules = dict(modules, **assets)

    for w in warnings:
        print("warning: %s" % w, file=sys.stderr)

    if args.print_imports:
        for name in order:
            print(name)
        return 0

    blob = build(main_src, order, modules,
                 FLAG_LANG_LISP if args.lisp else FLAG_LANG_LUA)

    out = args.output or (os.path.splitext(args.script)[0] + '.luapkg')
    with open(out, 'wb') as f:
        f.write(blob)

    print("%s: %d bytes, %d module%s bundled"
          % (out, len(blob), len(order), '' if len(order) == 1 else 's'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
