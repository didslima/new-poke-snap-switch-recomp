// SPDX-License-Identifier: MIT
// Ultrawide (21:9, 32:9) in the NVN layer. Design, switches and test results: docs/ultrawide.md.
//
// The guest keeps its 1920x1080 world. On a window wider than 16:9 (k = window aspect / (16/9) > 1) every 16:9 render target
// (the scene, its depth, the post chain, the window textures) gets an image k times wider than its render scale alone gives
// (TextureImpl::wide), and host/mods/lodanim.cpp divides the x term of 16:9 camera projections by k (vertical FOV kept). Each
// draw then picks how its guest x maps onto the wider image (ReplayCtx::wideMode):
//   full     3D draws with a widened camera, post passes that sample wide images, full-screen fills: guest x * scale * k
//   narrow   interface draws and 16:9 cameras that were not widened: the middle 16:9 of the image (guest x * scale + offset)
//   unwiden  a widened camera drawing into a 16:9-only target: the viewport is k times wider around
//            its center, which is exactly the original 16:9 projection
// A draw into a target that is not wide (photo, thumbnail, Evaluation depth) that samples a wide image reads its middle 16:9 at
// full resolution (WideCropSubstitute), so photos and scoring see the original framing. A present with no widened 3D draw
// (menus, photo select, movies, cutscene cameras) shows only the middle 16:9 with black bars (WidePresentCrop).
// SNAP_ULTRAWIDE=off (or the host never calling nvnHostSetUltrawide) leaves every path as before. SNAP_WIDE_LOG=1 logs decisions.
#include "nvn_internal.h"
#include "nvn_fx.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace nvn {
float g_wideK = 1.0f;

namespace {
std::atomic<int> g_wideDraws{0};       // widened-camera draws into wide targets since the last present
int g_lastFrameWide = -1;              // last present's decision (-1 before the first)
int g_wideHold = 0;                    // presents left to stay wide after the last widened draw
// Photo select and the album draw their 3D backdrop on every other present: a present stays wide while a widened draw happened in
// the last kHold presents, so the bars do not flicker.
constexpr int kHold = 4;
uint64_t g_stats[6] = {};              // full, narrow, unwiden, crop copies, wide presents, narrow presents

bool NoC3Patch() { static const bool v = getenv("SNAP_WIDE_NO_C3") != nullptr; return v; }   // developer aid: 16:9 effect cameras stay narrow
int LogLevel() { static const int v = [] { const char* e = getenv("SNAP_WIDE_LOG"); return e ? std::max(1, atoi(e)) : 0; }(); return v; }
bool IsDepthFmt(int f) { return f == FMT_STENCIL8 || (f >= FMT_DEPTH16 && f <= FMT_DEPTH32F_STENCIL8); }
// 16:9 within 4% (1920x1080, 960x540, 192x108, 1728x972 and their mip chains down to ~16 px)
bool Is169(int w, int h) { if (w < 16 || h < 9) return false; const double a = (double)w / h; return std::fabs(a / (16.0 / 9.0) - 1.0) < 0.04; }
TextureImpl* FirstRt(const ReplayCtx& cx) { for (int i = 0; i < 9; i++) if (cx.rt[i]) return cx.rt[i]; return nullptr; }
// Camera bank c3 (PerViewCBuffer): projection at +0x80, column-major. 0 = no perspective camera, 1 = 16:9, 2 = widened by k, 3 = other.
// The photo Evaluation's 3:2 camera is built from the 16:9 projection (snap_wide_eval_copy, host/mods/lodanim.cpp), so it is 3.
int CameraKind(const uint8_t* c3, uint64_t range) {
  if (!c3 || range < 0xC0) return 0;
  float f[16]; memcpy(f, c3 + 0x80, 64);
  const float m00 = f[0], m11 = f[5], m32 = f[11], m33 = f[15];
  if (!(std::fabs(m32) > 0.9f && std::fabs(m32) < 1.1f && std::fabs(m33) < 1e-3f && std::fabs(m00) > 1e-4f && std::fabs(m11) > 1e-4f)) return 0;
  const double a = std::fabs((double)m11 / m00), base = 16.0 / 9.0;
  if (std::fabs(a / base - 1.0) < 0.02) return 1;
  if (g_wideK > 1.0f && std::fabs(a / (base * g_wideK) - 1.0) < 0.02) return 2;
  return 3;
}
}  // namespace

