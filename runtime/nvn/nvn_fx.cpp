// SPDX-License-Identifier: MIT
// Fx plumbing: what every host-side screen-space effect (contact shadows, water SSR, ...) needs, done once (project code, ours).
//
// Frame model (independent of the upscaler, which only tracks frames while SNAP_UPSCALER is on):
//   phase 0  before the scene: waiting for a bind of the 1920x1080 (x render scale) DEPTH32F scene target
//   phase 1  scene being drawn: the camera (PerViewCBuffer c3, first scene draw) and the dynamic shadow caster (slot 9 of the ShadowCast draws) are captured,
//            the scene colour (RGBA16F drawn with that depth) is found
//   phase 2  the window texture was bound after the scene: the PRE-TONEMAP slot ran (see below); until the present
// Pre-tonemap slot: the upscaler reads the scene colour when the window texture is bound (UpscaleOnSetRenderTargets -> Resolve), not in the tonemap draw, and
//   UpscaleSubstitute then swaps the tonemap's input. An effect therefore has to be recorded right BEFORE that call (nvn_vk.cpp, SetRenderTargets), where no
//   rendering scope is open, so it needs no EndRendering / re-BeginRendering dance. Post-tonemap slot: FxAfterDraw, after the tonemap draw (AaAfterDraw's place).
// Photo guard: a submit whose lists draw into the Evaluation depth target (DEPTH32F_STENCIL8, 1920 wide) is a photo frame: that present and the next two
//   stay native (no Fx). Those are the photo-select and scoring passes, which render the pictures the game keeps. A photo taken on a ride is only a
//   360x240 pass that downscales the display two presents after the press (the present before it, which had the effects on anyway), so since 2026-11-19
//   it holds nothing: the old hold only made the effects drop out for 3 presents per photo (SNAP_FX_PHOTO_HOLD=1 brings it back). Fx work is never
//   recorded while an occlusion query scope is open (Evaluation counts visible pixels in one).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvn_fx.h"
#include "nvn_fx_spv.h"
#include "nvn_aa.h"
#include "nvn_internal.h"
#include "upscale/upscale_nvn.h"
#include "../hle/input_core.h"

extern "C" int snap_route_cond(const char* c, char* why, int n);   // runtime/hle/input_core.cpp: a route `wait` condition, read now

