// Route aim tool (project code, ours). Developer / test tool only: off unless SNAP_AIM=1, never set by the launcher or settings.
// With it unset nothing here runs (the two entry points return on one flag test and the function hook stays off).
//
// What it does: lets a route (SNAP_INPUT_SCRIPT, host/README.md "Scripted input") point the ride camera at a Pokemon instead of
// shooting blind.
//   aim <CH_xxx|nearest|pose:<text>|act:<text>|ev:<text>|off> [cap=<n>] [timeout=<s>] [tol=<half-screens>] [range=<m>] [up=<m>] [zoom=1|2] [near=<x>,<z>]
//                          up = how far above the actor's origin to aim (default 0.4 m), zoom=1 holds ZL once the target is centered
//                          (pose:<text> = the actor whose motion event name contains text, e.g. pose:EV_Mot_Mis_Stage001_017)
//                          steer the right stick until that actor sits on the reticle (and is within range); the route goes on
//                          when it does, or after the timeout (default 8 s; the run does not end). The target stays locked for
//                          the lines that follow, until `aim off` or the next aim line.
//   shoot                  press A for 4 presents and log the shot (target, screen offset, distance, pose); with NVN_FRAME_DUMP the
//                          frame is saved too. A centered shot marks that species done, so `aim nearest` picks another one next.
//   wait_pose <text> [timeout=<s>]
//                          go on when the locked target's (or, without one, any actor's) current motion event name contains
//                          <text>, e.g. wait_pose EV_Mot_Sp_P097_01
// Log lines: "aim: ..." (lock, centered, timeout) and "aim: shot target=<name> off=<x>,<y> dist=<m> pose=<event>".
//
// How it reads the game:
//  - camera: the ride camera builder's tanf call (main+0x296400, h_tanf in lodanim.cpp) hands over the camera object; eye /
//    target / up are at +0xd0 / +0xe0 / +0xf0, the aspect at +0x12c, and the tanf argument is the half field of view.
//  - actors: the collider component update (main+0x36f160: x0 = component, [x0+0x20] = owner actor, x2 = the owner's world
//    matrix, translation at +0x30) runs for every actor with a collider. It is watched through the function-entry hook
//    (g_fnprof, slow: about a fifth of guest CPU), which is why this is a test tool.
//  - what is centered is the picture (the photo is the whole view), not the game's pointer, which can sit off-center.
//  - SNAP_AIM_LOG=1 logs "aim: see <name> pose=<event> off=.. dist=.. pos=.." whenever a Pokemon's pose changes: the list of what
//    a course offers and when, to write a route from.
//  - SNAP_AIM_DISCOVER=1 logs the text found in each new actor's memory ("aim: actor ... str ..."), the way the name and pose
//    offsets below were found.
// Pure math: aim_core.h (tests/mods/aim_test.cpp).
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "loader.h"
#include "aim_core.h"

using namespace loader;

extern "C" volatile int g_fnprof;
extern "C" uint64_t snap_present_count();   // host/boot.cpp

