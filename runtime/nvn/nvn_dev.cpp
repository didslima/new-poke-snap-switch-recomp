// SPDX-License-Identifier: MIT
// Developer loop (project code, ours, no game data). See nvn_dev.h and docs/devloop.md. Off unless SNAP_DEV=1: with it unset nothing here
// runs (no thread, no file, no allocation); the inline wrappers in the header are one cached-bool branch.
#include "nvn_dev.h"

#include "nvn_os_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

bool dev_env_on() {
  const char* e = getenv("SNAP_DEV");
  return e && *e && strcmp(e, "0") != 0 && !nvn::os_host::ascii_ieq(e, "false") && !nvn::os_host::ascii_ieq(e, "off");
}

namespace {
double now_s() {
  return (double)nvn::os_host::monotonic_ns() / 1e9;
}
bool file_exists(const std::string& p) { return nvn::os_host::file_exists(p); }
bool file_time(const std::string& p, int64_t* t) { return nvn::os_host::file_mtime_ns(p, t); }
bool same_time(int64_t a, int64_t b) { return a == b; }
bool read_file(const std::string& p, std::string* out) {
  std::vector<uint8_t> bytes;
  if (!nvn::os_host::read_file_bytes(p, &bytes)) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}
bool write_file(const std::string& p, const std::string& data) {   // via a temp name and a rename, so a reader never sees half a file
  const std::string tmp = p + ".tmp";
  if (!nvn::os_host::write_file_bytes(tmp, data.data(), data.size())) {
    nvn::os_host::remove_file(tmp);
    return false;
  }
  if (!nvn::os_host::atomic_replace(tmp, p)) {
    nvn::os_host::remove_file(tmp);
    return false;
  }
  return true;
}
std::string exe_dir() { return nvn::os_host::exe_dir(); }
std::string parent(const std::string& p) { const size_t k = p.find_last_of("\\/"); return k == std::string::npos ? std::string() : p.substr(0, k); }
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
void logf(const char* fmt, ...) {
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  fprintf(stderr, "[dev] %s\n", b); fflush(stderr);
}

// ---- params ------------------------------------------------------------------------------------------------------------
struct Param {
  std::string name; char kind = 'f'; std::vector<void*> ptrs; double mn = 0, mx = 1, def = 0;
  double get() const {
    void* p = ptrs.empty() ? nullptr : ptrs[0];
    if (!p) return def;
    return kind == 'f' ? (double)*(volatile float*)p : kind == 'i' ? (double)*(volatile int*)p : (*(volatile bool*)p ? 1.0 : 0.0);
  }
  void put(double v) {
    v = std::min(std::max(v, mn), mx);
    for (void* p : ptrs) {
      if (kind == 'f') *(volatile float*)p = (float)v;
      else if (kind == 'i') *(volatile int*)p = (int)(v + (v < 0 ? -0.5 : 0.5));
      else *(volatile bool*)p = v >= 0.5;
    }
  }
};
std::mutex g_pm;
std::deque<Param> g_params;                       // stable addresses
std::unordered_map<std::string_view, int> g_pidx; // views into g_params names

Param* find_param(std::string_view n) { auto it = g_pidx.find(n); return it == g_pidx.end() ? nullptr : &g_params[(size_t)it->second]; }

bool parse_env(const char* name, double* v) {
  const char* e = getenv(name);
  if (!e || !*e) return false;
  if (nvn::os_host::ascii_ieq(e, "true") || nvn::os_host::ascii_ieq(e, "on")) { *v = 1; return true; }
  if (nvn::os_host::ascii_ieq(e, "false") || nvn::os_host::ascii_ieq(e, "off")) { *v = 0; return true; }
  *v = atof(e);
  return true;
}

void reg_param(const char* name, char kind, void* ptr, double mn, double mx, double def) {
  std::lock_guard<std::mutex> lk(g_pm);
  Param* p = find_param(name);
  if (!p) {
    g_params.push_back(Param{});
    p = &g_params.back();
    p->name = name; p->kind = kind;
    g_pidx[p->name] = (int)g_params.size() - 1;
  }
  p->mn = mn; p->mx = mx; p->def = def;
  const bool fresh = std::find(p->ptrs.begin(), p->ptrs.end(), ptr) == p->ptrs.end();
  if (fresh) p->ptrs.push_back(ptr);
  if (fresh) {
    double v;
    if (parse_env(name, &v)) p->put(v);
    else if (p->ptrs.size() > 1) p->put(p->get());   // a second pointer for the same name starts at the first one's value
  }
}
}  // namespace

