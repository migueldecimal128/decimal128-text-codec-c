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
 * Buffer size for the two ToString functions, NUL included. The longest
 * possible output is 43 bytes with its terminator (a plain-form value: sign,
 * six leading zeros, radix point and 34 digits); this is that bound with
 * headroom, so a caller never has to think about it.
 */
#define D128TEXT_MAX 64

/*
 * Render a BID128 / DPD128 encoding as a decimal character sequence.
 *
 * Writes a NUL-terminated string into `out`, which must have room for
 * D128TEXT_MAX bytes, and returns the length not counting the NUL.
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