float WideFor(const TextureBuilder& b, uint32_t tag) {
  if (g_wideK <= 1.0f) return 1.0f;
  if (b.target != TT_2D || b.samples > 1 || !Is169(b.width, b.height)) return 1.0f;
  if (b.flags & TF_DISPLAY) return g_wideK;                 // window textures: the picture the player sees
  if (!(b.flags & TF_COMPRESSIBLE) || (b.flags & (TF_LINEAR | TF_LINEAR_RENDER_TARGET))) return 1.0f;   // render targets only
  if (tag) for (uint32_t t : g_scale.exemptTags) if (t == tag) return 1.0f;   // the Evaluation depth target keeps 1920x1080
  for (const ScalePolicy::Exempt& e : g_scale.exempt)
    if ((e.w < 0 || e.w == b.width) && (e.h < 0 || e.h == b.height) && (e.fmt < 0 || e.fmt == b.format) && (!e.depthOnly || IsDepthFmt(b.format))) return 1.0f;
  return g_wideK;
}

float WideXform(const ReplayCtx& cx, const float* vpGuest, float* sx, float* ox) {
  const TextureImpl* t = FirstRt(cx);
  const float s = t ? t->scale : 1.0f;
  *sx = s; *ox = 0.0f;
  if (!t || g_wideK <= 1.0f) return s;
  if (t->wide > 1.0f) {
    if (cx.wideMode == 1) *ox = ((float)t->HW() - (float)t->b.width * s) * 0.5f;
    else *sx = (float)t->HW() / (float)t->b.width;
  } else if (cx.wideMode == 2 && vpGuest) {   // viewport only (a scissor keeps the 16:9 clip): x' = k x - (k - 1) c, c = viewport center
    const float c = vpGuest[0] + vpGuest[2] * 0.5f;
    *sx = s * g_wideK; *ox = -(g_wideK - 1.0f) * c * s;
  }
  return s;
}

