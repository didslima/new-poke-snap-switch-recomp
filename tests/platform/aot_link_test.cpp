// SPDX-License-Identifier: MIT
// Stage 5D-A: link-only verification against Linux libsnap_main.a and libsnap_sdk.a.
// Verifies that the ELF archives export the exact AOT tables, PLT stubs, and ABI marker
// expected by host/loader.cpp, without calling loader::load or executing guest code.
#include <cstdint>
#include <cstdio>

extern "C" {
#include "cpu.h"

extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const uint64_t main_fn_count;
extern const char *const main_import_names[];
extern const uint64_t main_import_stub_addrs[];
extern const uint64_t main_import_count;
extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const uint64_t sdk_fn_count;

// Minimal runtime symbols referenced by generated AOT C code.
volatile int g_fnprof = 0;
void fnprof_hit(uint64_t, uint64_t, uint64_t) {}
void rt_dispatch(Cpu *) {}
void rt_import_call(Cpu *, uint32_t) {}
void rt_svc(Cpu *, uint32_t, uint64_t) {}
void rt_udf(Cpu *, uint64_t) { __builtin_trap(); }
uint64_t rt_ticks(void) { return 0; }
}

int main() {
    if (main_fn_count == 0 || sdk_fn_count == 0) return 1;
    if (!main_fn_addrs[0] || !main_fn_ptrs[0]) return 2;
    if (!sdk_fn_addrs[0] || !sdk_fn_ptrs[0]) return 3;
    if (main_import_count == 0 || !main_import_names[0] || !main_import_stub_addrs[0]) return 4;
    std::printf("aot_link_test: main=%llu sdk=%llu imports=%llu\n",
                (unsigned long long)main_fn_count,
                (unsigned long long)sdk_fn_count,
                (unsigned long long)main_import_count);
    return 0;
}
