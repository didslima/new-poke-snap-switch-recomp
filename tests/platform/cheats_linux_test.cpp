// SPDX-License-Identifier: MIT
// Unit tests for Linux cheats support (runtime/hle/cheats.cpp + hle::host::mem_range_ok).
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/hle/cheats.h"
#include "runtime/hle/hle_host.h"

struct Cpu;
extern "C" void rt_dispatch(Cpu*) {}

namespace {

int g_fails = 0;
#define CHECK(cond)                                                                              \
  do {                                                                                           \
    if (!(cond)) {                                                                               \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++g_fails;                                                                                 \
    }                                                                                            \
  } while (0)

}  // namespace

int main() {
  const size_t pg = static_cast<size_t>(::sysconf(_SC_PAGESIZE));

  // 1. Memory-access constraints: RW, RO, PROT_NONE, unmapped
  void* rw = ::mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* ro = ::mmap(nullptr, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* no = ::mmap(nullptr, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(rw != MAP_FAILED && ro != MAP_FAILED && no != MAP_FAILED);

  CHECK(hle::host::mem_range_ok(reinterpret_cast<uint64_t>(rw), 8, false));
  CHECK(hle::host::mem_range_ok(reinterpret_cast<uint64_t>(rw), 8, true));
  CHECK(hle::host::mem_range_ok(reinterpret_cast<uint64_t>(ro), 8, false));
  CHECK(!hle::host::mem_range_ok(reinterpret_cast<uint64_t>(ro), 8, true));
  CHECK(!hle::host::mem_range_ok(reinterpret_cast<uint64_t>(no), 8, false));
  CHECK(!hle::host::mem_range_ok(reinterpret_cast<uint64_t>(no), 8, true));
  CHECK(!hle::host::mem_range_ok(0, 8, false));
  CHECK(!hle::host::mem_range_ok(0x1000, 8, false));

  // 2. Cheat parsing: invalid syntax returns error
  std::string err;
  CHECK(!hle::cheats::test_load("[Bad]\nnot_hex_opcode\n", &err));
  CHECK(!err.empty());

  // 3. Valid cheat: write 0x12345678 to [HEAP + 0x10] when A button (bit 0) is held
  auto* heap = static_cast<uint8_t*>(rw);
  std::memset(heap, 0, pg);
  const std::string cheat_txt =
      "[Test Write]\n"
      "80000001\n"           // if key A (1) held
      "04100000 00000010 12345678\n"   // write 32-bit 0x12345678 to HEAP + 0x10
      "20000000\n";          // end if

  err.clear();
  CHECK(hle::cheats::test_load(cheat_txt, &err));
  CHECK(err.empty());
  CHECK(hle::cheats::entries().size() == 1);
  hle::cheats::set_master(true);
  hle::cheats::set_on(0, true);

  // Without A button: value stays 0
  hle::cheats::test_hooks(reinterpret_cast<uint64_t>(heap), 0);
  CHECK(hle::cheats::test_run_once());
  uint32_t val = 0;
  std::memcpy(&val, heap + 0x10, 4);
  CHECK(val == 0);

  // With A button: value becomes 0x12345678
  hle::cheats::test_hooks(reinterpret_cast<uint64_t>(heap), 1);
  CHECK(hle::cheats::test_run_once());
  std::memcpy(&val, heap + 0x10, 4);
  CHECK(val == 0x12345678u);

  // 4. Write to read-only heap fails safely without SIGSEGV
  hle::cheats::test_hooks(reinterpret_cast<uint64_t>(ro), 1);
  CHECK(!hle::cheats::test_run_once());

  hle::cheats::set_master(false);
  ::munmap(rw, pg);
  ::munmap(ro, pg);
  ::munmap(no, pg);
  if (g_fails) return 1;
  std::printf("cheats_linux_test: all checks passed\n");
  return 0;
}
