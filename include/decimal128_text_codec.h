// SPDX-License-Identifier: MIT
#ifndef DECIMAL128_TEXT_CODEC_H
#define DECIMAL128_TEXT_CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin codec between the two IEEE 754-2019 decimal128 interchange encodings --
 * BID (Binary Integer Decimal) and DPD (Densely Packed Decimal) -- and decimal
 * character sequences.
 *
 * A decimal128 encoding is a pair of 64-bit words: `hi` holds the high 64 bits
 * of the encoding, `lo` the low 64. That is the entire boundary; there is no
 * value type, no context, no status flags.
 *
 * This library provides no arithmetic, no comparison and no field extraction.
 * For the full IEEE 754-2019 decimal128 implementation see decimal128-c at
 * https://decimal128.com.
 *
 * Rounding, where the text -> bits direction requires it, is the IEEE 754-2019
 * roundTiesToEven attribute and is not configurable. Output form is the GDAS
 * to-scientific-string form and is not configurable.
 *
 * All four functions are pure and safe for concurrent use.
 */

/*
 * The exact bound on a rendered decimal128, NOT counting the NUL. The widest
 * scientific form -9.999...E-6143 is 1 sign, 1 digit, 1 radix point, 33 digits,
 * 1 E, 1 sign and 4 exponent digits; the widest plain form -0.000000999... is
 * 1 sign, 1 zero, 1 radix point, 6 leading zeros and 34 digits. Both reach
 * exactly 42.
 *
 * The plain form's six leading zeros are pinned by the GDAS rule that selects
 * it only when the adjusted exponent is at least -6, and the exponent field
 * cannot exceed four digits inside decimal128's range, so neither number can
 * drift without the format itself changing.
 *
 * Every write in the renderers is guarded against this constant. Sizing it
 * exactly rather than generously is what turns those guards into real checks:
 * with slack, an error in the arithmetic above would quietly consume the slack
 * instead of firing.
 *
 * This is the same number, and the same meaning, as MAX_LEN / MaxLen / max_len
 * in the Rust, Go and Zig ports. Those write no terminator, so it is also their
 * buffer size; a C caller adds one for the NUL and declares
 * `char out[D128TEXT_MAX_LEN + 1]`.
 */
#define D128TEXT_MAX_LEN 42

/*
 * Render a BID128 / DPD128 encoding as a decimal character sequence.
 *
 * Writes a NUL-terminated string into `out`, which must have room for
 * D128TEXT_MAX_LEN + 1 bytes, and returns the length not counting the NUL.
 *
 * These cannot fail. Every 128-bit pattern renders: canonical encodings
 * produce their natural form, and a non-canonical BID coefficient (>= 10^34)
 * renders as zero at the encoded exponent per IEEE 754-2019 section 3.5.2.
 *
 * The output preserves cohort information -- "1.00" and "1.0" are distinct
 * outputs from distinct encodings of the same numerical value -- and always
 * writes the exponent sign, as in "6.02E+23".
 */
int d128text_bid128ToString(char *out, uint64_t hi, uint64_t lo);
int d128text_dpd128ToString(char *out, uint64_t hi, uint64_t lo);

/*
 * Parse a decimal character sequence into a BID128 / DPD128 encoding.
 *
 * Returns NULL on success, with the encoding written to *hi and *lo. On
 * failure returns a human-readable message and writes the canonical positive
 * quiet NaN, so a caller who ignores the return value still observes a legal
 * IEEE 754-2019 datum rather than stale memory. The exact wording of the
 * message is not part of the contract and may change between versions.
 *
 * Accepted: plain and scientific notation with either 'E' or 'e'; a leading
 * '+' or '-'; '_' digit separators between two digits; and the case-
 * insensitive words Infinity, Inf, NaN, qNaN and sNaN, each with an optional
 * sign and an optional NaN payload of decimal digits.
 *
 * A NaN payload may also be parenthesised -- NaN(123) as well as NaN123 --
 * matching the spelling C's own strtod reads. The form is positional and
 * balanced: '(' immediately after the word, at least one digit, ')' last;
 * NaN(), NaN(1, NaN(1)2(3), NaN[1] and NaN{1} are rejected. Rendering never
 * emits parentheses. The full decimal128 ports accept the same spelling.
 *
 * More than 34 significant digits round to 34, ties-to-even. Magnitudes above
 * the representable range become Infinity; magnitudes below it become a
 * subnormal or zero. None of those are failures.
 */
const char *d128text_bid128Parse(uint64_t *hi, uint64_t *lo, const char *text);
const char *d128text_dpd128Parse(uint64_t *hi, uint64_t *lo, const char *text);

#ifdef __cplusplus
}
#endif

#endif
