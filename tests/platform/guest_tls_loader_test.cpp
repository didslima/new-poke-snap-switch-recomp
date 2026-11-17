// SPDX-License-Identifier: MIT
// The guest TLS integration in the real host/loader.cpp (stage 4b), compiled and linked on Linux with a stand-in `main` module and no game
// data: loader::new_thread (the adopted main thread's TLS block), loader::init_guest_tls (what host/boot.cpp's hook calls), and the Linux
// hook binding (host/guest_tls_hooks.cpp) driven through the loader's own thunk dispatch. The AOT tables the loader links against and the
// few host observers it calls are stubbed in loader_test_stubs.cpp. What this cannot show: the ELF mapping (load() needs the game's files) and any guest code.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "guest_tls_hooks.h"
#include "loader.h"
#include "os/guest_tls_block.h"
#include "os/os_internal.h"

// the AOT tables and observers loader.cpp links against: tests/platform/loader_test_stubs.cpp
namespace osk {
void fatal(const char *fmt, ...) {   // the one OS-layer function the hook bodies call
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
}  // namespace osk

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(c) do { g_checks++; if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const uint8_t kImage[8] = {0xff, 0xff, 0xff, 0xff, 0x95, 0x23, 0x73, 0x00};
static const size_t kSpan = 0xccb900 + 0x1000;

// a stand-in for main: g_main.base/span are what loader.cpp reads the image from
static std::vector<uint8_t> g_module;
static void set_main(const uint8_t *image) {
    g_module.assign(kSpan, 0);
    if (image) memcpy(g_module.data() + hle::kMainTlsImageVaddr, image, 8);
    loader::g_main.base = (uint64_t)(uintptr_t)g_module.data();
    loader::g_main.span = g_module.size();
}
static int in_child(void (*fn)()) {
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) { fn(); _exit(g_fail.load() ? 1 : 0); }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static bool mapped(uint64_t tp) {
    uint64_t end;
    char d[64];
    return !osk::host::mem_find_free((const void *)(uintptr_t)(tp - hle::kGuestTlsTpOffset), &end, d, sizeof d);
}
static bool block_is_image_and_zeros(uint64_t tp, const uint8_t *img, size_t n) {
    const uint8_t *b = (const uint8_t *)(uintptr_t)(tp - hle::kGuestTlsTpOffset);
    if (img && memcmp(b + 0x8010, img, n)) return false;
    for (size_t i = 0; i < hle::kGuestTlsBlockSize; i++)
        if ((!img || i < 0x8010 || i >= 0x8010 + n) && b[i]) return false;
    return true;
}

// ---- new_thread: the adopted main thread ------------------------------------------------------------------------------------------------
static void child_new_thread_no_main() {   // nothing loaded: the old behaviour, a block of zeros, no crash
    loader::g_main.base = 0;
    loader::g_main.span = 0;
    loader::GuestThread *t = loader::new_thread();
    CHECK(t && t->c->tpidr != 0 && t->c->tpidrro != 0);
    CHECK(block_is_image_and_zeros(t->c->tpidr, nullptr, 0));
    CHECK(osk::gld<uint64_t>(t->c->tpidrro + 0x1f0) == t->c->tpidr);
    // a module offered later is still captured by the next thread
    set_main(kImage);
    loader::GuestThread *u = loader::new_thread();
    CHECK(block_is_image_and_zeros(u->c->tpidr, kImage, 8));
}
static void test_new_thread() {
    set_main(kImage);
    const size_t live0 = osk::guest_tls_blocks_live();
    loader::GuestThread *a = loader::new_thread();
    CHECK(a && a->c);
    const uint64_t tp = a->c->tpidr, tlr = a->c->tpidrro, tt = osk::gld<uint64_t>(tlr + 0x1f8);
    CHECK(tp != 0 && tlr != 0 && tt != 0);
    CHECK(tp % 0x8000 == 0 && (tp - 0x8000) % 0x10000 == 0);               // block 64 KiB aligned, tp in its middle
    CHECK(osk::gld<uint64_t>(tlr + 0x1f0) == tp);                          // TLR+0x1f0 is the thread pointer (an offset of the TLR)
    CHECK(osk::gld<uint64_t>(tp + 0x1f0) == 0);                            // and tp + 0x1f0 is just TLS: zero
    CHECK(block_is_image_and_zeros(tp, kImage, 8));
    CHECK(osk::guest_tls_block_owner(tp) == tt);                           // owned by this thread's ThreadType
    CHECK(osk::guest_tls_blocks_live() == live0 + 1);
    CHECK(a->c->sp >= a->stack_lo && a->c->sp <= a->stack_hi);

    // the capture is once: main's bytes change (the game writes its TLS source), the next thread still gets the original image
    memset(g_module.data() + hle::kMainTlsImageVaddr, 0xEE, 8);
    loader::GuestThread *b = loader::new_thread();
    CHECK(b->c->tpidr != tp && block_is_image_and_zeros(b->c->tpidr, kImage, 8));
    const hle::GuestTlsImage *img = osk::guest_tls_main_image_if_captured();
    CHECK(img && memcmp(img->data(), kImage, 8) == 0);

    // threads do not share: writes in one block are not seen in the other
    memset((void *)(uintptr_t)(tp + 0x100), 0x5a, 0x200);
    const uint8_t *other = (const uint8_t *)(uintptr_t)(b->c->tpidr + 0x100);
    bool clean = true;
    for (int i = 0; i < 0x200; i++) if (other[i]) clean = false;
    CHECK(clean);

    // several host threads making threads at once: every thread gets its own block, owner and image
    std::vector<uint64_t> tps(8, 0);
    std::vector<std::thread> th;
    for (int i = 0; i < 8; i++)
        th.emplace_back([&, i] { for (int k = 0; k < 20; k++) { loader::GuestThread *t = loader::new_thread(); if (k == 19) tps[i] = t->c->tpidr; else if (!block_is_image_and_zeros(t->c->tpidr, kImage, 8)) tps[i] = 1; } });
    for (auto &x : th) x.join();
    std::set<uint64_t> uniq(tps.begin(), tps.end());
    CHECK(uniq.size() == 8 && !uniq.count(0) && !uniq.count(1));
    for (uint64_t t : tps) CHECK(block_is_image_and_zeros(t, kImage, 8));

}

// ---- init_guest_tls: the entry point host/boot.cpp's hook calls with a block it allocated itself -------------------------------------------------
static void test_init_guest_tls() {
    set_main(kImage);
    loader::GuestThread *first = loader::new_thread();   // the first thread captures the image
    (void)first;
    // a fresh zeroed block, as VirtualAlloc gives it: only the image is written, at tp + 0x10, and nothing else
    void *mem = nullptr;
    CHECK(posix_memalign(&mem, 0x10000, 0x10000) == 0);
    memset(mem, 0, 0x10000);
    memset((uint8_t *)mem + 0x100, 0x33, 16);   // something the caller put there: kept (this entry point does not zero)
    loader::init_guest_tls((uint64_t)(uintptr_t)mem + 0x8000);
    CHECK(memcmp((uint8_t *)mem + 0x8010, kImage, 8) == 0);
    bool kept = true;
    for (int i = 0; i < 16; i++) if (((uint8_t *)mem)[0x100 + i] != 0x33) kept = false;
    CHECK(kept);
    free(mem);
}

// ---- the Linux hook binding, through the loader's thunk dispatch ----------------------------------------------------------------------------------
static void test_hooks_through_thunks() {
    set_main(kImage);
    loader::GuestThread *th = loader::new_thread();
    Cpu *c = th->c;
    uint64_t cell_init = 0, cell_destroy = 0, cell_set = 0x7777;
    OsConfig oc{};
    oc.tp_init_var = (uint64_t)(uintptr_t)&cell_init;
    oc.tp_destroy_var = (uint64_t)(uintptr_t)&cell_destroy;
    CHECK(snap_host::install_guest_tls_hooks(oc) == 2);
    CHECK(cell_init >= THUNK_BASE && cell_init < THUNK_BASE + THUNK_SPAN);
    CHECK(cell_destroy >= THUNK_BASE && cell_destroy < THUNK_BASE + THUNK_SPAN && cell_destroy != cell_init);
    const uint64_t ci = cell_init, cd = cell_destroy;
    CHECK(snap_host::install_guest_tls_hooks(oc) == 2);                      // already bound: left as it is, still reported as bound
    CHECK(cell_init == ci && cell_destroy == cd);
    OsConfig set{};
    set.tp_init_var = (uint64_t)(uintptr_t)&cell_set;                        // a hook someone else installed
    CHECK(snap_host::install_guest_tls_hooks(set) == 1);                     // one variable only: the other hook is missing, and said so
    CHECK(cell_set == 0x7777);
    OsConfig none{};
    CHECK(snap_host::install_guest_tls_hooks(none) == 0);                    // no variables (a game without the symbols): nothing bound, both said

    // a guest thread's TLR as host_entry builds it, then the trampoline's two calls, through the dispatch
    alignas(512) static uint8_t tlr[0x200];
    memset(tlr, 0, sizeof tlr);
    const uint64_t tt = 0x5555000;
    osk::gst<uint64_t>((uint64_t)(uintptr_t)tlr + 0x1f8, tt);
    Cpu saved = *c;
    c->tpidrro = (uint64_t)(uintptr_t)tlr;
    c->tpidr = 0;
    const size_t live0 = osk::guest_tls_blocks_live();
    loader::call_guest(c, cell_init, {tt});                                  // *nnmuslThreadPointerInitializer(ThreadType)
    const uint64_t tp = osk::gld<uint64_t>((uint64_t)(uintptr_t)tlr + 0x1f0);
    CHECK(tp != 0 && c->tpidr == 0);                                         // the hook wrote TLR+0x1f0; tpidr_el0 is host_entry's to set afterwards
    CHECK(block_is_image_and_zeros(tp, kImage, 8));
    CHECK(osk::guest_tls_block_owner(tp) == tt && osk::guest_tls_blocks_live() == live0 + 1);
    loader::call_guest(c, cell_destroy, {tt + 8});                           // another ThreadType: refused
    CHECK(osk::guest_tls_block_owner(tp) == tt && mapped(tp));
    loader::call_guest(c, cell_destroy, {tt});                               // the owner after its TLS destructors
    CHECK(!mapped(tp) && osk::guest_tls_blocks_live() == live0 && osk::gld<uint64_t>((uint64_t)(uintptr_t)tlr + 0x1f0) == 0);
    c->tpidr = tp;                                                           // a Cpu that still holds the pointer loses it with the block
    loader::call_guest(c, cell_init, {tt});
    const uint64_t tp2 = osk::gld<uint64_t>((uint64_t)(uintptr_t)tlr + 0x1f0);
    c->tpidr = tp2;
    loader::call_guest(c, cell_destroy, {tt});
    CHECK(c->tpidr == 0 && !mapped(tp2));
    loader::call_guest(c, cell_destroy, {tt});                               // again: nothing
    CHECK(osk::guest_tls_blocks_live() == live0);
    *c = saved;
}

int main() {
    CHECK(in_child(child_new_thread_no_main) == 0);   // first: the capture is once per process, so nothing may have been captured yet
    test_new_thread();
    test_init_guest_tls();
    test_hooks_through_thunks();
    if (g_fail.load()) { std::fprintf(stderr, "guest_tls_loader_test: %d of %d checks failed\n", g_fail.load(), g_checks.load()); return 1; }
    std::printf("guest_tls_loader_test: ok (%d checks)\n", g_checks.load());
    return 0;
}
