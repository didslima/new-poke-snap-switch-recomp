// SPDX-License-Identifier: MIT
// Host and guest TLS across fibers that change host threads (stage 4c). The real loader dispatch (rt_dispatch, thunks, g_tls_cpu), the real OS layer
// (threads, fibers, scheduler) and the System V context switch run together; the guest "code" is host functions registered as loader thunks, so every
// call between them goes through loader::rt_dispatch exactly as lifted code reaches an override.
//
// What it pins:
//   * the context switch never touches the host's thread-local storage: after every resume the fiber sees the %fs base, the TCB canary
//     (%fs:0x28), pthread_self(), the gettid() and a thread_local value of the host thread that resumed it, not of the one that ran it before;
//   * osk::self_fresh() follows the fiber to its new host thread, while a thread_local address taken before the switch does not (the hazard,
//     shown to be real: the cached read differs from the fresh one);
//   * guest side: the fiber runs with the resuming thread's tpidr / TLR and reads that thread's own TLS words; its own Cpu is never another's;
//   * loader::g_tls_cpu is host-thread state, "the Cpu of the last host call on this thread": right after a resume it still names the resuming
//     thread's Cpu (the call that switched to the fiber was made by that thread; only the very first run is a dispatch on the fiber's own Cpu),
//     and the fiber's next host call sets it to the fiber's Cpu. g_cur, the GuestThread new_thread made, belongs to the host thread that called new_thread and only feeds guest_chain's stack check.
// SNAP_HOPS=<n> sets the number of hops (default 600).
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "guest_tls_hooks.h"
#include "loader.h"
#include "os/guest_tls_block.h"
#include "os/os.h"
#include "os/os_internal.h"
#include "os/os_layout.h"

