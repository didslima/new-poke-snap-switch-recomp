// SPDX-License-Identifier: MIT
// Stage 6D-A: Linux unit and integration tests for the portable audio core (audio_out.cpp, audio_null.cpp, audio_host_posix.cpp).
#include "audio_cfg.h"
#include "audio_host.h"
#include "audio_internal.h"
#include "audio_mix.h"
#include "audio_out.h"
#include "spsc_ring.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

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

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

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

SnapAudioOutBuffer* wait_released(void* audioOut, int timeout_ms = 500) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (SnapAudioOutBuffer* b = snap_nn_audio_GetReleasedAudioOutBuffer(audioOut)) return b;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

void test_spsc_ring() {
  snap_audio::SpscRing<int, 8> r;
  int v = -1;
  CHECK(!r.pop(v));
  CHECK(r.size() == 0);

  for (int i = 0; i < 8; ++i) CHECK(r.push(i + 10));
  CHECK(r.size() == 8);
  CHECK(!r.push(999));

  for (int i = 0; i < 4; ++i) {
    CHECK(r.pop(v) && v == i + 10);
  }
  CHECK(r.size() == 4);

  // Wrap-around pushes and pops
  for (int i = 0; i < 4; ++i) CHECK(r.push(i + 100));
  CHECK(r.size() == 8);
  CHECK(!r.push(1000));

  std::vector<int> snap;
  r.copy_to(snap);
  CHECK(snap.size() == 8);
  CHECK(snap[0] == 14 && snap[4] == 100 && snap[7] == 103);

  snap_audio::SpscRing<int, 8> r2;
  r2.assign(snap.data(), snap.size());
  CHECK(r2.size() == 8);
  for (int i = 0; i < 8; ++i) {
    CHECK(r2.pop(v) && v == snap[i]);
  }
  CHECK(!r2.pop(v));
  r.reset();
  CHECK(r.size() == 0);
}

void test_sample_ring() {
  snap_audio::SampleRing<16> sr;
  float out[16] = {};
  CHECK(sr.read(out, 4) == 0);

  float in1[10];
  for (int i = 0; i < 10; ++i) in1[i] = (float)(i + 1);
  CHECK(sr.write(in1, 10) == 10);
  CHECK(sr.read(out, 6) == 6);
  for (int i = 0; i < 6; ++i) CHECK(near(out[i], (float)(i + 1)));

  // Wrap-around write/read
  float in2[12];
  for (int i = 0; i < 12; ++i) in2[i] = (float)(i + 100);
  CHECK(sr.write(in2, 12) == 12);
  // Overflow drops excess
  CHECK(sr.write(in2, 4) == 0);

  float out2[16] = {};
  CHECK(sr.read(out2, 16) == 16);
  for (int i = 0; i < 4; ++i) CHECK(near(out2[i], (float)(i + 7)));
  for (int i = 0; i < 12; ++i) CHECK(near(out2[4 + i], (float)(i + 100)));
  sr.reset();
  CHECK(sr.read(out2, 4) == 0);
}

void test_mixer_and_upmixer() {
  // Mono -> stereo
  {
    int16_t src[2] = {16384, -16384};
    float dst[4] = {};
    snap_audio::mix::accumulate_s16(dst, 2, src, 1, 2);
    CHECK(near(dst[0], 0.5f) && near(dst[1], 0.5f));
    CHECK(near(dst[2], -0.5f) && near(dst[3], -0.5f));
  }
  // Stereo -> stereo
  {
    int16_t src[2] = {16384, -8192};
    float dst[2] = {};
    snap_audio::mix::accumulate_s16(dst, 2, src, 2, 1);
    CHECK(near(dst[0], 0.5f) && near(dst[1], -0.25f));
  }
  // 5.1 -> stereo downmix
  {
    int16_t src[6] = {16384, 0, 16384, 32767, 16384, 0};
    float dst[2] = {};
    snap_audio::mix::accumulate_s16(dst, 2, src, 6, 1);
    const float expL = (0.5f + snap_audio::mix::kDownmixCenter * 0.5f + snap_audio::mix::kDownmixCenter * 0.5f) *
                       snap_audio::mix::kDownmixNorm;
    const float expR = (0.0f + snap_audio::mix::kDownmixCenter * 0.5f + 0.0f) * snap_audio::mix::kDownmixNorm;
    CHECK(near(dst[0], expL, 1e-3f));
    CHECK(near(dst[1], expR, 1e-3f));
  }
  // 5.1 -> 7.1 mapping (surrounds go to SL/SR [6,7], backs [4,5] stay 0)
  {
    int16_t src[6] = {3276, 6553, 9830, 13107, 16384, 19660};
    float dst[8] = {};
    snap_audio::mix::accumulate_s16(dst, 8, src, 6, 1);
    CHECK(near(dst[0], src[0] / 32768.0f));
    CHECK(near(dst[1], src[1] / 32768.0f));
    CHECK(near(dst[2], src[2] / 32768.0f));
    CHECK(near(dst[3], src[3] / 32768.0f));
    CHECK(near(dst[4], 0.0f) && near(dst[5], 0.0f));
    CHECK(near(dst[6], src[4] / 32768.0f));
    CHECK(near(dst[7], src[5] / 32768.0f));
  }
  // Upmixer: stereo-only 5.1 opens center gate; real center closes it
  {
    snap_audio::mix::Upmixer up;
    std::vector<float> buf(480 * 6, 0.0f);
    for (size_t i = 0; i < 480; ++i) {
      buf[i * 6 + 0] = 0.4f;
      buf[i * 6 + 1] = 0.4f;
    }
    up.process(buf.data(), 6, 480);
    CHECK(up.gate() > 0.05f);
    CHECK(buf[479 * 6 + 2] > 0.01f);

    std::vector<float> realCenter(480 * 6, 0.0f);
    for (size_t i = 0; i < 480; ++i) realCenter[i * 6 + 2] = 0.5f;
    const float before = up.gate();
    up.process(realCenter.data(), 6, 480);
    CHECK(up.gate() < before);
  }
}

