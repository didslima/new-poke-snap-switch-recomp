// SPDX-License-Identifier: MIT
// Tests of the POSIX implementation of runtime/os/os_host.h (runtime/os/linux/os_host_posix.cpp), alone: no scheduler, no guest, no GPU.
// Each check can fail on a wrong implementation: protections are probed with real accesses (SIGSEGV is caught or observed in a child),
// not assumed from the return values.
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "os_host.h"

using namespace osk::host;

static std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const size_t PAGE = 4096;
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- access probe: true when a byte can be read and written, false on SIGSEGV
static thread_local sigjmp_buf *t_jb;
static void on_segv(int) { if (t_jb) siglongjmp(*t_jb, 1); signal(SIGSEGV, SIG_DFL); }
static bool writable(void *p) {
    struct sigaction sa{}, old;
    sa.sa_handler = on_segv;
    sa.sa_flags = SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    sigjmp_buf jb;
    t_jb = &jb;
    bool ok = false;
    if (sigsetjmp(jb, 1) == 0) { volatile char *c = (volatile char *)p; char v = *c; *c = v; ok = true; }
    t_jb = nullptr;
    sigaction(SIGSEGV, &old, nullptr);
    return ok;
}

// ---- child process helper: runs fn, returns the terminating signal (or -1000 - exit status when it exited)
static int run_child(void (*fn)()) {
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        struct rlimit rl = {0, 0};
        setrlimit(RLIMIT_CORE, &rl);
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : -1000 - WEXITSTATUS(st);
}

// ---------------------------------------------------------------------------------------------------- time
static void test_time() {
    const int64_t f = counter_frequency();
    CHECK(f == 1000000000ll);
    int64_t prev = counter();
    bool mono = true;
    for (int i = 0; i < 100000; i++) { const int64_t c = counter(); if (c < prev) mono = false; prev = c; }
    CHECK(mono);
    // a sleep lasts at least what was asked, and not wildly more
    for (int64_t ns : {200000ll, 1500000ll, 20000000ll}) {
        const int64_t a = counter();
        sleep_precise_ns(ns);
        const int64_t d = counter() - a;
        CHECK(d >= ns);
        CHECK(d < ns + 30000000ll);
    }
    const int64_t a = counter();
    sleep_precise_ns(0);
    sleep_precise_ns(-5);
    CHECK(counter() - a < 5000000);   // non-positive: returns at once
    const double t0 = now_ms();
    sleep_ms(15);
    CHECK(now_ms() - t0 >= 15.0 && now_ms() - t0 < 60.0);
    timers_init();   // nothing to do on POSIX; must not fail
}

