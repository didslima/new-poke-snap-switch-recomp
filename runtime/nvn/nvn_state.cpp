// SPDX-License-Identifier: MIT
// Save states, NVN side (docs/save-states.md 1.4, 2.5-2.7).
//
// A restored guest memory image names the host objects ({magic, impl} pairs) that existed at the save. So from the first call to the end of
// the game every guest-visible object is tracked from creation (StateMade) to the guest's finalize (StateGone). While a state exists:
//  - an object that was alive at the save is never freed (StateKeep parks it: a "zombie");
//  - a load, with the GPU idle and every guest thread parked, frees the objects made since the save, takes the zombies off the graveyard,
//    puts the guest-API fields of the saved objects back (descriptor pool slots, sync values, window slot, command buffer recordings),
//    and re-registers revived memory pools in the synthetic address space. GPU truth (image layouts, mirrors, timelines) keeps its value.
// Queues and windows are not parked: a load is refused when the set of either changed.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#if __has_include("nvn_foliage.h")
#include "nvn_foliage.h"
#define SNAP_STATE_FOLIAGE 1
#endif
#include "nvn_hdphoto.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_shiny.h"

#include <atomic>
namespace hle::input { extern std::atomic<bool> g_shot_req; void route_state_loaded(); }   // runtime/hle/input_core.cpp: the screenshot request flag

extern "C" void snap_fs_state_save();                          // runtime/hle/fs.cpp: which files and directories are open
extern "C" void snap_fs_state_load(int* closed, int* reopened);
extern "C" void snap_upscale_state_loaded();    // runtime/nvn/upscale/upscale.cpp: temporal history reset
extern "C" void snap_framegen_state_loaded();   // runtime/nvn/nvn_framegen.cpp

extern "C" {
struct SnapStateHooks {
  void (*pre_save)();                          // the world is parked: GPU idle, readbacks landed, take the NVN snapshot
  int (*pre_load)(char *why, int cap);         // the world is parked: GPU idle, readbacks landed; 0 = refuse (why says why)
  void (*post_load)();                         // guest memory is back: fix up the host objects
  void (*shot)();                              // test aid: request a screenshot (SNAP_STATE_TEST ",shots")
  void (*abort_load)();                        // not used here (null); the state layer reads this member, so the layout must match os_state.cpp
};
void snap_state_set_hooks(const SnapStateHooks *h);   // runtime/os/os_state.cpp
}

