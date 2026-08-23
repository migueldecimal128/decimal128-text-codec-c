// SPDX-License-Identifier: MIT
/*
 * The four conformance tables, over the shared text_bid_dpd corpus.
 *
 * The vectors are read straight out of decimal128-resources, mounted as a
 * submodule at <repo>/resources -- no copy is carried here. They were generated
 * by the decimal128-c engine through its public API. That engine rounds by a
 * different algorithm than this codec does, so agreement is a cross-check
 * rather than a tautology.
 *
 *   T1 DECODE  bid_hex -> text and dpd_hex -> text      (100, noncanonical)
 *   T2 ENCODE  text -> bid_hex and text -> dpd_hex      (100)
 *   T3 PARSE   input_text -> bits, both formats         (parse, 945 rows)
 *   T4 ERROR   malformed input_text -> rejection        (error, 133 rows)
 *
 * Plus the cross-format assertion: for every row both encodings must render to
 * the same text. That is the strongest property the corpus encodes.
 *
 * The `canonical` file (84,764 rows) is held out of this executable's default
 * run: pass its path as argv[1] to run it as T1 and T2. CMake registers that as
 * the separate test_corpus_canonical test, so the portable (no-__int128) build
 * does not pay for those rows twice.
 *
 * READER NOTE, and this bites every port: `parse` and `error` double-quote
 * their text column with \\ and \" escapes, while the other three leave it
 * bare. Those two tables are input-driven and must carry the empty string,
 * leading and trailing spaces, an embedded comma, an embedded quote and an
 * embedded backslash. A naive comma split silently corrupts four rows and
 * still passes, because a malformed input fails to parse either way -- which
 * is why the row counts below are asserted.
 */

#include <stdlib.h>

#include "decimal128_text_codec.h"
#include "test_util.h"

typedef struct {
    char text[512];
    char bidHex[40];
    char dpdHex[40];
    char status[64];
    bool hasStatus;
} Row;

static const char *skipSpace(const char *p) {
    while (*p == ' ' || *p == '\t')
        ++p;
    return p;
}

/* Read one comma-delimited field, stopping at ',' or '#' or end of line, with
 * surrounding whitespace trimmed. */
static const char *readField(const char *p, char *dst, size_t cap) {
    p = skipSpace(p);
    size_t n = 0;
    const char *end = p;
    while (*end != '\0' && *end != ',' && *end != '#')
        ++end;
    const char *trim = end;
    while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t'))
        --trim;
    for (const char *q = p; q < trim && n + 1 < cap; ++q)
        dst[n++] = *q;
    dst[n] = '\0';
    return end;
}

/* Read the quoted text column, honouring \\ and \" escapes. */
static const char *readQuoted(const char *p, char *dst, size_t cap) {
    size_t n = 0;
    ++p; /* opening quote */
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p != '\0')
            c = *p++;
        if (n + 1 < cap)
            dst[n++] = c;
    }
    dst[n] = '\0';
    if (*p == '"')
        ++p;
    return p;
}

/* Returns false at end of file. Blank and comment lines are skipped. */
static bool readRow(FILE *f, Row *row) {
    char line[4096];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        const char *p = skipSpace(line);
        if (*p == '\0' || *p == '#')
            continue;

        if (*p == '"')
            p = readQuoted(p, row->text, sizeof row->text);
        else
            p = readField(p, row->text, sizeof row->text);

        p = skipSpace(p);
        if (*p == ',')
            ++p;
        p = readField(p, row->bidHex, sizeof row->bidHex);
        p = skipSpace(p);
        if (*p == ',')
            ++p;
        p = readField(p, row->dpdHex, sizeof row->dpdHex);
        p = skipSpace(p);
        row->hasStatus = false;
        row->status[0] = '\0';
        if (*p == ',') {
            ++p;
            p = readField(p, row->status, sizeof row->status);
            row->hasStatus = (row->status[0] != '\0');
        }
        return true;
    }
    return false;
}

static FILE *openVectors(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    return f;
}

