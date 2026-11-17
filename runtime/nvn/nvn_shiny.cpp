// SPDX-License-Identifier: MIT
// Shiny Pokemon mod, renderer side (docs/shiny-mod-plan.md). Cosmetic only: a draw that samples a known body texture of a shiny
// instance samples a recolored twin of it instead. Nothing in the guest, the saves or the scoring is touched.
//   SNAP_SHINY=1            master switch (off by default)
//   SNAP_SHINY_ODDS=<n>     1 in n per instance (default 4096; 1 = every instance)
//   SNAP_SHINY_FORCE=<key>  hex data key of one body texture whose draws are always shiny (testing)
//   SNAP_SHINY_TRACE=<key>  log the draw state of draws sampling that key (hex key, or "all" = one line per texture)
//   SNAP_SHINY_DIR=<dir>    pack folder (default %APPDATA%\SnapRecomp\mods\shiny-pokemon), holds shiny_table.txt + twin .nutexb files
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_shiny.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <vector>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

extern "C" void snap_shiny_fx_trigger(const char* species);   // runtime/ui/ui_shiny.cpp
namespace nvn {
namespace {
struct TwinFile { uint32_t w = 0, h = 0, mips = 0; std::string file, species; const uint8_t* data = nullptr; bool tried = false; };
struct Cfg {
  bool on = false; std::string only; unsigned odds = 4096; bool forceAll = false; uint64_t force = 0; bool trace = false, traceAll = false; uint64_t traceKey = 0;
  std::unordered_map<uint64_t, TwinFile> table; std::string dir;
};
const Cfg& C() {
  static const Cfg c = [] {
    Cfg c;
    const char* e = getenv("SNAP_SHINY");
    c.on = e && *e && *e != '0';
    if ((e = getenv("SNAP_SHINY_ODDS")) && *e) c.odds = (unsigned)std::max(1ul, strtoul(e, nullptr, 0));
    if ((e = getenv("SNAP_SHINY_ONLY")) && *e) c.only = e;   // debug: only species whose name contains this are shiny (mixed shiny / normal proofs)
    if ((e = getenv("SNAP_SHINY_FORCE")) && *e) { c.force = strtoull(e, nullptr, 16); c.forceAll = !c.force; }
    if ((e = getenv("SNAP_SHINY_TRACE")) && *e && *e != '0') {
      c.trace = true;
      if (!strcmp(e, "all") || !strcmp(e, "1")) c.traceAll = true; else c.traceKey = strtoull(e, nullptr, 16);
    }
    if ((e = getenv("SNAP_SHINY_DIR")) && *e) c.dir = e;
    else if ((e = getenv("APPDATA")) && *e) c.dir = std::string(e) + "\\SnapRecomp\\mods\\shiny-pokemon";
    if (c.on) {
      std::ifstream f(c.dir + "\\shiny_table.txt");
      std::string line;
      while (std::getline(f, line)) {
        std::istringstream is(line); std::string k; TwinFile t;
        if (line.empty() || line[0] == '#' || !(is >> k >> t.w >> t.h >> t.mips >> t.file)) continue;
        is >> t.species;
        c.table[strtoull(k.c_str(), nullptr, 16)] = t;
      }
      fprintf(stderr, "[shiny] on, odds 1/%u, %zu twin textures from %s%s\n", c.odds, c.table.size(), c.dir.c_str(), c.forceAll ? ", force all" : (c.force ? ", force key" : ""));
    }
    return c;
  }();
  return c;
}
uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
std::mutex g_mu;
// M3 data (photo badge): the last present at which a shiny twin was drawn, and which species. A photo-time consumer asks
// snap_shiny_last(); nothing here changes rendering.
std::atomic<int> g_lastFrame{-1};
std::atomic<uint64_t> g_lastKey{0};
std::string g_lastSpecies;
std::mutex g_spMu;   // guards g_lastSpecies only (NoteShiny can run under g_mu)
// ---- M3 photo marks ----
std::mutex g_pm;
std::set<const void*> g_thumbs;                 // 360x240 thumbnails shot while a shiny was on screen
std::map<const void*, int> g_rtShiny;           // render target -> present of the last shiny draw since its last clear
std::atomic<int> g_lastShutter{-1000000};          // present of the last shutter thumbnail pass
std::atomic<int> g_copyFrame{-1};               // present of the last photo-sized copy from a flagged texture
struct BadgeRect { float r[4]; int frame; bool big; };   // big: sampled texture is the 1080x720 photo (Score polaroid)
std::vector<BadgeRect> g_badges;
void NoteRt(const void* rt, int w = 0, int h = 0) {
  if (!rt) return;
  std::lock_guard<std::mutex> g(g_pm);

  g_rtShiny[rt] = g_presentCount;
  static std::set<std::pair<const void*, int>> logged; static int n = 0;
  if (w && w != 1920 && logged.insert({rt, w}).second && n++ < 60) fprintf(stderr, "[shiny] shiny drawn into %p %dx%d at present %d\n", rt, w, h, g_presentCount);
}

// ---- subject-accurate flag: samples-passed per shiny draw ----
// A draw that used a twin is wrapped in its own occlusion query (nvn_draw.cpp DoDraw). Samples are summed per (present, species)
// and scaled to 1080x720 pixels. A shutter thumbnail is flagged only when a species covers >= kMinPx of the frame it copies.
thread_local std::string tl_twinSp; thread_local int tl_twinArea = 0;
void TagTwin(const std::string& sp, const ShinyDraw& d) { if (d.rtW >= 640) { tl_twinSp = sp; tl_twinArea = d.rtW * d.rtH; } }
struct PendQ { uint32_t qi; int frame; std::string sp; int area; };
std::vector<PendQ> g_pendQ;
std::map<int, std::map<std::string, double>> g_cov;   // present -> species -> 1080x720-equivalent pixels
VkQueryPool g_sqPool = VK_NULL_HANDLE; uint32_t g_sqNext = 0; const uint32_t kSq = 1024;
struct PendThumb { const void* dst; int shot; };
std::vector<PendThumb> g_pendThumb;
std::map<const void*, std::string> g_thumbSp;
bool g_selFlag = false; int g_selFrame = -100000;   // the photo last shown large in photo select: flagged?
const double kMinPxDefault = 300.0;
static double MinPx() { static const double v = [] { const char* e = getenv("SNAP_SHINY_MINPX"); return (e && *e) ? atof(e) : kMinPxDefault; }(); return v; }   // SNAP_SHINY_MINPX: debug, raises the coverage bar so a mixed shiny / plain run can be made from photos of the same species
struct PendRt { const void* rt; int present; std::vector<const void*> follow; };   // follow: textures copied from this target before its decision; they take the same decision
std::vector<PendRt> g_pendRt;                           // 1080x720 capture targets waiting for their coverage decision
std::map<const void*, std::string> g_rtSp;              // flagged target -> species
std::set<const void*> g_known;                          // textures with an explicit shiny/normal decision (capture chain or upload from a buffer)
struct BufInfo { bool shiny; std::string sp; };
std::map<const uint8_t*, BufInfo> g_bufInfo;            // guest photo buffers filled by a GPU copy: was the source shiny
struct LastCopy { bool valid = false; const uint8_t* dst = nullptr; int frame = -1000000; BufInfo info{false, ""}; };
LastCopy g_lastCopy;
std::set<uint64_t> g_decSig;                            // signatures (FNV of the first 4 KB) of decoded shiny photos: the game copies the decode buffer to another address before the GPU upload
std::map<const uint8_t*, size_t> g_decShiny;             // guest buffers holding a decoded shiny photo (start -> bytes); textures made from them are flagged
std::set<const uint8_t*> g_painted;                     // decoded album buffers that already carry the badge (host/mods/shiny.cpp)
bool Decide(int shot, std::string* best, double* px) {
  double bp = 0; std::string b;
  for (int f = shot - 1; f <= shot + 1; f++) { auto it = g_cov.find(f); if (it == g_cov.end()) continue; for (auto& kv : it->second) if (kv.second > bp) { bp = kv.second; b = kv.first; } }
  *best = b; *px = bp; return bp >= MinPx();
}
void FxOnce(const std::string& sp) {
  static std::atomic<int> lastFx{-100000};
  if (g_presentCount - lastFx.load() > 150) { lastFx.store(g_presentCount); snap_shiny_fx_trigger(sp.c_str()); }
}
void PollLocked() {   // g_pm held
  Host* h = g_host;
  if (h && g_sqPool && !g_pendQ.empty()) {
    for (size_t i = 0; i < g_pendQ.size();) {
      uint64_t r[2] = {0, 0};
      const VkResult vr = vkGetQueryPoolResults(h->device, g_sqPool, g_pendQ[i].qi, 1, sizeof r, r, sizeof r, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if ((vr == VK_SUCCESS || vr == VK_NOT_READY) && r[1]) {
        g_cov[g_pendQ[i].frame][g_pendQ[i].sp] += (double)r[0] * (1080.0 * 720.0) / (double)std::max(g_pendQ[i].area, 1);
        g_pendQ.erase(g_pendQ.begin() + i);
      } else i++;
    }
  }
  while (g_cov.size() > 12) g_cov.erase(g_cov.begin());
  for (size_t i = 0; i < g_pendThumb.size();) {
    PendThumb pt = g_pendThumb[i];
    if (g_presentCount < pt.shot + 3) { i++; continue; }
    std::string best; double bestPx = 0; Decide(pt.shot, &best, &bestPx);
    g_pendThumb.erase(g_pendThumb.begin() + i);
    const bool shiny = bestPx >= MinPx();
    if (shiny) { g_thumbs.insert(pt.dst); g_thumbSp[pt.dst] = best; FxOnce(best); } else { g_thumbs.erase(pt.dst); g_thumbSp.erase(pt.dst); }
    static int n = 0; if (n++ < 400) fprintf(stderr, "[shiny] thumbnail %p shot at present %d: %s (%s %.0f px at 1080x720)\n", pt.dst, pt.shot, shiny ? "SHINY subject" : "no shiny subject", best.empty() ? "-" : best.c_str(), bestPx);
  }
  for (size_t i = 0; i < g_pendRt.size();) {
    const PendRt pr = g_pendRt[i];
    if (g_presentCount < pr.present + 3) { i++; continue; }
    std::string best; double px = 0; const bool shiny = Decide(pr.present, &best, &px);
    g_pendRt.erase(g_pendRt.begin() + i);
    g_known.insert(pr.rt);
    if (shiny) { g_rtShiny[pr.rt] = g_presentCount; g_rtSp[pr.rt] = best; } else { g_rtShiny.erase(pr.rt); g_rtSp.erase(pr.rt); }
    for (const void* fd : pr.follow) { g_known.insert(fd); if (shiny) { g_rtShiny[fd] = g_presentCount; g_rtSp[fd] = best; } else { g_rtShiny.erase(fd); g_rtSp.erase(fd); } }
    static int n = 0; if (n++ < 60) fprintf(stderr, "[shiny] capture target %p shot at present %d: %s (%s %.0f px)\n", pr.rt, pr.present, shiny ? "SHINY subject" : "no shiny subject", best.empty() ? "-" : best.c_str(), px);
  }
}
bool Flagged(const void* t) { std::lock_guard<std::mutex> g(g_pm); return g_thumbs.count(t) || g_rtShiny.count(t); }
void NoteShiny(uint64_t key, const std::string& sp) {
  g_lastFrame.store(g_presentCount, std::memory_order_relaxed);
  if (g_lastKey.exchange(key) != key) { std::lock_guard<std::mutex> g(g_spMu); g_lastSpecies = sp; }
}

// Per-instance roll (M2). An instance is identified by its first vertex buffer address; an address not drawn for 600 presents
// is a new instance (arena addresses get reused), which takes the next ordinal of its species. The roll is a hash of
// (seed, species key, ordinal): same seed and same spawn order give the same shinies. Never touches the game's own RNG.
// Instances: a Pokemon draws several meshes (body, heads) from separate vertex buffers, so the roll belongs to a cluster of
// buffers that the game created together (same thread, consecutive buffer sequence numbers, within one or two presents),
// not to one vb. BufferFinalize (nvn_gfx.cpp) ends a buffer's life: its vb leaves its cluster and an empty cluster is gone, so
// an address reused by a later spawn is rolled anew and one still alive keeps its color for as long as it lives.
struct BufRec { uint64_t size = 0; uint64_t seq = 0; unsigned tid = 0; int present = 0; };
struct Cluster { bool shiny = false; int refs = 0; uint64_t key = 0; };
std::mutex g_bufMu;
std::map<uint64_t, BufRec> g_bufs;                 // buffer address -> creation record
std::unordered_map<uint64_t, Cluster> g_clusters;   // instance buffer address -> roll
std::unordered_map<uint64_t, unsigned> g_ordinal;

uint64_t g_bufSeq = 0;
uint64_t Seed() {
  static const uint64_t s = [] {
    if (const char* e = getenv("SNAP_SHINY_SEED")) if (*e) return (uint64_t)strtoull(e, nullptr, 0) * 0x9E3779B97F4A7C15ull + 1;
    // profile seed: random once, kept beside the pack (host side, never in the game save); the launch counter makes each launch roll anew
    const std::string f = C().dir + "\\profile_seed.txt";
    unsigned long long seed = 0, launch = 0;
    if (FILE* fp = fopen(f.c_str(), "r")) { if (fscanf(fp, "%llx %llu", &seed, &launch) != 2) seed = launch = 0; fclose(fp); }
    if (!seed) seed = (nvn::os_host::monotonic_ms() << 20) ^ nvn::os_host::current_process_id() ^ 0xA5F152C3D0B7ull;
    if (FILE* fp = fopen(f.c_str(), "w")) { fprintf(fp, "%llx %llu\n", seed, launch + 1); fclose(fp); }
    return seed ^ (launch * 0x9E3779B97F4A7C15ull);
  }();
  return s;
}
bool RollInstance(uint64_t key, const ShinyDraw& d, const std::string& species) {
  (void)key;
  const uint64_t vb0 = d.vb0;
  std::lock_guard<std::mutex> g(g_mu);
  std::lock_guard<std::mutex> gb(g_bufMu);
  const int now = g_presentCount;
  // The instance is the buffer that holds this vertex buffer: the game creates ONE buffer per spawned model (trace 3-20261114_024944:
  // every mesh of a Dodrio, body and heads, lies inside one buffer; the species' textures differ per mesh, the buffer does not).
  uint64_t cid = vb0; const BufRec* me = nullptr;
  auto it = g_bufs.upper_bound(vb0);
  if (it != g_bufs.begin()) { --it; if (vb0 < it->first + it->second.size) { cid = it->first; me = &it->second; } }
  auto f = g_clusters.find(cid);
  if (f == g_clusters.end()) {
    const uint64_t sk = Fnv(reinterpret_cast<const uint8_t*>(species.data()), species.size());
    const unsigned ord = g_ordinal[sk]++;
    uint64_t h = Seed() ^ sk; h ^= ord * 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 29;
    Cluster cl; cl.shiny = (h % C().odds) == 0; cl.refs = 1; cl.key = sk;
    f = g_clusters.emplace(cid, cl).first;
    fprintf(stderr, "[shiny] roll: %s ordinal %u buffer %llx (size %llx) first mesh vb %llx at present %d: %s (1 in %u)\n", species.c_str(), ord, (unsigned long long)cid,
            me ? (unsigned long long)me->size : 0ull, (unsigned long long)vb0, now, cl.shiny ? "SHINY" : "normal", C().odds);
  }
  const bool shiny = f->second.shiny;
  {   // diagnostics: every new (vertex buffer, render target size, program) combination that draws this instance
    static std::set<std::tuple<uint64_t, int, int, int>> ctx; static int n = 0;
    if (ctx.insert({vb0, d.rtW, d.rtH, d.prog}).second && n++ < 400)
      fprintf(stderr, "[shiny] ctx: %s vb %llx buffer %llx rt %dx%d prog %d at present %d (%s)\n", species.c_str(), (unsigned long long)vb0, (unsigned long long)cid, d.rtW, d.rtH, d.prog, now, shiny ? "shiny" : "normal");
  }
  return shiny;
}
}  // namespace

bool ShinyActive() { return C().on || C().trace; }

void ShinyBufferInit(uint64_t va, uint64_t size) {
  std::lock_guard<std::mutex> g(g_bufMu);
  BufRec r; r.size = size; r.seq = ++g_bufSeq; r.tid = (uint32_t)nvn::os_host::current_thread_id(); r.present = g_presentCount;
  g_bufs[va] = r;
}
void ShinyBufferFinalize(uint64_t va, uint64_t size) {
  (void)size;
  std::lock_guard<std::mutex> g(g_mu);
  std::lock_guard<std::mutex> gb(g_bufMu);
  g_bufs.erase(va);
  if (g_clusters.erase(va)) { static int n = 0; if (n++ < 60) fprintf(stderr, "[shiny] instance buffer %llx ended at present %d\n", (unsigned long long)va, g_presentCount); }
}

TextureImpl* ShinySubstitute(TextureImpl* t, const ShinyDraw& d) {
  const Cfg& c = C();
  if (d.rtW == 1920 && d.vbf) {   // a full-screen sprite (dim veil behind a popup, fade) covers every badge collected earlier in this frame
    float vx0 = 1e9f, vy0 = 1e9f, vx1 = -1e9f, vy1 = -1e9f;
    for (int i = 0; i < 4; i++) { const float x = d.vbf[14 * i], y = d.vbf[14 * i + 1]; vx0 = std::min(vx0, x); vx1 = std::max(vx1, x); vy0 = std::min(vy0, y); vy1 = std::max(vy1, y); }
    if (vx1 - vx0 >= 1800.0f && vy1 - vy0 >= 1000.0f && !(t->b.width == 960 && t->b.height == 540)) {   // the 960x540 full-screen layer is part of normal screens (Score, photo select), not a dim veil
      std::lock_guard<std::mutex> g(g_pm);
      for (size_t i = 0; i < g_badges.size();) { if (g_badges[i].frame == g_presentCount) g_badges.erase(g_badges.begin() + i); else i++; }
    }
  }
  bool selBig = false;   // photo select's large preview (a 360x240 thumbnail drawn wide) names the selected photo; the Score polaroid shows it
  if (d.rtW == 1920 && d.vbf && t->b.width == 360 && t->b.height == 240) {
    const float w = std::fabs(d.vbf[14 * 2] - d.vbf[0]), w2 = std::fabs(d.vbf[14 * 1] - d.vbf[0]), ww = std::max(w, w2);
    if (ww > 600) { std::lock_guard<std::mutex> g(g_pm); g_selFlag = g_thumbs.count(t) != 0; g_selFrame = g_presentCount; selBig = true; }
  }
  bool flaggedDraw = Flagged(t);
  if (d.rtW == 1920 && t->b.width == 1080 && t->b.height == 720) { std::lock_guard<std::mutex> g(g_pm); if (!g_known.count(t)) flaggedDraw = false; }   // a photo texture is badged only by its own lineage (capture target, copy, buffer, upload), never by what was last shown; a texture with no lineage (the album save screen's copy) gets no overlay badge
  (void)selBig;
  if (d.rtW == 1920 && d.vbf && (t->b.width == 360 || t->b.width == 1080 || t->b.width == 204 || t->b.width == 240) && flaggedDraw) {   // a flagged photo is being drawn as a sprite
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < 4; i++) { const float x = d.vbf[14 * i], y = d.vbf[14 * i + 1]; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    bool clipped = false;   // the game clips scrolling strips (photo select / album film strip) with a scissor: a badge whose corner is outside it is not on screen
    if (d.scValid) { const float bx = x1 - 20.0f, by = y0 + 20.0f; clipped = bx < d.sc[0] || bx > d.sc[0] + d.sc[2] || by < d.sc[1] || by > d.sc[1] + d.sc[3]; }
    const float rw = x1 - x0, rh = y1 - y0, asp = rh > 0 ? rw / rh : 0;   // photo sprites are 3:2 and at least thumbnail sized; film-strip holes and filler quads sample the same texture
    if (!clipped && rw > 90 && rh > 60 && asp > 1.25f && asp < 1.9f && rw < 1900 && rh < 1060) {
      std::lock_guard<std::mutex> g(g_pm);
      for (auto& b : g_badges) if (b.frame >= g_presentCount - 1 && std::fabs(b.r[0] - x0) < 1 && std::fabs(b.r[1] - y0) < 1 && std::fabs(b.r[2] - x1) < 1) { b.frame = g_presentCount; goto have; }
      if (g_badges.size() > 256) g_badges.erase(g_badges.begin(), g_badges.begin() + 128);
      g_badges.push_back({{x0, y0, x1, y1}, g_presentCount, t->b.width == 1080});
      have:;
    }
  }
  { static const bool uiTrace = getenv("SNAP_SHINY_UITRACE") != nullptr;   // debug: which textures UI draws sample (photo thumbnails)
    if (uiTrace && ((t->b.width == 360 && t->b.height == 240) || (t->b.width == 1080 && t->b.height == 720))) {
      static std::set<std::tuple<void*, int, int, int>> seen; static int n = 0;
      std::lock_guard<std::mutex> g(g_mu);
      if (seen.insert({(void*)t, d.prog, d.rtW, g_presentCount / 300}).second && n++ < 700) {
        fprintf(stderr, "[shuitrace] p%d tex=%p %dx%d f%x flags=%x pool=%p off=%llx up=%d rt=%dx%d prog=%d", g_presentCount, (void*)t, t->b.width, t->b.height, t->b.format, t->b.flags, t->b.pool, (long long)t->b.poolOffset, (int)t->uploaded, d.rtW, d.rtH, d.prog);
        if (d.vbf) { fprintf(stderr, " | vb:"); for (int k = 0; k < 24; k++) fprintf(stderr, " %g", d.vbf[k]); }
        for (int i = 0; i < 4; i++) if (auto* p = static_cast<const PoolImpl*>(d.uboPool[i])) if (p->storage && d.uboRange[i] >= 16) {
          const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(p->storage) + d.uboOff[i]);
          fprintf(stderr, " | u%d(%llu):", i, (unsigned long long)d.uboRange[i]); for (int k = 0; k < 40 && (uint64_t)k * 4 < d.uboRange[i]; k++) fprintf(stderr, " %g", f[k]);
        }
        fprintf(stderr, "\n");
      }
    } }
  if (!t->shTried && !t->uploaded) {
    t->shTried = true;
    const TextureBuilder& b = t->b;
    const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
    if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR)))
      if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
    if (src && t->scale == 1.0f && b.samples <= 1 && b.target == TT_2D && b.width >= 64 && b.height >= 64) {
      t->shKey = Fnv(src, 4096);
      if (c.traceAll) {
        static int n = 0;
        if (n++ < 4000) fprintf(stderr, "[shiny] tex key %016" PRIx64 " %dx%d f%x lv%d tex %p\n", t->shKey, b.width, b.height, b.format, b.levels, (void*)t);
      }
    }
  }
  if (!t->shKey) return t;
  const uint64_t key = t->shKey;
  if (c.trace && !c.traceAll && key == c.traceKey) {
    static std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>> seen; static int n = 0;
    uint64_t us = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) { us = (us ^ d.uboOff[i]) * 1099511628211ull; us = (us ^ d.uboRange[i]) * 1099511628211ull; }
    std::lock_guard<std::mutex> g(g_mu);
    if (seen.insert({d.vb0, d.vb1, us, (uint64_t)(uintptr_t)t}).second && n++ < 600) {
      fprintf(stderr, "[shiny-trace] f=%d key=%016" PRIx64 " tex=%p vb0=%llx vb1=%llx\n", g_presentCount, key, (void*)t, (unsigned long long)d.vb0, (unsigned long long)d.vb1);
    }
  }
  if (!c.on) return t;
  auto it = c.table.find(key);
  if (it == c.table.end()) return t;
  const bool shiny = !c.only.empty() ? it->second.species.find(c.only) != std::string::npos : (c.forceAll || (c.force && key == c.force) || c.odds == 1 || RollInstance(key, d, it->second.species));
  if (!shiny) return t;
  if (t->shAlt) { NoteShiny(key, it->second.species); NoteRt(d.rt0, d.rtW, d.rtH); TagTwin(it->second.species, d); return t->shAlt; }
  std::lock_guard<std::mutex> g(g_mu);
  if (t->shAlt) return t->shAlt;
  TwinFile& tw = const_cast<TwinFile&>(it->second);
  if (!tw.tried) {
    tw.tried = true;
    if (FILE* f = fopen((c.dir + "\\" + tw.file).c_str(), "rb")) {
      fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
      auto* buf = new uint8_t[(size_t)n];   // never freed: the twin image is built from it on first use
      if (fread(buf, 1, (size_t)n, f) == (size_t)n) tw.data = buf;
      fclose(f);
    }
    fprintf(stderr, "[shiny] twin for key %016" PRIx64 " (%s) %s\n", key, tw.species.c_str(), tw.data ? "loaded" : "MISSING");
  }
  const TextureBuilder& b = t->b;
  if (!tw.data || tw.w != (uint32_t)b.width || tw.h != (uint32_t)b.height || tw.mips != (uint32_t)b.levels) return t;
  auto* s = new TextureImpl();
  s->host = t->host; s->fi = t->fi; s->b = b;
  s->b.pool = nullptr; s->b.poolOffset = 0; s->b.packaged = tw.data;
  s->shTried = true;
  t->shAlt = s;
  NoteShiny(key, tw.species); NoteRt(d.rt0, d.rtW, d.rtH); TagTwin(tw.species, d);
  return s;
}
}  // namespace nvn

