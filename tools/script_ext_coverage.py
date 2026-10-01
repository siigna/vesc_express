#!/usr/bin/env python3
"""Report which lisp extensions have Lua bindings yet.

Both surfaces are read out of the source, so the report cannot go stale the
way a hand-kept checklist does:

  lisp   every lbm_add_extension("name", ...) in the firmware
  lua    every entry in the luaL_Reg tables under main/script

The naming rule is mechanical -- lisp `can-send-sid` is Lua
`vesc.can_send_sid` -- which is what makes the comparison possible at all.

Exit status is 0 unless --min-coverage is given and not met, so CI can hold a
floor and ratchet it up as bindings land rather than blocking on the whole
surface at once.

Usage:
    script_ext_coverage.py
    script_ext_coverage.py --missing
    script_ext_coverage.py --min-coverage 5
    script_ext_coverage.py --category can
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LISP_RE = re.compile(r'lbm_add_extension\(\s*"([^"]+)"')
LUA_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*l_[A-Za-z0-9_]+\s*\}')


# Bindings deliberately not named after their lisp counterpart, because the
# mechanical translation reads badly in Lua. Without these the binding exists
# and the report calls it missing, which is worse than a slightly longer name
# would have been -- so each entry is a decision, not a workaround.
ALIASES = {
    # The function already says BMS; get_bms_val stutters.
    'get-bms-val': 'bms_val',
    'set-bms-val': 'bms_set_val',
    'send-bms-can': 'bms_send_can',
    'set-bms-chg-allowed': 'bms_chg_allowed',
}


def lisp_to_lua(name: str) -> str:
    """The naming rule, in one place."""
    if name in ALIASES:
        return ALIASES[name]
    return name.replace('-', '_').replace('?', '_p').replace('!', '_x')


def walk(root, suffixes=('.c',)):
    for dirpath, dirnames, filenames in os.walk(root):
        # Vendored trees are not ours to read for this.
        dirnames[:] = [d for d in dirnames
                       if d not in ('lispBM', 'lua', 'build', 'managed_components')
                       and not d.startswith('build_')]
        for fn in filenames:
            if fn.endswith(suffixes):
                yield os.path.join(dirpath, fn)


def collect_lisp(main_dir):
    found = {}
    for path in walk(main_dir):
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in LISP_RE.finditer(src):
            found.setdefault(m.group(1), os.path.relpath(path, ROOT))
    return found


def collect_lua(script_dir):
    found = {}
    if not os.path.isdir(script_dir):
        return found
    for path in walk(script_dir):
        src = open(path, encoding='utf-8', errors='replace').read()
        for m in LUA_RE.finditer(src):
            found.setdefault(m.group(1), os.path.relpath(path, ROOT))
    return found


def category(name: str) -> str:
    """Rough grouping, for a report that shows where the gaps are."""
    for prefix in ('can', 'gpio', 'i2c', 'uart', 'eeprom', 'disp', 'img', 'ttf',
                   'bms', 'wifi', 'ble', 'imu', 'conf', 'rgbled', 'touch',
                   'bufs', 'buf', 'str', 'crypto', 'aes', 'bits', 'file',
                   'sleep', 'event', 'log', 'nmea', 'as504x', 'bme280', 'clay'):
        if name.startswith(prefix):
            return prefix
    if name.startswith('get_') or name.startswith('get-'):
        return 'get'
    return 'misc'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--missing', action='store_true', help='list unbound extensions')
    ap.add_argument('--bound', action='store_true', help='list bound extensions')
    ap.add_argument('--category', help='restrict to one category')
    ap.add_argument('--min-coverage', type=float, default=None,
                    help='fail if coverage is below this percentage')
    args = ap.parse_args()

    lisp = collect_lisp(os.path.join(ROOT, 'main'))
    lua = collect_lua(os.path.join(ROOT, 'main', 'script'))

    mapped = {name: lisp_to_lua(name) for name in lisp}
    bound = {n for n, l in mapped.items() if l in lua}
    missing = sorted(set(lisp) - bound)

    # Lua functions with no lisp counterpart: either new, or misnamed against
    # the rule. Worth surfacing, since a typo here silently reads as a gap.
    extra = sorted(set(lua) - {lisp_to_lua(n) for n in lisp})

    if args.category:
        missing = [n for n in missing if category(lisp_to_lua(n)) == args.category]
        bound = {n for n in bound if category(lisp_to_lua(n)) == args.category}

    total = len(lisp)
    pct = 100.0 * len(bound) / total if total else 100.0

    print("lisp extensions: %d" % total)
    print("lua bindings:    %d" % len(lua))
    print("covered:         %d (%.1f%%)" % (len(bound), pct))
    print()

    by_cat = {}
    for name in lisp:
        c = category(lisp_to_lua(name))
        d = by_cat.setdefault(c, [0, 0])
        d[1] += 1
        if name in bound:
            d[0] += 1
    print("%-10s %8s %8s" % ("category", "bound", "total"))
    for c in sorted(by_cat, key=lambda x: (-by_cat[x][1], x)):
        got, tot = by_cat[c]
        print("%-10s %8d %8d" % (c, got, tot))

    if args.bound:
        print("\nbound:")
        for n in sorted(bound):
            print("  %-28s -> vesc.%s" % (n, lisp_to_lua(n)))

    if args.missing:
        print("\nnot yet bound:")
        for n in missing:
            print("  %-28s would be vesc.%s" % (n, lisp_to_lua(n)))

    if extra:
        print("\nlua functions with no lisp counterpart (new, or misnamed):")
        for n in extra:
            print("  vesc.%s" % n)

    if args.min_coverage is not None and pct < args.min_coverage:
        print("\nFAIL coverage %.1f%% is below the %.1f%% floor"
              % (pct, args.min_coverage), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
