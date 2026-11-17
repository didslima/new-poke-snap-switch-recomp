// SPDX-License-Identifier: MIT
// Tests of runtime/os/guest_tls_block.{h,cpp} on its own (stage 4b): the lazy one-time capture of main's image, where a block's memory comes from
// and goes back to, ownership of live blocks, and failure with rollback through the memory seam. No scheduler, no loader, no guest code: the
// hook bodies run on Cpu structs the test builds. The OS layer and the loader are tested together with it in guest_tls_os_test and
// guest_tls_loader_test.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "os/guest_tls_block.h"
#include "os/os_internal.h"

namespace osk {
// the OS layer is not linked here: the one function the hook bodies call on an unrecoverable error
void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
}  // namespace osk

using namespace osk;

static std::atomic<int> g_fail{0};
static std::atomic<int> g_checks{0};
#define CHECK(c) do { g_checks++; if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const uint8_t kImage[8] = {0xff, 0xff, 0xff, 0xff, 0x95, 0x23, 0x73, 0x00};
static const size_t kModuleSpan = 0xccb900 + 0x1000;   // a module big enough to hold main's image at its real offset

// a stand-in for the loaded main module, with the image at the offset the game has it
static std::vector<uint8_t> make_module(const uint8_t *image = kImage, uint8_t fill = 0) {
    std::vector<uint8_t> m(kModuleSpan, fill);
    memcpy(m.data() + hle::kMainTlsImageVaddr, image, 8);
    return m;
}

