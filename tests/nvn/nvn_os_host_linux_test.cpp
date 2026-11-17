// SPDX-License-Identifier: MIT
// Stage 6E-A Group 2A: Linux unit tests for nvn::os_host (time, threads, aligned memory, rdtsc, ascii_ieq).
#include "nvn_os_host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <thread>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* msg, int line) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, msg);
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_aligned_alloc_free() {
  void* p1 = nvn::os_host::aligned_alloc(0x1000, 100);  // size not a multiple of alignment
  CHECK(p1 != nullptr);
  CHECK((reinterpret_cast<uintptr_t>(p1) & 0xFFFull) == 0);
  std::memset(p1, 0xAB, 100);
  nvn::os_host::aligned_free(p1);

  void* p2 = nvn::os_host::aligned_alloc(64, 4096);
  CHECK(p2 != nullptr);
  CHECK((reinterpret_cast<uintptr_t>(p2) & 63ull) == 0);
  nvn::os_host::aligned_free(p2);

  nvn::os_host::aligned_free(nullptr);
}

void test_ids_and_priorities() {
  const uint64_t pid = nvn::os_host::current_process_id();
  const uint64_t main_tid = nvn::os_host::current_thread_id();
  CHECK(pid != 0);
  CHECK(main_tid != 0);

  const int main_prio_before = ::getpriority(PRIO_PROCESS, 0);
  uint64_t worker_tid = 0;
  std::thread th([&] {
    worker_tid = nvn::os_host::current_thread_id();
    // Must never crash even when unprivileged (set_current_thread_above_normal typically returns false).
    (void)nvn::os_host::set_current_thread_below_normal();
    (void)nvn::os_host::set_current_thread_above_normal();
  });
  th.join();
  CHECK(::getpriority(PRIO_PROCESS, 0) == main_prio_before);
  CHECK(worker_tid != 0);
  CHECK(worker_tid != main_tid);
}

void test_monotonic_and_rdtsc() {
  const uint64_t ns0 = nvn::os_host::monotonic_ns();
  const uint64_t ms0 = nvn::os_host::monotonic_ms();
  const uint64_t tsc0 = nvn::os_host::rdtsc();

  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  const uint64_t ns1 = nvn::os_host::monotonic_ns();
  const uint64_t ms1 = nvn::os_host::monotonic_ms();
  const uint64_t tsc1 = nvn::os_host::rdtsc();

  CHECK(ns1 > ns0);
  CHECK(ms1 >= ms0);
  CHECK(tsc1 != tsc0);

  uint64_t cyc = 12345;
  CHECK(!nvn::os_host::thread_cycle_time(&cyc));
  CHECK(cyc == 0);
}

void test_ascii_ieq() {
  CHECK(nvn::os_host::ascii_ieq("enhanced", "ENHANCED"));
  CHECK(nvn::os_host::ascii_ieq("On", "oN"));
  CHECK(!nvn::os_host::ascii_ieq("high", "low"));
  CHECK(!nvn::os_host::ascii_ieq("high", "high2"));
  CHECK(nvn::os_host::ascii_ieq(nullptr, nullptr));
  CHECK(!nvn::os_host::ascii_ieq(nullptr, "a"));
}

void test_yield_and_readable() {
  nvn::os_host::cpu_yield();
  const uint64_t val = 0x123456789abcdef0ull;
  CHECK(nvn::os_host::readable_addr(&val, sizeof(val)));
  CHECK(nvn::os_host::readable_addr(nullptr, 0));
  CHECK(!nvn::os_host::readable_addr(nullptr, 8));
  CHECK(!nvn::os_host::readable_addr(reinterpret_cast<const void*>(0x1000), 8));
}

}  // namespace

int main() {
  test_aligned_alloc_free();
  test_ids_and_priorities();
  test_monotonic_and_rdtsc();
  test_ascii_ieq();
  test_yield_and_readable();
  std::printf("nvn_os_host_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
