// SPDX-License-Identifier: MIT
// Persistent + asynchronous graphics-pipeline compilation for the NVN layer.
//
//  * Every pipeline the game creates is identified by a PipeDesc (stage cache keys + state) and recorded in an append-only key list
//    (<shadercache>\pipeline_keys.bin). The VkPipelineCache is saved next to it every 30 s while new pipelines appear, so killing the
//    process loses at most 30 s (it used to be written only on a clean device teardown, which never happens when the window closes).
//  * At startup the recorded keys are compiled on a worker pool (shaderprecompile_begin / mode NOW or PLAYING) and stored in the ready map,
//    so the first draw that needs them finds a finished VkPipeline.
//  * A pipeline the game needs that is not ready compiles on the pool (hot queue first); the draw is skipped until it is ready
//    (SNAP_PIPELINE_ASYNC=0 compiles inline on the replay thread instead, the old behaviour).
//  * Files are versioned by game build, translator version, descriptor layout and (cache blob only) driver identity; a mismatch discards them.
//    <shadercache>\cache_stamp.txt makes that a wipe: a changed GPU, device ID or driver version deletes the driver blob, a changed build,
//    translator or key layout deletes the key list as well. <shadercache>\status.json (key and ready counts) is written for the launcher chip.
#include "nvn_os_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "hle/shutter_hook.h"
#include "nvn_pipeline.h"
#include "os/perf.h"
#include "shaderprecompile.h"

namespace nvn {

namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kMagicKeys = 0x4B504E53;    // 'SNPK'
constexpr uint32_t kMagicCache = 0x43504E53;   // 'SNPC'

struct FileHdr {
  uint32_t magic, version, descSize, pad;
  uint64_t translator, gameBuild;
  uint32_t vendor, device, driver, pad2;
  uint8_t uuid[16];
};

uint64_t Fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
  return h;
}
// In-memory map key only (never written to a file; entries compare the whole desc). Word-wise (opt 2026-11-13): the byte-wise
// FNV over 464 bytes ran once per draw in PipelineAcquire, the hottest loop of the replay thread in a PGO profile.
uint64_t HashDesc(const PipeDesc& pd) {
  static_assert(sizeof(PipeDesc) % 8 == 0, "word hash");
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&pd);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < sizeof pd; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 31; }
  return h;
}

enum EntState { ES_PENDING = 0, ES_READY = 1, ES_FAILED = 2, ES_NEEDS_INLINE = 3 };
struct Ent {
  PipeDesc pd;
  std::atomic<VkPipeline> pipe{VK_NULL_HANDLE};   // atomic: a stale rebuild on the replay thread can race a worker's StoreResult for the same entry
  std::atomic<int> state{ES_PENDING};
  Ent* next = nullptr;   // hash chain
  std::atomic<bool> hot{false};   // a hot-queue job exists (set when the replay thread first asks for an entry that BeginRun queued cold)
  std::atomic<bool> stale{false};  // built from a fragment template that lacked early fragment tests: rebuilt inline on the next acquire
};

struct Tmpl {
  std::once_flag once;
  StageProg sp;      // as loaded from the translation cache; never modified after the load
  // What readers (background compiles, GetLayoutFor) use. PatchTmplEft publishes a patched COPY here instead of editing \sp\ in place: a worker
  // reading mod / spv / fragMods through the old pointer keeps a valid, unchanging object (the old one is never freed while the service runs).
  std::atomic<StageProg*> cur{nullptr};
  bool ok = false;
  std::mutex em;     // guards eft and the module swap in PatchTmplEft
  bool eft = false;  // the module carries layout(early_fragment_tests)
};

struct Job { PipeDesc pd; bool hot; };

struct Svc {
  Host* h = nullptr;
  bool up = false;
  std::string dir;
  uint64_t translatorHash = 0, buildHash = 0;
  bool async = true;
  std::atomic<int> mode{SHADERPRECOMPILE_MODE_UNSET};
  bool warm = false;   // the stamp matched and the driver cache loaded: the start-up replay is cache hits only, so it never holds the game

  // ready map
  std::mutex pm;
  std::unordered_map<uint64_t, Ent*> map;
  // stage templates, layouts
  std::mutex tm;
  std::unordered_map<std::string, Tmpl*> tmpls;
  std::unordered_map<std::string, std::string> fullKeys;   // key prefix -> full translation cache key
  std::unordered_set<std::string> eftKeys;                 // key prefixes whose program asks for early fragment tests (guarded by tm)
  std::mutex lm;
  std::unordered_map<std::string, ProgramImpl*> layouts;
  // job queues and workers
  std::mutex qm;
  std::condition_variable qcv;
  std::deque<Job> hot, cold;
  std::vector<std::thread> workers;
  std::atomic<int> activeLimit{2};
  int running = 0;
  bool stop = false;
  // key list
  std::mutex km;
  FILE* keysFile = nullptr;
  std::vector<PipeDesc> known;
  std::unordered_set<uint64_t> recorded;   // HashDesc of every key already in the user's list (guarded by km): a request for one is never appended again
  // progress of the current precompile run
  std::atomic<int> state{SHADERPRECOMPILE_IDLE};
  std::atomic<uint32_t> total{0}, done{0}, failed{0};
  std::atomic<uint64_t> runStartNs{0}, busyNs{0};
  std::atomic<bool> cancel{false};
  std::atomic<uint32_t> bgPending{0};
  // cache persistence
  std::atomic<bool> dirty{false};
  std::atomic<uint64_t> lastSaveNs{0};
  std::thread housekeeper;
  double avgMs = 25.0;   // measured average compile time of a precompile run (persisted in stats.txt)
};
Svc* S = nullptr;
std::mutex g_early;                // guards the early requests below
bool g_wantBegin = false;
int g_earlyMode = -1;
std::string g_buildId = "DDB900FF04A912ED42430508389853CFAF1ACCCE";

