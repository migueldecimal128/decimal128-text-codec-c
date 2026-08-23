// SPDX-License-Identifier: MIT
/*
 * DPD128 render and parse vectors, and the non-canonical declet behaviour --
 * transcribed from the reference implementation's TestDpd128ToString,
 * TestDpd128Parse and TestDpd128NonCanonical.
 *
 * Every expected pairing came originally from the shared string_bid_dpd
 * corpus, so these are an independent oracle rather than the codec grading
 * itself. They cover what the DPD layout actually distinguishes: both
 * combination-field cases, the exponent extremes, coefficients that exercise
 * every declet, and NaN payloads, which are coefficients and so are
 * declet-encoded too.
 *
 * DPD's non-canonicality is narrower than BID's, and structurally so. The
 * combination field cannot encode an out-of-range exponent, and a leading
 * digit of at most 9 followed by eleven declets of at most 999 reaches exactly
 * 10^34 - 1 -- so there is no coefficient-too-large rule to apply. What
 * remains is the 24 redundant declets and junk bits in an infinity's trailing
 * field.
 */

#include "decimal128_text_codec.h"
#include "test_util.h"

typedef struct { const char *hex; const char *text; } Case;

static const Case renderCases[] = {
    {"78000000000000000000000000000000", "Infinity"},
    {"F8000000000000000000000000000000", "-Infinity"},
    {"7C000000000000000000000000000000", "NaN"},
    {"FC000000000000000000000000000000", "-NaN"},
    {"7E000000000000000000000000000000", "sNaN"},
    {"22080000000000000000000000000000", "0"},
    {"A2080000000000000000000000000000", "-0"},
    {"22080000000000000000000000000001", "1"},
    {"A2080000000000000000000000000001", "-1"},
    {"22080000000000000000000000000009", "9"},
    {"22088000000000000000000000000008", "8E+2"},
    {"220B4000000000000000000000000008", "8E+13"},
    {"6E080FF3FCFF3FCFF3FCFF3FCFF3FCFF", "9999999999999999999999999999999999"},
    {"47FFC000000000000000000000000000", "1.000000000000000000000000000000000E+6144"},
    {"47FFC033790A64F9FE910BDA994B3322", "1.003198280313997724281686314944622E+6144"},
    {"00000000000000000000000000000000", "0E-6176"},
    {"80000000000000000000000000000000", "-0E-6176"},
    {"00000000000000000000000000000009", "9E-6176"},
    {"00010000000000000000000000000009", "9E-6172"},
    {"2202C000000000000020000000000000", "0.000008000000000000000"},
    {"23DE4000000000020000000000000000", "2.0000000000000000000E+1900"},
};

static const Case parseCases[] = {
    {"22080000000000000000000000000000", "0"},
    {"A2080000000000000000000000000000", "-0"},
    {"22080000000000000000000000000001", "1"},
    {"A2080000000000000000000000000001", "-1"},
    {"22080000000000000000000000000009", "9"},
    {"6E080FF3FCFF3FCFF3FCFF3FCFF3FCFF", "9999999999999999999999999999999999"},
    {"00000000000000000000000000000009", "9E-6176"},
    {"78000000000000000000000000000000", "Infinity"},
    {"F8000000000000000000000000000000", "-Infinity"},
    {"7C000000000000000000000000000000", "NaN"},
    {"7C000000000000000000000000000001", "NaN1"},
    {"7C000000000000000000000000000010", "NaN10"},
    {"7C000000000000000000000000000081", "NaN101"},
    {"FE0000000000000000000000534B9C1E", "-sNaN1234567890"},
    {"7C000000000000000BDA60F498388E4E", "NaN576460752303423488"},
    {"7C000000000000001000000000000000", "NaN1000000000000000000"},
    {"7C000928CF8BFD99413B2DCB4DF63344", "NaN112433781776314088827827199902644"},
    {"7E000000000152A2BCADC0B0E5E29592", "sNaN1284483683340543498125312"},
    {"FE00000340C8971F13CBA29DF88620BE", "-sNaN150148854371172722477708308918"},
    {"FC0000000001422AD0FF14CA4F4E18F6", "-NaN1208925819614629174706176"},
};

