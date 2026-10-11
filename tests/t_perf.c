// Microbenchmarks for the hot-path findings in scratch/perf/.
//
// These are the runtime's OWN entry points, driven the way a guest drives
// them, so a number here is the real per-call cost and a change to the
// implementation moves it. Nothing in here needs a guest, a headset, ANGLE or
// Metal — it links $(RUNTIME) like t_haptics does, on purpose: the point is to
// be runnable on any checkout, in `make perf`, and in a before/after pair.
//
// Every bench reports ns/op as the BEST of KL_PERF_ROUNDS rounds (best-of, not
// mean: the machine is a shared laptop and the minimum is the cleanest
// estimator of the code's own cost). Set KL_PERF_N to change the inner
// iteration count. Grep-able output: one "[bench] <name>: N ns/op" per line.
//
//   make perf            # default sizes (~2 s)
//   KL_PERF_N=500000 make perf
//
// What each one is for:
//   env_hit/env_miss    the per-frame getenv scans (build-app-driver X2/X9)
//   shim_hit/shim_miss  kl_shim_lookup's linear table scan (translation T2)
//   mtx_uncontended     the shim mutex path incl. waiter bookkeeping (T3)
//   mtx_contended       the same under 4 threads — the CAS-array funnel
//   ovrp_get            ovrp_slot + ovrp_log_arg per entry call (xr XR1/XR2)
//   jni_call            binding re-resolution per Call*Method (jni J1)
//   audio_convert       the resampler over kl_audio_write_src (media M1)
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/event.h>

#include "klepton.h"
#include "../runtime/kl_jni.h"
#include "../runtime/jni/kl_jni_int.h"
#include "../runtime/libc/kl_va.h"

// ---- entry points the runtime does not put in a header ----
void  *kl_shim_lookup(const char *name);                    // runtime/libc/kl_shim.c
int    klb_pthread_mutex_lock(void *m);                     // runtime/libc/kl_pthread.c
int    klb_pthread_mutex_unlock(void *m);
void  *kl_ovrp_sym(const char *name);                       // runtime/xr/kl_ovrp.c
size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes);
int    kl_audio_open(unsigned rate, unsigned channels, unsigned bits);
void   kl_audio_close(void);
int    klb_epoll_wait(int fd, void *events, int maxevents, int timeout_ms);

static volatile uint64_t g_sink;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long g_n      = 200000;   // inner iterations
static int  g_rounds = 5;

// Run fn(ctx, i) g_n times, KL_PERF_ROUNDS times; print the best round.
static void bench(const char *name, void (*fn)(void *ctx, long n), void *ctx) {
    double best = 1e30;
    for (int r = 0; r < g_rounds; r++) {
        uint64_t t0 = now_ns();
        fn(ctx, g_n);
        uint64_t dt = now_ns() - t0;
        double nsop = (double)dt / (double)g_n;
        if (nsop < best) best = nsop;
    }
    printf("[bench] %-18s %8.2f ns/op\n", name, best);
    fflush(stdout);
}

// ---- env ----
static void b_env_hit(void *ctx, long n) {
    (void)ctx;
    uint64_t acc = 0;
    for (long i = 0; i < n; i++) acc += (uint64_t)kl_env_on("KL_PERF_PRESENT", 0);
    g_sink += acc;
}
static void b_env_miss(void *ctx, long n) {
    (void)ctx;
    uint64_t acc = 0;
    for (long i = 0; i < n; i++) acc += (uint64_t)kl_env_on("KL_PERF_ABSENT", 0);
    g_sink += acc;
}

// ---- shim table ----
static void b_shim_hit(void *ctx, long n) {
    (void)ctx;
    uint64_t acc = 0;
    for (long i = 0; i < n; i++) acc += (uintptr_t)kl_shim_lookup("strlen") != 0;
    g_sink += acc;
}
static void b_shim_miss(void *ctx, long n) {
    (void)ctx;
    uint64_t acc = 0;
    for (long i = 0; i < n; i++) acc += (uintptr_t)kl_shim_lookup("no_such_symbol_xyz") != 0;
    g_sink += acc;
}

