// SPDX-License-Identifier: MIT
// Win32 host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <cstdlib>

#include "nvn_os_host.h"

namespace nvn::os_host {

int primary_screen_height() {
  return GetSystemMetrics(SM_CYSCREEN);
}

uint64_t monotonic_ns() {
  static const LARGE_INTEGER freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0) return 0;
  const uint64_t q = static_cast<uint64_t>(now.QuadPart);
  const uint64_t f = static_cast<uint64_t>(freq.QuadPart);
  return (q / f) * 1000000000ull + ((q % f) * 1000000000ull) / f;
}

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(GetTickCount64());
}

uint64_t current_thread_id() {
  return static_cast<uint64_t>(GetCurrentThreadId());
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(GetCurrentProcessId());
}

bool set_current_thread_above_normal() {
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
}

bool set_current_thread_below_normal() {
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
  return _aligned_malloc(size, alignment);
}

void aligned_free(void* ptr) {
  _aligned_free(ptr);
}

uint64_t rdtsc() {
  return __rdtsc();
}

void cpu_yield() {
  YieldProcessor();
}

bool readable_addr(const void* ptr, size_t size) {
  if (!size) return true;
  if (!ptr) return false;
  return !IsBadReadPtr(ptr, size);
}

bool thread_cycle_time(uint64_t* out_cycles) {
  if (!out_cycles) return false;
  ULONG64 cyc = 0;
  if (!QueryThreadCycleTime(GetCurrentThread(), &cyc)) return false;
  *out_cycles = static_cast<uint64_t>(cyc);
  return true;
}

}  // namespace nvn::os_host
