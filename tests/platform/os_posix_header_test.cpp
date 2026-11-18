// Compile-time and run-time check of the neutral OS interface (runtime/os/os_internal.h) on a POSIX host.
//
// os_internal.h is shared by every platform, so the names it declares must not collide with what the platform headers define. The POSIX
// headers below are included FIRST on purpose: <unistd.h> defines R_OK, W_OK, X_OK and F_OK as macros. The result code that is now called
// osk::R_SUCCESS used to be called R_OK, so this file did not compile on Linux (the preprocessor turned the declaration into
// `constexpr uint32_t 4 = 0`). The asserts also pin the Horizon result values the guest sees, so a rename cannot change them.
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>

#include "os/os_internal.h"

// the POSIX macro this test is about is really there (otherwise the include order above would prove nothing)
static_assert(R_OK == 4 && W_OK == 2 && X_OK == 1 && F_OK == 0, "the POSIX access macros are expected from <unistd.h>");

// svc results returned to the guest in x0 (os_kernel.cpp, os_memory.cpp, os_sync.cpp)
static_assert(osk::R_SUCCESS == 0, "Horizon ResultSuccess is 0");
static_assert(osk::R_TIMEOUT == 0xea01u, "svc::ResultTimedOut");
static_assert(osk::R_INVALID_HANDLE == 0xe401u, "svc::ResultInvalidHandle");
static_assert(osk::R_INVALID_STATE == 0xfa01u, "svc::ResultInvalidState");
static_assert(osk::R_INVALID_ENUM_VALUE == 0x7801u, "svc::ResultInvalidEnumValue");
static_assert(osk::R_INVALID_COMBINATION == 0xe001u, "svc::ResultInvalidCombination");
static_assert(osk::NS_INFINITE == INT64_MAX, "infinite timeout");

int main() {
    // HostCtx is the contract between os_thread.cpp and the platform's context switch: the switch reads the first four pointers by offset
    // (docs/linux-port.md, "os_ctx contract"). A Linux implementation must keep them where they are.
    static_assert(offsetof(osk::HostCtx, sp) == 0, "snap_ctx_switch stores the stack pointer at +0");
    static_assert(offsetof(osk::HostCtx, base) == 8 && offsetof(osk::HostCtx, limit) == 16 && offsetof(osk::HostCtx, dealloc) == 24,
                  "the Win64 switch copies the TIB stack fields through +8/+16/+24");
    static_assert(sizeof(void *) == 8, "the layout above assumes LP64 or LLP64 with 8-byte pointers");
    std::puts("os_posix_header_test: ok");
    return 0;
}
