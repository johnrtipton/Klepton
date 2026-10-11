// Equivalence check for the kl_audio convert() fast path.
//
// The fast path (ratio == 1.0, stereo->stereo) must be BIT-IDENTICAL to the
// general interpolation loop it short-circuits. This is a standalone copy of
// both, so the check does not need an audio device — it is the math being
// proved, and both bodies are byte-for-byte the ones in kl_audio.c
// (general loop = the FETCH-macro version, fast = the new one).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCH 2

typedef struct { double pos; float prev[MAXCH]; } st;

// ---- the GENERAL loop, as it was before the change ----
static size_t conv_general(const int16_t *src, size_t n, unsigned ich, unsigned och,
                           float *dst, st *s, float *out_prev, double *out_pos) {
    double ratio = 1.0;
    double pos = s->pos;
    size_t produced = 0;
    #define FETCH(idx, ch) \
        ((idx) < 0 ? s->prev[ch] \
                   : (float)src[(size_t)((idx) < (long)n ? (idx) : (long)n - 1) * ich \
                                + ((ch) < ich ? (ch) : 0)] * (1.0f / 32768.0f))
    while (pos < (double)n - 1.0 + 1e-9) {
        long i = (long)floor(pos);
        float f = (float)(pos - (double)i);
        for (unsigned c = 0; c < och; c++) {
            float a = FETCH(i, c), b = FETCH(i + 1, c);
            dst[produced * och + c] = a + (b - a) * f;
        }
        produced++;
        pos += ratio;
    }
    #undef FETCH
    if (n > 0)
        for (unsigned c = 0; c < och; c++)
            out_prev[c] = (float)src[(n - 1) * ich + (c < ich ? c : 0)] * (1.0f / 32768.0f);
    *out_pos = pos - (double)n;
    return produced;
}

// ---- the FAST path, as it is now ----
static size_t conv_fast(const int16_t *src, size_t n, unsigned ich, unsigned och,
                        float *dst, st *s, float *out_prev, double *out_pos) {
    if (ich == 2 && och == 2 && s->pos == 0.0 && n > 0) {
        const float k = 1.0f / 32768.0f;
        for (size_t i = 0; i < n * 2; i++) dst[i] = (float)src[i] * k;
        out_prev[0] = dst[(n - 1) * 2];
        out_prev[1] = dst[(n - 1) * 2 + 1];
        *out_pos = 0.0;
        return n;
    }
    return conv_general(src, n, ich, och, dst, s, out_prev, out_pos);
}

int main(void) {
    int fails = 0;
    srand(1234);
    for (int trial = 0; trial < 500; trial++) {
        size_t n = 1 + (size_t)(rand() % 4096);
        int16_t *src = malloc(n * 2 * sizeof *src);
        float *d1 = malloc((n + 8) * 2 * sizeof *d1);
        float *d2 = malloc((n + 8) * 2 * sizeof *d2);
        for (size_t i = 0; i < n * 2; i++) {
            // full int16 range incl. the extremes and the scale boundary
            int r = rand();
            src[i] = (int16_t)((r % 3 == 0) ? (r & 1 ? 32767 : -32768) : (r - 16384));
        }
        st s1 = { .pos = 0.0, .prev = {0,0} }, s2 = s1;
        float p1[2], p2[2]; double q1, q2;
        size_t n1 = conv_general(src, n, 2, 2, d1, &s1, p1, &q1);
        size_t n2 = conv_fast(src, n, 2, 2, d2, &s2, p2, &q2);
        if (n1 != n2) { printf("trial %d: frame count %zu != %zu\n", trial, n1, n2); fails++; }
        else if (memcmp(d1, d2, n1 * 2 * sizeof *d1) != 0) {
            printf("trial %d (n=%zu): SAMPLE MISMATCH\n", trial, n);
            fails++;
        } else if (memcmp(p1, p2, sizeof p1) != 0 || q1 != q2) {
            printf("trial %d: carry state mismatch (prev/pos)\n", trial); fails++;
        }
        free(src); free(d1); free(d2);
    }
    // ...and the one case the fast path must NOT take: a non-zero carry.
    {
        st s1 = { .pos = 0.5, .prev = {0.1f, -0.2f} }, s2 = s1;
        int16_t src[8] = {1,2,3,4,5,6,7,8};
        float d1[16], d2[16], p1[2], p2[2]; double q1, q2;
        size_t n1 = conv_general(src, 4, 2, 2, d1, &s1, p1, &q1);
        size_t n2 = conv_fast(src, 4, 2, 2, d2, &s2, p2, &q2);
        if (n1 != n2 || memcmp(d1, d2, n1 * 2 * sizeof *d1) != 0) {
            printf("carry case: fast path diverged from general\n"); fails++;
        }
    }
    printf(fails ? "FAIL: %d divergence(s)\n" : "PASS: fast path is bit-identical over 500 random buffers + the carry case\n", fails);
    return fails != 0;
}
