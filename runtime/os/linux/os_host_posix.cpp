// SPDX-License-Identifier: MIT
// POSIX (Linux x86-64) implementation of runtime/os/os_host.h. The Win32 implementation is runtime/os/os_host_win32.cpp; where the two
// differ, os_host.h and docs/linux-port.md ("Stage 3 part B") say how.
#if !defined(__linux__)
#error "runtime/os/linux/os_host_posix.cpp is the Linux implementation of os_host.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "../os_host.h"

namespace osk {
namespace host {

// ---- time
int64_t counter() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
int64_t counter_frequency() { return 1000000000ll; }

static void sleep_ns_abs_loop(int64_t ns) {
    // Relative sleeps restart from the remaining time after a signal, so a handler cannot shorten the wait.
    timespec req{(time_t)(ns / 1000000000ll), (long)(ns % 1000000000ll)}, rem;
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem) == EINTR) req = rem;
}
void sleep_precise_ns(int64_t ns) {
    if (ns > 0) sleep_ns_abs_loop(ns);
}
void sleep_ms(unsigned ms) { sleep_ns_abs_loop((int64_t)ms * 1000000ll); }
void timers_init() {}   // see os_host.h: the monotonic clock and clock_nanosleep are high-resolution already

// ---- threads
uint32_t thread_id() { return (uint32_t)syscall(SYS_gettid); }

struct HostThread {
    pthread_t th;
    void (*entry)(void *);
    void *arg;
    std::mutex m;
    std::condition_variable cv;
    bool go = false;   // set by thread_resume; the thread waits for it before it runs entry
};

static void *thread_main(void *p) {
    HostThread *t = (HostThread *)p;
    {
        std::unique_lock<std::mutex> lk(t->m);
        t->cv.wait(lk, [t] { return t->go; });
    }
    t->entry(t->arg);
    return nullptr;
}

HostThread *thread_create_suspended(size_t stack_reserve, void (*entry)(void *), void *arg) {
    HostThread *t = new HostThread;
    t->entry = entry;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t ss = (stack_reserve + page - 1) & ~(page - 1);
    if (ss < (size_t)PTHREAD_STACK_MIN) ss = (size_t)PTHREAD_STACK_MIN;
    pthread_attr_setstacksize(&attr, ss);
    const int rc = pthread_create(&t->th, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        delete t;
        return nullptr;
    }
    return t;
}
void thread_resume(HostThread *t) {
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->go = true;
    }
    t->cv.notify_one();
}
void thread_join(HostThread *t) {
    pthread_join(t->th, nullptr);
    delete t;
}
void set_current_thread_name(const char *name) {
    char b[16];   // the kernel limit: 15 characters and the terminator
    size_t i = 0;
    for (; name && name[i] && i < 15; i++) b[i] = name[i];
    b[i] = 0;
    pthread_setname_np(pthread_self(), b);
}
void yield() { sched_yield(); }
[[noreturn]] void terminate_process() { _exit(0); }
unsigned long last_error() { return (unsigned long)errno; }

// ---- profiling
uint64_t current_thread_cycles() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void for_each_thread(void (*fn)(void *ctx, uint32_t tid, uint64_t cpu_ns), void *ctx) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    const long hz = sysconf(_SC_CLK_TCK);
    while (dirent *e = readdir(d)) {
        const int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char path[64], buf[512];
        // schedstat: the first field is the CPU time of the thread in nanoseconds (no tick granularity); not every kernel has it
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", tid);
        if (FILE *sf = fopen(path, "r")) {
            unsigned long long ns = 0;
            const int got = fscanf(sf, "%llu", &ns);
            fclose(sf);
            if (got == 1) { fn(ctx, (uint32_t)tid, ns); continue; }
        }
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        const size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        // "pid (comm) state ..." : the name may contain spaces and parentheses, so start after the last ')'. Fields 14 and 15 are utime and stime in clock ticks.
        const char *p = strrchr(buf, ')');
        if (!p) continue;
        unsigned long long ut = 0, st = 0;
        if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        fn(ctx, (uint32_t)tid, (ut + st) * (1000000000ull / (unsigned long long)(hz > 0 ? hz : 100)));
    }
    closedir(d);
}
bool thread_description(uint32_t tid, char *out, size_t cap) {
    char path[64], buf[32];
    snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
    if (buf[0] && out && cap) snprintf(out, cap, "%s", buf);
    return true;
}