bool WideOnDraw(ReplayCtx& cx, TextureImpl* const* sampled, int nSampled, const uint8_t* c3, uint64_t c3range, const float* box, bool c3Only) {
  cx.wideVfSide = 0;
  if (g_wideK <= 1.0f) return false;
  TextureImpl* t = FirstRt(cx);
  if (!t) return false;
  bool patch = false;
  const int cam = cx.rt[8] ? CameraKind(c3, c3range) : 0;
  int mode = 0;
  const char* why = "";
  if (t->wide > 1.0f) {
    bool sampWide = false, sampBig = false;
    for (int i = 0; i < nSampled; i++) if (sampled[i]) { if (sampled[i]->wide > 1.0f) sampWide = true; if (sampled[i]->b.width > 64 || sampled[i]->b.height > 64) sampBig = true; }
    bool fullScreen = false, screenPart = false;
    if (box) {
      const bool ndc = std::fabs(box[0]) <= 1.5f && std::fabs(box[1]) <= 1.5f && std::fabs(box[2]) <= 1.5f && std::fabs(box[3]) <= 1.5f;
      const float x0 = ndc ? -1.0f : 0.0f, y0 = x0, x1 = ndc ? 1.0f : (float)t->b.width, y1 = ndc ? 1.0f : (float)t->b.height;
      const float ex = (x1 - x0) * 0.001f, ey = (y1 - y0) * 0.001f, cx0 = (x0 + x1) * 0.5f, cy0 = (y0 + y1) * 0.5f;
      fullScreen = box[0] <= x0 + ex && box[2] >= x1 - ex && box[1] <= y0 + ey && box[3] >= y1 - ey;
      // a quarter or half of the screen reaching from a side edge to the center lines: a piece of a mirrored full-screen mask
      const bool spanX = (box[0] <= x0 + ex && box[2] >= cx0 - ex) || (box[0] <= cx0 + ex && box[2] >= x1 - ex);
      const bool spanY = (box[1] <= y0 + ey && box[3] >= cy0 - ey) || (box[1] <= cy0 + ey && box[3] >= y1 - ey);
      screenPart = !fullScreen && spanX && spanY;
      // Photo-mode viewfinder mask: 4 mirrored quarters meeting at (cx0, cy0) sampling a half-res (960x540) texture into the display target.
      // Keep it narrow (16:9) so the viewfinder opening frames the 16:9/3:2 photo capture area rather than stretching to ~21:9.
      const bool quarterAligned = (std::fabs(box[0] - cx0) <= ex || std::fabs(box[2] - cx0) <= ex) &&
                                  (std::fabs(box[1] - cy0) <= ey || std::fabs(box[3] - cy0) <= ey);
      const bool viewfinderMask = screenPart && !ndc && (t->b.flags & TF_DISPLAY) && quarterAligned &&
                                  nSampled == 1 && sampled[0] && sampled[0]->b.width * 2 == t->b.width && sampled[0]->b.height * 2 == t->b.height &&
                                  cx.wideFs && std::strcmp(cx.wideFs, "0b6e3cad047c") == 0;
      if (viewfinderMask) {
        screenPart = false;
        cx.wideVfSide = (box[0] + box[2] < cx0 * 2.0f) ? 1 : 2;
        std::memcpy(cx.wideVfBox, box, sizeof(cx.wideVfBox));
      }
    }
    if (cam == 2) { mode = 0; why = "widened camera"; g_wideDraws.fetch_add(1, std::memory_order_relaxed); }
    else if (cam == 1 && c3Only && c3range >= 0x180 && !NoC3Patch() && box) { mode = 0; patch = true; why = "16:9 camera, c3 projection widened"; }   // effects, particles
    else if (cam == 1) { mode = 1; why = "16:9 camera"; }
    else if (sampWide) { mode = 0; why = "samples a wide image"; }
    else if (fullScreen && !sampBig) { mode = 0; why = "full-screen fill"; }
    else if ((fullScreen || screenPart) && (g_lastFrameWide == 1 || g_wideDraws.load(std::memory_order_relaxed) > 0)) {   // dimming, vignettes, wipes
      mode = 0; why = fullScreen ? "full-screen overlay on a wide view" : "screen-quarter overlay on a wide view";
    }
    else { mode = 1; why = cam == 3 ? "other camera" : "interface"; }
  } else if (cam == 2) { mode = 2; why = "widened camera into a 16:9 target"; }
  g_stats[mode]++;
  // SNAP_WIDE_LOG=2: each distinct decision once (target, rule, box, sampled sizes), except widened-camera draws (3: those too),
  // from present SNAP_WIDE_LOG_FROM, at most 4000 lines
  static const int logFrom = getenv("SNAP_WIDE_LOG_FROM") ? atoi(getenv("SNAP_WIDE_LOG_FROM")) : 0;
  if (LogLevel() >= 2 && g_presentCount >= logFrom && (t->wide > 1.0f || mode == 2) && (cam != 2 || LogLevel() >= 3)) {
    static int n = 0;
    static std::unordered_set<uint64_t> seen;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    if (cx.wideFs) for (const char* q = cx.wideFs; *q; q++) mix((uint64_t)(uint8_t)*q);
    mix((uint64_t)t->b.width << 32 | (uint32_t)t->b.height); mix((uint64_t)t->b.format << 8 | (uint64_t)mode); mix((uint64_t)(uintptr_t)why); mix((uint64_t)cam);
    if (box) for (int i = 0; i < 4; i++) mix((uint64_t)(int64_t)std::lround(box[i] * 4.0f));
    for (int i = 0; i < nSampled; i++) if (sampled[i]) mix((uint64_t)sampled[i]->b.width << 32 | (uint32_t)sampled[i]->b.height);
    if (seen.insert(h).second && n++ < 4000) {
      fprintf(stderr, "[wide] p%d draw into %dx%d f%x%s: %s (%s) cam=%d box=%s%.1f,%.1f..%.1f,%.1f samp", g_presentCount, t->b.width, t->b.height, t->b.format,
              (t->b.flags & TF_DISPLAY) ? " display" : "", mode == 0 ? "full" : mode == 1 ? "narrow" : "unwiden", why, cam, box ? "" : "none ",
              box ? box[0] : 0.f, box ? box[1] : 0.f, box ? box[2] : 0.f, box ? box[3] : 0.f);
      for (int i = 0; i < nSampled && i < 3; i++) if (sampled[i]) fprintf(stderr, " %dx%d", sampled[i]->b.width, sampled[i]->b.height);
      fprintf(stderr, " fs=%s\n", cx.wideFs ? cx.wideFs : "-");
    }
  }
  if (mode != cx.wideMode) { cx.wideMode = (uint8_t)mode; FxReemitViewState(cx); }
  return patch;
}

