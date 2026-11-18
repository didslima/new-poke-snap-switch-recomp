// SPDX-License-Identifier: MIT
// Win32 platform host implementation for runtime/ui (Stage 6F/6G).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "ui_host.h"
#include "ui_internal.h"

#pragma comment(lib, "ws2_32.lib")

extern "C" bool snap_benchmark_launch_from_game();
extern "C" bool snap_benchmark_launch_none() { return false; }
#pragma comment(linker, "/alternatename:snap_benchmark_launch_from_game=snap_benchmark_launch_none")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace snap::ui {
std::recursive_mutex& overlay_mutex();
bool overlay_imgui_ready();
bool overlay_modal_active();
}  // namespace snap::ui

namespace snap::ui::host {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_orig = nullptr;

LRESULT ui_wndproc_impl(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (w == VK_F1) {
        if (!(l & (1 << 30)) && !snap::ui::overlay_modal_active()) {
          snap::ui::overlay_set_open(!snap::ui::overlay_open());
        }
        return 0;
      }
      if (w == VK_F3 && snap::ui::fps_overlay_hotkey_free()) {
        if (!(l & (1 << 30))) snap::ui::fps_overlay_cycle();
        return 0;
      }
      if (w == VK_F4 && snap::ui::tune_hotkey_free()) {
        if (!(l & (1 << 30))) snap::ui::tune_toggle_overlay();
        return 0;
      }
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (w == VK_F1) return 0;
      if (w == VK_F3 && snap::ui::fps_overlay_hotkey_free()) return 0;
      if (w == VK_F4 && snap::ui::tune_hotkey_free()) return 0;
      break;
    case WM_MOUSEWHEEL:
      hle::input::post_wheel(GET_WHEEL_DELTA_WPARAM(w));
      break;
    case WM_KILLFOCUS:
    case WM_CLOSE:
    case WM_DESTROY:
      ClipCursor(nullptr);
      break;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE) ClipCursor(nullptr);
      break;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT && hle::input::hub().cursor_hidden() &&
          !snap::ui::overlay_open() && !snap::ui::overlay_modal_active()) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    default:
      break;
  }
  if ((snap::ui::overlay_open() || snap::ui::overlay_modal_active()) && snap::ui::overlay_imgui_ready()) {
    std::lock_guard<std::recursive_mutex> lk(snap::ui::overlay_mutex());
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return TRUE;
  }
  return CallWindowProcW(g_orig, h, m, w, l);
}

LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  try {
    return ui_wndproc_impl(h, m, w, l);
  } catch (const std::exception& e) {
    static int n = 0;
    if (n++ < 20) hle::log("ui: exception in the window procedure (msg 0x%x): %s", m, e.what());
  } catch (...) {
    static int n = 0;
    if (n++ < 20) hle::log("ui: unknown exception in the window procedure (msg 0x%x)", m);
  }
  return g_orig ? CallWindowProcW(g_orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void ensure_window(HWND h) {
  if (!h || g_hwnd == h) return;
  g_hwnd = h;
  g_orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ui_wndproc)));
  hle::log("ui: overlay hooked into the game window (F1 or hold Back+Start for 0.5 s opens the controls menu)");
}

}  // namespace

bool init_platform(void* native_window) {
  HWND h = static_cast<HWND>(native_window);
  ensure_window(h);
  return ImGui_ImplWin32_Init(h);
}

void new_frame(ImGuiIO& io, float width, float height) {
  (void)width;
  (void)height;
  ImGui_ImplWin32_NewFrame();
  (void)io;
}

void shutdown_platform() {
  ImGui_ImplWin32_Shutdown();
  g_hwnd = nullptr;
  g_orig = nullptr;
}

void release_cursor_confine() {
  ClipCursor(nullptr);
}

std::string system_font_path(const char* filename) {
  if (!filename || !*filename) return {};
  const char* root = std::getenv("SystemRoot");
  if (!root) return {};
  const std::string path = std::string(root) + "\\Fonts\\" + filename;
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  return path;
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(GetCurrentProcessId());
}

char path_sep() {
  return '\\';
}

namespace {
std::wstring widen_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(w.size() - 1);
  return w;
}
}  // namespace

void play_wav_async(const void* wav_bytes, size_t size) {
  if (!wav_bytes || size == 0) return;
  PlaySoundW(reinterpret_cast<LPCWSTR>(wav_bytes), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

std::string documents_dir() {
  PWSTR docs = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
    out = std::filesystem::path(docs).u8string();
    CoTaskMemFree(docs);
  }
  return out;
}

std::string exe_dir() {
  wchar_t exe[MAX_PATH * 2] = {};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) return {};
  return std::filesystem::path(exe).parent_path().u8string();
}

bool beside_exe(const char* rel_path) {
  if (!rel_path || !*rel_path) return false;
  wchar_t path[MAX_PATH * 2] = {};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
  wchar_t* sl = wcsrchr(path, L'\\');
  if (!sl) return false;
  const std::wstring wrel = widen_utf8(rel_path);
  if (wrel.size() >= MAX_PATH) return false;
  wcscpy(sl + 1, wrel.c_str());
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool benchmark_launch_from_game() {
  return snap_benchmark_launch_from_game();
}

bool tcp_port_open(const char* ipv4_addr, unsigned short port) {
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  bool ok = false;
  if (s != INVALID_SOCKET) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ipv4_addr ? ipv4_addr : "127.0.0.1", &a.sin_addr);
    ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    closesocket(s);
  }
  WSACleanup();
  return ok;
}

void sleep_ms(unsigned ms) {
  Sleep(ms);
}

void open_folder(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  ShellExecuteW(nullptr, L"explore", std::filesystem::u8path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_url(const std::string& url) {
  if (url.empty()) return;
  ShellExecuteW(nullptr, L"open", widen_utf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool launch_gallery_server(const std::string& root, const std::string& profile, const std::string& port_file) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root_p = fs::u8path(root);
  const fs::path py = root_p / "tools" / "venv-recomp" / "Scripts" / "python.exe";
  const std::wstring exe = fs::exists(py, ec) ? py.wstring() : L"python.exe";
  std::wstring cmd = L"\"" + exe + L"\" -E -P \"" + (root_p / "tools-src" / "gallery" / "serve.py").wstring() +
                     L"\" --no-open --profile \"" + widen_utf8(profile) +
                     L"\" --port 8777 --port-file \"" + fs::u8path(port_file).wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                      root_p.wstring().c_str(), &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace snap::ui::host
