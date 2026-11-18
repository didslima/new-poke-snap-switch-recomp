// SPDX-License-Identifier: MIT
// POSIX (Linux) host services for shared HLE infrastructure (stage 6B).
#if !defined(__linux__)
#error "runtime/hle/linux/hle_host_posix.cpp is the Linux implementation of hle_host.h"
#endif

#include "../hle_host.h"

#include <fcntl.h>
#include <signal.h>
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
extern "C" __attribute__((weak)) void snap_aim_set(const char*) {}
extern "C" __attribute__((weak)) int snap_aim_ready(char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 1; }
extern "C" __attribute__((weak)) int snap_aim_stick(float*, float*) { return 0; }
extern "C" __attribute__((weak)) void snap_aim_shot(void) {}
extern "C" __attribute__((weak)) int snap_aim_pose(const char*, char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 1; }
extern "C" __attribute__((weak)) int snap_aim_act(const char*, char* why, int n) { if (why && n > 0) std::snprintf(why, (size_t)n, "aim tool off"); return 0; }
extern "C" __attribute__((weak)) void snap_import_counts_dump(const char*) {}
extern "C" __attribute__((weak)) uint64_t snap_os_heap_base(void) { return 0; }

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

// Weak Stage 6B/6C link seam for unported profile picker UI (register_fs, register_hid, input, and profiles are strongly defined).
extern "C" __attribute__((weak)) int snap_ui_pick_profile(const hle::profiles::PickItem*, int) {
  return -1;
}

namespace hle::host {

char path_sep() { return '/'; }

std::string exe_dir() {
  char buf[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  return std::filesystem::path(buf).parent_path().string();
}

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

bool mem_range_ok(uint64_t addr, size_t size, bool write) {
  if (size == 0) return true;
  if (addr == 0 || addr + size < addr) return false;
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f) return false;
  uint64_t cur = addr;
  const uint64_t end = addr + size;
  char line[512];
  while (cur < end && std::fgets(line, sizeof(line), f)) {
    unsigned long long lo = 0, hi = 0;
    char perms[8] = {};
    if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
    if (hi <= cur) continue;
    if (lo > cur) break;
    if (perms[0] != 'r' || (write && perms[1] != 'w')) {
      std::fclose(f);
      return false;
    }
    cur = hi;
  }
  std::fclose(f);
  return cur >= end;
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

void set_last_os_error(int err) { errno = err; }

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

bool is_save_lock_held(const std::string& lock_path) {
  const std::filesystem::path p = std::filesystem::u8path(lock_path);
  int fd = -1;
  do {
    fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return !(errno == ENOENT || errno == ENOTDIR);
  }
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  const bool held = (rc != 0);
  if (!held) ::flock(fd, LOCK_UN);
  ::close(fd);
  return held;
}

void* pending_lock_begin(const std::string& lock_path) {
  const std::filesystem::path p = std::filesystem::u8path(lock_path);
  int fd = -1;
  do {
    fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return nullptr;
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    ::close(fd);
    return nullptr;
  }
  return fd_to_handle(fd);
}

void pending_lock_end(void* handle, const std::string& lock_path) {
  const int fd = handle_to_fd(handle);
  if (fd < 0) return;
  const std::filesystem::path p = std::filesystem::u8path(lock_path);
  ::unlink(p.c_str());
  ::flock(fd, LOCK_UN);
  ::close(fd);
}

bool rename_save_dir(const std::string& src_dir, const std::string& dst_dir, const std::string& lock_path,
                     bool* lock_busy, std::error_code* ec) {
  if (lock_busy) *lock_busy = false;
  void* lk = nullptr;
  // On POSIX, rename(2) succeeds even while another process holds .snap.lock open; acquire the lock across rename.
  if (try_acquire_save_lock(lock_path, &lk) != 0) {
    if (lock_busy) *lock_busy = true;
    return false;
  }
  std::error_code local_ec;
  std::filesystem::rename(std::filesystem::u8path(src_dir), std::filesystem::u8path(dst_dir), ec ? *ec : local_ec);
  const bool ok = !(ec ? *ec : local_ec);
  release_save_lock(lk);
  return ok;
}

void wait_process_exit(uint64_t pid, int timeout_ms) {
  if (pid <= 1) return;
  const uint64_t end = monotonic_ms() + static_cast<uint64_t>(timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) break;
    if (monotonic_ms() >= end) break;
    sleep_ms(20);
  }
}

bool local_calendar_tm(int64_t t, std::tm* out) {
  if (!out) return false;
  std::time_t tt = static_cast<std::time_t>(t);
  ::tzset();
  return ::localtime_r(&tt, out) != nullptr;
}

}  // namespace hle::host
