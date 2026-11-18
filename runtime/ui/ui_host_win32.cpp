// SPDX-License-Identifier: MIT
// Win32 platform host implementation for runtime/ui (Stage 6F).
#include <windows.h>

#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "ui_host.h"
#include "ui_internal.h"

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

}  // namespace snap::ui::host
