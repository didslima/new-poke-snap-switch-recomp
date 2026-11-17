// SPDX-License-Identifier: MIT
// Win32 implementation of runtime/os/os_host.h. The calls below are the ones os_kernel.cpp, os_memory.cpp, os_thread.cpp and perf.cpp
// made directly before the interface existed; each function keeps the arguments, the error handling and the order of calls of the code it
// replaced. The POSIX implementation is runtime/os/linux/os_host_posix.cpp.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <malloc.h>
#include <process.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "os_host.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace osk {
namespace host {

// ---- time
int64_t counter() {
    LARGE_INTEGER x;
    QueryPerformanceCounter(&x);
    return (int64_t)x.QuadPart;
}
int64_t counter_frequency() {
    static const int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (int64_t)x.QuadPart; }();
    return f;
}
void sleep_precise_ns(int64_t ns) {
    struct Timer { HANDLE h = nullptr; ~Timer() { if (h) CloseHandle(h); } };
    static thread_local Timer tm;
    if (!tm.h) tm.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (ns > 0) {
        int64_t step = std::min<int64_t>(ns, 1000000000ll * 3600);   // one hour per wait
        if (tm.h) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(step / 100, 1);
            SetWaitableTimer(tm.h, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(tm.h, INFINITE);
        } else {
            Sleep((DWORD)std::max<int64_t>(step / 1000000, 1));
        }
        ns -= step;
    }
}
void sleep_ms(unsigned ms) { Sleep((DWORD)ms); }
void timers_init() {
    // 1 ms system timer resolution for timed waits (SleepConditionVariableSRW); sleeps use high-resolution timers.
    using TB = UINT(WINAPI *)(UINT);
    if (HMODULE m = LoadLibraryW(L"winmm.dll"))
        if (TB tb = (TB)(void *)GetProcAddress(m, "timeBeginPeriod")) tb(1);
}

// ---- threads
uint32_t thread_id() { return (uint32_t)GetCurrentThreadId(); }

namespace {
struct Start { void (*entry)(void *); void *arg; };
unsigned __stdcall thread_main(void *p) {
    const Start s = *(const Start *)p;
    delete (Start *)p;
    s.entry(s.arg);
    return 0;
}
}  // namespace

HostThread *thread_create_suspended(size_t stack_reserve, void (*entry)(void *), void *arg) {
    Start *s = new Start{entry, arg};
    const size_t ss = std::min<size_t>(stack_reserve, 0xfff00000u);
    uintptr_t h = _beginthreadex(nullptr, (unsigned)ss, thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, nullptr);
    if (!h) { delete s; return nullptr; }
    return (HostThread *)h;
}
void thread_resume(HostThread *t) { ResumeThread((HANDLE)t); }
void thread_join(HostThread *t) {
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
}
void set_current_thread_name(const char *name) {
    using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static Fn fn = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (!fn) return;
    wchar_t w[40] = {};
    size_t i = 0;
    for (; name && name[i] && i < 39; i++) w[i] = (wchar_t)(unsigned char)name[i];
    w[i] = 0;
    fn(GetCurrentThread(), w);
}
void yield() { SwitchToThread(); }
[[noreturn]] void terminate_process() {
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) Sleep(1000);   // TerminateProcess does not return
}
unsigned long last_error() { return (unsigned long)GetLastError(); }

// ---- profiling
uint64_t current_thread_cycles() {
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    return (uint64_t)cyc;
}
void for_each_thread(void (*fn)(void *ctx, uint32_t tid, uint64_t cpu_ns), void *ctx) {
    static std::map<DWORD, HANDLE> h;   // tid -> handle, kept between samples (only the profiler's sampler thread calls this)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    std::map<DWORD, bool> seen;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            seen[te.th32ThreadID] = true;
            auto it = h.find(te.th32ThreadID);
            if (it == h.end()) {
                HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                it = h.emplace(te.th32ThreadID, th).first;
            }
            FILETIME c, e, k, u;
            if (!GetThreadTimes(it->second, &c, &e, &k, &u)) continue;
            const uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);   // 100 ns
            fn(ctx, (uint32_t)te.th32ThreadID, cpu * 100);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    for (auto it = h.begin(); it != h.end();) {
        if (!seen.count(it->first)) { CloseHandle(it->second); it = h.erase(it); } else ++it;
    }
}
bool thread_description(uint32_t tid, char *out, size_t cap) {
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!th) return false;
    using Fn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static Fn gd = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    wchar_t *d = nullptr;
    if (gd && SUCCEEDED(gd(th, &d)) && d) {
        if (*d && out && cap) snprintf(out, cap, "%ls", d);
        LocalFree(d);
    }
    CloseHandle(th);
    return true;
}