namespace nvn {
extern int g_fdBurst;   // nvn_vk.cpp: NVN_FRAME_DUMP burst request
namespace {
constexpr uint32_t kCamBytes = 0x250;
constexpr uint32_t kOffProj = 0x80, kOffViewProj = 0x100, kOffCamPos = 0x240;

std::vector<const FxEffect*>& Effects() { static std::vector<const FxEffect*> v; return v; }

struct State {
  // (effects live in Effects(): registration runs from static initializers of other files)
  int phase = 0;
  bool frameHold = false, submitPhoto = false;
  int hold = 0;
  TextureImpl* depth = nullptr;
  TextureImpl* color = nullptr;
  FxCam cam;
  FxCaster caster;
  bool ranFrame = false;
  bool midTried = false, midRan = false; int sceneDraws = 0, prevDraws = 0;
  int castSeen = -100;     // present of the last caster capture (first cast draw per present)
  int activePresent = -1; bool active = false;
  uint64_t frames = 0, runs = 0, holds = 0, photoSubmits = 0;
} F;

bool EnvOn(const char* n, bool def) { const char* e = getenv(n); return e && *e ? e[0] != '0' : def; }
bool Log() { static const bool on = EnvOn("SNAP_FX_LOG", false); return on; }
int AbP() { static const int p = [] { const char* e = getenv("SNAP_FX_AB"); return e ? std::max(0, atoi(e)) : 0; }(); return p; }
bool Split() { static const bool on = EnvOn("SNAP_FX_SPLIT", false); return on; }
bool MasterOff() { static const bool off = [] { const char* e = getenv("SNAP_FX"); return e && e[0] == '0'; }(); return off; }

bool IsDepthFmt(int f) { return f == FMT_STENCIL8 || (f >= FMT_DEPTH16 && f <= FMT_DEPTH32F_STENCIL8); }
bool IsSceneDepth(const TextureImpl* t) {
  return t && t->b.format == FMT_DEPTH32F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && t->b.samples <= 1;
}
bool IsSceneColor(const TextureImpl* t) {
  return t && t->b.format == FMT_RGBA16F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && !(t->b.flags & TF_DISPLAY);
}
bool IsDisplay(const TextureImpl* t) { return t && (t->b.flags & TF_DISPLAY); }
bool IsEvalDepth(const TextureImpl* t) { return t && t->b.format == FMT_DEPTH32F_STENCIL8 && t->b.width == 1920; }
bool IsPhotoColor(const TextureImpl* t) { return t && t->b.width == 360 && t->b.height == 240; }

// SNAP_FX_TIME=1: GPU time of the whole pre-tonemap block (timestamps around it, read back a few blocks later; logged every 300 blocks)
struct Timing {
  bool init = false, on = false; VkQueryPool pool = VK_NULL_HANDLE; float period = 1.f;
  static constexpr int kSlots = 16;
  bool pending[kSlots] = {}; int issued[kSlots] = {}; int next = 0;
  double sumMs = 0, maxMs = 0; unsigned long long n = 0;
} T;
int TimeBegin(ReplayCtx& cx) {
  if (!T.init) {
    T.init = true; T.on = EnvOn("SNAP_FX_TIME", false);
    if (T.on) {
      VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(cx.h->phys, &pr); T.period = pr.limits.timestampPeriod;
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = Timing::kSlots * 2;
      if (pr.limits.timestampComputeAndGraphics != VK_TRUE || vkCreateQueryPool(cx.h->device, &qi, nullptr, &T.pool) != VK_SUCCESS) T.on = false;
    }
  }
  if (!T.on) return -1;
  for (int i = 0; i < Timing::kSlots; i++) {   // harvest finished slots
    if (!T.pending[i] || g_presentCount - T.issued[i] < 3) continue;   // only slots whose command buffer was submitted a few presents ago
    uint64_t v[4] = {};
    if (vkGetQueryPoolResults(cx.h->device, T.pool, (uint32_t)i * 2, 2, sizeof v, v, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS) continue;
    if (!v[1] || !v[3]) continue;
    T.pending[i] = false;
    const double ms = (double)(v[2] - v[0]) * T.period * 1e-6;
    T.sumMs += ms; T.maxMs = std::max(T.maxMs, ms);
    if (++T.n % 300 == 0) { fprintf(stderr, "fx: GPU time of the pre-tonemap block: avg %.3f ms, max %.3f ms over %llu frames\n", T.sumMs / (double)T.n, T.maxMs, T.n); }
  }
  const int s = T.next++ % Timing::kSlots;
  if (T.pending[s]) return -1;   // not read back yet: skip this frame's sample
  vkCmdResetQueryPool(cx.cb, T.pool, (uint32_t)s * 2, 2);
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, (uint32_t)s * 2);
  return s;
}
void TimeEnd(ReplayCtx& cx, int s) {
  if (s < 0) return;
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, (uint32_t)s * 2 + 1);
  T.pending[s] = true; T.issued[s] = g_presentCount;
}

bool Active() {
  if (F.activePresent == g_presentCount) return F.active;
  F.activePresent = g_presentCount;
  F.active = false;
  if (!MasterOff()) for (const FxEffect* e : Effects()) if (e->enabled && e->enabled()) { F.active = true; break; }
  return F.active;
}

// 4x4 helpers (row-major doubles)
using M4 = double[4][4];
bool Invert(const M4 a, M4 r) {
  double m[4][8];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; c++) {
    int p = c; for (int i = c + 1; i < 4; i++) if (std::fabs(m[i][c]) > std::fabs(m[p][c])) p = i;
    if (std::fabs(m[p][c]) < 1e-30) return false;
    if (p != c) for (int j = 0; j < 8; j++) std::swap(m[p][j], m[c][j]);
    const double d = 1.0 / m[c][c];
    for (int j = 0; j < 8; j++) m[c][j] *= d;
    for (int i = 0; i < 4; i++) if (i != c) { const double f = m[i][c]; if (f != 0) for (int j = 0; j < 8; j++) m[i][j] -= f * m[c][j]; }
  }
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i][j] = m[i][j + 4];
  return true;
}

