// SPDX-License-Identifier: MIT
// Stage 5A: tests os_current_cpu() across pre-init, os_adopt_main_thread, fiber switches, and
// cross-thread fiber migration, verifying it tracks the scheduler's active Cpu* rather than loader::g_tls_cpu.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader.h"
#include "os/os.h"
#include "os/os_internal.h"
#include "os/os_layout.h"

using namespace oslay;

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#define S_CREATE  "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"
#define S_START   "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define S_WAITT   "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"
#define S_DESTROY "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"
#define S_FINIT   "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define S_FFINAL  "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define S_FSWITCH "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"

static GuestFn need(const char *s) {
    GuestFn f = os_override_find(s);
    if (!f) { std::fprintf(stderr, "missing override %s\n", s); std::abort(); }
    return f;
}
template <class... A> static uint64_t call_os(Cpu *c, const char *sym, A... a) {
    uint64_t v[] = {(uint64_t)a..., 0};
    for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = v[i];
    need(sym)(c);
    return c->x[0];
}
static uint64_t alloc_aligned(size_t n) {
    void *p = nullptr;
    if (posix_memalign(&p, 64, n) != 0) std::abort();
    std::memset(p, 0, n);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t gfn(void (*f)(Cpu *)) { return (uint64_t)(uintptr_t)f; }

static Cpu g_bogus_cpu;
static uint64_t g_fiber = 0;
static Cpu *g_fiber_cpu_on_main = nullptr;
static Cpu *g_fiber_cpu_on_worker = nullptr;
static Cpu *g_worker_own_cpu = nullptr;
static Cpu *g_worker_cpu_after = nullptr;
static std::atomic<int> g_fiber_step{0};

static void fiber_entry(Cpu *c) {
    // Step 1: running on main host thread.
    g_fiber_cpu_on_main = os_current_cpu();
    CHECK(g_fiber_cpu_on_main == c);
    g_fiber_step.store(1);

    // Yield back to main thread's own context; later T2 will resume us.
    uint64_t main_fib = osk::gld<uint64_t>(os_current_thread_type() + TT_THREAD_FIBER);
    CHECK(main_fib != 0);
    call_os(c, S_FSWITCH, main_fib);

    // Step 2: resumed on worker thread T2.
    g_fiber_cpu_on_worker = os_current_cpu();
    CHECK(g_fiber_cpu_on_worker == c);
    CHECK(g_fiber_cpu_on_worker == g_fiber_cpu_on_main);
    CHECK(g_fiber_cpu_on_worker != g_worker_own_cpu);
    CHECK(g_fiber_cpu_on_worker != loader::g_tls_cpu);
    g_fiber_step.store(2);

    // Return 0 so fiber_proc switches back to T2's own context.
    c->x[0] = 0;
}

static void worker_entry(Cpu *c) {
    g_worker_own_cpu = os_current_cpu();
    CHECK(g_worker_own_cpu == c);

    // Poison loader::g_tls_cpu on this host thread to prove os_current_cpu() does not read it.
    loader::g_tls_cpu = &g_bogus_cpu;
    call_os(c, S_FSWITCH, g_fiber);

    // Back on T2's own context after fiber_entry returned.
    g_worker_cpu_after = os_current_cpu();
    CHECK(g_worker_cpu_after == c);
    CHECK(g_worker_cpu_after == g_worker_own_cpu);
}

int main() {
    // 1. Before os_init(): must return nullptr.
    CHECK(os_current_cpu() == nullptr);

    OsConfig oc = {};
    oc.num_cores = 2;
    os_init(&oc);

    // 2. After os_init(), before os_adopt_main_thread(): still nullptr.
    CHECK(os_current_cpu() == nullptr);

    Cpu main_cpu{};
    main_cpu.excl_addr = ~0ull;
    os_adopt_main_thread(&main_cpu, 0, -1);

    // 3. After os_adopt_main_thread(): returns &main_cpu.
    CHECK(os_current_cpu() == &main_cpu);

    // Poison g_tls_cpu on main thread; os_current_cpu() must ignore it.
    loader::g_tls_cpu = &g_bogus_cpu;
    CHECK(os_current_cpu() == &main_cpu);

    // 4. Fiber switch on main thread.
    uint64_t fiber_fn = loader::add_override("test:fiber_entry", fiber_entry)->addr;
    uint64_t worker_fn = loader::add_override("test:worker_entry", worker_entry)->addr;
    g_fiber = alloc_aligned(FB_SIZE);
    uint64_t fstack = alloc_aligned(0x10000);
    call_os(&main_cpu, S_FINIT, g_fiber, fiber_fn, 0ull, fstack, 0x10000ull, 0ull);

    call_os(&main_cpu, S_FSWITCH, g_fiber);
    CHECK(g_fiber_step.load() == 1);
    CHECK(g_fiber_cpu_on_main != nullptr && g_fiber_cpu_on_main != &main_cpu);
    // Back on main thread's own context.
    CHECK(os_current_cpu() == &main_cpu);

    // 5. Resume the same fiber on a different host thread.
    uint64_t tt = alloc_aligned(TT_SIZE);
    uint64_t tstack = alloc_aligned(0x10000);
    CHECK(call_os(&main_cpu, S_CREATE, tt, worker_fn, 0ull, tstack, 0x10000ull, 16ull) == 0);
    call_os(&main_cpu, S_START, tt);
    call_os(&main_cpu, S_WAITT, tt);
    call_os(&main_cpu, S_DESTROY, tt);

    CHECK(g_fiber_step.load() == 2);
    CHECK(g_worker_own_cpu != nullptr && g_worker_own_cpu != &main_cpu);
    CHECK(g_fiber_cpu_on_worker == g_fiber_cpu_on_main);
    CHECK(os_current_cpu() == &main_cpu);

    call_os(&main_cpu, S_FFINAL, g_fiber);
    std::printf("os_current_cpu_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
