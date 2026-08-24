// SPDX-License-Identifier: MIT
/*
 * Exhaustive tests for the DPD declet primitives.
 *
 * A declet is 10 bits, so the entire input domain is 1024 values for decode
 * and 1000 for encode. These enumerate both completely: no sampling, and no
 * oracle needed beyond the format's own definition.
 *
 * The primitives are static, so this includes the translation unit rather than
 * linking the archive. That keeps the published surface at four functions
 * while still testing what is behind it.
 */

#include "decimal128_text_codec.c"

#include "test_util.h"

int main(void) {
    for (uint64_t declet = 0; declet <= 1023; ++declet) {
        uint64_t v = dpdDecodeDeclet(declet);
        CHECK(v <= 999, "declet %llu decoded to %llu, out of 0..999",
              (unsigned long long)declet, (unsigned long long)v);
    }

    for (uint64_t n = 0; n <= 999; ++n) {
        uint64_t d = dpdEncodeDeclet(n);
        CHECK(d <= 1023, "value %llu encoded to %llu, wider than 10 bits",
              (unsigned long long)n, (unsigned long long)d);
    }

    /* encode is a right inverse of decode over the whole value domain */
    for (uint64_t n = 0; n <= 999; ++n) {
        uint64_t back = dpdDecodeDeclet(dpdEncodeDeclet(n));
        CHECK(back == n, "%llu -> declet -> %llu", (unsigned long long)n, (unsigned long long)back);
    }

    /*
     * Every value 0..999 is reachable, and exactly 1000 of the 1024 declets are
     * canonical. The other 24 are the redundant spellings: they decode to a
     * legal value, but re-encoding yields the canonical declet instead. Those
     * 24 are the ones an encoder never emits and a decoder must still accept.
     */
    bool seen[1000];
    memset(seen, 0, sizeof seen);
    int distinct = 0;
    int canonical = 0;
    int nonCanonical = 0;
    for (uint64_t declet = 0; declet <= 1023; ++declet) {
        uint64_t v = dpdDecodeDeclet(declet);
        if (v <= 999 && !seen[v]) {
            seen[v] = true;
            ++distinct;
        }
        if (dpdEncodeDeclet(v) == declet)
            ++canonical;
        else
            ++nonCanonical;
    }
    CHECK(distinct == 1000, "only %d of 0..999 are reachable", distinct);
    CHECK(canonical == 1000, "%d canonical declets, expected 1000", canonical);
    CHECK(nonCanonical == 24, "%d non-canonical declets, expected 24", nonCanonical);

    /* zero must round-trip as the all-zero declet, not merely as some encoding of 0 */
    CHECK(dpdEncodeDeclet(0) == 0, "zero does not encode to the all-zero declet");
    CHECK(dpdDecodeDeclet(0) == 0, "the all-zero declet does not decode to zero");

    /*
     * Regression pins. 9 and 999 are hand-derived from Cowlishaw's bcd2dpd
     * equations: for 999 all three digits are 1001, giving pqrstuvwxy =
     * 0011111111 = 0x0FF; for 9 only v and y are set, giving 0x009. The rest
     * are here to catch drift, not to prove the equations -- the exhaustive
     * properties above do that.
     */
    CHECK(dpdEncodeDeclet(9) == 0x009, "encode(9)");
    CHECK(dpdEncodeDeclet(999) == 0x0FF, "encode(999)");
    CHECK(dpdEncodeDeclet(5) == 0x005, "encode(5)");
    CHECK(dpdEncodeDeclet(8) == 0x008, "encode(8)");
    CHECK(dpdEncodeDeclet(57) == 0x057, "encode(57)");
    CHECK(dpdEncodeDeclet(98) == 0x05E, "encode(98)");
    CHECK(dpdEncodeDeclet(198) == 0x0DE, "encode(198)");
    CHECK(dpdEncodeDeclet(321) == 0x1A1, "encode(321)");
    CHECK(dpdEncodeDeclet(399) == 0x1DF, "encode(399)");

    /* binFromDeclets and decletsFromBin are inverse over the 18-digit range */
    static const uint64_t roundTrip[] = {
        0, 1, 9, 10, 999, 1000, 123456789, 999999999999999999ULL,
        100000000000000000ULL, 123456789012345678ULL,
    };
    for (size_t i = 0; i < sizeof roundTrip / sizeof roundTrip[0]; ++i) {
        uint64_t v = roundTrip[i];
        uint64_t back = binFromDeclets(decletsFromBin(v));
        CHECK(back == v, "bin %llu -> declets -> %llu",
              (unsigned long long)v, (unsigned long long)back);
    }

    return testReport("test_dpd_declet");
}
