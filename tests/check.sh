#!/usr/bin/env bash
# Copyright 2026 Stephen Bouche
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything that can be checked without a board and without ESP-IDF.
#
#   ./tests/check.sh
#   ./tests/check.sh --fuzz        also fuzz the container parser for 30s
#   ./tests/check.sh --fuzz-only   nothing but the fuzzing
#
# This exists so that CI and a developer run the same thing. The jobs in
# .github/workflows/script-engine.yml call it rather than restating the
# commands, because the version that restated them drifted: the sanitizer
# guard grepped the Makefile for a string the Makefile had stopped containing,
# and the job sat red while the tests underneath it passed on every push.
#
# The ESP-IDF builds are not here. They need a toolchain this cannot assume,
# and they are the one part of that workflow which genuinely has to differ.
#
# Deliberately no `set -e`: one failing stage must not stop the others, or a
# single break hides every other result. The exit code is the verdict.

set -uo pipefail
cd "$(dirname "$0")/.."

fuzz=0
fuzz_secs=30

fuzz_only=0

for arg in "$@"; do
    case "$arg" in
        --fuzz) fuzz=1 ;;
        --fuzz-only) fuzz=1; fuzz_only=1 ;;
        --fuzz-secs=*) fuzz=1; fuzz_secs="${arg#*=}" ;;
        *) printf 'check.sh: unknown argument %s\n' "$arg" >&2; exit 2 ;;
    esac
done

fail=0

stage() { printf '\n=== %s ===\n' "$1"; }

report() {
    if [ "$1" -eq 0 ]; then
        printf '  ok\n'
    else
        printf '  FAILED\n'
        fail=1
    fi
}

# Each stage keeps its raw output and shows it only on failure. A stage that
# filters to a summary and then reports a bare FAILED has thrown away the only
# evidence there was.
raw=$(mktemp -d)
trap 'rm -rf "$raw"' EXIT

explain() {
    if [ "$1" -ne 0 ] && [ -s "$2" ]; then
        printf '  --- last 20 lines of raw output ---\n'
        tail -20 "$2" | sed 's/^/  | /'
    fi
}

# --fuzz-only exists so the fuzzing can have a CI job of its own without
# repeating the minute of host tests the other job already ran.
if [ "$fuzz_only" -eq 0 ]; then

stage "host tests (asan + ubsan)"
make -C main/script/test clean >/dev/null 2>&1
make -C main/script/test run > "$raw/host" 2>&1
st=$?
grep -E "checks, [0-9]+ failures?" "$raw/host"
explain $st "$raw/host"
report $st

# The sanitizers are the point of the stage above: UBSan caught a signed shift
# overflow in the container parser on the first run it ever did, so a green
# tick without them means less than it appears to.
#
# Checked in the binaries rather than in the Makefile. The Makefile sets both
# flags in one -fsanitize=address,undefined, so a grep for the second as a
# separate string can never match -- which is exactly how this went unnoticed.
stage "the sanitizers were actually linked"
(
    for b in main/script/test/test_script_pack main/script/test/test_script_lua; do
        [ -x "$b" ] || { echo "$b was not built"; exit 1; }
        nm "$b" > "$raw/syms" 2>/dev/null || { echo "nm failed on $b"; exit 1; }
        grep -q '__asan_init' "$raw/syms" || { echo "$b: no ASan runtime"; exit 1; }
        grep -q '__ubsan_handle' "$raw/syms" || { echo "$b: no UBSan runtime"; exit 1; }
        printf '  both runtimes in %s\n' "$(basename "$b")"
    done
) > "$raw/san" 2>&1
st=$?
cat "$raw/san"
report $st

stage "packer selftest"
python3 tools/luapack.py --selftest > "$raw/packer" 2>&1
st=$?
explain $st "$raw/packer"
report $st

# A floor, not a target. Raise it as bindings land; the point is that it
# cannot silently go backwards when someone adds a lisp extension without a
# Lua counterpart.
stage "binding coverage"
python3 tools/script_ext_coverage.py --min-coverage 4 > "$raw/cov" 2>&1
st=$?
grep -E "^covered:" "$raw/cov"
explain $st "$raw/cov"
report $st

# Scoped to the engine's own sources. Vendored Lua is excluded: it is not ours
# to change, and its warnings would drown everything that is.
stage "cppcheck"
if ! command -v cppcheck >/dev/null 2>&1; then
    # Skipped, not failed. A tool that is merely absent must not report the
    # same way as a tool that found something.
    printf '  skipped: no cppcheck\n'
else
    # --check-level=exhaustive, and not only for the extra thoroughness. At
    # the normal level cppcheck 2.18 emits an *informational* message about
    # limiting branch analysis, and --error-exitcode turns that advice into a
    # red stage -- a tool saying it did less work should not report the same
    # as a tool finding a bug. Ubuntu's 2.13 does not emit it at all, so this
    # passed in CI and failed in the pinned shell, which is the version
    # sensitivity the pinning exists to surface.
    #
    # toomanyconfigs is the same kind of notice about #ifdef combinations.
    cppcheck --enable=warning,style,performance,portability \
        --error-exitcode=1 --inline-suppr --quiet \
        --check-level=exhaustive \
        --suppress=missingIncludeSystem --suppress=toomanyconfigs \
        -I main/lua -I main/script \
        -I main/lispBM/utils -I main/display \
        main/script/script_pack.c main/script/script_lua.c \
        main/script/lua_vesc_disp.c main/display/disp_backend.c \
        main/display/ttf_font.c > "$raw/cppcheck" 2>&1
    st=$?
    cat "$raw/cppcheck"
    report $st
fi

# Configuration lives in main/script/.clang-tidy so that running this locally
# and running it in CI cannot disagree.
stage "clang-tidy"
if ! command -v clang-tidy >/dev/null 2>&1; then
    printf '  skipped: no clang-tidy\n'
else
    clang-tidy main/script/script_pack.c main/script/script_lua.c \
        -- -I main/lua -I main/script -std=c11 > "$raw/tidy" 2>&1
    st=$?
    grep -E "warning:|error:" "$raw/tidy" | head -5
    explain $st "$raw/tidy"
    report $st
fi

fi  # fuzz_only

if [ "$fuzz" -eq 1 ]; then
    # The property is not "does not crash" but that every import the parser
    # reports lies inside the blob -- an out-of-range pointer would be a remote
    # read primitive, since the blob arrives over USB or BLE.
    stage "fuzz the container parser (${fuzz_secs}s)"
    if ! command -v clang >/dev/null 2>&1; then
        printf '  skipped: no clang, which libFuzzer needs\n'
    else
        make -C main/script/test fuzz "FUZZ_SECONDS=$fuzz_secs" \
            > "$raw/fuzz" 2>&1
        st=$?
        explain $st "$raw/fuzz"
        report $st
    fi
fi

if [ "$fail" -eq 0 ]; then
    printf '\nall checks passed\n'
else
    printf '\nFAILURES above\n'
fi

exit $fail