// ---- heap
void *heap_aligned_alloc(size_t size, size_t align) {
    void *p = nullptr;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, size) == 0 ? p : nullptr;
}
void heap_aligned_free(void *p) { free(p); }

// ---- virtual memory
//
// Windows keeps the state of every page (free, reserved, committed) and refuses a commit or a decommit outside a reservation. Linux has
// neither the notion nor the refusal: mprotect on an unmapped page says ENOMEM (which would read as memory pressure), and mmap with
// MAP_FIXED would replace whatever is mapped there. So this file keeps what Windows keeps, under one mutex:
//   g_reserved   the reservations made here, base -> size (disjoint)
//   g_committed  the committed ranges inside them, begin -> end (disjoint, merged)
// A range that is not inside one reservation is refused (commit) or left alone (decommit, release), as on Windows.
namespace {
std::mutex g_mu;
std::map<uintptr_t, uintptr_t> g_reserved;    // base -> end
std::map<uintptr_t, uintptr_t> g_committed;   // begin -> end

void add_range(uintptr_t b, uintptr_t e) {   // under g_mu
    auto it = g_committed.lower_bound(b);
    if (it != g_committed.begin()) {
        auto p = std::prev(it);
        if (p->second >= b) { b = p->first; e = std::max(e, p->second); it = g_committed.erase(p); }
    }
    while (it != g_committed.end() && it->first <= e) { e = std::max(e, it->second); it = g_committed.erase(it); }
    g_committed[b] = e;
}
void remove_range(uintptr_t b, uintptr_t e) {   // under g_mu
    auto it = g_committed.lower_bound(b);
    if (it != g_committed.begin()) {
        auto p = std::prev(it);
        if (p->second > b) {
            const uintptr_t pe = p->second;
            p->second = b;
            if (pe > e) g_committed[e] = pe;
            it = g_committed.lower_bound(b);
        }
    }
    while (it != g_committed.end() && it->first < e) {
        if (it->second > e) { const uintptr_t ee = it->second; it = g_committed.erase(it); g_committed[e] = ee; break; }
        it = g_committed.erase(it);
    }
}
bool inside_reservation(uintptr_t b, uintptr_t e) {   // under g_mu: [b, e) lies in one reservation
    auto it = g_reserved.upper_bound(b);
    if (it == g_reserved.begin()) return false;
    --it;
    return b >= it->first && e <= it->second;
}
void fill(MemErr *e, int err) {
    if (!e) return;
    e->code = (unsigned long)err;
    e->transient = err == ENOMEM || err == EAGAIN;   // memory pressure: it may pass (an address-space or mapping-count limit is retried too, as on Win32)
}
size_t page_size() {
    static const size_t p = (size_t)sysconf(_SC_PAGESIZE);
    return p;
}
// Windows rounds a commit or decommit range outwards to whole pages; the same here, so that an unaligned caller gets the same pages.
void page_round(const void *addr, size_t size, uintptr_t *b, uintptr_t *e) {
    const uintptr_t pg = page_size();
    *b = (uintptr_t)addr & ~(pg - 1);
    *e = ((uintptr_t)addr + size + pg - 1) & ~(pg - 1);
}
}  // namespace

bool write_watch_supported() { return false; }   // stage 3 decision: full copies first; see docs/linux-port.md

void *mem_reserve(size_t size, size_t align, bool /*write_watch*/, MemErr *err) {
    const size_t page = page_size();
    if (align < (64u << 10)) align = 64u << 10;   // never below the Windows allocation granularity
    if (align < page) align = page;
    if (!size || (size & (page - 1)) || (align & (align - 1))) { fill(err, EINVAL); return nullptr; }
    if (size + align < size) { fill(err, ENOMEM); return nullptr; }
    // Map a PROT_NONE range big enough to contain an aligned base, then give back the head and the tail. MAP_NORESERVE: address space only.
    const size_t span = size + align;
    uint8_t *raw = (uint8_t *)mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == (uint8_t *)MAP_FAILED) { fill(err, errno); return nullptr; }
    const uintptr_t at = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
    if (at > (uintptr_t)raw) munmap(raw, at - (uintptr_t)raw);
    const uintptr_t end = at + size, raw_end = (uintptr_t)raw + span;
    if (raw_end > end) munmap((void *)end, raw_end - end);
    std::lock_guard<std::mutex> lk(g_mu);
    g_reserved[at] = end;
    return (void *)at;
}