void dev_param_f_impl(const char* name, float* p, float mn, float mx, float def) { reg_param(name, 'f', p, mn, mx, def); }
void dev_param_i_impl(const char* name, int* p, int mn, int mx, int def) { reg_param(name, 'i', p, mn, mx, def); }
void dev_param_b_impl(const char* name, bool* p, bool def) { reg_param(name, 'b', p, 0, 1, def ? 1 : 0); }

float dev_envf(const char* env, float def, float mn, float mx) {
  if (!dev_on()) { const char* e = getenv(env); return e && *e ? (float)atof(e) : def; }
  {
    std::lock_guard<std::mutex> lk(g_pm);
    if (Param* p = find_param(env)) return (float)p->get();
  }
  float* store = new float(def);   // lives for the process, like every registered pointer
  { double v; if (parse_env(env, &v)) *store = (float)v; }
  reg_param(env, 'f', store, mn, mx, def);
  std::lock_guard<std::mutex> lk(g_pm);
  Param* p = find_param(env);
  return p ? (float)p->get() : *store;
}
int dev_envi(const char* env, int def, int mn, int mx) {
  if (!dev_on()) { const char* e = getenv(env); return e && *e ? atoi(e) : def; }
  {
    std::lock_guard<std::mutex> lk(g_pm);
    if (Param* p = find_param(env)) return (int)p->get();
  }
  int* store = new int(def);
  { double v; if (parse_env(env, &v)) *store = (int)v; }
  reg_param(env, 'i', store, mn, mx, def);
  std::lock_guard<std::mutex> lk(g_pm);
  Param* p = find_param(env);
  return p ? (int)p->get() : *store;
}

int dev_param_list(DevParamView* out, int max) {
  std::lock_guard<std::mutex> lk(g_pm);
  int n = 0;
  for (const Param& p : g_params) {
    if (n >= max) break;
    out[n++] = DevParamView{p.name.c_str(), p.kind, p.get(), p.mn, p.mx, p.def};   // names are stable: g_params only grows
  }
  return n;
}
bool dev_param_set(const char* name, double v) {
  std::lock_guard<std::mutex> lk(g_pm);
  Param* p = find_param(name);
  if (!p) return false;
  p->put(v);
  return true;
}
bool dev_param_reset(const char* name) {
  std::lock_guard<std::mutex> lk(g_pm);
  Param* p = find_param(name);
  if (!p) return false;
  p->put(p->def);
  return true;
}

