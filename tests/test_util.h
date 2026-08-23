// SPDX-License-Identifier: MIT
#ifndef DECIMAL128_TEXT_CODEC_TEST_UTIL_H
#define DECIMAL128_TEXT_CODEC_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int testFailures = 0;
static int testChecks = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        ++testChecks;                                            \
        if (!(cond)) {                                           \
            ++testFailures;                                      \
            if (testFailures <= 40) {                            \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                fprintf(stderr, __VA_ARGS__);                    \
                fputc('\n', stderr);                             \
            }                                                    \
        }                                                        \
    } while (0)

static inline int testReport(const char *name) {
    if (testFailures != 0) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, testFailures, testChecks);
        return 1;
    }
    printf("%s: %d checks passed\n", name, testChecks);
    return 0;
}

/* Render an encoding as 32 uppercase hex digits, the corpus column form. */
static inline void hex32(char *out, uint64_t hi, uint64_t lo) {
    static const char *digits = "0123456789ABCDEF";
    for (int i = 0; i < 16; ++i)
        out[i] = digits[(hi >> (60 - 4 * i)) & 0xF];
    for (int i = 0; i < 16; ++i)
        out[16 + i] = digits[(lo >> (60 - 4 * i)) & 0xF];
    out[32] = '\0';
}

/* Parse 32 hex digits, with or without a leading 0x. */
static inline bool parseHex32(const char *s, uint64_t *hi, uint64_t *lo) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint64_t v[2] = {0, 0};
    for (int i = 0; i < 32; ++i) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        v[i / 16] = (v[i / 16] << 4) | (uint64_t)d;
    }
    *hi = v[0];
    *lo = v[1];
    return true;
}

#endif
