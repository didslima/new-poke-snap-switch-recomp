// SPDX-License-Identifier: MIT
// Neutral OS host interface for the shared NVN->Vulkan layer (stage 6E-A).
// Windows: runtime/nvn/nvn_os_host_win32.cpp; Linux: runtime/nvn/linux/nvn_os_host_posix.cpp.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>

namespace nvn::os_host {

int primary_screen_height();

uint64_t monotonic_ns();
uint64_t monotonic_ms();

uint64_t current_thread_id();
uint64_t current_process_id();
bool set_current_thread_above_normal();
bool set_current_thread_below_normal();

void* aligned_alloc(size_t alignment, size_t size);
void aligned_free(void* ptr);

uint64_t rdtsc();
void cpu_yield();
bool readable_addr(const void* ptr, size_t size);
// Returns true and writes the calling thread's hardware cycle counter when supported (Win32 QueryThreadCycleTime).
// Returns false on platforms without an unprivileged per-thread cycle counter (Linux).
bool thread_cycle_time(uint64_t* out_cycles);

inline bool ascii_ieq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

}  // namespace nvn::os_host
