// SPDX-License-Identifier: MIT
// The guest TLS hooks with the real OS layer (stage 4b): guest threads are created, started, switched, finished and destroyed through the nn::os
// overrides, and the hooks of runtime/os/guest_tls_block.cpp run where sdk's thread trampoline calls them (OsConfig::tp_init_var and
// tp_destroy_var, os_kernel.cpp host_entry). As in tests/os/os_test.cpp, a guest "function" is a host function taking Cpu*, and every nn::os
// call goes through os_override_find. What this cannot show, because the Linux runner does not exist yet: the hooks running as loader thunks
// (guest_tls_loader_test does that part) and any real game code reading tpidr_el0.
#include <atomic>
#include <chrono>
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
#define S_CREATE7 "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"
#define S_START "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define S_WAITT "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"
#define S_DESTROY "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"
#define S_CURT "_ZN2nn2os16GetCurrentThreadEv"
#define S_SLEEP "_ZN2nn2os11SleepThreadENS_8TimeSpanE"
#define S_FINIT "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define S_FFINAL "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define S_FSWITCH "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"
#define S_TLSALLOC "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"
#define S_TLSGET "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE"
#define S_TLSSET "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm"
static constexpr int64_t MS = 1000000;

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
static uint64_t obj(size_t n = 0x200) {
    void *p = nullptr;
    if (posix_memalign(&p, 64, n)) abort();
    memset(p, 0, n);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t gfn(void (*f)(Cpu *)) { return (uint64_t)(uintptr_t)f; }

struct GThread { uint64_t tt; };
static GThread spawn(Cpu *c, void (*f)(Cpu *), uint64_t arg, int core = -1) {
    GThread t{obj(TT_SIZE)};
    uint64_t stack = obj(0x4000);
    uint64_t r = core < 0 ? call(c, S_CREATE, t.tt, gfn(f), arg, stack, 0x4000ull, 16)
                          : call(c, S_CREATE7, t.tt, gfn(f), arg, stack, 0x4000ull, 16, core);
    CHECK(r == 0);
    call(c, S_START, t.tt);
    return t;
}
static void join(Cpu *c, GThread t) { call(c, S_WAITT, t.tt); call(c, S_DESTROY, t.tt); }

static const uint8_t kImage[8] = {0xff, 0xff, 0xff, 0xff, 0x95, 0x23, 0x73, 0x00};
static bool mapped(uint64_t tp) {
    uint64_t end;
    char d[64];
    return !osk::host::mem_find_free((const void *)(uintptr_t)(tp - hle::kGuestTlsTpOffset), &end, d, sizeof d);
}

// ---- the guest side of the two hooks: what sdk's trampoline would call, with a record of what the hook saw and did ----------------------------
struct Rec {
    uint64_t tlr = 0, tp_before = ~0ull, tpidr_before = ~0ull, thread_word_before = 0, tp_after = 0, tpidr_after = ~0ull;
    uint64_t entry_tpidr = 0, entry_tlr_tp = 0, entry_tp_in_tp = ~0ull;
    uint64_t destroy_tp = 0, destroy_owner_before = 0, destroy_word_after = ~0ull;
    bool image_ok = false, rest_zero = false, pattern_ok = false;
    int inits = 0, destroys = 0;
};
static std::mutex g_mu;
static std::map<uint64_t, Rec> g_rec;   // by ThreadType
static uint64_t g_cell_init, g_cell_destroy;

static void guest_tp_init(Cpu *c) {   // *nnmuslThreadPointerInitializer: x0 = ThreadType
    Rec r;
    const uint64_t tt = c->x[0];
    r.tlr = c->tpidrro;
    r.tp_before = osk::gld<uint64_t>(c->tpidrro + TLR_TP);
    r.tpidr_before = c->tpidr;
    r.thread_word_before = osk::gld<uint64_t>(c->tpidrro + TLR_THREAD);
    osk::guest_tls_hook_init(c);
    r.tp_after = osk::gld<uint64_t>(c->tpidrro + TLR_TP);
    r.tpidr_after = c->tpidr;
    std::lock_guard<std::mutex> lk(g_mu);
    Rec &d = g_rec[tt];
    d.tlr = r.tlr; d.tp_before = r.tp_before; d.tpidr_before = r.tpidr_before; d.thread_word_before = r.thread_word_before;
    d.tp_after = r.tp_after; d.tpidr_after = r.tpidr_after; d.inits++;
}
static void guest_tp_destroy(Cpu *c) {   // *nnmuslThreadPointerDestroyer: x0 = ThreadType
    const uint64_t tt = c->x[0], tp = osk::gld<uint64_t>(c->tpidrro + TLR_TP);
    const uint64_t owner = osk::guest_tls_block_owner(tp);
    osk::guest_tls_hook_destroy(c);
    std::lock_guard<std::mutex> lk(g_mu);
    Rec &d = g_rec[tt];
    d.destroy_tp = tp; d.destroy_owner_before = owner; d.destroy_word_after = osk::gld<uint64_t>(c->tpidrro + TLR_TP); d.destroys++;
}

// ---- guest threads that use their TLS ---------------------------------------------------------------------------------------------------
static const int N = 24;
static std::atomic<int> g_written{0};
// an nn::os TLS slot with a destructor: it runs at thread exit, after the entry and before the destroy hook, so the thread's block must still be
// there, owned, and hold the thread's own words (what a real TLS destructor reading its thread-locals needs)
static uint32_t g_slot;
static std::atomic<int> g_dtor_runs{0}, g_dtor_bad{0};
static void tls_dtor(Cpu *c) {
    g_dtor_runs++;
    const uint64_t tp = c->tpidr;
    const int32_t idx = (int32_t)c->x[0] - 1;
    int32_t word = -999;
    if (tp && osk::guest_tls_block_owner(tp) != 0) memcpy(&word, (const void *)(uintptr_t)(tp + 0x10), 4);
    if (word != idx) g_dtor_bad++;   // block gone (word -999) or someone else's
}
static void th_entry(Cpu *c) {
    const uint64_t idx = c->x[0], tp = c->tpidr, tlr = c->tpidrro;
    const uint64_t tt = call(c, S_CURT);
    Rec local;
    local.entry_tpidr = tp;
    local.entry_tlr_tp = osk::gld<uint64_t>(tlr + TLR_TP);
    local.entry_tp_in_tp = osk::gld<uint64_t>(tp + TLR_TP);   // the same offset taken from tp instead of the TLR: the TLS block holds zeros there
    const uint8_t *blk = (const uint8_t *)(uintptr_t)(tp - hle::kGuestTlsTpOffset);
    local.image_ok = memcmp(blk + 0x8010, kImage, 8) == 0;
    local.rest_zero = true;
    for (size_t i = 0; i < hle::kGuestTlsBlockSize; i++)
        if (i < 0x8010 || i >= 0x8018) if (blk[i]) local.rest_zero = false;
    // this thread's words: the bounds slot AddBounds would write, and a pattern of its own
    const int32_t slot = (int32_t)idx;
    memcpy((void *)(uintptr_t)(tp + 0x10), &slot, 4);
    memset((void *)(uintptr_t)(tp + 0x100), 0x40 + (int)idx, 0x400);
    call(c, S_TLSSET, (uint64_t)g_slot, idx + 1);   // arms the destructor for this thread
    g_written++;
    while (g_written.load() < N) call(c, S_SLEEP, 1 * MS);   // every thread is alive with its block while the others write theirs
    const uint8_t *pat = (const uint8_t *)(uintptr_t)(tp + 0x100);
    local.pattern_ok = true;
    for (size_t k = 0; k < 0x400; k++) if (pat[k] != (uint8_t)(0x40 + (int)idx)) local.pattern_ok = false;
    int32_t back;
    memcpy(&back, (const void *)(uintptr_t)(tp + 0x10), 4);
    if (back != slot) local.pattern_ok = false;
    if (osk::gld<uint32_t>(tp + 0x14) != 0x00732395u) local.pattern_ok = false;   // the hash word of the image is still there
    // a TLS slot of the OS layer and the thread pointer do not disturb each other
    std::lock_guard<std::mutex> lk(g_mu);
    Rec &d = g_rec[tt];
    d.entry_tpidr = local.entry_tpidr; d.entry_tlr_tp = local.entry_tlr_tp; d.entry_tp_in_tp = local.entry_tp_in_tp;
    d.image_ok = local.image_ok; d.rest_zero = local.rest_zero; d.pattern_ok = local.pattern_ok;
}

static void test_threads(Cpu *c) {
    const size_t live0 = osk::guest_tls_blocks_live();
    g_rec.clear();
    g_written = 0;
    g_dtor_runs = 0;
    g_dtor_bad = 0;
    uint64_t slot = obj(16);
    CHECK(call(c, S_TLSALLOC, slot, gfn(tls_dtor)) == 0);
    g_slot = *(uint32_t *)(uintptr_t)slot;
    std::vector<GThread> ts;
    for (int i = 0; i < N; i++) ts.push_back(spawn(c, th_entry, (uint64_t)i));
    for (auto &t : ts) join(c, t);

    std::set<uint64_t> tps, bases;
    int ok = 0;
    for (auto &t : ts) {
        auto it = g_rec.find(t.tt);
        CHECK(it != g_rec.end());
        if (it == g_rec.end()) continue;
        const Rec &r = it->second;
        CHECK(r.inits == 1 && r.destroys == 1);                           // each hook ran once per thread
        // before the hook: no thread pointer anywhere; the TLR already names the thread (host_entry set TLR+0x1f8 first)
        CHECK(r.tp_before == 0 && r.tpidr_before == 0);
        CHECK(r.thread_word_before == t.tt);
        // after the hook: TLR+0x1f0 holds tp, and tpidr_el0 is still the caller's to set
        CHECK(r.tp_after != 0 && r.tpidr_after == 0);
        // at the entry: tpidr is the TLR's word, the TLR is not the TLS block, TLR offsets are not tp offsets
        CHECK(r.entry_tpidr == r.tp_after && r.entry_tlr_tp == r.tp_after);
        CHECK(r.entry_tpidr != r.tlr && r.entry_tp_in_tp == 0);
        CHECK(r.image_ok && r.rest_zero && r.pattern_ok);
        tps.insert(r.tp_after);
        bases.insert(r.tp_after - hle::kGuestTlsTpOffset);
        // destroy: the hook saw its own block as live, owned by this ThreadType, released it, and cleared the TLR word
        CHECK(r.destroy_tp == r.tp_after && r.destroy_owner_before == t.tt && r.destroy_word_after == 0);
        CHECK(osk::guest_tls_block_owner(r.tp_after) == 0 && !mapped(r.tp_after));
        ok += r.image_ok && r.rest_zero && r.pattern_ok;
    }
    CHECK(tps.size() == (size_t)N && bases.size() == (size_t)N);           // one block per thread, no two the same
    CHECK(ok == N);
    CHECK(g_dtor_runs.load() == N && g_dtor_bad.load() == 0);              // every TLS destructor ran while its thread's block was live and intact
    CHECK(osk::guest_tls_blocks_live() == live0);                          // nothing left, nothing released twice
}

// ---- fibers keep the thread's thread pointer ---------------------------------------------------------------------------------------------------
static uint64_t g_fm;
static std::atomic<int> g_fm_n{0};
static std::atomic<uint64_t> g_fm_tp[4], g_fm_tpro[4], g_fm_word[4];
static void fiber_tls(Cpu *c) {
    for (;;) {
        int n = g_fm_n++;
        if (n < 4) {
            g_fm_tp[n] = c->tpidr;
            g_fm_tpro[n] = c->tpidrro;
            g_fm_word[n] = c->tpidr ? osk::gld<uint32_t>(c->tpidr + 0x14) : 0;   // reads the thread's TLS through the fiber's own Cpu
        }
        call(c, S_FSWITCH, 0ull);
    }
}
static std::atomic<uint64_t> g_user_tp[4], g_user_tpro[4];
static std::atomic<int> g_ready{0}, g_turn{0};
static void th_fiber_user(Cpu *c) {
    const int i = (int)c->x[0];
    g_user_tp[i] = c->tpidr;
    g_user_tpro[i] = c->tpidrro;
    osk::gst<uint32_t>(c->tpidr + 0x14, 0x1000u + (uint32_t)i);   // this thread's own word, written before the fiber runs
    g_ready++;
    while (g_ready.load() < 3) call(c, S_SLEEP, 1 * MS);          // all three threads are alive, each with its own block
    while (g_turn.load() != i) call(c, S_SLEEP, 1 * MS);          // one at a time: a fiber runs on one thread at once
    call(c, S_FSWITCH, g_fm);
    g_turn++;
    // back on the thread's own context: the same thread pointer, the same words
    CHECK(c->tpidr == g_user_tp[i].load() && c->tpidrro == g_user_tpro[i].load());
    CHECK(osk::gld<uint32_t>(c->tpidr + 0x14) == 0x1000u + (uint32_t)i);
}
static void test_fibers(Cpu *c) {
    const size_t live0 = osk::guest_tls_blocks_live();
    g_rec.clear();
    g_fm = obj(FB_SIZE);
    g_fm_n = 0;
    call(c, S_FINIT, g_fm, gfn(fiber_tls), 0ull, obj(0x4000), 0x4000ull, 0ull);
    // one fiber, resumed by three different threads in turn (the job scheduler pattern): it runs with the resuming thread's tp each time
    std::vector<GThread> ts;
    g_ready = 0;
    g_turn = 0;
    for (int i = 0; i < 3; i++) ts.push_back(spawn(c, th_fiber_user, (uint64_t)i, i));
    for (auto &t : ts) join(c, t);
    CHECK(g_fm_n.load() == 3);
    std::set<uint64_t> seen;
    for (int i = 0; i < 3; i++) {
        CHECK(g_fm_tp[i].load() == g_user_tp[i].load());                   // the fiber saw the resuming thread's tp, not another thread's
        CHECK(g_fm_tpro[i].load() == g_user_tpro[i].load());
        CHECK(g_fm_word[i].load() == 0x1000u + (uint32_t)i);               // and read that thread's own TLS through it
        seen.insert(g_user_tp[i].load());
    }
    CHECK(seen.size() == 3);
    call(c, S_FFINAL, g_fm);
    CHECK(osk::guest_tls_blocks_live() == live0);
}

// ---- the main thread has no hook and no block of this module -------------------------------------------------------------------------------
static void test_main_thread(Cpu *c) {
    CHECK(c->tpidr == 0);                                                  // the loader gives the main thread its TLS (guest_tls_loader_test), not the hook
    CHECK(osk::gld<uint64_t>(c->tpidrro + TLR_TP) == 0);
    CHECK(g_rec.count(call(c, S_CURT)) == 0);                              // the hooks never ran for it
}

int main() {
    // main's image is captured before the first guest thread, as the loader does at the first new_thread
    std::vector<uint8_t> module(0xccb900 + 0x1000, 0);
    memcpy(module.data() + hle::kMainTlsImageVaddr, kImage, 8);
    const hle::GuestTlsImage *img = osk::guest_tls_main_image(module.data(), module.size());
    CHECK(img != nullptr);

    g_cell_init = obj(8);
    g_cell_destroy = obj(8);
    *(uint64_t *)(uintptr_t)g_cell_init = gfn(guest_tp_init);
    *(uint64_t *)(uintptr_t)g_cell_destroy = gfn(guest_tp_destroy);
    OsConfig cfg{};
    cfg.tp_init_var = g_cell_init;
    cfg.tp_destroy_var = g_cell_destroy;
    os_init(&cfg);
    Cpu *c = (Cpu *)obj(sizeof(Cpu));
    os_adopt_main_thread(c, 0, -1);
    test_main_thread(c);
    test_threads(c);
    test_fibers(c);
    if (g_fail.load()) { std::fprintf(stderr, "guest_tls_os_test: %d of %d checks failed\n", g_fail.load(), g_checks.load()); return 1; }
    std::printf("guest_tls_os_test: ok (%d checks)\n", g_checks.load());
    return 0;
}