namespace {

const uint64_t kCollUpdate = 0x710036f160ull;

bool g_on, g_discover, g_census;   // g_census (SNAP_AIM_LOG=1): one line whenever a Pokemon's pose changes
int g_census_n;
std::mutex g_mu;

struct Actor {
  uint64_t own = 0;
  aim::V3 pos{};
  uint64_t present = 0;
  std::string name, pose;
  uint64_t checked = 0;   // present of the last name / pose read
};
std::unordered_map<uint64_t, Actor> g_act;
aim::Cam g_cam;
uint64_t g_cam_present = ~0ull;

bool readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
bool ident(unsigned char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '.' || ch == '/'; }

// Text at p when it looks like a name (4 to 63 name characters, at least one letter, then a NUL).
std::string cstr_at(const char* p, size_t want = 0) {
  std::string t;
  if (!readable(p, 4)) return t;
  bool letter = false;
  for (int i = 0; i < 64; i++) {
    if (!readable(p + i, 1)) return {};
    const unsigned char ch = (unsigned char)p[i];
    if (!ch) break;
    if (!ident(ch) || i == 63) return {};
    letter |= ch >= 'A';
    t += (char)ch;
  }
  if (t.size() < 4 || !letter || (want && t.size() != want)) return {};
  return t;
}

// A libc++ std::string at a (short: size*2 in byte 0, text from byte 1; long: capacity|1, size, pointer).
std::string stdstr_at(uint64_t a) {
  if (!readable((void*)a, 24)) return {};
  const unsigned char b0 = *(const unsigned char*)a;
  if (!(b0 & 1)) { const size_t n = b0 >> 1; return n >= 4 && n <= 22 ? cstr_at((const char*)a + 1, n) : std::string(); }
  const uint64_t n = *(const uint64_t*)(a + 8), p = *(const uint64_t*)(a + 16);
  return n >= 4 && n < 64 ? cstr_at((const char*)p, (size_t)n) : std::string();
}

// The actor's own name (CH_xxx...) and its current motion event name. Offsets from the discovery log.
// Name: the model's skeleton name, a std::string at [actor+0x40]+0x28 ("SK_CH_034_00_0_A00"; CH_<n> is the Snap model number),
// given without the "SK_". It is empty until the model has loaded. Pose: a std::string at actor+0x418 ("EV_Mot_Com_Ex_Sleep_01").
std::string name_of(uint64_t own) {
  if (!readable((void*)(own + 0x40), 8)) return {};
  std::string n = stdstr_at(*(const uint64_t*)(own + 0x40) + 0x28);
  if (n.compare(0, 3, "SK_") == 0) n.erase(0, 3);
  return n;
}
std::string pose_of(uint64_t own) {
  const std::string p = stdstr_at(own + 0x418);
  return p.compare(0, 3, "EV_") == 0 ? p : std::string();
}
bool is_pokemon(const std::string& n) { return n.size() >= 6 && n.compare(0, 3, "CH_") == 0 && n[3] >= '0' && n[3] <= '9'; }

const char* g_only;   // discover_block: only texts that start with this (the shot-time event dump)

void discover_block(uint64_t own, uint64_t base, size_t size, int depth, const char* path, std::set<std::string>& seen, int& budget) {
  for (size_t o = 0; o + 8 <= size && budget > 0; o += 8) {
    if (!readable((void*)(base + o), 8)) { o = (o | 0xfff) - 7; continue; }
    std::string s = stdstr_at(base + o);
    const char* kind = "str";
    const uint64_t v = *(const uint64_t*)(base + o);
    if (s.empty() && v > 0x10000 && (v >> 47) == 0) { s = cstr_at((const char*)v); kind = "ptr"; }
    if (!s.empty() && g_only && s.compare(0, strlen(g_only), g_only) != 0) continue;
    if (!s.empty()) {
      char key[160];
      snprintf(key, sizeof key, "%s+%#zx %s %s", path, o, kind, s.c_str());
      if (seen.insert(key).second) { budget--; trace("aim: actor %llx %s", (unsigned long long)own, key); }
      continue;
    }
    if (depth > 0 && v > 0x10000 && (v >> 47) == 0 && (v & 7) == 0 && (v >> 32) != 0x71 && readable((void*)v, 0x40)) {
      char sub[64];
      snprintf(sub, sizeof sub, "%s+%#zx>", path, o);
      discover_block(own, v, 0x180, depth - 1, sub, seen, budget);
    }
  }
}

void discover(uint64_t own) {
  static int actors;
  if (actors >= 400) return;
  actors++;
  std::set<std::string> seen;
  int budget = 60;
  discover_block(own, own, 0x600, 1, "", seen, budget);
}

}  // namespace

// loader.cpp fnprof_hit -> skips.cpp snap_skips_observe -> here, for every guest function entry while g_fnprof is set.
// SNAP_AIM_EVTABLE=<text>: the event table of every Pokemon whose name holds <text> (1 = all), logged each time its pose changes.
// The event component (updated at main+0x3b5f10: x0 = component, [x0+0x20] = owner actor) keeps at [x0+0x260] a hash map from event
// id to a row {id, stars, motion event name, category, stars, photo event name, category}; on an event change it copies the row to
// the actor at +0x408 (motion: what the Pokemon plays, name at +0x418) and +0x448 (photo: what a photo of it records, name at
// +0x450). A row with an empty photo name gives photos no event, so a request tied to that name cannot be met by that row.
const uint64_t kEventUpdate = 0x71003b5f10ull;
const char* g_evtable;
std::unordered_map<uint64_t, uint64_t> g_evcomp;   // owner actor -> event component