using namespace oslay;

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(c) do { g_checks++; if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#define S_CREATE "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"
#define S_START "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define S_WAITT "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"
#define S_DESTROY "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"
#define S_CURT "_ZN2nn2os16GetCurrentThreadEv"
#define S_SLEEP "_ZN2nn2os11SleepThreadENS_8TimeSpanE"
#define S_FINIT "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define S_FFINAL "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define S_FSWITCH "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"

static GuestFn need(const char *s) {
    GuestFn f = os_override_find(s);
    if (!f) { fprintf(stderr, "missing override %s\n", s); exit(2); }
    return f;
}
// a direct call of an override (the way os_test.cpp calls them)
template <class... A> static uint64_t call(Cpu *c, const char *sym, A... a) {
    uint64_t v[] = {(uint64_t)a..., 0};
    for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = v[i];
    need(sym)(c);
    return c->x[0];
}
// a call through the loader's dispatch, as lifted code reaches an override: sets loader::g_tls_cpu like every host call from guest code
template <class... A> static uint64_t callt(Cpu *c, uint64_t thunk, A... a) {
    uint64_t v[] = {(uint64_t)a..., 0};
    for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = v[i];
    c->x[30] = HOST_SENTINEL;
    c->pc = thunk;
    rt_dispatch(c);
    return c->x[0];
}
static uint64_t obj(size_t n) {
    void *p = nullptr;
    if (posix_memalign(&p, 64, n)) abort();
    memset(p, 0, n);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t reg(const char *name, GuestFn fn) { return loader::add_override(name, fn)->addr; }

// ---- the host's own thread state, read the way a program would -----------------------------------------------------------------------
static uint64_t fs_base() {   // a read of the kernel's value: this test never sets it
    uint64_t v = 0;
    syscall(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &v);
    return v;
}
static uint64_t canary() {
    uint64_t v;
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
    return v;
}
static thread_local uint64_t t_val = 0;       // each host thread sets its own
static thread_local uint64_t t_counter = 0;
// pthread_self() is declared __attribute__((const)), so the compiler may call it once and reuse the value across a context switch: the same hazard
// as a cached thread_local address, found here in the test itself. Asked through a function the compiler cannot see through, it is always fresh.
__attribute__((noinline)) static uint64_t self_id() { __asm__ volatile("" ::: "memory"); return (uint64_t)pthread_self(); }
__attribute__((noinline)) static uint64_t tl_get() { __asm__ volatile("" ::: "memory"); return t_val; }
__attribute__((noinline)) static volatile uint64_t *tl_addr() { __asm__ volatile("" ::: "memory"); return &t_val; }
__attribute__((noinline)) static void tl_bump() { __asm__ volatile("" ::: "memory"); t_counter++; }
__attribute__((noinline)) static uint64_t tl_counter() { __asm__ volatile("" ::: "memory"); return t_counter; }

struct Expect {   // published by the host thread about to resume the fiber
    uint64_t pself, fs, canary, tid, tval, tt, tp, tlr, thread_cpu, word;
};
static Expect g_expect;
static std::atomic<uint64_t> g_mismatch{0}, g_cached_differs{0}, g_resumes{0};
static std::atomic<uint64_t> g_tlscpu_stale_ok{0}, g_tlscpu_after_call_ok{0}, g_resume_checks{0};
static uint64_t g_thunk_switch, g_thunk_curt, g_fiber;

#define MISMATCH(cond) do { g_resume_checks++; if (!(cond)) { if (g_mismatch++ < 8) std::fprintf(stderr, "mismatch %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// the fiber: runs on whichever host thread resumed it
static void fiber_main(Cpu *c) {
    for (;;) {
        const Expect e = g_expect;
        g_resumes++;
        // what the host sees from this fiber, which now runs on the resuming thread
        MISMATCH(self_id() == e.pself);
        MISMATCH(fs_base() == e.fs);
        MISMATCH(canary() == e.canary);
        MISMATCH((uint64_t)syscall(SYS_gettid) == e.tid);
        MISMATCH(tl_get() == e.tval);
        osk::Thread *t = osk::self_fresh();
        MISMATCH(t && t->tt == e.tt && t->host_tid == e.tid);
        // what the guest sees: the resuming thread's pointers and TLS words, and a Cpu of its own
        MISMATCH(c->tpidr == e.tp && c->tpidrro == e.tlr);
        MISMATCH(c != (Cpu *)(uintptr_t)e.thread_cpu);
        MISMATCH(osk::gld<uint64_t>(c->tpidr + 0x20) == e.word);
        // loader::g_tls_cpu: host-thread state. The call that switched to this fiber was made on the resuming thread with its own Cpu
        const bool stale = loader::g_tls_cpu == (Cpu *)(uintptr_t)e.thread_cpu;
        g_tlscpu_stale_ok += stale;
        callt(c, g_thunk_curt);                                   // the fiber's own host call
        g_tlscpu_after_call_ok += loader::g_tls_cpu == c;
        // a thread_local address taken now and one asked for again after the next resume
        volatile uint64_t *cached = tl_addr();
        tl_bump();
        const uint64_t before = *cached;
        MISMATCH(before == e.tval);
        osk::gst<uint64_t>(c->tpidr + 0x28, before);              // a guest word of the thread's TLS, written from the fiber
        callt(c, g_thunk_switch, 0ull);                           // back to the thread; later resumed, maybe by another thread
        const uint64_t via_cached = *cached, fresh = tl_get();
        if (via_cached != fresh) g_cached_differs++;              // the cached address belongs to the previous host thread: the hazard is real
        const Expect e2 = g_expect;
        MISMATCH(fresh == e2.tval);                               // a fresh access is always right
    }
}

// ---- the guest threads that resume the fiber -----------------------------------------------------------------------------------------------
static const int NT = 3;
static int g_hops = 600;
static std::atomic<int> g_turn{0}, g_ready{0};
static std::atomic<uint64_t> g_counter_sum{0};
static uint64_t g_thread_cpu[NT];

static void thread_main(Cpu *c) {
    const int idx = (int)c->x[0];
    t_val = 0x1000 + (uint64_t)idx;
    g_thread_cpu[idx] = (uint64_t)(uintptr_t)c;
    // a word of this thread's own TLS, at the same offset in every thread: a fiber must read the resuming thread's
    osk::gst<uint64_t>(c->tpidr + 0x20, 0xA000 + (uint64_t)idx);
    g_ready++;
    while (g_ready.load() < NT) callt(c, g_thunk_curt);   // all threads alive before the first hop
    const uint64_t tt = callt(c, g_thunk_curt);
    for (;;) {
        while (g_turn.load() % NT != idx && g_turn.load() < g_hops) std::this_thread::yield();
        if (g_turn.load() >= g_hops) break;
        g_expect = Expect{self_id(), fs_base(), canary(), (uint64_t)syscall(SYS_gettid), tl_get(), tt, c->tpidr, c->tpidrro,
                          (uint64_t)(uintptr_t)c, 0xA000 + (uint64_t)idx};
        callt(c, g_thunk_switch, g_fiber);                    // runs the fiber on this host thread until it switches back
        // the thread's own context after the fiber: its host state is its own
        MISMATCH(self_id() == g_expect.pself && fs_base() == g_expect.fs && canary() == g_expect.canary);
        MISMATCH(tl_get() == 0x1000 + (uint64_t)idx);
        MISMATCH(c->tpidr == g_expect.tp && osk::gld<uint64_t>(c->tpidr + 0x28) == g_expect.tval);   // the word the fiber wrote is in OUR block
        g_turn++;
    }
    g_counter_sum += tl_counter();
    CHECK(tl_counter() > 0 || g_hops < NT * 4);               // every thread ran the fiber a share of the time
}

static void test_fiber_hops(Cpu *c) {
    g_turn = 0;
    g_ready = 0;
    g_fiber = obj(FB_SIZE);
    call(c, S_FINIT, g_fiber, reg("test.fiber_main", fiber_main), 0ull, obj(0x4000), 0x4000ull, 0ull);
    std::vector<uint64_t> tts;
    const uint64_t th_fn = reg("test.thread_main", thread_main);
    for (int i = 0; i < NT; i++) {
        const uint64_t tt = obj(TT_SIZE);
        CHECK(call(c, S_CREATE, tt, th_fn, (uint64_t)i, obj(0x4000), 0x4000ull, 16, i) == 0);
        tts.push_back(tt);
    }
    for (uint64_t tt : tts) call(c, S_START, tt);
    for (uint64_t tt : tts) { call(c, S_WAITT, tt); call(c, S_DESTROY, tt); }
    call(c, S_FFINAL, g_fiber);

    CHECK(g_resumes.load() == (uint64_t)g_hops);
    CHECK(g_mismatch.load() == 0);
    CHECK(g_resume_checks.load() >= (uint64_t)g_hops * 12);
    CHECK(g_cached_differs.load() > 0);                           // fibers really changed host threads, and a cached TLS address went stale
    CHECK(g_counter_sum.load() == (uint64_t)g_hops);              // every bump landed in the counter of the thread that ran it
    CHECK(g_tlscpu_stale_ok.load() == (uint64_t)g_hops - 1);      // right after a resume g_tls_cpu still names the resumer's Cpu (not the first one:
                                                                  // starting the fiber is itself a dispatch on the fiber's Cpu)
    CHECK(g_tlscpu_after_call_ok.load() == (uint64_t)g_hops);     // and the fiber's own host call sets it to the fiber's Cpu
    std::fprintf(stderr, "  %d hops: %llu cached-address reads went stale, %llu resume checks, 0 mismatches expected (%llu)\n", g_hops,
                 (unsigned long long)g_cached_differs.load(), (unsigned long long)g_resume_checks.load(), (unsigned long long)g_mismatch.load());
}

// ---- g_cur: the GuestThread new_thread made, per host thread, and what it feeds ---------------------------------------------------------------
// guest_chain walks frame records and asks fp_ok whether a frame pointer is plausible: with g_cur set (this host thread called new_thread) the
// frame must lie in that GuestThread's stack, otherwise within 64 MB above the Cpu's sp.
static void test_g_cur() {
    static uint64_t frames[16];
    frames[0] = (uint64_t)(uintptr_t)&frames[4];   // [fp] = next fp, [fp + 8] = return address
    frames[1] = 0x7100001000ull;
    frames[4] = 0;
    frames[5] = 0;                                  // end of the chain
    Cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.sp = (uint64_t)(uintptr_t)frames - 0x100;
    cpu.x[29] = (uint64_t)(uintptr_t)&frames[0];
    cpu.x[30] = 0x7100002000ull;
    const size_t here = loader::guest_chain(&cpu, 0x7100003000ull, 8).size();   // this host thread called new_thread: g_cur is set
    size_t other = 0;
    std::thread t([&] { other = loader::guest_chain(&cpu, 0x7100003000ull, 8).size(); });   // another host thread: g_cur is null
    t.join();
    CHECK(here == 2);                                                           // pc and lr only: the frame is not in new_thread's stack
    CHECK(other > 2);                                                           // the same Cpu on another host thread: the frame is plausible
}

// g_tls_cpu belongs to the host thread: a call through the dispatch on one thread is invisible on another
static void test_g_tls_cpu_per_thread(Cpu *c) {
    callt(c, g_thunk_curt);
    CHECK(loader::g_tls_cpu == c);
    Cpu other;
    memset(&other, 0, sizeof other);
    bool fresh_null = false, own = false;
    std::thread t([&] {
        fresh_null = loader::g_tls_cpu == nullptr;              // this host thread has made no host call yet
        callt(&other, g_thunk_curt);
        own = loader::g_tls_cpu == &other;
    });
    t.join();
    CHECK(fresh_null && own);
    CHECK(loader::g_tls_cpu == c);                              // and the other thread's call did not touch this one's
}

int main() {
    if (const char *h = getenv("SNAP_HOPS")) g_hops = atoi(h) > 0 ? atoi(h) : g_hops;
    // a stand-in main with the real image, as in guest_tls_loader_test
    static std::vector<uint8_t> module(0xccb900 + 0x1000, 0);
    const uint8_t image[8] = {0xff, 0xff, 0xff, 0xff, 0xef, 0xbe, 0xad, 0xde};   // synthetic: slot word -1, then the hash word 0xdeadbeef
    memcpy(module.data() + hle::kMainTlsImageVaddr, image, 8);
    loader::g_main.base = (uint64_t)(uintptr_t)module.data();
    loader::g_main.span = module.size();

    // the order host/guest_tls_hooks.h documents: os_init, install the hooks, new_thread for the main thread, adopt it
    uint64_t cells = obj(16);
    OsConfig cfg{};
    cfg.tp_init_var = cells;
    cfg.tp_destroy_var = cells + 8;
    os_init(&cfg);
    CHECK(snap_host::install_guest_tls_hooks(cfg) == 2);
    loader::GuestThread *th = loader::new_thread();
    Cpu *c = th->c;
    os_adopt_main_thread(c, 0, -1);
    osk::gst<uint64_t>(c->tpidrro + TLR_TP, c->tpidr);          // host/boot.cpp: TLR+0x1f0 = the thread pointer new_thread made
    g_thunk_switch = reg("test.switch", need(S_FSWITCH));
    g_thunk_curt = reg("test.curthread", need(S_CURT));

    test_g_tls_cpu_per_thread(c);
    test_g_cur();
    test_fiber_hops(c);
    if (g_fail.load()) { std::fprintf(stderr, "tls_host_fibers_test: %d of %d checks failed\n", g_fail.load(), g_checks.load()); return 1; }
    std::printf("tls_host_fibers_test: ok (%d checks, %d hops)\n", g_checks.load(), g_hops);
    return 0;
}