namespace nvn {
void CmdVecGive(std::vector<Cmd> &v);   // nvn_life.cpp
namespace {
std::mutex g_mu;
std::unordered_set<void *> g_live[SK_COUNT];    // guest-visible and not yet finalized
std::unordered_set<void *> g_saved[SK_COUNT];   // g_live at the save
bool g_hold = false;                            // a state exists
bool g_haveSnap = false;
struct Zombie { StateKind k; void *p; };
std::vector<Zombie> g_zombies;                  // alive at the save, finalized since: kept, not destroyed

struct TexSlotData { TextureImpl *tex; TexView view; bool hasView; };
struct SyncSnap { SyncImpl *s; VkSemaphore sem; uint64_t value; bool armed; };
struct WinSnap { WindowImpl *w; int nextSlot, presentInterval, crop[4]; };
struct CbSnap { CommandBufferImpl *c; bool recording; CommandBufferMemoryCallback cb; void *cbData; std::vector<Cmd> cmds; uint64_t commandBytes, controlBytes; uintptr_t ctlBase; size_t ctlSize, ctlUsed; };
struct TexPoolSnap { TexPoolImpl *p; std::vector<std::pair<unsigned, std::vector<TexSlotData>>> chunks; };
struct SampPoolSnap { SampPoolImpl *p; std::vector<std::pair<unsigned, std::vector<SamplerImpl *>>> chunks; };
std::vector<SyncSnap> g_syncs;
std::vector<WinSnap> g_wins;
std::vector<CbSnap> g_cbs;
std::vector<TexPoolSnap> g_tps;
std::vector<SampPoolSnap> g_sps;
uint64_t g_madeSince = 0, g_goneSince = 0;     // counted while a state exists (logged at a load)

void DestroyKindNow(StateKind k, void *o) {   // the GPU is idle
  switch (k) {
    case SK_TEXTURE: {
      auto *t = static_cast<TextureImpl *>(o);
#ifdef SNAP_STATE_FOLIAGE
      FoliageTextureGone(t);
#endif
      if (ShinyActive()) ShinyTexGone(t);
      if (HdPhotoOn()) HdPhotoTexGone(t);
      TexPrefetchForget(t);
      DestroyTexture(t);
      break;
    }
    case SK_SAMPLER: {
      auto *s = static_cast<SamplerImpl *>(o);
      if (s->host && s->host->device && s->sampler) vkDestroySampler(s->host->device, s->sampler, nullptr);
      delete s;
      break;
    }
    case SK_POOL: DestroyPool(static_cast<PoolImpl *>(o)); break;
    case SK_TEXPOOL: TexPoolLive(static_cast<TexPoolImpl *>(o), false); delete static_cast<TexPoolImpl *>(o); break;
    case SK_SAMPPOOL: SampPoolLive(static_cast<SampPoolImpl *>(o), false); delete static_cast<SampPoolImpl *>(o); break;
    case SK_PROGRAM: { auto *p = static_cast<ProgramImpl *>(o); ProgramDestroy(p); delete p; break; }
    case SK_SYNC: delete static_cast<SyncImpl *>(o); break;
    case SK_BUFFER: {
      auto *i = static_cast<BufferImpl *>(o);
      if (i->ownPool && i->pool) { void *mem = i->pool->storage; DestroyPool(i->pool); nvn::os_host::aligned_free(mem); }
      delete i;
      break;
    }
    case SK_CB: {
      auto *c = static_cast<CommandBufferImpl *>(o);
      ListRecorderGone(c, c->ctlBase, c->ctlBase + c->ctlSize);
      CmdVecGive(c->cmds);
      delete c;
      break;
    }
    default: break;   // queues and windows are never parked or destroyed here
  }
}

void ClearSnapshots() {
  g_syncs.clear(); g_wins.clear(); g_cbs.clear(); g_tps.clear(); g_sps.clear();
}

void DropOldState() {   // the GPU is idle
  for (Zombie &z : g_zombies) DestroyKindNow(z.k, z.p);
  g_zombies.clear();
  ClearSnapshots();
  for (auto &s : g_saved) s.clear();
}

void PreSave() {
  snap_fs_state_save();
  StateIdleAll();
  std::lock_guard<std::mutex> lk(g_mu);
  DropOldState();
  for (int k = 0; k < SK_COUNT; k++) g_saved[k] = g_live[k];
  for (void *p : g_saved[SK_SYNC]) { auto *s = static_cast<SyncImpl *>(p); g_syncs.push_back({s, s->sem, s->value, s->armed}); }
  for (void *p : g_saved[SK_WINDOW]) { auto *w = static_cast<WindowImpl *>(p); WinSnap ws{w, w->nextSlot, w->presentInterval, {}}; memcpy(ws.crop, w->crop, sizeof ws.crop); g_wins.push_back(ws); }
  for (void *p : g_saved[SK_CB]) {
    auto *c = static_cast<CommandBufferImpl *>(p);
    g_cbs.push_back({c, c->recording, c->cb, c->cbData, c->cmds, c->commandBytes, c->controlBytes, c->ctlBase, c->ctlSize, c->ctlUsed});
  }
  for (void *p : g_saved[SK_TEXPOOL]) {
    auto *tp = static_cast<TexPoolImpl *>(p);
    TexPoolSnap s{tp, {}};
    for (unsigned c = 0; c < SlotTable<TexSlot>::kChunks; c++) {
      TexSlot *ch = tp->slots.chunks[c].load(std::memory_order_acquire);
      if (!ch) continue;
      std::vector<TexSlotData> v(SlotTable<TexSlot>::kChunk);
      for (unsigned i = 0; i < SlotTable<TexSlot>::kChunk; i++) v[i] = {ch[i].tex, ch[i].view, ch[i].hasView};
      s.chunks.emplace_back(c, std::move(v));
    }
    g_tps.push_back(std::move(s));
  }
  for (void *p : g_saved[SK_SAMPPOOL]) {
    auto *sp = static_cast<SampPoolImpl *>(p);
    SampPoolSnap s{sp, {}};
    for (unsigned c = 0; c < SlotTable<SamplerImpl *>::kChunks; c++) {
      SamplerImpl **ch = sp->slots.chunks[c].load(std::memory_order_acquire);
      if (!ch) continue;
      s.chunks.emplace_back(c, std::vector<SamplerImpl *>(ch, ch + SlotTable<SamplerImpl *>::kChunk));
    }
    g_sps.push_back(std::move(s));
  }
  g_hold = true;
  g_haveSnap = true;
  g_madeSince = g_goneSince = 0;
}

int PreLoad(char *why, int cap) {
  StateIdleAll();
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_haveSnap) { snprintf(why, cap, "no NVN snapshot"); return 0; }
  if (g_live[SK_QUEUE] != g_saved[SK_QUEUE]) { snprintf(why, cap, "the set of NVN queues changed"); return 0; }
  if (g_live[SK_WINDOW] != g_saved[SK_WINDOW]) { snprintf(why, cap, "the set of NVN windows changed"); return 0; }
  return 1;
}

void PostLoad() {   // guest memory is restored; the GPU is idle; every guest thread is parked
  std::lock_guard<std::mutex> lk(g_mu);
  snap_upscale_state_loaded(); snap_framegen_state_loaded();   // the camera jumped: no temporal history across a load
  hle::input::route_state_loaded();   // the HLE route signals (film counter) were reported for the state that is gone
  // 1. objects made since the save: the restored guest names none of them
  std::vector<Zombie> dead;
  std::unordered_set<const void *> deadSet;
  for (int k = 0; k < SK_COUNT; k++) {
    if (k == SK_QUEUE || k == SK_WINDOW) continue;
    for (void *p : g_live[k])
      if (!g_saved[k].count(p)) { dead.push_back({(StateKind)k, p}); deadSet.insert(p); }
  }
  if (!dead.empty()) {
    std::vector<QueueImpl *> qs;
    for (void *p : g_saved[SK_QUEUE]) qs.push_back(static_cast<QueueImpl *>(p));
    for (QueueImpl *q : qs) PersistScrub(q, deadSet);   // the queue's persistent draw state forgets them
    // descriptor pool slots naming them are overwritten from the snapshot below (or the pool itself is among the dead)
  }
  // 2. objects alive at the save that the guest finalized since: they live again
  std::unordered_set<const void *> alive;
  for (int k = 0; k < SK_COUNT; k++) for (void *p : g_saved[k]) alive.insert(p);
  GraveRevive(alive);
  size_t revived = 0;
  for (Zombie &z : g_zombies) {
    if (z.k == SK_POOL) {   // MemoryPoolFinalize took it out of the synthetic address space
      auto *p = static_cast<PoolImpl *>(z.p);
      if (p->host) { std::lock_guard<std::mutex> vl(p->host->vaMutex); p->host->vaMap[p->va] = p; p->host->vaGen.fetch_add(1, std::memory_order_release); }
    }
    revived++;
  }
  g_zombies.clear();
  // 3. guest-API fields
  for (SyncSnap &s : g_syncs) { s.s->sem = s.sem; s.s->value = s.value; s.s->armed = s.armed; }
  for (WinSnap &w : g_wins) { w.w->nextSlot = w.nextSlot; w.w->presentInterval = w.presentInterval; memcpy(w.w->crop, w.crop, sizeof w.crop); }
  for (CbSnap &c : g_cbs) {
    c.c->recording = c.recording; c.c->cb = c.cb; c.c->cbData = c.cbData; c.c->cmds = c.cmds;
    c.c->commandBytes = c.commandBytes; c.c->controlBytes = c.controlBytes; c.c->ctlBase = c.ctlBase; c.c->ctlSize = c.ctlSize; c.c->ctlUsed = c.ctlUsed;
  }
  for (TexPoolSnap &s : g_tps) {
    for (unsigned c = 0; c < SlotTable<TexSlot>::kChunks; c++) {   // slots written since the save are cleared, saved ones put back
      TexSlot *ch = s.p->slots.chunks[c].load(std::memory_order_acquire);
      if (!ch) continue;
      const std::vector<TexSlotData> *snap = nullptr;
      for (auto &kv : s.chunks) if (kv.first == c) { snap = &kv.second; break; }
      for (unsigned i = 0; i < SlotTable<TexSlot>::kChunk; i++) {
        TexSlot &sl = ch[i];
        const uint32_t q = sl.seq.load(std::memory_order_relaxed);
        sl.seq.store(q | 1, std::memory_order_release);
        if (snap) { sl.tex = (*snap)[i].tex; sl.view = (*snap)[i].view; sl.hasView = (*snap)[i].hasView; }
        else { sl.tex = nullptr; sl.hasView = false; }
        sl.seq.store((q | 1) + 1, std::memory_order_release);
      }
    }
  }
  for (SampPoolSnap &s : g_sps) {
    for (unsigned c = 0; c < SlotTable<SamplerImpl *>::kChunks; c++) {
      SamplerImpl **ch = s.p->slots.chunks[c].load(std::memory_order_acquire);
      if (!ch) continue;
      const std::vector<SamplerImpl *> *snap = nullptr;
      for (auto &kv : s.chunks) if (kv.first == c) { snap = &kv.second; break; }
      for (unsigned i = 0; i < SlotTable<SamplerImpl *>::kChunk; i++) ch[i] = snap ? (*snap)[i] : nullptr;
    }
  }
  // 4. the objects made since the save go (their pool slots were reset above)
  for (Zombie &d : dead) DestroyKindNow(d.k, d.p);
  for (int k = 0; k < SK_COUNT; k++) if (k != SK_QUEUE && k != SK_WINDOW) g_live[k] = g_saved[k];
  fprintf(stderr, "[nvn] state load: %zu objects made since the save freed, %zu zombies revived (made %llu, finalized %llu since the save)\n", dead.size(), revived,
          (unsigned long long)g_madeSince, (unsigned long long)g_goneSince);
  int fsClosed = 0, fsReopened = 0;
  snap_fs_state_load(&fsClosed, &fsReopened);   // files and directories: opened since the save are closed, closed since the save are open again
  fprintf(stderr, "[fs] state load: %d handles opened since the save closed, %d closed since the save reopened\n", fsClosed, fsReopened);
  fflush(stderr);
}
}  // namespace

void StateMade(StateKind k, void *p) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_live[k].insert(p);
  if (g_hold) g_madeSince++;
}

void StateGone(StateKind k, void *p) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_live[k].erase(p);
  if (g_hold) g_goneSince++;
}

bool StateKeep(StateKind k, void *p) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_hold || !g_saved[k].count(p)) return false;
  g_zombies.push_back({k, p});
  return true;
}

namespace {
struct Register {
  Register() {
    static const SnapStateHooks h = {PreSave, PreLoad, PostLoad, [] { hle::input::g_shot_req = true; }};
    snap_state_set_hooks(&h);
  }
} g_register;
}  // namespace
}  // namespace nvn