/* Every row: both encodings must render to the expected text, and therefore to
 * each other. */
static int runDecodeTable(const char *path, const char *label) {
    FILE *f = openVectors(path);
    Row row;
    int rows = 0;
    while (readRow(f, &row)) {
        ++rows;
        uint64_t bidHi, bidLo, dpdHi, dpdLo;
        if (!parseHex32(row.bidHex, &bidHi, &bidLo) || !parseHex32(row.dpdHex, &dpdHi, &dpdLo)) {
            CHECK(false, "%s row %d: malformed hex column <%s> <%s>", label, rows, row.bidHex, row.dpdHex);
            continue;
        }
        char bidText[D128TEXT_MAX];
        char dpdText[D128TEXT_MAX];
        int bidLen = d128text_bid128ToString(bidText, bidHi, bidLo);
        int dpdLen = d128text_dpd128ToString(dpdText, dpdHi, dpdLo);
        CHECK(strcmp(bidText, row.text) == 0, "%s row %d: bid %s rendered <%s>, want <%s>",
              label, rows, row.bidHex, bidText, row.text);
        CHECK(strcmp(dpdText, row.text) == 0, "%s row %d: dpd %s rendered <%s>, want <%s>",
              label, rows, row.dpdHex, dpdText, row.text);
        CHECK(bidLen == (int)strlen(bidText), "%s row %d: bid length %d disagrees with <%s>",
              label, rows, bidLen, bidText);
        CHECK(dpdLen == (int)strlen(dpdText), "%s row %d: dpd length %d disagrees with <%s>",
              label, rows, dpdLen, dpdText);
    }
    fclose(f);
    return rows;
}

/* Round-trippable rows: the text must encode back to the same bits. */
static int runEncodeTable(const char *path, const char *label) {
    FILE *f = openVectors(path);
    Row row;
    int rows = 0;
    while (readRow(f, &row)) {
        ++rows;
        uint64_t hi = 0xFEED, lo = 0xFACE;
        const char *err = d128text_bid128Parse(&hi, &lo, row.text);
        CHECK(err == NULL, "%s row %d: bid parse <%s> rejected: %s", label, rows, row.text, err ? err : "");
        if (err == NULL) {
            char got[33];
            hex32(got, hi, lo);
            uint64_t wantHi, wantLo;
            parseHex32(row.bidHex, &wantHi, &wantLo);
            char want[33];
            hex32(want, wantHi, wantLo);
            CHECK(strcmp(got, want) == 0, "%s row %d: bid parse <%s> gave %s, want %s",
                  label, rows, row.text, got, want);
        }
        err = d128text_dpd128Parse(&hi, &lo, row.text);
        CHECK(err == NULL, "%s row %d: dpd parse <%s> rejected: %s", label, rows, row.text, err ? err : "");
        if (err == NULL) {
            char got[33];
            hex32(got, hi, lo);
            uint64_t wantHi, wantLo;
            parseHex32(row.dpdHex, &wantHi, &wantLo);
            char want[33];
            hex32(want, wantHi, wantLo);
            CHECK(strcmp(got, want) == 0, "%s row %d: dpd parse <%s> gave %s, want %s",
                  label, rows, row.text, got, want);
        }
    }
    fclose(f);
    return rows;
}

/* T3: input text, and the bits each format must produce. This is the only
 * table that reaches the rounding surface at all. */
