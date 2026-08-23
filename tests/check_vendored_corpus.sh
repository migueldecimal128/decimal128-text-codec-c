#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Drift check for the vendored conformance corpus.
#
# tests/vectors holds this family's corpus of record: four CSVs copied out of
# decimal128-resources, which is where the decimal128-c engine generates them.
# Copies drift silently, so this compares them against a resources checkout.
#
# This is a plain exact comparison: every data row must match, in order. It used
# to exempt "NaN(1)" and "NaN(123)", which upstream rejected and the codecs
# accepted; the full ports took the parenthesised payload in 2026-08-23 and
# upstream moved both rows to the parse table, so there is nothing left to
# exempt.
#
#   usage: check_vendored_corpus.sh <resources/string_bid_dpd> <tests/vectors>
#
# Exit 0 clean, 1 on drift, 2 on a usage or path error.

set -eu

UPSTREAM=${1:-}
VENDORED=${2:-}

if [ -z "$UPSTREAM" ] || [ -z "$VENDORED" ]; then
    echo "usage: $0 <resources/string_bid_dpd dir> <vendored dir>" >&2
    exit 2
fi
for d in "$UPSTREAM" "$VENDORED"; do
    if [ ! -d "$d" ]; then
        echo "not a directory: $d" >&2
        exit 2
    fi
done

FILES='string_bid128_dpd128_100.csv.txt
string_bid128_dpd128_error.csv.txt
string_bid128_dpd128_noncanonical.csv.txt
string_bid128_dpd128_parse.csv.txt'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Data rows only. The two copies are byte-identical today, but comparing data
# rows keeps the check meaningful if upstream ever regenerates its header
# (a Generated: date, a row-count line) without changing a single vector.
datarows() {
    grep -v '^[[:space:]]*#' "$1" | grep '[^[:space:]]' || true
}

status=0

for f in $FILES; do
    up="$UPSTREAM/$f"
    ven="$VENDORED/$f"
    if [ ! -f "$up" ]; then
        echo "MISSING upstream: $up" >&2
        status=1
        continue
    fi
    if [ ! -f "$ven" ]; then
        echo "MISSING vendored: $ven" >&2
        status=1
        continue
    fi

    datarows "$up" > "$tmp/up"
    datarows "$ven" > "$tmp/ven"

    if ! diff -u "$tmp/up" "$tmp/ven" > "$tmp/diff"; then
        echo "DRIFT $f: vendored copy differs from upstream" >&2
        sed -n '1,40p' "$tmp/diff" >&2
        status=1
    else
        echo "ok $f ($(wc -l < "$tmp/ven" | tr -d ' ') rows)"
    fi
done

if [ "$status" -eq 0 ]; then
    echo "vendored corpus matches upstream exactly"
fi
exit "$status"
