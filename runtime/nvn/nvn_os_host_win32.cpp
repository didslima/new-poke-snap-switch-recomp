// SPDX-License-Identifier: MIT
// Win32 host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

namespace {

std::atomic<int> g_wb_watch_hits{0};
const int* g_wb_present_count_ptr = nullptr;

LONG CALLBACK WbWatchHandler(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  if (g_wb_watch_hits.fetch_add(1) < 64) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const int present = g_wb_present_count_ptr ? *g_wb_present_count_ptr : 0;
    std::fprintf(stderr, "[wbwatch] p%d access %s 0x%llx by rip=0x%llx (exe+0x%llx) thread=%lu\n",
                 present,
                 ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]),
                 static_cast<unsigned long long>(ep->ContextRecord->Rip),
                 static_cast<unsigned long long>(ep->ContextRecord->Rip - base),
                 GetCurrentThreadId());
    void* fr[24];
    const USHORT nf = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
    std::fprintf(stderr, "[wbwatch]   stack:");
    for (USHORT i = 0; i < nf; ++i) {
      std::fprintf(stderr, " exe+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fr[i]) - base));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool watch_writeback_page(void* addr, size_t size, const int* present_count_ptr) {
  if (!addr || size == 0) return false;
  if (present_count_ptr) g_wb_present_count_ptr = present_count_ptr;
  static std::once_flag installed;
  std::call_once(installed, [] { AddVectoredExceptionHandler(1, WbWatchHandler); });
  DWORD old = 0;
  return VirtualProtect(addr, size, PAGE_READWRITE | PAGE_GUARD, &old) != 0;
}

namespace {

struct Win32ThreadTimer {
  HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  ~Win32ThreadTimer() {
    if (h) CloseHandle(h);
  }
};

double win32_query_hz(const wchar_t* gdi_name) {
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p.data(), &nm, m.data(), nullptr) == ERROR_SUCCESS) {
      for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof sn;
        sn.header.adapterId = p[i].sourceInfo.adapterId;
        sn.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdi_name)) continue;
        const UINT32 mi = p[i].targetInfo.modeInfoIdx;
        if (mi != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mi < nm && m[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
          const auto& v = m[mi].targetMode.targetVideoSignalInfo.vSyncFreq;
          if (v.Denominator && v.Numerator) return static_cast<double>(v.Numerator) / v.Denominator;
        }
        const auto& r = p[i].targetInfo.refreshRate;
        if (r.Denominator && r.Numerator) return static_cast<double>(r.Numerator) / r.Denominator;
      }
    }
  }
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(gdi_name, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
    double f = dm.dmDisplayFrequency;
    if (dm.dmDisplayFrequency % 60 == 59) f = (f + 1) * 1000.0 / 1001.0;
    return f;
  }
  return 0.0;
}

struct KmtOpen {
  WCHAR DeviceName[32];
  UINT hAdapter;
  LUID AdapterLuid;
  UINT VidPnSourceId;
};
struct KmtWait {
  UINT hAdapter;
  UINT hDevice;
  UINT VidPnSourceId;
};
struct KmtClose {
  UINT hAdapter;
};

struct KmtApi {
  HMODULE gdi = nullptr;
  LONG(APIENTRY* open)(KmtOpen*) = nullptr;
  LONG(APIENTRY* wait)(const KmtWait*) = nullptr;
  LONG(APIENTRY* close)(const KmtClose*) = nullptr;
  bool valid = false;
};

const KmtApi& kmt_api() {
  static const KmtApi api = [] {
    KmtApi a;
    a.gdi = LoadLibraryW(L"gdi32.dll");
    if (a.gdi) {
      a.open = reinterpret_cast<LONG(APIENTRY*)(KmtOpen*)>(GetProcAddress(a.gdi, "D3DKMTOpenAdapterFromGdiDisplayName"));
      a.wait = reinterpret_cast<LONG(APIENTRY*)(const KmtWait*)>(GetProcAddress(a.gdi, "D3DKMTWaitForVerticalBlankEvent"));
      a.close = reinterpret_cast<LONG(APIENTRY*)(const KmtClose*)>(GetProcAddress(a.gdi, "D3DKMTCloseAdapter"));
      a.valid = (a.open && a.wait && a.close);
    }
    return a;
  }();
  return api;
}

struct Win32VBlankHandle {
  KmtOpen open{};
};

}  // namespace

void sleep_until_steady_ns(int64_t target_ns, int64_t spin_ns) {
  thread_local Win32ThreadTimer tt;
  const HANDLE timer = tt.h;
  static std::once_flag period;
  const int64_t margin = (spin_ns > 100000) ? spin_ns : 100000;
  for (;;) {
    const int64_t left = target_ns - static_cast<int64_t>(monotonic_ns());
    if (left <= margin) break;
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -(left - margin) / 100;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      std::call_once(period, [] { timeBeginPeriod(1); });
      if (left < 1500000 + margin) break;
      Sleep(1);
    }
  }
  if (spin_ns > 0) {
    while (static_cast<int64_t>(monotonic_ns()) < target_ns) YieldProcessor();
  }
}

bool pace_query_display(void* native_window, int monitor_override, PaceDisplayInfo* out) {
  if (!out) return false;
  *out = PaceDisplayInfo{};
  HMONITOR m = MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY);
  out->monitor_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m));
  wchar_t wname[32] = {};
  if (monitor_override > 0) {
    _snwprintf_s(wname, _countof(wname), _TRUNCATE, L"\\\\.\\DISPLAY%d", monitor_override);
  } else {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(m, &mi)) wcsncpy_s(wname, mi.szDevice, _TRUNCATE);
  }
  if (wname[0]) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wname, -1, out->name, sizeof(out->name), nullptr, nullptr);
    if (n <= 0) out->name[0] = '\0';
    out->refresh_hz = win32_query_hz(wname);
  }
  return out->refresh_hz > 0.0;
}

bool pace_vblank_supported() {
  return kmt_api().valid;
}

void* pace_vblank_open(const char* display_name) {
  const auto& api = kmt_api();
  if (!api.valid || !display_name || !*display_name) return nullptr;
  auto* h = new Win32VBlankHandle{};
  const int n = MultiByteToWideChar(CP_UTF8, 0, display_name, -1, h->open.DeviceName, 32);
  if (n <= 0 || api.open(&h->open) != 0) {
    delete h;
    return nullptr;
  }
  return h;
}

bool pace_vblank_wait(void* handle, int* out_status) {
  const auto& api = kmt_api();
  if (!api.valid || !handle) {
    if (out_status) *out_status = -1;
    return false;
  }
  const auto* h = static_cast<const Win32VBlankHandle*>(handle);
  KmtWait w{h->open.hAdapter, 0, h->open.VidPnSourceId};
  const LONG st = api.wait(&w);
  if (out_status) *out_status = static_cast<int>(st);
  return st == 0;
}

void pace_vblank_close(void* handle) {
  if (!handle) return;
  const auto& api = kmt_api();
  auto* h = static_cast<Win32VBlankHandle*>(handle);
  if (api.valid) {
    KmtClose c{h->open.hAdapter};
    api.close(&c);
  }
  delete h;
}

}  // namespace nvn::os_host