void WideDrawViewfinderSide(ReplayCtx& cx, void (*emit)(void*), void* user) {
  if (cx.wideVfSide == 0 || g_wideK <= 1.0f || !(cx.vpHave & 1)) return;
  const TextureImpl* t = FirstRt(cx);
  if (!t || t->wide <= 1.0f) return;
  const float s = t->scale;
  const int hw = (int)t->HW(), hh = (int)t->HH();
  const int midX0 = (int)std::lround(((float)hw - (float)t->b.width * s) * 0.5f);
  const int midX1 = midX0 + (int)std::lround((float)t->b.width * s);
  const int sideX0 = (cx.wideVfSide == 1) ? 0 : std::min(hw, midX1);
  const int sideX1 = (cx.wideVfSide == 1) ? std::max(0, midX0) : hw;
  if (sideX1 <= sideX0) return;

  int sy = 0, sh = hh;
  if (cx.scHave & 1) {
    int y = cx.scGuest[0][1], ht = cx.scGuest[0][3];
    if (cx.hud.on) {
      const int y1 = (int)std::ceil(cx.hud.r * (y + ht) + cx.hud.t);
      y = (int)std::floor(cx.hud.r * y + cx.hud.t);
      ht = y1 - y;
    }
    if (y < 0) { ht += y; y = 0; }
    if (ht < 0) ht = 0;
    if (s != 1.0f) { y = (int)std::floor(y * s); ht = (int)std::ceil(ht * s); }
    sy = std::clamp(y, 0, hh);
    sh = std::clamp(ht, 0, hh - sy);
  }
  if (sh <= 0) return;

  float vh[4];
  std::memcpy(vh, cx.vpGuest[0], sizeof(vh));
  if (cx.hud.on) {
    vh[0] = cx.hud.p * vh[0] + cx.hud.q;
    vh[1] = cx.hud.r * vh[1] + cx.hud.t;
    vh[2] *= cx.hud.p;
    vh[3] *= cx.hud.r;
  }
  if (vh[2] <= 0.0f || vh[3] <= 0.0f) return;

  const float boxL = cx.hud.on ? cx.hud.p * cx.wideVfBox[0] + cx.hud.q : cx.wideVfBox[0];
  const float boxR = cx.hud.on ? cx.hud.p * cx.wideVfBox[2] + cx.hud.q : cx.wideVfBox[2];
  const float wBorder = std::max(1.0f, (boxR - boxL) * 0.03f);
  const float limVp = cx.h->props.limits.maxViewportDimensions[0] > 0
      ? std::min<float>((float)cx.h->props.limits.maxViewportDimensions[0], 16384.0f)
      : 8192.0f;
  const float vpW = std::max(vh[2] * s, limVp);
  const int tileW = std::max(1, (int)std::floor((wBorder / vh[2]) * vpW));

  for (int tx0 = sideX0; tx0 < sideX1; tx0 += tileW) {
    const int tx1 = std::min(sideX1, tx0 + tileW);
    VkViewport vp = MakeNvnViewport(cx.h->originMode, vh[0], vh[1], vh[2], vh[3], s);
    vp.width = vpW;
    vp.x = (cx.wideVfSide == 1)
        ? (float)tx0 - ((boxL - vh[0]) / vh[2]) * vpW
        : (float)tx1 - ((boxR - vh[0]) / vh[2]) * vpW;
    VkRect2D sc{{tx0, sy}, {(uint32_t)(tx1 - tx0), (uint32_t)sh}};
    vkCmdSetViewport(cx.cb, 0, 1, &vp);
    vkCmdSetScissor(cx.cb, 0, 1, &sc);
    emit(user);
  }
  FxReemitViewState(cx);
}