// ---- heap
void *heap_aligned_alloc(size_t size, size_t align) { return _aligned_malloc(size, align); }
void heap_aligned_free(void *p) { _aligned_free(p); }

// ---- virtual memory
namespace {
struct VaAddrReq { PVOID lo, hi; SIZE_T align; };
struct VaExtParam { DWORD64 type; PVOID ptr; };   // MEM_EXTENDED_PARAMETER: type 1 = MemExtendedParameterAddressRequirements
bool transient_error(DWORD err) {
    return err == ERROR_COMMITMENT_LIMIT || err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY || err == ERROR_NO_SYSTEM_RESOURCES ||
           err == ERROR_COMMITMENT_MINIMUM;
}
void fill(MemErr *e, DWORD err) {
    if (!e) return;
    e->code = err;
    e->transient = transient_error(err);
}
}  // namespace

bool write_watch_supported() { return true; }

void *mem_reserve(size_t size, size_t align, bool write_watch, MemErr *err) {
    const DWORD flags = MEM_RESERVE | (write_watch ? (DWORD)MEM_WRITE_WATCH : 0u);
    void *p = nullptr;
    if (align <= (64ull << 10)) {
        p = VirtualAlloc(nullptr, size, flags, PAGE_NOACCESS);
    } else {
        typedef PVOID(WINAPI * VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, VaExtParam *, ULONG);
        static const VA2 va2 = [] { HMODULE m = GetModuleHandleA("kernelbase.dll"); return m ? (VA2)(void *)GetProcAddress(m, "VirtualAlloc2") : nullptr; }();
        if (va2) {
            VaAddrReq req = {nullptr, nullptr, (SIZE_T)align};
            VaExtParam ep = {1, &req};
            p = va2(GetCurrentProcess(), nullptr, size, flags, PAGE_NOACCESS, &ep, 1);
        } else {
            for (int tries = 0; tries < 16 && !p; tries++) {   // no VirtualAlloc2: reserve size+align, release, re-reserve at the aligned address
                void *q = VirtualAlloc(nullptr, size + align, MEM_RESERVE, PAGE_NOACCESS);
                if (!q) break;
                const uint64_t at = ((uint64_t)(uintptr_t)q + align - 1) & ~(uint64_t)(align - 1);
                VirtualFree(q, 0, MEM_RELEASE);
                p = VirtualAlloc((void *)(uintptr_t)at, size, flags, PAGE_NOACCESS);
            }
        }
    }
    if (!p) fill(err, GetLastError());
    return p;
}
bool mem_commit(void *addr, size_t size, MemErr *err) {
    if (VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE)) return true;
    fill(err, GetLastError());
    return false;
}
void mem_decommit(void *addr, size_t size) { VirtualFree(addr, size, MEM_DECOMMIT); }
void mem_release(void *addr, size_t) { VirtualFree(addr, 0, MEM_RELEASE); }
void mem_count(const void *addr, size_t size, size_t *committed, size_t *not_committed) {
    size_t com = 0, not_com = 0;
    const uint64_t a = (uint64_t)(uintptr_t)addr;
    for (uint64_t p = a; p < a + size;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) break;
        const uint64_t end = std::min<uint64_t>(a + size, (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize);
        if (mi.State == MEM_COMMIT) com += end - p;
        else not_com += end - p;
        p = end;
    }
    if (committed) *committed = com;
    if (not_committed) *not_committed = not_com;
}
void *mem_reserve_at(void *addr, size_t size, bool write_watch) {
    return VirtualAlloc(addr, size, MEM_RESERVE | (write_watch ? (DWORD)MEM_WRITE_WATCH : 0u), PAGE_NOACCESS);
}
bool mem_find_free(const void *addr, uint64_t *region_end, char *detail, size_t cap) {
    MEMORY_BASIC_INFORMATION mi = {};
    const bool queried = VirtualQuery(addr, &mi, sizeof mi) != 0;
    if (queried && mi.State == MEM_FREE) {
        if (region_end) *region_end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        return true;
    }
    if (detail && cap)
        snprintf(detail, cap, "state %lx type %lx alloc base %llx size %llx", (unsigned long)mi.State, (unsigned long)mi.Type,
                 (unsigned long long)(uintptr_t)mi.AllocationBase, (unsigned long long)mi.RegionSize);
    return false;
}
MemErr mem_injected_error() {
    MemErr e;
    e.code = ERROR_COMMITMENT_LIMIT;
    e.transient = true;
    SetLastError(ERROR_COMMITMENT_LIMIT);
    return e;
}
void mem_app_range(uint64_t *lowest, uint64_t *highest) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (lowest) *lowest = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress;
    if (highest) *highest = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;
}
bool mem_readable(const void *addr, size_t size) { return !IsBadReadPtr(addr, size); }

}  // namespace host
}  // namespace osk
