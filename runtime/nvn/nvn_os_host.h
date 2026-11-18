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
[[noreturn]] void fatal_exit(int code);

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
bool run_shader_harness_server(const std::string& launch, const std::string& bin, const std::string& spv, const std::string& json,
                               int stage, uint32_t const_off, uint32_t code_off, bool* out_ok);
std::string resolve_shader_cache_key(const std::string& dir, std::string (*sha_fn)(void*, const std::string&), void* sha_ctx,
                                     const std::string& salt, int stage, uint32_t code_off, uint32_t const_off);
bool ensure_shader_cache_entry(const std::string& dir, const std::string& key, uint32_t const_off,
                               bool (*try_load)(void*), bool (*translate_fn)(void*, const std::string&, const std::string&, const std::string&),
                               void* ctx);
// Optional external glslang invocation for SNAP_DEV=1 live shader reload.
bool run_process_capture(const std::string& cmd, std::string* out_output, int* out_exit_code);
const char* glslang_default_exe();
std::string glslang_sdk_candidate(const std::string& root);

// Optional developer diagnostic (NVN_WB_WATCH=1): arms a one-shot writeback page guard on Windows.
// Returns false explicitly on Linux where PAGE_GUARD cannot be emulated safely without global signal handlers.
bool watch_writeback_page(void* addr, size_t size, const int* present_count_ptr);

// High-resolution steady-clock sleep until target_ns (same epoch as monotonic_ns()),
// optionally spinning for the last spin_ns.
void sleep_until_steady_ns(int64_t target_ns, int64_t spin_ns);

// Display & VBlank helpers for frame pacing (Group 2C-2).
struct PaceDisplayInfo {
  uint64_t monitor_id = 0;
  char name[64] = {};
  double refresh_hz = 0.0;
};

bool pace_query_display(void* native_window, int monitor_override, PaceDisplayInfo* out);
bool window_pixel_size(void* native_window, uint32_t* out_w, uint32_t* out_h);
bool pace_vblank_supported();
void* pace_vblank_open(const char* display_name);
bool pace_vblank_wait(void* handle, int* out_status);
void pace_vblank_close(void* handle);

// Hardware/OS dirty-page write watch (Win32 GetWriteWatch); returns false on Linux so callers fall back to full copies.
bool write_watch_supported();
bool query_write_watch(void* base, size_t size, bool reset, std::vector<void*>* out_addrs, uint32_t* out_error);

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