void test_null_backend_and_core_lifecycle() {
  TempDir tmp("snap_audio_core");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "null");
  EnvGuard g3("SNAP_AUDIO_OUTPUT", "stereo");

  // Repeated NullBackend start/stop directly
  {
    std::unique_ptr<snap_audio::Backend> nb(snap_audio::make_null_backend());
    CHECK(nb && nb->kind() == SNAP_AUDIO_BACKEND_NULL);
    snap_audio::DeviceFormat fmt{48000, 2, 0, SNAP_AUDIO_OUT_STEREO};
    for (int i = 0; i < 5; ++i) {
      CHECK(nb->start(fmt));
      nb->stop();
    }
  }

  // Core init/shutdown cycles
  for (int cycle = 0; cycle < 3; ++cycle) {
    SnapAudioConfig cfg;
    snap_audio_default_config(&cfg);
    cfg.backend = SNAP_AUDIO_BACKEND_NULL;
    cfg.outputMode = SNAP_AUDIO_OUT_STEREO;
    cfg.channelMode = SNAP_AUDIO_CH_STEREO;
    cfg.enableTap = 1;
    snap_audio_configure(&cfg);
    CHECK(snap_audio_game_channels() == 2);

    SnapAudioStats st{};
    snap_audio_get_stats(&st);
    CHECK(st.backendInUse == SNAP_AUDIO_BACKEND_NULL);
    CHECK(st.deviceAttached == 0);
    CHECK(st.deviceRate == 48000);
    CHECK(st.deviceChannels == 2);

    snap_audio_shutdown();
  }
}