void TakeCamera(const uint8_t* p) {
  float f[16], g[16];
  memcpy(f, p + kOffViewProj, 64); memcpy(g, p + kOffProj, 64);
  FxCam c;
  for (int i = 0; i < 16; i++) if (!std::isfinite(f[i]) || !std::isfinite(g[i])) return;
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) c.vp[r][k] = f[k * 4 + r];
  double P[4][4]; for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) P[r][k] = g[k * 4 + r];
  float e[3]; memcpy(e, p + kOffCamPos, 12);
  for (int i = 0; i < 3; i++) c.eye[i] = e[i];
  double n = 0;
  for (int i = 0; i < 3; i++) { c.fwd[i] = -c.vp[3][i]; n += c.fwd[i] * c.fwd[i]; }
  n = std::sqrt(n); if (n > 0) for (double& v : c.fwd) v /= n;
  if (!(std::fabs(P[3][2]) > 0.5) || std::fabs(P[1][1]) < 1e-6 || !Invert(c.vp, c.ivp)) return;   // not a perspective scene camera
  const double a = P[2][2], b = P[2][3];
  const double d0 = std::fabs(a) > 1e-12 ? b / a : INFINITY, d1 = std::fabs(1.0 + a) > 1e-12 ? b / (1.0 + a) : INFINITY;
  const double dn = std::min(std::fabs(d0), std::fabs(d1)), df = std::max(std::fabs(d0), std::fabs(d1));
  c.invDepth = std::fabs(d1) < std::fabs(d0);
  c.nearZ = (float)dn; c.infFar = !std::isfinite(df) || df > 1e7; c.farZ = c.infFar ? 1e7f : (float)df;
  c.fovY = (float)(2.0 * std::atan(1.0 / std::fabs(P[1][1])));
  c.valid = true;
  F.cam = c;
  if (Log()) { static int n2 = 0; if (n2++ < 3) fprintf(stderr, "fx: p%d camera eye (%.1f %.1f %.1f) near %.3f %s fovY %.1f\n", g_presentCount, c.eye[0], c.eye[1], c.eye[2], c.nearZ, c.infFar ? "reversed infinite z" : "finite z", c.fovY * 57.29578f); }
}

void TakeCaster(const uint8_t* p) {
  float m[16]; memcpy(m, p, 64);
  for (float v : m) if (!std::isfinite(v)) return;
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return;
  float a[3] = {m[2], m[6], m[10]};
  const float n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (!(n > 1e-6f)) return;
  FxCaster c; memcpy(c.m, m, 64);
  for (int i = 0; i < 3; i++) c.axis[i] = a[i] / n;
  c.present = g_presentCount; c.valid = true;
  F.caster = c;
}
}  // namespace

void FxRegister(const FxEffect* e) { if (e) Effects().push_back(e); }

