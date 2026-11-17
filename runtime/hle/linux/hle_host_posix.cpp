// SPDX-License-Identifier: MIT
// POSIX (Linux) host services for shared HLE infrastructure (stage 6B).
#if !defined(__linux__)
#error "runtime/hle/linux/hle_host_posix.cpp is the Linux implementation of hle_host.h"
#endif

#include "../hle_host.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../hle.h"
#include "../input_core.h"
#include "../profiles.h"

// Weak ELF fallbacks for optional NVN pacing, photo-export drain, audio checks, and save-state hooks.
extern "C" __attribute__((weak)) int snap_pace_vsync_wait(int64_t*) { return 0; }
extern "C" __attribute__((weak)) void snap_pace_vsync_note() {}
extern "C" __attribute__((weak)) void snap_photoexport_drain_for_exit(int) {}
extern "C" __attribute__((weak)) void snap_audio_log_checks(void) {}
extern "C" __attribute__((weak)) void snap_state_request(int, const char*) {}
extern "C" __attribute__((weak)) int snap_state_idle() { return 1; }
extern "C" __attribute__((weak)) int snap_state_count(const char*) { return 0; }

// Weak Stage 6C-B1 seams for unported NVN frame counters and cheats (overridden when ported).
namespace nvn {
__attribute__((weak)) int g_presentCount = 0;
__attribute__((weak)) int g_fdBurst = 0;
}  // namespace nvn

namespace hle::cheats {
__attribute__((weak)) void tick() {}
}  // namespace hle::cheats

// Weak Stage 6C-B1 backend fallback until SDL3 input_backends is ported in Stage 6C-B2.
namespace hle::input {
__attribute__((weak)) std::unique_ptr<Backend> make_default_backend() {
  struct NullBackend final : Backend {
    const char* name() const override { return "none"; }
    void poll(RawInput& out) override { out.focused = false; }
  };
  return std::make_unique<NullBackend>();
}
}  // namespace hle::input

// Weak Stage 6B/6C link seams for unported HLE profiles/UI (register_fs and register_hid are now strongly defined).
namespace hle::profiles {
__attribute__((weak)) Store::Store(std::string) {}
__attribute__((weak)) bool Store::load() { return false; }
__attribute__((weak)) const Profile* Store::find(const std::string&) const { return nullptr; }
__attribute__((weak)) std::string Store::save_dir(const std::string&) const { return ""; }
__attribute__((weak)) Active startup(const std::string&, const std::string&, const std::string&, PickerFn, bool) {
  return Active{};
}
__attribute__((weak)) void set_active(const Active&) {}
__attribute__((weak)) const Active& active() {
  static const Active kInactive{};
  return kInactive;
}
}  // namespace hle::profiles

extern "C" __attribute__((weak)) int snap_ui_pick_profile(const hle::profiles::PickItem*, int) {
  return -1;
}

namespace hle::host {

char path_sep() { return '/'; }

std::string default_data_root() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/SnapRecomp";
  const char* h = std::getenv("HOME");
  return std::string(h && *h ? h : ".") + "/.local/share/SnapRecomp";
}

std::string default_config_dir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/SnapRecomp";
  const char* h = std::getenv("HOME");
  return std::string(h && *h ? h : ".") + "/.config/SnapRecomp";
}

bool valid_utf8_path(const std::string&) { return true; }

std::string legacy_data_root() { return ""; }

bool readable_addr(uint64_t addr, size_t size) {
  if (!size) return true;
  char buf[256];
  const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(addr));
  while (size) {
    const size_t n = size < sizeof(buf) ? size : sizeof(buf);
    iovec l{buf, n}, r{const_cast<char*>(p), n};
    if (process_vm_readv(getpid(), &l, 1, &r, 1, 0) != (ssize_t)n) return false;
    p += n;
    size -= n;
  }
  return true;
}

bool atomic_replace_file(const std::string& tmp_path, const std::string& dst_path) {
  std::error_code ec;
  std::filesystem::rename(std::filesystem::u8path(tmp_path), std::filesystem::u8path(dst_path), ec);
  return !ec;
}

void set_env_var(const char* name, const char* value) {
  if (name && *name) setenv(name, value ? value : "", 1);
}

void unset_env_var(const char* name) {
  if (name && *name) unsetenv(name);
}

void show_error_dialog(const char*, const char*) {}

bool local_zone_at(int64_t t, int64_t* offset_out, bool* dst_out, char name_out[8]) {
  if (!offset_out || !dst_out || !name_out) return false;
  time_t tt = (time_t)t;
  struct tm lt{};
  // POSIX localtime_r is not required to call tzset(); call tzset() so runtime TZ changes take effect.
  tzset();
  if (!localtime_r(&tt, &lt)) return false;
  *offset_out = (int64_t)lt.tm_gmtoff;
  *dst_out = lt.tm_isdst > 0;
  std::memset(name_out, 0, 8);
  // Prefer lt.tm_zone (populated per-result by glibc localtime_r without racing on global tzname).
  const char* nm = (lt.tm_zone && *lt.tm_zone) ? lt.tm_zone : tzname[*dst_out ? 1 : 0];
  if (nm) std::strncpy(name_out, nm, 7);
  return true;
}

