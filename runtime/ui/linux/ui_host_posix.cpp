// SPDX-License-Identifier: MIT
// Linux SDL3 platform host implementation for runtime/ui (Stage 6F).
#if !defined(__linux__)
#error "runtime/ui/linux/ui_host_posix.cpp is Linux-only"
#endif

#include "../ui_host.h"
#include "../ui_internal.h"
#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace snap::ui {
std::recursive_mutex& overlay_mutex();
bool overlay_imgui_ready();
bool overlay_modal_active();
}  // namespace snap::ui

namespace snap::ui::host {
namespace {

#ifdef HLE_WITH_SDL3
SDL_Window* g_window = nullptr;
bool g_text_input_active = false;

ImGuiKey sdl_key_to_imgui(SDL_Keycode key) {
  switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: break;
  }
  if (key >= SDLK_A && key <= SDLK_Z) {
    return static_cast<ImGuiKey>(ImGuiKey_A + (key - SDLK_A));
  }
  if (key >= SDLK_0 && key <= SDLK_9) {
    return static_cast<ImGuiKey>(ImGuiKey_0 + (key - SDLK_0));
  }
  if (key >= SDLK_F1 && key <= SDLK_F12) {
    return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - SDLK_F1));
  }
  return ImGuiKey_None;
}

int sdl_button_to_imgui(Uint8 btn) {
  switch (btn) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
  }
}
#endif

std::chrono::steady_clock::time_point g_last_frame{};
bool g_have_last_frame = false;
bool g_initialized = false;

}  // namespace

bool init_platform(void* native_window) {
  g_have_last_frame = false;
  g_initialized = true;
#ifdef HLE_WITH_SDL3
  g_window = static_cast<SDL_Window*>(native_window);
  ImGuiIO& io = ImGui::GetIO();
  io.BackendPlatformName = "snap_sdl3_linux";
  if (g_window) {
    hle::log("ui: overlay hooked into SDL3 window (F1 or hold Back+Start for 0.5 s opens the controls menu)");
  }
#else
  (void)native_window;
#endif
  return true;
}

void new_frame(ImGuiIO& io, float width, float height) {
  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (g_have_last_frame) {
    dt = std::chrono::duration<float>(now - g_last_frame).count();
    if (dt <= 0.0f) dt = 1e-6f;
    if (dt > 0.5f) dt = 0.5f;
  }
  g_last_frame = now;
  g_have_last_frame = true;
  io.DeltaTime = dt;
  io.DisplaySize = ImVec2(width, height);

#ifdef HLE_WITH_SDL3
  if (g_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    const bool ui_active = snap::ui::overlay_open() || snap::ui::overlay_modal_active();
    if (ui_active && !g_text_input_active) {
      SDL_StartTextInput(g_window);
      g_text_input_active = true;
    } else if (!ui_active && g_text_input_active) {
      SDL_StopTextInput(g_window);
      g_text_input_active = false;
    }
    if (hle::input::hub().cursor_hidden() && !ui_active) {
      SDL_HideCursor();
    } else {
      SDL_ShowCursor();
    }
  }
#endif
}

void shutdown_platform() {
#ifdef HLE_WITH_SDL3
  if (g_window && g_text_input_active && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    SDL_StopTextInput(g_window);
  }
  g_text_input_active = false;
  g_window = nullptr;
#endif
  g_initialized = false;
  g_have_last_frame = false;
}

void release_cursor_confine() {
#ifdef HLE_WITH_SDL3
  if (g_window && SDL_WasInit(SDL_INIT_VIDEO) != 0) {
    SDL_SetWindowRelativeMouseMode(g_window, false);
    SDL_ShowCursor();
  }
#endif
}

std::string system_font_path(const char* filename) {
  if (!filename || !*filename) return {};
  // Map Windows fallback font names to standard Linux system fonts when present.
  static constexpr const char* kLatinCandidates[] = {
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
  };
  if (std::strcmp(filename, "segoeui.ttf") == 0) {
    std::error_code ec;
    for (const char* p : kLatinCandidates) {
      if (std::filesystem::is_regular_file(p, ec)) return p;
    }
  }
  return {};
}

uint64_t current_process_id() {
  return static_cast<uint64_t>(::getpid());
}

char path_sep() {
  return '/';
}

}  // namespace snap::ui::host

extern "C" void snap_ui_on_sdl_event(const void* sdl_event) {
#ifdef HLE_WITH_SDL3
  if (!sdl_event) return;
  const auto* ev = static_cast<const SDL_Event*>(sdl_event);
  switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
      if (!ev->key.repeat) {
        if (ev->key.key == SDLK_F1 && !snap::ui::overlay_modal_active()) {
          snap::ui::overlay_set_open(!snap::ui::overlay_open());
          return;
        }
        if (ev->key.key == SDLK_F3 && snap::ui::fps_overlay_hotkey_free()) {
          snap::ui::fps_overlay_cycle();
          return;
        }
        if (ev->key.key == SDLK_F4 && snap::ui::tune_hotkey_free()) {
          snap::ui::tune_toggle_overlay();
          return;
        }
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      hle::input::post_wheel(static_cast<int>(ev->wheel.y * 120.0f));
      break;
    default:
      break;
  }

  if (!(snap::ui::overlay_open() || snap::ui::overlay_modal_active()) || !snap::ui::overlay_imgui_ready()) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lk(snap::ui::overlay_mutex());
  ImGuiIO& io = ImGui::GetIO();
  switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
      io.AddMousePosEvent(ev->motion.x, ev->motion.y);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      const int b = snap::ui::host::sdl_button_to_imgui(ev->button.button);
      if (b >= 0) io.AddMouseButtonEvent(b, ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
      io.AddMouseWheelEvent(ev->wheel.x, ev->wheel.y);
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      const bool down = (ev->type == SDL_EVENT_KEY_DOWN);
      const SDL_Keymod mod = SDL_GetModState();
      io.AddKeyEvent(ImGuiMod_Ctrl, (mod & SDL_KMOD_CTRL) != 0);
      io.AddKeyEvent(ImGuiMod_Shift, (mod & SDL_KMOD_SHIFT) != 0);
      io.AddKeyEvent(ImGuiMod_Alt, (mod & SDL_KMOD_ALT) != 0);
      const ImGuiKey ik = snap::ui::host::sdl_key_to_imgui(ev->key.key);
      if (ik != ImGuiKey_None) io.AddKeyEvent(ik, down);
      break;
    }
    case SDL_EVENT_TEXT_INPUT:
      if (ev->text.text && *ev->text.text) io.AddInputCharactersUTF8(ev->text.text);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      io.AddFocusEvent(true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      io.AddFocusEvent(false);
      break;
    default:
      break;
  }
#else
  (void)sdl_event;
#endif
}
