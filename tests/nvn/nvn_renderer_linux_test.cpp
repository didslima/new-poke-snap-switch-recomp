// SPDX-License-Identifier: MIT
// Stage 6E-A Group 2E: Linux NVN->Vulkan renderer link-closure & initialization validation.
// Links against the complete production portable runtime ($<TARGET_OBJECTS:snap_portable_objects>)
// without running any guest/game code.
#include "nvn_os_host.h"
#include "nvn_os_host_vk.h"
#include "nvn_host.h"
#include "hle/live.h"
#include "hle/settings.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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

extern "C" {
unsigned snap_mods_ab_period();
bool snap_mods_ab_live(unsigned present);
const uint8_t* snap_mods_ab_original(const uint8_t* src, uint32_t w, uint32_t h, uint32_t mips,
                                     uint32_t* ow, uint32_t* oh, uint32_t* om);
void snap_shiny_fx_trigger(const char* species);
int snap_live_size_blocked(void);
}

void test_live_graphics_and_ultrawide() {
  // Isolate settings writes into a temporary SNAP_CONFIG_DIR so user settings are untouched.
  const std::filesystem::path tmp_cfg = std::filesystem::temp_directory_path() / "snap_nvn_live_test_cfg";
  std::error_code ec;
  std::filesystem::remove_all(tmp_cfg, ec);
  std::filesystem::create_directories(tmp_cfg, ec);
  const char* prev_cfg = std::getenv("SNAP_CONFIG_DIR");
  const std::string saved_cfg = prev_cfg ? prev_cfg : "";
  setenv("SNAP_CONFIG_DIR", tmp_cfg.c_str(), 1);
  hle::settings::load();

  std::atomic<unsigned> seen{0};
  (void)hle::live::changed(seen);

  // Verify each v1.3.0 live-apply graphics setting bumps the live generation via hle::settings::set.
  const struct { const char* k; const char* v; } live_cases[] = {
      {"gfx_render_scale", "1.5"},
      {"gfx_aa", "smaa"},
      {"gfx_shadow_res", "2048"},
      {"gfx_ultrawide", "off"},
      {"gfx_ultrawide", "auto"},
      {"ssao_quality", "high"},
  };
  for (const auto& tc : live_cases) {
    hle::settings::set(tc.k, tc.v);
    CHECK(hle::live::changed(seen));
    CHECK(!hle::live::changed(seen));
  }

  if (saved_cfg.empty()) unsetenv("SNAP_CONFIG_DIR");
  else setenv("SNAP_CONFIG_DIR", saved_cfg.c_str(), 1);
  std::filesystem::remove_all(tmp_cfg, ec);

  // Verify snap_live_size_blocked() reports 0 when no upscaler/framegen is active.
  CHECK(snap_live_size_blocked() == 0);

  // Verify live ultrawide factor updates (21:9, 32:9, and Off = 1.0) without caching.
  const float orig_uw = nvnHostGetUltrawide();
  nvnHostSetUltrawide(1.34f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 1.34f) < 0.001f);
  nvnHostSetUltrawide(2.0f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 2.0f) < 0.001f);
  nvnHostSetUltrawide(1.0f);
  CHECK(std::fabs(nvnHostGetUltrawide() - 1.0f) < 0.001f);
  nvnHostSetUltrawide(orig_uw);
}

void test_optional_stub_functions() {
  CHECK(snap_mods_ab_period() == 0u);
  CHECK(!snap_mods_ab_live(0));
  CHECK(!snap_mods_ab_live(120));
  uint32_t ow = 1, oh = 1, om = 1;
  const uint8_t dummy[4] = {};
  CHECK(snap_mods_ab_original(dummy, 64, 64, 1, &ow, &oh, &om) == nullptr);
  snap_shiny_fx_trigger("Pikachu");
}

}  // namespace

int main() {
  test_nvn_bootstrap_table();
  test_vulkan_device_and_sdl_surface();
  test_optional_stub_functions();
  test_live_graphics_and_ultrawide();
  std::printf("nvn_renderer_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