// ---------------------------------------------------------------------------------------------------- threads
static std::atomic<int> g_ran{0};
static std::atomic<uint32_t> g_tid_in_thread{0};
static void entry_flag(void *arg) { g_tid_in_thread = thread_id(); ((std::atomic<int> *)arg)->fetch_add(1); }
static size_t g_touch;
static void entry_touch(void *) {   // uses g_touch bytes of the thread's own stack
    volatile char *buf = (volatile char *)alloca(g_touch);
    for (size_t i = 0; i < g_touch; i += 1024) buf[i] = 1;
    g_ran = 1;
}
static void entry_name(void *arg) {
    set_current_thread_name("a-very-long-thread-name-that-does-not-fit");
    char b[64] = {};
    CHECK(thread_description(thread_id(), b, sizeof b));
    std::strncpy((char *)arg, b, 63);
}
static void child_small_stack() {
    g_touch = 3u << 20;
    g_ran = 0;
    HostThread *t = thread_create_suspended(1u << 20, entry_touch, nullptr);   // 1 MiB reserved, 3 MiB used
    thread_resume(t);
    thread_join(t);
}
static void test_threads_forks() {
    CHECK(run_child(child_small_stack) == SIGSEGV);   // the reservation is the size of the stack: overflowing it faults
}
static void test_threads() {
    // not running before resume, running after, joined
    g_ran = 0;
    HostThread *t = thread_create_suspended(1u << 20, entry_flag, &g_ran);
    CHECK(t != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(g_ran.load() == 0);   // created suspended
    thread_resume(t);
    thread_join(t);
    CHECK(g_ran.load() == 1);
    CHECK(g_tid_in_thread.load() != 0 && g_tid_in_thread.load() != thread_id());
    CHECK(thread_id() == (uint32_t)syscall(SYS_gettid));
    // a thread that ends at once is still joined (the OS layer stores the handle, then resumes)
    for (int i = 0; i < 200; i++) {
        std::atomic<int> n{0};
        HostThread *h = thread_create_suspended(256u << 10, entry_flag, &n);
        thread_resume(h);
        thread_join(h);
        CHECK(n.load() == 1);
    }
    // stack: 3 MiB fit in a 4 MiB reservation
    g_touch = 3u << 20;
    g_ran = 0;
    HostThread *s = thread_create_suspended(4u << 20, entry_touch, nullptr);
    thread_resume(s);
    thread_join(s);
    CHECK(g_ran.load() == 1);
    // the 256 MiB reservation the scheduler asks for per guest thread
    HostThread *big = thread_create_suspended(256ull << 20, entry_flag, &g_ran);
    CHECK(big != nullptr);
    if (big) { thread_resume(big); thread_join(big); }
    // names: cut to 15 characters
    char got[64] = {};
    HostThread *nm = thread_create_suspended(1u << 20, entry_name, got);
    thread_resume(nm);
    thread_join(nm);
    CHECK(std::strcmp(got, "a-very-long-thr") == 0);
    yield();
    CHECK(last_error() == last_error());   // callable
}
static int g_atexit_pipe = -1;
static void atexit_mark() { (void)!write(g_atexit_pipe, "X", 1); }
static void child_terminate() {
    int fds[2];
    if (pipe(fds)) _exit(3);
    g_atexit_pipe = fds[1];
    atexit(atexit_mark);
    std::thread other([] { for (;;) sleep_ms(10); });   // another thread is running when the process ends
    other.detach();
    sleep_ms(20);
    terminate_process();
}
static void test_terminate() {
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        close(fds[0]);
        g_atexit_pipe = fds[1];
        atexit(atexit_mark);
        std::thread other([] { for (;;) sleep_ms(10); });
        other.detach();
        sleep_ms(20);
        terminate_process();
    }
    close(fds[1]);
    int st = 0;
    waitpid(p, &st, 0);
    char c;
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);   // ended, did not hang or crash
    CHECK(read(fds[0], &c, 1) == 0);                 // the atexit handler did not run (end of file, nothing written)
    close(fds[0]);
    (void)child_terminate;
}

// ---------------------------------------------------------------------------------------------------- heap
static void test_heap() {
    for (size_t a : {16ul, 64ul, 512ul, 4096ul, 65536ul}) {
        void *p = heap_aligned_alloc(1, a);
        CHECK(p && ((uintptr_t)p & (a - 1)) == 0);
        std::memset(p, 0x5a, 1);
        heap_aligned_free(p);
    }
    void *big = heap_aligned_alloc(100u << 20, 64);
    CHECK(big != nullptr);
    heap_aligned_free(big);
    CHECK(heap_aligned_alloc((size_t)1 << 62, 64) == nullptr);
    heap_aligned_free(nullptr);
}

