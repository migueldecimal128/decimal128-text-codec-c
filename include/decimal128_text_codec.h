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
 * A decimal128 encoding is 128 bits: a sign bit, a combination field and a
 * trailing significand field, laid out per IEEE 754-2019 section 3.5.2. It
 * crosses this API as a pair of uint64_t words, hi and lo. That is the entire
 * boundary; there is no value type, no context, no status flags.
 *
 * This codec provides no arithmetic, no comparison and no field extraction.
 * For the full IEEE 754-2019 decimal128 implementation -- arithmetic,
 * comparison, every rounding-direction attribute, IEEE status flags -- see
 * https://decimal128.com.
 *
 * Rounding
 *
 * Rounding, where the text -> bits direction requires it, is the IEEE 754-2019
 * roundTiesToEven attribute and is not configurable. IEEE 754-2019 section
 * 5.4.2 places convertFromDecimalCharacter under the applicable
 * rounding-direction attribute; this codec deliberately implements only the
 * ties-to-even case and is therefore a conforming subset of that operation.
 * Callers needing another rounding direction want the full library.
 *
 * Output form is the GDAS to-scientific-string form and is not configurable.
 * Cohort information is preserved: "1.00" and "1.0" are distinct outputs from
 * distinct encodings of the same numerical value.
 *
 * Round-trip behavior
 *
 * The renderer's output round-trips exactly: parsing rendered text and
 * rendering it again reproduces that text, and rendering a canonical encoding
 * and parsing the result reproduces the original bits. Three classes of input
 * do not round-trip:
 *
 * - More than 34 significant digits are accepted and rounded to 34 with
 *   roundTiesToEven. The rounded value is encoded; the input text is not
 *   preserved.
 * - Magnitudes outside the representable range overflow to Infinity or
 *   underflow to a subnormal or zero, per IEEE 754-2019.
 * - Non-canonical bit patterns render as something the encoder would never
 *   have emitted, so re-parsing yields the canonical pattern instead.
 *
 * Non-canonical bit patterns
 *
 * Not every 128-bit pattern is canonical. Each encoding has its own
 * non-canonical forms -- patterns the encoder would never emit -- and what
 * makes a pattern non-canonical is a property of that encoding, documented
 * where that encoding is decoded.
 *
 * Neither renderer can fail on one: every 128-bit pattern renders as the value
 * IEEE 754-2019 assigns it. Decoding is one-way for such a pattern, because
 * the parsers emit only canonical encodings, so re-parsing the rendered text
 * yields bits that differ from the original. Non-canonical patterns never
 * arise from this codec's own parsers; they appear only when encodings come
 * from outside, such as network or file data.
 *
 * Thread safety
 *
 * All functions are pure and safe for concurrent use.
 *
 * In this port
 *
 * Nothing here allocates. The renderers write a NUL-terminated string into a
 * caller buffer of D128TEXT_MAX_LEN bytes and return its length; the parsers
 * return NULL on success, or a human-readable message on failure. The library
 * is one translation unit with no dependency beyond the C standard library.
 */

/*
 * The exact bound on a rendered decimal128, in ASCII character bytes.
 *
 * The widest scientific form -9.999...E-6143 is 1 sign, 1 digit, 1 radix
 * point, 33 digits, 1 E, 1 sign and 4 exponent digits; the widest plain form
 * -0.00000999... is 1 sign, 1 zero, 1 radix point, 5 leading zeros and 34
 * digits. Each form reaches exactly 42.
 *
 * The plain form's five leading zeros are pinned by the GDAS rule that selects
 * it only when the adjusted exponent is at least -6, which puts five zeros
 * between the radix point and the first significant digit. The exponent field
 * cannot exceed four digits inside decimal128's range, so neither number can
 * drift without the format itself changing.
 *
 * Every write in the renderers is guarded against this bound. Sizing it
 * exactly rather than generously is what turns those guards into real checks:
 * with slack, an error in the arithmetic above would quietly consume the slack
 * instead of firing.
 *
 * This constant is 43: the 42 bytes of text plus the NUL that the C renderers
 * write. The other ports spell the same bound 42 and write no terminator,
 * handing back a length or a view instead.
 */