// SNAP_AIM_EVTABLE=mis: every species once, only its request rows (a motion or photo event name with "Mis_").
bool g_evmis;
std::set<std::string> g_evdumped;

void evtable(uint64_t own, const std::string& name) {
  uint64_t comp = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_evcomp.find(own);
    if (it != g_evcomp.end()) comp = it->second;
    if (g_evmis && (!comp || !g_evdumped.insert(name.substr(0, 6)).second)) return;   // not yet known: the next pose change tries again
  }
  if (!comp || !readable((void*)(comp + 0x260), 8)) { trace("aim: evtable %s no event component", name.c_str()); return; }
  const uint64_t map = *(const uint64_t*)(comp + 0x260);
  if (!readable((void*)map, 0x20)) { trace("aim: evtable %s no table", name.c_str()); return; }
  trace("aim: evtable %s rows=%llu", name.c_str(), (unsigned long long)*(const uint64_t*)(map + 0x18));
  uint64_t node = *(const uint64_t*)(map + 0x10);
  for (int i = 0; i < 400 && readable((void*)node, 0x90); i++, node = *(const uint64_t*)node) {
    const uint64_t row = node + 0x18;
    const std::string m = stdstr_at(row + 0x10), ph = stdstr_at(row + 0x48), mc = stdstr_at(row + 0x28), pc = stdstr_at(row + 0x60);
    if (g_evmis && m.find("Mis_") == std::string::npos && ph.find("Mis_") == std::string::npos) continue;
    trace("aim: evtable %s key=%u motion=%d/%d %s [%s] photo=%d %s [%s]", name.c_str(), *(const uint32_t*)(node + 0x10), *(const int*)row,
          *(const int*)(row + 8), m.empty() ? "-" : m.c_str(), mc.empty() ? "-" : mc.c_str(), *(const int*)(row + 0x40), ph.empty() ? "-" : ph.c_str(),
          pc.empty() ? "-" : pc.c_str());
  }
}

// The game's request check (main+0x500ba0: x0 = request manager, w1 = course id, x2 = one photo subject record, w3 = 1 to mark the request
// complete). It walks the request table ([[x0+8]+0x48 .. +0x50], 0x70 bytes each: +0 index, +4 species, +6 course, +0x13 "any photo"
// flag, +0x50 event name, +0x58 list of other event names) and takes the first request of this course and species whose save state
// (save +0x7ac0, 0x18 bytes per request, byte 1) is 1 or 2 and whose event name equals the record's (+0x38), or one of the others
// equals the record's second name (+0x90). SNAP_AIM_LOG=1 logs every call: what the record carries and each request of that species.
const uint64_t kReqCheck = 0x7100500ba0ull;

void reqcheck(uint64_t x0) {
  static int n;
  Cpu* c = loader::g_tls_cpu;
  if (!c || n >= 600) return;
  const uint64_t rec = c->x[2];
  if (!readable((void*)rec, 0xb0) || !readable((void*)(x0 + 8), 8)) return;
  n++;
  const int course = (int)(uint32_t)c->x[1], species = *(const int*)(rec + 0x24);
  trace("aim: reqcheck course=%d species=%d skip=%d commit=%d event=%s second=%s", course, species, *(const int*)(rec + 0x20), (int)(c->x[3] & 1),
        stdstr_at(rec + 0x38).c_str(), stdstr_at(rec + 0x90).c_str());
  const uint64_t mgr = *(const uint64_t*)(x0 + 8);
  if (!readable((void*)(mgr + 0x48), 16)) return;
  const uint64_t gp = 0x7100cfab68ull;   // the save state pointer the function reads
  const uint64_t save = readable((void*)gp, 8) ? *(const uint64_t*)gp : 0;
  const uint64_t st = save && readable((void*)(save + 0x7ac0), 16) ? *(const uint64_t*)(save + 0x7ac0) : 0;
  for (uint64_t m = *(const uint64_t*)(mgr + 0x48), e = *(const uint64_t*)(mgr + 0x50); m < e && readable((void*)m, 0x70); m += 0x70) {
    if (*(const uint16_t*)(m + 4) != species) continue;
    const int idx = *(const uint16_t*)m - 1;
    const uint64_t ev = *(const uint64_t*)(m + 0x50);
    trace("aim: reqcheck   request %d course=%d any=%d state=%d open=%d event=%s", idx + 1, *(const uint8_t*)(m + 6), *(const uint8_t*)(m + 0x13),
          st && readable((void*)(st + idx * 0x18ull), 2) ? *(const uint8_t*)(st + idx * 0x18ull + 1) : -1,
          save && readable((void*)(save + 0x3e53), 1) ? *(const uint8_t*)(save + 0x3e53) : -1, ev ? cstr_at((const char*)ev).c_str() : "-");
  }
}

