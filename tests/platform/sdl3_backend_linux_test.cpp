// SPDX-License-Identifier: MIT
// Stage 6C-B2: isolated tests for Linux SDL3 input backend (and NullBackend when built headless without SDL3).
// Never depends on a physical controller or an SDL window; never executes guest code.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

extern "C" void os_get_stats(void*) {}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_backend_lifecycle_and_headless() {
    auto b = hle::input::make_default_backend();
    CHECK(b != nullptr);
#ifdef HLE_WITH_SDL3
    CHECK(std::string(b->name()) == "sdl3");
#else
    CHECK(std::string(b->name()) == "none");
#endif

    // Safe poll & rumble with no window and no physical device.
    hle::input::RawInput raw;
    b->poll(raw);
#ifdef HLE_WITH_SDL3
    CHECK(!raw.focused);
#else
    CHECK(!raw.pad_connected);
#endif
    CHECK(raw.keys.none());
    CHECK(raw.mouse_buttons == 0);
    CHECK(!raw.mouse_in_window);
    b->rumble(0.5f, 0.5f);
    b->rumble(0.0f, 0.0f);

    // Clean destruction and re-creation.
    b.reset();
    b = hle::input::make_default_backend();
    CHECK(b != nullptr);
    raw = hle::input::RawInput{};
    b->poll(raw);
    b.reset();
}

#ifdef HLE_WITH_SDL3
static void test_sdl3_virtual_hotplug_and_disconnect() {
    auto b = hle::input::make_default_backend();
    CHECK(b != nullptr && std::string(b->name()) == "sdl3");

    // Ensure SDL initialized before attaching virtual joystick.
    hle::input::RawInput raw;
    b->poll(raw);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.name = "SnapTestVirtualPad";

    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (vid == 0) {
        std::fprintf(stderr, "sdl3_backend_linux_test: virtual joystick unsupported in this environment (%s); skipping virtual hotplug checks\n",
                     SDL_GetError());
        return;
    }

    // 1. Hotplug discovery (pads_ was empty, so poll() scans immediately).
    raw = hle::input::RawInput{};
    b->poll(raw);
    CHECK(raw.pad_connected);

    // 2. Drive virtual button + axis and verify Hub receives mapped input.
    SDL_Joystick* vjoy = SDL_OpenJoystick(vid);
    CHECK(vjoy != nullptr);
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, true);
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -32767);  // up (negative in SDL -> positive in RawInput.ly)
    }

    hle::input::Hub hub;
    hub.set_backend(std::move(b));
    hub.set_mapping(hle::input::Mapping::defaults());
    hub.poll(0);

    const auto r = hub.raw();
    CHECK(r.pad_connected);
    CHECK(r.pad.test(hle::input::kPadEast));
    CHECK(r.ly > 0.9f);
    CHECK((hub.pad().buttons & (1ull << hle::input::kA)) != 0);
    CHECK(hub.pad().ly > 20000);

    // 3. Rumble fallback on virtual device without rumble capability must not crash.
    hub.rumble(0.75f, 0.25f);
    hub.rumble(0.0f, 0.0f);

    // 4. Disconnect handling.
    if (vjoy) SDL_CloseJoystick(vjoy);
    CHECK(SDL_DetachVirtualJoystick(vid));
    hub.poll(0);
    CHECK(!hub.raw().pad_connected);
}
#endif

int main() {
    test_backend_lifecycle_and_headless();
#ifdef HLE_WITH_SDL3
    test_sdl3_virtual_hotplug_and_disconnect();
#endif
    std::printf("sdl3_backend_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