bool FxActive() { return Active(); }
const FxCam* FxCamera() { return F.cam.valid ? &F.cam : nullptr; }
TextureImpl* FxSceneDepth() { return F.phase ? F.depth : nullptr; }
TextureImpl* FxSceneColor() { return F.phase ? F.color : nullptr; }
bool FxIsPhotoFrame() { return F.frameHold || F.submitPhoto; }
void FxJitter(float* jx, float* jy) { UpscaleSceneJitter(jx, jy); }
const FxCaster* FxCasterNow() { return F.caster.valid && g_presentCount - F.caster.present <= 30 ? &F.caster : nullptr; }
bool FxAbSkip() { const int p = AbP(); return p > 0 && ((g_presentCount / p) & 1) == 0; }
uint32_t FxSplitX(uint32_t w) { return Split() ? w / 2 : 0; }
bool FxIsTonemapKey(const char* k) { return AaIsTonemapKey(k); }
bool FxWritable(const TextureImpl* t) {
  if (t && !IsDepthFmt(t->b.format) && !IsEvalDepth(t) && !IsPhotoColor(t)) return true;
  static int n; if (n++ < 4) fprintf(stderr, "fx: refused to write target %p (depth format, Evaluation depth or 360x240 photo target)\n", (const void*)t);
  return false;
}

bool FxOnSetRenderTargets(ReplayCtx& cx) {
  if (!Active()) return false;
  TextureImpl* d = cx.rt[8]; TextureImpl* c0 = cx.rt[0];
  if (d && IsSceneDepth(d)) {
    if (F.phase == 0) {
      F.phase = 1; F.depth = d; F.color = nullptr; F.cam.valid = false; F.midTried = F.midRan = false; F.sceneDraws = 0;
      F.frameHold = F.submitPhoto || F.hold > 0 || FxAbSkip();
      F.frames++; if (F.frameHold) F.holds++;
    }
    if (F.phase == 1 && d == F.depth && IsSceneColor(c0)) F.color = c0;
    return false;
  }
  if (F.phase != 1 || !IsDisplay(c0)) return false;
  F.phase = 2;
  if (F.frameHold || F.submitPhoto || cx.occOpen || cx.rendering || !F.color || !F.depth || !F.cam.valid) {
    if (Log()) { static int n; if (n++ < 20) fprintf(stderr, "fx: p%d pre-tonemap skipped (hold %d photo %d occ %d color %p cam %d)\n", g_presentCount, (int)F.frameHold, (int)F.submitPhoto, (int)cx.occOpen, (void*)F.color, (int)F.cam.valid); }
    return false;
  }
  if (!F.color->EnsureImage() || !F.depth->EnsureImage()) return false;
  bool rec = false;
  const int tsSlot = TimeBegin(cx);
  for (const FxEffect* e : Effects()) if (e->preTonemap && !(e->opaqueOnly && F.midRan) && e->enabled && e->enabled()) { e->preTonemap(cx); rec = true; F.runs++; }
  TimeEnd(cx, tsSlot);
  if (Log() && rec) { static int n; if (n++ < 6) fprintf(stderr, "fx: p%d pre-tonemap effects ran on %ux%u\n", g_presentCount, F.color->HW(), F.color->HH()); }
  return rec;
}

void FxOnDraw(ReplayCtx& cx, const uint8_t* const* vsUbo, const uint64_t* vsRange, const uint8_t* slot9, uint64_t slot9Range) {
  if (!Active()) return;
  TextureImpl* d = cx.rt[8];
  if (!d) return;
  if (F.phase == 1 && d == F.depth) {
    if (!F.cam.valid && vsUbo[0] && vsRange[0] >= kCamBytes) TakeCamera(vsUbo[0]);
    F.sceneDraws++;
    return;
  }
  // ShadowCast: a square depth-only target of a shadow-map size, the 64-byte caster matrix in slot 9
  if (slot9 && slot9Range == 64 && !cx.rt[0] && IsDepthFmt(d->b.format) && d->b.width == d->b.height && d->b.width >= 512 && d->b.width <= 8192 && F.castSeen != g_presentCount) {
    TakeCaster(slot9);
    if (F.caster.valid && F.caster.present == g_presentCount) {
      F.castSeen = g_presentCount; F.caster.mapSize = d->b.width;
      if (Log()) { static int n; if (n++ < 6) fprintf(stderr, "fx: p%d caster %ux%u axis (%.3f %.3f %.3f)\n", g_presentCount, d->b.width, d->b.height, F.caster.axis[0], F.caster.axis[1], F.caster.axis[2]); }
    }
  }
}