// ---------------------------------------------------------------------------------------------------- virtual memory
static void child_touch_reserved() {
    MemErr e;
    char *p = (char *)mem_reserve(16 * PAGE, PAGE, false, &e);
    *(volatile char *)p = 1;   // reserved only: no access
}
static void child_touch_decommitted() {
    MemErr e;
    char *p = (char *)mem_reserve(16 * PAGE, PAGE, false, &e);
    mem_commit(p, 16 * PAGE, &e);
    p[0] = 1;
    mem_decommit(p, 16 * PAGE);
    *(volatile char *)p = 2;
}
static void child_touch_released() {
    MemErr e;
    char *p = (char *)mem_reserve(16 * PAGE, PAGE, false, &e);
    mem_commit(p, 16 * PAGE, &e);
    mem_release(p, 16 * PAGE);
    *(volatile char *)p = 2;
}
static void test_memory_forks() {
    CHECK(run_child(child_touch_reserved) == SIGSEGV);
    CHECK(run_child(child_touch_decommitted) == SIGSEGV);
    CHECK(run_child(child_touch_released) == SIGSEGV);
}
static void test_memory_basic() {
    CHECK(!write_watch_supported());
    MemErr e;
    // alignment: the base is aligned to the request, and never to less than 64 KiB
    for (size_t al : {PAGE, (size_t)64 << 10, (size_t)2 << 20, (size_t)32 << 20, (size_t)1 << 30}) {
        void *p = mem_reserve(16 * PAGE, al, false, &e);
        CHECK(p != nullptr);
        if (!p) continue;
        const size_t want = al < (64u << 10) ? (64u << 10) : al;
        CHECK(((uintptr_t)p & (want - 1)) == 0);
        mem_release(p, 16 * PAGE);
    }
    // a write-watch request is accepted and gives a plain reservation where it is not supported
    void *w = mem_reserve(8 * PAGE, PAGE, true, &e);
    CHECK(w != nullptr);
    CHECK(mem_commit(w, 8 * PAGE, &e));
    CHECK(writable(w));
    mem_release(w, 8 * PAGE);

    char *p = (char *)mem_reserve(64 * PAGE, PAGE, false, &e);
    CHECK(p != nullptr);
    CHECK(!writable(p));                                   // reserved: no access
    size_t com = 99, not_com = 99;
    mem_count(p, 64 * PAGE, &com, &not_com);
    CHECK(com == 0 && not_com == 64 * PAGE);
    CHECK(mem_commit(p + 4 * PAGE, 8 * PAGE, &e));
    mem_count(p, 64 * PAGE, &com, &not_com);
    CHECK(com == 8 * PAGE && not_com == 56 * PAGE);
    CHECK(!writable(p + 3 * PAGE) && writable(p + 4 * PAGE) && writable(p + 11 * PAGE) && !writable(p + 12 * PAGE));
    std::memset(p + 4 * PAGE, 0xAB, 8 * PAGE);
    CHECK(mem_commit(p + 6 * PAGE, 8 * PAGE, &e));        // overlaps committed pages: legal, keeps their contents
    CHECK((uint8_t)p[7 * PAGE] == 0xAB && (uint8_t)p[11 * PAGE + 17] == 0xAB);
    CHECK((uint8_t)p[13 * PAGE] == 0);                    // the newly committed part is zero
    mem_count(p + 4 * PAGE, 10 * PAGE, &com, &not_com);
    CHECK(com == 10 * PAGE && not_com == 0);
    mem_decommit(p + 8 * PAGE, 2 * PAGE);                  // a hole in the middle
    CHECK(!writable(p + 8 * PAGE) && !writable(p + 9 * PAGE) && writable(p + 7 * PAGE) && writable(p + 10 * PAGE));
    mem_count(p, 64 * PAGE, &com, &not_com);
    CHECK(com == 8 * PAGE && not_com == 56 * PAGE);
    mem_decommit(p, 64 * PAGE);                            // whole range, including pages that were never committed
    mem_count(p, 64 * PAGE, &com, &not_com);
    CHECK(com == 0);
    // decommitted pages come back zero-filled
    CHECK(mem_commit(p + 4 * PAGE, 8 * PAGE, &e));
    bool zero = true;
    for (size_t i = 0; i < 8 * PAGE; i++) if (p[4 * PAGE + i] != 0) zero = false;
    CHECK(zero);
    mem_release(p, 64 * PAGE);
    mem_count(p, 64 * PAGE, &com, &not_com);
    CHECK(com == 0);
    // released address space can be taken again at the same address; a taken range is refused
    void *again = mem_reserve_at(p, 64 * PAGE, false);
    CHECK(again == (void *)p);
    CHECK(mem_reserve_at(p + 4 * PAGE, 8 * PAGE, false) == nullptr);
    mem_release(p, 64 * PAGE);

    // free / taken address space
    uint64_t end = 0;
    char detail[160] = {};
    char *q = (char *)mem_reserve(4 * PAGE, PAGE, false, &e);
    CHECK(!mem_find_free(q, &end, detail, sizeof detail));
    CHECK(std::strstr(detail, "mapped") != nullptr);
    mem_release(q, 4 * PAGE);
    CHECK(mem_find_free(q, &end, detail, sizeof detail));
    CHECK(end > (uint64_t)(uintptr_t)q);
    CHECK(mem_find_free(q, &end, detail, sizeof detail) && end >= (uint64_t)(uintptr_t)q + 4 * PAGE);

    // errors
    CHECK(mem_reserve((size_t)1 << 50, PAGE, false, &e) == nullptr);
    CHECK(e.transient && e.code != 0);                     // out of memory / address space: may pass
    CHECK(mem_reserve(PAGE + 1, PAGE, false, &e) == nullptr);
    CHECK(!e.transient);                                   // a malformed request does not
    const MemErr inj = mem_injected_error();
    CHECK(inj.transient && inj.code != 0);
    uint64_t lo = 0, hi = 0;
    mem_app_range(&lo, &hi);
    CHECK(lo == 0x10000 && hi == 0x7fffffffffffull);
}