std::string ui_language_tag() {
  const char* loc = std::getenv("LC_ALL");
  if (!loc || !*loc) loc = std::getenv("LC_MESSAGES");
  if (!loc || !*loc) loc = std::getenv("LANG");
  if (!loc || !*loc) return "en";
  std::string s;
  for (const char* p = loc; *p && *p != '.' && *p != '@'; ++p) {
    s += (*p == '_') ? '-' : (char)std::tolower((unsigned char)*p);
  }
  auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };
  if (starts("zh-hans") || starts("zh-cn") || starts("zh-sg")) return "zh-hans";
  if (starts("zh-hant") || starts("zh-tw") || starts("zh-hk") || starts("zh-mo")) return "zh-hant";
  if (starts("ja")) return "ja";
  if (starts("fr")) return "fr";
  if (starts("de")) return "de";
  if (starts("it")) return "it";
  if (starts("es")) return "es";
  if (starts("ko")) return "ko";
  return "en";
}

bool shift_held_at_launch() { return false; }

uint64_t process_id() { return (uint64_t)getpid(); }

// Legacy Windows %APPDATA% migration is unused on Linux (legacy_data_root() == "");
// if cross-process file locking is needed later on Linux, implement with flock()/fcntl().
void* lock_file_begin(const std::string&, int) { return reinterpret_cast<void*>(1); }
void lock_file_end(void*) {}

namespace {
inline void* fd_to_handle(int fd) {
  return fd < 0 ? nullptr : reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
}
inline int handle_to_fd(void* handle) {
  return handle ? static_cast<int>(reinterpret_cast<intptr_t>(handle) - 1) : -1;
}
}  // namespace

void* file_open(const std::string& path, bool write) {
  const std::filesystem::path p = std::filesystem::u8path(path);
  int fd = -1;
  do {
    fd = ::open(p.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  return fd_to_handle(fd);
}

void file_close(void* handle) {
  const int fd = handle_to_fd(handle);
  if (fd >= 0) ::close(fd);
}

int64_t file_size(void* handle) {
  const int fd = handle_to_fd(handle);
  if (fd < 0) return -1;
  struct stat st{};
  return ::fstat(fd, &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}

int64_t file_read_at(void* handle, int64_t off, void* buf, size_t n) {
  const int fd = handle_to_fd(handle);
  if (fd < 0 || off < 0) return -1;
  size_t done = 0;
  char* b = static_cast<char*>(buf);
  while (done < n) {
    const ssize_t got = ::pread(fd, b + done, n - done, static_cast<off_t>(off + static_cast<int64_t>(done)));
    if (got < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (got == 0) break;
    done += static_cast<size_t>(got);
  }
  return static_cast<int64_t>(done);
}

bool file_write_at(void* handle, int64_t off, const void* buf, size_t n) {
  const int fd = handle_to_fd(handle);
  if (fd < 0 || off < 0) return false;
  size_t done = 0;
  const char* b = static_cast<const char*>(buf);
  while (done < n) {
    const ssize_t got = ::pwrite(fd, b + done, n - done, static_cast<off_t>(off + static_cast<int64_t>(done)));
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (got == 0) return false;
    done += static_cast<size_t>(got);
  }
  return true;
}

bool file_set_size(void* handle, int64_t size) {
  const int fd = handle_to_fd(handle);
  if (fd < 0 || size < 0) return false;
  int rc = 0;
  do {
    rc = ::ftruncate(fd, static_cast<off_t>(size));
  } while (rc != 0 && errno == EINTR);
  return rc == 0;
}

bool file_flush(void* handle) {
  const int fd = handle_to_fd(handle);
  if (fd < 0) return false;
  int rc = 0;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  return rc == 0;
}

int try_acquire_save_lock(const std::string& lock_path, void** out_handle) {
  if (out_handle) *out_handle = nullptr;
  const std::filesystem::path p = std::filesystem::u8path(lock_path);
  int fd = -1;
  do {
    fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return (errno == EACCES || errno == EPERM) ? 1 : 2;
  }
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    const int err = errno;
    ::close(fd);
    return (err == EWOULDBLOCK || err == EAGAIN) ? 1 : 2;
  }
  if (out_handle) *out_handle = fd_to_handle(fd);
  else ::close(fd);
  return 0;
}

void release_save_lock(void* handle) {
  const int fd = handle_to_fd(handle);
  if (fd >= 0) ::close(fd);
}

bool is_disk_full_error(const std::error_code& ec) {
  if (ec == std::errc::no_space_on_device || ec == std::errc::file_too_large) return true;
#ifdef EDQUOT
  if (ec.category() == std::generic_category() && ec.value() == EDQUOT) return true;
#endif
  return false;
}

std::error_code last_os_error() {
  return std::error_code(errno, std::generic_category());
}

uint64_t monotonic_ms() {
  struct timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

void sleep_ms(int ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

[[noreturn]] void exit_process(int code) {
  std::fflush(nullptr);
  std::_Exit(code);
}

}  // namespace hle::host