bool FxMidPass(ReplayCtx& cx, bool blend, bool depthWrite) {
  if (F.phase != 1 || F.midTried || !blend || depthWrite || !Active()) return false;
  if (cx.rt[8] != F.depth || !F.color || cx.rt[0] != F.color || F.sceneDraws < std::max(30, F.prevDraws * 6 / 10)) return false;   // not yet the opaque part's end
  F.midTried = true;
  if (F.frameHold || F.submitPhoto || cx.occOpen || !F.cam.valid || !cx.rendering) return false;
  bool any = false;
  for (const FxEffect* e : Effects()) if (e->opaqueOnly && e->preTonemap && e->enabled && e->enabled()) any = true;
  if (!any || !F.color->EnsureImage() || !F.depth->EnsureImage()) return false;
  EndRendering(cx);
  const int tsSlot = TimeBegin(cx);
  for (const FxEffect* e : Effects()) if (e->opaqueOnly && e->preTonemap && e->enabled && e->enabled()) { e->preTonemap(cx); F.runs++; }
  TimeEnd(cx, tsSlot);
  F.midRan = true;
  if (Log()) { static int n; if (n++ < 6) fprintf(stderr, "fx: p%d mid-pass effects ran after %d scene draws (previous frame %d)\n", g_presentCount, F.sceneDraws, F.prevDraws); }
  return true;
}

void FxAfterDraw(ReplayCtx& cx, const char* key) {
  if (!Active() || FxIsPhotoFrame() || cx.occOpen || !FxIsTonemapKey(key)) return;
  for (const FxEffect* e : Effects()) if (e->postTonemap && e->enabled && e->enabled()) e->postTonemap(cx, key);
}

// Submits that held a photo pass (Evaluation or 360x240), whether or not the effects were held for it: host\ab_capture.ps1 reads it to know a photo was taken
static std::atomic<uint64_t> g_photoPasses{0};
uint64_t FxPhotoPassCount() { return g_photoPasses.load(std::memory_order_relaxed); }

// SNAP_FX_PHOTO_HOLD: which photo passes switch the effects off. 0 (default): only the Evaluation passes. 1: a 360x240 photo pass too, and the two
// presents after it (the behaviour before 2026-11-19: the effects dropped out for 3 presents at every photo of a ride).
int PhotoHoldMode() { static const int m = [] { const char* e = getenv("SNAP_FX_PHOTO_HOLD"); return e && *e ? atoi(e) : 0; }(); return m; }

