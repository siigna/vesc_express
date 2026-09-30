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


def collect(main_path: str, verbose=False):
    """Read the main script and every module reachable through require.

    Transitive, so a module may require another, and cycles terminate because
    a name already collected is not visited again -- the same property that
    makes require() itself safe at runtime.
    """
    base_dir = os.path.dirname(os.path.abspath(main_path)) or '.'
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
    """Assemble the container."""
    if len(order) > MAX_IMPORTS:
        raise ValueError("%d modules exceeds the %d the firmware accepts"
                         % (len(order), MAX_IMPORTS))

    body = bytearray()
    body += struct.pack('>H', lang_flag)

    src_bytes = main_src.encode('utf-8')
    if b'\0' in src_bytes:
        raise ValueError("the script contains a NUL byte, which terminates the "
                         "source field")
    body += src_bytes + b'\0'

    # The table has to be laid out before the payloads so offsets are known,
    # and an entry's size depends only on its name length.
    table_start = len(body)
    table_size = 2
    for name in order:
        table_size += len(name.encode('utf-8')) + 1 + 4 + 4

    payload_at = table_start + table_size
    offsets = {}
    cursor = payload_at
    for name in order:
        payload = modules[name].encode('utf-8')
        offsets[name] = (cursor, len(payload))
        cursor += len(payload)

    body += struct.pack('>H', len(order))
    for name in order:
        off, length = offsets[name]
        # Offsets are relative to the start of the source, which is where the
        # firmware's pointer begins -- header_size is already excluded.
        body += name.encode('utf-8') + b'\0'
        body += struct.pack('>i', off - 2)
        body += struct.pack('>i', length)

    for name in order:
        body += modules[name].encode('utf-8')

    # body currently starts with the flags word, which is inside the crc'd and
    # counted region, matching what VESC Tool writes.
    blob = struct.pack('>I', len(body)) + struct.pack('>H', crc16(bytes(body))) + bytes(body)

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
        ok("size field matches", size == len(blob) - 6)
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
        for _ in range(count):
            end = base.index(b'\0', ind)
            name = base[ind:end].decode()
            ind = end + 1
            off, length = struct.unpack('>ii', base[ind:ind + 8])
            ind += 8
            found[name] = base[off:off + length].decode()

        ok("mod payload", found.get('mod') == modules['mod'])
        ok("pkg.sub payload", found.get('pkg.sub') == modules['pkg.sub'])

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
    ap.add_argument('--lisp', action='store_true',
                    help='mark the container as LispBM instead of Lua')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true', help='run built-in checks')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.script:
        ap.error('a script is required unless --selftest is given')

    main_src, order, modules, warnings = collect(args.script, args.verbose)

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