// ---- pthread shim ----
static pthread_mutex_t g_bench_mtx = PTHREAD_MUTEX_INITIALIZER;
static void b_mtx(void *ctx, long n) {
    (void)ctx;
    for (long i = 0; i < n; i++) {
        klb_pthread_mutex_lock(&g_bench_mtx);
        klb_pthread_mutex_unlock(&g_bench_mtx);
    }
}

struct mtx_thread { pthread_t th; long n; };
static void *mtx_worker(void *arg) {
    struct mtx_thread *t = arg;
    for (long i = 0; i < t->n; i++) {
        klb_pthread_mutex_lock(&g_bench_mtx);
        klb_pthread_mutex_unlock(&g_bench_mtx);
    }
    return NULL;
}
// 4 threads hammering one mutex: the waiter-table CAS funnel shows up here.
static void bench_mtx_contended(void) {
    enum { NT = 4 };
    struct mtx_thread t[NT];
    long per = g_n / NT;
    double best = 1e30;
    for (int r = 0; r < g_rounds; r++) {
        uint64_t t0 = now_ns();
        for (int i = 0; i < NT; i++) { t[i].n = per; pthread_create(&t[i].th, NULL, mtx_worker, &t[i]); }
        for (int i = 0; i < NT; i++) pthread_join(t[i].th, NULL);
        double nsop = (double)(now_ns() - t0) / (double)(per * NT);
        if (nsop < best) best = nsop;
    }
    printf("[bench] %-18s %8.2f ns/op\n", "mtx_contended(4t)", best);
    fflush(stdout);
}

// ---- ovrp entry ----
static void b_ovrp(void *ctx, long n) {
    uint64_t (*fn)(int, uint32_t *) = ctx;
    uint64_t acc = 0;
    uint32_t out = 0;
    for (long i = 0; i < n; i++) acc += fn(9, &out);
    g_sink += acc + out;
}

// ---- jni dispatch ----
static void b_jni(void *ctx, long n) {
    void **c = ctx;                 // { env, cls, mid }
    uint64_t acc = 0;
    for (long i = 0; i < n; i++) {
        klj_val r = klj_call(c[0], c[1], c[2], NULL, 'J');
        acc += r.j;
    }
    g_sink += acc;
}

// ---- audio converter (through the real write path) ----
static void b_audio(void *ctx, long n) {
    (void)ctx;
    enum { FRAMES = 1024 };
    static int16_t pcm[FRAMES * 2];
    for (int i = 0; i < FRAMES * 2; i++) pcm[i] = (int16_t)(i * 7);
    for (long i = 0; i < n; i++) g_sink += kl_audio_write_src(NULL, pcm, sizeof pcm);
}

static void b_epoll(void *ctx, long n) {
    int fd = *(int *)ctx;
    struct { uint32_t events, pad; uint64_t data; } events[64];
    for (long i = 0; i < n; i++) {
        if (klb_epoll_wait(fd, events, 64, 0) < 0) { perror("epoll benchmark"); exit(1); }
    }
}