void FxBeforeSubmit(int n, CmdList* const* lists) {
  if (!Active()) return;
  bool eval = false, thumb = false;
  char seen[256] = ""; size_t sl = 0;   // SNAP_FX_LOG: the targets this submit draws into
  for (int k = 0; k < n && !(eval && thumb && !Log()); k++) {
    if (!lists[k]) continue;
    int bound = 0; bool noted = true;
    const TextureImpl* c0 = nullptr; const TextureImpl* d = nullptr;
    for (const Cmd& c : lists[k]->cmds) {
      if (c.op == Op::SetRenderTargets) {
        bound = IsEvalDepth(c.tex[8]) ? 1 : IsPhotoColor(c.tex[0]) ? 2 : 0;
        c0 = c.tex[0]; d = c.tex[8]; noted = false;
      } else if (c.op == Op::Draw || c.op == Op::DrawIndexed || c.op == Op::DrawIndirect || c.op == Op::DrawIndexedIndirect) {
        if (bound == 1) eval = true; else if (bound == 2) thumb = true;
        if (!noted && Log() && sl + 40 < sizeof seen) {
          noted = true;
          char one[40]; snprintf(one, sizeof one, " %ux%u/f%d+d%d", c0 ? c0->b.width : 0, c0 ? c0->b.height : 0, c0 ? (int)c0->b.format : 0, d ? (int)d->b.format : 0);
          if (!strstr(seen, one)) { memcpy(seen + sl, one, strlen(one) + 1); sl += strlen(one); }
        }
        if (eval && thumb && !Log()) break;
      }
    }
  }
  const bool photo = eval || (thumb && PhotoHoldMode() >= 1);
  if (Log() && (eval || thumb)) fprintf(stderr, "fx: p%d photo submit (%s%s%s): %s; draws into%s\n", g_presentCount, eval ? "Evaluation depth" : "", eval && thumb ? " + " : "",
                                        thumb ? "360x240" : "", photo ? "Fx held" : "Fx stays on", seen);
  if (eval || thumb) g_photoPasses++;
  F.submitPhoto = photo;
  if (!photo) return;
  F.photoSubmits++;
  F.hold = std::max(F.hold, 2);
  if (F.phase == 1) F.frameHold = true;
}

// Counters for host\ab_capture.ps1 (docs/ab-capture.md): effect passes that drew, and how many of those drew on a held frame (FxIsPhotoFrame; a gating bug; must stay 0)
static std::atomic<uint64_t> g_ranTotal{0}, g_ranOnPhoto{0};
uint64_t FxRanCount() { return g_ranTotal.load(std::memory_order_relaxed); }
uint64_t FxRanOnPhotoCount() { return g_ranOnPhoto.load(std::memory_order_relaxed); }
void FxNoteRan() { F.ranFrame = true; g_ranTotal++; if (FxIsPhotoFrame()) g_ranOnPhoto++; }

void FxOnPresent() {
  if (F.phase && AbP() > 0 && Log()) fprintf(stderr, "fx: AB p%d scene %s\n", g_presentCount, F.frameHold ? "OFF (held)" : (F.ranFrame ? "ON (effect drew)" : "ON (eligible, effect skipped)"));   // g_presentCount is already the number the frame dump uses
  F.ranFrame = false;
  if (!Active() && F.phase == 0 && !F.hold) return;
  if (F.phase) F.prevDraws = F.sceneDraws;
  F.phase = 0; F.depth = nullptr; F.color = nullptr; F.cam.valid = false;
  if (F.hold > 0 && !F.submitPhoto) F.hold--;
  // SNAP_FX_SHUTTER_DUMP=<n> (test aid, needs NVN_FRAME_DUMP): every present of the n after a press of A is dumped, for the first 4 presses on a course ride that are
  // at least 240 presents apart, so a run shows whether the picture changes while a photo is taken. SNAP_FX_LOG names the photo passes by present.
  static const int dumpN = [] { const char* e = getenv("SNAP_FX_SHUTTER_DUMP"); return e ? std::max(0, atoi(e)) : 0; }();
  if (dumpN > 0) {
    static uint64_t lastBtn = 0; static int left = 0, presses = 0, lastPress = -100000;
    const uint64_t btn = hle::input::hub().pad().buttons;
    if (((btn & ~lastBtn) >> hle::input::kA & 1) && left == 0 && presses < 4 && g_presentCount - lastPress > 240 && snap_route_cond("ride>=1", nullptr, 0)) {
      left = dumpN; presses++; lastPress = g_presentCount;
      fprintf(stderr, "fx: p%d A pressed: dumping %d presents\n", g_presentCount, dumpN);
    }
    lastBtn = btn;
    if (left > 0) { g_fdBurst = 4; left--; }
  }
  // No hold on button presses: the photo pass renders into its own 360x240 / Evaluation targets (guarded by the submit look-ahead above), and a hold
  // on every A press made the effect drop out for six presents each time the player advanced a dialog line.
}

