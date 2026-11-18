// SPDX-License-Identifier: MIT
// Stage 6E-A Group 2E: Linux NVN->Vulkan renderer link-closure & initialization validation.
// Links against the complete production portable runtime ($<TARGET_OBJECTS:snap_portable_objects>)
// without running any guest/game code.
#include "nvn_os_host.h"
#include "nvn_os_host_vk.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

// os_kernel.cpp calls rt_dispatch for guest execution; this renderer link test never runs guest code.
extern "C" void rt_dispatch(void*) {
  std::fprintf(stderr, "unexpected rt_dispatch call in nvn_renderer_linux_test\n");
  std::abort();
}

extern "C" {
void (*nvnBootstrapLoader(const char* name))(void);
void nvnHostSetFramePacing(double hz);
}

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* msg, int line) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, msg);
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_nvn_bootstrap_table() {
  CHECK(nvnBootstrapLoader("nvnDeviceInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnDeviceFinalize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnDeviceGetProcAddress") != nullptr);
  CHECK(nvnBootstrapLoader("nvnQueueInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnWindowInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnCommandBufferDrawArrays") != nullptr);
  CHECK(nvnBootstrapLoader("nvnTextureInitialize") != nullptr);
  CHECK(nvnBootstrapLoader("nvnSyncWait") != nullptr);
  CHECK(nvnBootstrapLoader("nonexistent_entry_point") == nullptr);
  nvnHostSetFramePacing(60.0);
}

void test_vulkan_device_and_sdl_surface() {
#ifdef HLE_WITH_SDL3
  const bool sdl_ok = SDL_Init(SDL_INIT_VIDEO);
#else
  const bool sdl_ok = false;
#endif

  using FnDevBuilderSetDefaults = void (*)(void*);
  using FnDevInit = uint8_t (*)(void*, const void*);
  using FnDevFinalize = void (*)(void*);

  auto dev_builder_defaults = reinterpret_cast<FnDevBuilderSetDefaults>(nvnBootstrapLoader("nvnDeviceBuilderSetDefaults"));
  auto dev_init = reinterpret_cast<FnDevInit>(nvnBootstrapLoader("nvnDeviceInitialize"));
  auto dev_finalize = reinterpret_cast<FnDevFinalize>(nvnBootstrapLoader("nvnDeviceFinalize"));
  CHECK(dev_builder_defaults != nullptr && dev_init != nullptr && dev_finalize != nullptr);

  // Storage for opaque NVNDevice / NVNDeviceBuilder (host layer only stores pointers in HandleMap).
  alignas(16) uint8_t dev_builder[256]{};
  alignas(16) uint8_t dev[256]{};
  dev_builder_defaults(dev_builder);

  // Only attempt real VkDevice creation when a Vulkan 1.3 physical device exists;
  // in headless/no-ICD environments, DeviceInitialize must return 0 cleanly without crashing.
  const uint8_t init_ok = dev_init(dev, dev_builder);
  if (init_ok) {
    std::printf("nvn_renderer_linux_test: real Vulkan 1.3 Host::Init succeeded\n");
#ifdef HLE_WITH_SDL3
    if (sdl_ok) {
      SDL_Window* win = SDL_CreateWindow("nvn_renderer_linux_test", 128, 128, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
      CHECK(win != nullptr);
      if (win) {
        nvn::os_host::PaceDisplayInfo dinfo{};
        (void)nvn::os_host::pace_query_display(win, 0, &dinfo);
        SDL_DestroyWindow(win);
      }
    }
#endif
    dev_finalize(dev);
  } else {
    std::printf("nvn_renderer_linux_test: no Vulkan 1.3 ICD/GPU available; DeviceInitialize returned 0 cleanly\n");
  }

#ifdef HLE_WITH_SDL3
  if (sdl_ok) SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
}

}  // namespace

int main() {
  test_nvn_bootstrap_table();
  test_vulkan_device_and_sdl_surface();
  std::printf("nvn_renderer_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