// ---- shaders -----------------------------------------------------------------------------------------------------------
namespace {
struct Shader {
  std::string name, src, stage;           // src: source path when one exists
  const uint32_t* emb = nullptr; size_t embBytes = 0;
  const uint32_t* words = nullptr; size_t bytes = 0;
  uint32_t gen = 0;
  int64_t srcT = 0, spvT = 0, pendT = 0;   // last applied source / spv time, source time seen but not yet settled
  bool havePend = false, inited = false, inflight = false, fromSpv = false;
  std::string err;
};
std::mutex g_sm;
std::deque<Shader> g_shaders;
std::unordered_map<std::string_view, int> g_sidx;
std::deque<std::vector<uint32_t>> g_blobs;
std::map<std::string, int64_t> g_incT;    // include files (.glsl / .h) seen in the shader dir
std::string g_dir, g_glslang;
bool g_dirInit = false;

struct Job { std::string name, src, stage, dir, glslang; int64_t t; };
struct Result { std::string name; int64_t t; bool ok; std::vector<uint32_t> words; std::string err; double ms; };
std::mutex g_jm;
std::condition_variable g_jcv;
std::deque<Job> g_jobs;
std::vector<Result> g_results;
std::thread* g_worker = nullptr;

void init_dir() {   // g_sm held
  if (g_dirInit) return;
  g_dirInit = true;
  const char* d = getenv("SNAP_SHADER_DIR");
  if (!d || !*d) return;
  g_dir = d;
  while (!g_dir.empty() && (g_dir.back() == '\\' || g_dir.back() == '/')) g_dir.pop_back();
  if (const char* g = getenv("SNAP_GLSLANG"); g && *g) { g_glslang = g; }
  else {
    for (std::string start : {g_dir, exe_dir()}) {
      for (std::string p = start; !p.empty() && g_glslang.empty(); p = parent(p)) {
        const std::string c = nvn::os_host::glslang_sdk_candidate(p);
        if (file_exists(c)) g_glslang = c;
      }
      if (!g_glslang.empty()) break;
    }
    if (g_glslang.empty()) g_glslang = nvn::os_host::glslang_default_exe();
  }
  logf("shader dir %s, compiler %s", g_dir.c_str(), g_glslang.c_str());
}

const char* kSrcExt[3][2] = {{".frag", "frag"}, {".comp", "comp"}, {".vert", "vert"}};
bool find_src(const std::string& dir, const std::string& name, std::string* src, std::string* stage) {
  for (auto& e : kSrcExt) {
    const std::string p = dir + std::string(1, nvn::os_host::path_sep()) + name + e[0];
    if (file_exists(p)) { *src = p; *stage = e[1]; return true; }
  }
  return false;
}

// glslangValidator -V -S <stage> -I<dir> <src> -o <tmp>; the same flags as shaders/make_*_spv.py so the result matches the embedded copy
bool compile_file(const Job& j, std::vector<uint32_t>* words, std::string* err) {
  const std::string sep(1, nvn::os_host::path_sep());
  std::string tdir = nvn::os_host::temp_dir();
  if (!tdir.empty() && tdir.back() != '\\' && tdir.back() != '/') tdir += sep;
  const std::string tmp = tdir + "snapdev_" + std::to_string(nvn::os_host::current_process_id()) + "_" + j.name + ".spv";
  nvn::os_host::remove_file(tmp);
  const std::string cmd = "\"" + j.glslang + "\" -V -S " + j.stage + " -I\"" + j.dir + "\" \"" + j.src + "\" -o \"" + tmp + "\"";
  std::string out;
  int code = 1;
  if (!nvn::os_host::run_process_capture(cmd, &out, &code)) { *err = "cannot start " + j.glslang; return false; }
  std::string bin;
  const bool have = code == 0 && read_file(tmp, &bin) && bin.size() >= 20 && bin.size() % 4 == 0;
  nvn::os_host::remove_file(tmp);
  if (!have) { *err = out.empty() ? "compile failed" : out; return false; }
  words->resize(bin.size() / 4);
  memcpy(words->data(), bin.data(), bin.size());
  if ((*words)[0] != 0x07230203u) { *err = "output is not SPIR-V"; return false; }
  return true;
}

void worker_main() {
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> lk(g_jm);
      g_jcv.wait(lk, [] { return !g_jobs.empty(); });
      j = std::move(g_jobs.front()); g_jobs.pop_front();
    }
    Result r; r.name = j.name; r.t = j.t;
    const double t0 = now_s();
    r.ok = compile_file(j, &r.words, &r.err);
    r.ms = (now_s() - t0) * 1000.0;
    std::lock_guard<std::mutex> lk(g_jm);
    g_results.push_back(std::move(r));
  }
}
void enqueue(Job j) {
  std::lock_guard<std::mutex> lk(g_jm);
  if (!g_worker) g_worker = new std::thread(worker_main);   // never joined: it sleeps on the cv until exit
  g_jobs.push_back(std::move(j));
  g_jcv.notify_one();
}
std::string first_lines(const std::string& s, int n) {
  std::string o; int c = 0;
  for (size_t i = 0; i < s.size() && c < n; i++) { if (s[i] == '\r') continue; o += s[i]; if (s[i] == '\n') c++; }
  while (!o.empty() && (o.back() == '\n' || o.back() == ' ')) o.pop_back();
  return o;
}
}  // namespace