// the tracked commit state follows the real protections through a long random sequence of operations
static void test_memory_model() {
    const size_t N = 256;
    MemErr e;
    char *p = (char *)mem_reserve(N * PAGE, 64 << 10, false, &e);
    CHECK(p != nullptr);
    std::vector<bool> model(N, false);
    std::mt19937 rng(12345);
    for (int it = 0; it < 3000; it++) {
        const size_t a = rng() % N, n = 1 + rng() % std::min<size_t>(40, N - a);
        const int op = (int)(rng() % 3);
        if (op == 0) { CHECK(mem_commit(p + a * PAGE, n * PAGE, &e)); for (size_t i = a; i < a + n; i++) model[i] = true; }
        else if (op == 1) { mem_decommit(p + a * PAGE, n * PAGE); for (size_t i = a; i < a + n; i++) model[i] = false; }
        else {
            const size_t b = rng() % N, m = 1 + rng() % (N - b);
            size_t com = 0, not_com = 0, want = 0;
            mem_count(p + b * PAGE, m * PAGE, &com, &not_com);
            for (size_t i = b; i < b + m; i++) want += model[i] ? PAGE : 0;
            CHECK(com == want && com + not_com == m * PAGE);
        }
        if (it % 150 == 0)
            for (size_t i = 0; i < N; i++) CHECK(writable(p + i * PAGE) == model[i]);   // the real protection agrees with the model
    }
    mem_release(p, N * PAGE);
}

// several threads commit and decommit their own parts of one reservation while others count
static void test_memory_threads() {
    MemErr e;
    const size_t T = 8, per = 64;
    char *p = (char *)mem_reserve(T * per * PAGE, 64 << 10, false, &e);
    CHECK(p != nullptr);
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (size_t t = 0; t < T; t++)
        th.emplace_back([&, t] {
            std::mt19937 rng((unsigned)t);
            char *base = p + t * per * PAGE;
            std::vector<bool> model(per, false);
            for (int it = 0; it < 4000; it++) {
                const size_t a = rng() % per, n = 1 + rng() % std::min<size_t>(8, per - a);
                MemErr le;
                if (rng() & 1) { if (!mem_commit(base + a * PAGE, n * PAGE, &le)) bad++; for (size_t i = a; i < a + n; i++) model[i] = true; }
                else { mem_decommit(base + a * PAGE, n * PAGE); for (size_t i = a; i < a + n; i++) model[i] = false; }
                size_t com, nc, want = 0;
                mem_count(base, per * PAGE, &com, &nc);
                for (size_t i = 0; i < per; i++) want += model[i] ? PAGE : 0;
                if (com != want) bad++;
            }
        });
    for (auto &x : th) x.join();
    CHECK(bad.load() == 0);
    mem_release(p, T * per * PAGE);
}

