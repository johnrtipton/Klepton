// Include the actual index implementation so wrong slot identities can be
// checked directly, without relying on diagnostic counters or timing.
#include "../runtime/xr/kl_ovrp.c"
#include "../runtime/jni/kl_jni_int.h"
#include "../runtime/kl_jni.h"
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>

enum { THREADS = 8, NAMES = 512 };
static char names[NAMES][48];
static _Atomic int ready, start;
static void *mid, *env, *cls;
static void *worker(void *arg) {
    unsigned offset = (unsigned)(uintptr_t)arg;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    for (unsigned i = 0; i < 20000; i++) {
        const char *name = names[(i + offset) % NAMES];
        int slot = ovrp_slot(name);
        assert(slot >= 0 && slot < NAMES);
        assert(!strcmp(g_ovrp[slot].name, name));
        assert(((klj_wanted *)mid)->b != NULL);
        assert(klj_call(env, cls, mid, NULL, 'J').j != 0);
    }
    return NULL;
}

int klb_epoll_wait(int, void *, int, int);
int klb_epoll_ctl(int, int, int, void *);
typedef struct { uint32_t events, pad; uint64_t data; } ep_event;
static void test_epoll(void) {
    int q = kqueue(), fd[2];
    assert(q >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    ep_event interest = { .events = 1, .data = 0x12345678 };
    assert(klb_epoll_ctl(q, 1, fd[0], &interest) == 0);
    ep_event out[65];
    int sizes[] = { 1, 64, 65 };
    for (unsigned i = 0; i < 3; i++) {
        assert(klb_epoll_wait(q, out, sizes[i], 0) == 0);
        assert(write(fd[1], "x", 1) == 1);
        assert(klb_epoll_wait(q, out, sizes[i], 0) == 1);
        assert(out[0].data == interest.data && (out[0].events & 1));
        char c; assert(read(fd[0], &c, 1) == 1 && c == 'x');
    }
    errno = 0; assert(klb_epoll_wait(q, out, 0, 0) == -1 && errno == EINVAL);
    close(q); close(fd[0]); close(fd[1]);
    errno = 0; assert(klb_epoll_wait(q, out, 64, 0) == -1 && errno == EBADF);
    errno = 0; assert(klb_epoll_wait(q, out, 65, 0) == -1 && errno == EBADF);
}

int main(void) {
    setenv("KL_OVRP_VERBOSE", "0", 1);
    env = kl_jni_env(); cls = kl_jni_class("java/lang/System");
    mid = klj_want(cls, "nanoTime", "()J", 'M');
    assert(mid && ((klj_wanted *)mid)->b);
    klj_wanted *absent = klj_want(cls, "absentPerfTest", "()J", 'M');
    assert(absent && !absent->b);
    for (unsigned i = 0; i < NAMES; i++) snprintf(names[i], sizeof names[i], "perf_index_%u", i);
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    while (atomic_load(&ready) != THREADS) sched_yield();
    atomic_store(&start, 1);
    for (unsigned i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
    assert(g_novrp == NAMES && ovrp_slot("full_table") == -1);
    test_epoll();
    puts("PASS: concurrent OVR index/JNI dispatch; epoll stack/heap boundaries and errors");
    return 0;
}