#define D128TEXT_MAX_LEN 43

/*
 * Render a BID128 encoding into a caller-supplied buffer, allocating nothing.
 *
 * Writes a NUL-terminated string into out, which must have room for
 * D128TEXT_MAX_LEN bytes, and returns the length not counting the NUL.
 *
 * The output is the GDAS to-scientific-string form: integer notation when the
 * quantum exponent is zero (e.g. "123"), plain notation when the adjusted
 * exponent is in the range -6 to 0 (e.g. "123.45"), normalized scientific
 * notation otherwise (e.g. "1.234E+5"). The exponent sign is always written,
 * as in "6.02E+23". Cohort information is preserved: "1.00" and "1.0" are
 * distinct outputs from distinct encodings of the same numerical value.
 *
 * Special values render as "Infinity", "-Infinity", "NaN" and "sNaN", each
 * with an optional sign and an optional payload suffix such as "NaN123".
 *
 * The two interchange encodings differ only in how the coefficient is stored:
 * BID128 as a binary integer, DPD128 as a leading digit plus eleven 10-bit
 * declets. The sign and the 14-bit biased exponent are the same in both, and
 * peer encodings of the same value render to identical text.
 *
 * This cannot fail. Every 128-bit pattern renders: a non-canonical BID128
 * coefficient (>= 10^34) renders as zero at the encoded exponent, and a NaN
 * payload >= 10^33 renders as no payload, both per IEEE 754-2019 section
 * 3.5.2.
 */
int d128text_bid128ToAscii(char *out, uint64_t hi, uint64_t lo);

/*
 * Render a DPD128 encoding into a caller-supplied buffer, allocating nothing.
 *
 * Writes a NUL-terminated string into out, which must have room for
 * D128TEXT_MAX_LEN bytes, and returns the length not counting the NUL.
 *
 * The output is the GDAS to-scientific-string form: integer notation when the
 * quantum exponent is zero (e.g. "123"), plain notation when the adjusted
 * exponent is in the range -6 to 0 (e.g. "123.45"), normalized scientific
 * notation otherwise (e.g. "1.234E+5"). The exponent sign is always written,
 * as in "6.02E+23". Cohort information is preserved: "1.00" and "1.0" are
 * distinct outputs from distinct encodings of the same numerical value.
 *
 * Special values render as "Infinity", "-Infinity", "NaN" and "sNaN", each
 * with an optional sign and an optional payload suffix such as "NaN123".
 *
 * The two interchange encodings differ only in how the coefficient is stored:
 * BID128 as a binary integer, DPD128 as a leading digit plus eleven 10-bit
 * declets. The sign and the 14-bit biased exponent are the same in both, and
 * peer encodings of the same value render to identical text.
 *
 * This cannot fail. Every 128-bit pattern renders: a DPD128 coefficient is
 * always in range, since a leading digit of at most 9 followed by eleven
 * declets cannot reach 10^34; a NaN payload, carried in those same declets, is
 * likewise always in range; and non-canonical declets decode to their value,
 * per IEEE 754-2019 section 3.5.2.
 */
int d128text_dpd128ToAscii(char *out, uint64_t hi, uint64_t lo);

