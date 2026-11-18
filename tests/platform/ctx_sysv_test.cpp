// SPDX-License-Identifier: MIT
// Isolated tests of the Linux x86-64 System V context switch (runtime/os/linux/os_ctx_sysv.cpp). No scheduler, no guest, no GPU, no SDL.
//
//   ctx_sysv_test [--repeat N]     runs the whole suite N times (default 1) in one process
//
// What is checked, and how it can fail:
//  * callee-saved registers: snap_test_switch_regs (below, test-only assembly) loads rbx, rbp, r12-r15 with a pattern, calls
//    snap_ctx_switch, and stores the registers it finds when it is resumed. Other contexts run in between with other patterns.
//  * MXCSR and the x87 control word: every context sets its own, and the values are also put to work (rounding of a conversion) so a
//    restored-but-not-effective value would be seen.
//  * stack alignment: rsp is recorded by assembly at the first instruction of an entry function; locals are checked after resumes.
//  * a function that ends: by switching away (the way os_thread.cpp's fiber_proc ends) and by returning (must trap: ud2 -> SIGILL, run in
//    a child process).
//  * resume on another host thread, including %fs and thread_local values; many hops.
//  * many switches with a checksum and per-context stack canaries.
#include <emmintrin.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/prctl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "os/os_internal.h"

using osk::HostCtx;

static std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---------------------------------------------------------------------------------------------------- test-only assembly
extern "C" {
uint64_t g_entry_rsp;                                   // rsp at the first instruction of snap_test_entry_probe
void snap_test_entry_probe(void *arg);                  // records rsp, then jumps to snap_test_entry_body (same alignment)
void snap_test_entry_body(void *arg);
void snap_test_switch_regs(HostCtx *from, HostCtx *to, const uint64_t in[6], uint64_t out[6]);
}
__asm__(
    ".text\n"
    ".globl snap_test_entry_probe\n"
    "snap_test_entry_probe:\n"
    "  movq %rsp, g_entry_rsp(%rip)\n"
    "  jmp snap_test_entry_body@PLT\n"
    ".globl snap_test_switch_regs\n"
    "snap_test_switch_regs:\n"      // rdi = from, rsi = to, rdx = in[6] (rbx rbp r12 r13 r14 r15), rcx = out[6]
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $24, %rsp\n"            // 8 (return) + 48 + 24 = 80: rsp 16-aligned for the call
    "  movq %rcx, 0(%rsp)\n"
    "  movq 0(%rdx), %rbx\n  movq 8(%rdx), %rbp\n  movq 16(%rdx), %r12\n  movq 24(%rdx), %r13\n  movq 32(%rdx), %r14\n  movq 40(%rdx), %r15\n"
    "  call snap_ctx_switch@PLT\n"
    "  movq 0(%rsp), %rax\n"
    "  movq %rbx, 0(%rax)\n  movq %rbp, 8(%rax)\n  movq %r12, 16(%rax)\n  movq %r13, 24(%rax)\n  movq %r14, 32(%rax)\n  movq %r15, 40(%rax)\n"
    "  addq $24, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
    "  ret\n");

