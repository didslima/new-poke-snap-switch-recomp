// SPDX-License-Identifier: MIT
// Neutral host interface for shared HLE infrastructure (overrides, config, settings, services_sys).
// Windows implementation: runtime/hle/hle_host_win32.cpp; Linux implementation: runtime/hle/linux/hle_host_posix.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hle::host {

char path_sep();
std::string default_data_root();
std::string default_config_dir();
std::string legacy_data_root();
bool valid_utf8_path(const std::string& s);
bool readable_addr(uint64_t addr, size_t size);
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

}  // namespace hle::host