std::string Dir() {
  const char* e = getenv("SNAP_SHADERCACHE_DIR");
  const std::string sep(1, nvn::os_host::path_sep());
  std::string d;
  if (e && *e) d = e;
  else if ((e = getenv("SNAP_CONFIG_DIR")) && *e) d = std::string(e) + sep + "shadercache";
  else d = nvn::os_host::cache_dir() + sep + "shadercache";
  if (const char* r = getenv("SNAP_XLATE_REV"); r && *r && strcmp(r, "0")) d += std::string("_r") + r;   // translator revision: its own pipeline keys and VkPipelineCache
  if (const char* sm = getenv("SNAP_SAFE_MODE"); sm && sm[0] == '1') d += "_safe";   // safe mode: a separate cache folder, so the real one is neither read nor changed (docs/safe-mode.md)
  nvn::os_host::ensure_dir(d);
  return d;
}

FileHdr MakeHdr(Host* h, uint32_t magic, bool withDriver) {
  FileHdr x{};
  x.magic = magic; x.version = kFormatVersion; x.descSize = (uint32_t)sizeof(PipeDesc);
  x.translator = S->translatorHash; x.gameBuild = S->buildHash;
  if (withDriver) {
    x.vendor = h->props.vendorID; x.device = h->props.deviceID; x.driver = h->props.driverVersion;
    memcpy(x.uuid, h->props.pipelineCacheUUID, 16);
  }
  return x;
}
bool HdrMatches(const FileHdr& a, const FileHdr& b) { return memcmp(&a, &b, sizeof a) == 0; }

double NowMs() { return (double)snap_perf::now_ns() / 1e6; }

// ---- templates and layouts ------------------------------------------------------------------------------------------------------
// Early fragment tests on background templates. BuildStage adds layout(early_fragment_tests) when the program's control section asks for
// it (EvaluationAggregatePS) and leaves a <key>.efz marker so cache loads can do the same. Test runs mirror the shader cache from
// build\snap\nvn_cache, which has no markers, so the startup precompile could load the Aggregate template before the game rebuilt the
// marker: its pipeline then ran the stencil test after the shader's atomics and every visibility counter read 518400 (all of 1920x1080 / 4),
// so no photo passed. Now BuildStage also tells the service directly (eftKeys); a template loaded too early is patched and the pipelines
// built from it are rebuilt inline. NVN_NO_EFT_FIX=1 restores the old marker-only behavior.
bool EftFixOn() { static const bool on = getenv("NVN_NO_EFT_FIX") == nullptr; return on; }

void PatchTmplEft(Tmpl* t, const char* key12) {
  {
    std::lock_guard<std::mutex> lk(t->em);
    StageProg* base = t->cur.load(std::memory_order_acquire);
    if (t->eft || !t->ok || !base || base->stage != 1 || base->spv.empty()) return;
    std::vector<uint32_t> words = base->spv;
    if (!SpvAddEarlyFragmentTests(words)) return;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4; ci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(S->h->device, &ci, nullptr, &mod) != VK_SUCCESS) return;
    // Copy without fragMods (not "*np = *base": copying base->fragMods would race a worker inserting a variant). fragMods starts empty: the
    // old variants were built from the module without early fragment tests.
    StageProg* np = new StageProg;
    CopyStageMeta(*np, *base);
    np->mod = mod;   // the old module is left alive on purpose: a worker may be compiling with it right now (one module per patched stage)
    np->spv = std::move(words);
    t->cur.store(np, std::memory_order_release);
    t->eft = true;
  }
  int n = 0;
  {
    std::lock_guard<std::mutex> lk(S->pm);
    for (auto& kv : S->map)
      for (Ent* e = kv.second; e; e = e->next)
        if (!strncmp(e->pd.stage[1], key12, 12)) { e->stale = true; n++; }
  }
  Log("pipeline service: fragment template %.12s patched with early fragment tests, %d pipelines rebuilt on next use", key12, n);
}

Tmpl* GetTmpl(const char* key12) {
  Tmpl* t;
  std::string full;
  {
    std::lock_guard<std::mutex> lk(S->tm);
    auto& slot = S->tmpls[key12];
    if (!slot) slot = new Tmpl;
    t = slot;
    auto it = S->fullKeys.find(key12);
    if (it != S->fullKeys.end()) full = it->second;
  }
  if (full.empty() && S->h) {   // startup precompile: the game has not built this stage yet, find its translation in the shader cache by the 12-hex prefix
    const std::vector<std::string> matches = nvn::os_host::list_files(S->h->cacheDir, key12, ".spv");
    if (!matches.empty()) {
      const std::string& n = matches.front();
      if (n.size() > 4) full = n.substr(0, n.size() - 4);
    }
  }
  std::call_once(t->once, [&] {
    if (full.empty()) return;
    bool want, applied = false;
    { std::lock_guard<std::mutex> lk(S->tm); want = EftFixOn() && S->eftKeys.count(key12) != 0; }
    t->ok = LoadStageFromCache(S->h, full, &t->sp, want, &applied);
    t->cur.store(&t->sp, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(t->em); t->eft = applied; }
    bool late;   // the game registered the program as early-fragment-tests while this load ran
    { std::lock_guard<std::mutex> lk(S->tm); late = t->ok && EftFixOn() && S->eftKeys.count(key12) != 0; }
    if (late) PatchTmplEft(t, key12);
  });
  return t->ok ? t : nullptr;
}

