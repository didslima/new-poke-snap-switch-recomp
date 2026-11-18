// SPDX-License-Identifier: MIT
// Player cheats (runtime/hle/cheats.h, docs/cheats.md). The VM in runtime/hle/cheat/ only calls back into the Memory adapter below; this file owns
// the file, the per-cheat switches and the once-per-frame run.
#include "cheats.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#include "cheat/cheat_vm.h"
#include "cheat/cheat_vm.cpp"   // compiled with this file: the build globs runtime/hle/*.cpp only, and the VM has no other user
#include "hle.h"
#include "hle_host.h"
#include "input_core.h"
#include "settings.h"

namespace nvn { extern int g_presentCount; }
extern "C" uint64_t snap_os_heap_base(void);   // runtime/os/cheat_heap.cpp (the HLE tests do not link runtime/os: bound in hle_host_win32.cpp / weak in hle_host_posix.cpp)
extern "C" uint64_t snap_os_heap_base_none(void) { return 0; }

namespace hle::cheats {

namespace {

constexpr const char* kBuildId = "DDB900FF04A912ED42430508389853CFAF1ACCCE";   // the 2.0.1 main NSO's build id (same value as runtime/nvn/nvn_pipeline.cpp)
constexpr uint64_t kMainBase = 0x7100000000ull;                                // host/loader.h GUEST_MAIN_BASE; the guest's addresses are host addresses
constexpr size_t kStepBudget = 100000;

std::mutex g_mu;
std::unique_ptr<cheat::Vm> g_vm;
bool g_loaded = false;          // a load was tried since the last reload
bool g_found = false;
std::string g_file, g_status = "Cheats are off.", g_last_error;
std::string g_test_text;
bool g_test = false;
uint64_t g_test_heap = 0, g_test_keys = 0;
uint64_t g_heap_cache = 0;
std::atomic<bool> g_master{false};

std::string env(const char* n) { const char* e = std::getenv(n); return e && *e ? e : ""; }
std::string build_id() { const std::string e = env("SNAP_GAME_BUILD_ID"); return e.empty() ? kBuildId : e; }

bool range_ok(uint64_t a, size_t n, bool write) { return hle::host::mem_range_ok(a, n, write); }

uint64_t game_heap_base() {
  if (g_test) return g_test_heap;
  if (!g_heap_cache) {
    g_heap_cache = snap_os_heap_base();
    if (g_heap_cache) hle::log("cheats: the game's heap starts at %#llx", (unsigned long long)g_heap_cache);
  }
  return g_heap_cache;
}

struct GuestMemory final : cheat::Memory {
  bool read(uint64_t a, void* d, size_t n) noexcept override {
    if (!range_ok(a, n, false)) return false;
    std::memcpy(d, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), n);
    return true;
  }
  bool write(uint64_t a, const void* s, size_t n) noexcept override {
    if (!range_ok(a, n, true)) return false;
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), s, n);
    return true;
  }
  uint64_t heap_base() const noexcept override { return game_heap_base(); }
  uint64_t main_nso_base() const noexcept override { return kMainBase; }
  uint64_t alias_base() const noexcept override { return 0; }   // the game maps no alias region: an address built from it fails the run
  uint64_t aslr_base() const noexcept override { return kMainBase; }
  uint64_t keys_held() const noexcept override {
    if (g_test) return g_test_keys;
    return input::hub().pad().buttons;   // PadSample.buttons has the Switch key layout (A = bit 0 ... Down = bit 15), the one the cheat format uses
  }
  bool pause() noexcept override { return true; }    // the VM runs on the game's own thread at a safe point: nothing else to stop
  bool resume() noexcept override { return true; }
  void debug_log(unsigned id, uint64_t value, unsigned width) noexcept override {
    hle::log("cheat: debug log %u = %#llx (%u bytes)", id, (unsigned long long)value, width);
  }
};

std::set<std::string> saved_on() {
  std::set<std::string> r;
  const std::string e = env("SNAP_CHEATS_ON");   // test runs: the enabled names joined with |, instead of the saved list
  const std::string v = e.empty() ? settings::get("cheats_on", "") : e;
  size_t i = 0;
  while (i <= v.size()) {
    size_t j = v.find('|', i);
    if (j == std::string::npos) j = v.size();
    if (j > i) r.insert(v.substr(i, j - i));
    i = j + 1;
  }
  return r;
}
void save_on() {
  std::string v;
  if (g_vm) for (const auto& s : g_vm->program().sections) if (!s.master && s.enabled) v += (v.empty() ? "" : "|") + s.name;
  if (v.empty()) settings::erase("cheats_on"); else settings::set("cheats_on", v);
}

std::string dir_for_files() {
  const std::string o = env("SNAP_CHEATS_DIR");
  if (!o.empty()) return o;
  return (std::filesystem::u8path(hle::host::exe_dir()) / "cheats").string();
}

