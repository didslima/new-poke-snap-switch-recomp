// SPDX-License-Identifier: MIT
// POSIX (Linux) host services for shared HLE infrastructure (stage 6B).
#if !defined(__linux__)
#error "runtime/hle/linux/hle_host_posix.cpp is the Linux implementation of hle_host.h"
#endif

#include "../hle_host.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "../hle.h"
#include "../input_core.h"
#include "../profiles.h"

// Weak ELF fallbacks for optional NVN pacing hooks (overridden by strong symbols when NVN is linked).
extern "C" __attribute__((weak)) int snap_pace_vsync_wait(int64_t*) { return 0; }
extern "C" __attribute__((weak)) void snap_pace_vsync_note() {}

// Weak Stage 6B link seams for unported HLE subsystems (filesystem, HID, profiles, input_core).
// None of these register fake overrides or claim the subsystem is available; Stage 6C replaces them.
namespace hle {
__attribute__((weak)) void register_fs(std::vector<Entry>&) {}
__attribute__((weak)) void register_hid(std::vector<Entry>&) {}
}  // namespace hle

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

namespace hle::input {
__attribute__((weak)) Mapping Mapping::defaults() { return Mapping{}; }
__attribute__((weak)) bool swap_ab_effective(const Mapping&) { return false; }
__attribute__((weak)) const std::vector<Source>& action_sources(const Mapping&, int) {
  static const std::vector<Source> kEmpty{};
  return kEmpty;
}
__attribute__((weak)) const char* key_name(int) { return ""; }
__attribute__((weak)) const char* mouse_name(int) { return ""; }
__attribute__((weak)) Hub::Hub() {}
__attribute__((weak)) Mapping Hub::mapping() const { return Mapping{}; }
__attribute__((weak)) PadFx& pad_fx() {
  static PadFx fx{};
  return fx;
}
__attribute__((weak)) Hub& hub() {
  static Hub h{};
  return h;
}
__attribute__((weak)) void script_event(const char*) {}
}  // namespace hle::input

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

}  // namespace hle::host
