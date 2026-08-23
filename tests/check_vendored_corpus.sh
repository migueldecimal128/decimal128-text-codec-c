#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Drift check for the vendored conformance corpus.
#
# tests/vectors holds this family's corpus of record: four CSVs copied out of
# decimal128-resources, which is where the decimal128-c engine generates them.
# Copies drift silently, so this compares them against a resources checkout.
#
# The error table is EXPECTED to differ, by exactly two rows and no others.
# "NaN(1)" and "NaN(123)" assert a NaN-payload rejection the codecs no longer
# perform, and upstream is deliberately left alone because it is generated and
# the engine is right to reject them (see the header note in the vendored
# file). Anything beyond those two rows is real drift and fails here.
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

# The rows the codecs deliberately do not carry. Matched on the whole quoted
# text field so a row that merely mentions NaN( is unaffected.
EXEMPT='^"NaN\(1\)" ,|^"NaN\(123\)" ,'

FILES='string_bid128_dpd128_100.csv.txt
string_bid128_dpd128_error.csv.txt
string_bid128_dpd128_noncanonical.csv.txt
string_bid128_dpd128_parse.csv.txt'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Data rows only: comments and blank lines carry the prose that legitimately
# differs between the two copies (the vendored error file has the note
# explaining the delta).
datarows() {
    grep -v '^[[:space:]]*#' "$1" | grep '[^[:space:]]' || true
}

status=0
exemptSeen=0

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

    if [ "$f" = "string_bid128_dpd128_error.csv.txt" ]; then
        exemptSeen=$(grep -cE "$EXEMPT" "$tmp/up" || true)
        if [ "$exemptSeen" -ne 2 ]; then
            echo "DRIFT $f: expected the 2 exempt rows upstream, found $exemptSeen" >&2
            echo "       (upstream has been edited -- the corpus is a fixed artifact," >&2
            echo "        its generator is gone, so reread the" >&2
            echo "        header note in the vendored copy before changing anything)" >&2
            status=1
        fi
        grep -vE "$EXEMPT" "$tmp/up" > "$tmp/up.filtered" || true
        mv "$tmp/up.filtered" "$tmp/up"
        if grep -qE "$EXEMPT" "$tmp/ven"; then
            echo "DRIFT $f: the vendored copy carries an exempt row it should not" >&2
            status=1
        fi
    fi

    if ! diff -u "$tmp/up" "$tmp/ven" > "$tmp/diff"; then
        echo "DRIFT $f: vendored copy differs from upstream beyond the exemptions" >&2
        sed -n '1,40p' "$tmp/diff" >&2
        status=1
    else
        echo "ok $f ($(wc -l < "$tmp/ven" | tr -d ' ') rows)"
    fi
done

if [ "$status" -eq 0 ]; then
    echo "vendored corpus matches upstream, with the 2 documented exemptions"
fi
exit "$status"
