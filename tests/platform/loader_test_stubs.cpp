// SPDX-License-Identifier: MIT
// What host/loader.cpp links against that no loader test needs: the AOT tables of the lifted modules (empty here: no function, no import) and the
// host observers and crash log it calls. Linked by the tests that run the real loader dispatch (guest_tls_loader, tls_host_fibers).
#include <cstdint>
#include <cstdio>

extern "C" {
struct Cpu;
typedef void (*GuestFn)(Cpu *);
extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const uint64_t main_fn_count;
extern const char *const main_import_names[];
extern const uint64_t main_import_stub_addrs[];
extern const uint64_t main_import_count;
extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const uint64_t sdk_fn_count;
const uint64_t main_fn_addrs[1] = {0};
const GuestFn main_fn_ptrs[1] = {nullptr};
const uint64_t main_fn_count = 0;
const char *const main_import_names[1] = {""};
const uint64_t main_import_stub_addrs[1] = {0};
const uint64_t main_import_count = 0;
const uint64_t sdk_fn_addrs[1] = {0};
const GuestFn sdk_fn_ptrs[1] = {nullptr};
const uint64_t sdk_fn_count = 0;
void snap_crashlog_fatal(const char *kind, const char *detail) { fprintf(stderr, "[stub] fatal %s: %s\n", kind, detail); }
void snap_fn_observe(uint64_t, uint64_t, uint64_t) {}
void snap_skips_observe(uint64_t, uint64_t, uint64_t) {}
void snap_neo_observe(uint64_t, uint64_t, uint64_t) {}
void snap_highfps_observe(uint64_t, uint64_t, uint64_t) {}
}
