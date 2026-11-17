// SPDX-License-Identifier: MIT
// Neutral OS host interface for the shared NVN->Vulkan layer (stage 6E-A).
// Windows: runtime/nvn/nvn_os_host_win32.cpp; Linux: runtime/nvn/linux/nvn_os_host_posix.cpp.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nvn::os_host {

int primary_screen_height();

uint64_t monotonic_ns();
uint64_t monotonic_ms();
void sleep_ms(uint32_t ms);

uint64_t current_thread_id();
uint64_t current_process_id();
bool set_current_thread_above_normal();
bool set_current_thread_normal();
bool set_current_thread_below_normal();

void* aligned_alloc(size_t alignment, size_t size);
void aligned_free(void* ptr);

uint64_t rdtsc();
void cpu_yield();
bool readable_addr(const void* ptr, size_t size);
// Returns true and writes the calling thread's hardware cycle counter when supported (Win32 QueryThreadCycleTime).
// Returns false on platforms without an unprivileged per-thread cycle counter (Linux).
bool thread_cycle_time(uint64_t* out_cycles);

// Filesystem, paths, SHA-256, and optional dev/harness process helpers (Group 2B).
char path_sep();
std::string cache_dir();
std::string temp_dir();
std::string exe_dir();
bool ensure_dir(const std::string& path);
bool file_exists(const std::string& path);
bool file_mtime_ns(const std::string& path, int64_t* out_ns);
bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out);
bool write_file_bytes(const std::string& path, const void* data, size_t size);
bool remove_file(const std::string& path);
bool atomic_replace(const std::string& tmp_path, const std::string& dst_path);
std::vector<std::string> list_files(const std::string& dir, const char* prefix, const char* suffix);

void sha256(const void* data, size_t len, uint8_t out[32]);
void sha256_2(const void* d1, size_t n1, const void* d2, size_t n2, uint8_t out[32]);

// Optional external ShaderHarness invocation (Windows only today; returns false on Linux).
bool run_shader_harness(const std::string& cmd, const std::string& json_path);
// Optional external glslang invocation for SNAP_DEV=1 live shader reload.
bool run_process_capture(const std::string& cmd, std::string* out_output, int* out_exit_code);
const char* glslang_default_exe();
std::string glslang_sdk_candidate(const std::string& root);

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