DevSpv dev_spv_impl(const char* name, const uint32_t* embedded, size_t embedded_bytes) {
  Job job; bool compile = false; Shader* sh = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_sm);
    init_dir();
    auto it = g_sidx.find(name);
    if (it != g_sidx.end()) {
      sh = &g_shaders[(size_t)it->second];
      return DevSpv{sh->words, sh->bytes};
    }
    g_shaders.push_back(Shader{});
    sh = &g_shaders.back();
    sh->name = name; sh->emb = embedded; sh->embBytes = embedded_bytes; sh->words = embedded; sh->bytes = embedded_bytes;
    g_sidx[sh->name] = (int)g_shaders.size() - 1;
    if (!g_dir.empty()) {
      std::string src, stage;
      if (find_src(g_dir, name, &src, &stage)) {
        sh->src = src; sh->stage = stage;
        int64_t t = 0; if (file_time(src, &t)) sh->srcT = t;
        job = Job{name, src, stage, g_dir, g_glslang, sh->srcT};
        compile = true;
      }
      const std::string sp = g_dir + std::string(1, nvn::os_host::path_sep()) + name + ".spv";
      file_time(sp, &sh->spvT);
    }
  }
  std::vector<uint32_t> words; std::string err; double ms = 0;
  bool ok = false;
  if (compile) {
    const double t0 = now_s();
    ok = compile_file(job, &words, &err);   // synchronous at first use: the shader is needed right now
    ms = (now_s() - t0) * 1000.0;
  }
  std::lock_guard<std::mutex> lk(g_sm);
  sh = &g_shaders[(size_t)g_sidx.find(name)->second];
  if (ok) {
    g_blobs.push_back(std::move(words));
    sh->words = g_blobs.back().data(); sh->bytes = g_blobs.back().size() * 4;
    logf("shader %s: compiled %s in %.0f ms (first use)", name, sh->src.c_str(), ms);
  } else {
    if (compile) { sh->err = err; logf("shader %s: COMPILE ERROR, using the %s copy:\n%s", name, "embedded", first_lines(err, 12).c_str()); }
    if (!compile && !g_dir.empty()) {
      std::string bin;
      const std::string sp = g_dir + std::string(1, nvn::os_host::path_sep()) + name + ".spv";
      if (read_file(sp, &bin) && bin.size() >= 20 && bin.size() % 4 == 0) {
        std::vector<uint32_t> w(bin.size() / 4); memcpy(w.data(), bin.data(), bin.size());
        if (w[0] == 0x07230203u) {
          g_blobs.push_back(std::move(w));
          sh->words = g_blobs.back().data(); sh->bytes = g_blobs.back().size() * 4; sh->fromSpv = true;
          logf("shader %s: using %s", name, sp.c_str());
        }
      }
    }
  }
  sh->inited = true;
  return DevSpv{sh->words, sh->bytes};
}

bool dev_spv_changed_impl(const char* name, uint32_t* seen) {
  std::lock_guard<std::mutex> lk(g_sm);
  auto it = g_sidx.find(name);
  if (it == g_sidx.end()) return false;
  const uint32_t g = g_shaders[(size_t)it->second].gen;
  if (*seen == g) return false;
  *seen = g;
  return true;
}

int dev_shader_errors(char* out, int out_len) {
  std::lock_guard<std::mutex> lk(g_sm);
  std::string all; int n = 0;
  for (const Shader& s : g_shaders) {
    if (s.err.empty()) continue;
    n++;
    all += s.name + ": " + first_lines(s.err, 5) + "\n";
  }
  if (out && out_len > 0) snprintf(out, (size_t)out_len, "%s", all.c_str());
  return n;
}

namespace {
// a shader that has a source file: queue a compile when it changed (called under g_sm). `force` ignores the timestamps.
void request_compile(Shader& s, bool force) {
  if (s.src.empty() || s.inflight) return;
  int64_t t = 0; if (!file_time(s.src, &t)) return;
  if (!force && same_time(t, s.srcT)) return;
  s.inflight = true;
  s.havePend = false;
  enqueue(Job{s.name, s.src, s.stage, g_dir, g_glslang, t});
}
}  // namespace

void dev_shader_reload_all() {
  std::lock_guard<std::mutex> lk(g_sm);
  for (Shader& s : g_shaders) request_compile(s, true);
}