extern "C" void snap_aim_observe(uint64_t a, uint64_t x0) {
  if (!g_on) return;
  if (a == kReqCheck) { if (g_census) reqcheck(x0); return; }
  if (a == kEventUpdate) {
    if (g_evtable && readable((void*)(x0 + 0x20), 8)) { std::lock_guard<std::mutex> lk(g_mu); g_evcomp[*(const uint64_t*)(x0 + 0x20)] = x0; }
    return;
  }
  if (a != kCollUpdate) return;
  Cpu* cc = loader::g_tls_cpu;
  const uint64_t x2 = cc ? cc->x[2] : 0;
  if (!readable((void*)(x0 + 0x20), 8) || !readable((void*)(x2 + 0x30), 12)) return;
  const uint64_t own = *(const uint64_t*)(x0 + 0x20);
  if (!own) return;
  const float* t = (const float*)(x2 + 0x30);
  bool fresh = false;
  std::string dump;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Actor& ac = g_act[own];
    fresh = !ac.own;
    ac.own = own;
    ac.pos = {t[0], t[1], t[2]};
    ac.present = snap_present_count();
    if (fresh || ac.present - ac.checked >= 8) {   // the model name arrives late and the pose changes: read them every 8 presents
      ac.checked = ac.present;
      if (ac.name.empty()) ac.name = name_of(own);
      if (g_census && is_pokemon(ac.name)) {
        std::string po = pose_of(own);
        if (po.empty()) po = "-";
        { const std::string act = stdstr_at(own + 0x450); po += " act=" + (act.empty() ? std::string("-") : act); }   // +0x450: the event the motion system is playing
        if (po != ac.pose) {
          ac.pose = po;
          if (g_evtable && (g_evmis || g_evtable[0] == '1' || ac.name.find(g_evtable) != std::string::npos)) dump = ac.name;
          aim::V3 p = ac.pos; p.y += 0.4f;
          const aim::Proj pr = aim::project(g_cam, p);
          if (g_census_n++ < 20000)
            trace("aim: see %s (%llx) pose=%s off=%.2f,%.2f%s dist=%.1f pos=%.1f,%.1f,%.1f present=%llu", ac.name.c_str(), (unsigned long long)own, po.c_str(),
                  pr.x, pr.y, pr.front ? "" : " behind", pr.dist, ac.pos.x, ac.pos.y, ac.pos.z, (unsigned long long)ac.present);
        }
      }
    }
  }
  if (!dump.empty()) evtable(own, dump);
  if (fresh && g_discover) discover(own);
}

