// SPDX-License-Identifier: MIT
// Win32 host services for shared HLE infrastructure (stage 6B).
#include "hle_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "hle_config.h"

// [pace] runtime/nvn/nvn_pace.cpp: fallback when NVN is not linked.
extern "C" int snap_pace_vsync_wait_none(int64_t*);
extern "C" void snap_pace_vsync_note_none();
#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:snap_pace_vsync_wait=snap_pace_vsync_wait_none")
#pragma comment(linker, "/alternatename:snap_pace_vsync_note=snap_pace_vsync_note_none")
#pragma comment(linker, "/alternatename:snap_ui_pick_profile=snap_ui_pick_profile_none")
#pragma comment(linker, "/alternatename:snap_photoexport_drain_for_exit=snap_photoexport_drain_none")
#endif

namespace hle {
std::string appdata_utf8() {
  std::wstring w;
  if (const wchar_t* e = _wgetenv(L"APPDATA"); e && *e) w = e;
  else {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) w = p;
    if (p) CoTaskMemFree(p);
  }
  if (w.empty()) return ".";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace hle

namespace hle::host {

char path_sep() { return '\\'; }

std::string default_data_root() { return hle::appdata_utf8() + "\\SnapRecomp"; }

std::string default_config_dir() { return default_data_root() + "\\config"; }

bool valid_utf8_path(const std::string& s) {
  return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
}

std::string legacy_data_root() {
  const wchar_t* w = _wgetenv(L"APPDATA");
  if (!w || !*w) return "";
  const int wn = (int)wcslen(w);
  int n = WideCharToMultiByte(CP_ACP, 0, w, wn, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string ansi((size_t)n, '\0');
  WideCharToMultiByte(CP_ACP, 0, w, wn, ansi.data(), n, nullptr, nullptr);
  return valid_utf8_path(ansi) ? (ansi + "\\SnapRecomp") : std::string();
}

bool readable_addr(uint64_t addr, size_t size) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  const size_t avail = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)addr);
  return size <= avail;
}

bool atomic_replace_file(const std::string& tmp_path, const std::string& dst_path) {
  const std::wstring wtmp = std::filesystem::u8path(tmp_path).wstring();
  const std::wstring wdst = std::filesystem::u8path(dst_path).wstring();
  for (int i = 0; i < 5; i++) {
    if (MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return true;
    Sleep(20);
  }
  return false;
}

void set_env_var(const char* name, const char* value) { _putenv_s(name, value ? value : ""); }

void unset_env_var(const char* name) { _putenv_s(name, ""); }

void show_error_dialog(const char* title, const char* message) {
  MessageBoxA(nullptr, message, title, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool local_zone_at(int64_t t, int64_t* offset_out, bool* dst_out, char name_out[8]) {
  __time64_t tt = (__time64_t)t;
  struct tm lt;
  if (_localtime64_s(&lt, &tt) != 0) return false;
  const int64_t days = days_from_civil(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday);
  *offset_out = days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec - t;
  *dst_out = lt.tm_isdst > 0;
  char nm[64] = {};
  size_t n = 0;
  _get_tzname(&n, nm, sizeof nm, *dst_out ? 1 : 0);
  std::memset(name_out, 0, 8);
  int k = 0;
  if (std::strchr(nm, ' ')) {
    for (const char* q = nm; *q && k < 7; ++q)
      if (q == nm || q[-1] == ' ') name_out[k++] = *q;
  } else {
    std::strncpy(name_out, nm, 7);
  }
  return true;
}

static std::string tag_from_locale_name(const std::string& raw) {
  std::string s;
  for (char c : raw) s += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (c == '_' ? '-' : c);
  auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };
  if (starts("zh-hans") || starts("zh-cn") || starts("zh-sg")) return "zh-hans";
  if (starts("zh-hant") || starts("zh-tw") || starts("zh-hk") || starts("zh-mo")) return "zh-hant";
  if (starts("ja")) return "ja";
  if (starts("fr")) return "fr";
  if (starts("de")) return "de";
  if (starts("it")) return "it";
  if (starts("es")) return "es";
  if (starts("ko")) return "ko";
  if (starts("en")) return "en";
  return "";
}

std::string ui_language_tag() {
  ULONG n = 0, len = 0;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, nullptr, &len) && len > 1) {
    std::wstring buf(len, L' ');
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, buf.data(), &len)) {
      for (size_t i = 0; i < buf.size() && buf[i];) {
        std::wstring w(buf.c_str() + i);
        i += w.size() + 1;
        std::string a;
        for (wchar_t ch : w) a += ch < 128 ? (char)ch : '?';
        const std::string tag = tag_from_locale_name(a);
        if (!tag.empty()) return tag;
      }
    }
  }
  return "en";
}

bool shift_held_at_launch() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

uint64_t process_id() { return (uint64_t)GetCurrentProcessId(); }

struct WinMigrateLock {
  HANDLE h = INVALID_HANDLE_VALUE;
  OVERLAPPED ov{};
};