/*
 * Parse a decimal character sequence into a BID128 encoding.
 *
 * Returns NULL on success, with the encoding written to *hi and *lo. On
 * failure returns a human-readable message and writes the canonical positive
 * quiet NaN, so a caller who ignores the return value still observes a legal
 * IEEE 754-2019 datum rather than stale memory. The exact wording of the
 * message is not part of the contract and may change between versions.
 *
 * Accepted input:
 *
 * - Plain notation: "0", "-1.5", "123.456", "0.0001".
 * - Scientific notation: "1.5E10", "-2.3e-7", "6.02E+23". Both E and e are
 *   accepted as the exponent separator.
 * - Digit separators: _ between two digits, in the coefficient or the exponent
 *   ("1_000", "1.234_567", "1e1_0"). An underscore must be both preceded and
 *   followed by a digit, so "1_", "_1", "1__2", "1_.5" and "1._5" are all
 *   rejected.
 * - The case-insensitive words Infinity, Inf, NaN, qNaN and sNaN, each with an
 *   optional sign and an optional NaN payload of decimal digits ("NaN123",
 *   "-sNaN42"). A payload may also be parenthesised, as "NaN(123)".
 *
 * The parenthesised payload form is positional and balanced: ( immediately
 * after the word, at least one digit, ) last. "NaN()", "NaN(1", "NaN(1)2(3)",
 * "NaN[1]" and "NaN{1}" are rejected. Rendering never emits parentheses, and
 * the full decimal128 ports accept the same spelling.
 *
 * More than 34 significant digits round to 34, ties-to-even. Magnitudes above
 * the representable range become Infinity; magnitudes below it become a
 * subnormal or zero. None of those are failures.
 *
 * Failures are structural: an empty sequence, unrecognized characters, missing
 * digits around the radix point, a malformed exponent field.
 *
 * The two interchange encodings differ only in how the coefficient is stored:
 * BID128 as a binary integer, DPD128 as a leading digit plus eleven 10-bit
 * declets. The same text parses to peer encodings of the same value in either
 * format, and the canonical positive quiet NaN is the same bit pattern in
 * both.
 */
const char *d128text_bid128Parse(uint64_t *hi, uint64_t *lo, const char *text);

/*
 * Parse a decimal character sequence into a DPD128 encoding.
 *
 * Returns NULL on success, with the encoding written to *hi and *lo. On
 * failure returns a human-readable message and writes the canonical positive
 * quiet NaN, so a caller who ignores the return value still observes a legal
 * IEEE 754-2019 datum rather than stale memory. The exact wording of the
 * message is not part of the contract and may change between versions.
 *
 * Accepted input:
 *
 * - Plain notation: "0", "-1.5", "123.456", "0.0001".
 * - Scientific notation: "1.5E10", "-2.3e-7", "6.02E+23". Both E and e are
 *   accepted as the exponent separator.
 * - Digit separators: _ between two digits, in the coefficient or the exponent
 *   ("1_000", "1.234_567", "1e1_0"). An underscore must be both preceded and
 *   followed by a digit, so "1_", "_1", "1__2", "1_.5" and "1._5" are all
 *   rejected.
 * - The case-insensitive words Infinity, Inf, NaN, qNaN and sNaN, each with an
 *   optional sign and an optional NaN payload of decimal digits ("NaN123",
 *   "-sNaN42"). A payload may also be parenthesised, as "NaN(123)".
 *
 * The parenthesised payload form is positional and balanced: ( immediately
 * after the word, at least one digit, ) last. "NaN()", "NaN(1", "NaN(1)2(3)",
 * "NaN[1]" and "NaN{1}" are rejected. Rendering never emits parentheses, and
 * the full decimal128 ports accept the same spelling.
 *
 * More than 34 significant digits round to 34, ties-to-even. Magnitudes above
 * the representable range become Infinity; magnitudes below it become a
 * subnormal or zero. None of those are failures.
 *
 * Failures are structural: an empty sequence, unrecognized characters, missing
 * digits around the radix point, a malformed exponent field.
 *
 * The two interchange encodings differ only in how the coefficient is stored:
 * BID128 as a binary integer, DPD128 as a leading digit plus eleven 10-bit
 * declets. The same text parses to peer encodings of the same value in either
 * format, and the canonical positive quiet NaN is the same bit pattern in
 * both.
 */
const char *d128text_dpd128Parse(uint64_t *hi, uint64_t *lo, const char *text);

#ifdef __cplusplus
}
#endif

#endif