ProgramImpl* GetLayoutFor(const PipeDesc& pd, StageProg* out[6]) {
  std::string key;
  Tmpl* ts[5] = {};
  for (int s = 0; s < 5; s++) {
    out[s] = nullptr;
    if (!pd.stage[s][0]) continue;
    ts[s] = GetTmpl(pd.stage[s]);
    if (!ts[s]) return nullptr;
    out[s] = ts[s]->cur.load(std::memory_order_acquire);
    key += pd.stage[s]; key += '|';
  }
  out[5] = nullptr;
  std::lock_guard<std::mutex> lk(S->lm);
  auto it = S->layouts.find(key);
  if (it != S->layouts.end()) return it->second;
  auto* p = new ProgramImpl;
  p->host = S->h; p->composite = true;
  // [audit F3] layout metadata only: the template's fragMods may be growing on another worker right now (CreateGraphicsPipelineImpl)
  for (int s = 0; s < 5; s++) if (ts[s]) CopyStageMeta(p->st[s], *ts[s]->cur.load(std::memory_order_acquire));
  if (!ProgramBuildLayout(p)) { delete p; S->layouts[key] = nullptr; return nullptr; }
  S->layouts[key] = p;
  return p;
}

// ---- key list --------------------------------------------------------------------------------------------------------------------
void LoadKeys() {
  const std::string path = S->dir + std::string(1, nvn::os_host::path_sep()) + "pipeline_keys.bin";
  const FileHdr want = MakeHdr(S->h, kMagicKeys, false);
  bool good = false;
  {
    std::ifstream f(path, std::ios::binary);
    FileHdr got{};
    if (f && f.read((char*)&got, sizeof got) && HdrMatches(got, want)) {
      good = true;
      PipeDesc pd;
      std::unordered_set<uint64_t> seen;
      uint64_t nGood = 0;
      // Records are fixed size, so a torn tail (the process was killed mid-write) would shift every record appended after it. Stop at the first
      // short or implausible record and cut the file there before anything is appended.
      while (f.read((char*)&pd, sizeof pd)) {
        bool ok = pd.nattr >= 0 && pd.nattr <= 16 && pd.nstream >= 0 && pd.nstream <= 16 && pd.nColor >= 0 && pd.nColor <= 8;
        for (int s = 0; s < 5 && ok; s++) ok = pd.stage[s][15] == 0;
        if (!ok) break;
        nGood++;
        // [vkval] keys recorded before the compressed-render-target stand-in existed carry a block-compressed colour format (BC1 photo
        // thumbnails): Vulkan cannot render to those (VUID-VkGraphicsPipelineCreateInfo-renderPass-06582 at startup), and the live draw
        // now uses the stand-in format, so the old key is never hit. Do not precompile it.
        bool compressedRt = false;
        for (int i = 0; i < pd.nColor; i++) if (pd.colorFmt[i] >= 131 && pd.colorFmt[i] <= 184) compressedRt = true;   // VK_FORMAT_BC1_RGB..ASTC_12x12
        if (compressedRt) continue;
        if (seen.insert(HashDesc(pd)).second) S->known.push_back(pd);
      }
      S->recorded = seen;
      f.close();
      const uint64_t goodLen = sizeof(FileHdr) + nGood * sizeof(PipeDesc);
      std::error_code ec;
      const uint64_t have = std::filesystem::file_size(path, ec);
      if (!ec && have != goodLen) {
        std::filesystem::resize_file(path, goodLen, ec);
        Log("pipeline keys: cut %llu stray byte(s) after record %llu%s", (unsigned long long)(have - goodLen), (unsigned long long)nGood, ec ? " (resize failed)" : "");
        if (ec) good = false;   // cannot repair: start a fresh list rather than append after a torn record
      }
    }
  }
  if (!good) {   // missing or from another build / translator / descriptor layout: start a fresh list
    S->known.clear();
    S->keysFile = fopen(path.c_str(), "wb");
    if (S->keysFile) { fwrite(&want, sizeof want, 1, S->keysFile); fflush(S->keysFile); }
  } else {
    S->keysFile = fopen(path.c_str(), "ab");
  }
}
// Optional seed key list (SNAP_PIPELINE_SEED=<file>, else <exe dir>\pipeline_keys_seed.bin; SNAP_PIPELINE_SEED=0 turns it off): pipelines recorded
// elsewhere (e.g. a sweep of every course) are added to the startup precompile, so the first visit to a course finds its pipelines ready instead
// of compiling them while playing (draws skipped, compile hitches). Same header check as the user list; never written to, never appended to the
// user list; keys already known are skipped.
void AddSeedKeys() {
  std::string path;
  if (const char* e = getenv("SNAP_PIPELINE_SEED")) { if (!strcmp(e, "0")) return; path = e; }
  if (path.empty()) {
    const std::string dir = nvn::os_host::exe_dir();
    path = (dir.empty() ? std::string() : dir + std::string(1, nvn::os_host::path_sep())) + "pipeline_keys_seed.bin";
  }
  std::ifstream f(path, std::ios::binary);
  FileHdr got{};
  const FileHdr want = MakeHdr(S->h, kMagicKeys, false);
  if (!f || !f.read((char*)&got, sizeof got)) return;
  if (!HdrMatches(got, want)) { Log("pipeline seed: %s is from another build / translator, ignored", path.c_str()); return; }
  std::unordered_set<uint64_t> seen;
  for (const PipeDesc& k : S->known) seen.insert(HashDesc(k));
  PipeDesc pd;
  size_t added = 0, total = 0;
  while (f.read((char*)&pd, sizeof pd)) {
    bool ok = pd.nattr >= 0 && pd.nattr <= 16 && pd.nstream >= 0 && pd.nstream <= 16 && pd.nColor >= 0 && pd.nColor <= 8;
    for (int s = 0; s < 5 && ok; s++) ok = pd.stage[s][15] == 0;
    if (!ok) break;
    total++;
    bool compressedRt = false;
    for (int i = 0; i < pd.nColor; i++) if (pd.colorFmt[i] >= 131 && pd.colorFmt[i] <= 184) compressedRt = true;
    if (compressedRt) continue;
    if (seen.insert(HashDesc(pd)).second) { S->known.push_back(pd); added++; }
  }
  Log("pipeline seed: %s: %zu record(s), %zu new", path.c_str(), total, added);
}
void RecordKey(const PipeDesc& pd) {
  std::lock_guard<std::mutex> lk(S->km);
  if (!S->keysFile) return;
  // A short write (disk full) leaves a torn fixed-size record; appending after it would shift every later record. Stop recording for the
  // session instead; LoadKeys cuts the torn tail on the next start.
  if (fwrite(&pd, sizeof pd, 1, S->keysFile) != 1 || fflush(S->keysFile) != 0) {
    Log("pipeline keys: write failed, recording stopped for this session");
    fclose(S->keysFile);
    S->keysFile = nullptr;
  }
}

