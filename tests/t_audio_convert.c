// Compare the actual production converter against the pre-PR interpolation
// loop, including carry and rate/channel conversion. No audio device needed.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/media/kl_audio.c"
#define MAXCH 2

typedef struct { double pos; float prev[MAXCH]; } st;

// ---- the GENERAL loop, as it was before the change ----
static size_t conv_general(const int16_t *src, size_t n, unsigned ich, unsigned och,
                           float *dst, st *s, float *out_prev, double *out_pos) {
    double ratio = (double)g_in_rate / g_out_rate;
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

// Exercise the production converter without opening an audio device.
static size_t conv_fast(const int16_t *src, size_t n, unsigned ich, unsigned och,
                       float *dst, st *s, float *prev, double *pos) {
    g_in_ch = ich; g_out_ch = och;
    g_pos = s->pos; memcpy(g_prev, s->prev, sizeof g_prev);
    float *out = NULL;
    size_t frames = convert(src, n, &out);
    memcpy(dst, out, frames * och * sizeof *out);
    memcpy(prev, g_prev, sizeof g_prev); *pos = g_pos;
    return frames;
}

int main(void) {
    int fails = 0;
    srand(1234);
    for (int trial = 0; trial < 500; trial++) {
        g_in_rate = trial % 2 ? 48000 : (trial % 3 ? 44100 : 96000);
        g_out_rate = 48000;
        unsigned ich = trial % 2 ? 2 : 1, och = 2;
        size_t n = 1 + (size_t)(rand() % 4096);
        int16_t *src = malloc(n * 2 * sizeof *src);
        float *d1 = malloc((n * 2 + 8) * 2 * sizeof *d1);
        float *d2 = malloc((n * 2 + 8) * 2 * sizeof *d2);
        for (size_t i = 0; i < n * 2; i++) {
            // full int16 range incl. the extremes and the scale boundary
            int r = rand();
            src[i] = (int16_t)((r % 3 == 0) ? (r & 1 ? 32767 : -32768) : (r - 16384));
        }
        st s1 = { .pos = trial % 2 ? 0 : (trial % 3 - 1) * 0.5, .prev = {0.2f,-0.3f} }, s2 = s1;
        float p1[2], p2[2]; double q1, q2;
        size_t n1 = conv_general(src, n, ich, och, d1, &s1, p1, &q1);
        size_t n2 = conv_fast(src, n, ich, och, d2, &s2, p2, &q2);
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
        g_in_rate = 48000; g_out_rate = 48000;
        st s1 = { .pos = 0.5, .prev = {0.1f, -0.2f} }, s2 = s1;
        int16_t src[8] = {1,2,3,4,5,6,7,8};
        float d1[16], d2[16], p1[2], p2[2]; double q1, q2;
        size_t n1 = conv_general(src, 4, 2, 2, d1, &s1, p1, &q1);
        size_t n2 = conv_fast(src, 4, 2, 2, d2, &s2, p2, &q2);
        if (n1 != n2 || memcmp(d1, d2, n1 * 2 * sizeof *d1) != 0) {
            printf("carry case: fast path diverged from general\n"); fails++;
        }
    }
    printf(fails ? "FAIL: %d divergence(s)\n" : "PASS: production converter matches 500 rate/channel/carry cases\n", fails);
    // Isolated converter timing: excludes the live ring's pacing and device.
    int16_t pcm[2048] = {0}; float *out;
    g_in_rate = 48000; g_out_rate = 48000; g_in_ch = 2; g_out_ch = 2; g_pos = 0;
    struct timespec a, b;
    volatile float sink = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 20000; i++) {
        pcm[0] = (int16_t)i;
        convert(pcm, 1024, &out); sink += out[0];
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("production converter: %.2f ns/1024 stereo frames (sink %g)\n",
        ((b.tv_sec-a.tv_sec)*1e9 + b.tv_nsec-a.tv_nsec)/20000, (double)sink);
    free(g_scratch);
    return fails != 0;
}
