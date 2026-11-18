// SPDX-License-Identifier: MIT
// Stage 6E-B: Linux SDL3 host window and non-blocking event loop for the NVN->Vulkan renderer.
#if !defined(__linux__)
#error "host/linux/window_sdl3.cpp is Linux-only"
#endif

#include "window_sdl3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

extern "C" __attribute__((weak)) void snap_ui_on_sdl_event(const void*) {}

namespace snap_linux {

HostWindow::~HostWindow() {
  destroy();
}

bool HostWindow::create(const WindowConfig& cfg) {
  destroy();
#ifdef HLE_WITH_SDL3
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "[window_sdl3] SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
    return false;
  }
  sdl_video_owned_ = true;

  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    std::fprintf(stderr, "[window_sdl3] SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
    destroy();
    return false;
  }

  int w = cfg.width > 0 ? cfg.width : 1280;
  int h = cfg.height > 0 ? cfg.height : 720;
  if (const char* ew = std::getenv("SNAP_WINDOW_W"); ew && *ew) {
    const int v = std::atoi(ew);
    if (v >= 64) w = v;
  }
  if (const char* eh = std::getenv("SNAP_WINDOW_H"); eh && *eh) {
    const int v = std::atoi(eh);
    if (v >= 64) h = v;
  }
  bool fs = cfg.fullscreen;
  if (const char* ef = std::getenv("SNAP_FULLSCREEN"); ef && *ef) {
    fs = (*ef == '1');
  }
  int disp_idx = cfg.display_index;
  if (const char* ed = std::getenv("SNAP_DISPLAY"); ed && *ed) {
    const int v = std::atoi(ed);
    if (v > 0) disp_idx = v;
  }

  SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
  if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (fs) flags |= SDL_WINDOW_FULLSCREEN;
  if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;
  if (cfg.high_pixel_density) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

  if (disp_idx > 0) {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_Rect bounds{};
    bool have_bounds = false;
    if (displays && disp_idx <= count) {
      have_bounds = SDL_GetDisplayBounds(displays[disp_idx - 1], &bounds);
    }
    if (displays) SDL_free(displays);

    if (have_bounds) {
      SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, cfg.title.c_str());
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x + std::max(0, (bounds.w - w) / 2));
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y + std::max(0, (bounds.h - h) / 2));
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
      SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
      window_ = SDL_CreateWindowWithProperties(props);
      SDL_DestroyProperties(props);
    }
  }

  if (!window_) {
    window_ = SDL_CreateWindow(cfg.title.c_str(), w, h, flags);
  }
  if (!window_) {
    std::fprintf(stderr, "[window_sdl3] SDL_CreateWindow failed: %s\n", SDL_GetError());
    destroy();
    return false;
  }

  if (!cfg.hidden) {
    SDL_SyncWindow(window_);
  }
  window_id_ = SDL_GetWindowID(window_);
  fullscreen_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
  focused_ = (SDL_GetKeyboardFocus() == window_);
  minimized_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
  quit_requested_ = false;
  sync_sizes();
  return true;
#else
  (void)cfg;
  std::fprintf(stderr, "[window_sdl3] built without SDL3 (HLE_WITH_SDL3 not defined)\n");
  return false;
#endif
}

void HostWindow::destroy() {
#ifdef HLE_WITH_SDL3
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdl_video_owned_) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    sdl_video_owned_ = false;
  }
#endif
  window_id_ = 0;
  fullscreen_ = false;
  quit_requested_ = false;
  focused_ = false;
  minimized_ = false;
  width_ = 0;
  height_ = 0;
  pixel_width_ = 0;
  pixel_height_ = 0;
}

bool HostWindow::set_fullscreen(bool fullscreen) {
#ifdef HLE_WITH_SDL3
  if (!window_) return false;
  if (!SDL_SetWindowFullscreen(window_, fullscreen)) return false;
  SDL_SyncWindow(window_);
  fullscreen_ = fullscreen;
  sync_sizes();
  return true;
#else
  (void)fullscreen;
  return false;
#endif
}

void HostWindow::sync_sizes() {
#ifdef HLE_WITH_SDL3
  if (!window_) return;
  int w = 0, h = 0, pw = 0, ph = 0;
  if (SDL_GetWindowSize(window_, &w, &h)) {
    width_ = w;
    height_ = h;
  }
  if (SDL_GetWindowSizeInPixels(window_, &pw, &ph)) {
    pixel_width_ = pw;
    pixel_height_ = ph;
  } else {
    pixel_width_ = width_;
    pixel_height_ = height_;
  }
#endif
}

WindowEvents HostWindow::poll_events() {
  WindowEvents ev_out{};
#ifdef HLE_WITH_SDL3
  if (!window_) return ev_out;
  SDL_Event ev{};
  while (SDL_PollEvent(&ev)) {
    snap_ui_on_sdl_event(&ev);
    switch (ev.type) {
      case SDL_EVENT_QUIT:
        quit_requested_ = true;
        ev_out.quit_requested = true;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (ev.window.windowID == window_id_) {
          quit_requested_ = true;
          ev_out.quit_requested = true;
        }
        break;
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (ev.window.windowID == window_id_) {
          sync_sizes();
          ev_out.resized = true;
        }
        break;
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        if (ev.window.windowID == window_id_) {
          sync_sizes();
          ev_out.display_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (ev.window.windowID == window_id_) {
          focused_ = true;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (ev.window.windowID == window_id_) {
          focused_ = false;
          ev_out.focus_changed = true;
        }
        break;
      case SDL_EVENT_WINDOW_MINIMIZED:
        if (ev.window.windowID == window_id_) {
          minimized_ = true;
        }
        break;
      case SDL_EVENT_WINDOW_RESTORED:
        if (ev.window.windowID == window_id_) {
          minimized_ = false;
          sync_sizes();
          ev_out.resized = true;
        }
        break;
      default:
        break;
    }
  }
#endif
  ev_out.width = width_;
  ev_out.height = height_;
  ev_out.pixel_width = pixel_width_;
  ev_out.pixel_height = pixel_height_;
  ev_out.focused = focused_;
  ev_out.minimized = minimized_;
  return ev_out;
}

float HostWindow::compute_ultrawide_factor(int pixel_width, int pixel_height) {
  if (pixel_width <= 0 || pixel_height <= 0) return 1.0f;
  const double k = static_cast<double>(pixel_width) / static_cast<double>(pixel_height) / (16.0 / 9.0);
  if (k <= 1.01) return 1.0f;
  return static_cast<float>(std::min(k, 4.0));
}

}  // namespace snap_linux
