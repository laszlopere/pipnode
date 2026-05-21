#!/usr/bin/env bash
#
# Compile and run all pipnode C unit tests (tests/unit/test-*.c).
#
# These are the headless, in-process node tests: they never touch the
# filesystem, the network, or open a GUI, so this is safe to run on a
# server.  The separate Python functional tests under tests/ — which
# drive the GTK editor over D-Bus and need a display — are NOT run here.
#
# Usage:
#   ./run-unit-tests.sh [-v|--verbose]
#
#   -v, --verbose   pass --verbose to each test for the per-check breakdown
#   -h, --help      show this help
#
# Exit status is non-zero if any test fails.

set -euo pipefail

verbose=0
case "${1:-}" in
    -v|--verbose) verbose=1 ;;
    -h|--help)
        sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    "") ;;
    *)  echo "$0: unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

# Always operate from the repository root (the directory holding this
# script), so the script works regardless of the caller's CWD.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

# --- Bootstrap the build system on a fresh checkout -------------------
# The generated autotools files are not tracked, so regenerate them the
# first time (or after configure.ac / a Makefile.am changes adds the
# tests/unit subdir).
if [ ! -x ./configure ]; then
    echo ">> autoreconf -i"
    autoreconf -i
fi
if [ ! -f tests/unit/Makefile ]; then
    echo ">> ./configure"
    ./configure
fi

# --- Compile: the library first, then the unit-test programs ----------
# `check TESTS=` builds every check_PROGRAM listed in the Makefile but
# runs none of them, so we drive the runs ourselves below (which lets us
# stream --verbose output and tally the result).
echo ">> building libpipnode and unit tests"
build_log="$(mktemp)"
trap 'rm -f "$build_log"' EXIT
if ! { make -C lib && make -C tests/unit check TESTS=; } >"$build_log" 2>&1; then
    echo "!! build failed:" >&2
    cat "$build_log" >&2
    exit 1
fi

# --- Run every discovered test program -------------------------------
pass=0
fail=0
failed=()
shopt -s nullglob
echo ">> running unit tests"
echo "------------------------------------------------------------"
for src in tests/unit/test-*.c; do
    base="$(basename "$src" .c)"
    args=()
    [ "$verbose" -eq 1 ] && args=(--verbose)
    # Run from tests/unit so the libtool wrapper resolves libpipnode.
    if ( cd tests/unit && ./"$base" "${args[@]}" ); then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("$base")
    fi
    echo
done
echo "------------------------------------------------------------"

if [ "$((pass + fail))" -eq 0 ]; then
    echo "no unit tests found under tests/unit/"
    exit 1
fi

echo "unit tests: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
    printf 'failed: %s\n' "${failed[*]}"
    exit 1
fi
