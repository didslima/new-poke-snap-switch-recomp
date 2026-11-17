// SPDX-License-Identifier: MIT
// Stage 5A: tests host/hooks.cpp on Linux with host/linux/hooks_mods_stub.cpp.
// Exercises install_libc_overrides(), setjmp/longjmp hooks, SNAP_MEMLOG atomic/thread_id tracking,
// and verifies that unported mod stubs do not register any overrides.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "loader.h"

extern "C" {
void rt_hook_setjmp(Cpu *c);
void rt_hook_longjmp(Cpu *c);
}
void snap_memev(char k, uint64_t a, uint64_t sz, uint64_t lr);
void snap_memlog_find(uint64_t addr);

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main() {
    // 1. Install libc overrides (also calls the Linux no-op mod stubs).
    loader::install_libc_overrides();

    Thunk *t_memcpy = loader::find_override("memcpy");
    Thunk *t_memmove = loader::find_override("memmove");
    Thunk *t_memset = loader::find_override("memset");
    Thunk *t_strlen = loader::find_override("strlen");
    CHECK(t_memcpy != nullptr && t_memcpy->fn1 != nullptr);
    CHECK(t_memmove != nullptr && t_memmove->fn1 != nullptr);
    CHECK(t_memset != nullptr && t_memset->fn1 != nullptr);
    CHECK(t_strlen != nullptr && t_strlen->fn1 != nullptr);

    // 2. Linux mod stubs must not register or advertise mod overrides.
    CHECK(loader::find_override("_ZN2nn4util8snprintfEPcmPKcz") == nullptr);
    CHECK(loader::find_override("host:lodanim") == nullptr);
    CHECK(loader::thunk_addr("ovr:memcpy") == t_memcpy->addr);

    // 3. Exercise libc leaf overrides via Cpu.
    Cpu c{};
    char dst[32] = {};
    const char src[] = "snap-linux-5a";
    c.x[0] = (uint64_t)(uintptr_t)dst;
    c.x[1] = (uint64_t)(uintptr_t)src;
    c.x[2] = sizeof(src);
    t_memcpy->fn1(&c);
    CHECK(std::strcmp(dst, src) == 0);

    c.x[0] = (uint64_t)(uintptr_t)dst;
    t_strlen->fn1(&c);
    CHECK(c.x[0] == std::strlen(src));

    c.x[0] = (uint64_t)(uintptr_t)dst;
    c.x[1] = 'X';
    c.x[2] = 4;
    t_memset->fn1(&c);
    CHECK(std::memcmp(dst, "XXXX", 4) == 0);

    // 4. Exercise setjmp / longjmp hooks.
    uint64_t jb[22] = {};
    Cpu sc{};
    sc.x[0] = (uint64_t)(uintptr_t)jb;
    sc.x[19] = 0x1122334455667788ull;
    sc.x[29] = 0x7000100000ull;
    sc.x[30] = 0x7100001234ull;
    sc.sp = 0x70000ff000ull;
    rt_hook_setjmp(&sc);
    CHECK(sc.x[0] == 0);

    Cpu lc{};
    lc.x[0] = (uint64_t)(uintptr_t)jb;
    lc.x[1] = 42;
    rt_hook_longjmp(&lc);
    CHECK(lc.resuming == 1);
    CHECK(lc.x[0] == 42);
    CHECK(lc.x[19] == 0x1122334455667788ull);
    CHECK(lc.sp == 0x70000ff000ull);
    CHECK(lc.pc == 0x7100001234ull);

    // 5. Concurrent snap_memev across threads (atomic index + osk::host::thread_id()).
    std::vector<std::thread> ths;
    for (int t = 0; t < 4; t++) {
        ths.emplace_back([t]() {
            for (int i = 0; i < 200; i++)
                snap_memev('m', 0x7000000000ull + (uint64_t)t * 0x1000, 64, 0);
        });
    }
    for (auto &th : ths) th.join();
    snap_memlog_find(0x7000000000ull);

    std::printf("hooks_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