// run fn in a child process (the capture is once per process); returns 0 when the child exited cleanly
static int in_child(void (*fn)()) {
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        fn();
        _exit(g_fail.load() ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

// ---- the lazy one-time capture -----------------------------------------------------------------------------------------------------
static void child_capture_once() {
    CHECK(guest_tls_main_image_if_captured() == nullptr);               // nothing yet
    CHECK(guest_tls_main_image(nullptr, 100) == nullptr);                // no module: no capture, and no latch
    std::vector<uint8_t> small(0x1000, 0);
    CHECK(guest_tls_main_image(small.data(), small.size()) == nullptr);  // a module too small to hold the image
    CHECK(guest_tls_main_image_if_captured() == nullptr);               // failures leave the next call free to capture

    std::vector<uint8_t> module = make_module();
    const hle::GuestTlsImage *img = guest_tls_main_image(module.data(), module.size());
    CHECK(img && img->size() == 8 && memcmp(img->data(), kImage, 8) == 0);
    CHECK(guest_tls_main_image_if_captured() == img);

    // the module changes (the game writes over its TLS source), another module is offered: the first capture stays
    memset(module.data() + hle::kMainTlsImageVaddr, 0xEE, 8);
    std::vector<uint8_t> other = make_module(kImage, 0x11);
    memset(other.data() + hle::kMainTlsImageVaddr, 0x77, 8);
    CHECK(guest_tls_main_image(module.data(), module.size()) == img);
    CHECK(guest_tls_main_image(other.data(), other.size()) == img);
    CHECK(guest_tls_main_image(nullptr, 0) == img);                      // arguments are not looked at once captured
    CHECK(memcmp(img->data(), kImage, 8) == 0);

    // blocks made from it carry the captured bytes
    uint64_t tp = guest_tls_block_create(img, 1);
    CHECK(tp != 0 && memcmp((const void *)(uintptr_t)(tp + 0x10), kImage, 8) == 0);
    CHECK(guest_tls_block_destroy(tp, 1));
}
// many threads race to be the first: one capture wins and everybody sees it
static void child_capture_race() {
    const int T = 16;
    std::vector<std::vector<uint8_t>> modules;
    for (int i = 0; i < T; i++) {
        uint8_t img[8];
        memset(img, 0x10 + i, 8);   // every thread offers a different image
        modules.push_back(make_module(img));
    }
    std::vector<const hle::GuestTlsImage *> got(T, nullptr);
    std::vector<std::vector<uint8_t>> early(T), late(T);   // what each thread read right after its call, and a little later
    std::atomic<int> go{0};
    std::vector<std::thread> th;
    for (int i = 0; i < T; i++)
        th.emplace_back([&, i] {
            while (!go.load()) {}
            got[i] = guest_tls_main_image(modules[i].data(), modules[i].size());
            if (got[i]) early[i].assign(got[i]->data(), got[i]->data() + got[i]->size());
            for (int k = 0; k < 50; k++) std::this_thread::yield();
            if (got[i]) late[i].assign(got[i]->data(), got[i]->data() + got[i]->size());
        });
    go = 1;
    for (auto &t : th) t.join();
    for (int i = 1; i < T; i++) CHECK(got[i] == got[0]);                 // the same object
    for (int i = 0; i < T; i++) CHECK(early[i] == late[i] && early[i] == early[0]);   // and it never changed under anyone: one capture, not several
    CHECK(got[0] && got[0]->size() == 8);
    int matches = 0;
    for (int i = 0; i < T; i++) {
        uint8_t want[8];
        memset(want, 0x10 + i, 8);
        if (memcmp(got[0]->data(), want, 8) == 0) matches++;
    }
    CHECK(matches == 1);                                                 // exactly one thread's image, whole (never a mixture)
}
static void test_capture() {
    CHECK(in_child(child_capture_once) == 0);
    for (int i = 0; i < 60; i++) CHECK(in_child(child_capture_race) == 0);
}

// ---- blocks: memory, ownership, release ---------------------------------------------------------------------------------------------------
static const hle::GuestTlsImage &image() {
    static hle::GuestTlsImage img = [] {
        hle::GuestTlsImage i;
        hle::GuestTlsImage::capture_bytes(kImage, 8, &i);
        return i;
    }();
    return img;
}
static bool mapped(uint64_t tp) {   // is the memory of the block still there?
    uint64_t end;
    char d[64];
    return !host::mem_find_free((const void *)(uintptr_t)(tp - hle::kGuestTlsTpOffset), &end, d, sizeof d);
}
static void test_blocks() {
    const size_t live0 = guest_tls_blocks_live();
    host::MemErr e;
    uint64_t tp = guest_tls_block_create(&image(), 0x1000, &e);
    CHECK(tp != 0);
    const uint64_t base = tp - hle::kGuestTlsTpOffset;
    CHECK(base % 0x10000 == 0);                                          // 64 KiB aligned, like the allocation it replaces
    CHECK(tp - base == 0x8000);
    const uint8_t *b = (const uint8_t *)(uintptr_t)base;
    CHECK(memcmp(b + 0x8010, kImage, 8) == 0);
    bool rest = true;
    for (size_t i = 0; i < 0x10000; i++)
        if ((i < 0x8010 || i >= 0x8018) && b[i]) rest = false;
    CHECK(rest);
    CHECK(guest_tls_blocks_live() == live0 + 1);
    CHECK(guest_tls_block_owner(tp) == 0x1000);

    // the block is read/write over its whole length
    uint8_t *w = (uint8_t *)(uintptr_t)base;
    w[0] = 1;
    w[0xffff] = 2;
    CHECK(w[0] == 1 && w[0xffff] == 2);

    // release: only the owner, only once, only a live tp
    CHECK(!guest_tls_block_destroy(tp, 0x2000));                         // someone else's
    CHECK(mapped(tp) && guest_tls_blocks_live() == live0 + 1);
    CHECK(!guest_tls_block_destroy(tp + 0x10, 0x1000));                  // an interior pointer is not a block
    CHECK(!guest_tls_block_destroy(base, 0x1000));                       // nor is the base of the reservation
    CHECK(!guest_tls_block_destroy(0, 0x1000));
    CHECK(!guest_tls_block_destroy(tp, 0));
    CHECK(mapped(tp));
    CHECK(guest_tls_block_destroy(tp, 0x1000));
    CHECK(!mapped(tp));                                                  // the memory went back
    CHECK(guest_tls_blocks_live() == live0);
    CHECK(!guest_tls_block_destroy(tp, 0x1000));                         // a second release finds nothing
    CHECK(guest_tls_block_owner(tp) == 0);
    CHECK(!guest_tls_block_destroy(0xdeadbeef000, 1));

    // an owner of 0 is not an owner
    CHECK(guest_tls_block_create(&image(), 0) == 0);
    CHECK(guest_tls_blocks_live() == live0);

    // no image: a block of zeros
    uint64_t z = guest_tls_block_create(nullptr, 5);
    CHECK(z != 0);
    const uint8_t *zb = (const uint8_t *)(uintptr_t)(z - 0x8000);
    bool zero = true;
    for (size_t i = 0; i < 0x10000; i++) if (zb[i]) zero = false;
    CHECK(zero);
    CHECK(guest_tls_block_destroy(z, 5));
}

// blocks do not share anything and are released one by one
static void test_isolation_and_leaks() {
    const size_t live0 = guest_tls_blocks_live();
    const int N = 64;
    std::vector<uint64_t> tps;
    for (int i = 0; i < N; i++) {
        uint64_t tp = guest_tls_block_create(&image(), 100 + i);
        CHECK(tp != 0);
        tps.push_back(tp);
    }
    std::set<uint64_t> bases;
    for (uint64_t tp : tps) bases.insert(tp - 0x8000);
    CHECK(bases.size() == (size_t)N);                                    // distinct reservations
    for (int i = 0; i < N; i++) memset((void *)(uintptr_t)(tps[i] + 0x100), 0x40 + i, 0x400);
    for (int i = 0; i < N; i++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)(tps[i] + 0x100);
        bool ok = true;
        for (size_t k = 0; k < 0x400; k++) if (p[k] != (uint8_t)(0x40 + i)) ok = false;
        CHECK(ok);
        CHECK(memcmp((const void *)(uintptr_t)(tps[i] + 0x10), kImage, 8) == 0);
    }
    // release every other block: the rest are untouched and still registered
    for (int i = 0; i < N; i += 2) CHECK(guest_tls_block_destroy(tps[i], 100 + i));
    for (int i = 0; i < N; i++) {
        CHECK(mapped(tps[i]) == (i % 2 == 1));
        CHECK(guest_tls_block_owner(tps[i]) == (i % 2 == 1 ? (uint64_t)(100 + i) : 0));
    }
    CHECK(guest_tls_blocks_live() == live0 + N / 2);
    for (int i = 1; i < N; i += 2) CHECK(guest_tls_block_destroy(tps[i], 100 + i));
    CHECK(guest_tls_blocks_live() == live0);

    // many create/release cycles leave no mappings behind
    auto maps = [] { FILE *f = fopen("/proc/self/maps", "r"); int n = 0; char l[512]; while (f && fgets(l, sizeof l, f)) n++; if (f) fclose(f); return n; };
    const int before = maps();
    for (int i = 0; i < 5000; i++) {
        uint64_t tp = guest_tls_block_create(&image(), 7);
        if (!tp) { CHECK(false); break; }
        if (!guest_tls_block_destroy(tp, 7)) { CHECK(false); break; }
    }
    CHECK(maps() <= before + 2);
    CHECK(guest_tls_blocks_live() == live0);
}

// threads create and release their own blocks; none can release another's
static void test_threads() {
    const size_t live0 = guest_tls_blocks_live();
    const int T = 8;
    std::vector<uint64_t> kept(T, 0);
    std::atomic<int> bad{0}, ready{0}, go{0};
    std::vector<std::thread> th;
    for (int t = 0; t < T; t++)
        th.emplace_back([&, t] {
            const uint64_t me = 1000 + (uint64_t)t;
            for (int i = 0; i < 400; i++) {
                uint64_t tp = guest_tls_block_create(&image(), me);
                if (!tp || memcmp((const void *)(uintptr_t)(tp + 0x10), kImage, 8) != 0) { bad++; continue; }
                if (!guest_tls_block_destroy(tp, me)) bad++;
            }
            kept[t] = guest_tls_block_create(&image(), me);   // one block stays alive while the others try to release it
            ready++;
            while (!go.load()) {}
            // every thread tries to release every other thread's block: refused
            for (int o = 0; o < T; o++)
                if (o != t && guest_tls_block_destroy(kept[o], me)) bad++;
            if (guest_tls_block_destroy(0, me)) bad++;
        });
    while (ready.load() < T) {}
    go = 1;
    for (auto &x : th) x.join();
    CHECK(bad.load() == 0);
    CHECK(guest_tls_blocks_live() == live0 + T);                         // the foreign releases changed nothing
    for (int t = 0; t < T; t++) {
        CHECK(mapped(kept[t]) && guest_tls_block_owner(kept[t]) == 1000 + (uint64_t)t);
        CHECK(guest_tls_block_destroy(kept[t], 1000 + (uint64_t)t));
    }
    CHECK(guest_tls_blocks_live() == live0);
}

// the same block released by many threads at once: exactly one wins
static void test_racing_release() {
    for (int round = 0; round < 200; round++) {
        uint64_t tp = guest_tls_block_create(&image(), 42);
        std::atomic<int> won{0}, go{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; t++)
            th.emplace_back([&] { while (!go.load()) {} if (guest_tls_block_destroy(tp, 42)) won++; });
        go = 1;
        for (auto &x : th) x.join();
        CHECK(won.load() == 1);
        CHECK(!mapped(tp));
    }
}

// ---- failure through the memory seam ------------------------------------------------------------------------------------------------------
struct Counting {
    static inline int reserves, commits, releases, outstanding;
    static inline bool fail_reserve, fail_commit;
    static inline void *last_base;
    static inline size_t last_release_size;
    static void reset() { reserves = commits = releases = outstanding = 0; fail_reserve = fail_commit = false; last_base = nullptr; }
    static void *reserve(size_t size, size_t align, bool ww, host::MemErr *err) {
        reserves++;
        if (fail_reserve) { if (err) { err->code = 12; err->transient = true; } return nullptr; }
        void *p = host::mem_reserve(size, align, ww, err);
        if (p) { outstanding++; last_base = p; }
        return p;
    }
    static bool commit(void *addr, size_t size, host::MemErr *err) {
        commits++;
        if (fail_commit) { if (err) { err->code = 77; err->transient = false; } return false; }
        return host::mem_commit(addr, size, err);
    }
    static void release(void *addr, size_t size) {
        releases++;
        outstanding--;
        last_release_size = size;
        host::mem_release(addr, size);
    }
};
static void test_failures() {
    const GuestTlsMemOps ops = {Counting::reserve, Counting::commit, Counting::release};
    guest_tls_set_mem_ops(&ops);
    const size_t live0 = guest_tls_blocks_live();

    Counting::reset();
    host::MemErr e;
    uint64_t tp = guest_tls_block_create(&image(), 9, &e);   // all works: one reserve, one commit, no release
    CHECK(tp && Counting::reserves == 1 && Counting::commits == 1 && Counting::releases == 0 && Counting::outstanding == 1);
    CHECK(guest_tls_block_destroy(tp, 9));
    CHECK(Counting::releases == 1 && Counting::outstanding == 0);      // the destroy released through the same seam, once
    CHECK(Counting::last_release_size == hle::kGuestTlsBlockSize);     // the whole reservation (a host that honours the size would leak the rest)

    Counting::reset();
    Counting::fail_reserve = true;
    e = host::MemErr{};
    CHECK(guest_tls_block_create(&image(), 9, &e) == 0);
    CHECK(e.code == 12 && e.transient);                                  // the host's error reaches the caller
    CHECK(Counting::reserves == 1 && Counting::commits == 0 && Counting::releases == 0 && Counting::outstanding == 0);
    CHECK(guest_tls_blocks_live() == live0);

    Counting::reset();
    Counting::fail_commit = true;
    e = host::MemErr{};
    CHECK(guest_tls_block_create(&image(), 9, &e) == 0);
    CHECK(e.code == 77 && !e.transient);
    CHECK(Counting::reserves == 1 && Counting::commits == 1);
    CHECK(Counting::releases == 1 && Counting::outstanding == 0);        // the reservation did not outlive the failed commit
    CHECK(Counting::last_release_size == hle::kGuestTlsBlockSize);
    CHECK(Counting::last_base != nullptr);
    uint64_t end;
    char d[64];
    CHECK(host::mem_find_free(Counting::last_base, &end, d, sizeof d));  // and the address space really is free again
    CHECK(guest_tls_blocks_live() == live0);

    // failures do not stick: the next call works
    Counting::reset();
    tp = guest_tls_block_create(&image(), 9);
    CHECK(tp != 0 && Counting::outstanding == 1);
    CHECK(guest_tls_block_destroy(tp, 9) && Counting::outstanding == 0);

    // the hook fatal()s when no block can be made: tested in a child (it aborts the process)
    guest_tls_set_mem_ops(nullptr);
    Counting::reset();
}
static void child_hook_fatal() {
    const GuestTlsMemOps ops = {Counting::reserve, Counting::commit, Counting::release};
    guest_tls_set_mem_ops(&ops);
    Counting::reset();
    Counting::fail_reserve = true;
    alignas(512) uint8_t tlr[0x200] = {};
    Cpu c;
    memset(&c, 0, sizeof c);
    c.tpidrro = (uint64_t)(uintptr_t)tlr;
    c.x[0] = 0x4000;
    guest_tls_hook_init(&c);   // must abort
    _exit(0);
}

// ---- the hook bodies on a Cpu the test builds -----------------------------------------------------------------------------------------------
static void test_hooks() {
    const size_t live0 = guest_tls_blocks_live();
    alignas(512) static uint8_t tlr_a[0x200], tlr_b[0x200];
    memset(tlr_a, 0, sizeof tlr_a);
    memset(tlr_b, 0, sizeof tlr_b);
    Cpu a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.tpidrro = (uint64_t)(uintptr_t)tlr_a;
    b.tpidrro = (uint64_t)(uintptr_t)tlr_b;
    a.x[0] = 0xA000;   // ThreadType of thread a
    b.x[0] = 0xB000;

    // the image is not captured in this process yet (only in the children above): the hook warns and makes a block of zeros
    CHECK(guest_tls_main_image_if_captured() == nullptr);
    guest_tls_hook_init(&a);
    const uint64_t tpa = gld<uint64_t>(a.tpidrro + oslay::TLR_TP);
    CHECK(tpa != 0 && gld<uint64_t>(a.tpidrro + oslay::TLR_THREAD) == 0);   // the hook writes TLR+0x1f0 (relative to the TLR) and nothing else
    CHECK(a.tpidr == 0);                                                    // tpidr_el0 is the caller's to set, after the hook
    CHECK(guest_tls_block_owner(tpa) == 0xA000);
    CHECK(gld<uint32_t>(tpa + 0x10) == 0);                                  // no image captured: zeros
    for (size_t i = 0; i < sizeof tlr_a; i++)
        if (i < oslay::TLR_TP || i >= oslay::TLR_TP + 8) CHECK(tlr_a[i] == 0);   // no other byte of the TLR changed

    // with the image captured: threads started afterwards get it
    std::vector<uint8_t> module = make_module();
    const hle::GuestTlsImage *img = guest_tls_main_image(module.data(), module.size());
    CHECK(img != nullptr);
    guest_tls_hook_init(&b);
    const uint64_t tpb = gld<uint64_t>(b.tpidrro + oslay::TLR_TP);
    CHECK(tpb != 0 && tpb != tpa && memcmp((const void *)(uintptr_t)(tpb + 0x10), kImage, 8) == 0);
    CHECK(guest_tls_block_owner(tpb) == 0xB000);

    // a second init on the same TLR does nothing: no second block, the word is unchanged
    const size_t live = guest_tls_blocks_live();
    guest_tls_hook_init(&b);
    CHECK(guest_tls_blocks_live() == live && gld<uint64_t>(b.tpidrro + oslay::TLR_TP) == tpb);

    // destroy: another thread's ThreadType cannot release this block
    Cpu wrong = b;
    wrong.x[0] = 0xA000;
    guest_tls_hook_destroy(&wrong);
    CHECK(guest_tls_block_owner(tpb) == 0xB000 && gld<uint64_t>(b.tpidrro + oslay::TLR_TP) == tpb);
    // a TLR that holds a pointer nobody made: nothing is released and the word stays
    alignas(512) static uint8_t tlr_c[0x200];
    memset(tlr_c, 0, sizeof tlr_c);
    gst<uint64_t>((uint64_t)(uintptr_t)tlr_c + oslay::TLR_TP, 0x123450000ull);
    Cpu c;
    memset(&c, 0, sizeof c);
    c.tpidrro = (uint64_t)(uintptr_t)tlr_c;
    c.x[0] = 0xC000;
    guest_tls_hook_destroy(&c);
    CHECK(guest_tls_blocks_live() == live);
    // the owner releases, the TLR word is cleared, a second call finds nothing
    guest_tls_hook_destroy(&b);
    CHECK(!mapped(tpb) && gld<uint64_t>(b.tpidrro + oslay::TLR_TP) == 0 && guest_tls_blocks_live() == live - 1);
    guest_tls_hook_destroy(&b);
    CHECK(guest_tls_blocks_live() == live - 1);
    guest_tls_hook_destroy(&a);
    CHECK(guest_tls_blocks_live() == live0);
    Cpu none;
    memset(&none, 0, sizeof none);
    guest_tls_hook_init(&none);      // no TLR: refused, no crash
    guest_tls_hook_destroy(&none);
    CHECK(guest_tls_blocks_live() == live0);
}

int main() {
    test_capture();            // children: the capture is once per process
    test_blocks();
    test_isolation_and_leaks();
    test_threads();
    test_racing_release();
    test_failures();
    CHECK(in_child(child_hook_fatal) == 128 + SIGABRT);
    test_hooks();              // last: it captures the image in this process
    if (g_fail.load()) { std::fprintf(stderr, "guest_tls_block_test: %d of %d checks failed\n", g_fail.load(), g_checks.load()); return 1; }
    std::printf("guest_tls_block_test: ok (%d checks)\n", g_checks.load());
    return 0;
}
