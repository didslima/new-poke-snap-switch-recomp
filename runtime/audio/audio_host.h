// SPDX-License-Identifier: MIT
// Neutral OS host services for the shared audio core (audio_out.cpp).
// Windows: runtime/audio/audio_host_win32.cpp; Linux: runtime/audio/linux/audio_host_posix.cpp.
#pragma once

#include <cstddef>
#include <string>

namespace snap_audio {

class Backend;

namespace host {

char path_sep();
std::string config_dir();
bool ensure_dir(const std::string& path);
bool read_text_file(const std::string& path, char* buf, size_t cap, size_t* out_len);
bool atomic_write_text(const std::string& path, const char* data, size_t len);
Backend* create_backend(int kind);

}  // namespace host
}  // namespace snap_audio
