# decimal128-text-codec-c

A thin codec between the two IEEE 754-2019 decimal128 interchange encodings —
BID and DPD — and decimal character sequences. Nothing else: no arithmetic, no
comparison, no value type, no context, no status flags.

One translation unit, four exported functions, zero dependencies. The point is
that a caller who holds interchange bits or a string, and wants the other one,
can depend on this without pulling in a full IEEE 754-2019 engine.

For the complete decimal128 implementation see `decimal128-c` at
<https://decimal128.com>.

## API

```c
#include "decimal128_text_codec.h"

char out[D128TEXT_MAX_LEN];
int len = d128text_bid128ToAscii(out, hi, lo);   /* cannot fail */
int len = d128text_dpd128ToAscii(out, hi, lo);

uint64_t hi, lo;
const char *err = d128text_bid128Parse(&hi, &lo, "1.23E+45");   /* NULL = success */
const char *err = d128text_dpd128Parse(&hi, &lo, "1.23E+45");
```

`hi` is the high 64 bits of the encoding, `lo` the low 64, in both directions.
Both parse functions **always write** their out-params — the canonical positive
quiet NaN on failure — so a caller who ignores the return value still observes
a legal IEEE 754-2019 datum rather than stale memory. The error message is
human-readable; its exact wording is not part of the contract.

The renderers write into a buffer the caller owns and return its length, so
nothing allocates. `D128TEXT_MAX_LEN` is 43 — the 42 bytes of the longest
possible output plus the NUL these renderers write; the other ports spell the
same bound 42 and write no terminator.

## Behavior

**Rendering cannot fail.** Every 128-bit pattern maps to a valid decimal
character sequence. A non-canonical BID coefficient (≥ 10³⁴) renders as zero at
the encoded exponent, per IEEE 754-2019 §3.5.2, and a NaN payload ≥ 10³³
renders as no payload. Those patterns never arise from this codec's own encode
direction — they arise from network and file data, which is why the codec
handles them.

**Output is the GDAS `to-scientific-string` form, and only that.** Integer form
when the quantum exponent is zero, plain form when the adjusted exponent is in
`[-6, 0]`, scientific otherwise. The exponent sign is always written:
`6.02E+23`, never `6.02E23`. Cohort information is preserved — `1.00` and `1.0`
are distinct outputs from distinct encodings of the same numerical value, and
nothing is ever normalized or stripped.

**Accepted input** matches what the full `decimal128-{lang}` ports accept, with
one deliberate exception below, so a caller can move between this codec and the
full library without the grammar shifting under them: plain and scientific
notation with `E` or `e`, a leading `+` or `-`, `_` digit separators *between
two digits*, and the case-insensitive words `Infinity`, `Inf`, `NaN`, `qNaN`,
`sNaN` with an optional sign and an optional payload of decimal digits.

**The exception: a NaN payload may be parenthesised** — `NaN(123)` as well as
`NaN123`. C's own `strtod` reads `NAN(n-char-sequence)`, so the spelling
reaches a C caller from the platform itself. The form is positional and
balanced: `(` immediately after the word, at least one digit, `)` last. So
`NaN()`, `NaN(1`, `NaN(1]`, `NaN(1)2(3)`, `NaN (1)`, `NaN[1]` and `NaN{1}` are
all rejected. The resemblance to `strtod` stops at the parentheses — its
sequence is letters, digits and underscores read as an implementation-defined
integer, so `NAN(abc)` and `NAN(0x1c8)` are legal there and rejected here.
Rendering never emits parentheses; `NaN(123)` parses and renders back as
`NaN123`.

The full ports accept the same spelling — `decimal128-c` took it on 2026-08-23,
closing what had been the codecs' one grammar divergence from them.

**Rounding is ties-to-even, hardcoded**, and reachable only from the text →
bits direction: more than 34 significant digits, overflow to Infinity, or
underflow to a subnormal or zero. There is no rounding parameter and no format
selector — the premise is a four-function surface with no configuration. IEEE
754-2019 §5.4.2 places `convertFromDecimalCharacter` under the applicable
rounding-direction attribute; this codec deliberately implements only the
ties-to-even case, and is therefore a conforming **subset** of that operation.
Callers needing another rounding direction want the full library.

## Building

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

C11, no dependencies.

`unsigned __int128` is used when the compiler has it and synthesized from
64-bit limbs when it does not, so the codec builds on MSVC. The
`test_corpus_vectors_portable` target forces the synthesized path on a machine
that has the native type, so the fallback is actually exercised rather than
assumed.

Internal-consistency claims — the ones unreachable for any input — are spelled
`verify` and gated on `-DVERIFY_ENABLED=1`. The default is off, because a
consumer compiles these sources into their own program and must not take an
abort inside a library on data they do not control; the test build arms them.
`demand` — the buffer-sizing guards and the precision invariant that stands in
for the removed precision-truncation step — is live in every configuration.

