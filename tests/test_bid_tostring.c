// SPDX-License-Identifier: MIT
/*
 * BID128 render vectors, transcribed from the reference implementation's
 * TestHexBid128ToString and TestBid128ToString.
 *
 * The hex table pins bit patterns against expected text -- cohort members,
 * non-canonical coefficients, the exponent extremes, and the '+' that must
 * appear at every exponent digit length.
 *
 * The round-trip table is the reference's canonical-text list. There it is
 * graded by the full decimal128-kotlin engine as an independent encode oracle;
 * here the independent oracle is the shared corpus in test_corpus, so this
 * table checks the weaker but still useful property that parse and render are
 * inverse over it, in both signs.
 */

#include "decimal128_text_codec.h"
#include "test_util.h"

typedef struct { const char *hex; const char *text; } HexCase;

static const HexCase hexCases[] = {
    {"7E000000000000008000000000000000", "sNaN9223372036854775808"},
    {"00000000000000000000000000000000", "0E-6176"},
    {"30400000000000000000000000000000", "0"},
    {"5FFE0000000000000000000000000000", "0E+6111"},
    {"B0400000000000000000000000000000", "-0"},
    {"30400000000000000000000000000001", "1"},
    {"3040000000000000000000000000000A", "10"},
    {"30400000000000000000000000000064", "100"},
    {"B0400000000000000000000000000001", "-1"},
    {"303E000000000000000000000000000A", "1.0"},
    {"303C0000000000000000000000000064", "1.00"},
    {"303A00000000000000000000000003E8", "1.000"},
    {"3040314DC6448D9338C15B0A00000000", "1000000000000000000000000000000000"},
    {"3041ED09BEAD87C0378D8E63FFFFFFFF", "9999999999999999999999999999999999"},
    {"6C7B86F26FC100000000000000000000", "0E+215"},
    {"78000000000000000000000000000000", "Infinity"},
    {"F8000000000000000000000000000000", "-Infinity"},
    {"78000000000000000000000000000001", "Infinity"},
    {"304A0000000000000000000000000001", "1E+5"},
    {"304C000000000000000000000000000F", "1.5E+7"},
    {"30520000000000000000000000000001", "1E+9"},
    {"30540000000000000000000000000001", "1E+10"},
    {"31060000000000000000000000000001", "1E+99"},
    {"31080000000000000000000000000001", "1E+100"},
    {"380E0000000000000000000000000001", "1E+999"},
    {"38100000000000000000000000000001", "1E+1000"},
    {"30420000000000000000000000000000", "0E+1"},
    {"B0420000000000000000000000000000", "-0E+1"},
    {"30520000000000000000000000000000", "0E+9"},
    {"30540000000000000000000000000000", "0E+10"},
    {"30400000000000000000000000000001", "1"},
    {"30C60000000000000000000000000001", "1E+67"},
    {"2FB00000000000000000000000000001", "1E-72"},
    {"5FFFED09BEAD87C0378D8E63FFFFFFFF", "9.999999999999999999999999999999999E+6144"},
    {"DFFFED09BEAD87C0378D8E63FFFFFFFF", "-9.999999999999999999999999999999999E+6144"},
    {"5FFFED09BEAD87C10000000000000000", "0E+6111"},
    {"5FFFED09BEAD87C0378D8E6400000000", "0E+6111"},
    {"00420000000000000000000000000001", "1E-6143"},
    {"00000000000000000000000000000001", "1E-6176"},
    {"80000000000000000000000000000001", "-1E-6176"},
    {"00200000000000000000000000000001", "1E-6160"},
    {"003E000000000000000000000000000A", "1.0E-6144"},
    {"78000000000000000000000000000000", "Infinity"},
    {"F8000000000000000000000000000000", "-Infinity"},
    {"7C000000000000000000000000000000", "NaN"},
    {"FC000000000000000000000000000000", "-NaN"},
    {"7C000000000000000000000000000001", "NaN1"},
    {"7E000000000000000000000000000000", "sNaN"},
    {"7E000000000000000000000000000001", "sNaN1"},
    {"7E000000000000007FFFFFFFFFFFFFFF", "sNaN9223372036854775807"},
    {"7E000000000000008000000000000000", "sNaN9223372036854775808"},
    {"7E00000000000000FFFFFFFFFFFFFFFF", "sNaN18446744073709551615"},
    {"7C00314DC6448D9338C15B09FFFFFFFF", "NaN999999999999999999999999999999999"},
    {"7C00314DC6448D9438C15B09FFFFFFFF", "NaN"},
    {"7C00314DC6448D9338C15B0A00000000", "NaN"},
    {"20491165061C532A535089A5C8F9DA39", "5.545101757363565785893501553728057E-2011"},
    {"812C0000000000000000000000000000", "-0E-6026"},
    {"2FFCA45894E4829567D9DA2155555555", "0.3333333333333333333333333333333333"},
    {"A9481E81F1AC7DF596DCD9BAA6738F4A", "-6.18767515491324263605526166998858E-860"},
    {"AFFDB28CFC8BFD252B61B866EF8F5F22", "-0.8813735870195430252326093249797922"},
};

