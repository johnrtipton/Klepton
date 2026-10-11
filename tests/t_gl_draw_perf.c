// Real draw wrappers with a controlled GL dispatch table. This tests forwarding
// and diagnostics independently of a driver; timings are wrapper CPU only.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned formats;
static int counted_snprintf(char *buf, size_t n, const char *fmt, ...) {
    formats++;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap); return r;
}
#define snprintf counted_snprintf
#include "../runtime/gfx/kl_glfb.c"
#undef snprintf

static unsigned queries, errors, draws;
static uint32_t pending_error;
static int32_t driver_fbo = 97;
static int generate_error;
static int check_args = 1;
static void get_integer(uint32_t p, int32_t *v) {
    queries++;
    if (p == 0x0BA2) { v[0] = v[1] = 0; v[2] = v[3] = 64; }
    else *v = p == 0x8CA6 ? driver_fbo : 0;
}
static uint32_t get_error(void) {
    errors++; uint32_t e = pending_error; pending_error = 0; return e;
}
static void elements(uint32_t mode, int32_t count, uint32_t type, const void *idx) {
    if (check_args) assert(mode == 4 && count == 36 && type == 0x1403 && idx == (void *)16);
    draws++;
    if (generate_error) pending_error = 0x502;
}
static void arrays(uint32_t mode, int32_t first, int32_t count) {
    if (check_args) assert(mode == 4 && first == 3 && count == 36);
    draws++;
}
static void range(uint32_t mode, uint32_t start, uint32_t end, int32_t count,
                  uint32_t type, const void *idx) {
    if (check_args) assert(start == 3 && end == 40);
    elements(mode, count, type, idx);
}
static void elements_inst(uint32_t mode, int32_t count, uint32_t type,
                          const void *idx, int32_t n) {
    if (check_args) assert(n == 2); elements(mode, count, type, idx);
}
static void arrays_inst(uint32_t mode, int32_t first, int32_t count, int32_t n) {
    if (check_args) assert(n == 2); arrays(mode, first, count);
}
static void call_elements(void) { klfb_DrawElements(4, 36, 0x1403, (void *)16); }
static void call_arrays(void) { klfb_DrawArrays(4, 3, 36); }
static void call_range(void) { klfb_DrawRangeElements(4, 3, 40, 36, 0x1403, (void *)16); }
static void call_elements_inst(void) { klfb_DrawElementsInstanced(4, 36, 0x1403, (void *)16, 2); }
static void call_base(void) { klfb_DrawElementsBaseVertex(4, 36, 0x1403, (void *)16, 0); }
static void call_range_base(void) { klfb_DrawRangeElementsBaseVertex(4, 3, 40, 36, 0x1403, (void *)16, 0); }
static void call_arrays_inst(void) { klfb_DrawArraysInstanced(4, 3, 36, 2); }
static uint64_t clock_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void bench(const char *name, void (*fn)(void)) {
    enum { N = 200000, ROUNDS = 7 };
    uint64_t best = UINT64_MAX;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t a = clock_ns();
        for (int i = 0; i < N; i++) fn();
        uint64_t dt = clock_ns() - a; if (dt < best) best = dt;
    }
    printf("[draw bench] %s %.2f ns/op\n", name, (double)best / N);
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "off";
    int census = !strcmp(mode, "census") || !strcmp(mode, "objects");
    int timeline = !strcmp(mode, "timeline");
    int probe = !strcmp(mode, "error");
    unsetenv("KL_FULL"); unsetenv("KL_VERBOSE_ALL");
    setenv("KL_GL_CENSUS", !strcmp(mode, "objects") ? "1" : "0", 1);
    setenv("KL_GLFB_DRAW_CENSUS", !strcmp(mode, "census") ? "1" : "0", 1);
    setenv("KL_GLFB_BLIT_PROBE", timeline ? "1" : "0", 1);
    setenv("KL_GLFB_ERRPROBE", probe ? "1" : "0", 1);
    setenv("KL_GLFB_DRAW_PROBE", "0", 1);
    a_glGetIntegerv = get_integer; a_glGetError = get_error;
    g_real_DrawElements = elements; g_real_DrawArrays = arrays;
    g_real_DrawRangeElements = range; g_real_DrawElementsInstanced = elements_inst;
    g_real_DrawArraysInstanced = arrays_inst;
    g_draw_fb = 11; // Live GL says 97: diagnostics must still query the driver.
    void (*calls[])(void) = { call_elements, call_arrays, call_range, call_elements_inst,
                             call_base, call_range_base, call_arrays_inst };
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++) calls[i]();
    assert(draws == 7);
    if (!strcmp(mode, "bench")) {
        check_args = 0; formats = queries = errors = draws = 0;
        bench("DrawElements", call_elements);
        bench("DrawArrays", call_arrays);
        bench("DrawElementsInstanced", call_elements_inst);
        printf("dispatch counts: draws=%u queries=%u formats=%u errors=%u\n",
               draws, queries, formats, errors);
        return 0;
    }
    assert(errors == (probe ? 12u : 0u));
    if (census || timeline) {
        assert(queries >= 7 && g_ndraw_fbs == 1);
        assert(g_draw_fbs[0].fb == 97 && g_draw_fbs[0].draws == 7);
        assert(g_last_draw_fb == 97);
        driver_fbo = -1; call_arrays();
        assert(g_last_draw_fb == 11 && g_ndraw_fbs == 2);
        assert(g_draw_fbs[1].fb == 11 && g_draw_fbs[1].draws == 1);
    } else {
        assert(queries == 0 && g_ndraw_fbs == 0);
    }
    if (probe) assert(formats >= 6);
    else assert(formats == 0);
    if (!probe) {
        pending_error = 0x502; call_elements();
        assert(pending_error == 0x502); // Disabled probes must not consume guest errors.
    } else {
        pending_error = 0x502; call_elements();
        assert(pending_error == 0 && errors == 14);
        generate_error = 1; call_elements();
        assert(pending_error == 0 && errors == 16);
    }
    printf("PASS: draw forwarding and %s diagnostics\n", mode);
    return 0;
}
