// SPDX-License-Identifier: MIT
// Stage 6D-D: Audio runtime validation (SDL3 Input+Audio coexistence, synthetic ABI wrappers,
// OS SystemEvent signaling/try_wait path, and optional real-driver probe — zero guest code executed).
#include "audio/audio_internal.h"
#include "audio/audio_out.h"
#include "hle/input_core.h"
#include "loader.h"
#include "os/os.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

extern "C" {
extern const uint64_t main_fn_addrs[1] = {0};
extern const GuestFn main_fn_ptrs[1] = {nullptr};
extern const uint64_t main_fn_count = 0;
extern const char* const main_import_names[1] = {""};
extern const uint64_t main_import_stub_addrs[1] = {0};
extern const uint64_t main_import_count = 0;
extern const uint64_t sdk_fn_addrs[1] = {0};
extern const GuestFn sdk_fn_ptrs[1] = {nullptr};
extern const uint64_t sdk_fn_count = 0;
}

int snap_bind_audio();

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
  std::filesystem::path path;
  explicit TempDir(const char* prefix) {
    path = std::filesystem::temp_directory_path() /
           (std::string(prefix) + "_" + std::to_string((long long)::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
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

GuestFn find_os_override(const char* sym) {
  size_t n = 0;
  const OsOverride* tab = os_override_table(&n);
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(tab[i].sym, sym) == 0) return tab[i].fn;
  }
  return nullptr;
}

GuestFn find_audio_override(const char* sym) {
  Thunk* t = loader::find_override(sym);
  return t ? t->fn1 : nullptr;
}

// 1. Optional probe of real Linux audio drivers (runs in forked child before parent touches SDL).
void test_real_driver_probe() {
#ifdef HLE_WITH_SDL3
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::unsetenv("SDL_AUDIO_DRIVER");
    const int ndrv = SDL_GetNumAudioDrivers();
    std::fprintf(stderr, "[probe] SDL3 audio drivers compiled (%d):", ndrv);
    for (int i = 0; i < ndrv; ++i) std::fprintf(stderr, " %s", SDL_GetAudioDriver(i));
    std::fprintf(stderr, "\n");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      const char* cur = SDL_GetCurrentAudioDriver();
      SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
      SDL_AudioStream* st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      std::fprintf(stderr, "[probe] real audio driver='%s', stream=%s\n", cur ? cur : "(null)", st ? "opened" : "unavailable");
      if (st) SDL_DestroyAudioStream(st);
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    } else {
      std::fprintf(stderr, "[probe] no real audio daemon/device in container (%s) — acceptable\n", SDL_GetError());
    }
    ::_exit(ndrv > 0 ? 0 : 1);
  }
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

// 2. Coexistence of SDL3 Input (gamepad/joystick) and SDL3 Audio (refcounting & independent shutdown).
void test_sdl3_input_audio_coexistence() {
  TempDir tmp("snap_audio_coex");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "sdl3");
  EnvGuard g3("SDL_AUDIO_DRIVER", "dummy");

