// SPDX-License-Identifier: MIT
// Stage 6E-A Groups 2A & 2B: Linux unit tests for nvn::os_host.
#include "nvn_os_host.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

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

std::string to_hex(const uint8_t d[32]) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = kHex[d[i] >> 4];
    out[i * 2 + 1] = kHex[d[i] & 0xF];
  }
  return out;
}

void test_sha256_vectors() {
  uint8_t digest[32]{};

  // FIPS 180-4 empty message
  nvn::os_host::sha256("", 0, digest);
  CHECK(to_hex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // FIPS 180-4 "abc"
  nvn::os_host::sha256("abc", 3, digest);
  CHECK(to_hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // FIPS 180-4 448-bit (multi-block after padding)
  const char* mb = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  nvn::os_host::sha256(mb, std::strlen(mb), digest);
  CHECK(to_hex(digest) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Binary buffer containing NUL bytes + sha256_2 equivalence
  const uint8_t bin1[] = {0x00, 0x01, 0x00, 0xff, 0x10};
  const uint8_t bin2[] = {0x00, 0x20, 0x30, 0x00};
  std::vector<uint8_t> combined;
  combined.insert(combined.end(), std::begin(bin1), std::end(bin1));
  combined.insert(combined.end(), std::begin(bin2), std::end(bin2));

  uint8_t d_single[32]{}, d_two[32]{};
  nvn::os_host::sha256(combined.data(), combined.size(), d_single);
  nvn::os_host::sha256_2(bin1, sizeof(bin1), bin2, sizeof(bin2), d_two);
  CHECK(std::memcmp(d_single, d_two, 32) == 0);
}

void test_cache_dir_xdg() {
  const char* old_xdg = std::getenv("XDG_CACHE_HOME");
  const char* old_home = std::getenv("HOME");
  const std::string saved_xdg = old_xdg ? old_xdg : "";
  const std::string saved_home = old_home ? old_home : "";

  ::setenv("HOME", "/tmp/snap_test_home", 1);
  ::setenv("XDG_CACHE_HOME", "/tmp/snap_test_xdg", 1);
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_xdg/SnapRecomp");

  ::unsetenv("XDG_CACHE_HOME");
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_home/.cache/SnapRecomp");

  ::setenv("XDG_CACHE_HOME", "", 1);
  CHECK(nvn::os_host::cache_dir() == "/tmp/snap_test_home/.cache/SnapRecomp");

  if (old_xdg) ::setenv("XDG_CACHE_HOME", saved_xdg.c_str(), 1);
  else ::unsetenv("XDG_CACHE_HOME");
  if (old_home) ::setenv("HOME", saved_home.c_str(), 1);
  else ::unsetenv("HOME");
}

void test_filesystem_and_harness() {
  CHECK(nvn::os_host::path_sep() == '/');
  CHECK(!nvn::os_host::temp_dir().empty());
  CHECK(!nvn::os_host::exe_dir().empty());

  const std::string base = nvn::os_host::temp_dir() + "/snap_nvn_test_" + std::to_string(nvn::os_host::current_process_id());
  std::error_code ec;
  std::filesystem::remove_all(base, ec);

  const std::string nested = base + "/a/b/c";
  CHECK(nvn::os_host::ensure_dir(nested));
  CHECK(std::filesystem::is_directory(nested, ec));

  const std::string f1 = nested + "/deadbeef1234.spv";
  const std::string f2 = nested + "/deadbeef9999.spv";
  const std::string f3 = nested + "/other.json";
  const uint8_t payload1[] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x01};
  const uint8_t payload2[] = {0xaa, 0xbb, 0xcc};

  CHECK(nvn::os_host::write_file_bytes(f1, payload1, sizeof(payload1)));
  CHECK(nvn::os_host::write_file_bytes(f2, payload2, sizeof(payload2)));
  CHECK(nvn::os_host::write_file_bytes(f3, "{}", 2));

  CHECK(nvn::os_host::file_exists(f1));
  CHECK(!nvn::os_host::file_exists(nested));  // directory is not a regular file

  int64_t mtime = 0;
  CHECK(nvn::os_host::file_mtime_ns(f1, &mtime));
  CHECK(mtime > 0);

  std::vector<uint8_t> read_back;
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload1));
  CHECK(std::memcmp(read_back.data(), payload1, sizeof(payload1)) == 0);

  const auto spv_matches = nvn::os_host::list_files(nested, "deadbeef", ".spv");
  CHECK(spv_matches.size() == 2);
  const auto json_matches = nvn::os_host::list_files(nested, nullptr, ".json");
  CHECK(json_matches.size() == 1 && json_matches[0] == "other.json");

  // Atomic replace overwrites destination
  const std::string tmp_ok = nested + "/f1.tmp";
  CHECK(nvn::os_host::write_file_bytes(tmp_ok, payload2, sizeof(payload2)));
  CHECK(nvn::os_host::atomic_replace(tmp_ok, f1));
  CHECK(!nvn::os_host::file_exists(tmp_ok));
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload2));
  CHECK(std::memcmp(read_back.data(), payload2, sizeof(payload2)) == 0);

  // Failed atomic replace preserves prior destination file
  CHECK(!nvn::os_host::atomic_replace(nested + "/missing.tmp", f1));
  CHECK(nvn::os_host::read_file_bytes(f1, &read_back));
  CHECK(read_back.size() == sizeof(payload2));

  CHECK(nvn::os_host::remove_file(f1));
  CHECK(!nvn::os_host::file_exists(f1));

  // Explicit false on Linux for ShaderHarness
  CHECK(!nvn::os_host::run_shader_harness("ShaderHarness.exe", f3));

  std::filesystem::remove_all(base, ec);
}

}  // namespace

int main() {
  test_aligned_alloc_free();
  test_ids_and_priorities();
  test_monotonic_and_rdtsc();
  test_ascii_ieq();
  test_yield_and_readable();
  test_sha256_vectors();
  test_cache_dir_xdg();
  test_filesystem_and_harness();
  std::printf("nvn_os_host_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