static int runParseTable(const char *path) {
    FILE *f = openVectors(path);
    Row row;
    int rows = 0;
    while (readRow(f, &row)) {
        ++rows;
        uint64_t hi = 0xFEED, lo = 0xFACE;
        const char *err = d128text_bid128Parse(&hi, &lo, row.text);
        CHECK(err == NULL, "parse row %d: bid rejected <%s>: %s", rows, row.text, err ? err : "");
        if (err == NULL) {
            char got[33], want[33];
            uint64_t wantHi, wantLo;
            hex32(got, hi, lo);
            parseHex32(row.bidHex, &wantHi, &wantLo);
            hex32(want, wantHi, wantLo);
            CHECK(strcmp(got, want) == 0, "parse row %d: bid <%s> gave %s, want %s",
                  rows, row.text, got, want);
        }
        err = d128text_dpd128Parse(&hi, &lo, row.text);
        CHECK(err == NULL, "parse row %d: dpd rejected <%s>: %s", rows, row.text, err ? err : "");
        if (err == NULL) {
            char got[33], want[33];
            uint64_t wantHi, wantLo;
            hex32(got, hi, lo);
            parseHex32(row.dpdHex, &wantHi, &wantLo);
            hex32(want, wantHi, wantLo);
            CHECK(strcmp(got, want) == 0, "parse row %d: dpd <%s> gave %s, want %s",
                  rows, row.text, got, want);
        }
    }
    fclose(f);
    return rows;
}

/*
 * T4: malformed input. Both formats must reject, must agree on the message,
 * and must still have written the canonical positive quiet NaN -- the corpus
 * carries that pattern in its bits columns precisely so a port can assert the
 * written value and not merely the rejection.
 */
static int runErrorTable(const char *path) {
    FILE *f = openVectors(path);
    Row row;
    int rows = 0;
    while (readRow(f, &row)) {
        ++rows;
        uint64_t bHi = 0xFEED, bLo = 0xFACE, dHi = 0xFEED, dLo = 0xFACE;
        const char *bErr = d128text_bid128Parse(&bHi, &bLo, row.text);
        const char *dErr = d128text_dpd128Parse(&dHi, &dLo, row.text);
        CHECK(bErr != NULL, "error row %d: bid ACCEPTED <%s>, corpus says %s", rows, row.text, row.status);
        CHECK(dErr != NULL, "error row %d: dpd ACCEPTED <%s>, corpus says %s", rows, row.text, row.status);
        if (bErr != NULL && dErr != NULL)
            CHECK(strcmp(bErr, dErr) == 0, "error row %d: the formats disagree on <%s>: %s vs %s",
                  rows, row.text, bErr, dErr);
        uint64_t wantHi, wantLo;
        parseHex32(row.bidHex, &wantHi, &wantLo);
        CHECK(bHi == wantHi && bLo == wantLo, "error row %d: bid <%s> left %016llX%016llX",
              rows, row.text, (unsigned long long)bHi, (unsigned long long)bLo);
        CHECK(dHi == wantHi && dLo == wantLo, "error row %d: dpd <%s> left %016llX%016llX",
              rows, row.text, (unsigned long long)dHi, (unsigned long long)dLo);
    }
    fclose(f);
    return rows;
}

int main(int argc, char **argv) {
    int rows;

    rows = runDecodeTable(VECTOR_DIR "/text_bid128_dpd128_100.csv.txt", "100");
    CHECK(rows == 100, "the 100 file has %d rows, expected 100", rows);
    rows = runEncodeTable(VECTOR_DIR "/text_bid128_dpd128_100.csv.txt", "100");
    CHECK(rows == 100, "the 100 file has %d rows, expected 100", rows);

    rows = runDecodeTable(VECTOR_DIR "/text_bid128_dpd128_noncanonical.csv.txt", "noncanonical");
    CHECK(rows == 1267, "the noncanonical file has %d rows, expected 1267", rows);

    rows = runParseTable(VECTOR_DIR "/text_bid128_dpd128_parse.csv.txt");
    CHECK(rows == 1052, "the parse file has %d rows, expected 1052", rows);

    rows = runErrorTable(VECTOR_DIR "/text_bid128_dpd128_error.csv.txt");
    CHECK(rows == 133, "the error file has %d rows, expected 133", rows);

    if (argc > 1) {
        rows = runDecodeTable(argv[1], "canonical");
        printf("canonical: %d rows decoded\n", rows);
        rows = runEncodeTable(argv[1], "canonical");
        printf("canonical: %d rows encoded\n", rows);
        CHECK(rows == 84783, "the canonical file has %d rows, expected 84783", rows);
    }

    return testReport("test_corpus");
}