// lodanim.cpp h_tanf, at the ride camera builder's tanf call: x20 = camera object, half = the half field of view (radians).
extern "C" void snap_aim_camera(Cpu* c, float half) {
  if (!g_on) return;
  if (((c->x[30] - 0x7100000000ull) & 0xffffffffull) != 0x296400) return;
  const uint64_t obj = c->x[20];
  if (!readable((void*)(obj + 0xd0), 0x30) || !readable((void*)(obj + 0x12c), 4)) return;
  const float* f = (const float*)(obj + 0xd0);
  if (g_discover && snap_present_count() % 10 == 0 && readable((void*)obj, 0x400)) {   // every camera object that comes through here, with its floats
    static uint64_t lastp[8], objs[8];
    int i = 0;
    while (i < 8 && objs[i] && objs[i] != obj) i++;
    if (i < 8 && (objs[i] != obj || lastp[i] != snap_present_count())) {
      objs[i] = obj; lastp[i] = snap_present_count();
      static char b[8000]; int o = 0;
      for (int k = 0; k < 0x400; k += 4) { const float v = *(const float*)(obj + k); if (v != 0 && std::fabs(v) < 1e6f && std::fabs(v) > 1e-6f) o += snprintf(b + o, sizeof b - o, " %x:%.4g", k, v); }
      trace("aim: camobj %d %llx present=%llu%s", i, (unsigned long long)obj, (unsigned long long)lastp[i], b);
    }
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_cam.eye = {f[0], f[1], f[2]};
  g_cam.target = {f[4], f[5], f[6]};
  g_cam.up = {f[8], f[9], f[10]};
  g_cam.half_y = half;
  const float asp = *(const float*)(obj + 0x12c);
  if (asp > 0.5f && asp < 8.0f) g_cam.aspect = asp;
  const uint64_t pr = snap_present_count();
  if (g_discover && pr != g_cam_present && pr % 300 == 0)
    trace("aim: camera eye=%.1f,%.1f,%.1f look=%.2f,%.2f,%.2f half=%.3f aspect=%.3f actors=%zu", g_cam.eye.x, g_cam.eye.y, g_cam.eye.z, g_cam.target.x - g_cam.eye.x,
          g_cam.target.y - g_cam.eye.y, g_cam.target.z - g_cam.eye.z, half, g_cam.aspect, g_act.size());
  g_cam_present = pr;
}

// ---- route interface (runtime/hle/input_core.cpp: the `aim`, `shoot` and `wait_pose` lines) ----
namespace {
std::string g_spec;            // "", "nearest" or a name (part of it is enough, case ignored)
float g_tol = 0.06f, g_range = 0, g_up = 0.4f;
bool g_near;            // near=<x>,<z>: of several Pokemon with the name, take the one closest to this world point
float g_nx, g_nz;
bool g_zoom;                   // zoom=1: hold ZL (the camera zoom) from the moment the target is centered
float g_ix, g_iy;             // integral of the offset: takes out the lag behind a target that keeps moving across the picture
bool g_zlatch;                 // ... which has happened for this lock (tracking a moving target is steadier unzoomed)
uint64_t g_lock;               // the actor the target resolved to
int g_flip_x = 1, g_flip_y = 1;
std::set<std::string> g_done;  // species (CH_xxx) already shot on the reticle: `aim nearest` moves on to another one
// `aim act:<text>`: the target is whichever actor's photo event contains text right now (act:Mis_ = any request event), so one ride can
// photograph every request event that comes up. After g_act_cap photos of one event name that event is done and the aim moves on.
std::map<std::string, int> g_act_hits;
std::map<std::string, uint64_t> g_act_last;
std::set<std::string> g_act_done;
int g_act_cap = 5;
bool act_spec() { return g_spec.compare(0, 4, "act:") == 0 || g_spec.compare(0, 3, "ev:") == 0; }
// The event name of the actor that the sweep spec means, empty = no match. The spec text is a list of alternatives split by '|', each
// `<text>` or `<name part>@<text>` (only actors whose model name holds the name part; an empty text then takes the actor whatever it
// does). The text is looked for in the photo event and, with `ev:`, also in the motion event (a request motion whose table row names no
// photo event). Example: ev:Mis_Stage001_095|Sp_P004_03|CH_099@Sleep_01|CH_089@
std::string sweep_event(uint64_t own, const std::string& name) {
  const std::string a = stdstr_at(own + 0x450), m = g_spec[0] == 'e' ? pose_of(own) : std::string();
  size_t p = g_spec.find(':') + 1;
  while (p <= g_spec.size()) {
    size_t e = g_spec.find('|', p);
    if (e == std::string::npos) e = g_spec.size();
    std::string alt = g_spec.substr(p, e - p);
    p = e + 1;
    const size_t at = alt.find('@');
    if (at != std::string::npos) {
      if (name.find(alt.substr(0, at)) == std::string::npos) continue;
      alt.erase(0, at + 1);
      if (alt.empty()) return name.substr(0, 6);
    } else if (alt.empty()) continue;
    if (!a.empty() && a.find(alt) != std::string::npos) return a;
    if (!m.empty() && m.find(alt) != std::string::npos) return m;
  }
  return {};
}

bool contains_nocase(const std::string& h, const std::string& n) {
  if (n.empty()) return true;
  for (size_t i = 0; i + n.size() <= h.size(); i++) {
    size_t k = 0;
    while (k < n.size() && tolower((unsigned char)h[i + k]) == tolower((unsigned char)n[k])) k++;
    if (k == n.size()) return true;
  }
  return false;
}

// g_mu held. The actor the spec means right now: the locked one while it is still updating, else the closest match in front.
const Actor* resolve(aim::Proj* out) {
  if (g_spec.empty()) return nullptr;
  const uint64_t now = snap_present_count();
  const Actor* best = nullptr;
  aim::Proj bp;
  float bd = 1e30f;
  for (const auto& kv : g_act) {
    const Actor& ac = kv.second;
    if (now - ac.present > 30 || ac.name.empty()) continue;
    if (g_spec == "nearest") { if (!is_pokemon(ac.name) || g_done.count(ac.name.substr(0, 6))) continue; }
    else if (g_spec.compare(0, 5, "pose:") == 0) { if (ac.own != g_lock && pose_of(ac.own).find(g_spec.c_str() + 5) == std::string::npos) continue; }
    else if (act_spec()) {
      const std::string a = sweep_event(ac.own, ac.name);
      if (a.empty() || g_act_done.count(a)) continue;
    }
    else if (!contains_nocase(ac.name, g_spec)) continue;
    aim::V3 p = ac.pos;
    p.y += g_up;
    const aim::Proj pr = aim::project(g_cam, p);
    if (ac.own == g_lock) { *out = pr; return &ac; }
    if (pr.dist < 0.5f || (g_spec == "nearest" && g_range > 0 && pr.dist > g_range)) continue;   // a named target is tracked at any distance; range then only gates "ready"
    const float score = g_near ? std::hypot(ac.pos.x - g_nx, ac.pos.z - g_nz) : pr.dist * (pr.front ? 1.0f : 3.0f);
    if (score < bd) { bd = score; best = &ac; bp = pr; }
  }
  if (best) *out = bp;
  return best;
}
}  // namespace

extern "C" void snap_aim_set(const char* spec) {
  if (!g_on) return;
  std::lock_guard<std::mutex> lk(g_mu);
  std::string s = spec ? spec : "";
  g_tol = 0.06f; g_range = 0; g_up = 0.4f; g_near = false; g_lock = 0; g_zoom = false; g_zlatch = false; g_ix = g_iy = 0;
  size_t sp = s.find(' ');
  std::string opts = sp == std::string::npos ? "" : s.substr(sp);
  if (sp != std::string::npos) s.resize(sp);
  if (const char* t = strstr(opts.c_str(), "tol=")) g_tol = (float)atof(t + 4);
  if (const char* t = strstr(opts.c_str(), "range=")) g_range = (float)atof(t + 6);
  if (const char* t = strstr(opts.c_str(), "up=")) g_up = (float)atof(t + 3);
  if (const char* t = strstr(opts.c_str(), "near=")) { g_near = sscanf(t + 5, "%f,%f", &g_nx, &g_nz) == 2; }
  if (const char* t = strstr(opts.c_str(), "cap=")) g_act_cap = atoi(t + 4);
  if (const char* t = strstr(opts.c_str(), "zoom=")) { g_zoom = t[5] != '0'; g_zlatch = t[5] == '2'; }   // zoom=2: zoomed from the start
  g_spec = s == "off" ? "" : s;
  aim::Proj pr;
  const Actor* ac = resolve(&pr);
  if (ac) g_lock = ac->own;
  if (g_spec.empty()) trace("aim: off");
  else if (ac) trace("aim: target '%s' locked on %s (%llx) off=%.2f,%.2f dist=%.1f present=%llu", g_spec.c_str(), ac->name.c_str(), (unsigned long long)ac->own, pr.x, pr.y, pr.dist, (unsigned long long)snap_present_count());
  else trace("aim: target '%s': no such actor in sight yet (%zu actors known)", g_spec.c_str(), g_act.size());
}

extern "C" int snap_aim_stick(float* rx, float* ry) {
  if (!g_on) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  aim::Proj pr;
  const Actor* ac = resolve(&pr);
  if (!ac) return 0;
  if (!g_lock || (act_spec() && g_lock != ac->own)) {   // an act: target moves on when its event ends
    g_lock = ac->own; g_ix = g_iy = 0;
    trace("aim: target '%s' locked on %s (%llx) off=%.2f,%.2f dist=%.1f act=%s", g_spec.c_str(), ac->name.c_str(), (unsigned long long)ac->own, pr.x, pr.y, pr.dist, sweep_event(ac->own, ac->name).c_str());
  }
  const float zk = aim::zoom_gain(g_cam.half_y);
  const aim::Stick st = aim::steer(pr, g_tol * 0.5f, 2.5f * zk, 0.22f * (zk < 0.5f ? 0.5f : zk));
  if (pr.front) {
    g_ix = std::clamp(g_ix + pr.x * 0.04f * zk, -0.7f, 0.7f);
    g_iy = std::clamp(g_iy + pr.y * 0.04f * zk, -0.7f, 0.7f);
  } else g_ix = g_iy = 0;
  *rx = std::clamp(st.rx + g_ix, -1.0f, 1.0f) * (float)g_flip_x;
  *ry = std::clamp(st.ry + g_iy, -1.0f, 1.0f) * (float)g_flip_y;
  return g_zlatch ? 2 : 1;
}

extern "C" int snap_aim_ready(char* why, int n) {
  if (!g_on) { if (why && n > 0) snprintf(why, (size_t)n, "SNAP_AIM is not set"); return 1; }
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_spec.empty()) { if (why && n > 0) snprintf(why, (size_t)n, "no target"); return 1; }
  aim::Proj pr;
  const Actor* ac = resolve(&pr);
  if (!ac) { if (why && n > 0) snprintf(why, (size_t)n, "target '%s' not in sight", g_spec.c_str()); return 0; }
  if (why && n > 0) snprintf(why, (size_t)n, "%s off=%.2f,%.2f dist=%.1f", ac->name.c_str(), pr.x, pr.y, pr.dist);
  const bool hit = aim::centered(pr, g_zlatch ? g_tol * 3 : g_tol, g_range);   // zoomed in, the same angle is a larger offset
  if (hit && g_zoom) g_zlatch = true;
  return hit ? 1 : 0;
}