// ---- cache stamp and status -------------------------------------------------------------------------------------------------------
// <dir>\cache_stamp.txt says what the files in this folder were made for (two lines: GPU and driver, then translator / game build / layout).
// A different GPU, device ID, driver version or pipeline-cache UUID wipes pipeline_cache.bin (the driver blob; it is rebuilt from the key list
// at the next start, which is the "compile shaders" progress screen); a different translator, game build or PipeDesc layout wipes the key list
// too, and with it prepare.json (the "shaders prepared" mark of host/shaderprep.cpp). A folder with no stamp (made before this existed) is
// kept as it is: the file headers still reject whatever does not match. Writes <dir>\status.json for the launcher (key and ready counts).
std::string HexBytes(const uint8_t* b, size_t n) { static const char* d = "0123456789abcdef"; std::string o; for (size_t i = 0; i < n; i++) { o += d[b[i] >> 4]; o += d[b[i] & 15]; } return o; }

// Returns true when the folder was made for this GPU, driver and build (a missing stamp, a wipe or a new stamp all return false).
bool CheckStamp(Host* h) {
  char gpu[200], bld[160];
  snprintf(gpu, sizeof gpu, "vendor=%u device=%u driver=%u uuid=%s", h->props.vendorID, h->props.deviceID, h->props.driverVersion, HexBytes(h->props.pipelineCacheUUID, 16).c_str());
  snprintf(bld, sizeof bld, "translator=%016llx game=%016llx desc=%u format=%u", (unsigned long long)S->translatorHash, (unsigned long long)S->buildHash, (unsigned)sizeof(PipeDesc), kFormatVersion);
  const std::string path = S->dir + std::string(1, nvn::os_host::path_sep()) + "cache_stamp.txt";
  std::string oldGpu, oldBld;
  {
    std::ifstream f(path);
    if (f) { std::getline(f, oldGpu); std::getline(f, oldBld); }
  }
  while (!oldGpu.empty() && (oldGpu.back() == '\r')) oldGpu.pop_back();
  while (!oldBld.empty() && (oldBld.back() == '\r')) oldBld.pop_back();
  auto rm = [&](const char* n) { nvn::os_host::remove_file(S->dir + std::string(1, nvn::os_host::path_sep()) + n); };
  if (!oldGpu.empty() || !oldBld.empty()) {
    if (oldBld != bld) {
      Log("pipeline cache: the game build, translator or key layout changed (%s -> %s): key list and driver cache wiped", oldBld.c_str(), bld);
      rm("pipeline_keys.bin"); rm("pipeline_cache.bin"); rm("prepare.json"); rm("status.json");
    } else if (oldGpu != gpu) {
      Log("pipeline cache: the GPU or driver changed (%s -> %s): driver cache wiped, the key list is kept and compiled again", oldGpu.c_str(), gpu);
      rm("pipeline_cache.bin");
    }
  }
  const bool same = !oldGpu.empty() && oldGpu == gpu && oldBld == bld;
  if (!same) {
    std::ofstream f(path, std::ios::trunc);
    f << gpu << "\n" << bld << "\n";
  }
  return same;
}

