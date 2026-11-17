// SPDX-License-Identifier: MIT
// Life cycle of guest TLS blocks with the real OS layer (stage 4c): many guest threads created, run and destroyed; threads that never start;
// a fiber that outlives the thread that last ran it; a destroy hook replayed with stale values. Hooks are the ones of runtime/os/guest_tls_block.cpp,
// called by os_kernel.cpp host_entry through OsConfig::tp_init_var / tp_destroy_var; guest functions are host functions (as in os_test.cpp).
//
// Facts about how a guest thread ends, read from the sources and relied on here: the only way out of host_entry is the thread's entry returning
// (there is no nn::os::ExitThread override and no handled svcExitThread); then, in this order, the TLS destructors, the destroy hook, the fiber
// bookkeeping, the thread state. A thread that was created and never started never reaches host_entry, so it never has a block.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "os/guest_tls_block.h"
#include "os/os.h"
#include "os/os_internal.h"
#include "os/os_layout.h"

using namespace oslay;

extern "C" void rt_dispatch(Cpu *c) { ((GuestFn)(uintptr_t)c->pc)(c); }

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(c) do { g_checks++; if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#define S_CREATE "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"
#define S_START "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define S_WAITT "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"
#define S_DESTROY "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"
#define S_CURT "_ZN2nn2os16GetCurrentThreadEv"
#define S_FINIT "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define S_FFINAL "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define S_FSWITCH "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"
#define S_TLSALLOC "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"
#define S_TLSSET "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm"

static GuestFn need(const char *s) {
    GuestFn f = os_override_find(s);
    if (!f) { fprintf(stderr, "missing override %s\n", s); exit(2); }
    return f;
}
template <class... A> static uint64_t call(Cpu *c, const char *sym, A... a) {
    uint64_t v[] = {(uint64_t)a..., 0};
    for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = v[i];
    need(sym)(c);
    return c->x[0];
}
static uint64_t obj(size_t n) {
    void *p = nullptr;
    if (posix_memalign(&p, 64, n)) abort();
    memset(p, 0, n);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t gfn(void (*f)(Cpu *)) { return (uint64_t)(uintptr_t)f; }

struct GThread { uint64_t tt; };
static GThread make(Cpu *c, void (*f)(Cpu *), uint64_t arg, int core = -1) {
    GThread t{obj(TT_SIZE)};
    CHECK(call(c, S_CREATE, t.tt, gfn(f), arg, obj(0x4000), 0x4000ull, 16) == 0);
    (void)core;
    return t;
}
static void join(Cpu *c, GThread t) { call(c, S_WAITT, t.tt); call(c, S_DESTROY, t.tt); }
static uint64_t g_cell_init, g_cell_destroy;

// ---- hooks with a record of every call ---------------------------------------------------------------------------------------------------------
static std::mutex g_mu;
static std::map<uint64_t, uint64_t> g_tp_of;       // ThreadType -> its tp, as the init hook left it
static std::atomic<int> g_inits{0}, g_destroys{0}, g_tpidr_left{0};
static void guest_tp_init(Cpu *c) {
    osk::guest_tls_hook_init(c);
    std::lock_guard<std::mutex> lk(g_mu);
    g_tp_of[c->x[0]] = osk::gld<uint64_t>(c->tpidrro + TLR_TP);
    g_inits++;
}
static void guest_tp_destroy(Cpu *c) {
    const uint64_t before = c->tpidr;
    osk::guest_tls_hook_destroy(c);
    if (before && c->tpidr != 0) g_tpidr_left++;   // the Cpu kept a pointer into the released block
    g_destroys++;
}
static bool mapped(uint64_t tp) {
    uint64_t end;
    char d[64];
    return !osk::host::mem_find_free((const void *)(uintptr_t)(tp - hle::kGuestTlsTpOffset), &end, d, sizeof d);
}
static int count_maps() {   // all mappings of the process; loose evidence only (neighbouring anonymous mappings merge)
    FILE *f = fopen("/proc/self/maps", "r");
    int n = 0;
    char l[512];
    while (f && fgets(l, sizeof l, f)) n++;
    if (f) fclose(f);
    return n;
}
// exact leak evidence: every reservation and release a block makes goes through the module's memory seam, counted here
static std::atomic<long> g_reserves{0}, g_releases{0};
static void *c_reserve(size_t size, size_t align, bool ww, osk::host::MemErr *err) {
    void *p = osk::host::mem_reserve(size, align, ww, err);
    if (p) g_reserves++;
    return p;
}
static bool c_commit(void *a, size_t n, osk::host::MemErr *e) { return osk::host::mem_commit(a, n, e); }
static void c_release(void *a, size_t n) { g_releases++; osk::host::mem_release(a, n); }

static const uint8_t kImage[8] = {0xff, 0xff, 0xff, 0xff, 0x95, 0x23, 0x73, 0x00};
static std::atomic<int> g_bad{0}, g_ran{0};
static uint32_t g_slot;
static std::atomic<int> g_dtor_bad{0};
static void tls_dtor(Cpu *c) {   // at thread exit, before the destroy hook: the block must be live and still hold this thread's word
    const uint64_t tp = c->tpidr;
    if (!tp || !osk::guest_tls_block_owner(tp) || osk::gld<uint64_t>(tp + 0x10) != (uint64_t)c->x[0]) g_dtor_bad++;
}
static void th_work(Cpu *c) {
    const uint64_t tp = c->tpidr;
    if (!tp || memcmp((const void *)(uintptr_t)(tp + 0x10), kImage, 8) != 0) g_bad++;
    // the word the destructor will look for, and a few more of the thread's own
    osk::gst<uint64_t>(tp + 0x10, 0x5000 + (c->x[0] & 0xff));
    call(c, S_TLSSET, (uint64_t)g_slot, 0x5000 + (c->x[0] & 0xff));
    g_ran++;
}

// ---- 1. many threads, created and destroyed in waves ---------------------------------------------------------------------------------------------
static void test_cycles(Cpu *c) {
    const size_t live0 = osk::guest_tls_blocks_live();
    uint64_t slot = obj(16);
    CHECK(call(c, S_TLSALLOC, slot, gfn(tls_dtor)) == 0);
    g_slot = *(uint32_t *)(uintptr_t)slot;
    g_tp_of.clear();
    g_inits = g_destroys = g_bad = g_ran = g_tpidr_left = g_dtor_bad = 0;
    g_reserves = g_releases = 0;
    const int maps0 = count_maps();
    int made = 0;
    for (int wave = 0; wave < 40; wave++) {
        std::vector<GThread> ts;
        for (int i = 0; i < 16; i++) { ts.push_back(make(c, th_work, (uint64_t)made++)); }
        for (auto &t : ts) call(c, S_START, t.tt);
        for (auto &t : ts) join(c, t);
        CHECK(osk::guest_tls_blocks_live() == live0);                       // every wave leaves nothing behind
    }
    CHECK(g_ran.load() == made && g_inits.load() == made && g_destroys.load() == made);   // one init and one destroy per thread
    CHECK(g_bad.load() == 0 && g_dtor_bad.load() == 0 && g_tpidr_left.load() == 0);
    for (auto &kv : g_tp_of) CHECK(osk::guest_tls_block_owner(kv.second) == 0);          // none of the blocks is still registered under its old owner
    CHECK(g_reserves.load() == made && g_releases.load() == made);          // 640 reservations made, 640 released: none left behind, none released twice
    CHECK(count_maps() <= maps0 + 64);                                      // loose: the process did not accumulate mappings (the test's own guest stacks are malloc'd)
}

// ---- 2. threads that never start never have a block --------------------------------------------------------------------------------------------
static void th_never(Cpu *) { g_bad++; }
static void test_never_started(Cpu *c) {
    const size_t live0 = osk::guest_tls_blocks_live();
    const int inits0 = g_inits.load(), destroys0 = g_destroys.load();
    std::vector<GThread> ts;
    for (int i = 0; i < 8; i++) ts.push_back(make(c, th_never, 0));
    for (auto &t : ts) call(c, S_DESTROY, t.tt);
    CHECK(g_inits.load() == inits0 && g_destroys.load() == destroys0);       // neither hook ran
    CHECK(osk::guest_tls_blocks_live() == live0);
    CHECK(g_bad.load() == 0);
}

// ---- 3. a fiber outlives the thread that last ran it -------------------------------------------------------------------------------------------
static uint64_t g_fiber;
static std::atomic<uint64_t> g_fib_tp{0}, g_fib_tpro{0}, g_fib_word{0};
static std::atomic<int> g_fib_runs{0};
static void fiber_main(Cpu *c) {
    for (;;) {
        g_fib_tp = c->tpidr;
        g_fib_tpro = c->tpidrro;
        g_fib_word = osk::gld<uint64_t>(c->tpidr + 0x18);   // read through the thread pointer the fiber was given this time
        g_fib_runs++;
        call(c, S_FSWITCH, 0ull);
    }
}
static std::atomic<uint64_t> g_th_tp[2], g_th_tt[2];
static void th_fiber_user(Cpu *c) {
    const int i = (int)c->x[0];
    g_th_tp[i] = c->tpidr;
    g_th_tt[i] = call(c, S_CURT);
    osk::gst<uint64_t>(c->tpidr + 0x18, 0x7000 + (uint64_t)i);
    call(c, S_FSWITCH, g_fiber);
    CHECK(c->tpidr == g_th_tp[i].load());
}
static void test_fiber_outlives_thread(Cpu *c) {
    g_fiber = obj(FB_SIZE);
    g_fib_runs = 0;
    call(c, S_FINIT, g_fiber, gfn(fiber_main), 0ull, obj(0x4000), 0x4000ull, 0ull);
    GThread a = make(c, th_fiber_user, 0);
    call(c, S_START, a.tt);
    join(c, a);                                                              // thread A is gone, and so is its block
    const uint64_t a_tp = g_th_tp[0].load();
    CHECK(g_fib_runs.load() == 1 && g_fib_tp.load() == a_tp && g_fib_word.load() == 0x7000);
    CHECK(osk::guest_tls_block_owner(a_tp) != g_th_tt[0].load());            // not registered under A any more (the address may be reused by another thread)
    GThread b = make(c, th_fiber_user, 1);
    call(c, S_START, b.tt);
    join(c, b);
    CHECK(g_fib_runs.load() == 2);
    CHECK(g_fib_tp.load() == g_th_tp[1].load());                             // the parked fiber got B's thread pointer, not A's
    CHECK(g_fib_tpro.load() != 0);
    CHECK(g_fib_word.load() == 0x7001);                                      // and read B's own word through it
    call(c, S_FFINAL, g_fiber);
}

// ---- 4. a destroy hook replayed with the values of a thread that is gone ------------------------------------------------------------------------
static void th_remember(Cpu *c) { g_th_tp[0] = c->tpidr; g_th_tt[0] = call(c, S_CURT); }
static void test_stale_destroy(Cpu *c) {
    const size_t live0 = osk::guest_tls_blocks_live();
    GThread a = make(c, th_remember, 0);
    call(c, S_START, a.tt);
    join(c, a);
    const uint64_t old_tp = g_th_tp[0].load(), old_tt = g_th_tt[0].load();
    CHECK(osk::guest_tls_blocks_live() == live0);
    // a live block of someone else, then the stale call: the old ThreadType with the old tp
    uint64_t other = osk::guest_tls_block_create(nullptr, 0xBEEF);
    CHECK(other != 0);
    alignas(512) static uint8_t tlr[0x200];
    memset(tlr, 0, sizeof tlr);
    osk::gst<uint64_t>((uint64_t)(uintptr_t)tlr + TLR_TP, old_tp);
    Cpu fake;
    memset(&fake, 0, sizeof fake);
    fake.tpidrro = (uint64_t)(uintptr_t)tlr;
    fake.tpidr = old_tp;
    fake.x[0] = old_tt;
    osk::guest_tls_hook_destroy(&fake);                                      // nothing registered under (old_tp, old_tt): refused
    CHECK(osk::guest_tls_blocks_live() == live0 + 1 && mapped(other));       // the live block is untouched
    CHECK(osk::guest_tls_block_destroy(other, 0xBEEF));
    CHECK(osk::guest_tls_blocks_live() == live0);
}

int main() {
    std::vector<uint8_t> module(0xccb900 + 0x1000, 0);
    memcpy(module.data() + hle::kMainTlsImageVaddr, kImage, 8);
    CHECK(osk::guest_tls_main_image(module.data(), module.size()) != nullptr);
    g_cell_init = obj(8);
    g_cell_destroy = obj(8);
    *(uint64_t *)(uintptr_t)g_cell_init = gfn(guest_tp_init);
    *(uint64_t *)(uintptr_t)g_cell_destroy = gfn(guest_tp_destroy);
    OsConfig cfg{};
    cfg.tp_init_var = g_cell_init;
    cfg.tp_destroy_var = g_cell_destroy;
    const osk::GuestTlsMemOps ops = {c_reserve, c_commit, c_release};
    osk::guest_tls_set_mem_ops(&ops);
    os_init(&cfg);
    Cpu *c = (Cpu *)obj(sizeof(Cpu));
    os_adopt_main_thread(c, 0, -1);
    test_cycles(c);
    test_never_started(c);
    test_fiber_outlives_thread(c);
    test_stale_destroy(c);
    if (g_fail.load()) { std::fprintf(stderr, "guest_tls_lifecycle_test: %d of %d checks failed\n", g_fail.load(), g_checks.load()); return 1; }
    std::printf("guest_tls_lifecycle_test: ok (%d checks)\n", g_checks.load());
    return 0;
}
