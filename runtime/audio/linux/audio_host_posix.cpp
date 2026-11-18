// SPDX-License-Identifier: MIT
// POSIX (Linux) host services for the shared audio core (audio_out.cpp).
#if !defined(__linux__)
#error "runtime/audio/linux/audio_host_posix.cpp is the Linux implementation of audio_host.h"
#endif

#include "../audio_host.h"
#include "../audio_internal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

// Weak fallbacks when audio_out.cpp is linked into Linux targets without HLE access.cpp or os_state.cpp.
extern "C" {
__attribute__((weak)) int snap_access_mono(void) { return 0; }
struct SnapStateHooks;
__attribute__((weak)) void snap_state_set_hooks(const SnapStateHooks*) {}
}

namespace snap_audio {

// Weak factory overridden by runtime/audio/linux/audio_sdl3.cpp in Stage 6D-B.
__attribute__((weak)) Backend* make_sdl3_audio_backend() { return nullptr; }

namespace host {

char path_sep() { return '/'; }

std::string config_dir() {
  if (const char* e = std::getenv("SNAP_CONFIG_DIR"); e && *e) return e;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::string(xdg) + "/SnapRecomp";
  if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.config/SnapRecomp";
  return "/tmp/SnapRecomp";
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path), ec);
  return !ec;
}

bool read_text_file(const std::string& path, char* buf, size_t cap, size_t* out_len) {
  if (!buf || cap == 0) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  size_t n = std::fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  std::fclose(f);
  if (out_len) *out_len = n;
  return true;
}

bool atomic_write_text(const std::string& path, const char* data, size_t len) {
  const std::filesystem::path dst(path);
  if (dst.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) return false;
  }
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool wrote = std::fwrite(data, 1, len, f) == len && std::ferror(f) == 0;
  const bool closed = std::fclose(f) == 0;
  if (!wrote || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

Backend* create_backend(int kind) {
  if (kind == SNAP_AUDIO_BACKEND_AUTO || kind == SNAP_AUDIO_BACKEND_SDL3) {
    return make_sdl3_audio_backend();
  }
  return nullptr;
}

}  // namespace host
}  // namespace snap_audio