void WriteStatus() {
  uint32_t ready = 0, entries = 0;
  {
    std::lock_guard<std::mutex> lk(S->pm);
    for (auto& kv : S->map) for (Ent* e = kv.second; e; e = e->next) { entries++; if (e->state.load() == ES_READY) ready++; }
  }
  size_t known;
  { std::lock_guard<std::mutex> lk(S->km); known = S->known.size(); }
  std::error_code ec;
  const std::string sep(1, nvn::os_host::path_sep());
  const uint64_t blob = std::filesystem::file_size(S->dir + sep + "pipeline_cache.bin", ec);
  char b[320];
  snprintf(b, sizeof b, "{\"format\": 1, \"known\": %zu, \"ready\": %u, \"entries\": %u, \"blob_bytes\": %llu, \"translator\": \"%016llx\", \"game\": \"%016llx\", \"updated\": %lld}\n",
           known, ready, entries, (unsigned long long)(ec ? 0 : blob), (unsigned long long)S->translatorHash, (unsigned long long)S->buildHash, (long long)time(nullptr));
  const std::string path = S->dir + sep + "status.json", tmp = path + ".tmp";
  { std::ofstream f(tmp, std::ios::trunc); f << b; if (!f.good()) return; }
  nvn::os_host::atomic_replace(tmp, path);
}

// ---- pipeline cache persistence ---------------------------------------------------------------------------------------------------
bool LoadCache(Host* h) {
  std::ifstream f(S->dir + std::string(1, nvn::os_host::path_sep()) + "pipeline_cache.bin", std::ios::binary);
  FileHdr got{};
  const FileHdr want = MakeHdr(h, kMagicCache, true);
  if (!f || !f.read((char*)&got, sizeof got) || !HdrMatches(got, want)) return false;   // other driver / build / translator: the blob is useless
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty()) return false;
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = data.size(); ci.pInitialData = data.data();
  VkPipelineCache pc = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(h->device, &ci, nullptr, &pc) == VK_SUCCESS) {
    if (h->pipelineCache) vkDestroyPipelineCache(h->device, h->pipelineCache, nullptr);
    h->pipelineCache = pc;
    Log("pipeline cache loaded (%zu bytes)", data.size());
    return true;
  }
  return false;
}
void SaveCache(bool force) {
  Host* h = S->h;
  if (!h->pipelineCache || (!force && !S->dirty.exchange(false))) return;
  size_t n = 0;
  if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
  std::vector<char> data(n);
  if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, data.data()) != VK_SUCCESS) return;
  // The housekeeper and the thread finishing a precompile run can both get here: each writes its own tmp file, then renames it into place
  // (MoveFileEx replace is atomic), so the cache file is always one writer's complete blob.
  static std::atomic<unsigned> saveSeq{0};
  const std::string path = S->dir + std::string(1, nvn::os_host::path_sep()) + "pipeline_cache.bin",
                    tmp = path + "." + std::to_string(nvn::os_host::current_process_id()) + "." + std::to_string(saveSeq.fetch_add(1)) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    const FileHdr hdr = MakeHdr(h, kMagicCache, true);
    f.write((const char*)&hdr, sizeof hdr);
    f.write(data.data(), (std::streamsize)n);
    f.flush();
    if (!f.good()) { f.close(); nvn::os_host::remove_file(tmp); return; }
  }
  if (!nvn::os_host::atomic_replace(tmp, path)) { nvn::os_host::remove_file(tmp); return; }
  S->lastSaveNs = snap_perf::now_ns();
  Log("pipeline cache saved (%zu bytes)", n);
  WriteStatus();
}

// ---- compile jobs -----------------------------------------------------------------------------------------------------------------
void StoreResult(Ent* e, VkPipeline pipe, int failState) {
  e->pipe = pipe;
  e->state.store(pipe ? ES_READY : failState, std::memory_order_release);
}

Ent* FindOrNull(const PipeDesc& pd, uint64_t hsh) {   // caller holds pm
  auto it = S->map.find(hsh);
  for (Ent* e = it == S->map.end() ? nullptr : it->second; e; e = e->next)
    if (memcmp(&e->pd, &pd, sizeof pd) == 0) return e;
  return nullptr;
}

void RunJob(const Job& j) {
  const uint64_t hsh = HashDesc(j.pd);
  Ent* e;
  { std::lock_guard<std::mutex> lk(S->pm); e = FindOrNull(j.pd, hsh); }
  if (!e || e->state.load() == ES_READY) return;
  StageProg* st[6];
  ProgramImpl* lay = GetLayoutFor(j.pd, st);
  VkPipeline pipe = VK_NULL_HANDLE;
  if (lay) pipe = CreateGraphicsPipeline(S->h, j.pd, st, lay->layout);
  StoreResult(e, pipe, ES_NEEDS_INLINE);   // a failed background build is retried inline by the replay thread (it has the real program)
  if (pipe) S->dirty = true;
}