#ifdef HLE_WITH_SDL3
  // Order A: Input first -> Audio -> Audio shutdown (Input stays alive) -> Input shutdown
  {
    auto in_backend = hle::input::make_default_backend();
    CHECK(in_backend != nullptr);
    hle::input::RawInput raw{};
    in_backend->poll(raw);
    CHECK((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0);

    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
    cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
    snap_audio_configure(&cfg);
    CHECK(snap_audio_game_channels() == 2);
    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0);
    CHECK((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0);

    snap_audio_shutdown();
    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    CHECK((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0);
    in_backend->poll(raw);  // input must still poll cleanly after audio shutdown

    in_backend.reset();
    CHECK(SDL_WasInit(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK) == 0);
  }

  // Order B: Audio first -> Input -> Input shutdown (Audio stays alive & mixing) -> Audio shutdown
  {
    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_SDL3;
    cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
    cfg.enableTap = 1;
    snap_audio_configure(&cfg);
    CHECK(snap_audio_game_channels() == 2);

    auto in_backend = hle::input::make_default_backend();
    CHECK(in_backend != nullptr);
    hle::input::RawInput raw{};
    in_backend->poll(raw);
    CHECK((SDL_WasInit(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) & (SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) ==
          (SDL_INIT_AUDIO | SDL_INIT_GAMEPAD));

    in_backend.reset();
    CHECK((SDL_WasInit(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) == 0);
    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0);

    // Audio must continue mixing after input backend is destroyed
    alignas(16) uint8_t audioOut[0x168] = {};
    SnapAudioOutParameter param{};
    snap_nn_audio_InitializeAudioOutParameter(&param);
    param.sampleRate = 48000;
    param.channelCount = 2;
    CHECK(snap_nn_audio_OpenDefaultAudioOut(audioOut, &param) == 0);
    CHECK(snap_nn_audio_StartAudioOut(audioOut) == 0);
    std::vector<int16_t> pcm(960 * 2, 16384);
    SnapAudioOutBuffer buf{};
    snap_nn_audio_SetAudioOutBufferInfo(&buf, pcm.data(), pcm.size() * 2, pcm.size() * 2);
    snap_nn_audio_AppendAudioOutBuffer(audioOut, &buf);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    SnapAudioOutBuffer* rel = nullptr;
    while (std::chrono::steady_clock::now() < deadline) {
      rel = snap_nn_audio_GetReleasedAudioOutBuffer(audioOut);
      if (rel) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rel == &buf);
    snap_nn_audio_StopAudioOut(audioOut);
    snap_nn_audio_CloseAudioOut(audioOut);

    snap_audio_shutdown();
    CHECK(SDL_WasInit(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK) == 0);
  }
#endif
}

// 3. Synthetic ABI bridge tests (bridge_int and bridge_float) without guest code or guest memory.
// Note: GetReleasedAudioOutBuffer's bridge wrapper is intentionally not called here because
// apply_category_volumes() reads kMgrPtrAddr / kSettingsGot in mapped guest memory.
void test_abi_wrappers_synthetic() {
  TempDir tmp("snap_audio_abi");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "null");

  CHECK(snap_bind_audio() == 16);

  GuestFn fn_init_param = find_audio_override("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE");
  GuestFn fn_open = find_audio_override("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE");
  GuestFn fn_start = find_audio_override("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE");
  GuestFn fn_state = find_audio_override("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE");
  GuestFn fn_ch = find_audio_override("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE");
  GuestFn fn_set_buf = find_audio_override("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm");
  GuestFn fn_get_ptr = find_audio_override("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE");
  GuestFn fn_append = find_audio_override("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE");
  GuestFn fn_list_dev = find_audio_override("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi");
  GuestFn fn_set_dev_vol = find_audio_override("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf");
  GuestFn fn_get_dev = find_audio_override("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE");
  GuestFn fn_stop = find_audio_override("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE");
  GuestFn fn_close = find_audio_override("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE");

  CHECK(fn_init_param && fn_open && fn_start && fn_state && fn_ch && fn_set_buf && fn_get_ptr &&
        fn_append && fn_list_dev && fn_set_dev_vol && fn_get_dev && fn_stop && fn_close);

  Cpu c{};
  SnapAudioOutParameter param{0x1234, 99, 1, {2, 3}};
  c.x[0] = reinterpret_cast<uint64_t>(&param);
  fn_init_param(&c);
  CHECK(param.sampleRate == 0 && param.channelCount == 0);

  param.sampleRate = 48000;
  param.channelCount = 2;
  alignas(16) uint8_t audioOut[0x168] = {};
  c = Cpu{};
  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  c.x[1] = reinterpret_cast<uint64_t>(&param);
  fn_open(&c);
  CHECK(c.x[0] == 0);

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_ch(&c);
  CHECK(c.x[0] == 2);

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_start(&c);
  CHECK(c.x[0] == 0);

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_state(&c);
  CHECK(c.x[0] == SNAP_AUDIO_STATE_STARTED);

  std::vector<int16_t> pcm(480 * 2, 1000);
  SnapAudioOutBuffer buf{};
  c.x[0] = reinterpret_cast<uint64_t>(&buf);
  c.x[1] = reinterpret_cast<uint64_t>(pcm.data());
  c.x[2] = pcm.size() * sizeof(int16_t);
  c.x[3] = pcm.size() * sizeof(int16_t);
  fn_set_buf(&c);

  c.x[0] = reinterpret_cast<uint64_t>(&buf);
  fn_get_ptr(&c);
  CHECK(c.x[0] == reinterpret_cast<uint64_t>(pcm.data()));

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  c.x[1] = reinterpret_cast<uint64_t>(&buf);
  fn_append(&c);

  SnapAudioDeviceName names[4]{};
  c.x[0] = reinterpret_cast<uint64_t>(names);
  c.x[1] = 4;
  fn_list_dev(&c);
  CHECK(c.x[0] == 4);

  SnapAudioDeviceName active{};
  c.x[0] = reinterpret_cast<uint64_t>(&active);
  fn_get_dev(&c);
  CHECK(std::strcmp(active.name, "AudioTvOutput") == 0);

  // bridge_float: float passed in c.v[0]
  c = Cpu{};
  c.x[0] = reinterpret_cast<uint64_t>(&active);
  float vol = 0.5f;
  std::memcpy(&c.v[0], &vol, sizeof(float));
  fn_set_dev_vol(&c);

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_stop(&c);
  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_state(&c);
  CHECK(c.x[0] == SNAP_AUDIO_STATE_STOPPED);

  c.x[0] = reinterpret_cast<uint64_t>(audioOut);
  fn_close(&c);

  snap_audio_shutdown();
}

// 4. End-to-end OS SystemEvent notification path -> ov_TryWaitSystemEvent().
void test_os_event_notification_path() {
  OsConfig oc{};
  os_init(&oc);

  snap_audio_set_os_hooks([](void* ev) {
    os_system_event_signal(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev)));
  });
  snap_audio_set_event_init_hook([](void* ev) {
    os_system_event_create(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev)), 1);
  });

  GuestFn fn_acq = find_audio_override("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE");
  GuestFn fn_try_wait = find_os_override("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
  CHECK(fn_acq != nullptr && fn_try_wait != nullptr);

  alignas(16) uint8_t sev[64] = {};
  Cpu c{};
  c.x[0] = reinterpret_cast<uint64_t>(sev);
  fn_acq(&c);
  CHECK(c.x[0] == 0);

  // Not yet signaled
  c.x[0] = reinterpret_cast<uint64_t>(sev);
  fn_try_wait(&c);
  CHECK(c.x[0] == 0);

  // Device change notifies the guest SystemEvent
  snap_audio::core_notify_device_change();
  c.x[0] = reinterpret_cast<uint64_t>(sev);
  fn_try_wait(&c);
  CHECK(c.x[0] == 1);

  // Autoclear: second wait must read 0
  c.x[0] = reinterpret_cast<uint64_t>(sev);
  fn_try_wait(&c);
  CHECK(c.x[0] == 0);

  snap_audio_set_os_hooks(nullptr);
  snap_audio_set_event_init_hook(nullptr);
}

}  // namespace

int main() {
  test_real_driver_probe();
  test_sdl3_input_audio_coexistence();
  test_abi_wrappers_synthetic();
  test_os_event_notification_path();
  std::printf("audio_runtime_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