namespace {
// the watcher: runs at 4 Hz from dev_frame. A change must hold still for one scan (editors write twice) before it is compiled.
void shader_scan() {
  std::lock_guard<std::mutex> lk(g_sm);
  if (g_dir.empty()) return;
  // include files: a changed one recompiles every watched shader that has a source
  {
    const std::string sep(1, nvn::os_host::path_sep());
    bool incChanged = false;
    for (const char* pat : {".glsl", ".h"}) {
      for (const std::string& n : nvn::os_host::list_files(g_dir, nullptr, pat)) {
        int64_t ft = 0;
        if (!file_time(g_dir + sep + n, &ft)) continue;
        auto it = g_incT.find(n);
        if (it == g_incT.end()) g_incT[n] = ft;
        else if (!same_time(it->second, ft)) { it->second = ft; incChanged = true; logf("include %s changed", n.c_str()); }
      }
    }
    if (incChanged) for (Shader& s : g_shaders) request_compile(s, true);
  }
  for (Shader& s : g_shaders) {
    if (!s.inited) continue;
    if (s.src.empty()) {   // a source may appear later; a .spv may change
      std::string src, stage;
      if (find_src(g_dir, s.name, &src, &stage)) { s.src = src; s.stage = stage; s.srcT = 0; request_compile(s, true); continue; }
      int64_t t = 0;
      const std::string sp = g_dir + std::string(1, nvn::os_host::path_sep()) + s.name + ".spv";
      if (file_time(sp, &t) && !same_time(t, s.spvT)) {
        s.spvT = t;
        std::string bin;
        if (read_file(sp, &bin) && bin.size() >= 20 && bin.size() % 4 == 0) {
          std::vector<uint32_t> w(bin.size() / 4); memcpy(w.data(), bin.data(), bin.size());
          if (w[0] == 0x07230203u) {
            g_blobs.push_back(std::move(w));
            s.words = g_blobs.back().data(); s.bytes = g_blobs.back().size() * 4; s.gen++; s.err.clear();
            logf("shader %s: reloaded %s (generation %u)", s.name.c_str(), sp.c_str(), s.gen);
          }
        }
      }
      continue;
    }
    int64_t t = 0;
    if (!file_time(s.src, &t) || same_time(t, s.srcT) || s.inflight) continue;
    if (s.havePend && same_time(t, s.pendT)) request_compile(s, false);
    else { s.pendT = t; s.havePend = true; }
  }
}

void drain_results() {
  std::vector<Result> rs;
  { std::lock_guard<std::mutex> lk(g_jm); rs.swap(g_results); }
  if (rs.empty()) return;
  std::lock_guard<std::mutex> lk(g_sm);
  for (Result& r : rs) {
    auto it = g_sidx.find(r.name);
    if (it == g_sidx.end()) continue;
    Shader& s = g_shaders[(size_t)it->second];
    s.inflight = false; s.srcT = r.t;
    if (r.ok) {
      g_blobs.push_back(std::move(r.words));
      s.words = g_blobs.back().data(); s.bytes = g_blobs.back().size() * 4; s.gen++; s.err.clear();
      logf("shader %s reloaded live in %.0f ms (generation %u)", s.name.c_str(), r.ms, s.gen);
    } else {
      s.err = r.err;
      logf("shader %s: COMPILE ERROR, keeping the previous shader:\n%s", s.name.c_str(), first_lines(r.err, 12).c_str());
    }
  }
}
}  // namespace

// ---- pass timing -------------------------------------------------------------------------------------------------------
namespace {
std::mutex g_tm;
std::unordered_map<std::string, double> g_acc;
int g_accFrames = 0; double g_accBusy = 0;
double g_winEnd = 0;
std::vector<DevPassStat> g_last;
double g_lastBusy = -1;
bool g_passesDirty = false;
std::atomic<bool> g_overlay{false}, g_timing{false}, g_all{false};
}  // namespace

void dev_pass_sample(const char* label, double ms) {
  if (!label || label[0] == '(' || !strcmp(label, "cb-start")) return;
  std::lock_guard<std::mutex> lk(g_tm);
  g_acc[label] += ms;
}
void dev_gpu_frame(double busy_ms) {
  std::lock_guard<std::mutex> lk(g_tm);
  g_accFrames++; g_accBusy += busy_ms;
  const double t = now_s();
  if (g_winEnd == 0) g_winEnd = t + 1.0;
  if (t < g_winEnd) return;
  g_winEnd = t + 1.0;
  g_last.clear();
  for (auto& kv : g_acc) {
    DevPassStat s{}; snprintf(s.name, sizeof s.name, "%s", kv.first.c_str()); s.ms = kv.second / std::max(1, g_accFrames);
    g_last.push_back(s);
  }
  std::sort(g_last.begin(), g_last.end(), [](const DevPassStat& a, const DevPassStat& b) { return a.ms > b.ms; });
  g_lastBusy = g_accBusy / std::max(1, g_accFrames);
  g_acc.clear(); g_accFrames = 0; g_accBusy = 0; g_passesDirty = true;
}
int dev_pass_stats(DevPassStat* out, int max) {
  std::lock_guard<std::mutex> lk(g_tm);
  const int n = std::min<int>(max, (int)g_last.size());
  for (int i = 0; i < n; i++) out[i] = g_last[(size_t)i];
  return n;
}
double dev_gpu_busy() { std::lock_guard<std::mutex> lk(g_tm); return g_lastBusy; }