VkBuffer WideC3Copy(ReplayCtx& cx, const uint8_t* c3, uint64_t range) {
  // one copy per recording (ReplayCtx) and camera content: the draws of one view share it
  range = std::min<uint64_t>(range, 0x400);
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < range; i += 8) { uint64_t v; memcpy(&v, c3 + i, std::min<uint64_t>(8, range - i)); h = (h ^ v) * 1099511628211ull; }
  h ^= range;
  for (int i = 0; i < 8; i++) if (cx.wideC3Buf[i] && cx.wideC3Hash[i] == h) return cx.wideC3Buf[i];
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* m = NewScratch(cx, range, &buf, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  if (!m) return VK_NULL_HANDLE;
  memcpy(m, c3, range);
  float f[16];
  const float k = g_wideK;
  for (uint64_t off : {0x80ull, 0x100ull}) {   // projection, viewProjection: clip x / k (row 0 of the column-major matrix)
    memcpy(f, m + off, 64);
    for (int c = 0; c < 4; c++) f[c * 4] /= k;
    memcpy(m + off, f, 64);
  }
  for (uint64_t off : {0xc0ull, 0x140ull}) {   // their inverses: column 0 times k
    if (off + 64 > range) continue;
    memcpy(f, m + off, 64);
    for (int r = 0; r < 4; r++) f[r] *= k;
    memcpy(m + off, f, 64);
  }
  cx.wideC3Hash[cx.wideC3Next] = h; cx.wideC3Buf[cx.wideC3Next] = buf;
  cx.wideC3Next = (uint8_t)((cx.wideC3Next + 1) % 8);
  return buf;
}