void test_core_mix_volume_mute_and_checks() {
  TempDir tmp("snap_audio_mix");
  EnvGuard g1("SNAP_CONFIG_DIR", tmp.path.c_str());
  EnvGuard g2("SNAP_AUDIO_BACKEND", "null");

  snap_audio_reset_stats();
  SnapAudioConfig cfg;
  snap_audio_default_config(&cfg);
  cfg.backend = SNAP_AUDIO_BACKEND_NULL;
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
  CHECK(snap_nn_audio_GetAudioOutState(audioOut) == SNAP_AUDIO_STATE_STOPPED);
  CHECK(snap_nn_audio_GetAudioOutChannelCount(audioOut) == 2);
  CHECK(snap_nn_audio_StartAudioOut(audioOut) == 0);
  CHECK(snap_nn_audio_GetAudioOutState(audioOut) == SNAP_AUDIO_STATE_STARTED);

  // Submit a 20 ms (960 frames) PCM16 buffer and let NullBackend mix it into the tap
  std::vector<int16_t> pcm(960 * 2, 16384);  // 0.5 float
  SnapAudioOutBuffer buf{};
  snap_nn_audio_SetAudioOutBufferInfo(&buf, pcm.data(), pcm.size() * sizeof(int16_t), pcm.size() * sizeof(int16_t));
  CHECK(snap_nn_audio_GetAudioOutBufferDataPointer(&buf) == pcm.data());

  snap_audio_set_volume(SNAP_VOL_MASTER, 1.0f);
  snap_audio_set_muted(0);
  // Drain any prior silence from the tap
  std::vector<float> drain(4096);
  while (snap_audio_tap_read(drain.data(), drain.size()) > 0) {}

  snap_nn_audio_AppendAudioOutBuffer(audioOut, &buf);
  CHECK(wait_released(audioOut) == &buf);
  CHECK(snap_nn_audio_GetReleasedAudioOutBuffer(audioOut) == nullptr);

  std::vector<float> tap(8192);
  const uint64_t read_n = snap_audio_tap_read(tap.data(), tap.size());
  CHECK(read_n > 0);
  float peak = 0.0f;
  for (uint64_t i = 0; i < read_n; ++i) peak = std::max(peak, std::fabs(tap[i]));
  CHECK(peak > 0.2f);

  // Mute silences output without changing master volume
  snap_audio_set_muted(1);
  CHECK(snap_audio_get_muted() == 1);
  while (snap_audio_tap_read(drain.data(), drain.size()) > 0) {}
  snap_nn_audio_AppendAudioOutBuffer(audioOut, &buf);
  CHECK(wait_released(audioOut) == &buf);
  const uint64_t muted_n = snap_audio_tap_read(tap.data(), tap.size());
  CHECK(muted_n > 0);
  // Last block in tap must be 0 after ramp-down
  CHECK(near(tap[muted_n - 1], 0.0f, 1e-4f));
  snap_audio_set_muted(0);

  // Diagnostics checks (also stop backend before calling core_mix with invalid args)
  snap_nn_audio_StopAudioOut(audioOut);
  CHECK(snap_nn_audio_GetAudioOutState(audioOut) == SNAP_AUDIO_STATE_STOPPED);
  snap_nn_audio_CloseAudioOut(audioOut);
  snap_nn_audio_StopAudioOut(audioOut);  // badHandle

  CHECK(snap_nn_audio_OpenDefaultAudioOut(nullptr, &param) != 0);
  snap_audio_set_volume(99, 0.5f);
  snap_nn_audio_SetAudioOutBufferInfo(nullptr, nullptr, 0, 0);

  snap_audio_shutdown();
  snap_audio::core_mix(nullptr, 10);  // badPull (safe with backend stopped)

  SnapAudioChecks chk{};
  snap_audio_get_checks(&chk);
  CHECK(chk.badOpen >= 1);
  CHECK(chk.badPull >= 1);
  CHECK(chk.badVolume >= 1);
  CHECK(chk.badBuffer >= 1);
  CHECK(chk.badHandle >= 1);
  CHECK(chk.underruns >= 1);
  snap_audio_log_checks();
}

void test_xdg_and_persistence() {
  TempDir tmp("snap_audio_xdg");
  const fs::path xdg = tmp.path / "xdg_cfg";
  const fs::path home = tmp.path / "home_dir";
  const fs::path explicitDir = tmp.path / "explicit_cfg";

  {
    EnvGuard g1("SNAP_CONFIG_DIR", nullptr);
    EnvGuard g2("XDG_CONFIG_HOME", xdg.c_str());
    EnvGuard g3("HOME", home.c_str());
    CHECK(snap_audio::host::config_dir() == (xdg / "SnapRecomp").string());
  }
  {
    EnvGuard g1("SNAP_CONFIG_DIR", nullptr);
    EnvGuard g2("XDG_CONFIG_HOME", nullptr);
    EnvGuard g3("HOME", home.c_str());
    CHECK(snap_audio::host::config_dir() == (home / ".config/SnapRecomp").string());
  }
  {
    EnvGuard g1("SNAP_CONFIG_DIR", explicitDir.c_str());
    EnvGuard g2("XDG_CONFIG_HOME", xdg.c_str());
    CHECK(snap_audio::host::config_dir() == explicitDir.string());
  }

  // Persistence of audio.json via atomic_write_text + read_text_file + snap_audio_set_volume/output
  {
    EnvGuard g1("SNAP_CONFIG_DIR", explicitDir.c_str());
    snap_audio_set_volume(SNAP_VOL_MASTER, 0.75f);
    snap_audio_set_volume(SNAP_VOL_BGM, 0.50f);
    snap_audio_set_output(SNAP_AUDIO_OUT_SPEAKERS, 6, 8);

    const fs::path cfgFile = explicitDir / "audio.json";
    CHECK(fs::exists(cfgFile));
    CHECK(!fs::exists(explicitDir / "audio.json.tmp"));

    char buf[1024] = {};
    size_t len = 0;
    CHECK(snap_audio::host::read_text_file(cfgFile.string(), buf, sizeof buf, &len));
    CHECK(len > 0);
    int ch = 0, spk = 0;
    CHECK(snap_audio::cfg::read_int(buf, "channels", &ch) && ch == 6);
    CHECK(snap_audio::cfg::read_int(buf, "speakers", &spk) && spk == 8);
    CHECK(std::strstr(buf, "\"output\": \"speakers\"") != nullptr);
    CHECK(std::strstr(buf, "\"master\": 0.7500") != nullptr);
  }
}

}  // namespace

int main() {
  test_spsc_ring();
  test_sample_ring();
  test_mixer_and_upmixer();
  test_null_backend_and_core_lifecycle();
  test_core_mix_volume_mute_and_checks();
  test_xdg_and_persistence();
  std::printf("audio_core_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
