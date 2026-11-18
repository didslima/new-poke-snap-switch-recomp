// SPDX-License-Identifier: MIT
// Neutral host interface for shared HLE infrastructure (overrides, config, settings, services_sys).
// Windows implementation: runtime/hle/hle_host_win32.cpp; Linux implementation: runtime/hle/linux/hle_host_posix.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <system_error>

namespace hle::host {

char path_sep();
std::string exe_dir();
std::string default_data_root();
std::string default_config_dir();
std::string legacy_data_root();
bool valid_utf8_path(const std::string& s);
bool readable_addr(uint64_t addr, size_t size);
bool mem_range_ok(uint64_t addr, size_t size, bool write);
bool atomic_replace_file(const std::string& tmp_path, const std::string& dst_path);
void set_env_var(const char* name, const char* value);
void unset_env_var(const char* name);
void show_error_dialog(const char* title, const char* message);
bool local_zone_at(int64_t t, int64_t* offset_out, bool* dst_out, char name_out[8]);
std::string ui_language_tag();
bool shift_held_at_launch();
uint64_t process_id();
void* lock_file_begin(const std::string& path, int timeout_ms);
void lock_file_end(void* handle);

// Filesystem host primitives (used by fs.cpp and fs_core.cpp).
void* file_open(const std::string& path, bool write);
void file_close(void* handle);
int64_t file_size(void* handle);
int64_t file_read_at(void* handle, int64_t off, void* buf, size_t n);
bool file_write_at(void* handle, int64_t off, const void* buf, size_t n);
bool file_set_size(void* handle, int64_t size);
bool file_flush(void* handle);

// Save folder exclusive lock: 0 = acquired (out_handle set), 1 = target locked, 2 = space/other error.
int try_acquire_save_lock(const std::string& lock_path, void** out_handle);
void release_save_lock(void* handle);

bool is_disk_full_error(const std::error_code& ec);
std::error_code last_os_error();
void set_last_os_error(int err);
uint64_t monotonic_ms();
void sleep_ms(int ms);
[[noreturn]] void exit_process(int code);

// Profile & process helpers (used by profiles.cpp).
bool is_save_lock_held(const std::string& lock_path);
void* pending_lock_begin(const std::string& lock_path);
void pending_lock_end(void* handle, const std::string& lock_path);
bool rename_save_dir(const std::string& src_dir, const std::string& dst_dir, const std::string& lock_path,
                     bool* lock_busy, std::error_code* ec);
void wait_process_exit(uint64_t pid, int timeout_ms);
bool local_calendar_tm(int64_t t, std::tm* out);

}  // namespace hle::host
