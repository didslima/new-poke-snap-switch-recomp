// SPDX-License-Identifier: MIT
// POSIX (Linux) host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#if !defined(__linux__)
#error "runtime/nvn/linux/nvn_os_host_posix.cpp is the Linux implementation of nvn_os_host.h"
#endif

#include "../nvn_os_host.h"

#include <chrono>
#include <cstdlib>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace nvn::os_host {

int primary_screen_height() {
  return 0;
}

uint64_t monotonic_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t current_thread_id() {
#if defined(SYS_gettid)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
  return static_cast<uint64_t>(::pthread_self());
#endif
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(::getpid());
}

bool set_current_thread_above_normal() {
  // May fail with EACCES/EPERM when unprivileged without RLIMIT_NICE/CAP_SYS_NICE; never fatal.
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), -2) == 0;
}

bool set_current_thread_below_normal() {
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), 5) == 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
  if (size == 0) size = 1;
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size) != 0) return nullptr;
  return ptr;
}

void aligned_free(void* ptr) {
  std::free(ptr);
}

uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return monotonic_ns();
#endif
}

void cpu_yield() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

bool readable_addr(const void* ptr, size_t size) {
  if (!size) return true;
  if (!ptr) return false;
  char buf[4096];
  const char* p = static_cast<const char*>(ptr);
  const pid_t self = ::getpid();
  while (size) {
    const size_t n = size < sizeof(buf) ? size : sizeof(buf);
    iovec l{buf, n};
    iovec r{const_cast<char*>(p), n};
    if (::process_vm_readv(self, &l, 1, &r, 1, 0) != static_cast<ssize_t>(n)) return false;
    p += n;
    size -= n;
  }
  return true;
}

bool thread_cycle_time(uint64_t* out_cycles) {
  if (out_cycles) *out_cycles = 0;
  // Linux does not expose an unprivileged per-thread hardware cycle counter equivalent to Win32 QueryThreadCycleTime.
  return false;
}

}  // namespace nvn::os_host
