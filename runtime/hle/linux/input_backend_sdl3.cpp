// SPDX-License-Identifier: MIT
// Stage 6C-B2: Linux SDL3 input backend (and NullBackend when built headless without SDL3).
// Replaces the weak Stage 6C-B1 make_default_backend() in hle_host_posix.cpp.
#if !defined(__linux__)
#error "runtime/hle/linux/input_backend_sdl3.cpp is the Linux implementation of make_default_backend()"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "../hle.h"
#include "../input_core.h"

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace hle::input {

#ifdef HLE_WITH_SDL3
namespace {

void merge_pad(RawInput& dst, const RawInput& src) {
  if (!src.pad_connected) return;
  dst.pad_connected = true;
  if (dst.pad_family == kPadFamNone) dst.pad_family = src.pad_family;
  dst.pad |= src.pad;
  auto big = [](float& d, float s) {
    if (std::fabs(s) > std::fabs(d)) d = s;
  };
  big(dst.lx, src.lx);
  big(dst.ly, src.ly);
  big(dst.rx, src.rx);
  big(dst.ry, src.ry);
  dst.lt = std::max(dst.lt, src.lt);
  dst.rt = std::max(dst.rt, src.rt);
  if (src.gyro_present) {
    dst.gyro_present = true;
    dst.gyro_x = src.gyro_x;
    dst.gyro_y = src.gyro_y;
    dst.gyro_z = src.gyro_z;
  }
}

int family_of(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return kPadFamPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return kPadFamSwitch;
    default:
      return kPadFamXbox;
  }
}

class Sdl3Backend final : public Backend {
 public:
  Sdl3Backend() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    inited_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
    if (!inited_) log("sdl3: gamepad init failed: %s", SDL_GetError());
  }

  ~Sdl3Backend() override {
    for (SDL_Gamepad* p : pads_) {
      if (p) SDL_CloseGamepad(p);
    }
    pads_.clear();
    if (inited_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
  }

  const char* name() const override { return "sdl3"; }

  void poll(RawInput& out) override {
    if (!inited_) return;
    const uint64_t now_ns = static_cast<uint64_t>(SDL_GetTicksNS());
    if (!last_scan_ns_ || pads_.empty() || now_ns - last_scan_ns_ > 1000000000ull) {
      last_scan_ns_ = now_ns;
      scan_pads();
    }
    SDL_UpdateGamepads();

    // Drop any pad that disconnected between scans.
    for (size_t i = 0; i < pads_.size();) {
      if (!SDL_GamepadConnected(pads_[i])) {
        SDL_CloseGamepad(pads_[i]);
        pads_.erase(pads_.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }

    for (SDL_Gamepad* pad : pads_) {
      RawInput r;
      r.pad_connected = true;
      r.pad_family = family_of(SDL_GetGamepadType(pad));
      auto b = [&](SDL_GamepadButton sb, int bit) {
        if (SDL_GetGamepadButton(pad, sb)) r.pad.set(static_cast<size_t>(bit));
      };
      b(SDL_GAMEPAD_BUTTON_SOUTH, kPadSouth);
      b(SDL_GAMEPAD_BUTTON_EAST, kPadEast);
      b(SDL_GAMEPAD_BUTTON_WEST, kPadWest);
      b(SDL_GAMEPAD_BUTTON_NORTH, kPadNorth);
      b(SDL_GAMEPAD_BUTTON_BACK, kPadBack);
      b(SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide);
      b(SDL_GAMEPAD_BUTTON_START, kPadStart);
      b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLStick);
      b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRStick);
      b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLShoulder);
      b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRShoulder);
      b(SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDUp);
      b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDDown);
      b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDLeft);
      b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDRight);
      auto ax = [&](SDL_GamepadAxis a) {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f, -1.0f, 1.0f);
      };
      r.lx = ax(SDL_GAMEPAD_AXIS_LEFTX);
      r.ly = -ax(SDL_GAMEPAD_AXIS_LEFTY);
      r.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX);
      r.ry = -ax(SDL_GAMEPAD_AXIS_RIGHTY);
      r.lt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
      r.rt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

      if (pad_fx().gyro_on.load() && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        float g[3] = {0.0f, 0.0f, 0.0f};
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3)) {
          constexpr float kDeg = 57.29578f;
          r.gyro_present = true;
          r.gyro_x = g[0] * kDeg;
          r.gyro_y = g[1] * kDeg;
          r.gyro_z = g[2] * kDeg;
        }
      }
      merge_pad(out, r);
    }

    // Keyboard and mouse only when an SDL window has keyboard focus.
    out.focused = SDL_GetKeyboardFocus() != nullptr;
    if (out.focused) {
      int n = 0;
      const bool* ks = SDL_GetKeyboardState(&n);
      auto key = [&](int id, SDL_Scancode sc) {
        if (ks && sc < n && ks[sc]) out.keys.set(static_cast<size_t>(id));
      };
      for (int k = 1; k <= 26; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_A + (k - 1)));
      key(27, SDL_SCANCODE_0);
      for (int k = 28; k <= 36; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (k - 28)));
      for (int k = 37; k <= 48; ++k) key(k, static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (k - 37)));
      static const SDL_Scancode fixed[] = {
          SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
          SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_BACKSPACE,
          SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LALT,
          SDL_SCANCODE_RALT, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_SEMICOLON,
          SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_MINUS,
          SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_GRAVE, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
          SDL_SCANCODE_HOME, SDL_SCANCODE_END, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN};
      constexpr int nf = static_cast<int>(sizeof fixed / sizeof fixed[0]);
      for (int i = 0; i < nf; ++i) key(49 + i, fixed[i]);
      key(49 + nf, SDL_SCANCODE_KP_0);
      for (int i = 1; i <= 9; ++i) key(49 + nf + i, static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + (i - 1)));

      float mx = 0.0f, my = 0.0f;
      const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
      if (mb & SDL_BUTTON_LMASK) out.mouse_buttons |= 1;
      if (mb & SDL_BUTTON_RMASK) out.mouse_buttons |= 2;
      if (mb & SDL_BUTTON_MMASK) out.mouse_buttons |= 4;
      SDL_Window* w = SDL_GetMouseFocus();
      int ww = 0, wh = 0;
      if (w && SDL_GetWindowSize(w, &ww, &wh) && ww > 0 && wh > 0) {
        out.mouse_in_window = true;
        out.mouse_x = mx / static_cast<float>(ww);
        out.mouse_y = my / static_cast<float>(wh);
      }
      float dx = 0.0f, dy = 0.0f;
      SDL_GetRelativeMouseState(&dx, &dy);
      out.mouse_dx = dx;
      out.mouse_dy = dy;
    }
  }

  void rumble(float low, float high) override {
    if (!inited_ || pads_.empty()) return;
    const Uint16 lo = static_cast<Uint16>(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
    const Uint16 hi = static_cast<Uint16>(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
    for (SDL_Gamepad* pad : pads_) {
      if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) continue;
      SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 500 : 0);
    }
  }

 private:
  void scan_pads() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    for (int k = 0; k < n; ++k) {
      const SDL_JoystickID id = ids[k];
      if (!SDL_IsGamepad(id)) continue;
      bool already_open = false;
      for (SDL_Gamepad* g : pads_) {
        if (SDL_GetGamepadID(g) == id) {
          already_open = true;
          break;
        }
      }
      if (already_open) continue;
      if (SDL_Gamepad* g = SDL_OpenGamepad(id)) pads_.push_back(g);
    }
    SDL_free(ids);
  }

  bool inited_ = false;
  uint64_t last_scan_ns_ = 0;
  std::vector<SDL_Gamepad*> pads_;
};

}  // namespace
#endif

std::unique_ptr<Backend> make_default_backend() {
#ifdef HLE_WITH_SDL3
  return std::make_unique<Sdl3Backend>();
#else
  struct NullBackend final : Backend {
    const char* name() const override { return "none"; }
    void poll(RawInput&) override {}
  };
  return std::make_unique<NullBackend>();
#endif
}

}  // namespace hle::input
