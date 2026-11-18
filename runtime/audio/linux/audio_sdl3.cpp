// SPDX-License-Identifier: MIT
// Stage 6D-B: Linux SDL3 audio backend (replaces weak make_sdl3_audio_backend() in audio_host_posix.cpp).
#if !defined(__linux__)
#error "runtime/audio/linux/audio_sdl3.cpp is the Linux implementation of make_sdl3_audio_backend()"
#endif

#include "../audio_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace snap_audio {

#ifdef HLE_WITH_SDL3
namespace {

constexpr uint32_t kChunkFrames = 2048;

class Sdl3AudioBackend final : public Backend {
 public:
  ~Sdl3AudioBackend() override { stop(); }

  int kind() const override { return SNAP_AUDIO_BACKEND_SDL3; }
  int spatial_available() override { return 0; }

  uint32_t native_channels() override {
    const bool was_init = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
    if (!was_init && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return 2;
    SDL_AudioSpec spec{};
    uint32_t ch = 2;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr) && spec.channels > 0) {
      ch = static_cast<uint32_t>(spec.channels);
    }
    if (!was_init) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return ch;
  }

  bool start(const DeviceFormat& fmt) override {
    stop();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    subsys_init_ = true;

    const uint32_t rate = fmt.rate ? fmt.rate : 48000u;
    uint32_t ch = fmt.channels ? fmt.channels : 2u;
    int active_mode = fmt.mode == SNAP_AUDIO_OUT_SPATIAL ? SNAP_AUDIO_OUT_SPEAKERS : fmt.mode;
    if (ch != 2 && ch != 6 && ch != 8) ch = 2;

    SDL_AudioSpec want{};
    want.format = SDL_AUDIO_F32;
    want.freq = static_cast<int>(rate);
    want.channels = static_cast<int>(ch);

    // Pre-allocate before opening/resuming so audio_cb never allocates.
    mix_buf_.assign(static_cast<size_t>(kChunkFrames) * ch, 0.0f);
    channels_ = ch;
    rate_ = rate;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, &Sdl3AudioBackend::audio_cb, this);
    if (!stream_ && ch != 2) {
      ch = 2;
      active_mode = SNAP_AUDIO_OUT_STEREO;
      want.channels = 2;
      channels_ = 2;
      mix_buf_.assign(static_cast<size_t>(kChunkFrames) * 2, 0.0f);
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, &Sdl3AudioBackend::audio_cb, this);
    }
    if (!stream_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      subsys_init_ = false;
      return false;
    }

    int sample_frames = 0;
    SDL_AudioSpec actual{};
    const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
    if (!SDL_GetAudioDeviceFormat(dev ? dev : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &actual, &sample_frames)) {
      sample_frames = 0;
    }
    const uint32_t buf_frames = sample_frames > 0 ? static_cast<uint32_t>(sample_frames) : 0u;
    const double latency_ms = (buf_frames > 0 && rate > 0) ? (1000.0 * buf_frames / static_cast<double>(rate)) : 0.0;
    core_set_device_info(true, buf_frames, latency_ms, ch, rate, active_mode);

    watch_added_ = SDL_AddEventWatch(&Sdl3AudioBackend::event_watch, this);
    active_.store(true, std::memory_order_release);

    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      stop();
      return false;
    }
    return true;
  }

  void stop() override {
    active_.store(false, std::memory_order_release);
    if (watch_added_) {
      SDL_RemoveEventWatch(&Sdl3AudioBackend::event_watch, this);
      watch_added_ = false;
    }
    if (stream_) {
      SDL_PauseAudioStreamDevice(stream_);
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    if (subsys_init_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      subsys_init_ = false;
    }
  }

 private:
  static void SDLCALL audio_cb(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
    auto* self = static_cast<Sdl3AudioBackend*>(userdata);
    if (!self || !self->active_.load(std::memory_order_acquire) || additional_amount <= 0) return;

    const uint32_t ch = self->channels_;
    if (ch == 0) return;
    const uint32_t frame_bytes = ch * static_cast<uint32_t>(sizeof(float));
    uint32_t frames = static_cast<uint32_t>(additional_amount) / frame_bytes;
    while (frames > 0) {
      const uint32_t n = std::min(frames, kChunkFrames);
      core_mix(self->mix_buf_.data(), n);
      SDL_PutAudioStreamData(stream, self->mix_buf_.data(), static_cast<int>(n * frame_bytes));
      frames -= n;
    }
  }

  static bool SDLCALL event_watch(void* userdata, SDL_Event* ev) {
    auto* self = static_cast<Sdl3AudioBackend*>(userdata);
    if (!self || !self->active_.load(std::memory_order_acquire) || !ev) return true;
    if (ev->type == SDL_EVENT_AUDIO_DEVICE_ADDED || ev->type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
        ev->type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED) {
      core_notify_device_change();
    }
    return true;
  }

  SDL_AudioStream* stream_ = nullptr;
  std::vector<float> mix_buf_;
  uint32_t channels_ = 2;
  uint32_t rate_ = 48000;
  std::atomic<bool> active_{false};
  bool watch_added_ = false;
  bool subsys_init_ = false;
};

}  // namespace

Backend* make_sdl3_audio_backend() { return new Sdl3AudioBackend(); }

#else

Backend* make_sdl3_audio_backend() { return nullptr; }

#endif

}  // namespace snap_audio