bool dev_overlay_get() { return dev_on() && g_overlay.load(std::memory_order_relaxed); }
bool dev_timing_wanted() { return dev_on() && (g_overlay.load(std::memory_order_relaxed) || g_timing.load(std::memory_order_relaxed)); }
static void update_timing() {
  const bool want = dev_timing_wanted();
  dev_timing_apply(want);
  if (!want) { std::lock_guard<std::mutex> lk(g_tm); g_last.clear(); g_lastBusy = -1; g_acc.clear(); g_accFrames = 0; g_accBusy = 0; g_winEnd = 0; }
}
void dev_overlay_set(bool on) { if (!dev_on()) return; g_overlay.store(on); update_timing(); }

// ---- control channel ---------------------------------------------------------------------------------------------------
namespace {
std::string g_ctl; bool g_ctlInit = false;
double g_nextPoll = 0, g_nextScan = 0;
std::string ctl_path(const char* ext) { return g_ctl + std::string(1, nvn::os_host::path_sep()) + std::to_string(nvn::os_host::current_process_id()) + ext; }

void ctl_init() {
  if (g_ctlInit) return;
  g_ctlInit = true;
  const std::string sep(1, nvn::os_host::path_sep());
  if (const char* c = getenv("SNAP_DEV_CTL"); c && *c) g_ctl = c;
  else {
    const std::string ed = exe_dir();
    for (std::string p = ed; !p.empty(); p = parent(p)) {
      const size_t k = p.find_last_of("\\/");
      if (lower(k == std::string::npos ? p : p.substr(k + 1)) == "build") { g_ctl = p + sep + "ctl"; break; }
    }
    if (g_ctl.empty()) g_ctl = ed + sep + "ctl";
  }
  // create the directory chain
  nvn::os_host::ensure_dir(g_ctl);
  const unsigned long long pid = static_cast<unsigned long long>(nvn::os_host::current_process_id());
  std::string info = "pid " + std::to_string(pid) + "\nprotocol 1\nexe " + exe_dir() + "\n";
  write_file(ctl_path(".info"), info);
  // a leftover command file of an earlier process that had this pid must not run
  nvn::os_host::remove_file(ctl_path(".cmd"));
  logf("control channel %s%c%llu.cmd", g_ctl.c_str(), nvn::os_host::path_sep(), pid);
}

std::string fnum(double v) { char b[48]; snprintf(b, sizeof b, "%.6g", v); return b; }

std::string handle_cmd(const std::string& line) {
  char verb[32] = {}, a[256] = {}, b[256] = {};
  const int n = sscanf(line.c_str(), "%31s %255s %255s", verb, a, b);
  if (n < 1) return "";
  const std::string v = lower(verb);
  if (v == "set") {
    if (n < 3) return "err set needs NAME VALUE\n";
    double x;
    if (nvn::os_host::ascii_ieq(b, "true") || nvn::os_host::ascii_ieq(b, "on")) x = 1; else if (nvn::os_host::ascii_ieq(b, "false") || nvn::os_host::ascii_ieq(b, "off")) x = 0; else x = atof(b);
    if (!dev_param_set(a, x)) return std::string("err unknown param ") + a + "\n";
    std::lock_guard<std::mutex> lk(g_pm);
    return std::string("ok ") + a + " " + fnum(find_param(a)->get()) + "\n";
  }
  if (v == "get") {
    std::lock_guard<std::mutex> lk(g_pm);
    Param* p = find_param(a);
    return p ? std::string("ok ") + a + " " + fnum(p->get()) + "\n" : std::string("err unknown param ") + a + "\n";
  }
  if (v == "reset") return dev_param_reset(a) ? std::string("ok reset ") + a + "\n" : std::string("err unknown param ") + a + "\n";
  if (v == "list") {
    std::string o = "ok list\n";
    std::lock_guard<std::mutex> lk(g_pm);
    for (const Param& p : g_params) o += p.name + " " + p.kind + " " + fnum(p.get()) + " " + fnum(p.mn) + " " + fnum(p.mx) + " " + fnum(p.def) + "\n";
    return o;
  }
  if (v == "passes") {
    std::string o = "ok passes\n";
    DevPassStat st[64]; const int c = dev_pass_stats(st, 64);
    o += "gpu_busy_ms " + fnum(dev_gpu_busy()) + "\n";
    for (int i = 0; i < c; i++) o += std::string(st[i].name) + "\t" + fnum(st[i].ms) + "\n";
    if (!dev_timing_wanted()) o += "# timing is off: send `timing on` or `overlay on`\n";
    return o;
  }
  if (v == "overlay") { dev_overlay_set(n < 2 || !nvn::os_host::ascii_ieq(a, "off")); return std::string("ok overlay ") + (dev_overlay_get() ? "on" : "off") + "\n"; }
  if (v == "timing") { g_timing.store(n < 2 || !nvn::os_host::ascii_ieq(a, "off")); update_timing(); return std::string("ok timing ") + (g_timing.load() ? "on" : "off") + "\n"; }
  if (v == "renderer") { g_all.store(n < 2 || !nvn::os_host::ascii_ieq(a, "off")); update_timing(); return std::string("ok renderer marks ") + (g_all.load() ? "on" : "off") + "\n"; }
  if (v == "reload") {
    if (n >= 2) {
      std::lock_guard<std::mutex> lk(g_sm);
      auto it = g_sidx.find(a);
      if (it == g_sidx.end()) return std::string("err unknown shader ") + a + "\n";
      request_compile(g_shaders[(size_t)it->second], true);
    } else dev_shader_reload_all();
    return "ok reload queued\n";
  }
  if (v == "shaders") {
    std::string o = "ok shaders\n";
    std::lock_guard<std::mutex> lk(g_sm);
    for (const Shader& s : g_shaders) o += s.name + " gen " + std::to_string(s.gen) + (s.src.empty() ? "" : " src") + (s.err.empty() ? "" : " ERROR") + "\n";
    return o;
  }
  if (v == "ping") return "ok pong\n";
  return std::string("err unknown command ") + verb + "\n";
}

void ctl_poll() {
  const std::string cmd = ctl_path(".cmd");
  std::string body;
  if (!file_exists(cmd) || !read_file(cmd, &body)) return;
  nvn::os_host::remove_file(cmd);   // consume first: a script waits for this file to vanish, then reads <pid>.out
  std::string out;
  size_t i = 0;
  while (i <= body.size()) {
    size_t e = body.find('\n', i);
    if (e == std::string::npos) e = body.size();
    std::string line = body.substr(i, e - i);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line[0] != '#') out += handle_cmd(line);
    i = e + 1;
  }
  write_file(ctl_path(".out"), out);
}
}  // namespace

