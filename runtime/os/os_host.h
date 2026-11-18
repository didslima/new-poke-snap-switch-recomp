// SPDX-License-Identifier: MIT
// The host services the OS layer (os_kernel.cpp, os_memory.cpp, os_thread.cpp, perf.cpp) needs from the platform, and nothing else.
// One implementation per platform: runtime/os/os_host_win32.cpp (the Win32 calls these files used directly before, moved unchanged) and
// runtime/os/linux/os_host_posix.cpp. Contract, per-primitive differences and tests: docs/linux-port.md ("Stage 3 part B").
//
// Only primitives with a caller are here. Guest state, scheduling decisions and the guest-visible error codes stay in the OS layer.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace osk {
namespace host {

// ---- time
// A monotonic counter that never goes backwards, in units of 1 / counter_frequency() seconds. Win32: QueryPerformanceCounter and
// QueryPerformanceFrequency. POSIX: CLOCK_MONOTONIC in nanoseconds (frequency 1e9). The guest clock is built from it (os_ticks).
int64_t counter();
int64_t counter_frequency();
// Blocks the calling thread for at least `ns` nanoseconds at the finest resolution the host offers (Win32: a high-resolution waitable
// timer per thread, Sleep as the fallback). ns <= 0 returns at once. Not interrupted by signals.
void sleep_precise_ns(int64_t ns);
// Blocks for at least `ms` milliseconds (Win32 Sleep, which os_init gives 1 ms granularity with timers_init).
void sleep_ms(unsigned ms);
// Asks the host for the finest timer resolution, once, from os_init. Win32: timeBeginPeriod(1). POSIX: nothing to ask for, the monotonic
// clock and clock_nanosleep already run on high-resolution timers; the function exists so that the call site is the same everywhere.
void timers_init();

// ---- threads
uint32_t thread_id();   // id of the calling host thread (Win32 GetCurrentThreadId, POSIX gettid)
struct HostThread;      // opaque handle of a thread made by thread_create_suspended
// Creates a thread that does not run `entry(arg)` until thread_resume. `stack_reserve` bytes of stack are reserved (committed lazily).
// nullptr when the thread cannot be created. The OS layer stores the handle before resuming, so that a thread that ends at once is joined.
HostThread *thread_create_suspended(size_t stack_reserve, void (*entry)(void *), void *arg);
void thread_resume(HostThread *t);
// Waits until the thread's entry has returned and releases the handle (the handle is invalid afterwards).
void thread_join(HostThread *t);
// Names the calling thread for debuggers and profilers. The platform limit applies: Win32 about 38 characters, POSIX 15 (longer names
// are cut). Best effort: a host that cannot name threads ignores the call.
void set_current_thread_name(const char *name);
void yield();   // let another ready host thread run (SwitchToThread / sched_yield)
// Ends the process at once, without static destructors or atexit handlers, and with the other threads stopped (TerminateProcess / _exit).
[[noreturn]] void terminate_process();
unsigned long last_error();   // the platform error of the last failed host call on this thread (GetLastError / errno), for messages only

// ---- profiling (perf.cpp, only with SNAP_PERF)
// CPU time of the calling thread in the unit the frame log calls "mcycles". Win32: QueryThreadCycleTime (cycles). POSIX:
// CLOCK_THREAD_CPUTIME_ID in nanoseconds (the log column is a relative load figure; the unit differs by platform).
uint64_t current_thread_cycles();
// Calls fn(ctx, tid, cpu_ns) once for every thread of the process, with the CPU time (user + kernel) it has used so far in nanoseconds.
void for_each_thread(void (*fn)(void *ctx, uint32_t tid, uint64_t cpu_ns), void *ctx);
// The name a thread gave itself (set_current_thread_name), UTF-8, "" or untouched when it has none. false when the thread cannot be read.
bool thread_description(uint32_t tid, char *out, size_t cap);

// ---- heap
// Memory for guest-visible objects (Cpu, thread locals, fibers) with the alignment the guest layout needs. nullptr on failure.
void *heap_aligned_alloc(size_t size, size_t align);
void heap_aligned_free(void *p);

// ---- virtual memory (the guest's address space: guest addresses are host addresses)
// A reservation is address space with no access. Committing a range makes it read/write and zero-filled; decommitting returns it to
// no access and drops its contents; releasing returns the address space. Sizes and addresses are multiples of the page size (4 KiB).
struct MemErr {
    unsigned long code = 0;   // the platform error, for messages
    bool transient = false;   // memory pressure that may pass: the VAMM retries after a pause (Win32: commit limit, not enough memory, ...)
};
// Reserves `size` bytes at a base aligned to `align` (a power of two); the base is never aligned to less than 64 KiB, the Windows
// allocation granularity the guest sizes were tuned for. `write_watch` asks for a reservation GetWriteWatch can track and is honoured only
// where write_watch_supported(); elsewhere the reservation is plain (the NVN layer then copies whole ranges: SNAP_SYNC_FULL=1 behaviour).
// nullptr on failure, with *err filled.
void *mem_reserve(size_t size, size_t align, bool write_watch, MemErr *err);
bool write_watch_supported();
// Commits [addr, addr + size) of a reservation (the range is rounded outwards to whole pages, as VirtualAlloc does). Committing committed pages
// is legal and keeps their contents. false on failure (*err); a range that is not inside one reservation fails with a non-transient error.
bool mem_commit(void *addr, size_t size, MemErr *err);
// Decommits [addr, addr + size); pages that are not committed are left alone. The contents of committed pages are dropped. A range that is not
// inside one reservation is ignored (VirtualFree fails and changes nothing; the POSIX side must not touch memory it did not reserve).
void mem_decommit(void *addr, size_t size);
// Releases a whole reservation made by mem_reserve or mem_reserve_at: `addr` is its base (anything else is ignored, as MEM_RELEASE fails) and
// `size` the size passed when it was made; the whole reservation goes whatever `size` says.
void mem_release(void *addr, size_t size);
// How many bytes of [addr, addr + size) are committed and how many are not (reserved, or not part of any reservation).
void mem_count(const void *addr, size_t size, size_t *committed, size_t *not_committed);
// Save states only (os_memory.cpp state_mem_*): reserves exactly [addr, addr + size), nullptr when the range is taken.
void *mem_reserve_at(void *addr, size_t size, bool write_watch);
// Whether the page at `addr` is free address space (nothing mapped or reserved). If so *region_end is the end of the free span that starts
// there; if not (or the query fails) `detail` gets a short description of what is there for diagnostics, and false is returned.
bool mem_find_free(const void *addr, uint64_t *region_end, char *detail, size_t cap);
// A transient error the memory-pressure fault injection (SNAP_FAULT_VAMM) reports: the platform's own "commit limit" / "out of memory".
MemErr mem_injected_error();
// The lowest and the highest address (inclusive) a process may use.
void mem_app_range(uint64_t *lowest, uint64_t *highest);
// Whether all of [addr, addr + size) can be read right now without faulting (diagnostics that follow guest pointers into stacks that may be gone:
// host/loader.cpp guest_chain). Win32: !IsBadReadPtr. POSIX: process_vm_readv on this process; where that call is refused the answer is false.
bool mem_readable(const void *addr, size_t size);
// Windows-only hardware debug-register watch (SNAP_HWWATCH); no-op on POSIX.
void hwwatch_request(uint64_t q, void (*on_hit)(uint64_t dr6, uint64_t rip));


// Whether guest reservations ask for write tracking: only where the host can track writes, and not when the player turned it off
// (SNAP_SYNC_FULL=1 or SNAP_WRITE_WATCH=0, the strings of the two environment variables, null when unset). A pure function so that the
// whole table can be tested on a host that cannot track writes (os_memory.cpp ww_flag, tests/platform/os_host_test.cpp).
inline bool write_watch_wanted(bool supported, const char *sync_full, const char *write_watch) {
    return supported && !((sync_full && *sync_full == '1') || (write_watch && *write_watch == '0'));
}

}  // namespace host
}  // namespace osk
