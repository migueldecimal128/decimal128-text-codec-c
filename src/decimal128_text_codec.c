// SPDX-License-Identifier: MIT
/*
 * decimal128-text-codec-c
 *
 * Ported from decimal128-text-codec-kotlin (Decimal128TextCodec.kt @ 55d5171),
 * which is the reference implementation for the family. The shape follows it
 * closely enough to be read side by side; where C differs, the differences are
 * noted at the point they occur. The three deliberate ones:
 *
 *  - dwords are `uint64_t`, not the reference's signed `Long`. The Kotlin
 *    carries unsignedMulHi / unsignedLT / ushr only because the JVM has no
 *    unsigned 64-bit type, and signed wrap would be undefined here anyway --
 *    a consumer compiles these sources with their own flags, so the codec
 *    cannot lean on -fwrapv the way the full port does.
 *  - the renderers write into a caller buffer and return a length, rather
 *    than allocating an exactly-sized array and returning a String.
 *  - the parse direction reads through a NUL-returning accessor that never
 *    steps past the terminator, which is how the Kotlin's getOrNul sentinel
 *    contract translates to a C string.
 *
 * Everything reachable from outside is the four functions in the header; the
 * rest is static.
 */

#include "decimal128_text_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal-state assertions
 *
 * Two mechanisms, and the split matters more in C than it does in Kotlin.
 *
 *   verify(cond)  internal-consistency claims -- unreachable for ANY input, so
 *                 a failure is a codec bug rather than bad data. Compiled out
 *                 unless VERIFY_ENABLED is set, and the default is OFF: a
 *                 consumer compiles these sources into their own program and
 *                 must not take an abort inside a library on data they do not
 *                 control. The test build arms it with -DVERIFY_ENABLED=1.
 *
 *   demand(cond)  buffer-sizing preconditions, which guard a write into the
 *                 caller's buffer. In Kotlin the equivalent violation throws
 *                 IndexOutOfBounds; here it would be a buffer overrun, so
 *                 these stay live in every configuration.
 * ------------------------------------------------------------------------ */

#ifndef VERIFY_ENABLED
#define VERIFY_ENABLED 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CODEC_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define CODEC_NORETURN __declspec(noreturn)
#else
#define CODEC_NORETURN _Noreturn
#endif

CODEC_NORETURN static void codecAbort(const char *what) {
    fputs("decimal128-text-codec: impossible state: ", stderr);
    fputs(what, stderr);
    fputc('\n', stderr);
    abort();
}