void Worker() {
  for (;;) {
    Job j;
    bool isHot;
    {
      std::unique_lock<std::mutex> lk(S->qm);
      for (;;) {
        if (S->stop) return;
        const bool canRun = S->running < S->activeLimit.load() || !S->hot.empty();
        if (canRun && (!S->hot.empty() || (!S->cold.empty() && !S->cancel.load()))) break;
        if (S->cancel.load() && !S->cold.empty()) { S->cold.clear(); }
        S->qcv.wait_for(lk, std::chrono::milliseconds(200));
      }
      isHot = !S->hot.empty();
      if (isHot) { j = S->hot.front(); S->hot.pop_front(); }
      else { j = S->cold.front(); S->cold.pop_front(); }
      S->running++;
    }
    if (isHot || S->mode.load() == SHADERPRECOMPILE_MODE_NOW) nvn::os_host::set_current_thread_normal();
    else nvn::os_host::set_current_thread_below_normal();
    const uint64_t t0 = snap_perf::now_ns();
    RunJob(j);
    const uint64_t dt = snap_perf::now_ns() - t0;
    if (isHot) { if (S->bgPending.load()) S->bgPending--; }
    else {
      S->busyNs += dt;
      const uint32_t d = ++S->done;
      if (d >= S->total.load() && S->state.load() == SHADERPRECOMPILE_RUNNING) {
        S->state = SHADERPRECOMPILE_DONE;
        const double wallMs = (double)(snap_perf::now_ns() - S->runStartNs.load()) / 1e6;
        char det[96]; snprintf(det, sizeof det, "%u pipelines", d);
        snap_perf::event("precompile_done", wallMs, det);
        Log("pipeline run: %u pipelines built in %.0f ms", d, wallMs);
        if (d) S->avgMs = (double)S->busyNs.load() / 1e6 / d;
        SaveCache(true);
        std::ofstream st(S->dir + std::string(1, nvn::os_host::path_sep()) + "stats.txt", std::ios::trunc);
        st << S->avgMs << "\n";
      }
    }
    { std::lock_guard<std::mutex> lk(S->qm); S->running--; }
    S->qcv.notify_all();
  }
}

int WorkerCountFor(int mode) {
  if (const char* e = getenv("SNAP_PIPELINE_THREADS")) { int n = atoi(e); if (n > 0) return std::min(n, 32); }
  const int hw = (int)std::max(2u, std::thread::hardware_concurrency());
  if (mode == SHADERPRECOMPILE_MODE_NOW) return std::clamp(hw / 2, 2, 16);
  return std::clamp(hw / 8, 2, 4);
}
void EnsureWorkers(int want) {
  std::lock_guard<std::mutex> lk(S->qm);
  S->activeLimit = want;
  while ((int)S->workers.size() < want) S->workers.emplace_back(Worker);
}

void Housekeeping() {
  while (true) {
    for (int i = 0; i < 50; i++) {
      nvn::os_host::sleep_ms(100);
      std::lock_guard<std::mutex> lk(S->qm);
      if (S->stop) return;
    }
    if (S->dirty.load() && (snap_perf::now_ns() - S->lastSaveNs.load()) > 30ull * 1000000000ull) SaveCache(false);
  }
}

int BeginRun() {
  if (!S || !S->up) { std::lock_guard<std::mutex> lk(g_early); g_wantBegin = true; return 0; }
  std::vector<PipeDesc> todo, knownCopy;
  { std::lock_guard<std::mutex> lk(S->km); knownCopy = S->known; }
  {
    std::lock_guard<std::mutex> lk(S->pm);
    for (const PipeDesc& pd : knownCopy) {
      const uint64_t hsh = HashDesc(pd);
      if (FindOrNull(pd, hsh)) continue;
      Ent* e = new Ent; memcpy(&e->pd, &pd, sizeof pd);
      e->next = S->map.count(hsh) ? S->map[hsh] : nullptr;
      S->map[hsh] = e;
      todo.push_back(pd);
    }
  }
  S->cancel = false;
  S->done = 0; S->failed = 0; S->busyNs = 0;
  S->total = (uint32_t)todo.size();
  S->runStartNs = snap_perf::now_ns();
  Log("pipeline run: %zu known, %zu to build, mode %d, %s", knownCopy.size(), todo.size(), S->mode.load(),
      S->warm ? "warm start (same GPU, driver and build: the game is not held)" : "cold start");
  if (todo.empty()) { S->state = SHADERPRECOMPILE_DONE; return 0; }
  S->state = SHADERPRECOMPILE_RUNNING;
  EnsureWorkers(WorkerCountFor(S->mode.load()));
  {
    std::lock_guard<std::mutex> lk(S->qm);
    for (auto& pd : todo) S->cold.push_back({pd, false});
  }
  S->qcv.notify_all();
  return (int)todo.size();
}

}  // namespace

// ---- public service API -----------------------------------------------------------------------------------------------------------
void PipelineRegisterStageKey(const char* key12, const char* fullKey, bool eft) {
  if (!S) return;
  Tmpl* t = nullptr;
  {
    std::lock_guard<std::mutex> lk(S->tm);
    S->fullKeys.emplace(key12, fullKey);
    if (eft && EftFixOn()) {
      S->eftKeys.insert(key12);
      auto it = S->tmpls.find(key12);
      if (it != S->tmpls.end()) t = it->second;
    }
  }
  if (t && t->ok) PatchTmplEft(t, key12);   // loaded before the game built the program (a load still running re-checks eftKeys itself)
}

std::atomic<uint64_t> g_shutterNs{0};
// Registered as hle::g_shutter_hook: the pad poll (runtime/hle/hid.cpp) calls it while the A button is down.
static void ShutterNote() { g_shutterNs.store(snap_perf::now_ns(), std::memory_order_relaxed); }