## Tests

One source file, three CTest targets.

| file | target | what it covers |
|---|---|---|
| `tests/test_corpus_vectors.c` | `test_corpus_vectors` | the four conformance tables over the shared corpus — 10,986 checks |
| | `test_corpus_vectors_portable` | the same, over the no-`__int128` build |
| | `test_corpus_vectors_canonical` | the full 84,783-row canonical table — 689,251 checks |

That is the whole suite, and the shape is deliberate: **every test vector in
this repo comes from the shared corpus.** Nothing in the suite reaches past the
public surface.

An exhaustive unit test of the DPD declet primitives used to sit here too. It
was removed once instrumentation showed the corpus already drives all 1024
declet patterns through `dpdDecodeDeclet` and all 1000 values through
`dpdEncodeDeclet` — the canonical table supplying the 1000 by sheer volume, and
the noncanonical table's 462 declet rows supplying the 24 patterns a canonical
encoder never emits. What went with it is the *statement* that there are
exactly 24: the corpus proves each one decodes correctly without asserting how
many there are.

The repo is standalone: it depends on no decimal128 port, at runtime or in
tests. The independent oracle is the shared `text_bid_dpd` corpus, generated by
the `decimal128-c` engine through its public API; that engine reaches its
answers by different algorithms, so agreement is a cross-check rather than a
tautology.

In-repo vector tables used to sit alongside the corpus here — BID render and
parse vectors, DPD render and parse vectors, the Cowlishaw `dqEncode` /
`dqCanonical` suite, and the contract tests that rode with them. They are gone.
Every input they held that the corpus did not is now a corpus row,
engine-authored, and the Cowlishaw files themselves live in
`decimal128-resources` under `rosetta/dectest`, so nothing is beyond a reader's
reach.

They were worth removing for a reason beyond duplication: the same tables were
carried in this port and its Swift and Kotlin siblings, and a whole vector
shape — `apply #a -> #b` — had silently fallen out of all three copies at once.
No test could have told us; it was found by joining against the source file.

The vectors are read from `resources/text_bid_dpd`, where `resources` is the
`decimal128-resources` submodule — the same mount every decimal128 port uses.
Nothing is copied into this repo, so there is no drift class to police and no
separate corpus of record for this family: upstream is it. The directory is
compiled in as `VECTOR_DIR`.

Two things worth knowing if you touch the tests:

- **The corpus quotes two of its five files.** `parse` and `error` double-quote
  the text column with `\\` and `\"` escapes; the other three leave it bare. A
  naive comma split silently corrupts four rows and still passes, because a
  malformed input fails to parse either way. The reader honours the quoting,
  the row counts are asserted, and `runQuotedColumnTable` checks those four
  rows by hand.
- **The submodule must be checked out.** `cmake` fails configure with a pointed
  message when `resources/text_bid_dpd` is empty, rather than registering a
  corpus suite that quietly tests nothing. Clone with `--recurse-submodules`,
  or run `git submodule update --init --recursive`.

## Provenance

Ported from `decimal128-text-codec-kotlin` (`Decimal128TextCodec.kt` @
`55d5171`), which is the reference implementation for the family. The design
spec is `TextCodecLayer.md` in the `decimal128-workorder` repository.

Where this port differs from the reference, in each case because C asked a
question Kotlin never had to:

- **The boundary is a pair of `uint64_t` words**, `hi` and `lo`, because C has
  no 128-bit integer it can rely on and no string type it can hand back.
- **The renderers write into the caller's buffer** and return a length, rather
  than returning an owned string, so nothing allocates.
- **The widening multiply is a three-way split** — `unsigned __int128` where
  the compiler has it, `__umulh` on MSVC, and a synthesize-from-limbs fallback
  otherwise, with a second build target that exercises the fallback.

The finalizer's precision-truncation step is removed, in this port and in the
reference alike. It descends from a finalizer built for an arithmetic engine,
whose multiply and FMA hand it a double-width product; a codec's only caller is
a parser that accumulates at most 19 + 15 = 34 significant digits and turns
everything past them into a residue. Measured on the C port: zero executions
across the shared corpus, 400,000 random numerals, and 76,280 finalize calls
from inputs of up to 5,000 digits. A dead path is an unverified path, so it is
replaced by an unconditional `demand`. It stays live in every full port's
`Finalize`, where multiply and FMA do reach it; a caller here that is not a
parser needs it back.

The codec stands on the interchange formats alone. It holds no intermediate
value type: parsing yields loose scalars — sign, quantum exponent, coefficient,
residue — which are finalized and written *toward* an encoding. It does not
pivot through any engine's in-memory representation, and must not be changed
to.

## License

MIT.
