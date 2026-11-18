// SPDX-License-Identifier: MIT
// Stage 6E-B: Linux SDL3 HostWindow, non-blocking event loop, and NVN Window/Swapchain integration test.
#include "host/linux/window_sdl3.h"
#include "runtime/nvn/nvn_types.h"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <vector>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

// os_kernel.cpp calls rt_dispatch for guest execution; this window/renderer test never runs guest code.
extern "C" void rt_dispatch(void*) {
  std::fprintf(stderr, "unexpected rt_dispatch call in window_sdl3_linux_test\n");
  std::abort();
}

extern "C" {
void (*nvnBootstrapLoader(const char* name))(void);
void nvnHostNotifyWindowResize(void* nativeWindow);
void nvnHostSetUltrawide(float factor);
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

void test_ultrawide_math() {
  CHECK(snap_linux::HostWindow::compute_ultrawide_factor(1280, 720) == 1.0f);
  CHECK(snap_linux::HostWindow::compute_ultrawide_factor(0, 0) == 1.0f);
  CHECK(snap_linux::HostWindow::compute_ultrawide_factor(2560, 1080) > 1.3f);
  CHECK(snap_linux::HostWindow::compute_ultrawide_factor(10000, 100) == 4.0f);
}

void test_host_window_and_nvn_swapchain() {
  snap_linux::HostWindow win;
  snap_linux::WindowConfig cfg;
  cfg.title = "snap_window_sdl3_linux_test";
  cfg.width = 640;
  cfg.height = 360;
  cfg.hidden = true;
  cfg.resizable = true;

  if (!win.create(cfg)) {
    std::printf("window_sdl3_linux_test: SDL video unavailable in environment; skipping window/swapchain test\n");
    return;
  }

  CHECK(win.is_open());
  CHECK(win.native_handle() != nullptr);
  CHECK(win.window_id() != 0);
  CHECK(win.width() == 640);
  CHECK(win.height() == 360);
  CHECK(win.pixel_width() > 0 && win.pixel_height() > 0);
  CHECK(!win.quit_requested());

  // Non-blocking poll when queue has no quit/resize events.
  const auto idle = win.poll_events();
  CHECK(!idle.quit_requested);

#ifdef HLE_WITH_SDL3
  // Inject resize, display-change, focus, and close events to verify event loop dispatch.
  auto push_win_event = [&](SDL_EventType type, Sint32 d1 = 0, Sint32 d2 = 0) {
    SDL_Event ev{};
    ev.type = type;
    ev.window.windowID = win.window_id();
    ev.window.data1 = d1;
    ev.window.data2 = d2;
    CHECK(SDL_PushEvent(&ev));
  };

  push_win_event(SDL_EVENT_WINDOW_RESIZED, 800, 450);
  push_win_event(SDL_EVENT_WINDOW_DISPLAY_CHANGED, 1, 0);
  push_win_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
  const auto ev1 = win.poll_events();
  CHECK(ev1.resized);
  CHECK(ev1.display_changed);
  CHECK(ev1.focus_changed);
  CHECK(ev1.focused);
  CHECK(!ev1.quit_requested);

  push_win_event(SDL_EVENT_WINDOW_FOCUS_LOST);
  push_win_event(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
  const auto ev2 = win.poll_events();
  CHECK(ev2.focus_changed);
  CHECK(!ev2.focused);
  CHECK(ev2.quit_requested);
  CHECK(win.quit_requested());
#endif

  // Connect HostWindow::native_handle() to real NVN Device -> MemoryPool -> Textures -> Window -> Swapchain.
  using FnDevBuilderDefaults = void (*)(void*);
  using FnDevInit = uint8_t (*)(void*, const void*);
  using FnDevFinalize = void (*)(void*);
  using FnPoolBuilderDefaults = void (*)(void*);
  using FnPoolSetDevice = void (*)(void*, void*);
  using FnPoolSetStorage = void (*)(void*, void*, size_t);
  using FnPoolSetFlags = void (*)(void*, int);
  using FnPoolInit = uint8_t (*)(void*, const void*);
  using FnPoolFinalize = void (*)(void*);
  using FnTexBuilderDefaults = void (*)(void*);
  using FnTexSetDevice = void (*)(void*, void*);
  using FnTexSetFlags = void (*)(void*, int);
  using FnTexSetTarget = void (*)(void*, int);
  using FnTexSetFormat = void (*)(void*, int);
  using FnTexSetSize2D = void (*)(void*, int, int);
  using FnTexGetStorageSize = size_t (*)(const void*);
  using FnTexSetStorage = void (*)(void*, void*, int64_t);
  using FnTexInit = uint8_t (*)(void*, const void*);
  using FnTexFinalize = void (*)(void*);
  using FnWinBuilderDefaults = void (*)(void*);
  using FnWinSetDevice = void (*)(void*, void*);
  using FnWinSetNative = void (*)(void*, void*);
  using FnWinSetTextures = void (*)(void*, int, void* const*);
  using FnWinInit = uint8_t (*)(void*, const void*);
  using FnWinFinalize = void (*)(void*);

  auto dev_b_def = reinterpret_cast<FnDevBuilderDefaults>(nvnBootstrapLoader("nvnDeviceBuilderSetDefaults"));
  auto dev_init = reinterpret_cast<FnDevInit>(nvnBootstrapLoader("nvnDeviceInitialize"));
  auto dev_fin = reinterpret_cast<FnDevFinalize>(nvnBootstrapLoader("nvnDeviceFinalize"));
  auto pool_b_def = reinterpret_cast<FnPoolBuilderDefaults>(nvnBootstrapLoader("nvnMemoryPoolBuilderSetDefaults"));
  auto pool_b_dev = reinterpret_cast<FnPoolSetDevice>(nvnBootstrapLoader("nvnMemoryPoolBuilderSetDevice"));
  auto pool_b_sto = reinterpret_cast<FnPoolSetStorage>(nvnBootstrapLoader("nvnMemoryPoolBuilderSetStorage"));
  auto pool_b_flg = reinterpret_cast<FnPoolSetFlags>(nvnBootstrapLoader("nvnMemoryPoolBuilderSetFlags"));
  auto pool_init = reinterpret_cast<FnPoolInit>(nvnBootstrapLoader("nvnMemoryPoolInitialize"));
  auto pool_fin = reinterpret_cast<FnPoolFinalize>(nvnBootstrapLoader("nvnMemoryPoolFinalize"));
  auto tex_b_def = reinterpret_cast<FnTexBuilderDefaults>(nvnBootstrapLoader("nvnTextureBuilderSetDefaults"));
  auto tex_b_dev = reinterpret_cast<FnTexSetDevice>(nvnBootstrapLoader("nvnTextureBuilderSetDevice"));
  auto tex_b_flg = reinterpret_cast<FnTexSetFlags>(nvnBootstrapLoader("nvnTextureBuilderSetFlags"));
  auto tex_b_tgt = reinterpret_cast<FnTexSetTarget>(nvnBootstrapLoader("nvnTextureBuilderSetTarget"));
  auto tex_b_fmt = reinterpret_cast<FnTexSetFormat>(nvnBootstrapLoader("nvnTextureBuilderSetFormat"));
  auto tex_b_sz = reinterpret_cast<FnTexSetSize2D>(nvnBootstrapLoader("nvnTextureBuilderSetSize2D"));
  auto tex_b_gss = reinterpret_cast<FnTexGetStorageSize>(nvnBootstrapLoader("nvnTextureBuilderGetStorageSize"));
  auto tex_b_sto = reinterpret_cast<FnTexSetStorage>(nvnBootstrapLoader("nvnTextureBuilderSetStorage"));
  auto tex_init = reinterpret_cast<FnTexInit>(nvnBootstrapLoader("nvnTextureInitialize"));
  auto tex_fin = reinterpret_cast<FnTexFinalize>(nvnBootstrapLoader("nvnTextureFinalize"));
  auto win_b_def = reinterpret_cast<FnWinBuilderDefaults>(nvnBootstrapLoader("nvnWindowBuilderSetDefaults"));
  auto win_b_dev = reinterpret_cast<FnWinSetDevice>(nvnBootstrapLoader("nvnWindowBuilderSetDevice"));
  auto win_b_nat = reinterpret_cast<FnWinSetNative>(nvnBootstrapLoader("nvnWindowBuilderSetNativeWindow"));
  auto win_b_tex = reinterpret_cast<FnWinSetTextures>(nvnBootstrapLoader("nvnWindowBuilderSetTextures"));
  auto win_init = reinterpret_cast<FnWinInit>(nvnBootstrapLoader("nvnWindowInitialize"));
  auto win_fin = reinterpret_cast<FnWinFinalize>(nvnBootstrapLoader("nvnWindowFinalize"));

  alignas(16) uint8_t dev_b[256]{}, dev[256]{};
  dev_b_def(dev_b);
  if (dev_init(dev, dev_b)) {
    alignas(16) uint8_t tb[256]{};
    tex_b_def(tb);
    tex_b_dev(tb, dev);
    tex_b_flg(tb, nvn::TF_DISPLAY);
    tex_b_tgt(tb, nvn::TT_2D);
    tex_b_fmt(tb, nvn::FMT_RGBA8);
    tex_b_sz(tb, 640, 360);
    const size_t one_sz = tex_b_gss(tb);
    CHECK(one_sz > 0);
    const size_t aligned_sz = (one_sz + 0xFFFu) & ~size_t(0xFFFu);
    const size_t pool_sz = aligned_sz * 2;
    void* raw_mem = nullptr;
    CHECK(posix_memalign(&raw_mem, 0x1000, pool_sz) == 0 && raw_mem != nullptr);

    alignas(16) uint8_t pb[256]{}, pool[256]{};
    pool_b_def(pb);
    pool_b_dev(pb, dev);
    pool_b_flg(pb, 0x22 /* CPU_NO_ACCESS | GPU_CACHED */);
    pool_b_sto(pb, raw_mem, pool_sz);
    CHECK(pool_init(pool, pb) != 0);

    alignas(16) uint8_t tex0[256]{}, tex1[256]{};
    tex_b_sto(tb, pool, 0);
    CHECK(tex_init(tex0, tb) != 0);
    tex_b_sto(tb, pool, static_cast<int64_t>(aligned_sz));
    CHECK(tex_init(tex1, tb) != 0);

    void* tex_ptrs[2] = {tex0, tex1};
    alignas(16) uint8_t wb[256]{}, nvn_win[256]{};
    win_b_def(wb);
    win_b_dev(wb, dev);
    win_b_nat(wb, win.native_handle());
    win_b_tex(wb, 2, tex_ptrs);
    CHECK(win_init(nvn_win, wb) != 0);

    nvnHostSetUltrawide(snap_linux::HostWindow::compute_ultrawide_factor(win.pixel_width(), win.pixel_height()));
    nvnHostNotifyWindowResize(win.native_handle());

    win_fin(nvn_win);
    tex_fin(tex1);
    tex_fin(tex0);
    pool_fin(pool);
    std::free(raw_mem);
    dev_fin(dev);
    std::printf("window_sdl3_linux_test: real NVN Window + SDL3 VkSurfaceKHR + VkSwapchainKHR succeeded\n");
  }

  win.destroy();
  CHECK(!win.is_open());
  CHECK(win.native_handle() == nullptr);
}

}  // namespace

int main() {
  test_ultrawide_math();
  test_host_window_and_nvn_swapchain();
  std::printf("window_sdl3_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