// what Windows refuses or ignores and POSIX would not: the host layer must behave the same (a commit outside a reservation is an error that is
// not memory pressure; a decommit or release of memory it did not reserve must not touch that memory)
static void test_memory_contract() {
    MemErr e;
    char *p = (char *)mem_reserve(16 * PAGE, PAGE, false, &e);
    CHECK(p != nullptr);
    // a commit that is not inside one reservation fails, and says it is not transient (a retry loop would never end)
    e = MemErr{};
    CHECK(!mem_commit(p + 15 * PAGE, 2 * PAGE, &e));          // runs past the end
    CHECK(e.code != 0 && !e.transient);
    e = MemErr{};
    CHECK(!mem_commit(p - 64 * PAGE, PAGE, &e) && !e.transient);  // before it: not ours
    char *gap = (char *)mem_reserve(4 * PAGE, PAGE, false, &e);
    mem_release(gap, 4 * PAGE);
    e = MemErr{};
    CHECK(!mem_commit(gap, PAGE, &e) && e.code != 0 && !e.transient);   // released address space
    size_t com = 7, nc = 7;
    mem_count(p, 16 * PAGE, &com, &nc);
    CHECK(com == 0);                                          // a failed commit commits nothing
    // an unaligned range is rounded outwards to whole pages
    CHECK(mem_commit(p + 3 * PAGE + 100, 1, &e));
    mem_count(p, 16 * PAGE, &com, &nc);
    CHECK(com == PAGE && writable(p + 3 * PAGE) && !writable(p + 2 * PAGE) && !writable(p + 4 * PAGE));
    CHECK(mem_commit(p + 5 * PAGE + PAGE - 1, 2, &e));        // straddles two pages
    CHECK(writable(p + 5 * PAGE) && writable(p + 6 * PAGE) && !writable(p + 7 * PAGE));

    // memory that is not ours is not touched by decommit or release
    char *other = (char *)mmap(nullptr, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(other != MAP_FAILED);
    std::memset(other, 0x77, 4 * PAGE);
    mem_decommit(other, 4 * PAGE);
    CHECK(writable(other) && (uint8_t)other[PAGE + 5] == 0x77);
    mem_release(other, 4 * PAGE);
    CHECK(writable(other) && (uint8_t)other[3 * PAGE] == 0x77);
    mem_decommit(p + 12 * PAGE, 16 * PAGE);                   // starts inside, ends outside: refused, nothing changes
    CHECK(writable(p + 3 * PAGE));
    munmap(other, 4 * PAGE);

    // release needs the base of a reservation; anything else is ignored; the whole reservation goes whatever size is passed
    mem_release(p + PAGE, 15 * PAGE);
    uint64_t end = 0;
    char detail[160];
    CHECK(!mem_find_free(p, &end, detail, sizeof detail));    // still mapped
    CHECK(writable(p + 3 * PAGE));
    mem_release(p, PAGE);                                     // the base, with a size that is too small
    CHECK(mem_find_free(p, &end, detail, sizeof detail));
    CHECK(mem_find_free(p + 15 * PAGE, &end, detail, sizeof detail));
    mem_count(p, 16 * PAGE, &com, &nc);
    CHECK(com == 0);
    mem_release(p, 16 * PAGE);                                // a second release of the same base: ignored

    // a range taken with mem_reserve_at is a reservation like any other
    char *r = (char *)mem_reserve(32 * PAGE, 64 << 10, false, &e);
    mem_release(r, 32 * PAGE);
    char *at = (char *)mem_reserve_at(r, 32 * PAGE, false);
    CHECK(at == r);
    CHECK(mem_commit(r + 8 * PAGE, 4 * PAGE, &e) && writable(r + 8 * PAGE));
    mem_decommit(r + 8 * PAGE, 4 * PAGE);
    CHECK(!writable(r + 8 * PAGE));
    mem_release(r, 32 * PAGE);
    CHECK(mem_reserve_at(r + 1, PAGE, false) == nullptr);     // not page aligned
    CHECK(mem_reserve_at(r, 0, false) == nullptr);
}

// SNAP_SYNC_FULL / SNAP_WRITE_WATCH: the policy table, on a host that cannot track writes and on one that can
static void test_write_watch_policy() {
    CHECK(write_watch_wanted(true, nullptr, nullptr));        // default on a host that can track
    CHECK(write_watch_wanted(true, "", ""));
    CHECK(write_watch_wanted(true, "0", "1"));                // the "off" strings are SYNC_FULL=1 and WRITE_WATCH=0 only
    CHECK(!write_watch_wanted(true, "1", nullptr));           // SNAP_SYNC_FULL=1: full copies
    CHECK(!write_watch_wanted(true, nullptr, "0"));           // SNAP_WRITE_WATCH=0: full copies
    CHECK(!write_watch_wanted(true, "1", "0"));
    CHECK(!write_watch_wanted(false, nullptr, nullptr));      // a host that cannot track (Linux): always plain
    CHECK(!write_watch_wanted(false, "0", "1"));
    CHECK(!write_watch_wanted(write_watch_supported(), nullptr, nullptr));   // this host
}

static void test_profiling() {
    CHECK(current_thread_cycles() > 0);
    volatile uint64_t x = 0;
    const uint64_t a = current_thread_cycles();
    for (int i = 0; i < 20000000; i++) x += (uint64_t)i;
    CHECK(current_thread_cycles() > a);
    struct S { std::vector<uint32_t> tids; bool self = false; } s;
    for_each_thread([](void *c, uint32_t tid, uint64_t) { ((S *)c)->tids.push_back(tid); }, &s);
    bool me = false;
    for (uint32_t t : s.tids) if (t == thread_id()) me = true;
    CHECK(me);
    char b[32] = {};
    set_current_thread_name("host-test");
    CHECK(thread_description(thread_id(), b, sizeof b) && std::strcmp(b, "host-test") == 0);
    CHECK(!thread_description(0x7ffffff0, b, sizeof b));

    // the CPU time of another thread: a thread that burns about 120 ms shows at least 80 ms, one that sleeps shows (almost) nothing
    std::atomic<bool> stop{false}, burn_ready{false};
    std::atomic<uint32_t> burn_tid{0}, idle_tid{0};
    std::thread burner([&] {
        burn_tid = thread_id();
        const uint64_t c0 = current_thread_cycles();   // CPU time of this thread, so that sharing one CPU with other threads does not matter
        volatile uint64_t y = 0;
        while (current_thread_cycles() - c0 < 120000000ull) y += 1;
        burn_ready = true;
        while (!stop) sleep_ms(2);
    });
    std::thread idler([&] { idle_tid = thread_id(); while (!stop) sleep_ms(2); });
    while (!burn_ready) sleep_ms(2);
    struct Seen { uint32_t burn, idle; uint64_t burn_ns = 0, idle_ns = 0; } seen{burn_tid.load(), idle_tid.load()};
    for_each_thread([](void *c, uint32_t tid, uint64_t ns) {
        Seen *s2 = (Seen *)c;
        if (tid == s2->burn) s2->burn_ns = ns;
        if (tid == s2->idle) s2->idle_ns = ns;
    }, &seen);
    stop = true;
    burner.join();
    idler.join();
    CHECK(seen.burn_ns >= 80000000ull && seen.burn_ns < 2000000000ull);
    CHECK(seen.idle_ns < 40000000ull);
}

int main(int argc, char **argv) {
    int repeat = 1;
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--repeat")) repeat = std::atoi(argv[i + 1]);
    for (int r = 0; r < repeat; r++) {
        test_memory_forks();     // fork before any helper thread exists
        test_threads_forks();
        test_terminate();
        test_time();
        test_threads();
        test_heap();
        test_memory_basic();
        test_memory_model();
        test_memory_threads();
        test_memory_contract();
        test_write_watch_policy();
        test_profiling();
        if (g_fail.load()) break;
    }
    if (g_fail.load()) { std::fprintf(stderr, "os_host_test: %d check(s) failed\n", g_fail.load()); return 1; }
    std::printf("os_host_test: ok (%d run%s)\n", repeat, repeat == 1 ? "" : "s");
    return 0;
}
