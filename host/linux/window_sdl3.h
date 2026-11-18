// SPDX-License-Identifier: MIT
// Stage 6E-B: Linux SDL3 host window and non-blocking event loop for the NVN->Vulkan renderer.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace snap_linux {

struct WindowConfig {
  std::string title = "New Pokemon Snap (native recompilation)";
  int width = 1280;
  int height = 720;
  int display_index = 0;   // 0 = primary/default display; 1..N = 1-based display index
  bool fullscreen = false;
  bool resizable = true;
  bool hidden = false;
  bool high_pixel_density = true;
};

struct WindowEvents {
  bool quit_requested = false;
  bool resized = false;
  bool display_changed = false;
  bool focus_changed = false;
  bool toggle_fullscreen = false;
  int width = 0;
  int height = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  bool focused = false;
  bool minimized = false;
};

class HostWindow {
 public:
  HostWindow() = default;
  ~HostWindow();

  HostWindow(const HostWindow&) = delete;
  HostWindow& operator=(const HostWindow&) = delete;

  bool create(const WindowConfig& cfg = {});
  void destroy();

  bool is_open() const { return window_ != nullptr; }
  void* native_handle() const { return window_; }   // SDL_Window*, passed to nvnWindowBuilderSetNativeWindow
  uint32_t window_id() const { return window_id_; }

  bool set_fullscreen(bool fullscreen);
  bool is_fullscreen() const { return fullscreen_; }
  bool set_display(int index);          // 0-based display index
  bool set_window_size(int w, int h);
  static int display_count();
  static std::string display_name(int index);   // "Name WxH" or empty when out of range
  void request_close() { quit_requested_ = true; }
  bool quit_requested() const { return quit_requested_; }

  int width() const { return width_; }
  int height() const { return height_; }
  int pixel_width() const { return pixel_width_; }
  int pixel_height() const { return pixel_height_; }
  bool focused() const { return focused_; }
  bool minimized() const { return minimized_; }

  // Non-blocking event pump: drains SDL events so input_backend_sdl3 sees updated keyboard/mouse/pad state.
  WindowEvents poll_events();

  // Computes ultrawide aspect factor (> 16:9 -> clamped to [1.0, 4.0]).
  static float compute_ultrawide_factor(int pixel_width, int pixel_height);

 private:
  void sync_sizes();

  SDL_Window* window_ = nullptr;
  uint32_t window_id_ = 0;
  bool sdl_video_owned_ = false;
  bool fullscreen_ = false;
  bool quit_requested_ = false;
  bool focused_ = false;
  bool minimized_ = false;
  int width_ = 0;
  int height_ = 0;
  int pixel_width_ = 0;
  int pixel_height_ = 0;
};

}  // namespace snap_linux