void* lock_file_begin(const std::string& path, int timeout_ms) {
  auto* lk = new WinMigrateLock;
  const std::wstring wp = std::filesystem::u8path(path).wstring();
  lk->h = CreateFileW(wp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (lk->h == INVALID_HANDLE_VALUE) { delete lk; return nullptr; }
  for (int waited = 0;; waited += 50) {
    if (LockFileEx(lk->h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lk->ov)) return lk;
    if (waited >= timeout_ms) { CloseHandle(lk->h); delete lk; return nullptr; }
    Sleep(50);
  }
}

void lock_file_end(void* handle) {
  auto* lk = static_cast<WinMigrateLock*>(handle);
  if (!lk) return;
  if (lk->h != INVALID_HANDLE_VALUE) {
    UnlockFileEx(lk->h, 0, 1, 0, &lk->ov);
    CloseHandle(lk->h);
  }
  delete lk;
}

void* file_open(const std::string& path, bool write) {
  const std::wstring wp = std::filesystem::u8path(path).wstring();
  const DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
  HANDLE h = CreateFileW(wp.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  return h == INVALID_HANDLE_VALUE ? nullptr : static_cast<void*>(h);
}

void file_close(void* handle) {
  if (handle) CloseHandle(static_cast<HANDLE>(handle));
}

int64_t file_size(void* handle) {
  if (!handle) return -1;
  LARGE_INTEGER s;
  return GetFileSizeEx(static_cast<HANDLE>(handle), &s) ? s.QuadPart : -1;
}

int64_t file_read_at(void* handle, int64_t off, void* buf, size_t n) {
  if (!handle) return -1;
  HANDLE h = static_cast<HANDLE>(handle);
  size_t done = 0;
  char* b = static_cast<char*>(buf);
  while (done < n) {
    const DWORD chunk = (DWORD)std::min<size_t>(n - done, 1u << 30);
    DWORD got = 0;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)((off + (int64_t)done) & 0xffffffff);
    ov.OffsetHigh = (DWORD)((off + (int64_t)done) >> 32);
    if (!ReadFile(h, b + done, chunk, &got, &ov)) {
      if (GetLastError() == ERROR_HANDLE_EOF) break;
      return -1;
    }
    if (got == 0) break;
    // Mark filled pages written from user mode for GetWriteWatch dirty-page tracking.
    for (uintptr_t p = (uintptr_t)(b + done) & ~(uintptr_t)3, e = (uintptr_t)(b + done + got); p < e; p = (p | 4095) + 1)
      InterlockedOr((volatile LONG*)p, 0);
    done += got;
  }
  return (int64_t)done;
}

bool file_write_at(void* handle, int64_t off, const void* buf, size_t n) {
  if (!handle) return false;
  HANDLE h = static_cast<HANDLE>(handle);
  const char* b = static_cast<const char*>(buf);
  size_t done = 0;
  while (done < n) {
    const DWORD chunk = (DWORD)std::min<size_t>(n - done, 1u << 30);
    DWORD got = 0;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)((off + (int64_t)done) & 0xffffffff);
    ov.OffsetHigh = (DWORD)((off + (int64_t)done) >> 32);
    if (!WriteFile(h, b + done, chunk, &got, &ov)) return false;
    done += got;
  }
  return true;
}

bool file_set_size(void* handle, int64_t size) {
  if (!handle) return false;
  HANDLE h = static_cast<HANDLE>(handle);
  LARGE_INTEGER li;
  li.QuadPart = size;
  return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
}

bool file_flush(void* handle) {
  return handle && FlushFileBuffers(static_cast<HANDLE>(handle)) != 0;
}

int try_acquire_save_lock(const std::string& lock_path, void** out_handle) {
  if (out_handle) *out_handle = nullptr;
  const std::wstring wp = std::filesystem::u8path(lock_path).wstring();
  HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    return (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED) ? 1 : 2;
  }
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    const DWORD e = GetLastError();
    CloseHandle(h);
    return e == ERROR_LOCK_VIOLATION ? 1 : 2;
  }
  if (out_handle) *out_handle = static_cast<void*>(h);
  else CloseHandle(h);
  return 0;
}

void release_save_lock(void* handle) {
  if (handle) CloseHandle(static_cast<HANDLE>(handle));
}

bool is_disk_full_error(const std::error_code& ec) {
  if (ec.category() == std::system_category() && (ec.value() == ERROR_DISK_FULL || ec.value() == ERROR_HANDLE_DISK_FULL)) return true;
  return ec == std::errc::no_space_on_device || ec == std::errc::file_too_large;
}

std::error_code last_os_error() {
  return std::error_code((int)GetLastError(), std::system_category());
}

uint64_t monotonic_ms() { return (uint64_t)GetTickCount64(); }

void sleep_ms(int ms) {
  if (ms > 0) Sleep((DWORD)ms);
}

[[noreturn]] void exit_process(int code) { ExitProcess((UINT)code); }

}  // namespace hle::host
