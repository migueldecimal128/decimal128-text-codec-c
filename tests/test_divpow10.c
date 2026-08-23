// SPDX-License-Identifier: MIT
/*
 * The table-free divide kernel, against an independent oracle.
 *
 * This test exists because the shared corpus barely reaches this code. The
 * measurement on the reference implementation: the two render suites call the
 * kernel ZERO times, and only the parse suite reaches it at all -- 20 calls,
 * across five distinct powers. A codec can be green on 86,126 corpus rows with
 * this kernel substantially wrong.
 *
 * The reference's own kernel test was deleted upstream because it had drifted
 * into testing a private copy of the algorithm rather than the production one.
 * C can call the real function directly, so this reconstructs the coverage
 * against the classes of input that break reciprocal-free division: the decade
 * edges, the 2^64 seam, the 49-bit boundary in the shifted dividend, the
 * worst-case remainder r1 = 5^n - 1, and the full 113-bit input width.
 *
 * The oracle is native 128-bit division, which shares no code with the kernel.
 * Where the compiler has no __int128 (MSVC), the oracle is unavailable and the
 * test reports that rather than pretending to have run.
 */

#include "decimal128_text_codec.c"

#include "test_util.h"

#if defined(__SIZEOF_INT128__)

static void checkOne(uint64_t hi, uint64_t lo, int pow10) {
    /* the kernel's domain is a 113-bit dividend */
    hi &= (1ULL << 49) - 1;

    uint64_t qHi = 0xFEED, qLo = 0xFACE;
    uint64_t rem = u113DivModPow10Max9(&qHi, &qLo, hi, lo, pow10);

    unsigned __int128 value = ((unsigned __int128)hi << 64) | lo;
    unsigned __int128 divisor = 1;
    for (int i = 0; i < pow10; ++i)
        divisor *= 10;
    unsigned __int128 wantQ = value / divisor;
    unsigned __int128 wantR = value % divisor;

    unsigned __int128 gotQ = ((unsigned __int128)qHi << 64) | qLo;
    CHECK(gotQ == wantQ, "div %016llX%016llX by 10^%d: quotient %016llX%016llX, want %016llX%016llX",
          (unsigned long long)hi, (unsigned long long)lo, pow10,
          (unsigned long long)(uint64_t)(gotQ >> 64), (unsigned long long)(uint64_t)gotQ,
          (unsigned long long)(uint64_t)(wantQ >> 64), (unsigned long long)(uint64_t)wantQ);
    CHECK(rem == (uint64_t)wantR, "div %016llX%016llX by 10^%d: remainder %llu, want %llu",
          (unsigned long long)hi, (unsigned long long)lo, pow10,
          (unsigned long long)rem, (unsigned long long)(uint64_t)wantR);
}

static void checkValue(unsigned __int128 v, int pow10) {
    checkOne((uint64_t)(v >> 64), (uint64_t)v, pow10);
}

int main(void) {
    static const int POWERS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    for (size_t pi = 0; pi < sizeof POWERS / sizeof POWERS[0]; ++pi) {
        int n = POWERS[pi];

        unsigned __int128 pow10 = 1;
        for (int i = 0; i < n; ++i)
            pow10 *= 10;
        unsigned __int128 pow5 = 1;
        for (int i = 0; i < n; ++i)
            pow5 *= 5;

        /* trivial, and the decade edges around the divisor itself */
        checkValue(0, n);
        checkValue(1, n);
        checkValue(pow10 - 1, n);
        checkValue(pow10, n);
        checkValue(pow10 + 1, n);
        checkValue(pow10 * 10 - 1, n);
        checkValue(pow10 * 10, n);

        /* every decade up to the coefficient bound, and its neighbours */
        unsigned __int128 decade = 1;
        for (int d = 0; d <= 33; ++d) {
            checkValue(decade - 1, n);
            checkValue(decade, n);
            checkValue(decade + 1, n);
            decade *= 10;
        }

        /* powers of two: the 2^64 seam, the 49-bit boundary in the shifted
         * dividend, and the full 113-bit width */
        for (int k = 0; k <= 112; ++k) {
            unsigned __int128 p2 = (unsigned __int128)1 << k;
            checkValue(p2 - 1, n);
            checkValue(p2, n);
            checkValue(p2 + 1, n);
        }

        /* worst-case remainders: r == 5^n - 1 and r == 10^n - 1 */
        for (int mult = 1; mult <= 64; ++mult) {
            checkValue(pow5 * (unsigned __int128)mult - 1, n);
            checkValue(pow10 * (unsigned __int128)mult - 1, n);
            checkValue(pow10 * (unsigned __int128)mult + pow10 / 2, n);
        }

        /* 2^113 - 10^k, the reference's other named worst case */
        unsigned __int128 top = ((unsigned __int128)1 << 113) - 1;
        unsigned __int128 tenK = 1;
        for (int k = 0; k <= 33; ++k) {
            checkValue(top - tenK, n);
            tenK *= 10;
        }

        /* the decimal128 coefficient bounds */
        checkValue((unsigned __int128)MAXX_COEFF_34_HI << 64 | MAXX_COEFF_34_LO, n);
        checkValue(((unsigned __int128)MAXX_COEFF_34_HI << 64 | MAXX_COEFF_34_LO) - 1, n);
        checkValue((unsigned __int128)MIN_PRECISION_34_COEFF_HI << 64 | MIN_PRECISION_34_COEFF_LO, n);

        /* a deterministic sweep across the whole 113-bit domain */
        uint64_t state = 0x243F6A8885A308D3ULL ^ (uint64_t)n;
        for (int i = 0; i < 20000; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t lo = state;
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t hi = state;
            checkOne(hi, lo, n);
        }
    }

    return testReport("test_divpow10");
}

#else

int main(void) {
    printf("test_divpow10: skipped, no __int128 oracle on this compiler\n");
    return 0;
}

#endif
