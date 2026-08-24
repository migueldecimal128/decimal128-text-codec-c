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

char out[D128TEXT_MAX];
int len = d128text_bid128ToString(out, hi, lo);   /* cannot fail */
int len = d128text_dpd128ToString(out, hi, lo);

uint64_t hi, lo;
const char *err = d128text_bid128Parse(&hi, &lo, "1.23E+45");   /* NULL = success */
const char *err = d128text_dpd128Parse(&hi, &lo, "1.23E+45");
```

`hi` is the high 64 bits of the encoding, `lo` the low 64, in both directions.
Both parse functions **always write** their out-params — the canonical positive
quiet NaN on failure — so a caller who ignores the return value still observes
a legal IEEE 754-2019 datum rather than stale memory. The error message is
human-readable; its exact wording is not part of the contract.

## Behavior

**Rendering cannot fail.** Every 128-bit pattern maps to a valid decimal
character sequence. A non-canonical BID coefficient (≥ 10³⁴) renders as zero at
the encoded exponent, per IEEE 754-2019 §3.5.2, and a NaN payload ≥ 10³³ reads
as a zero payload. Those patterns never arise from this codec's own encode
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
`NaN123`. C's own `strtod` reads `NAN(n-char-sequence)`, so the spelling reaches
a C caller from the platform itself. The form is positional and balanced: `(`
immediately after the word, at least one digit, `)` last. So `NaN()`, `NaN(1`,
`NaN(1]`, `NaN(1)2(3)`, `NaN (1)`, `NaN[1]` and `NaN{1}` are all rejected. The
resemblance to `strtod` stops at the parentheses — its sequence is letters,
digits and underscores read as an implementation-defined integer, so `NAN(abc)`
and `NAN(0x1c8)` are legal there and rejected here. Rendering never emits
parentheses; `NaN(123)` parses and renders back as `NaN123`.

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

C11, no dependencies. The `demand` buffer-sizing checks are live in every
configuration; the internal-state `verify` assertions default to **off**,
because a consumer compiles these sources into their own program and must not
take an abort inside a library on data they do not control. The test build arms
them with `-DVERIFY_ENABLED=1`.

`unsigned __int128` is used when the compiler has it and synthesized from
64-bit limbs when it does not, so the codec builds on MSVC. The
`test_corpus_vectors_portable` target forces the synthesized path on a machine that has
the native type, so the fallback is actually exercised rather than assumed.

## Tests

Three source files, four CTest targets.

| file | target | what it covers |
|---|---|---|
| `tests/test_corpus_vectors.c` | `test_corpus_vectors` | the four conformance tables over the shared corpus — 10,982 checks |
| | `test_corpus_vectors_portable` | the same, over the no-`__int128` build |
| | `test_corpus_vectors_canonical` | the full 84,783-row canonical table — 689,247 checks |
| `tests/test_dpd_declet.c` | `test_dpd_declet` | the declet codec, exhaustively over all 1024 patterns — 3,048 checks |
| `tests/test_util.h` | — | the CHECK macro and the hex helpers |

That is the whole suite, and the shape is deliberate: **every conformance vector
comes from the shared corpus — there is no hand-written vector left in this
repo.** `test_dpd_declet` is the one test that holds data of its own, and it holds
none: it enumerates the declet primitive's entire 1024-value domain, where no
oracle exists or is required.

The conformance vectors are read from `resources/text_bid_dpd`, where
`resources` is the `decimal128-resources` submodule — the same mount every
decimal128 port uses. Nothing is copied into this repo, so there is no drift
class to police and no separate corpus of record for this family: upstream is
it. The tables were generated by the `decimal128-c` engine through its public
API; that engine reaches its answers by different algorithms, so agreement is a
cross-check rather than a tautology.

Hand-written vector tables used to sit alongside the corpus here — BID render
and parse vectors, DPD render and parse vectors, the Cowlishaw `dqEncode` /
`dqCanonical` suite, and the contract tests that rode with them. They are gone.
Every input they held that the corpus did not is now a corpus row,
engine-authored, and the Cowlishaw files themselves live in
`decimal128-resources` under `rosetta/dectest`, so nothing is beyond a reader's
reach.

They were worth removing for a reason beyond duplication: the same tables were
transcribed by hand into this port and its Swift and Kotlin siblings, and a
whole vector shape — `apply #a -> #b` — had silently fallen out of all three
copies at once. No test could have told us; it was found by joining against the
source file.

Three things worth knowing if you touch the tests:

- **The corpus quotes two of its five files.** `parse` and `error` double-quote
  the text column with `\\` and `\"` escapes; the other three leave it bare. A
  naive comma split silently corrupts four rows and still passes, because a
  malformed input fails to parse either way. The reader honours the quoting and
  the row counts are asserted.
- **The submodule must be checked out.** `cmake` fails configure with a
  pointed message when `resources/text_bid_dpd` is empty, rather than
  registering a corpus suite that quietly tests nothing. Clone with
  `--recurse-submodules`, or run `git submodule update --init --recursive`.
- **`u113DivModPow10Max9` is not directly covered.** The corpus barely reaches
  it, and a dedicated kernel test existed and was removed. The codec can be
  green on the entire corpus with that kernel substantially wrong.

## Provenance

Ported from `decimal128-text-codec-kotlin` (`Decimal128TextCodec.kt` @
`55d5171`), which is the reference implementation for the family. The design
spec is `TextCodecLayer.md` in the `decimal128-workorder` repository.

The finalizer's precision-truncation step is removed, in this port and in the
reference alike. It descends from a finalizer built for an arithmetic engine,
whose multiply and FMA hand it a double-width product; a codec's only caller is
the parser, which never accumulates more than 34 significant digits, so the step
never runs. An unconditional `demand` enforces that rather than shipping a path
nothing has ever exercised. It stays live in every full port's `Finalize`, where
multiply and FMA do reach it; a caller here that is not a parser needs it back.

The codec stands on the interchange formats alone. It holds no intermediate
value type: parsing yields loose scalars — sign, quantum exponent, coefficient,
residue — which are finalized and written *toward* an encoding. It does not
pivot through any engine's in-memory representation, and must not be changed to.

Beyond the committed suite, the port was checked against the `decimal128-c`
engine on 300,000 random numerals (accept/reject, BID bits, DPD bits and
rendered text) and 300,000 random 128-bit patterns (both render directions),
with zero divergence. Those differentials are not in the repository because
they would give a zero-dependency library a dependency on the engine.

## License

MIT.