static const char *const failureCases[] = {
    "",
    "abc",
    "1.2.3",
    "1E",
    "1E+",
    "1_",
    "_1",
    "NaNx",
    ".",
};

/* Canonical output must never emit one of the 24 redundant declets. */
static const char *const encodeCases[] = {
    "0",
    "1",
    "9",
    "88",
    "99",
    "888",
    "899",
    "988",
    "999",
    "9999999999999999999999999999999999",
    "8888888888888888888888888888888888",
    "9898989898989898989898989898989898",
    "1.888888888888888888888888888888888E+99",
    "NaN999",
    "NaN888999888999888999888999888999",
    "-sNaN999999999999999999999999999999999",
};

/* The 24 declet patterns a canonical encoder never produces. All decode to
 * values whose last two digits are both 8 or 9. */
static const uint64_t nonCanonicalDeclets[] = {
    0x16E, 0x16F, 0x17E, 0x17F, 0x1EE, 0x1EF, 0x1FE, 0x1FF,
    0x26E, 0x26F, 0x27E, 0x27F, 0x2EE, 0x2EF, 0x2FE, 0x2FF,
    0x36E, 0x36F, 0x37E, 0x37F, 0x3EE, 0x3EF, 0x3FE, 0x3FF,
};

/* qExp 0, leading digit 0, positive */
#define BASE_HI 0x2208000000000000ULL

/* All eleven declets of the trailing significand field. */
static void decletsOf(uint64_t hi, uint64_t lo, uint64_t *out) {
    for (int i = 0; i < 6; ++i)
        out[i] = (lo >> (i * 10)) & 0x3FF;
    uint64_t high = ((hi & 0x3FFFFFFFFFFFULL) << 4) | (lo >> 60);
    for (int i = 0; i < 5; ++i)
        out[6 + i] = (high >> (i * 10)) & 0x3FF;
}