// The route condition `act=<text>`: the game's viewfinder has locked onto an actor (the locked target, or any when there is none)
// whose playing event contains text. Only then does a photo carry that event (actor+0x450; +0x418 is the event asked for).
extern "C" int snap_aim_act(const char* text, char* why, int n) {
  if (!g_on) { if (why && n > 0) snprintf(why, (size_t)n, "SNAP_AIM is not set"); return 0; }
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string want = text ? text : "";
  const uint64_t now = snap_present_count();
  for (const auto& kv : g_act) {
    const Actor& ac = kv.second;
    if (now - ac.present > 30 || ac.name.empty()) continue;
    if (act_spec()) {   // sweep target: only an actor on the reticle counts, and each event name only g_act_cap photos
      aim::V3 p = ac.pos; p.y += g_up;
      const aim::Proj pr = aim::project(g_cam, p);
      if (!aim::centered(pr, g_tol > 0.12f ? g_tol : 0.12f, g_range)) continue;   // range=<m>: no photo from farther away (the game rejects a subject it cannot make out)
    } else if (g_lock && ac.own != g_lock) {   // a second actor can stand in for the target (the hidden Pichu pops up as another one): it counts when it is in the picture's middle
      aim::V3 p = ac.pos; p.y += g_up;
      const aim::Proj pr = aim::project(g_cam, p);
      if (!aim::in_window(pr, 0.4f)) continue;
    }
    const std::string a = act_spec() ? sweep_event(ac.own, ac.name) : stdstr_at(ac.own + 0x450);
    if (!a.empty() && a.find(want) != std::string::npos) {
      if (act_spec()) {
        if (g_act_done.count(a)) continue;
        uint64_t& last = g_act_last[a];
        if (now - last >= 4 && ++g_act_hits[a] > g_act_cap) { g_act_done.insert(a); trace("aim: event %s done (%d photos)", a.c_str(), g_act_cap); continue; }   // a gap in the presses = the next photo
        if (now - last >= 4) trace("aim: act photo %s %s photo %d present=%llu", ac.name.c_str(), a.c_str(), g_act_hits[a], (unsigned long long)now);
        last = now;
      }
      if (why && n > 0) snprintf(why, (size_t)n, "%s plays %s", ac.name.c_str(), a.c_str());
      return 1;
    }
  }
  if (why && n > 0) snprintf(why, (size_t)n, "no actor plays '%s'", want.c_str());
  return 0;
}