static const char *const roundTripCases[] = {
    "9999999999999999999999999999999999",
    "1234567890123456789012345678901234",
    "Infinity",
    "NaN",
    "NaN1",
    "NaN987654321012345678",
    "sNaN",
    "sNaN123456789012345678901234567890123",
    "0",
    "0.0",
    "0.000000",
    "1",
    "999",
    "1234567890123456789012345678901234",
    "9999999999999999999999999999999999",
    "1.0",
    "1.00",
    "1.000",
    "1.0000",
    "1.00000",
    "1.000000",
    "1.0000000",
    "1.00000000",
    "1.000000000",
    "1.0000000000",
    "1.00000000000",
    "1.000000000000",
    "99.44",
    "0.000006",
    "123456789012345678901234567890.1234",
    "1234567890123456789012345678.901234",
    "999999999999999999999999999999999.9",
    "99999999999999999999999999999999.99",
    "9999999999999999999999999999999.999",
    "999999999999999999999999999999.9999",
    "99999999999999999999999999999.99999",
    "9999999999999999999999999999.999999",
    "6.02E+23",
    "9.999999999999999999999999999999999",
    "9.999999999999999999999999999999999E+6144",
    "9.999999999999999999999999999999999E-6143",
    "1E-6143",
    "1E-6144",
    "1E-6176",
    "1.2345678901234568E-6160",
};

int main(void) {
    for (size_t i = 0; i < sizeof hexCases / sizeof hexCases[0]; ++i) {
        uint64_t hi, lo;
        CHECK(parseHex32(hexCases[i].hex, &hi, &lo), "bad hex %s", hexCases[i].hex);
        char got[D128TEXT_MAX];
        int len = d128text_bid128ToString(got, hi, lo);
        CHECK(strcmp(got, hexCases[i].text) == 0, "%s rendered <%s>, want <%s>",
              hexCases[i].hex, got, hexCases[i].text);
        CHECK(len == (int)strlen(got), "%s: length %d disagrees with <%s>",
              hexCases[i].hex, len, got);
    }

    for (size_t i = 0; i < sizeof roundTripCases / sizeof roundTripCases[0]; ++i) {
        const char *text = roundTripCases[i];
        uint64_t hi, lo;
        const char *err = d128text_bid128Parse(&hi, &lo, text);
        CHECK(err == NULL, "parse <%s>: %s", text, err ? err : "");
        if (err != NULL)
            continue;
        char got[D128TEXT_MAX];
        d128text_bid128ToString(got, hi, lo);
        CHECK(strcmp(got, text) == 0, "<%s> round-tripped to <%s>", text, got);

        char negated[128];
        snprintf(negated, sizeof negated, "-%s", text);
        err = d128text_bid128Parse(&hi, &lo, negated);
        CHECK(err == NULL, "parse <%s>: %s", negated, err ? err : "");
        if (err != NULL)
            continue;
        d128text_bid128ToString(got, hi, lo);
        CHECK(strcmp(got, negated) == 0, "<%s> round-tripped to <%s>", negated, got);
    }

    return testReport("test_bid_tostring");
}