// ---------------------------------------------------------------------------------------------------- helpers
static uint64_t mix(uint64_t x) {   // splitmix64
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
static void pattern(uint64_t id, uint64_t round, uint64_t out[6]) { for (int i = 0; i < 6; i++) out[i] = mix(id * 0x1000003ull + round * 8 + (uint64_t)i); }
static bool same(const uint64_t a[6], const uint64_t b[6]) { return std::memcmp(a, b, 48) == 0; }

static uint32_t get_mxcsr() { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(uint32_t v) { __asm__ volatile("ldmxcsr %0" ::"m"(v)); }
static uint16_t get_fcw() { uint16_t v; __asm__ volatile("fnstcw %0" : "=m"(v)); return v; }
static void set_fcw(uint16_t v) { __asm__ volatile("fldcw %0" ::"m"(v)); }
static constexpr uint32_t kMxcsrCtl = 0xFFC0;   // control bits (the low six are sticky status flags)

// a conversion whose result depends on the rounding mode of the SSE unit / the x87 unit
static int sse_round(double v) { return _mm_cvtsd_si32(_mm_set_sd(v)); }
static int x87_round(double v) { int r; __asm__ volatile("fldl %1\n fistpl %0" : "=m"(r) : "m"(v) : "st"); return r; }

static const size_t kStack = 1 << 20;   // 1 MiB per test fiber

// ---------------------------------------------------------------------------------------------------- 1. state and layout
static void test_new_contexts() {
    HostCtx *t = osk::ctx_new_thread();
    CHECK(t && t->sp == nullptr && t->alloc == nullptr && t->serial != 0);
    HostCtx *f = osk::ctx_new_fiber(kStack, [](void *) {}, nullptr);
    CHECK(f != nullptr);
    if (f) {
        CHECK(((uintptr_t)f->sp & 15) == 8);                                     // rsp after the frame (sp + 72) is 16-aligned
        CHECK(((uintptr_t)f->sp + 72) % 16 == 0);
        CHECK(f->alloc == f->dealloc && (uintptr_t)f->base == (uintptr_t)f->alloc + f->reserve);
        CHECK(f->reserve == kStack && f->sp > f->limit && f->sp < f->base);
        CHECK((uintptr_t)f->limit == (uintptr_t)f->alloc + (size_t)sysconf(_SC_PAGESIZE));
        CHECK(f->serial != t->serial);
        // the usable range is writable at both ends
        ((volatile char *)f->limit)[0] = 1;
        ((volatile char *)f->base)[-1] = 1;
        // initial control words
        uint32_t m; uint16_t c;
        std::memcpy(&m, f->sp, 4); std::memcpy(&c, (char *)f->sp + 4, 2);
        CHECK(m == 0x1F80 && c == 0x037F);
    }
    HostCtx *g = osk::ctx_new_fiber(kStack, [](void *) {}, nullptr);
    CHECK(g && f && g->serial > f->serial);
    osk::ctx_free(f);   // never ran
    osk::ctx_free(g);
    osk::ctx_free(t);
    osk::ctx_free(nullptr);
    CHECK(osk::ctx_new_fiber(4096, [](void *) {}, nullptr) == nullptr);                 // too small to hold a guard page and a frame
    CHECK(osk::ctx_new_fiber((size_t)1 << 62, [](void *) {}, nullptr) == nullptr);      // impossible reservation: an error, not a crash
}

// ---------------------------------------------------------------------------------------------------- 2. registers, ring of three
struct Ring {
    HostCtx *ctx[3];
    int rounds;
    std::atomic<int> errors{0};
    std::atomic<int> finished{0};
};
static Ring *g_ring;

static void ring_step(Ring *r, int self) {
    const int next = (self + 1) % 3;
    uint64_t in[6], out[6];
    for (int round = 0; round < r->rounds; round++) {
        pattern((uint64_t)self, (uint64_t)round, in);
        alignas(16) uint64_t canary[32];
        for (int i = 0; i < 32; i++) canary[i] = mix((uint64_t)self * 77 + (uint64_t)round + (uint64_t)i);
        snap_test_switch_regs(r->ctx[self], r->ctx[next], in, out);
        if (!same(in, out)) r->errors++;
        for (int i = 0; i < 32; i++) if (canary[i] != mix((uint64_t)self * 77 + (uint64_t)round + (uint64_t)i)) r->errors++;
        if (((uintptr_t)canary & 15) != 0) r->errors++;
    }
}
extern "C" void snap_test_entry_body(void *arg) {   // used by the ring and by the alignment test
    Ring *r = g_ring;
    const int self = (int)(intptr_t)arg;
    alignas(16) float v[4] = {1, 2, 3, 4};
    if (g_entry_rsp % 16 != 8 || ((uintptr_t)v & 15) != 0) r->errors++;   // ABI alignment at the entry, and of a local
    __m128 x = _mm_load_ps(v);
    x = _mm_add_ps(x, x);
    _mm_store_ps(v, x);
    if (v[3] != 8.0f) r->errors++;
    ring_step(r, self);
    r->finished++;
    // ends the way os_thread.cpp's fiber_proc does: by switching away, for good
    uint64_t in[6] = {}, out[6];
    snap_test_switch_regs(r->ctx[self], r->ctx[0], in, out);
    std::abort();   // never resumed
}

// after its own rounds main resumes each fiber once so that it can finish its last switch; each then hands control back for good
static void drain(Ring &ring) {
    for (int i = 0; i < 2; i++) {
        uint64_t in[6] = {}, out[6];
        const int before = ring.finished.load();
        snap_test_switch_regs(ring.ctx[0], ring.ctx[1 + i], in, out);
        if (ring.finished.load() != before + 1) ring.errors++;
    }
}

static void test_registers_ring() {
    Ring ring;
    ring.rounds = 2000;
    g_ring = &ring;
    ring.ctx[0] = osk::ctx_new_thread();
    ring.ctx[1] = osk::ctx_new_fiber(kStack, snap_test_entry_probe, (void *)1);
    ring.ctx[2] = osk::ctx_new_fiber(kStack, snap_test_entry_probe, (void *)2);
    CHECK(ring.ctx[1] && ring.ctx[2]);
    if (!ring.ctx[1] || !ring.ctx[2]) return;
    // main is context 0: it takes part in the ring, then waits for the other two to finish
    ring_step(&ring, 0);
    // the fibers finish their rounds by switching to ctx[0] one last time each; collect both
    drain(ring);
    CHECK(ring.errors.load() == 0);
    CHECK(ring.finished.load() == 2);
    for (HostCtx *c : ring.ctx) osk::ctx_free(c);
}

// ---------------------------------------------------------------------------------------------------- 3. MXCSR and x87 control word
struct Fpu {
    HostCtx *ctx[3];
    uint32_t mxcsr[3];
    uint16_t fcw[3];
    std::atomic<int> errors{0};
    std::atomic<int> done{0};
};
static Fpu *g_fpu;
static void fpu_check(Fpu *f, int self, bool initial) {
    if ((get_mxcsr() & kMxcsrCtl) != (f->mxcsr[self] & kMxcsrCtl)) f->errors++;
    if (get_fcw() != f->fcw[self]) f->errors++;
    (void)initial;
}
static void fpu_loop(Fpu *f, int self, int rounds) {
    const int next = (self + 1) % 3;
    uint64_t in[6] = {}, out[6];
    for (int i = 0; i < rounds; i++) {
        set_mxcsr(f->mxcsr[self]);
        set_fcw(f->fcw[self]);
        snap_ctx_switch(f->ctx[self], f->ctx[next]);
        fpu_check(f, self, false);
        // the values are in effect, not only stored: 2.5 rounds according to the control word
        const bool up = ((f->mxcsr[self] >> 13) & 3) == 2;
        const bool x87up = ((f->fcw[self] >> 10) & 3) == 2;
        if (sse_round(2.5) != (up ? 3 : 2)) f->errors++;
        if (x87_round(2.5) != (x87up ? 3 : 2)) f->errors++;
        (void)in; (void)out;
    }
}
static void fpu_fiber(void *arg) {
    Fpu *f = g_fpu;
    const int self = (int)(intptr_t)arg;
    // a new context starts with the defaults, whatever its creator had set
    if ((get_mxcsr() & kMxcsrCtl) != (0x1F80 & kMxcsrCtl) || get_fcw() != 0x037F) f->errors++;
    fpu_loop(f, self, 500);
    f->done++;
    for (;;) snap_ctx_switch(f->ctx[self], f->ctx[0]);   // never resumed by main again once done
}
static void test_fpu() {
    Fpu f;
    g_fpu = &f;
    f.mxcsr[0] = 0x1F80 | (2 << 13);                 // round up
    f.mxcsr[1] = 0x1F80 | (1 << 13) | 0x8000 | 0x40; // round down, FTZ, DAZ
    f.mxcsr[2] = 0x1F80;                             // nearest
    f.fcw[0] = 0x037F | (2 << 10);                   // x87 round up, extended precision
    f.fcw[1] = 0x027F | (1 << 10);                   // round down, double precision
    f.fcw[2] = 0x007F;                               // nearest, single precision
    const uint32_t save_m = get_mxcsr();
    const uint16_t save_c = get_fcw();
    set_mxcsr(f.mxcsr[0] | 0x40);                    // weird values at creation: must not leak into the new contexts
    set_fcw(0x0C7F);
    f.ctx[0] = osk::ctx_new_thread();
    f.ctx[1] = osk::ctx_new_fiber(kStack, fpu_fiber, (void *)1);
    f.ctx[2] = osk::ctx_new_fiber(kStack, fpu_fiber, (void *)2);
    CHECK(f.ctx[1] && f.ctx[2]);
    if (f.ctx[1] && f.ctx[2]) {
        // main joins the ring as context 0 for the same number of rounds; fibers end after 500 rounds
        fpu_loop(&f, 0, 500);
        // main resumes each fiber once so that it can finish its last switch; each then hands control back for good
        for (int i = 0; i < 2; i++) snap_ctx_switch(f.ctx[0], f.ctx[1 + i]);
        CHECK(f.errors.load() == 0);
        CHECK(f.done.load() == 2);
    }
    set_mxcsr(save_m);
    set_fcw(save_c);
    for (HostCtx *c : f.ctx) osk::ctx_free(c);
}

// ---------------------------------------------------------------------------------------------------- 4. termination
static pid_t fork_run(void (*child)()) {
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        struct rlimit rl = {0, 0};
        setrlimit(RLIMIT_CORE, &rl);   // a killed child must not leave a core file
        child();
        _exit(0);
    }
    return p;
}
static int wait_signal(pid_t p) {
    int st = 0;
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : (WIFEXITED(st) ? -WEXITSTATUS(st) - 1000 : -1);
}
static HostCtx *g_child_main;
static void returning_entry(void *) { /* returns into the trampoline */ }
static void child_return() {
    g_child_main = osk::ctx_new_thread();
    HostCtx *f = osk::ctx_new_fiber(kStack, returning_entry, nullptr);
    snap_ctx_switch(g_child_main, f);
    _exit(0);   // not reached: the trampoline executes ud2 when the entry returns
}
static void child_guard() {
    HostCtx *f = osk::ctx_new_fiber(kStack, returning_entry, nullptr);
    ((volatile char *)f->limit)[-1] = 1;   // the guard page
    _exit(0);
}
static void test_termination() {
    CHECK(wait_signal(fork_run(child_return)) == SIGILL);   // an entry that returns traps instead of running off the stack
    CHECK(wait_signal(fork_run(child_guard)) == SIGSEGV);   // the page below the usable range is inaccessible
}

// ---------------------------------------------------------------------------------------------------- 5. resume on another host thread
static thread_local int tl_value = 0;
__attribute__((noinline)) static int read_tl() { __asm__ volatile("" ::: "memory"); return tl_value; }
static uint64_t fs_base() { unsigned long v = 0; syscall(SYS_arch_prctl, ARCH_GET_FS, &v); return (uint64_t)v; }

struct Hop {
    HostCtx *fiber;
    std::atomic<HostCtx *> base[2];   // the base context of each of the two threads (published after fs/tid)
    std::atomic<int> turn{0};         // which thread may switch into the fiber
    std::atomic<int> cur{0};          // the thread the fiber is running on right now
    std::atomic<int> errors{0};
    std::atomic<int> hops{0};
    int target;
    uint64_t fs[2] = {0, 0};
    pid_t tid[2] = {0, 0};
    std::atomic<bool> done{false};
};
static Hop *g_hop;
static void hop_fiber(void *) {
    Hop *h = g_hop;
    for (int round = 0;; round++) {
        uint64_t in[6], out[6];
        pattern(99, (uint64_t)round, in);
        const int me = h->cur.load();                         // the thread this step runs on
        if (fs_base() != h->fs[me]) h->errors++;              // %fs is the one of the thread that resumed us
        if ((pid_t)syscall(SYS_gettid) != h->tid[me]) h->errors++;
        if (read_tl() != 1000 + me) h->errors++;              // thread_local read after a hop gives the new thread's value
        alignas(16) char probe[16];
        if (((uintptr_t)probe & 15) != 0) h->errors++;
        h->hops++;
        if (h->hops.load() >= h->target) {
            h->done = true;                                   // the thread that resumed us sees it and ends; the other one leaves its wait too
            snap_test_switch_regs(h->fiber, h->base[me].load(), in, out);
            std::abort();
        }
        snap_test_switch_regs(h->fiber, h->base[me].load(), in, out);   // the thread hands the fiber on only after this switch has completed
        if (!same(in, out)) h->errors++;                      // callee-saved registers survived a hop to another thread and back
    }
}
static void hop_thread(int me) {
    Hop *h = g_hop;
    tl_value = 1000 + me;
    h->fs[me] = fs_base();
    h->tid[me] = (pid_t)syscall(SYS_gettid);
    HostCtx *mine = osk::ctx_new_thread();
    h->base[me].store(mine, std::memory_order_release);
    while (!h->done.load()) {
        if (h->turn.load() != me) { sched_yield(); continue; }
        h->cur = me;
        uint64_t in[6], out[6];
        pattern(7 + (uint64_t)me, 0, in);
        const uint64_t fs_before = fs_base();
        snap_test_switch_regs(mine, h->fiber, in, out);
        h->turn = 1 - me;                                     // the fiber is fully suspended now: the other thread may take it
        if (fs_base() != fs_before) h->errors++;              // the switch itself leaves %fs alone
        if (!same(in, out)) h->errors++;
        if (read_tl() != 1000 + me) h->errors++;
    }
    osk::ctx_free(mine);
}
static void test_thread_hop() {
    Hop h;
    h.target = 2000;
    g_hop = &h;
    h.fiber = osk::ctx_new_fiber(kStack, hop_fiber, nullptr);
    CHECK(h.fiber != nullptr);
    if (!h.fiber) return;
    // both threads publish fs/tid/base before the first turn is taken: start them, then release turn 0 when both are ready
    h.turn = -1;
    std::thread a(hop_thread, 0), b(hop_thread, 1);
    while (!h.base[0].load(std::memory_order_acquire) || !h.base[1].load(std::memory_order_acquire)) sched_yield();
    CHECK(h.fs[0] != h.fs[1] && h.tid[0] != h.tid[1]);
    h.turn = 0;
    a.join();
    b.join();
    CHECK(h.errors.load() == 0);
    CHECK(h.hops.load() >= h.target);
    osk::ctx_free(h.fiber);
}

// ---------------------------------------------------------------------------------------------------- 6. many switches
static void test_many_cycles() {
    Ring ring;
    ring.rounds = 60000;   // 3 contexts x 60k = 180k switches
    g_ring = &ring;
    ring.ctx[0] = osk::ctx_new_thread();
    ring.ctx[1] = osk::ctx_new_fiber(kStack, snap_test_entry_probe, (void *)1);
    ring.ctx[2] = osk::ctx_new_fiber(kStack, snap_test_entry_probe, (void *)2);
    if (!ring.ctx[1] || !ring.ctx[2]) { CHECK(false); return; }
    ring_step(&ring, 0);
    drain(ring);
    CHECK(ring.errors.load() == 0);
    for (HostCtx *c : ring.ctx) osk::ctx_free(c);
}

int main(int argc, char **argv) {
    int repeat = 1;
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--repeat")) repeat = std::atoi(argv[i + 1]);
    for (int i = 0; i < repeat; i++) {
        test_new_contexts();
        test_termination();      // forks: before any thread exists
        test_registers_ring();
        test_fpu();
        test_thread_hop();
        test_many_cycles();
        if (g_fail.load()) break;
    }
    if (g_fail.load()) { std::fprintf(stderr, "ctx_sysv_test: %d check(s) failed\n", g_fail.load()); return 1; }
    std::printf("ctx_sysv_test: ok (%d run%s)\n", repeat, repeat == 1 ? "" : "s");
    return 0;
}