extern "C" void snap_aim_shot(void) {
  if (!g_on) return;
  std::lock_guard<std::mutex> lk(g_mu);
  aim::Proj pr;
  const Actor* ac = resolve(&pr);
  if (!ac) { trace("aim: shot target=none present=%llu", (unsigned long long)snap_present_count()); return; }
  const bool hit = aim::centered(pr, g_zlatch ? g_tol * 3 : g_tol, g_range);
  std::string po = pose_of(ac->own);
  if (po.empty()) po = "-";
  { const std::string act = stdstr_at(ac->own + 0x450); po += " act=" + (act.empty() ? std::string("-") : act); }
  trace("aim: shot target=%s off=%.2f,%.2f dist=%.1f pose=%s %s fov=%.3f%s present=%llu", ac->name.c_str(), pr.x, pr.y, pr.dist, po.empty() ? "-" : po.c_str(),
        hit ? "centered" : "off-center", g_cam.half_y, g_zlatch ? " zoom" : "", (unsigned long long)snap_present_count());
  if (g_census && getenv("SNAP_AIM_SHOTDUMP")) {   // every EV_ text reachable from the actor at the moment of the shot
    std::set<std::string> seen;
    int budget = 60;
    g_only = "EV_";
    discover_block(ac->own, ac->own, 0x1400, 2, "shot", seen, budget);
    g_only = nullptr;
  }
  if (hit && is_pokemon(ac->name)) g_done.insert(ac->name.substr(0, 6));
}