const char* dev_ctl_dir() { ctl_init(); return g_ctl.c_str(); }
bool dev_renderer_marks() { return g_all.load(std::memory_order_relaxed); }
void dev_renderer_set(bool on) { if (!dev_on()) return; g_all.store(on); update_timing(); }

void dev_frame(uint64_t) {
  if (!dev_on()) return;
  static bool started = [] {
    ctl_init();
    if (const char* o = getenv("SNAP_DEV_OVERLAY"); o && *o && atoi(o) != 0) g_overlay.store(true);
    if (const char* o = getenv("SNAP_DEV_TIMING"); o && *o && atoi(o) != 0) g_timing.store(true);
    if (const char* o = getenv("SNAP_DEV_RENDERER_MARKS"); o && *o && atoi(o) != 0) g_all.store(true);
    update_timing();
    return true;
  }();
  (void)started;
  const double t = now_s();
  drain_results();
  if (t >= g_nextPoll) { g_nextPoll = t + 0.1; ctl_poll(); }
  if (t >= g_nextScan) { g_nextScan = t + 0.25; shader_scan(); }
  bool dirty = false;
  { std::lock_guard<std::mutex> lk(g_tm); dirty = g_passesDirty; g_passesDirty = false; }
  if (dirty) {
    std::string o = "gpu_busy_ms\t" + fnum(dev_gpu_busy()) + "\n";
    DevPassStat st[64]; const int c = dev_pass_stats(st, 64);
    for (int i = 0; i < c; i++) o += std::string(st[i].name) + "\t" + fnum(st[i].ms) + "\n";
    write_file(ctl_path(".passes"), o);
  }
}