// Present index of the last shiny draw (-1 = none yet) and its species name (copied into out).
extern "C" int snap_shiny_last(char* out, int n) {
  if (out && n > 0) { std::lock_guard<std::mutex> g(nvn::g_spMu); snprintf(out, (size_t)n, "%s", nvn::g_lastSpecies.c_str()); }
  return nvn::g_lastFrame.load(std::memory_order_relaxed);
}

// 1 when a shiny twin was drawn within the last `frames` presents (species name copied into out), else 0 (host/mods/shiny.cpp).
extern "C" int snap_shiny_recent(int frames, char* out, int n) {
  const int last = nvn::g_lastFrame.load(std::memory_order_relaxed);
  if (last < 0 || nvn::g_presentCount - last > frames) return 0;
  if (out && n > 0) { std::lock_guard<std::mutex> g(nvn::g_spMu); snprintf(out, (size_t)n, "%s", nvn::g_lastSpecies.c_str()); }
  return 1;
}

namespace nvn {
void ShinyThumbCapture(TextureImpl* dst) {
  g_lastShutter.store(g_presentCount, std::memory_order_relaxed);   // the 360x240 thumbnail pass of a shutter: decide from the shiny draws' pixel coverage
  std::lock_guard<std::mutex> g(g_pm);
  g_pendThumb.push_back({dst, g_presentCount});
  PollLocked();
}
uint32_t ShinyQueryBegin(Host* h, VkCommandBuffer cb) {
  if (tl_twinSp.empty() || !h->hostQueryReset) { tl_twinSp.clear(); return UINT32_MAX; }
  if (!g_sqPool) { VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = kSq; if (vkCreateQueryPool(h->device, &qi, nullptr, &g_sqPool) != VK_SUCCESS) { g_sqPool = VK_NULL_HANDLE; tl_twinSp.clear(); return UINT32_MAX; } }
  std::lock_guard<std::mutex> g(g_pm);
  g_host = h;
  if (g_pendQ.size() > kSq - 64) g_pendQ.erase(g_pendQ.begin(), g_pendQ.begin() + 256);
  const uint32_t qi = g_sqNext++ % kSq;
  vkResetQueryPool(h->device, g_sqPool, qi, 1);
  vkCmdBeginQuery(cb, g_sqPool, qi, h->features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
  g_pendQ.push_back({qi, g_presentCount, tl_twinSp, tl_twinArea});
  tl_twinSp.clear();
  return qi;
}
void ShinyQueryDiscard() { tl_twinSp.clear(); }
void ShinyQueryEnd(VkCommandBuffer cb, uint32_t qi) { if (qi != UINT32_MAX) vkCmdEndQuery(cb, g_sqPool, qi); }
void ShinyCopyTex(const TextureImpl* s, const TextureImpl* d) {   // a copy takes the flag of its source (the album save screen's photo is a copy of the capture target)
  const bool f = Flagged(s);
  std::lock_guard<std::mutex> g(g_pm);
  g_rtShiny.erase(d); g_rtSp.erase(d); g_known.erase(d);
  bool pend = false;
  for (size_t i = g_pendRt.size(); i-- > 0;) if (g_pendRt[i].rt == s) { g_pendRt[i].follow.push_back(d); pend = true; break; }   // the source's coverage decision is not in yet: the copy follows it
  if (pend) return;
  if (!g_known.count(s) && !f) return;   // a source with no lineage gives no lineage
  g_known.insert(d);
  if (f) { g_rtShiny[d] = g_presentCount; auto it = g_rtSp.find(s); if (it != g_rtSp.end()) g_rtSp[d] = it->second; }
}
void ShinyRtClear(const TextureImpl* rt) { std::lock_guard<std::mutex> g(g_pm); g_rtShiny.erase(rt); }
void ShinyTexGone(const TextureImpl* t) { std::lock_guard<std::mutex> g(g_pm); g_thumbs.erase(t); g_rtShiny.erase(t); }
void ShinyCaptureDraw(const TextureImpl* rt) {   // a capture pass into a 1080x720 target: decide its flag from the shutter's coverage
  if (rt->b.width != 1080 || rt->b.height != 720) return;
  std::lock_guard<std::mutex> g(g_pm);
  for (auto& p : g_pendRt) if (p.rt == rt && g_presentCount - p.present <= 2) return;
  g_pendRt.push_back({rt, g_presentCount});
}
void ShinyCopySrc(const TextureImpl* t, const uint8_t* dst) {   // a GPU->guest copy of a photo-sized image (the album photo before JpegEncoder)
  if (t->b.width < 640) return;
  { std::lock_guard<std::mutex> g(g_pm); PollLocked(); }
  const bool f = Flagged(t);
  std::string sp;
  { std::lock_guard<std::mutex> g(g_pm);
    auto it = g_rtSp.find(t);
    if (it != g_rtSp.end()) sp = it->second; else { std::lock_guard<std::mutex> g2(g_spMu); sp = g_lastSpecies; }
    if (t->b.width == 1080) { g_lastCopy.valid = true; g_lastCopy.dst = dst; g_lastCopy.frame = g_presentCount; g_lastCopy.info = {f, sp}; }
    if (dst) { g_bufInfo[dst] = {f, sp}; if (g_bufInfo.size() > 256) g_bufInfo.erase(g_bufInfo.begin()); g_painted.erase(dst); } }
  static int n = 0; if (n++ < 60) fprintf(stderr, "[shiny] photo copy from %p %dx%d to buffer %p at present %d: %s (selected-photo flag %d, %d presents ago)\n", (const void*)t, t->b.width, t->b.height, (const void*)dst, g_presentCount, f ? "flagged SHINY" : "not flagged", (int)g_selFlag, g_presentCount - g_selFrame);
  g_copyFrame.store(f ? g_presentCount : -1, std::memory_order_relaxed);
}
void ShinyBufUpload(const TextureImpl* t, const uint8_t* src) {   // CopyBufferToTexture: a texture made from a photo buffer shows that photo
  if (!src) return;
  { std::lock_guard<std::mutex> g(g_pm);
    bool sigHit = false;
    if (!g_decSig.empty() && t->b.width >= 100 && t->b.height >= 100) sigHit = g_decSig.count(Fnv(src, 4096)) != 0;
    auto up = g_decShiny.upper_bound(src);
    if (sigHit) { up = g_decShiny.begin(); }
    if (sigHit || (up != g_decShiny.begin() && (--up, src >= up->first && src < up->first + up->second))) { {   // a decoded shiny photo (album, Your Info, photo editor): flag the texture, never the pixels
      g_known.insert(t); g_rtShiny[t] = g_presentCount; g_rtSp[t] = "shiny";
      static int nd = 0; if (nd++ < 40) fprintf(stderr, "[shiny] texture %p %dx%d made from a decoded shiny photo at present %d\n", (const void*)t, t->b.width, t->b.height, g_presentCount);
      return; } } }
  if (t->b.width < 300) return;
  std::lock_guard<std::mutex> g(g_pm);
  auto it = g_bufInfo.find(src);
  if (g_painted.count(src)) { g_rtShiny.erase(t); g_known.insert(t); return; }   // the badge is in the pixels already
  if (it == g_bufInfo.end()) { g_rtShiny.erase(t); return; }   // new pixels from an untracked buffer: whatever flag the texture had is stale
  g_known.insert(t);
  if (it->second.shiny) { g_rtShiny[t] = g_presentCount; g_rtSp[t] = it->second.sp; } else g_rtShiny.erase(t);
  static int n = 0; if (n++ < 40) fprintf(stderr, "[shiny] texture %p %dx%d made from photo buffer %p: %s\n", (const void*)t, t->b.width, t->b.height, (const void*)src, it->second.shiny ? "SHINY" : "normal");
}
}  // namespace nvn

extern "C" int snap_shiny_badge_rects(float* out, int max) {
  std::lock_guard<std::mutex> g(nvn::g_pm);
  nvn::PollLocked();
  int k = 0;
  for (auto& b : nvn::g_badges) if (b.frame >= nvn::g_presentCount - 2 && k < max) { for (int i = 0; i < 4; i++) out[k * 4 + i] = b.r[i]; k++; }
  return k;
}
extern "C" int snap_shiny_badge_rects5(float* out, int max) {   // 5 floats per badge: rect and 1 when the sprite shows the 1080x720 photo (Score)
  std::lock_guard<std::mutex> g(nvn::g_pm);
  nvn::PollLocked();
  int k = 0;
  for (auto& b : nvn::g_badges) if (b.frame >= nvn::g_presentCount - 2 && k < max) { for (int i = 0; i < 4; i++) out[k * 5 + i] = b.r[i]; out[k * 5 + 4] = b.big ? 1.0f : 0.0f; k++; }
  return k;
}
extern "C" int snap_shiny_since_shutter() { return nvn::g_presentCount - nvn::g_lastShutter.load(std::memory_order_relaxed); }   // presents since the last shutter thumbnail
extern "C" int snap_shiny_copy_flag(int frames, char* out, int n) {
  const int f = nvn::g_copyFrame.load(std::memory_order_relaxed);
  if (f < 0 || nvn::g_presentCount - f > frames) return 0;
  if (out && n > 0) { std::lock_guard<std::mutex> g(nvn::g_spMu); snprintf(out, (size_t)n, "%s", nvn::g_lastSpecies.c_str()); }
  return 1;
}

extern "C" int snap_shiny_buf_flag(const uint8_t* pix, char* out, int n) {   // host/mods/shiny.cpp: was this photo buffer copied from a shiny capture
  std::lock_guard<std::mutex> g(nvn::g_pm);
  // exact buffer, else a buffer that contains the pointer (4 bytes per pixel, 1080x720), else the last photo copy if it is recent and unconsumed
  const nvn::BufInfo* bi = nullptr; const char* how = "exact";
  auto it = nvn::g_bufInfo.find(pix);
  if (nvn::g_lastCopy.valid && nvn::g_presentCount - nvn::g_lastCopy.frame <= 12) { bi = &nvn::g_lastCopy.info; how = "last copy"; }   // the copy just made is the photo being encoded, even when the game moved it to another buffer first (an older copy at the encode address is stale)
  else if (it != nvn::g_bufInfo.end()) bi = &it->second;
  else {
    auto up = nvn::g_bufInfo.upper_bound(pix);
    if (up != nvn::g_bufInfo.begin()) { --up; if (pix >= up->first && pix < up->first + 1080 * 720 * 4) { bi = &up->second; how = "inside"; } }
  }
  if (!bi && nvn::g_lastCopy.valid && nvn::g_presentCount - nvn::g_lastCopy.frame < 900) { bi = &nvn::g_lastCopy.info; how = "last copy"; }
  static int nlog = 0; if (nlog++ < 40) fprintf(stderr, "[shiny] encode buffer %p: %s (%s), last copy %p flagged %d at present %d (now %d)\n", (const void*)pix, bi ? "matched" : "no match", bi ? how : "-", (const void*)nvn::g_lastCopy.dst, (int)nvn::g_lastCopy.info.shiny, nvn::g_lastCopy.frame, nvn::g_presentCount);
  if (bi && how[0] == 'l') nvn::g_lastCopy.valid = false;
  if (!bi || !bi->shiny) return 0;
  if (out && n > 0) snprintf(out, (size_t)n, "%s", bi->sp.c_str());
  return 1;
}
extern "C" void snap_shiny_buf_decoded(const uint8_t* buf, int w, int h) {   // host/mods/shiny.cpp h_decode
  std::lock_guard<std::mutex> g(nvn::g_pm);
  if (w <= 0 || h <= 0) { auto it = nvn::g_decShiny.find(buf); if (it != nvn::g_decShiny.end()) nvn::g_decShiny.erase(it); return; }
  nvn::g_decShiny[buf] = (size_t)w * (size_t)h * 4;
  if ((size_t)w * (size_t)h * 4 >= 4096) { nvn::g_decSig.insert(nvn::Fnv(buf, 4096)); if (nvn::g_decSig.size() > 256) nvn::g_decSig.erase(nvn::g_decSig.begin()); }
  if (nvn::g_decShiny.size() > 256) nvn::g_decShiny.erase(nvn::g_decShiny.begin());
}
extern "C" void snap_shiny_buf_painted(const uint8_t* buf) { std::lock_guard<std::mutex> g(nvn::g_pm); nvn::g_painted.insert(buf); if (nvn::g_painted.size() > 256) nvn::g_painted.erase(nvn::g_painted.begin()); nvn::g_bufInfo.erase(buf); }