extern "C" int snap_aim_pose(const char* text, char* why, int n) {
  if (!g_on) { if (why && n > 0) snprintf(why, (size_t)n, "SNAP_AIM is not set"); return 1; }
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string want = text ? text : "";
  const uint64_t now = snap_present_count();
  std::string last;
  for (const auto& kv : g_act) {
    const Actor& ac = kv.second;
    if (now - ac.present > 30 || ac.name.empty()) continue;
    if (g_lock && ac.own != g_lock) continue;
    last = pose_of(ac.own);
    if (!last.empty() && last.find(want) != std::string::npos) { if (why && n > 0) snprintf(why, (size_t)n, "%s pose %s", ac.name.c_str(), last.c_str()); return 1; }
  }
  if (why && n > 0) snprintf(why, (size_t)n, "pose '%s' not seen (last %s)", want.c_str(), last.empty() ? "-" : last.c_str());
  return 0;
}

namespace loader {
void snap_aim_install() {
  const char* e = getenv("SNAP_AIM");
  if (!e || !*e || *e == '0') return;
  g_on = true;
  const char* d = getenv("SNAP_AIM_DISCOVER");
  g_discover = d && *d && *d != '0';
  const char* l = getenv("SNAP_AIM_LOG");
  g_census = l && *l && *l != '0';
  const char* t = getenv("SNAP_AIM_EVTABLE");
  g_evmis = t && strncmp(t, "mis", 3) == 0;
  g_evtable = t && *t && *t != '0' ? t : nullptr;
  g_fnprof = 1;
  fprintf(stderr, "[aim] SNAP_AIM=1: route aim tool on (function hook on: slower run)%s\n", g_discover ? ", discovery log" : "");
}
}  // namespace loader