int main(void) {
    for (size_t i = 0; i < sizeof renderCases / sizeof renderCases[0]; ++i) {
        uint64_t hi, lo;
        CHECK(parseHex32(renderCases[i].hex, &hi, &lo), "bad hex %s", renderCases[i].hex);
        char got[D128TEXT_MAX];
        d128text_dpd128ToString(got, hi, lo);
        CHECK(strcmp(got, renderCases[i].text) == 0, "%s rendered <%s>, want <%s>",
              renderCases[i].hex, got, renderCases[i].text);
    }

    for (size_t i = 0; i < sizeof parseCases / sizeof parseCases[0]; ++i) {
        const char *text = parseCases[i].text;
        uint64_t hi = 0, lo = 0;
        const char *err = d128text_dpd128Parse(&hi, &lo, text);
        CHECK(err == NULL, "<%s> rejected: %s", text, err ? err : "");
        if (err != NULL)
            continue;
        char got[33];
        hex32(got, hi, lo);
        CHECK(strcmp(got, parseCases[i].hex) == 0, "<%s> gave %s, want %s",
              text, got, parseCases[i].hex);
        /* and every vector must round-trip */
        char back[D128TEXT_MAX];
        d128text_dpd128ToString(back, hi, lo);
        CHECK(strcmp(back, text) == 0, "<%s> round-tripped to <%s>", text, back);
    }

    for (size_t i = 0; i < sizeof failureCases / sizeof failureCases[0]; ++i) {
        const char *text = failureCases[i];
        uint64_t dHi = 0xFEED, dLo = 0xFACE, bHi = 0, bLo = 0;
        const char *dErr = d128text_dpd128Parse(&dHi, &dLo, text);
        const char *bErr = d128text_bid128Parse(&bHi, &bLo, text);
        CHECK(dErr != NULL, "<%s> was accepted", text);
        CHECK(bErr != NULL && dErr != NULL && strcmp(bErr, dErr) == 0,
              "the formats must reject <%s> the same way", text);
        CHECK(dHi == 0x7C00000000000000ULL && dLo == 0,
              "<%s> rejected but left %016llX%016llX", text,
              (unsigned long long)dHi, (unsigned long long)dLo);
    }

    /* a non-canonical declet renders exactly as its canonical spelling does,
     * in the low group, in the high group, and in a NaN payload */
    for (size_t i = 0; i < sizeof nonCanonicalDeclets / sizeof nonCanonicalDeclets[0]; ++i) {
        uint64_t d = nonCanonicalDeclets[i];
        char viaNonCanonical[D128TEXT_MAX], viaCanonical[D128TEXT_MAX];

        /* the canonical spelling of the same value, obtained through the
         * public surface: render the non-canonical form, re-parse it */
        d128text_dpd128ToString(viaNonCanonical, BASE_HI, d);
        uint64_t canonHi, canonLo;
        CHECK(d128text_dpd128Parse(&canonHi, &canonLo, viaNonCanonical) == NULL,
              "re-parsing the render of declet 0x%llX failed", (unsigned long long)d);
        CHECK(canonHi == BASE_HI, "declet 0x%llX re-parsed to a different high word",
              (unsigned long long)d);
        CHECK(canonLo != d, "declet 0x%llX should not be its own canonical form",
              (unsigned long long)d);

        d128text_dpd128ToString(viaCanonical, BASE_HI, canonLo);
        CHECK(strcmp(viaNonCanonical, viaCanonical) == 0,
              "low-group declet 0x%llX renders differently from its canonical form",
              (unsigned long long)d);

        d128text_dpd128ToString(viaNonCanonical, BASE_HI | (d << 6), 0);
        d128text_dpd128ToString(viaCanonical, BASE_HI | (canonLo << 6), 0);
        CHECK(strcmp(viaNonCanonical, viaCanonical) == 0,
              "high-group declet 0x%llX renders differently from its canonical form",
              (unsigned long long)d);

        d128text_dpd128ToString(viaNonCanonical, 0x7C00000000000000ULL, d);
        d128text_dpd128ToString(viaCanonical, 0x7C00000000000000ULL, canonLo);
        CHECK(strcmp(viaNonCanonical, viaCanonical) == 0,
              "NaN payload declet 0x%llX renders differently from its canonical form",
              (unsigned long long)d);
    }

    /* canonical output never emits a non-canonical declet */
    for (size_t i = 0; i < sizeof encodeCases / sizeof encodeCases[0]; ++i) {
        uint64_t hi, lo;
        CHECK(d128text_dpd128Parse(&hi, &lo, encodeCases[i]) == NULL,
              "parse failed for <%s>", encodeCases[i]);
        uint64_t declets[11];
        decletsOf(hi, lo, declets);
        for (int k = 0; k < 11; ++k)
            for (size_t j = 0; j < sizeof nonCanonicalDeclets / sizeof nonCanonicalDeclets[0]; ++j)
                CHECK(declets[k] != nonCanonicalDeclets[j],
                      "encoding of <%s> emitted non-canonical declet 0x%llX",
                      encodeCases[i], (unsigned long long)declets[k]);
    }

    /* infinity ignores whatever sits in the trailing field */
    static const uint64_t junk[] = {0, 1, 0x3FF, 0xFFFFFFFFFFFFFFFFULL, 0x1FF};
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; ++i) {
        char got[D128TEXT_MAX];
        d128text_dpd128ToString(got, 0x7800000000000000ULL, junk[i]);
        CHECK(strcmp(got, "Infinity") == 0, "infinity with trailing junk rendered <%s>", got);
        d128text_dpd128ToString(got, 0xF800000000000000ULL, junk[i]);
        CHECK(strcmp(got, "-Infinity") == 0, "-infinity with trailing junk rendered <%s>", got);
    }
    {
        char got[D128TEXT_MAX];
        d128text_dpd128ToString(got, 0x7800FFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
        CHECK(strcmp(got, "Infinity") == 0, "infinity with high-group junk rendered <%s>", got);
    }

    return testReport("test_dpd");
}
