// Test the production Vulkan image registry without constructing a GPU device.
// Differential oracle is the pre-index registry, including duplicates/full mode.
#include <assert.h>
#include <time.h>
#include <stdatomic.h>
#include "../runtime/gfx/kl_vulkan.c"
#if !__has_include(<vulkan/vulkan.h>)
#error "t_vk_image_reg needs the MoltenVK Vulkan headers (make mvk)"
#else

static VkImage legacy[KLVK_IMG_REG_MAX];
static int legacy_n;
static VkImage handle(uint64_t x) { return (VkImage)(uintptr_t)x; }
static void reference_add(VkImage img) {
    if (img && legacy_n < KLVK_IMG_REG_MAX) legacy[legacy_n++] = img;
}
static void reference_del(VkImage img) {
    if (!img) return;
    for (int i = 0; i < legacy_n; i++) if (legacy[i] == img) {
        legacy[i] = legacy[--legacy_n]; break;
    }
}
static int reference_has(VkImage img) {
    if (legacy_n >= KLVK_IMG_REG_MAX) return 1;
    for (int i = 0; i < legacy_n; i++) if (legacy[i] == img) return 1;
    return 0;
}
static void add(VkImage img) { klvk_img_reg_add(img); reference_add(img); assert(g_img_reg_n == legacy_n); }
static void del(VkImage img) { klvk_img_reg_del(img); reference_del(img); assert(g_img_reg_n == legacy_n); }
static void check(VkImage img) { assert(klvk_img_reg_has(img) == reference_has(img)); }
static void empty(void) { while (legacy_n) del(legacy[0]); assert(g_img_reg_n == 0); }
static uint64_t rnd = 123456789;
static uint64_t random64(void) { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; return rnd; }
static void differential(void) {
    add(VK_NULL_HANDLE); del(VK_NULL_HANDLE); check(VK_NULL_HANDLE);
    for (int i = 0; i < 50000; i++) {
        VkImage img = handle((1 + random64() % 4096) << 8);
        if (random64() % 3) add(img); else del(img);
        check(img); check(handle((1 + random64() % 4096) << 8));
    }
    empty();
    // Collision chain wraps across the table boundary; deletion must not hide
    // remaining members. Hash is already used elsewhere in kl_vulkan.c.
    VkImage colliding[32]; unsigned n = 0;
    for (uint64_t x = 1; n < 32; x++)
        if ((klvk_hash64(x) & 32767) == 32767) colliding[n++] = handle(x);
    for (unsigned i = 0; i < n; i++) add(colliding[i]);
    for (unsigned i = 0; i < n; i += 2) del(colliding[i]);
    for (unsigned i = 0; i < n; i++) check(colliding[i]);
    for (unsigned i = 0; i < n; i += 2) add(colliding[i]);
    for (unsigned i = 0; i < n; i++) check(colliding[i]);
    empty();
    // Match the old capacity/fail-open/recovery policy, including ignored adds.
    for (int i = 1; i <= KLVK_IMG_REG_MAX; i++) add(handle((uint64_t)i << 8));
    VkImage outside = handle(UINT64_C(0xfedcba9876543200));
    add(outside); check(outside); check(VK_NULL_HANDLE);
    del(handle(256)); check(outside); check(handle(256));
    empty();
    // Handle reuse and duplicate registration count exactly as before.
    for (int i = 0; i < 10000; i++) {
        add(handle(0xabcdef)); add(handle(0xabcdef)); del(handle(0xabcdef));
        check(handle(0xabcdef)); del(handle(0xabcdef)); check(handle(0xabcdef));
    }
}
static _Atomic int ready, start;
static void *worker(void *ctx) {
    VkImage img = handle(0x10000000 + ((uintptr_t)ctx % 2) * 256);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    for (int i = 0; i < 20000; i++) {
        klvk_img_reg_add(img); assert(klvk_img_reg_has(img)); klvk_img_reg_del(img);
    }
    return NULL;
}
static void concurrent(void) {
    pthread_t t[8];
    for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(&t[i], NULL, worker, (void *)i));
    while (atomic_load(&ready) < 8) sched_yield(); atomic_store(&start, 1);
    for (int i = 0; i < 8; i++) assert(!pthread_join(t[i], NULL));
    assert(g_img_reg_n == 0);
}
static uint64_t now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void bench(int n, int miss) {
    for (int i = 1; i <= n; i++) add(handle((uint64_t)i << 8));
    VkImage img = handle((uint64_t)(miss ? n + 1 : n) << 8);
    uint64_t best = UINT64_MAX; volatile unsigned sink = 0;
    for (int r = 0; r < 7; r++) {
        uint64_t a = now();
        for (int i = 0; i < 100000; i++) sink += klvk_img_reg_has(img);
        uint64_t dt = now() - a; if (dt < best) best = dt;
    }
    printf("[registry bench] n=%d %s %.2f ns/op sink=%u\n", n, miss ? "miss" : "hit", (double)best / 100000, sink);
    empty();
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        bench(2000, 0); bench(2000, 1); bench(12000, 0); bench(12000, 1); return 0;
    }
    differential(); concurrent();
    puts("PASS: image registry differential, collision/deletion/reuse/full-mode and eight-thread checks");
    return 0;
}

#endif // Vulkan headers