int main(void) {
    const char *e = getenv("KL_PERF_N");
    if (e) g_n = atol(e);
    e = getenv("KL_PERF_ROUNDS");
    if (e) g_rounds = atoi(e);
    if (g_n < 1) g_n = 1;
    if (g_rounds < 1) g_rounds = 1;

    printf("=== klepton microbenchmarks (best of %d rounds, n=%ld) ===\n", g_rounds, g_n);

    setenv("KL_PERF_PRESENT", "1", 1);
    bench("env_hit",  b_env_hit,  NULL);
    unsetenv("KL_PERF_PRESENT");
    bench("env_miss", b_env_miss, NULL);

    bench("shim_hit",  b_shim_hit,  NULL);
    bench("shim_miss", b_shim_miss, NULL);

    bench("mtx_uncontended", b_mtx, NULL);
    bench_mtx_contended();

    int epfd = kqueue();
    if (epfd < 0) { perror("kqueue"); return 1; }
    bench("epoll_poll(64)", b_epoll, &epfd);
    close(epfd);

    // ovrp: resolve the way a guest does (through kl_ovrp_sym), then call.
    // Pre-resolve a realistic spread of entry points first: ovrp_slot's table
    // is per-DISTINCT-name, and a real guest brings dozens to hundreds (Unity's
    // OVRPlugin init resolves its whole surface). Benching against a table of
    // one would measure nothing.
    {
        static const char *const names[] = {
            "ovrp_Initialize", "ovrp_Initialize2", "ovrp_Initialize3",
            "ovrp_Initialize4", "ovrp_Initialize5", "ovrp_Shutdown",
            "ovrp_Update", "ovrp_Update2", "ovrp_Update3",
            "ovrp_WaitToBeginFrame", "ovrp_BeginFrame", "ovrp_BeginFrame4",
            "ovrp_EndFrame", "ovrp_EndFrame4", "ovrp_EndEye", "ovrp_EndEye2",
            "ovrp_GetNodePoseState", "ovrp_GetNodePoseState3", "ovrp_GetNodePose",
            "ovrp_GetNodePositionTracked", "ovrp_GetNodeOrientationTracked",
            "ovrp_GetNodePositionValid", "ovrp_GetNodeOrientationValid",
            "ovrp_GetNodePresent", "ovrp_GetAppHasVrFocus", "ovrp_GetAppShouldQuit",
            "ovrp_GetUserPresent", "ovrp_GetSystemHeadsetType",
            "ovrp_GetDisplayFrequency", "ovrp_SetDisplayFrequency",
            "ovrp_GetTrackingOriginType", "ovrp_SetTrackingOriginType",
            "ovrp_GetControllerState", "ovrp_GetControllerState2",
            "ovrp_GetControllerState3", "ovrp_GetControllerState4",
            "ovrp_GetEyeTextureSize", "ovrp_SetupEyeTexture", "ovrp_SetupEyeTexture2",
            "ovrp_GetSystemRecommendedMSAALevel", "ovrp_GetGPUUtilSupported",
            "ovrp_GetSystemDisplayFrequency", "ovrp_GetTimeInSeconds",
            "ovrp_GetAppFocused", "ovrp_GetAppHasInputFocus",
            "ovrp_GetTrackingCalibratedOrigin", "ovrp_RecenterTrackingOrigin",
            "ovrp_GetTrackingPoseEnabled", "ovrp_GetSystemVolume",
        };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            (void)kl_ovrp_sym(names[i]);
    }
    void *ovrp = kl_ovrp_sym("ovrp_GetNodePositionValid");
    if (ovrp) bench("ovrp_get", b_ovrp, ovrp);
    else      printf("[bench] %-18s SKIP (symbol not resolvable here)\n", "ovrp_get");

    // jni: FindClass + GetMethodID once, then CallStaticLongMethod in a loop —
    // the per-call path being measured is resolve_binding, not the lookup.
    void *env = kl_jni_env();
    void *cls = kl_jni_class("java/lang/System");
    void *mid = cls ? klj_want(cls, "nanoTime", "()J", 'M') : NULL;
    if (env && cls && mid) {
        void *ctx[3] = { env, cls, mid };
        bench("jni_call", b_jni, ctx);
    } else {
        printf("[bench] %-18s SKIP (JNI surface unavailable)\n", "jni_call");
    }

    // audio: needs a real output unit; skip cleanly when there is none.
    if (kl_audio_open(48000, 2, 16) == 0) {
        bench("audio_convert", b_audio, NULL);
        kl_audio_close();
    } else {
        printf("[bench] %-18s SKIP (no audio device)\n", "audio_convert");
    }

    printf("=== done (sink %llu) ===\n", (unsigned long long)g_sink);
    return 0;
}
