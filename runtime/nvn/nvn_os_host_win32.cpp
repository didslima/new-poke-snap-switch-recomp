// SPDX-License-Identifier: MIT
// Win32 host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_os_host.h"

namespace {

std::wstring widen_utf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string narrow_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

namespace nvn::os_host {

int primary_screen_height() {
  return GetSystemMetrics(SM_CYSCREEN);
}

uint64_t monotonic_ns() {
  static const LARGE_INTEGER freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0) return 0;
  const uint64_t q = static_cast<uint64_t>(now.QuadPart);
  const uint64_t f = static_cast<uint64_t>(freq.QuadPart);
  return (q / f) * 1000000000ull + ((q % f) * 1000000000ull) / f;
}

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(GetTickCount64());
}

void sleep_ms(uint32_t ms) {
  Sleep(ms);
}

uint64_t current_thread_id() {
  return static_cast<uint64_t>(GetCurrentThreadId());
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(GetCurrentProcessId());
}

bool set_current_thread_above_normal() {
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
}

bool set_current_thread_normal() {
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL) != 0;
}

bool set_current_thread_below_normal() {
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
  return _aligned_malloc(size, alignment);
}

void aligned_free(void* ptr) {
  _aligned_free(ptr);
}

uint64_t rdtsc() {
  return __rdtsc();
}

void cpu_yield() {
  YieldProcessor();
}

bool readable_addr(const void* ptr, size_t size) {
  if (!size) return true;
  if (!ptr) return false;
  return !IsBadReadPtr(ptr, size);
}

bool thread_cycle_time(uint64_t* out_cycles) {
  if (!out_cycles) return false;
  ULONG64 cyc = 0;
  if (!QueryThreadCycleTime(GetCurrentThread(), &cyc)) return false;
  *out_cycles = static_cast<uint64_t>(cyc);
  return true;
}

char path_sep() { return '\\'; }

std::string cache_dir() {
  char buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::string(buf, n) + "\\SnapRecomp";
  return ".\\SnapRecomp";
}

std::string temp_dir() {
  wchar_t tp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tp);
  return narrow_utf8(tp);
}

std::string exe_dir() {
  wchar_t b[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
  std::string p = n ? narrow_utf8(std::wstring(b, n)) : std::string();
  const size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '\\' || path[i] == '/') {
      const std::string sub = path.substr(0, i);
      if (!(sub.size() == 2 && sub[1] == ':')) CreateDirectoryW(widen_utf8(sub).c_str(), nullptr);
    }
  }
  return CreateDirectoryW(widen_utf8(path).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool file_exists(const std::string& path) {
  const DWORD a = GetFileAttributesW(widen_utf8(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool file_mtime_ns(const std::string& path, int64_t* out_ns) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(widen_utf8(path).c_str(), GetFileExInfoStandard, &d)) return false;
  if (out_ns) {
    const uint64_t ticks = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) |
                           static_cast<uint64_t>(d.ftLastWriteTime.dwLowDateTime);
    *out_ns = static_cast<int64_t>(ticks);
  }
  return true;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  HANDLE fh = CreateFileW(widen_utf8(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fh == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = false;
  if (GetFileSizeEx(fh, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (64ll << 20)) {
    out->resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ok = sz.QuadPart == 0 || (::ReadFile(fh, out->data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) &&
                              got == static_cast<DWORD>(sz.QuadPart));
  }
  CloseHandle(fh);
  if (!ok) out->clear();
  return ok;
}

bool write_file_bytes(const std::string& path, const void* data, size_t size) {
  FILE* f = _wfopen(widen_utf8(path).c_str(), L"wb");
  if (!f) return false;
  const bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
  const bool closed = std::fclose(f) == 0;
  return ok && closed;
}

bool remove_file(const std::string& path) {
  return DeleteFileW(widen_utf8(path).c_str()) != 0;
}

bool atomic_replace(const std::string& tmp_path, const std::string& dst_path) {
  return MoveFileExW(widen_utf8(tmp_path).c_str(), widen_utf8(dst_path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<std::string> list_files(const std::string& dir, const char* prefix, const char* suffix) {
  std::vector<std::string> out;
  const std::string pat = dir + "\\" + (prefix ? prefix : "") + "*" + (suffix ? suffix : "");
  WIN32_FIND_DATAW fd{};
  HANDLE hf = FindFirstFileW(widen_utf8(pat).c_str(), &fd);
  if (hf == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      out.push_back(narrow_utf8(fd.cFileName));
    }
  } while (FindNextFileW(hf, &fd));
  FindClose(hf);
  return out;
}

void sha256_2(const void* d1, size_t n1, const void* d2, size_t n2, uint8_t out[32]) {
  static BCRYPT_ALG_HANDLE keep = [] {
    BCRYPT_ALG_HANDLE a = nullptr;
    return BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ? nullptr : a;
  }();
  std::memset(out, 0, 32);
  BCRYPT_ALG_HANDLE alg = keep;
  if (!keep && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    if (d1 && n1) BCryptHashData(h, const_cast<PUCHAR>(static_cast<const uint8_t*>(d1)), static_cast<ULONG>(n1), 0);
    if (d2 && n2) BCryptHashData(h, const_cast<PUCHAR>(static_cast<const uint8_t*>(d2)), static_cast<ULONG>(n2), 0);
    BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h);
  }
  if (!keep) BCryptCloseAlgorithmProvider(alg, 0);
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
  sha256_2(data, len, nullptr, 0, out);
}

bool run_shader_harness(const std::string& cmd, const std::string& json_path) {
  std::string cmd_buf = cmd;
  STARTUPINFOA si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi)) {
    return false;
  }
  const DWORD w = WaitForSingleObject(pi.hProcess, 600000);
  DWORD code = 1;
  if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return code == 0 && GetFileAttributesA(json_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool run_process_capture(const std::string& cmd, std::string* out_output, int* out_exit_code) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::wstring wcmd = widen_utf8(cmd);
  const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    if (out_output) *out_output = "cannot start process (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  std::string msg;
  char buf[512];
  DWORD got = 0;
  while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) msg.append(buf, got);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (out_output) *out_output = std::move(msg);
  if (out_exit_code) *out_exit_code = static_cast<int>(code);
  return code == 0;
}

const char* glslang_default_exe() { return "glslangValidator.exe"; }

std::string glslang_sdk_candidate(const std::string& root) {
  return root + "\\tools\\vulkan_sdk\\Bin\\glslangValidator.exe";
}

}  // namespace nvn::os_host