TextureImpl* WideCropSubstitute(ReplayCtx& cx, TextureImpl* src) {
  if (g_wideK <= 1.0f || !src || src->wide <= 1.0f) return src;
  const TextureImpl* t = FirstRt(cx);
  if (!t || t->wide > 1.0f) return src;   // wide targets sample wide images as they are
  for (int i = 0; i < 9; i++) if (cx.rt[i] == src) return src;   // bound as a target too: no copy inside its own pass
  if (!src->EnsureImage()) return src;
  TextureImpl* tw = src->wideTwin;
  const uint32_t gw = (uint32_t)std::max(1, src->b.width);
  const uint32_t cw = std::min(src->HW(), (uint32_t)std::lround((double)gw * src->scale));
  if (!tw) {
    tw = new TextureImpl();
    tw->host = src->host; tw->b = src->b; tw->fi = src->fi; tw->scale = src->scale; tw->wide = 1.0f;
    tw->hostW = cw; tw->hostH = src->HH();
    if (!tw->EnsureImage()) { delete tw; return src; }
    src->wideTwin = tw;
    src->wideDirty = true;
    if (LogLevel()) fprintf(stderr, "[wide] crop stand-in for %dx%d f%x (image %ux%u -> %ux%u) sampled into %dx%d f%x\n", src->b.width, src->b.height, src->b.format,
                            src->HW(), src->HH(), cw, tw->HH(), t->b.width, t->b.height, t->b.format);
  }
  if (src->wideDirty) {   // the source was drawn to since the last copy (or never copied)
    EndRendering(cx);
    const uint32_t levels = std::min(src->imgLevels, tw->imgLevels), layers = std::min(src->imgLayers, tw->imgLayers);
    Transition(cx.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cx.cb, tw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy rg[16]; uint32_t n = 0;
    for (uint32_t l = 0; l < levels && n < 16; l++) {
      const uint32_t w = std::max(1u, tw->HW() >> l), h = std::max(1u, tw->HH() >> l), sw = std::max(1u, src->HW() >> l);
      VkImageCopy& c = rg[n++];
      c = VkImageCopy{};
      c.srcSubresource = {src->fi->aspect, l, 0, layers};
      c.dstSubresource = {tw->fi->aspect, l, 0, layers};
      c.srcOffset = {(int32_t)((sw - std::min(sw, w)) / 2), 0, 0};
      c.extent = {std::min(w, sw), h, 1};
    }
    vkCmdCopyImage(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tw->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, rg);
    tw->uploaded = true;
    src->wideDirty = false;
    g_stats[3]++;
    if (LogLevel() >= 2) fprintf(stderr, "[wide] p%d crop copy %dx%d f%x for a draw into %dx%d f%x\n", g_presentCount, src->b.width, src->b.height, src->b.format, t->b.width, t->b.height, t->b.format);
  }
  return tw;
}

void WideMarkDrawn(TextureImpl* t) { if (t && t->wide > 1.0f) t->wideDirty = true; }

bool WidePresentCrop(const TextureImpl* src, int* sx, int* sw) {
  if (g_wideK <= 1.0f || !src || src->wide <= 1.0f) return false;
  if (g_wideDraws.exchange(0, std::memory_order_relaxed) > 0) g_wideHold = kHold;
  else if (g_wideHold > 0) g_wideHold--;
  const bool wide = g_wideHold > 0;
  g_stats[wide ? 4 : 5]++;
  if (LogLevel() && wide != (g_lastFrameWide == 1)) fprintf(stderr, "[wide] p%d present: %s\n", g_presentCount, wide ? "wide (3D view widened)" : "16:9 in the middle (no widened camera)");
  g_lastFrameWide = wide ? 1 : 0;
  if (LogLevel() && (g_presentCount % 3600) == 0)
    fprintf(stderr, "[wide] p%d stats: draws full %llu narrow %llu unwiden %llu, crop copies %llu, presents wide %llu 16:9 %llu\n", g_presentCount,
            (unsigned long long)g_stats[0], (unsigned long long)g_stats[1], (unsigned long long)g_stats[2], (unsigned long long)g_stats[3], (unsigned long long)g_stats[4], (unsigned long long)g_stats[5]);
  if (wide) return false;
  const int cw = (int)std::lround((double)src->b.width * src->scale);
  if (cw <= 0 || cw >= (int)src->HW()) return false;
  *sx = ((int)src->HW() - cw) / 2; *sw = cw;
  return true;
}
}  // namespace nvn

NVN_HOST_EXPORT void nvnHostSetUltrawide(float k) {
  nvn::g_wideK = (k > 1.01f && k <= 4.0f) ? k : 1.0f;
  fprintf(stderr, "[wide] ultrawide factor %.4f (%s)\n", nvn::g_wideK, nvn::g_wideK > 1.0f ? "3D view widened, interface and photos 16:9" : "off");
}
NVN_HOST_EXPORT float nvnHostGetUltrawide(void) { return nvn::g_wideK; }