#define demand(cond)                     \
    do {                                 \
        if (!(cond))                     \
            codecAbort(#cond);           \
    } while (0)

#if VERIFY_ENABLED
#define verify(cond) demand(cond)
#else
#define verify(cond) ((void)0)
#endif

/* --------------------------------------------------------------------------
 * Format constants -- IEEE 754-2019 Table 3.6, decimal128
 * ------------------------------------------------------------------------ */

#define BIAS 6176
#define Q_MIN (-6176) /* the minimum quantum exponent */
#define Q_MAX 6111    /* the maximum quantum exponent */
#define T_WIDTH 110   /* trailing significand field width in bits */
#define W5 17         /* combination field width in bits */

/* 10^34, one past the largest coefficient; maxx = max eXclusive */
#define MAXX_COEFF_34_HI 0x0001ED09BEAD87C0ULL
#define MAXX_COEFF_34_LO 0x378D8E6400000000ULL

/* 10^33, the smallest 34-digit coefficient */
#define MIN_PRECISION_34_COEFF_HI 0x0000314DC6448D93ULL
#define MIN_PRECISION_34_COEFF_LO 0x38C15B0A00000000ULL

/* 33 nines -- the largest canonical NaN payload */
#define MAX_PAYLOAD_HI MIN_PRECISION_34_COEFF_HI
#define MAX_PAYLOAD_LO (MIN_PRECISION_34_COEFF_LO - 1ULL)

#define SIGN_BIT 0x8000000000000000ULL
#define INF_PATTERN_HI 0x7800000000000000ULL
#define QNAN_PATTERN_HI 0x7C00000000000000ULL
#define SNAN_PATTERN_HI 0x7E00000000000000ULL

#define TEN_POW_15 1000000000000000ULL
#define TEN_POW_18 1000000000000000000ULL

/* --------------------------------------------------------------------------
 * Low-level unsigned arithmetic
 * ------------------------------------------------------------------------ */

/*
 * High 64 bits of the 128-bit product x * y. The portable branch is the
 * reference's four-partial-product decomposition, which is also the MSVC
 * synthesize-from-limbs path on targets without __umulh.
 */
static inline uint64_t mulhi64(uint64_t x, uint64_t y) {
#if defined(__SIZEOF_INT128__) && !defined(D128TEXT_NO_INT128)
    return (uint64_t)(((unsigned __int128)x * (unsigned __int128)y) >> 64);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return __umulh(x, y);
#else
    uint64_t xLo = x & 0xFFFFFFFFULL;
    uint64_t xHi = x >> 32;
    uint64_t yLo = y & 0xFFFFFFFFULL;
    uint64_t yHi = y >> 32;

    uint64_t pp00 = xLo * yLo;
    uint64_t pp01 = xHi * yLo;
    uint64_t pp10 = xLo * yHi;
    uint64_t pp11 = xHi * yHi;

    uint64_t mid = pp01 + pp10; /* may wrap */
    uint64_t midCarryShifted = (mid < pp01) ? (1ULL << 32) : 0ULL;
    uint64_t midWithLo = (pp00 >> 32) + (mid & 0xFFFFFFFFULL);

    return pp11 + (mid >> 32) + midCarryShifted + (midWithLo >> 32);
#endif
}

/* Leading zero count, 64 for zero -- matching Kotlin's countLeadingZeroBits. */
static inline int clz64(uint64_t v) {
    if (v == 0)
        return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#else
    int n = 0;
    if ((v >> 32) == 0) { n += 32; v <<= 32; }
    if ((v >> 48) == 0) { n += 16; v <<= 16; }
    if ((v >> 56) == 0) { n += 8; v <<= 8; }
    if ((v >> 60) == 0) { n += 4; v <<= 4; }
    if ((v >> 62) == 0) { n += 2; v <<= 2; }
    if ((v >> 63) == 0) { n += 1; }
    return n;
#endif
}

/*
 * Powers of 10 from 10^0 through 10^34 as 128-bit values in little-endian
 * order: POW10[2*i] is the low 64 bits of 10^i, POW10[2*i + 1] the high 64.
 *
 * Sized to the coefficient bound and indexed directly -- no mask, no padding.
 * Every reachable index is <= 34: the binding case is calcDigitLen128, whose
 * input is always a coefficient or NaN payload below 10^34, so bitLen <= 113
 * and (bitLen * 1233) >> 12 <= 34. It doubles as the 5^n source for the
 * divide kernel, since 10^n >> n is exactly 5^n; there is no POW5 table.
 */
static const uint64_t POW10[(34 + 1) * 2] = {
    1ULL, 0ULL,                                   /* 10^0  */
    0x000000000000000AULL, 0ULL,                  /* 10^1  */
    0x0000000000000064ULL, 0ULL,                  /* 10^2  */
    0x00000000000003E8ULL, 0ULL,                  /* 10^3  */
    0x0000000000002710ULL, 0ULL,                  /* 10^4  */
    0x00000000000186A0ULL, 0ULL,                  /* 10^5  */
    0x00000000000F4240ULL, 0ULL,                  /* 10^6  */
    0x0000000000989680ULL, 0ULL,                  /* 10^7  */
    0x0000000005F5E100ULL, 0ULL,                  /* 10^8  */
    0x000000003B9ACA00ULL, 0ULL,                  /* 10^9  */
    0x00000002540BE400ULL, 0ULL,                  /* 10^10 */
    0x000000174876E800ULL, 0ULL,                  /* 10^11 */
    0x000000E8D4A51000ULL, 0ULL,                  /* 10^12 */
    0x000009184E72A000ULL, 0ULL,                  /* 10^13 */
    0x00005AF3107A4000ULL, 0ULL,                  /* 10^14 */
    0x00038D7EA4C68000ULL, 0ULL,                  /* 10^15 */
    0x002386F26FC10000ULL, 0ULL,                  /* 10^16 */
    0x016345785D8A0000ULL, 0ULL,                  /* 10^17 */
    0x0DE0B6B3A7640000ULL, 0ULL,                  /* 10^18 */
    0x8AC7230489E80000ULL, 0ULL,                  /* 10^19 */
    0x6BC75E2D63100000ULL, 0x0000000000000005ULL, /* 10^20 */
    0x35C9ADC5DEA00000ULL, 0x0000000000000036ULL, /* 10^21 */
    0x19E0C9BAB2400000ULL, 0x000000000000021EULL, /* 10^22 */
    0x02C7E14AF6800000ULL, 0x000000000000152DULL, /* 10^23 */
    0x1BCECCEDA1000000ULL, 0x000000000000D3C2ULL, /* 10^24 */
    0x161401484A000000ULL, 0x0000000000084595ULL, /* 10^25 */
    0xDCC80CD2E4000000ULL, 0x000000000052B7D2ULL, /* 10^26 */
    0x9FD0803CE8000000ULL, 0x00000000033B2E3CULL, /* 10^27 */
    0x3E25026110000000ULL, 0x00000000204FCE5EULL, /* 10^28 */
    0x6D7217CAA0000000ULL, 0x00000001431E0FAEULL, /* 10^29 */
    0x4674EDEA40000000ULL, 0x0000000C9F2C9CD0ULL, /* 10^30 */
    0xC0914B2680000000ULL, 0x0000007E37BE2022ULL, /* 10^31 */
    0x85ACEF8100000000ULL, 0x000004EE2D6D415BULL, /* 10^32 */
    0x38C15B0A00000000ULL, 0x0000314DC6448D93ULL, /* 10^33 */
    0x378D8E6400000000ULL, 0x0001ED09BEAD87C0ULL, /* 10^34 */
};

/* Bit length of a 128-bit value: highest set bit plus one, 0 for zero. */
static inline int calcBitLen128(uint64_t dw1, uint64_t dw0) {
    if (dw1 != 0)
        return 128 - clz64(dw1);
    if (dw0 != 0)
        return 64 - clz64(dw0);
    return 0;
}

/*
 * Decimal digit count of a 128-bit value.
 *
 * (bitLen * 1233) >> 12 is floor(bitLen * log10(2)), which is either the exact
 * digit count or one too small; a single unsigned compare against the
 * next-larger power of ten resolves the off-by-one.
 */
static inline int calcDigitLen128(uint64_t dw1, uint64_t dw0) {
    int bitLen = calcBitLen128(dw1, dw0);
    int loDigitCount = (bitLen * 1233) >> 12;
    int hiDigitCount = loDigitCount + 1;
    int pow10Offset = loDigitCount << 1;
    uint64_t p1 = POW10[pow10Offset + 1];
    uint64_t p0 = POW10[pow10Offset];
    if (dw1 != p1)
        return (dw1 < p1) ? hiDigitCount - 1 : hiDigitCount;
    return (dw0 < p0) ? hiDigitCount - 1 : hiDigitCount;
}

/*
 * Multiply a 64-bit value by 10^pow10 and add a; the 128-bit result goes to
 * *outHi / *outLo.
 */
static inline void u128FmaPow10(uint64_t *outHi, uint64_t *outLo, uint64_t x, int pow10, uint64_t a) {
    uint64_t y = POW10[pow10 << 1];
    uint64_t prodLo = x * y;
    uint64_t prodHi = mulhi64(x, y);
    uint64_t sumLo = prodLo + a;
    *outHi = prodHi + ((sumLo < prodLo) ? 1ULL : 0ULL);
    *outLo = sumLo;
}

/* --------------------------------------------------------------------------
 * The divide kernel
 * ------------------------------------------------------------------------ */

/*
 * Divide a 113-bit value by 10^pow10 for pow10 in 1..9, returning the
 * remainder and writing the quotient to *qHi / *qLo.
 *
 * Table-free: 10^pow10 factors as 2^pow10 * 5^pow10, so the low pow10 bits
 * fall straight out as the low part of the remainder and the shifted value is
 * divided by 5^pow10 with two hardware 64-bit divides.
 */
static uint64_t u113DivModPow10Max9(uint64_t *qHi, uint64_t *qLo, uint64_t dwHi, uint64_t dwLo, int pow10) {
    verify(pow10 >= 1 && pow10 <= 9);
    verify(clz64(dwHi) >= 15);

    uint64_t mask = (1ULL << pow10) - 1;
    uint64_t b = dwLo & mask;
    uint64_t sHi = dwHi >> pow10;
    uint64_t sLo = (dwLo >> pow10) | (dwHi << (64 - pow10));

    int k = 50 - pow10;
    uint64_t maskK = (1ULL << k) - 1;
    uint64_t hi63 = (sHi << (14 + pow10)) | (sLo >> k);
    uint64_t loK = sLo & maskK;

    /* two slots per power in POW10, hence the doubled index */
    uint64_t d = POW10[pow10 << 1] >> pow10; /* = 5^pow10 */
    uint64_t q1 = hi63 / d;
    uint64_t r1 = hi63 % d;
    uint64_t mid = (r1 << k) | loK;
    uint64_t q0 = mid / d;
    uint64_t r0 = mid % d;

    *qLo = (q1 << k) + q0;
    *qHi = q1 >> (14 + pow10);
    return (r0 << pow10) | b;
}

/* --------------------------------------------------------------------------
 * Residue -- how much was discarded, relative to half a ulp
 * ------------------------------------------------------------------------ */

#define EXACT 0
#define LT_HALF 1
#define HALF 2
#define GT_HALF 3

/* Two bits per digit 0..9: 0 -> EXACT, 1..4 -> LT_HALF, 5 -> HALF, 6..9 -> GT_HALF */
#define DIGIT_MAP 0xFF954ULL

static inline int residueFromDecimalDigit(int digit) {
    return (int)((DIGIT_MAP >> (digit << 1)) & 0x03ULL);
}

/*
 * Combine a primary residue with a sticky one from less-significant digits,
 * round-bit-and-sticky-bit style: the sticky residue collapses to a single
 * nonzero bit which is OR'd into the low bit of the round residue.
 */
static inline int residueMerge(int roundResidue, int stickyResidue) {
    int s = (stickyResidue & 1) | (stickyResidue >> 1);
    return (roundResidue | s) & 0x03;
}

/* Classify a 128-bit value relative to half of 10^pow10. */
static int residueFromValuePow10(uint64_t dw1, uint64_t dw0, int pow10) {
    if ((dw1 | dw0) == 0)
        return EXACT;
    int pow10Index = pow10 << 1;
    uint64_t pow10Hi = POW10[pow10Index + 1];
    uint64_t pow10Lo = POW10[pow10Index];

    uint64_t halfHi = pow10Hi >> 1;
    uint64_t halfLo = (pow10Hi << 63) | (pow10Lo >> 1);
    if (dw1 != halfHi)
        return (dw1 < halfHi) ? LT_HALF : GT_HALF;
    if (dw0 != halfLo)
        return (dw0 < halfLo) ? LT_HALF : GT_HALF;
    return HALF;
}

/*
 * Divide a 128-bit coefficient by 10^truncationNeeded, in chunks of at most
 * nine digits, returning the accumulated residue class.
 */
static int u128ScaleDownPow10(uint64_t *outHi, uint64_t *outLo, uint64_t dw1, uint64_t dw0, int truncationNeeded) {
    verify((dw1 | dw0) != 0);
    verify(truncationNeeded > 0 && truncationNeeded < calcDigitLen128(dw1, dw0));
    int scaleResidue = EXACT;
    uint64_t dw1T = dw1;
    uint64_t dw0T = dw0;
    int digitsRemaining = truncationNeeded;
    do {
        int stepPow10 = (digitsRemaining < 9) ? digitsRemaining : 9;
        uint64_t rem = u113DivModPow10Max9(&dw1T, &dw0T, dw1T, dw0T, stepPow10);
        int stepResidue = residueFromValuePow10(0, rem, stepPow10);
        scaleResidue = residueMerge(stepResidue, scaleResidue);
        digitsRemaining -= stepPow10;
    } while (digitsRemaining > 0);
    *outHi = dw1T;
    *outLo = dw0T;
    return scaleResidue;
}

/* --------------------------------------------------------------------------
 * DPD declet conversion
 *
 * A declet is 10 bits holding three decimal digits, 0..999. This is the one
 * place BID and DPD genuinely differ: BID stores the 34-digit coefficient as a
 * binary integer, DPD as a leading digit plus eleven declets.
 *
 * Both directions are pure boolean arithmetic (Cowlishaw's dpd2bcd and
 * bcd2dpd) rather than lookup tables, matching the reference.
 * ------------------------------------------------------------------------ */

/* Decode a 10-bit declet (bits p q r s t u v w x y) into 0..999. Accepts the
 * 24 non-canonical declets as well as the 1000 canonical ones. */
static uint64_t dpdDecodeDeclet(uint64_t declet) {
    verify(declet <= 1023);

    if (declet == 0)
        return 0;

    uint64_t p = (declet >> 9) & 1;
    uint64_t q = (declet >> 8) & 1;
    uint64_t r = (declet >> 7) & 1;
    uint64_t s = (declet >> 6) & 1;
    uint64_t t = (declet >> 5) & 1;
    uint64_t u = (declet >> 4) & 1;
    uint64_t v = (declet >> 3) & 1;
    uint64_t w = (declet >> 2) & 1;
    uint64_t x = (declet >> 1) & 1;
    uint64_t y = declet & 1;

    uint64_t ns = s ^ 1;
    uint64_t nx = x ^ 1;
    uint64_t nv = v ^ 1;
    uint64_t nw = w ^ 1;
    uint64_t nt = t ^ 1;

    uint64_t a = (v & w) & (ns | t | nx);
    uint64_t b = p & (nv | nw | (s & ~t & x));
    uint64_t c = q & (nv | nw | (s & ~t & x));
    uint64_t d = r;

    uint64_t e = v & ((nw & x) | (~t & x) | (s & x));
    uint64_t f = (s & (nv | nx)) | (p & ns & t & v & w & x);
    uint64_t g = (t & (nv | nx)) | (q & ns & t & w);
    uint64_t h = u;

    uint64_t i = v & ((nw & nx) | (w & x & (s | t)));
    uint64_t j = (nv & w) | (s & v & nw & x) | (p & w & (nx | (ns & ~t)));
    uint64_t k = (nv & x) | (t & nw & x) | (q & v & w & (nx | (ns & nt)));
    uint64_t m = y;

    uint64_t d2 = (a << 3) | (b << 2) | (c << 1) | d;
    uint64_t d1 = (e << 3) | (f << 2) | (g << 1) | h;
    uint64_t d0 = (i << 3) | (j << 2) | (k << 1) | m;

    return d2 * 100 + d1 * 10 + d0;
}

/* Encode 0..999 into a 10-bit declet. The inverse of dpdDecodeDeclet over the
 * canonical declets. */
static uint64_t dpdEncodeDeclet(uint64_t n) {
    verify(n <= 999);

    if (n == 0)
        return 0;

    uint64_t q10 = (n * 205) >> 11;  /* n / 10, exact for n <= 999 */
    uint64_t q100 = (n * 41) >> 12;  /* n / 100, exact for n <= 999 */

    uint64_t d2 = q100;
    uint64_t d1 = q10 - (q100 * 10);
    uint64_t d0 = n - (q10 * 10);

    uint64_t a = (d2 >> 3) & 1;
    uint64_t b = (d2 >> 2) & 1;
    uint64_t c = (d2 >> 1) & 1;
    uint64_t d = d2 & 1;

    uint64_t e = (d1 >> 3) & 1;
    uint64_t f = (d1 >> 2) & 1;
    uint64_t g = (d1 >> 1) & 1;
    uint64_t h = d1 & 1;

    uint64_t i = (d0 >> 3) & 1;
    uint64_t j = (d0 >> 2) & 1;
    uint64_t k = (d0 >> 1) & 1;
    uint64_t m = d0 & 1;

    uint64_t na = a ^ 1;
    uint64_t ne = e ^ 1;
    uint64_t ni = i ^ 1;

    uint64_t p = b | ((a & j) | ((a & f) & i));
    uint64_t q = c | ((a & k) | ((a & g) & i));
    uint64_t r = d;
    uint64_t s = (f & (na | ni)) | ((na & (e & j)) | (e & i));
    uint64_t t = g | ((na & (e & k)) | (a & i));
    uint64_t u = h;
    uint64_t v = a | (e | i);
    uint64_t w = a | ((e & i) | (ne & j));
    uint64_t x = e | ((a & i) | (na & k));
    uint64_t y = m;

    return (p << 9) | (q << 8) | (r << 7) | (s << 6) | (t << 5) |
           (u << 4) | (v << 3) | (w << 2) | (x << 1) | y;
}

/*
 * Decode up to seven packed declets into a binary integer. Declets are ten
 * bits each, least significant first; the topmost may be narrower, which is
 * how the coefficient's leading digit rides along -- 0..9 encodes to itself.
 */
static uint64_t binFromDeclets(uint64_t declets) {
    uint64_t bin = 0;
    uint64_t rest = declets;
    uint64_t multiplier = 1;
    while (rest != 0) {
        bin += dpdDecodeDeclet(rest & 0x3FF) * multiplier;
        multiplier *= 1000;
        rest >>= 10;
    }
    return bin;
}

/* Encode a value of at most 18 digits into six packed declets, least
 * significant first. The inverse of binFromDeclets over that range. */
static uint64_t decletsFromBin(uint64_t bin) {
    verify(bin < TEN_POW_18);
    uint64_t rest = bin;
    uint64_t declets = 0;
    int shift = 0;
    while (rest != 0) {
        uint64_t q = mulhi64(rest, 0x020C49BA5E353F7DULL) >> 3; /* rest / 1000 */
        declets |= dpdEncodeDeclet(rest - (q * 1000)) << shift;
        shift += 10;
        rest = q;
    }
    return declets;
}

/* --------------------------------------------------------------------------
 * Rendering integers into ASCII
 * ------------------------------------------------------------------------ */

/*
 * Reciprocal-multiplication constants. The 64-bit ones fold in the 64-bit
 * shift that mulhi64 already performs, so the named shift is the remainder.
 */
#define M_U64_DIV_1E8 (0x8000000000000000ULL | 0x2BCC77118461CEFDULL)
#define S_U64_DIV_1E8 26
#define M_U32_DIV_1E1 0xCCCCCCCDULL
#define S_U32_DIV_1E1 35
#define M_U32_DIV_1E2 0x51EB851FULL
#define S_U32_DIV_1E2 37
#define M_U64_DIV_1E4 0x346DC5D63886594BULL
#define S_U64_DIV_1E4 11
#define MASK32 0xFFFFFFFFULL

static const uint64_t MAGIC_DIV_1E8 = (M_U64_DIV_1E8);

/*
 * Render exactly 8 decimal digits of dw, ending at offMaxx. Zero-pads. The
 * caller guarantees dw < 10^8 and that the 8 bytes are inside the buffer.
 *
 * A binary cascade: one divide by 10^4 to split into halves, two by 10^2 to
 * split those into pairs, four by 10 for the digits.
 */
static void render8DigitsBeforeIndex(uint64_t dw, char *ascii, int offMaxx) {
    uint64_t abcd = mulhi64(dw, M_U64_DIV_1E4) >> S_U64_DIV_1E4;
    uint64_t efgh = dw - (abcd * 10000);

    uint64_t ab = (abcd * M_U32_DIV_1E2) >> S_U32_DIV_1E2;
    uint64_t cd = abcd - (ab * 100);

    uint64_t ef = (efgh * M_U32_DIV_1E2) >> S_U32_DIV_1E2;
    uint64_t gh = efgh - (ef * 100);

    uint64_t a = (ab * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
    uint64_t b = ab - (a * 10);

    uint64_t c = (cd * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
    uint64_t d = cd - (c * 10);

    uint64_t e = (ef * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
    uint64_t f = ef - (e * 10);

    uint64_t g = (gh * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
    uint64_t h = gh - (g * 10);

    demand(offMaxx >= 8);
    ascii[offMaxx - 8] = (char)('0' + a);
    ascii[offMaxx - 7] = (char)('0' + b);
    ascii[offMaxx - 6] = (char)('0' + c);
    ascii[offMaxx - 5] = (char)('0' + d);
    ascii[offMaxx - 4] = (char)('0' + e);
    ascii[offMaxx - 3] = (char)('0' + f);
    ascii[offMaxx - 2] = (char)('0' + g);
    ascii[offMaxx - 1] = (char)('0' + h);
}

/*
 * Render exactly renderDigitCount digits of dw, ending at offMaxx, zero-padded.
 * Four digits per iteration while at least four remain, then one at a time.
 */
static void renderTailDigitsBeforeIndex(int renderDigitCount, uint64_t dw, char *ascii, int offMaxx) {
    uint64_t t = dw;
    int remainingDigitCount = renderDigitCount;
    int ib = offMaxx;
    demand(offMaxx - renderDigitCount >= 0);
    while (remainingDigitCount >= 4) {
        uint64_t t0 = mulhi64(t, M_U64_DIV_1E4) >> S_U64_DIV_1E4;
        uint64_t abcd = t - (t0 * 10000);
        t = t0;
        uint64_t ab = (abcd * M_U32_DIV_1E2) >> S_U32_DIV_1E2;
        uint64_t cd = abcd - (ab * 100);
        uint64_t a = (ab * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
        uint64_t b = ab - (a * 10);
        uint64_t c = (cd * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
        uint64_t d = cd - (c * 10);
        ascii[ib - 4] = (char)('0' + a);
        ascii[ib - 3] = (char)('0' + b);
        ascii[ib - 2] = (char)('0' + c);
        ascii[ib - 1] = (char)('0' + d);
        ib -= 4;
        remainingDigitCount -= 4;
    }
    while (remainingDigitCount > 0) {
        uint64_t divTen = (t * M_U32_DIV_1E1) >> S_U32_DIV_1E1;
        int digit = (int)(t - (divTen * 10));
        ascii[--ib] = (char)('0' + digit);
        t = divTen;
        --remainingDigitCount;
    }
    verify(offMaxx - ib == renderDigitCount);
}

/*
 * Render a 128-bit value as digitPrintCount decimal digits, ending at
 * off + digitPrintCount.
 *
 * Three phases: while the high word is live, peel 8 digits per iteration via
 * three divides by 10^8 across rotating 96-bit chunks; once it fits in 64
 * bits, one divide by 10^8 per 8 digits; then the 1..7 leftover digits.
 */
static int u128ToAscii(int digitPrintCount, uint64_t dw1, uint64_t dw0, char *ascii, int off) {
    uint64_t dw1T = dw1;
    uint64_t dw0T = dw0;
    int ich = off + digitPrintCount;
    int remainingDigitCount = digitPrintCount;
    while (dw1T != 0) {
        uint64_t q2 = mulhi64(dw1T, MAGIC_DIV_1E8) >> S_U64_DIV_1E8;
        uint64_t r2 = dw1T - (q2 * 100000000ULL);
        uint64_t s2 = (r2 << 32) | (dw0T >> 32);
        uint64_t q1 = mulhi64(s2, MAGIC_DIV_1E8) >> S_U64_DIV_1E8;
        uint64_t r1 = s2 - (q1 * 100000000ULL);
        uint64_t s1 = (r1 << 32) | (dw0T & MASK32);
        uint64_t q0 = mulhi64(s1, MAGIC_DIV_1E8) >> S_U64_DIV_1E8;
        uint64_t r0 = s1 - (q0 * 100000000ULL);
        dw0T = (q1 << 32) + q0;
        dw1T = q2;
        render8DigitsBeforeIndex(r0, ascii, ich);
        ich -= 8;
        remainingDigitCount -= 8;
    }
    while (remainingDigitCount >= 8) {
        uint64_t t0 = mulhi64(dw0T, MAGIC_DIV_1E8) >> S_U64_DIV_1E8;
        uint64_t r0 = dw0T - (t0 * 100000000ULL);
        dw0T = t0;
        render8DigitsBeforeIndex(r0, ascii, ich);
        ich -= 8;
        remainingDigitCount -= 8;
    }
    if (remainingDigitCount > 0)
        renderTailDigitsBeforeIndex(remainingDigitCount, dw0T, ascii, ich);
    return digitPrintCount;
}

/* Write a signed decimal integer; returns the number of characters written.
 * Replaces the reference's Int.toString in the zero renderer. */
static int appendInt(char *out, int v) {
    char tmp[12];
    int n = 0;
    int len = 0;
    unsigned int magnitude;
    if (v < 0) {
        out[len++] = '-';
        magnitude = (unsigned int)(-(long)v);
    } else {
        magnitude = (unsigned int)v;
    }
    do {
        tmp[n++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);
    while (n > 0)
        out[len++] = tmp[--n];
    return len;
}

/* --------------------------------------------------------------------------
 * Rendering a decoded value
 *
 * These four know nothing about BID or DPD: both decoders reduce an encoding
 * to (sign, qExp, coefficient) and hand it here.
 * ------------------------------------------------------------------------ */

static int infString(char *out, bool sign) {
    const char *s = sign ? "-Infinity" : "Infinity";
    int len = (int)strlen(s);
    demand(len + 1 <= D128TEXT_MAX);
    memcpy(out, s, (size_t)len + 1);
    return len;
}

static int nanString(char *out, bool sign, bool isSignaling, uint64_t payloadHi, uint64_t payloadLo) {
    const char *base;
    if (!isSignaling)
        base = sign ? "-NaN" : "NaN";
    else
        base = sign ? "-sNaN" : "sNaN";
    int baseLen = (int)strlen(base);

    if ((payloadHi | payloadLo) == 0 ||
        /* non-canonical payload, IEEE 754-2019 3.5.2 c) 2) -- read as zero */
        payloadHi > MAX_PAYLOAD_HI ||
        (payloadHi == MAX_PAYLOAD_HI && payloadLo > MAX_PAYLOAD_LO)) {
        demand(baseLen + 1 <= D128TEXT_MAX);
        memcpy(out, base, (size_t)baseLen + 1);
        return baseLen;
    }
    int digitLen = calcDigitLen128(payloadHi, payloadLo);
    int totalLen = baseLen + digitLen;
    demand(totalLen + 1 <= D128TEXT_MAX);
    memcpy(out, base, (size_t)baseLen);
    u128ToAscii(digitLen, payloadHi, payloadLo, out, baseLen);
    out[totalLen] = '\0';
    return totalLen;
}

static const char *const ZEROS[16] = {
    "0", "0.0", "0.00", "0.000", "0.0000", "0.00000", "0.000000", "",
    "-0", "-0.0", "-0.00", "-0.000", "-0.0000", "-0.00000", "-0.000000", "",
};

static int zerString(char *out, bool sign, int qExp) {
    if (qExp <= 0 && qExp >= -6) {
        int signBit = sign ? 1 : 0;
        int index = (-qExp + (signBit << 3)) & 0x0F;
        const char *s = ZEROS[index];
        int len = (int)strlen(s);
        demand(len + 1 <= D128TEXT_MAX);
        memcpy(out, s, (size_t)len + 1);
        return len;
    }
    const char *base;
    if (qExp > 0)
        base = sign ? "-0E+" : "0E+";
    else
        base = sign ? "-0E" : "0E"; /* qExp is negative and prints its own sign */
    int baseLen = (int)strlen(base);
    demand(baseLen + 6 <= D128TEXT_MAX);
    memcpy(out, base, (size_t)baseLen);
    int len = baseLen + appendInt(out + baseLen, qExp);
    out[len] = '\0';
    return len;
}

/* qExp == 0: a plain integer, no radix point and no exponent. */
static int fnzToIntegerString(char *out, bool sign, int digitLen, uint64_t dw1, uint64_t dw0) {
    verify(digitLen > 0);
    int signLen = sign ? 1 : 0;
    int totalLen = signLen + digitLen;
    demand(totalLen + 1 <= D128TEXT_MAX);
    out[0] = '-'; /* overwritten by the first digit when unsigned */
    u128ToAscii(digitLen, dw1, dw0, out, signLen);
    out[totalLen] = '\0';
    return totalLen;
}

/*
 * Fixed-point form: leading zeros, then the coefficient digits shifted right
 * by one position to make room for the radix point.
 */
static int fnzToDecimalPointString(char *out, bool sign, int qExp, int digitLen, uint64_t dw1, uint64_t dw0) {
    int digitsRightOfDecimal = -qExp;
    int leadingZeroCount = 1 + digitsRightOfDecimal - digitLen;
    if (leadingZeroCount < 0)
        leadingZeroCount = 0;
    int signLen = sign ? 1 : 0;
    int decimalPointLen = 1;
    int totalLen = signLen + leadingZeroCount + decimalPointLen + digitLen;
    demand(totalLen + 1 <= D128TEXT_MAX);
    out[0] = '-';
    for (int i = signLen; i <= leadingZeroCount; ++i) /* one extra, overwritten below */
        out[i] = '0';
    u128ToAscii(digitLen, dw1, dw0, out, signLen + leadingZeroCount);
    for (int i = totalLen - 1; i >= totalLen - digitsRightOfDecimal; --i)
        out[i] = out[i - 1];
    out[totalLen - digitsRightOfDecimal - 1] = '.';
    out[totalLen] = '\0';
    return totalLen;
}

/*
 * Normalized scientific form. The exponent sign is always written, per the
 * GDAS to-scientific-string form: 1.234E+5, never 1.234E5.
 */
static int fnzToNormalizedScientificString(char *out, bool sign, int qExp, int digitLen, uint64_t dw1, uint64_t dw0) {
    verify(digitLen > 0);
    int eExp = qExp + (digitLen - 1);
    int eExpAbs = (eExp < 0) ? -eExp : eExp;
    int signLen = sign ? 1 : 0;
    int decimalPointLen = (digitLen > 1) ? 1 : 0;
    int expELen = 1;
    int expSignLen = 1;
    char expSignByte = (eExp < 0) ? '-' : '+';
    int expDigitLen =
        (eExpAbs >= 1000) ? 4 : (eExpAbs >= 100) ? 3 : (eExpAbs >= 10) ? 2 : 1;
    int totalLen = signLen + decimalPointLen + digitLen + expELen + expSignLen + expDigitLen;
    demand(totalLen + 1 <= D128TEXT_MAX);
    out[0] = '-';
    u128ToAscii(digitLen, dw1, dw0, out, signLen + decimalPointLen);
    if (decimalPointLen > 0) {
        out[signLen] = out[signLen + 1];
        out[signLen + 1] = '.';
    }
    int iE = signLen + decimalPointLen + digitLen;
    out[iE] = 'E';
    out[iE + 1] = expSignByte;
    renderTailDigitsBeforeIndex(expDigitLen, (uint64_t)eExpAbs, out, totalLen);
    out[totalLen] = '\0';
    return totalLen;
}

/*
 * Finite non-zero: pick the form. Integer when the quantum exponent is zero,
 * fixed-point when the value sits within six leading zeros of unity,
 * scientific otherwise -- the GDAS auto-select rule.
 */
static int fnzString(char *out, bool sign, int qExp, uint64_t dw1, uint64_t dw0) {
    int digitLen = calcDigitLen128(dw1, dw0);
    int eExp = qExp + (digitLen - 1);
    if (qExp == 0)
        return fnzToIntegerString(out, sign, digitLen, dw1, dw0);
    if (qExp <= 0 && eExp >= -6)
        return fnzToDecimalPointString(out, sign, qExp, digitLen, dw1, dw0);
    return fnzToNormalizedScientificString(out, sign, qExp, digitLen, dw1, dw0);
}

/* --------------------------------------------------------------------------
 * bits -> text
 * ------------------------------------------------------------------------ */

/* Unpack a BID128 encoding, per IEEE 754-2019 3.5.2, and render it. */
static int decodeBid128toString(char *out, uint64_t bid128Hi, uint64_t bid128Lo) {
    verify(1 + W5 + T_WIDTH == 128);

    bool sign = (bid128Hi & SIGN_BIT) != 0;
    int combination = (int)((bid128Hi >> (T_WIDTH - 64)) & 0x1FFFF);
    uint64_t coeffTHi = bid128Hi & ((1ULL << (T_WIDTH - 64)) - 1);

    uint64_t coeffHi;
    int qExp;

    if ((combination >> (W5 - 2)) != 0x3) {
        /* G0 G1 is 00, 01 or 10: the exponent is the top of the field and the
         * coefficient's three high bits are the bottom of it. */
        int biasedExponentE = combination >> 3;
        qExp = biasedExponentE - BIAS;
        coeffHi = (uint64_t)(combination & 0x07);
    } else if ((combination >> (W5 - 4)) != 0xF) {
        /* G0 G1 is 11 and G2 G3 is not: the coefficient's high bits are
         * 8 + Gw+4, and the exponent shifts down by two. */
        int biasedExponentE = (combination >> 1) & ((1 << (W5 - 5 + 2)) - 1);
        qExp = biasedExponentE - BIAS;
        coeffHi = 8ULL + (uint64_t)(combination & 1);
    } else if ((combination >> (W5 - 5)) == 0x1E) {
        return infString(out, sign);
    } else {
        verify((combination >> (W5 - 5)) == 0x1F);
        bool isSignaling = ((combination >> (W5 - 6)) & 1) != 0;
        return nanString(out, sign, isSignaling, coeffTHi, bid128Lo);
    }

    uint64_t dw1 = (coeffHi << (T_WIDTH - 64)) | coeffTHi;
    uint64_t dw0 = bid128Lo;

    /* IEEE 754-2019 3.5.2: a coefficient at or above 10^34 is non-canonical
     * and the value used for it is zero, sign and exponent retained. */
    if ((dw1 | dw0) == 0 ||
        dw1 > MAXX_COEFF_34_HI ||
        (dw1 == MAXX_COEFF_34_HI && dw0 >= MAXX_COEFF_34_LO))
        return zerString(out, sign, qExp);
    return fnzString(out, sign, qExp, dw1, dw0);
}

/* Unpack a DPD128 encoding and hand the pieces to the same renderers. */
static int decodeDpd128toString(char *out, uint64_t dpd128Hi, uint64_t dpd128Lo) {
    const int w = 12; /* exponent continuation width */

    bool sign = (dpd128Hi & SIGN_BIT) != 0;
    int combination = (int)((dpd128Hi >> (T_WIDTH - 64)) & 0x1FFFF);
    /* in DPD the low w bits of the biased exponent always sit at the bottom of G */
    int expContinuation = combination & 0x0FFF;

    uint64_t d0;
    int qExp;
    bool isNaN = false;
    bool isSignaling = false;

    if ((combination >> (W5 - 2)) != 0x3) {
        /* leading digit 0..7, exponent MSBs in G0 G1 */
        int expMSB = combination >> (W5 - 2);
        qExp = ((expMSB << w) | expContinuation) - BIAS;
        d0 = (uint64_t)((combination >> w) & 0x7);
    } else if ((combination >> (W5 - 4)) != 0xF) {
        /* leading digit 8 or 9, exponent MSBs displaced into G2 G3 */
        int expMSB = (combination >> (W5 - 4)) & 0x3;
        qExp = ((expMSB << w) | expContinuation) - BIAS;
        d0 = 8ULL + (uint64_t)((combination >> w) & 0x1);
    } else if ((combination >> (W5 - 5)) == 0x1E) {
        return infString(out, sign);
    } else {
        verify((combination >> (W5 - 5)) == 0x1F);
        isNaN = true;
        isSignaling = ((combination >> (W5 - 6)) & 1) != 0;
        d0 = 0;
        qExp = 0;
    }

    /* Rebuild the binary coefficient from the declets. The leading digit rides
     * along as a sixth declet of the high group: 0..9 encodes to itself. */
    uint64_t decletsHi = (d0 << 50) | ((dpd128Hi & 0x3FFFFFFFFFFFULL) << 4) | (dpd128Lo >> 60);
    uint64_t decletsLo = dpd128Lo & 0x0FFFFFFFFFFFFFFFULL; /* low 60 bits, six declets */
    uint64_t binHi = binFromDeclets(decletsHi);
    uint64_t binLo = binFromDeclets(decletsLo);

    uint64_t dw1;
    uint64_t dw0;
    if (binHi == 0) {
        dw1 = 0;
        dw0 = binLo;
    } else {
        dw0 = (binHi * TEN_POW_18) + binLo;
        uint64_t carry = (dw0 < binLo) ? 1ULL : 0ULL;
        dw1 = mulhi64(binHi, TEN_POW_18) + carry;
    }

    if (isNaN)
        return nanString(out, sign, isSignaling, dw1, dw0);
    if ((dw1 | dw0) == 0)
        return zerString(out, sign, qExp);
    return fnzString(out, sign, qExp, dw1, dw0);
}

/* --------------------------------------------------------------------------
 * Packing a finalized value into an encoding
 *
 * Everything upstream of these -- parsing, rounding, clamping, the underflow
 * region -- is format-neutral; the two formats diverge only here.
 * ------------------------------------------------------------------------ */

static void bid128SetFinite(uint64_t *hi, uint64_t *lo, bool sign, int qExp, uint64_t dw1, uint64_t dw0) {
    verify(qExp >= Q_MIN && qExp <= Q_MAX);
    verify(calcDigitLen128(dw1, dw0) <= 34);
    uint64_t qBiased = (uint64_t)(qExp + BIAS);
    uint64_t s = sign ? SIGN_BIT : 0;
    uint64_t g = qBiased << (3 + 46);
    *hi = s | g | dw1;
    *lo = dw0;
}

/*
 * Pack a finite value into DPD128: the same sign and the same 14-bit biased
 * exponent as BID, and a coefficient of the same 34 digits laid out as a
 * leading digit in the combination field plus eleven declets.
 *
 * The leading digit cannot be packed independently of the exponent -- DPD puts
 * it in the same five-bit prefix as the exponent's top two bits -- so unlike
 * BID there is no shared sign-and-exponent step to factor out.
 */
static void dpd128SetFinite(uint64_t *hi, uint64_t *lo, bool sign, int qExp, uint64_t dw1, uint64_t dw0) {
    verify(qExp >= Q_MIN && qExp <= Q_MAX);
    verify(calcDigitLen128(dw1, dw0) <= 34);

    /* Split the coefficient into its low 18 digits and its high 16. Two
     * divisions by 10^9 rather than one by 10^18, so the existing 128-bit
     * kernel does the work and no wider machinery is needed. */
    uint64_t tHi, tLo;
    uint64_t r1 = u113DivModPow10Max9(&tHi, &tLo, dw1, dw0, 9);
    uint64_t r2 = u113DivModPow10Max9(&tHi, &tLo, tHi, tLo, 9);
    uint64_t binLo = (r2 * 1000000000ULL) + r1;
    verify(tHi == 0); /* the high 16 digits fit in one word */
    uint64_t binHi = tLo;

    /* the top digit rides in the combination field; the other 15 are declets */
    uint64_t d0 = binHi / TEN_POW_15;
    uint64_t declets5Hi = decletsFromBin(binHi - (d0 * TEN_POW_15));
    uint64_t declets6Lo = decletsFromBin(binLo);
    verify(d0 <= 9);

    int biasedQExp = qExp + BIAS;
    int expMSB = (biasedQExp >> 12) & 0x3;
    int expLSB = biasedQExp & 0x0FFF;
    int gPrefix =
        (d0 <= 7) ? ((expMSB << 3) | (int)d0)                    /* G0G1 = exponent MSBs */
                  : (0x18 | (expMSB << 1) | ((int)d0 & 1));      /* G0G1 = 11, digit is 8 or 9 */
    uint64_t signCombo = ((uint64_t)(sign ? 1 : 0) << 17) | ((uint64_t)gPrefix << 12) | (uint64_t)expLSB;

    *hi = (signCombo << 46) | (declets5Hi >> 4);
    *lo = (declets5Hi << 60) | declets6Lo;
}

static void setFinite(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign, int qExp, uint64_t dw1, uint64_t dw0) {
    if (formatIsBid)
        bid128SetFinite(hi, lo, sign, qExp, dw1, dw0);
    else
        dpd128SetFinite(hi, lo, sign, qExp, dw1, dw0);
}

static void bid128SetNaN(uint64_t *hi, uint64_t *lo, bool sign, bool isSignaling, uint64_t payloadDw1, uint64_t payloadDw0) {
    uint64_t withoutPayload =
        (isSignaling ? SNAN_PATTERN_HI : QNAN_PATTERN_HI) | (sign ? SIGN_BIT : 0);
    *hi = withoutPayload | payloadDw1;
    *lo = payloadDw0;
}

/*
 * Pack a NaN into DPD128. The payload occupies the whole 110-bit trailing
 * field as eleven declets -- 33 digits, with no leading digit in the
 * combination field, since G0..G4 are fixed at 11111 and G5 carries the
 * signaling flag.
 */
static void dpd128SetNaN(uint64_t *hi, uint64_t *lo, bool sign, bool isSignaling, uint64_t payloadDw1, uint64_t payloadDw0) {
    verify(calcDigitLen128(payloadDw1, payloadDw0) <= 33);

    uint64_t tHi, tLo;
    uint64_t r1 = u113DivModPow10Max9(&tHi, &tLo, payloadDw1, payloadDw0, 9);
    uint64_t r2 = u113DivModPow10Max9(&tHi, &tLo, tHi, tLo, 9);
    uint64_t binLo = (r2 * 1000000000ULL) + r1;
    verify(tHi == 0); /* a 33-digit payload leaves 15 digits up top */
    uint64_t declets5Hi = decletsFromBin(tLo);
    uint64_t declets6Lo = decletsFromBin(binLo);

    uint64_t gField = (0x1FULL << 12) | (isSignaling ? (1ULL << 11) : 0ULL);
    uint64_t signCombo = ((uint64_t)(sign ? 1 : 0) << 17) | gField;
    *hi = (signCombo << 46) | (declets5Hi >> 4);
    *lo = (declets5Hi << 60) | declets6Lo;
}

static void setNaN(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign, bool isSignaling, uint64_t payloadDw1, uint64_t payloadDw0) {
    if (formatIsBid)
        bid128SetNaN(hi, lo, sign, isSignaling, payloadDw1, payloadDw0);
    else
        dpd128SetNaN(hi, lo, sign, isSignaling, payloadDw1, payloadDw0);
}

static void finalizeOverflow(uint64_t *hi, uint64_t *lo, bool sign) {
    *hi = (sign ? SIGN_BIT : 0) | INF_PATTERN_HI;
    *lo = 0;
}

/* --------------------------------------------------------------------------
 * Finalize: rounding, clamping, underflow
 * ------------------------------------------------------------------------ */

static void roundAndFinalizeFinite(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                   int qExpIn, uint64_t dw1In, uint64_t dw0In, int residueIn);

/* Does this coefficient/exponent pair fit as a canonical encoding? */
static inline bool coeffQExpFit(uint64_t dw1, uint64_t dw0, int qExp) {
    if (qExp < Q_MIN || qExp > Q_MAX)
        return false;
    /* at most 113 bits is necessarily below 10^34, since 2^113 > 10^34 */
    if (clz64(dw1) > 128 - 113)
        return true;
    return dw1 < MAXX_COEFF_34_HI || (dw1 == MAXX_COEFF_34_HI && dw0 < MAXX_COEFF_34_LO);
}

/*
 * A finite value whose quantum exponent exceeds Q_MAX but whose coefficient
 * has leading capacity: absorb the excess into the coefficient, bringing the
 * exponent down to exactly Q_MAX without changing the value.
 */
static void finalizeClamping(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign, int qExp, uint64_t dw1, uint64_t dw0) {
    int qExcess = qExp - Q_MAX;
    verify(qExcess > 0 && qExcess <= 34 - calcDigitLen128(dw1, dw0));
    int pow10Offset = qExcess << 1;
    uint64_t y1 = POW10[pow10Offset + 1];
    uint64_t y0 = POW10[pow10Offset];
    uint64_t pp00Hi = mulhi64(dw0, y0);
    uint64_t pp00Lo = dw0 * y0;
    uint64_t pp10Lo = dw1 * y0;
    uint64_t pp01Lo = dw0 * y1;
    uint64_t dw0Scaled = pp00Lo;
    uint64_t dw1Scaled = pp00Hi + pp10Lo + pp01Lo;
    verify(coeffQExpFit(dw1Scaled, dw0Scaled, Q_MAX));
    setFinite(hi, lo, formatIsBid, sign, Q_MAX, dw1Scaled, dw0Scaled);
}

/*
 * The underflow boundary: the coefficient's most significant digit sits
 * exactly one position right of the representable range, so the coefficient
 * itself decides the rounding. Under ties-to-even the outcome is zero unless
 * the total residue is greater than half, zero being even.
 */
static void finalizeUnderflowBoundary(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                      uint64_t dw1, uint64_t dw0, int residueIn) {
    int digitLen = calcDigitLen128(dw1, dw0);
    verify(digitLen != 0);
    int scaleResidue = residueFromValuePow10(dw1, dw0, digitLen);
    int totalResidue = residueMerge(scaleResidue, residueIn);
    setFinite(hi, lo, formatIsBid, sign, Q_MIN, 0, (totalResidue == GT_HALF) ? 1 : 0);
}

/* Representable as a subnormal: truncate to the minimum exponent, keeping
 * what precision survives, and round what is left. */
static void finalizeSubnormal(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign, int qExpIn,
                              uint64_t dw1In, uint64_t dw0In, int residueIn) {
    int truncationNeeded = Q_MIN - qExpIn;
    verify(truncationNeeded > 0 && truncationNeeded < calcDigitLen128(dw1In, dw0In));

    uint64_t dw1T, dw0T;
    int scaleResidue = u128ScaleDownPow10(&dw1T, &dw0T, dw1In, dw0In, truncationNeeded);
    int totalResidue = residueMerge(scaleResidue, residueIn);
    int qExpT = Q_MIN;

    bool roundUp = (totalResidue == GT_HALF) || (totalResidue == HALF && (dw0T & 1) != 0);
    if (roundUp) {
        ++dw0T;
        dw1T += (dw0T == 0) ? 1 : 0;
        if (dw1T == MAXX_COEFF_34_HI && dw0T == MAXX_COEFF_34_LO) {
            dw1T = MIN_PRECISION_34_COEFF_HI;
            dw0T = MIN_PRECISION_34_COEFF_LO;
            ++qExpT;
        }
    }
    setFinite(hi, lo, formatIsBid, sign, qExpT, dw1T, dw0T);
}

/* Dispatch the three underflow cases: swamped to zero, the exact boundary,
 * and the subnormal region. */
static void finalizeUnderflowRegion(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                    int qExp, uint64_t dw1, uint64_t dw0, int residue) {
    verify(qExp < Q_MIN);
    int truncationNeeded = Q_MIN - qExp;
    int digitLen = calcDigitLen128(dw1, dw0);
    verify(digitLen <= 34);
    if (truncationNeeded > digitLen) {
        /* below half the smallest subnormal: zero, and always inexact */
        roundAndFinalizeFinite(hi, lo, formatIsBid, sign, Q_MIN, 0, 0, LT_HALF);
    } else if (truncationNeeded == digitLen) {
        finalizeUnderflowBoundary(hi, lo, formatIsBid, sign, dw1, dw0, residue);
    } else {
        finalizeSubnormal(hi, lo, formatIsBid, sign, qExp, dw1, dw0, residue);
    }
}

/*
 * Round a parsed coefficient/exponent pair to a canonical encoding, under
 * ties-to-even, and pack it.
 *
 *  1. fast path -- already in range and no rounding to do
 *  2. a zero coefficient with an out-of-range exponent clamps
 *  3. (special values never arrive here)
 *  4. underflow, when the exponent is below the minimum
 *  5. truncate to 34 digits -- REMOVED as unreachable, see the note in the body
 *  6. round, and renormalize if the coefficient rolls over to 10^34
 *  7. overflow to Infinity, or clamp near Emax
 */
static void roundAndFinalizeFinite(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                   int qExpIn, uint64_t dw1In, uint64_t dw0In, int residueIn) {
    /* Step 1 */
    if (residueIn <= LT_HALF || (residueIn == HALF && (dw0In & 1) == 0)) {
        if (coeffQExpFit(dw1In, dw0In, qExpIn)) {
            setFinite(hi, lo, formatIsBid, sign, qExpIn, dw1In, dw0In);
            return;
        }
        /* Step 2 */
        if ((dw1In | dw0In) == 0) {
            int qClamped = qExpIn;
            if (qClamped > Q_MAX)
                qClamped = Q_MAX;
            if (qClamped < Q_MIN)
                qClamped = Q_MIN;
            setFinite(hi, lo, formatIsBid, sign, qClamped, 0, 0);
            return;
        }
    }

    /*
     * Step 5 -- precision truncation -- IS NOT HERE, and this is the reason.
     *
     * The finalizer this descends from serves an arithmetic engine, whose
     * callers hand it coefficients wider than the 34-digit precision limit:
     * multiply and FMA produce a double-width product, and the engine
     * truncates it here, accumulating the discarded digits into the residue
     * before rounding. A codec has no such caller.
     *
     * The parser accumulates at most 19 + 15 = 34 significant digits and turns
     * every digit past them into a residue instead, so the coefficient it
     * hands over is never wider than the limit. The only other entry to this
     * function is the swamped-underflow branch below, which passes a zero
     * coefficient. So the truncation step is unreachable, and rather than ship
     * a path that has never once executed -- and therefore has never been
     * verified to work -- it is removed and the invariant is enforced.
     *
     * Measured: zero occurrences across the 86,126-row corpus and 400,000
     * random numerals, and across 76,280 finalizer calls driven by inputs of
     * up to 5,000 digits, where the widest coefficient ever seen was 34.
     *
     * This is `demand`, not `verify`: it must survive into shipping builds,
     * because falling through would encode an out-of-range coefficient
     * silently. It cannot fire on a consumer's data -- no input reaches it --
     * only on a future caller that breaks the invariant, which is exactly when
     * a loud failure is what you want. A port that parses differently, or a
     * caller that is not a parser, needs the truncation step back; it is in
     * the reference implementation and in every full port's Finalize.
     */
    demand(calcDigitLen128(dw1In, dw0In) <= 34);

    /* Step 4 -- with no precision truncation to outrank it, the underflow test
     * is simply whether the exponent is below the minimum. */
    if (Q_MIN - qExpIn > 0) {
        finalizeUnderflowRegion(hi, lo, formatIsBid, sign, qExpIn, dw1In, dw0In, residueIn);
        return;
    }

    int totalResidue = residueIn;
    uint64_t dw1 = dw1In;
    uint64_t dw0 = dw0In;
    int qExp = qExpIn;

    /* Step 6 -- ties-to-even, the only rounding this codec has */
    bool applyRounding = (totalResidue == GT_HALF) || (totalResidue == HALF && (dw0 & 1) != 0);
    if (applyRounding) {
        ++dw0;
        dw1 += (dw0 == 0) ? 1 : 0;
        if (dw1 == MAXX_COEFF_34_HI && dw0 == MAXX_COEFF_34_LO) {
            dw1 = MIN_PRECISION_34_COEFF_HI;
            dw0 = MIN_PRECISION_34_COEFF_LO;
            ++qExp;
        }
    }

    /* Step 7 */
    verify(qExp >= Q_MIN);
    verify(calcDigitLen128(dw1, dw0) <= 34);
    if (qExp > Q_MAX) {
        int qExcess = qExp - Q_MAX;
        int digitLen = calcDigitLen128(dw1, dw0);
        if (digitLen + qExcess <= 34)
            finalizeClamping(hi, lo, formatIsBid, sign, qExp, dw1, dw0);
        else
            finalizeOverflow(hi, lo, sign);
        return;
    }
    setFinite(hi, lo, formatIsBid, sign, qExp, dw1, dw0);
}

/* --------------------------------------------------------------------------
 * text -> bits
 * ------------------------------------------------------------------------ */

/*
 * Read the character at *pos and advance, except at the terminator, where it
 * stays put and keeps returning NUL. This is how the reference's getOrNul
 * sentinel -- which reads NUL forever past the end of a Kotlin String --
 * translates without ever stepping past a C string's terminator.
 */
static inline int chAt(const char *str, size_t *pos) {
    int c = (unsigned char)str[*pos];
    if (c != 0)
        ++*pos;
    return c;
}

/*
 * Parse a finite numeral: optional radix point, optional E exponent, any
 * number of digits.
 *
 * Coefficients with more than 34 significant digits are accepted; leading
 * zeros are flushed, and the digits past 34 are tracked as residue for the
 * ties-to-even rounding that follows. Exponent magnitudes clamp to 9999,
 * which is enough for finalize to detect overflow or underflow.
 */
static const char *parseFiniteValueText(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                        int chFirst, const char *str, size_t startPos) {
    size_t pos = startPos;
    int residue = EXACT;

    bool hasCoefficientDigit = false; /* any digit at all, including zero */
    int significantDigitCount = 0;    /* does not count leading zeros */
    bool hasDot = false;
    bool expSign = false;
    /* an underscore is a digit separator: legal only between two digits, so it
     * must follow a digit and the next character must be a digit too */
    bool prevWasDigit = false;
    bool afterUnderscore = false;

    verify(chFirst != 0);
    int ch = chFirst;
    int fractionalDigitCount = 0;
    uint64_t accum19a = 0;
    uint64_t accum19b = 0;
    int exp = 0;

    while ((ch >= '0' && ch <= '9') || ch == '.' || ch == '_') {
        if (ch >= '0' && ch <= '9') {
            int d = ch - '0';
            hasCoefficientDigit = true;
            /* count while flushing leading zeros */
            significantDigitCount += ((significantDigitCount | d) != 0);
            if (significantDigitCount <= 19)
                accum19a = (accum19a * 10) + (uint64_t)d;
            else if (significantDigitCount <= 34)
                accum19b = (accum19b * 10) + (uint64_t)d;
            else if (significantDigitCount == 34 + 1)
                residue = residueFromDecimalDigit(d);
            else
                residue = residueMerge(residue, residueFromDecimalDigit(d));
            if (hasDot)
                ++fractionalDigitCount;
            prevWasDigit = true;
            afterUnderscore = false;
        } else if (ch == '_') {
            if (!prevWasDigit)
                return "invalid underscore location";
            prevWasDigit = false;
            afterUnderscore = true;
        } else { /* '.' */
            if (afterUnderscore)
                return "invalid underscore location";
            if (hasDot)
                return "double radix dot";
            hasDot = true;
            prevWasDigit = false;
        }
        ch = chAt(str, &pos);
    }
    if (afterUnderscore)
        return "invalid underscore location";
    if (!hasCoefficientDigit)
        return "no coefficient digit";
    /* this path has at least one digit */
    if (ch == 'E' || ch == 'e') {
        ch = chAt(str, &pos);
        if (ch == '+' || ch == '-') {
            expSign = (ch == '-');
            ch = chAt(str, &pos);
        }
        bool hasExpDigit = false;
        int expSignificantDigitCount = 0;
        prevWasDigit = false;
        afterUnderscore = false;
        while ((ch >= '0' && ch <= '9') || ch == '_') {
            if (ch == '_') {
                if (!prevWasDigit)
                    return "invalid underscore location";
                prevWasDigit = false;
                afterUnderscore = true;
            } else {
                hasExpDigit = true;
                int eDigit = ch - '0';
                /* count while flushing leading zeros */
                expSignificantDigitCount += ((expSignificantDigitCount | eDigit) != 0);
                exp = exp * 10 + eDigit;
                prevWasDigit = true;
                afterUnderscore = false;
            }
            ch = chAt(str, &pos);
        }
        if (afterUnderscore)
            return "invalid underscore location";
        if (!hasExpDigit)
            return "no exponent digit";
        /* clamp once after the loop */
        if (expSignificantDigitCount > 4)
            exp = 9999;
    }
    if (ch != 0)
        return "unexpected char";

    uint64_t dw0 = accum19a;
    uint64_t dw1 = 0;
    if (significantDigitCount > 19) {
        int pow10 = (significantDigitCount < 34 ? significantDigitCount : 34) - 19;
        u128FmaPow10(&dw1, &dw0, accum19a, pow10, accum19b);
    }

    /*
     * Excess digits to the right of the radix point and excess digits beyond
     * the precision limit pull the quantum exponent in opposite directions,
     * so both corrections land here.
     */
    int signedExp = expSign ? -exp : exp;
    int excessDigits = (significantDigitCount > 34) ? significantDigitCount - 34 : 0;
    int qExp = signedExp - fractionalDigitCount + excessDigits;
    roundAndFinalizeFinite(hi, lo, formatIsBid, sign, qExp, dw1, dw0, residue);
    return NULL;
}

/* Parse Infinity, or its three-letter abbreviation Inf, case-insensitively. */
static const char *parseInfinityText(uint64_t *hi, uint64_t *lo, bool sign,
                                     int chFirst, const char *str, size_t startPos) {
    size_t pos = startPos;
    int ch = chFirst;
    int chPrevCode = 0;
    static const char target[] = "infinity";
    for (int i = 0; i < 8; ++i) {
        int targetCode = (unsigned char)target[i];
        if ((ch | 0x20) != targetCode) {
            if (ch == 0)
                break;
            return "failure parsing infinity";
        }
        chPrevCode = ch | 0x20;
        ch = chAt(str, &pos);
    }
    if (ch != 0 || (chPrevCode != 'f' && chPrevCode != 'y'))
        return "failure parsing Infinity";
    *hi = INF_PATTERN_HI | (sign ? SIGN_BIT : 0);
    *lo = 0;
    return NULL;
}

/*
 * Parse NaN, qNaN or sNaN, case-insensitively, with an optional payload of
 * decimal digits. A payload of 34 or more digits AS WRITTEN is non-canonical
 * and reads as zero, per IEEE 754-2019 3.5.2.
 *
 * The payload may be written bare (NaN123) or enclosed in parentheses
 * (NaN(123)). The parenthesised form is positional and balanced: the '(' must
 * follow the word immediately, at least one digit must appear, and the ')'
 * must be the last character of the input. So NaN(1)2(3), NaN(1, NaN(1] and
 * NaN() are all rejected, and NaN[1] and NaN{1} are not this form at all.
 *
 * This is the one place the codecs accept text the full decimal128 ports
 * reject, and it is deliberate: C's own strtod reads NAN(n-char-sequence), so
 * the spelling reaches a C caller from the platform itself. The resemblance
 * stops at the parentheses -- strtod's sequence is letters, digits and
 * underscores read as an implementation-defined integer (NAN(abc) and
 * NAN(0x1c8) are legal there), while a decimal128 payload is decimal digits.
 * This is a paren-tolerant spelling of our own payload syntax, not C's.
 */
static const char *parseNanText(uint64_t *hi, uint64_t *lo, bool formatIsBid, bool sign,
                                int chFirst, const char *str, size_t startPos) {
    size_t pos = startPos;
    int ch = chFirst;
    bool hasS = (ch | 0x20) == 's';
    bool hasQ = (ch | 0x20) == 'q';
    if (hasQ || hasS)
        ch = chAt(str, &pos);
    if (((ch | 0x20) != 'n') ||
        ((chAt(str, &pos) | 0x20) != 'a') ||
        ((chAt(str, &pos) | 0x20) != 'n'))
        return "failure parsing NaN";

    uint64_t payloadDw0 = 0;
    uint64_t payloadDw1 = 0;
    ch = chAt(str, &pos);
    bool hasParen = (ch == '(');
    if (hasParen)
        ch = chAt(str, &pos);
    if (hasParen || ch != 0) {
        int accumDigitCount = 0;
        uint64_t accum19a = 0;
        uint64_t accum19b = 0;
        while (ch >= '0' && ch <= '9') {
            int d = ch - '0';
            /* Count every digit written, leading zeros included. The
             * 33-digit limit is a limit on the payload as spelled, not on
             * its significant digits, so "NaN" followed by forty zeros and
             * a one is over the limit and reads as zero. Leading zeros
             * contribute nothing to the accumulators, so the value of a
             * payload within the limit is unaffected. */
            ++accumDigitCount;
            if (accumDigitCount <= 19)
                accum19a = (accum19a * 10) + (uint64_t)d;
            else
                accum19b = (accum19b * 10) + (uint64_t)d;
            ch = chAt(str, &pos);
        }
        if (accumDigitCount == 0)
            return hasParen ? "failure parsing NaN" : "non-digit NaN payload character";
        if (hasParen) {
            /* the ')' must be present and must end the input */
            if (ch != ')')
                return (ch == 0) ? "failure parsing NaN" : "non-digit NaN payload character";
            if (chAt(str, &pos) != 0)
                return "failure parsing NaN";
        } else if (ch != 0) {
            return "non-digit NaN payload character";
        }
        if (accumDigitCount > 33) {
            payloadDw0 = 0;
            payloadDw1 = 0;
        } else {
            payloadDw0 = accum19a;
            payloadDw1 = 0;
            if (accumDigitCount > 19)
                u128FmaPow10(&payloadDw1, &payloadDw0, accum19a, accumDigitCount - 19, accum19b);
        }
    }
    setNaN(hi, lo, formatIsBid, sign, hasS, payloadDw1, payloadDw0);
    return NULL;
}

/* Read the optional sign, then dispatch on the leading character. */
static const char *encodeStringOrError(uint64_t *hi, uint64_t *lo, bool formatIsBid, const char *str) {
    size_t pos = 0;
    int chFirst = chAt(str, &pos);
    int ch = chFirst;
    bool sign = (ch == '-');
    if (ch == '+' || ch == '-')
        ch = chAt(str, &pos);
    if ((ch >= '0' && ch <= '9') || ch == '.')
        return parseFiniteValueText(hi, lo, formatIsBid, sign, ch, str, pos);
    int chLower = ch | 0x20;
    if (chLower == 'i')
        return parseInfinityText(hi, lo, sign, ch, str, pos);
    if (chLower == 'n' || chLower == 's' || chLower == 'q')
        return parseNanText(hi, lo, formatIsBid, sign, ch, str, pos);
    if (chFirst == 0)
        return "empty string";
    return "invalid format";
}

/* --------------------------------------------------------------------------
 * The published surface
 * ------------------------------------------------------------------------ */

int d128text_bid128ToString(char *out, uint64_t hi, uint64_t lo) {
    demand(out != NULL);
    return decodeBid128toString(out, hi, lo);
}

int d128text_dpd128ToString(char *out, uint64_t hi, uint64_t lo) {
    demand(out != NULL);
    return decodeDpd128toString(out, hi, lo);
}

static const char *parseInto(uint64_t *hi, uint64_t *lo, bool formatIsBid, const char *text) {
    demand(hi != NULL && lo != NULL && text != NULL);
    const char *errorStr = encodeStringOrError(hi, lo, formatIsBid, text);
    if (errorStr != NULL) {
        /* Always write the out-params, so a caller who discards the return
         * value still observes a legal datum. The canonical positive quiet NaN
         * is the same bit pattern in both formats. */
        *hi = QNAN_PATTERN_HI;
        *lo = 0;
    }
    return errorStr;
}

const char *d128text_bid128Parse(uint64_t *hi, uint64_t *lo, const char *text) {
    return parseInto(hi, lo, true, text);
}

const char *d128text_dpd128Parse(uint64_t *hi, uint64_t *lo, const char *text) {
    return parseInto(hi, lo, false, text);
}
