// SPDX-License-Identifier: MIT
// Stage 5D-A2: minimal link verification against the real ABI 3 libsnap_sdk.a (36,831 functions).
// Validates that sdk_fn_addrs, sdk_fn_ptrs, sdk_fn_count, and snap_runtime_abi_3 link cleanly
// without executing any guest code or calling loader::load().
#include <cstdint>
#include <cstdio>

extern "C" {
#include "cpu.h"

extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const uint64_t sdk_fn_count;

volatile int g_fnprof = 0;
void fnprof_hit(uint64_t, uint64_t, uint64_t) {}
void rt_dispatch(Cpu *) {}
void rt_svc(Cpu *, uint32_t, uint64_t) {}
void rt_udf(Cpu *, uint64_t) { __builtin_trap(); }
uint64_t rt_ticks(void) { return 0; }
void rt_hook_get_exception_info(Cpu *) {}
void rt_hook_longjmp(Cpu *) {}
void rt_hook_setjmp(Cpu *) {}
void rt_hook_unw_jumpto(Cpu *) {}
}

int main() {
    if (sdk_fn_count == 0 || !sdk_fn_addrs[0] || !sdk_fn_ptrs[0]) return 1;
    std::printf("aot_sdk_real_link_test: sdk_fn_count=%llu first=%#llx\n",
                (unsigned long long)sdk_fn_count, (unsigned long long)sdk_fn_addrs[0]);
    return 0;
}