// ---- Vulkan helpers ---------------------------------------------------------------------------------------------------
VkShaderModule FxMakeModule(Host* h, const uint32_t* words, size_t bytes) {
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = bytes; mi.pCode = words;
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(h->device, &mi, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
  return m;
}

VkShaderModule FxFullscreenVs(Host* h) {
  static VkShaderModule m = VK_NULL_HANDLE;
  if (!m) m = FxMakeModule(h, kFxFullscreenVert, sizeof kFxFullscreenVert);
  return m;
}

VkPipeline FxMakeFullscreenPipeline(Host* h, VkPipelineLayout pl, VkShaderModule fs, VkFormat fmt, bool multiply) {
  VkShaderModule vs = FxFullscreenVs(h);
  if (!vs || !fs) return VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  if (multiply) {   // rgb = dst * src, alpha untouched
    cba.blendEnable = VK_TRUE; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
  } else cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
  gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = pl;
  VkPipeline p = VK_NULL_HANDLE;
  const VkResult r = vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &p);
  if (r != VK_SUCCESS) { fprintf(stderr, "fx: vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
  return p;
}

VkDescriptorSet FxAllocSet(ReplayCtx& cx, VkDescriptorSetLayout layout) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (cx.pools->empty() || attempt == 1) cx.pools->push_back(AcquireDescriptorPool(cx.q));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cx.pools->back(); ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(cx.h->device, &ai, &set) == VK_SUCCESS) return set;
  }
  return VK_NULL_HANDLE;
}

void FxDrawFullscreen(ReplayCtx& cx, TextureImpl* t, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set, const void* pc, uint32_t pcBytes) {
  if (!FxWritable(t) || !t->EnsureImage() || !pipe || !set) return;
  VkCommandBuffer cb = cx.cb;
  Transition(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  SubRange rr;
  VkImageView v = t->RenderView(nullptr, &rr);
  const uint32_t w = t->LevelW(rr.lb), h = t->LevelH(rr.lb);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = v; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  if (g_perfPassOn) { char lb[96]; snprintf(lb, sizeof lb, "fx pass %ux%u", w, h); PerfMark(cx.h, cb, lb); g_perfDraws++; }
  vkCmdBeginRendering(cb, &ri);
  const uint32_t x0 = FxSplitX(w);
  VkViewport vp{0, 0, (float)w, (float)h, 0, 1}; VkRect2D sc{{(int32_t)x0, 0}, {w - x0, h}};
  vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
  if (pc && pcBytes) vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcBytes, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // as EndRendering: the pass' writes are visible to everything after
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}

VkImageView FxDepthRead(ReplayCtx& cx, TextureImpl* z, VkImageLayout* prev) {
  *prev = z->subLayout.empty() ? z->layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  Transition(cx.cb, z, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  return z->View(nullptr, true);
}

void FxDepthCopyRows(ReplayCtx& cx, TextureImpl* z, VkBuffer dst, uint32_t rowStep, uint32_t rows) {
  const VkImageLayout orig = z->subLayout.empty() ? z->layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  Transition(cx.cb, z, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  std::vector<VkBufferImageCopy> rg(rows);
  const uint32_t w = z->HW();
  for (uint32_t i = 0; i < rows; i++) {
    VkBufferImageCopy& r = rg[i]; r = {};
    r.bufferOffset = (VkDeviceSize)i * w * 4; r.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    r.imageOffset = {0, (int32_t)(i * rowStep), 0}; r.imageExtent = {w, 1, 1};
  }
  vkCmdCopyImageToBuffer(cx.cb, z->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, rows, rg.data());
  Transition(cx.cb, z, orig);
}

void FxDepthDone(ReplayCtx& cx, TextureImpl* z, VkImageLayout prev) {
  if (prev != VK_IMAGE_LAYOUT_UNDEFINED && prev != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) Transition(cx.cb, z, prev);
}
}  // namespace nvn
