// SPDX-License-Identifier: MIT
// Stage 6D-B: Linux integration tests for Sdl3AudioBackend (dummy driver, channels, hotplug, NullBackend fallback).
#include "audio_internal.h"
#include "audio_out.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace fs = std::filesystem;

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

struct TempDir {
  fs::path path;
  explicit TempDir(const char* prefix) {
    path = fs::temp_directory_path() / (std::string(prefix) + "_" + std::to_string((long long)::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct EnvGuard {
  std::string name;
  bool had = false;
  std::string old;
  EnvGuard(const char* n, const char* val) : name(n) {
    if (const char* cur = std::getenv(n)) {
      had = true;
      old = cur;
    }
    if (val) ::setenv(n, val, 1);
    else ::unsetenv(n);
  }
  ~EnvGuard() {
    if (had) ::setenv(name.c_str(), old.c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

SnapAudioOutBuffer* wait_released(void* audioOut, int timeout_ms = 1000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (SnapAudioOutBuffer* b = snap_nn_audio_GetReleasedAudioOutBuffer(audioOut)) return b;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

void test_sdl3_lifecycle_and_callback() {
  TempDir tmp("snap_audio_sdl3");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "sdl3");
  EnvGuard g3("SDL_AUDIO_DRIVER", "dummy");

  // 5 repeated init/shutdown cycles
  for (int i = 0; i < 5; ++i) {
    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
    cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
    cfg.channelMode = SNAP_AUDIO_CH_STEREO;
    cfg.enableTap = 1;
    snap_audio_configure(&cfg);

    CHECK(snap_audio_game_channels() == 2);
    SnapAudioStats st{};
    snap_audio_get_stats(&st);
    CHECK(st.backendInUse == SNAP_AUDIO_BACKEND_SDL3);
    CHECK(st.deviceAttached == 1);
    CHECK(st.deviceRate == 48000);
    CHECK(st.deviceChannels == 2);
    CHECK(snap_audio_spatial_available() == 0);
    snap_audio_shutdown();
  }

  // Real SDL3 callback pull -> core_mix -> tap
  SnapAudioConfig cfg;
  snap_audio_default_config(&cfg);
  cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
  cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
  cfg.channelMode = SNAP_AUDIO_CH_STEREO;
  cfg.enableTap = 1;
  snap_audio_configure(&cfg);

  alignas(16) uint8_t audioOut[0x168] = {};
  SnapAudioOutParameter param{};
  snap_nn_audio_InitializeAudioOutParameter(&param);
  param.sampleRate = 48000;
  param.channelCount = 2;

  CHECK(snap_nn_audio_OpenDefaultAudioOut(audioOut, &param) == 0);
  CHECK(snap_nn_audio_StartAudioOut(audioOut) == 0);

  std::vector<int16_t> pcm(960 * 2, 16384);
  SnapAudioOutBuffer buf{};
  snap_nn_audio_SetAudioOutBufferInfo(&buf, pcm.data(), pcm.size() * sizeof(int16_t), pcm.size() * sizeof(int16_t));

  snap_audio_set_volume(SNAP_VOL_MASTER, 1.0f);
  snap_audio_set_muted(0);
  std::vector<float> drain(4096);
  while (snap_audio_tap_read(drain.data(), drain.size()) > 0) {}

  snap_nn_audio_AppendAudioOutBuffer(audioOut, &buf);
  CHECK(wait_released(audioOut) == &buf);

  std::vector<float> tap(8192);
  const uint64_t n = snap_audio_tap_read(tap.data(), tap.size());
  CHECK(n > 0);
  float peak = 0.0f;
  for (uint64_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(tap[i]));
  CHECK(peak > 0.2f);

  snap_nn_audio_StopAudioOut(audioOut);
  snap_nn_audio_CloseAudioOut(audioOut);
  snap_audio_shutdown();
}

void test_sdl3_channels_and_fallback() {
  TempDir tmp("snap_audio_sdl3_ch");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "sdl3");
  EnvGuard g3("SDL_AUDIO_DRIVER", "dummy");

  for (int spk : {6, 8}) {
    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
    cfg.outputMode = SNAP_AUDIO_OUT_SPEAKERS;
    cfg.channelMode = SNAP_AUDIO_CH_SURROUND51;
    cfg.speakerChannels = spk;
    snap_audio_configure(&cfg);

    CHECK(snap_audio_game_channels() == 6);
    SnapAudioStats st{};
    snap_audio_get_stats(&st);
    CHECK(st.backendInUse == SNAP_AUDIO_BACKEND_SDL3);
    CHECK(st.deviceAttached == 1);
    CHECK(st.deviceChannels == static_cast<uint32_t>(spk) || st.deviceChannels == 2);
    snap_audio_shutdown();
  }
}

void test_sdl3_device_notification() {
  TempDir tmp("snap_audio_sdl3_ev");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "sdl3");
  EnvGuard g3("SDL_AUDIO_DRIVER", "dummy");

  static std::atomic<int> signaled{0};
  signaled.store(0);
  int dummy_event = 42;
  snap_audio_set_os_hooks([](void* ev) {
    if (ev) signaled.fetch_add(1);
  });
  snap_nn_audio_AcquireAudioDeviceNotificationForOutput(&dummy_event);

  SnapAudioConfig cfg;
  snap_audio_default_config(&cfg);
  cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
  cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
  snap_audio_configure(&cfg);
  CHECK(snap_audio_game_channels() == 2);

#ifdef HLE_WITH_SDL3
  const int before = signaled.load();
  SDL_Event ev{};
  ev.type = SDL_EVENT_AUDIO_DEVICE_ADDED;
  ev.adevice.which = 1;
  ev.adevice.recording = false;
  SDL_PushEvent(&ev);
  SDL_PumpEvents();
  CHECK(signaled.load() > before);
#endif

  snap_audio_shutdown();
  snap_audio_set_os_hooks(nullptr);
}

void test_sdl3_failure_falls_back_to_null() {
  // Fork so SDL_AUDIO_DRIVER=invalid_nonexistent runs in a fresh process without affecting parent SDL state.
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    TempDir tmp("snap_audio_sdl3_fail");
    ::setenv("SNAP_CONFIG_DIR", tmp.path.c_str(), 1);
    ::setenv("SNAP_AUDIO_BACKEND", "auto", 1);
    ::setenv("SDL_AUDIO_DRIVER", "invalid_nonexistent_driver", 1);

    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_AUTO;
    cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
    snap_audio_configure(&cfg);

    const int ch = snap_audio_game_channels();
    SnapAudioStats st{};
    snap_audio_get_stats(&st);
    snap_audio_shutdown();

    const bool ok = (ch == 2) && (st.backendInUse == SNAP_AUDIO_BACKEND_NULL) && (st.deviceAttached == 0);
    ::_exit(ok ? 0 : 1);
  }
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace

int main() {
  test_sdl3_failure_falls_back_to_null();
  test_sdl3_lifecycle_and_callback();
  test_sdl3_channels_and_fallback();
  test_sdl3_device_notification();
  std::printf("audio_sdl3_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
