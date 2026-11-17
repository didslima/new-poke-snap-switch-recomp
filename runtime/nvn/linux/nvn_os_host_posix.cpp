// SPDX-License-Identifier: MIT
// POSIX (Linux) host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#if !defined(__linux__)
#error "runtime/nvn/linux/nvn_os_host_posix.cpp is the Linux implementation of nvn_os_host.h"
#endif

#include "../nvn_os_host.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Sha256Ctx {
  uint32_t s[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  uint64_t bits = 0;
  uint8_t buf[64] = {};
  size_t len = 0;

  static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t b[64]) {
    static constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) |
             (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], c = s[1], d = s[2], e = s[3], f = s[4], g = s[5], h = s[6], j = s[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(f, 6) ^ rotr(f, 11) ^ rotr(f, 25);
      const uint32_t ch = (f & g) ^ ((~f) & h);
      const uint32_t t1 = j + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & c) ^ (a & d) ^ (c & d);
      const uint32_t t2 = S0 + maj;
      j = h; h = g; g = f; f = e + t1; e = d; d = c; c = a; a = t1 + t2;
    }
    s[0] += a; s[1] += c; s[2] += d; s[3] += e; s[4] += f; s[5] += g; s[6] += h; s[7] += j;
  }

  void update(const void* data, size_t n) {
    if (!data || n == 0) return;
    const auto* p = static_cast<const uint8_t*>(data);
    bits += uint64_t(n) * 8ull;
    while (n > 0) {
      const size_t take = std::min(n, 64 - len);
      std::memcpy(buf + len, p, take);
      len += take;
      p += take;
      n -= take;
      if (len == 64) {
        block(buf);
        len = 0;
      }
    }
  }

  void finish(uint8_t out[32]) {
    buf[len++] = 0x80u;
    if (len > 56) {
      while (len < 64) buf[len++] = 0;
      block(buf);
      len = 0;
    }
    while (len < 56) buf[len++] = 0;
    for (int i = 0; i < 8; ++i) buf[56 + i] = static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xffu);
    block(buf);
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((s[i] >> 24) & 0xffu);
      out[i * 4 + 1] = static_cast<uint8_t>((s[i] >> 16) & 0xffu);
      out[i * 4 + 2] = static_cast<uint8_t>((s[i] >> 8) & 0xffu);
      out[i * 4 + 3] = static_cast<uint8_t>(s[i] & 0xffu);
    }
  }
};

}  // namespace

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

void sleep_ms(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
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

bool set_current_thread_normal() {
  return ::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), 0) == 0;
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

char path_sep() { return '/'; }

std::string cache_dir() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/SnapRecomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.cache/SnapRecomp";
  }
  return "/tmp/SnapRecomp";
}

std::string temp_dir() {
  if (const char* t = std::getenv("TMPDIR"); t && *t) return t;
  return "/tmp";
}

std::string exe_dir() {
  char buf[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  const std::string p(buf, static_cast<size_t>(n));
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(path), ec);
  return !ec;
}

bool file_exists(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(fs::path(path), ec);
}

bool file_mtime_ns(const std::string& path, int64_t* out_ns) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (out_ns) {
    *out_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
              static_cast<int64_t>(st.st_mtim.tv_nsec);
  }
  return true;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  const uintmax_t sz = fs::file_size(fs::path(path), ec);
  if (ec || sz >= (64ull << 20)) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->resize(static_cast<size_t>(sz));
  const bool ok = sz == 0 || std::fread(out->data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz);
  std::fclose(f);
  if (!ok) out->clear();
  return ok;
}

bool write_file_bytes(const std::string& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
  const bool flush_ok = std::fflush(f) == 0;
  const bool close_ok = std::fclose(f) == 0;
  return ok && flush_ok && close_ok;
}

bool remove_file(const std::string& path) {
  std::error_code ec;
  return fs::remove(fs::path(path), ec);
}

bool atomic_replace(const std::string& tmp_path, const std::string& dst_path) {
  std::error_code ec;
  fs::rename(fs::path(tmp_path), fs::path(dst_path), ec);
  return !ec;
}

std::vector<std::string> list_files(const std::string& dir, const char* prefix, const char* suffix) {
  std::vector<std::string> out;
  std::error_code ec;
  const std::string pfx = prefix ? prefix : "";
  const std::string sfx = suffix ? suffix : "";
  for (const auto& entry : fs::directory_iterator(fs::path(dir), ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() >= pfx.size() + sfx.size() &&
        name.compare(0, pfx.size(), pfx) == 0 &&
        name.compare(name.size() - sfx.size(), sfx.size(), sfx) == 0) {
      out.push_back(name);
    }
  }
  return out;
}

void sha256_2(const void* d1, size_t n1, const void* d2, size_t n2, uint8_t out[32]) {
  Sha256Ctx ctx;
  ctx.update(d1, n1);
  ctx.update(d2, n2);
  ctx.finish(out);
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
  sha256_2(data, len, nullptr, 0, out);
}

bool run_shader_harness(const std::string&, const std::string&) {
  // ShaderHarness.exe is not available on Linux; report unavailable explicitly.
  return false;
}

bool run_process_capture(const std::string& cmd, std::string* out_output, int* out_exit_code) {
  std::string full = cmd + " 2>&1";
  FILE* p = ::popen(full.c_str(), "r");
  if (!p) {
    if (out_output) *out_output = "cannot start process";
    if (out_exit_code) *out_exit_code = -1;
    return false;
  }
  std::string msg;
  char buf[512];
  while (const size_t n = std::fread(buf, 1, sizeof(buf), p)) {
    msg.append(buf, n);
  }
  const int st = ::pclose(p);
  const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  if (out_output) *out_output = std::move(msg);
  if (out_exit_code) *out_exit_code = code;
  return code == 0;
}

const char* glslang_default_exe() { return "glslangValidator"; }

std::string glslang_sdk_candidate(const std::string& root) {
  return root + "/tools/vulkan_sdk/bin/glslangValidator";
}

}  // namespace nvn::os_host