bool mem_commit(void *addr, size_t size, MemErr *err) {
    if (!size) return true;
    uintptr_t b, e;
    page_round(addr, size, &b, &e);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!inside_reservation(b, e)) { fill(err, EINVAL); return false; }   // Windows: ERROR_INVALID_ADDRESS, not a transient error
    if (mprotect((void *)b, e - b, PROT_READ | PROT_WRITE) != 0) { fill(err, errno); return false; }
    add_range(b, e);
    return true;
}

void mem_decommit(void *addr, size_t size) {
    if (!size) return;
    uintptr_t b, e;
    page_round(addr, size, &b, &e);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!inside_reservation(b, e)) return;   // Windows: the call fails and changes nothing
    // A fresh anonymous PROT_NONE mapping over the range: the old pages and their contents are gone and later access faults, like a
    // decommitted Windows page; committing it again gives zero-filled pages. The range is ours (inside a reservation), so MAP_FIXED
    // cannot replace anything else.
    void *p = mmap((void *)b, e - b, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {   // not expected for a range we reserved; keep the contract anyway
        madvise((void *)b, e - b, MADV_DONTNEED);
        mprotect((void *)b, e - b, PROT_NONE);
    }
    remove_range(b, e);
}

void mem_release(void *addr, size_t /*size*/) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_reserved.find((uintptr_t)addr);
    if (it == g_reserved.end()) return;   // Windows: MEM_RELEASE needs the base of a reservation; anything else fails and changes nothing
    munmap((void *)it->first, it->second - it->first);   // the whole reservation, whatever size the caller passed (as MEM_RELEASE does)
    remove_range(it->first, it->second);
    g_reserved.erase(it);
}

void mem_count(const void *addr, size_t size, size_t *committed, size_t *not_committed) {
    const uintptr_t b = (uintptr_t)addr, e = b + size;
    size_t com = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_committed.upper_bound(b);
        if (it != g_committed.begin()) --it;
        for (; it != g_committed.end() && it->first < e; ++it) {
            const uintptr_t lo = std::max(b, it->first), hi = std::min(e, it->second);
            if (hi > lo) com += hi - lo;
        }
    }
    if (committed) *committed = com;
    if (not_committed) *not_committed = size - com;
}

void *mem_reserve_at(void *addr, size_t size, bool /*write_watch*/) {
    if (!size || ((uintptr_t)addr & (page_size() - 1)) || (size & (page_size() - 1))) return nullptr;
    // MAP_FIXED_NOREPLACE: fail instead of replacing what is there. A kernel that ignores the flag may answer with another address.
    void *p = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (p != addr) { munmap(p, size); return nullptr; }
    std::lock_guard<std::mutex> lk(g_mu);
    g_reserved[(uintptr_t)p] = (uintptr_t)p + size;
    return p;
}

bool mem_find_free(const void *addr, uint64_t *region_end, char *detail, size_t cap) {
    // /proc/self/maps lists the mappings in address order: "start-end perms ...". The page is free when no mapping contains it; the free
    // span then ends where the next mapping starts (or at the top of the 47-bit user space).
    const uint64_t a = (uint64_t)(uintptr_t)addr;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        if (detail && cap) snprintf(detail, cap, "cannot read /proc/self/maps (errno %d)", errno);
        return false;
    }
    uint64_t next = 0x800000000000ull;   // end of the user address space unless a later mapping starts earlier
    bool found_next = false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) < 2) continue;
        if (a >= s && a < e) {
            if (detail && cap) snprintf(detail, cap, "mapped %llx-%llx %s", s, e, perms);
            fclose(f);
            return false;
        }
        if (s > a && !found_next) { next = s; found_next = true; }
    }
    fclose(f);
    if (region_end) *region_end = next;
    return true;
}

MemErr mem_injected_error() {
    MemErr e;
    e.code = ENOMEM;
    e.transient = true;
    errno = ENOMEM;
    return e;
}

void mem_app_range(uint64_t *lowest, uint64_t *highest) {
    // 47-bit user address space (the x86-64 default without 5-level paging); the lowest page the kernel lets a process map is
    // vm.mmap_min_addr (64 KiB by default), the same value Win32 reports.
    if (lowest) *lowest = 0x10000ull;
    if (highest) *highest = 0x7fffffffffffull;
}

}  // namespace host
}  // namespace osk