void PipelineServiceInit(Host* h) {
  if (S) return;
  hle::g_shutter_hook = &ShutterNote;
  S = new Svc;
  S->h = h;
  S->dir = Dir();
  S->translatorHash = Fnv(TranslatorVersion(), strlen(TranslatorVersion()));
  const char* b = getenv("SNAP_GAME_BUILD_ID");
  S->buildHash = Fnv(b && *b ? b : g_buildId.c_str(), strlen(b && *b ? b : g_buildId.c_str()));
  const char* as = getenv("SNAP_PIPELINE_ASYNC");
  S->async = !(as && *as == '0');
  // mode: env override, else <dir>\mode.txt
  int mode = SHADERPRECOMPILE_MODE_UNSET;
  if (const char* e = getenv("SNAP_SHADER_PRECOMPILE")) mode = !strcmp(e, "now") ? SHADERPRECOMPILE_MODE_NOW : !strcmp(e, "playing") ? SHADERPRECOMPILE_MODE_PLAYING : mode;
  else {
    std::ifstream f(S->dir + std::string(1, nvn::os_host::path_sep()) + "mode.txt");
    std::string w;
    if (f >> w) mode = w == "now" ? SHADERPRECOMPILE_MODE_NOW : w == "playing" ? SHADERPRECOMPILE_MODE_PLAYING : mode;
  }
  { std::lock_guard<std::mutex> lk(g_early); if (g_earlyMode >= 0) mode = g_earlyMode; }
  S->mode = mode;
  { std::ifstream f(S->dir + std::string(1, nvn::os_host::path_sep()) + "stats.txt"); double v; if (f >> v && v > 0.1 && v < 5000) S->avgMs = v; }
  // enumerate the translation cache so background jobs can load any stage by its key prefix
  for (const std::string& n : nvn::os_host::list_files(h->cacheDir, nullptr, ".json")) {
    if (n.size() == 64 + 5) S->fullKeys.emplace(n.substr(0, 12), n.substr(0, 64));
  }
  const bool stampSame = CheckStamp(h);
  const bool blobLoaded = LoadCache(h);
  S->warm = stampSame && blobLoaded;
  LoadKeys();
  AddSeedKeys();
  S->lastSaveNs = snap_perf::now_ns();
  S->housekeeper = std::thread(Housekeeping);
  S->up = true;
  Log("pipeline service: %zu recorded pipelines, mode %d, async %d, dir %s", S->known.size(), mode, (int)S->async, S->dir.c_str());
  bool begin;
  { std::lock_guard<std::mutex> lk(g_early); begin = g_wantBegin; g_wantBegin = false; }
  // NOW and PLAYING both start the run at boot; only NOW holds the game. UNSET waits for the UI's choice (or an explicit begin()).
  if (begin || mode == SHADERPRECOMPILE_MODE_NOW || mode == SHADERPRECOMPILE_MODE_PLAYING) BeginRun();
}

void PipelineServiceShutdown(Host* h) {
  if (!S) return;
  { std::lock_guard<std::mutex> lk(S->qm); S->stop = true; S->cancel = true; }
  S->qcv.notify_all();
  for (auto& t : S->workers) if (t.joinable()) t.join();
  if (S->housekeeper.joinable()) S->housekeeper.join();
  SaveCache(true);
  for (auto& kv : S->map)
    for (Ent* e = kv.second; e;) { Ent* n = e->next; if (e->pipe) vkDestroyPipeline(h->device, e->pipe, nullptr); delete e; e = n; }
  S->map.clear();
  for (auto& kv : S->layouts) if (kv.second) { ProgramDestroy(kv.second); delete kv.second; }
  for (auto& kv : S->tmpls) {
    Tmpl* t = kv.second;
    StageProg* c = t->cur.load();
    if (t->ok && t->sp.mod) vkDestroyShaderModule(h->device, t->sp.mod, nullptr);
    if (c && c != &t->sp) { if (c->mod) vkDestroyShaderModule(h->device, c->mod, nullptr); delete c; }   // the PatchTmplEft copy
    delete t;
  }
  if (S->keysFile) fclose(S->keysFile);
  S->up = false;
}

bool PipelineShutterWindow() {
  static const bool on = [] { const char* e = getenv("SNAP_PHOTO_NOSKIP"); return !(e && *e == '0'); }();
  if (!on) return false;
  const uint64_t t = g_shutterNs.load(std::memory_order_relaxed);
  return t && snap_perf::now_ns() - t < 500000000ull;   // 0.5 s: the capture is resampled within a few frames of the press
}