bool build_vm(const std::string& text, const std::string& where) {
  cheat::Program prog;
  cheat::Error err;
  if (!cheat::parse(text, prog, err)) {
    g_vm.reset();
    g_status = "The cheat file has a mistake on line " + std::to_string(err.line) + ": " + err.message;
    hle::log("cheats: %s (%s)", g_status.c_str(), where.c_str());
    return false;
  }
  try { g_vm = std::make_unique<cheat::Vm>(std::move(prog)); }
  catch (const std::exception& e) { g_vm.reset(); g_status = std::string("The cheat file was refused: ") + e.what(); return false; }
  const std::set<std::string> on = saved_on();
  size_t n = 0, nm = 0;
  for (size_t i = 0; i < g_vm->program().sections.size(); ++i) {
    const auto& s = g_vm->program().sections[i];
    if (s.master) { ++nm; continue; }
    ++n;
    if (on.count(s.name)) g_vm->enable(i, true);
  }
  g_status = "Loaded " + std::to_string(n) + " cheat" + (n == 1 ? "" : "s") + (nm ? " and " + std::to_string(nm) + " always-on" : "") + " from " + where + ".";
  hle::log("cheats: %s", g_status.c_str());
  return true;
}

// Under g_mu.
void ensure_loaded() {
  if (g_loaded) return;
  g_loaded = true;
  g_vm.reset();
  g_found = false;
  g_last_error.clear();
  if (g_test) { build_vm(g_test_text, "the test text"); g_found = (bool)g_vm; return; }
  namespace fs = std::filesystem;
  const fs::path dir = dir_for_files();
  const std::string id = build_id();
  const std::string names[] = {id + ".txt", id.substr(0, 16) + ".txt"};   // the whole build id, or the first 8 bytes (the usual name)
  g_file = (dir / names[0]).string();
  std::error_code ec;
  for (const std::string& n : names) {
    const fs::path p = dir / n;
    if (!fs::is_regular_file(p, ec)) continue;
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    g_file = p.string();
    g_found = true;
    build_vm(ss.str(), "cheats\\" + n);
    return;
  }
  g_status = "No cheat file for this game build. Put one at " + g_file + ".";
}

bool run_locked() {
  ensure_loaded();
  if (!g_vm) return false;
  GuestMemory mem;
  const cheat::Result r = g_vm->run(mem, kStepBudget);
  if (!r.ok) {
    const std::string e = "line " + std::to_string(r.error.line) + ": " + r.error.message;
    std::string who;
    const auto& secs = g_vm->program().sections;
    if (r.section < secs.size() && !secs[r.section].master) {   // switch the failing cheat off, so the cheats after it in the file keep running
      who = "\"" + secs[r.section].name + "\" ";
      g_vm->enable(r.section, false);   // not saved: the list in settings still names it, and a reload tries it again
    }
    if (e != g_last_error) { g_last_error = e; hle::log("cheats: %sfailed at %s%s", who.c_str(), e.c_str(), who.empty() ? "" : " and was switched off"); }
    g_status = (who.empty() ? "An always-on cheat failed at " : "The cheat " + who + "was switched off: ") + e;
    return false;
  }
  return true;
}

}  // namespace

bool master_on() {
  if (!env("SNAP_CHEATS").empty()) { const std::string v = env("SNAP_CHEATS"); return v != "0" && v != "off"; }
  return settings::get_bool("cheats_enabled", false);
}

void set_master(bool on) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (on) settings::set_bool("cheats_enabled", true); else settings::erase("cheats_enabled");
  g_master = on;
  if (on) { g_loaded = false; ensure_loaded(); }
}

std::vector<Entry> entries() {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_loaded();
  std::vector<Entry> r;
  if (g_vm) for (const auto& s : g_vm->program().sections) r.push_back({s.name, s.master, s.enabled});
  return r;
}

void set_on(size_t index, bool on) {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_loaded();
  if (!g_vm || index >= g_vm->program().sections.size() || g_vm->program().sections[index].master) return;
  g_vm->enable(index, on);
  save_on();
}

void reload() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_loaded = false;
  ensure_loaded();
}

std::string file_path() {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_loaded();
  return g_file;
}

std::string status() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_master || master_on()) ensure_loaded();
  return g_status;
}

bool active() {
  if (!master_on()) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_loaded();
  if (!g_vm) return false;
  for (const auto& s : g_vm->program().sections) if (s.master || s.enabled) return true;
  return false;
}

void tick() {
  static int last_present = -1, last_check = -1000000;
  const int pc = nvn::g_presentCount;
  if (pc == last_present) return;   // the game reads the pad more than once a frame
  last_present = pc;
  if (pc - last_check >= 30 || pc < last_check) { last_check = pc; g_master = master_on(); }   // the menu and the env var are looked at twice a second, not every frame
  if (!g_master) return;
  std::lock_guard<std::mutex> lk(g_mu);
  static int noted = 0;
  if (noted < 2 && pc >= (noted ? 7200 : 1800)) {   // once a minute or so into the game: say whether the game has a heap region cheats can use
    ++noted;
    hle::log("cheats: memory heap base %#llx (0 = the game has not set one: cheats that use the heap fail)", (unsigned long long)game_heap_base());
  }
  run_locked();
}

bool test_load(const std::string& text, std::string* error) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_test = true; g_test_text = text; g_loaded = false;
  ensure_loaded();
  if (error) *error = g_vm ? "" : g_status;
  return (bool)g_vm;
}

void test_hooks(uint64_t heap_base, uint64_t keys) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_test_heap = heap_base; g_test_keys = keys;
}

bool test_run_once() {
  std::lock_guard<std::mutex> lk(g_mu);
  return run_locked();
}

}  // namespace hle::cheats
