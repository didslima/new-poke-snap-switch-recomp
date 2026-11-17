// SPDX-License-Identifier: MIT
// Win32 host services for the shared audio core (audio_out.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

#include "audio_host.h"
#include "audio_internal.h"

namespace {

std::wstring widen(const std::string& u) {
  if (u.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
  return w;
}

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

// Weak fallbacks when audio_out.cpp is built without HLE access.cpp or os_state.cpp.
extern "C" {
int snap_access_mono_none(void) { return 0; }
struct SnapStateHooks;
void snap_state_set_hooks_none(const SnapStateHooks*) {}
}
#pragma comment(linker, "/alternatename:snap_access_mono=snap_access_mono_none")
#pragma comment(linker, "/alternatename:snap_state_set_hooks=snap_state_set_hooks_none")

namespace snap_audio::host {

char path_sep() { return '\\'; }

std::string config_dir() {
  if (const wchar_t* e = _wgetenv(L"SNAP_CONFIG_DIR"); e && *e) return narrow(e);
  if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) return narrow(a) + "\\SnapRecomp";
  return ".";
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  return CreateDirectoryW(widen(path).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool read_text_file(const std::string& path, char* buf, size_t cap, size_t* out_len) {
  if (!buf || cap == 0) return false;
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return false;
  size_t n = std::fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  std::fclose(f);
  if (out_len) *out_len = n;
  return true;
}

bool atomic_write_text(const std::string& path, const char* data, size_t len) {
  if (const size_t cut = path.find_last_of("\\/"); cut != std::string::npos) {
    ensure_dir(path.substr(0, cut));
  }
  const std::string tmp = path + ".tmp";
  FILE* f = _wfopen(widen(tmp).c_str(), L"wb");
  if (!f) return false;
  const bool wrote = std::fwrite(data, 1, len, f) == len && std::ferror(f) == 0;
  const bool closed = std::fclose(f) == 0;
  if (!wrote || !closed ||
      !MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(widen(tmp).c_str());
    return false;
  }
  return true;
}

Backend* create_backend(int kind) {
  if (kind == SNAP_AUDIO_BACKEND_AUTO || kind == SNAP_AUDIO_BACKEND_WASAPI) {
    return make_wasapi_backend();
  }
  return nullptr;
}

}  // namespace snap_audio::host