VkPipeline PipelineAcquire(Host* h, const PipeDesc& pd, ProgramImpl* p, bool* pending, bool mustHave) {
  if (!S || !S->up) return VK_NULL_HANDLE;
  const uint64_t hsh = HashDesc(pd);
  Ent* e;
  bool isNew = false;
  {
    std::lock_guard<std::mutex> lk(S->pm);
    e = FindOrNull(pd, hsh);
    if (!e) {
      e = new Ent; memcpy(&e->pd, &pd, sizeof pd);
      auto it = S->map.find(hsh);
      e->next = it == S->map.end() ? nullptr : it->second;
      S->map[hsh] = e;
      isNew = true;
    }
  }
  if (!isNew) {
    int st = e->state.load(std::memory_order_acquire);
    if (st == ES_PENDING && mustHave && S->async) {   // wait for the background compile (hot jobs run first); give up after 10 s
      if (!e->hot.exchange(true)) {
        S->bgPending++;
        { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
        S->qcv.notify_one();
      }
      for (int i = 0; i < 10000 && (st = e->state.load(std::memory_order_acquire)) == ES_PENDING; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (st == ES_READY && e->stale.load(std::memory_order_acquire) && p) {   // see PatchTmplEft: rebuild with the game's own (patched) stages
      StageProg* sts[6] = {};
      for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
      if (VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout)) { e->stale = false; e->pipe = pipe; S->dirty = true; }   // old pipeline leaked: may be in flight
      return e->pipe;
    }
    if (st == ES_READY) return e->pipe;
    if (st == ES_PENDING) {
      if (S->async && !e->hot.exchange(true)) {   // queued cold by the precompile run, but the game needs it now: add a hot job
        S->bgPending++;
        { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
        S->qcv.notify_one();
      }
      *pending = true;
      return VK_NULL_HANDLE;
    }
    if (st == ES_FAILED) return VK_NULL_HANDLE;
    // ES_NEEDS_INLINE: the background build failed (e.g. a stage missing from the translation cache): build on this thread with the real program
    StageProg* sts[6] = {};
    for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
    VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout);
    StoreResult(e, pipe, ES_FAILED);
    if (pipe) S->dirty = true;
    return pipe;
  }
  // first request of this pipeline in this run
  bool known = false;
  {
    // recorded earlier (a previous run) pipelines are already in the map from BeginRun; reaching here means it is genuinely new
    (void)known;
    bool dup;
    { std::lock_guard<std::mutex> lk(S->km); dup = !S->recorded.insert(hsh).second; }
    // Requests that arrive before BeginRun has filled the map (the first presents) used to append a copy of a key already in the list every run
    // (4,010 records for 339 distinct ones); LoadKeys deduplicated them, so only the file grew.
    if (!dup) {
      RecordKey(pd);
      std::lock_guard<std::mutex> lk(S->km);
      S->known.push_back(pd);
    }
  }
  if (S->async && !mustHave) {
    EnsureWorkers(std::max(S->activeLimit.load(), WorkerCountFor(S->mode.load())));
    e->hot = true;
    S->bgPending++;
    { std::lock_guard<std::mutex> lk(S->qm); S->hot.push_back({pd, true}); }
    S->qcv.notify_one();
    *pending = true;
    return VK_NULL_HANDLE;
  }
  StageProg* sts[6] = {};
  for (int s = 0; s < 5; s++) sts[s] = p->st[s].valid ? &p->st[s] : nullptr;
  VkPipeline pipe = CreateGraphicsPipeline(h, pd, sts, p->layout);
  StoreResult(e, pipe, ES_FAILED);
  if (pipe) S->dirty = true;
  return pipe;
}

bool PipelineHoldGame() {
  // A warm start (same GPU, driver and build, driver cache loaded) replays the key list as cache hits in under a second: the game is not held
  // for it, and nothing compiles. Only a changed driver, GPU or build (or a first run) holds the game on the progress screen.
  return S && S->up && S->mode.load() == SHADERPRECOMPILE_MODE_NOW && S->state.load() == SHADERPRECOMPILE_RUNNING && !S->cancel.load() && !S->warm;
}

}  // namespace nvn

// ---- C API ----------------------------------------------------------------------------------------------------------------------
using namespace nvn;
extern "C" {

int shaderprecompile_begin(void) { return BeginRun(); }
int shaderprecompile_state(void) { return S ? S->state.load() : SHADERPRECOMPILE_IDLE; }
void shaderprecompile_progress(uint32_t* done, uint32_t* total, double* eta) {
  const uint32_t d = S ? S->done.load() : 0, t = S ? S->total.load() : 0;
  if (done) *done = d;
  if (total) *total = t;
  if (eta) {
    *eta = -1;
    if (S && d > 0 && S->state.load() == SHADERPRECOMPILE_RUNNING) {
      const double el = (double)(snap_perf::now_ns() - S->runStartNs.load()) / 1e9;
      *eta = el / d * (t - d);
    } else if (S && S->state.load() == SHADERPRECOMPILE_DONE) *eta = 0;
  }
}
void shaderprecompile_cancel(void) {
  if (!S) return;
  S->cancel = true;
  if (S->state.load() == SHADERPRECOMPILE_RUNNING) S->state = SHADERPRECOMPILE_CANCELLED;
  S->qcv.notify_all();
}
int shaderprecompile_get_mode(void) { return S ? S->mode.load() : SHADERPRECOMPILE_MODE_UNSET; }
void shaderprecompile_set_mode(int mode) {
  if (mode < 0 || mode > 2) return;
  if (!S || !S->up) { std::lock_guard<std::mutex> lk(g_early); g_earlyMode = mode; return; }
  S->mode = mode;
  std::ofstream f(S->dir + std::string(1, nvn::os_host::path_sep()) + "mode.txt", std::ios::trunc);
  f << (mode == SHADERPRECOMPILE_MODE_NOW ? "now" : mode == SHADERPRECOMPILE_MODE_PLAYING ? "playing" : "unset") << "\n";
  if (mode == SHADERPRECOMPILE_MODE_NOW && S->state.load() != SHADERPRECOMPILE_RUNNING) BeginRun();
}
double shaderprecompile_estimate_seconds(void) {
  if (!S) return -1;
  const int thr = WorkerCountFor(SHADERPRECOMPILE_MODE_NOW);
  size_t todo = S->total.load() ? (S->state.load() == SHADERPRECOMPILE_RUNNING ? S->total.load() - S->done.load() : S->total.load()) : S->known.size();
  return (double)todo * S->avgMs / 1000.0 / std::max(thr, 1);
}
int shaderprecompile_is_holding_game(void) { return PipelineHoldGame() ? 1 : 0; }
uint32_t shaderprecompile_background_pending(void) { return S ? S->bgPending.load() : 0; }

}  // extern "C"

