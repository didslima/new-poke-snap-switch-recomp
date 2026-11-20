// SPDX-License-Identifier: MIT
// Lens effects (project code, ours): bokeh depth of field while zoomed, and motion blur. Two Fx pre-tonemap effects (nvn_fx.cpp) in one file; both are DISPLAY ONLY.
//   They draw into the linear HDR scene colour before the upscaler and the tonemap read it, on the 1920x1080 (x render scale) scene frame. The Fx plumbing never
//   runs a pre-tonemap effect on a photo frame (the Evaluation passes, the 360x240 photo passes, and the presents a photo reads), and FxWritable refuses the
//   Evaluation depth and photo targets, so the saved photo and everything photo scoring reads stay as the game made them: sharp, no blur. See docs/lens-effects.md.
//
//   SNAP_BOKEH_DOF=<strength>  Bokeh depth of field while the camera is zoomed (0 / unset = off; 1 = Medium; the menu row offers 0.6, 1, 1.6).
//       Zoom = tan(18 deg) / tan(half-FOV), from the projection matrix of the scene camera (the ride camera builder main+0x295170 / tanf hook
//       main+0x296400 sets it): the ride camera is 36 deg vertical, ZL zooms it to 14.44 deg (2.56x), the lab and fly-overs are wider. Off below 1.05x. The focus distance is the median depth under the
//       reticle (the image centre), smoothed in diopters. Blur: a thin-lens CoC, a half-to-eighth resolution disk gather (round bokeh), a CoC-aware upsample.
//   SNAP_MOTION_BLUR=<strength>  Motion blur (0 / unset = off; 1 = a 1/120 s shutter; the menu row offers 0.5, 1, 2). Velocity = this frame's view-projection against the
//       previous one, applied to the scene depth (the same reprojection as the upscaler's motion vectors, upscale_mv.comp, so it works without an upscaler). It is camera
//       motion on the depth buffer: a Pokemon's own motion is not in it.
//   Test aids: SNAP_LIGHT_AB=<P> (same-moment A/B: the effects are on in odd blocks of P presents, off in even ones), SNAP_BOKEH_FORCE=<zoom> (treat the camera as zoomed by
//   that factor), SNAP_BOKEH_DEBUG=1 (CoC tint and a cyan reticle dot), SNAP_MBLUR_DEBUG=1 (velocity tint), SNAP_BOKEH_BASE_FOV=<deg> (fixed unzoomed vertical FOV),
//   SNAP_BOKEH_K (CoC strength, default 120), SNAP_BOKEH_MAXCOC (max CoC in pixels at 1080p, default from the zoom), SNAP_LENS_LOG=1.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/lighting_preset.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_lens_spv.h"

namespace nvn {
namespace {
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; uint32_t w = 0, h = 0; };
struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule fs[5] = {};   // focus, prep, blur, comp, mblur
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  VkPipeline pFocus = VK_NULL_HANDLE, pPrep = VK_NULL_HANDLE, pBlur = VK_NULL_HANDLE, pComp = VK_NULL_HANDLE, pMb = VK_NULL_HANDLE; VkFormat sceneFmt = VK_FORMAT_UNDEFINED;
  Img src, prep, blur, focus[2]; uint32_t rw = 0, rh = 0, ds = 2; int fcur = 0, focusLast = -1000;
} g;

float EnvF(const char* n, float def) { const char* e = getenv(n); return e && *e ? (float)atof(e) : def; }
float Strength(const char* name) {
  const char* e = getenv(name);
  if (!e || !*e || nvn::os_host::ascii_ieq(e, "off")) return 0.f;
  if (nvn::os_host::ascii_ieq(e, "on")) return 1.f;
  const float v = (float)atof(e);
  return v > 0.f ? std::min(v, 4.f) : 0.f;
}
float DofStrength() { return SNAP_LIVE(float, Strength("SNAP_BOKEH_DOF")); }       // live rows (hle/live.h)
float MblurStrength() { return SNAP_LIVE(float, Strength("SNAP_MOTION_BLUR")); }
bool LogOn() { static const bool on = getenv("SNAP_LENS_LOG") != nullptr; return on; }

struct PcFocus { float dp[4], sz[4], ms[4]; };
struct PcPrep { float dp[4], sz[4], cc[4], ms[4]; };
struct PcBlur { float sz[4]; };
struct PcMb { float r[16]; float sz[4], dp[4], mp[4]; };
static_assert(sizeof(PcMb) <= 128 && sizeof(PcPrep) <= 128, "push constants must fit the 128 byte guarantee");

void ImgBar(VkCommandBuffer cb, Img& im, VkImageLayout to) {
  if (im.layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = im.layout; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  im.layout = to;
}
bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt, VkImageUsageFlags extra) {
  if (o.img || o.view || o.mem) { RetireVkImage(h, o.img, o.view, o.mem); o = Img{}; }   // a size change: the old image goes once the GPU is past it
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  o = Img{}; o.w = w; o.h = hh;
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}

bool Setup(Host* h, VkFormat sceneFmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(h->device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 4; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    g.fs[0] = FxMakeModule(h, kLensFocusFrag, sizeof kLensFocusFrag);
    g.fs[1] = FxMakeModule(h, kLensPrepFrag, sizeof kLensPrepFrag);
    g.fs[2] = FxMakeModule(h, kLensBlurFrag, sizeof kLensBlurFrag);
    g.fs[3] = FxMakeModule(h, kLensCompFrag, sizeof kLensCompFrag);
    g.fs[4] = FxMakeModule(h, kLensMblurFrag, sizeof kLensMblurFrag);
    for (VkShaderModule m : g.fs) if (!m) return false;
    g.pFocus = FxMakeFullscreenPipeline(h, g.pl, g.fs[0], VK_FORMAT_R32G32B32A32_SFLOAT, false);
    if (!g.pFocus) return false;
    g.ready = true;
  }
  if (g.sceneFmt != sceneFmt || !g.pComp) {   // the scene colour format is fixed (RGBA16F), so this runs once
    g.pPrep = FxMakeFullscreenPipeline(h, g.pl, g.fs[1], sceneFmt, false);
    g.pBlur = FxMakeFullscreenPipeline(h, g.pl, g.fs[2], sceneFmt, false);
    g.pComp = FxMakeFullscreenPipeline(h, g.pl, g.fs[3], sceneFmt, false);
    g.pMb = FxMakeFullscreenPipeline(h, g.pl, g.fs[4], sceneFmt, false);
    g.sceneFmt = sceneFmt;
  }
  return g.pPrep && g.pBlur && g.pComp && g.pMb;
}

bool EnsureImages(Host* h, uint32_t w, uint32_t hh, VkFormat fmt) {
  if (g.rw == w && g.rh == hh && g.src.img) return true;
  if (g.src.img) fprintf(stderr, "lens: render size changed to %ux%u\n", w, hh);   // a command buffer may still read them
  g.ds = (uint32_t)std::max(2, (int)std::lround((double)hh / 540.0));
  const uint32_t lw = (w + g.ds - 1) / g.ds, lh = (hh + g.ds - 1) / g.ds;
  if (!MakeImg(h, g.src, w, hh, fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !MakeImg(h, g.prep, lw, lh, fmt, 0) || !MakeImg(h, g.blur, lw, lh, fmt, 0) ||
      !MakeImg(h, g.focus[0], 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0) || !MakeImg(h, g.focus[1], 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0)) return false;
  g.rw = w; g.rh = hh; g.fcur = 0; g.focusLast = -1000;
  return true;
}

// One full-screen pass into one of our own images (whole image, contents not needed), then a barrier to SHADER_READ_ONLY.
void DrawTo(ReplayCtx& cx, Img& t, VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcBytes) {
  VkCommandBuffer cb = cx.cb;
  ImgBar(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = t.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {t.w, t.h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cb, &ri);
  VkViewport vp{0, 0, (float)t.w, (float)t.h, 0, 1}; VkRect2D sc{{0, 0}, {t.w, t.h}};
  vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcBytes, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  ImgBar(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// All four bindings get a valid view (the shader only reads the ones it uses). Binding 0 is the scene depth, the others are images of ours.
struct Binds { VkImageView depth; VkImageView b1, b2, b3; VkSampler s1, s2; };
VkDescriptorSet MakeSet(ReplayCtx& cx, const Binds& b) {
  VkDescriptorSet set = FxAllocSet(cx, g.dsl);
  if (!set) return VK_NULL_HANDLE;
  const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo ii[4] = {{g.nearest, b.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {b.s1, b.b1, ro}, {b.s2, b.b2, ro}, {g.nearest, b.b3, ro}};
  VkWriteDescriptorSet w[4];
  for (uint32_t i = 0; i < 4; i++) { w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i]; }
  vkUpdateDescriptorSets(cx.h->device, 4, w, 0, nullptr);
  return set;
}

// The scene colour is copied once per effect run: the passes read the copy and write the scene colour itself.
void CopyScene(ReplayCtx& cx, TextureImpl* c) {
  VkCommandBuffer cb = cx.cb;
  ImgBar(cb, g.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Transition(cb, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkImageCopy r{}; r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.extent = {g.rw, g.rh, 1};
  vkCmdCopyImage(cb, c->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  ImgBar(cb, g.src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// SNAP_LENS_TIME=1: GPU time of each effect alone (timestamps around its passes, read back a few presents later, averaged over the frames it drew, logged every 300 runs).
struct Tm {
  const char* name; bool init = false, on = false; VkQueryPool pool = VK_NULL_HANDLE; float period = 1.f;
  static constexpr int kSlots = 8;
  bool pending[kSlots] = {}; int issued[kSlots] = {}; int next = 0;
  double sum = 0, mx = 0; unsigned long long n = 0;
};
Tm g_tm[2] = {{"bokeh"}, {"motion blur"}};
int TmBegin(ReplayCtx& cx, Tm& t) {
  if (!t.init) {
    t.init = true; const char* e = getenv("SNAP_LENS_TIME"); t.on = e && *e && *e != '0';
    if (t.on) {
      VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(cx.h->phys, &pr); t.period = pr.limits.timestampPeriod;
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = Tm::kSlots * 2;
      if (pr.limits.timestampComputeAndGraphics != VK_TRUE || vkCreateQueryPool(cx.h->device, &qi, nullptr, &t.pool) != VK_SUCCESS) t.on = false;
    }
  }
  if (!t.on) return -1;
  for (int i = 0; i < Tm::kSlots; i++) {
    if (!t.pending[i] || g_presentCount - t.issued[i] < 3) continue;
    uint64_t v[4] = {};
    if (vkGetQueryPoolResults(cx.h->device, t.pool, (uint32_t)i * 2, 2, sizeof v, v, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS) continue;
    if (!v[1] || !v[3]) continue;
    t.pending[i] = false;
    const double ms = (double)(v[2] - v[0]) * t.period * 1e-6;
    t.sum += ms; t.mx = std::max(t.mx, ms);
    if (++t.n % 300 == 0) fprintf(stderr, "lens: GPU time of %s: avg %.3f ms, max %.3f ms over %llu runs\n", t.name, t.sum / (double)t.n, t.mx, t.n);
  }
  const int s = t.next++ % Tm::kSlots;
  if (t.pending[s]) return -1;
  vkCmdResetQueryPool(cx.cb, t.pool, (uint32_t)s * 2, 2);
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, t.pool, (uint32_t)s * 2);
  return s;
}
void TmEnd(ReplayCtx& cx, Tm& t, int s) {
  if (s < 0) return;
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, t.pool, (uint32_t)s * 2 + 1);
  t.pending[s] = true; t.issued[s] = g_presentCount;
}

// ---- per scene frame state (shared by both effects) -----------------------------------------------------------------------------------------------------------
struct Track {
  int present = -1000;           // present of the last TrackFrame
  bool have = false;             // prev* hold the previous scene frame
  int prevPresent = -1000;
  double prevVp[4][4] = {}, prevEye[3] = {}, prevFwd[3] = {};
  float baseTan = 0.f;           // tan(half vertical FOV) of the unzoomed camera
  float tanNow = 0.f, zoom = 1.f;
  double R[4][4] = {};           // previous view-projection * inverse of this frame's (identity when there is no usable previous frame)
  bool rValid = false;
  uint64_t lastNs = 0; double dt = 1.0 / 60.0;   // smoothed seconds between scene frames
  int frameNo = 0;
} T;

void TrackFrame(const FxCam* cam) {
  if (T.present == g_presentCount) return;
  const int gap = g_presentCount - T.prevPresent;
  T.present = g_presentCount;
  // seconds between scene frames (the blur length follows the real frame time, so the look stays the same at 60, 120 or 144 fps)
  const uint64_t nowNs = nvn::os_host::monotonic_ns();
  if (T.lastNs) { const double d = (double)(nowNs - T.lastNs) * 1e-9; if (d > 0.0005 && d < 0.1) T.dt += (d - T.dt) * 0.25; }
  T.lastNs = nowNs;
  bool cut = false;
  if (T.have && gap <= 3) {
    double d2 = 0, dot = 0;
    for (int i = 0; i < 3; i++) { const double d = cam->eye[i] - T.prevEye[i]; d2 += d * d; dot += cam->fwd[i] * T.prevFwd[i]; }
    cut = std::sqrt(d2) > 5.0 || dot < std::cos(30.0 * 3.14159265358979 / 180.0);
  }
  // zoom: measured on a Florio ride (SNAP_LENS_LOG), the unzoomed ride camera is 36 degrees vertical and ZL zooms it to 14.44 degrees (2.56x). The fly-overs and the lab
  // use wider fields (42 to 55 degrees), so a fixed 36 degree reference is the unzoomed view: wider fields read as "not zoomed" (zoom below 1).
  T.tanNow = (float)std::tan(cam->fovY * 0.5);
  static const float baseDeg = []() { const char* e = getenv("SNAP_BOKEH_BASE_FOV"); const float d = e ? (float)atof(e) : 36.f; return d > 1.f && d < 170.f ? d : 36.f; }();
  T.baseTan = (float)std::tan(baseDeg * 3.14159265358979 / 360.0);
  T.zoom = T.baseTan / std::max(T.tanNow, 1e-4f);
  if (const char* e = getenv("SNAP_BOKEH_FORCE")) { const float z = (float)atof(e); if (z > 1.f) T.zoom = z; }
  // reprojection matrix
  T.rValid = T.have && gap <= 3 && !cut;
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) {
    if (!T.rValid) { T.R[r][k] = r == k ? 1.0 : 0.0; continue; }
    double s = 0; for (int j = 0; j < 4; j++) s += T.prevVp[r][j] * cam->ivp[j][k];
    T.R[r][k] = s;
  }
  memcpy(T.prevVp, cam->vp, sizeof T.prevVp); memcpy(T.prevEye, cam->eye, sizeof T.prevEye); memcpy(T.prevFwd, cam->fwd, sizeof T.prevFwd);
  T.have = true; T.prevPresent = g_presentCount; T.frameNo++;
  if (LogOn() && T.frameNo % 120 == 1) fprintf(stderr, "lens: p%d fovY %.2f deg base tan %.4f zoom %.2fx dt %.2f ms cut %d\n", g_presentCount, cam->fovY * 57.29578f, T.baseTan, T.zoom, T.dt * 1000.0, (int)cut);
}

float DepthMode(const FxCam* cam) { return cam->invDepth ? (cam->infFar ? 0.f : 1.f) : 2.f; }
float Smooth(float a, float b, float x) { const float t = std::min(std::max((x - a) / (b - a), 0.f), 1.f); return t * t * (3.f - 2.f * t); }

// ---- bokeh depth of field -------------------------------------------------------------------------------------------------------------------------------------
bool DofEnabled() { return DofStrength() > 0.f && !g.failed; }

// Runtime check of the display-only rule: the Fx plumbing must never call a pre-tonemap effect on a photo frame. Counted, logged once, and the effect is skipped anyway.
bool PhotoGuard(const char* who) {
  if (!FxIsPhotoFrame()) return false;
  static int n; if (n++ == 0) fprintf(stderr, "lens: %s was called on a photo frame (p%d): skipped\n", who, g_presentCount);
  return true;
}

void PreDof(ReplayCtx& cx) {
  if (g.failed || PhotoGuard("bokeh")) return;
  const FxCam* cam = FxCamera();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  if (!cam || !c || !z || c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  TrackFrame(cam);
  const float strength = DofStrength();
  if (T.zoom < 1.05f) return;   // not zoomed: nothing is recorded
  if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH(), c->fi->vk)) { g.failed = true; fprintf(stderr, "lens: setup failed, disabled\n"); return; }
  const float H = (float)g.rh, hs = H / 1080.f;
  const float amount = Smooth(1.05f, 1.35f, T.zoom);
  const float K = EnvF("SNAP_BOKEH_K", 120.f) * (T.zoom - 1.f) * strength * amount * hs;                       // pixels * metres at the render size
  const float maxCoc = EnvF("SNAP_BOKEH_MAXCOC", std::min(7.f + 6.f * (T.zoom - 1.f), 26.f) * std::sqrt(std::min(strength, 1.6f))) * hs;
  float jx, jy; FxJitter(&jx, &jy);

  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  Img& fout = g.focus[g.fcur]; Img& fold = g.focus[g.fcur ^ 1];
  Binds bd{dv, g.src.view, g.src.view, fold.view, g.linear, g.linear};
  VkDescriptorSet sFocus = MakeSet(cx, bd);
  if (dv && sFocus) {
    if (fold.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, fold, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // never written: given a defined layout (snap below ignores its contents)
    if (g.src.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, g.src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bindings the focus pass does not read still need a valid layout
    const bool abOff = !snaplight::ab_on((unsigned long long)g_presentCount);   // test aid: the focus keeps tracking, nothing is drawn in the "off" blocks
    const int tmS = abOff ? -1 : TmBegin(cx, g_tm[0]);
    // 1. focus distance under the reticle
    PcFocus pf{};
    pf.dp[0] = cam->nearZ; pf.dp[1] = cam->farZ; pf.dp[2] = DepthMode(cam); pf.dp[3] = H;
    pf.sz[2] = (float)g.rw; pf.sz[3] = (float)g.rh;
    pf.ms[0] = (float)T.dt; pf.ms[1] = EnvF("SNAP_BOKEH_FOCUS_RATE", 6.f); pf.ms[2] = g_presentCount - g.focusLast > 6 ? 1.f : 0.f;
    DrawTo(cx, fout, g.pFocus, sFocus, &pf, sizeof pf);
    g.focusLast = g_presentCount;
    if (!abOff) {
      CopyScene(cx, c);
      // 2. prep: colour and CoC at 1/ds
      const uint32_t lw = g.prep.w, lh = g.prep.h;
      PcPrep pp{};
      pp.dp[0] = cam->nearZ; pp.dp[1] = cam->farZ; pp.dp[2] = DepthMode(cam); pp.dp[3] = H;
      pp.sz[0] = (float)lw; pp.sz[1] = (float)lh; pp.sz[2] = (float)g.rw; pp.sz[3] = (float)g.rh;
      pp.cc[0] = K; pp.cc[1] = maxCoc; pp.cc[2] = EnvF("SNAP_BOKEH_DEADZONE", 0.08f); pp.cc[3] = 0.5f;
      pp.ms[0] = (float)g.ds;
      Binds b2{dv, g.src.view, g.src.view, fout.view, g.linear, g.linear};
      VkDescriptorSet s2 = MakeSet(cx, b2);
      // 3. blur: the disk gather
      PcBlur pb{};
      pb.sz[0] = (float)lw; pb.sz[1] = (float)lh; pb.sz[2] = (float)g.ds; pb.sz[3] = (jx != 0.f || jy != 0.f) ? (float)(1 + (g_presentCount & 255)) : 0.f;
      Binds b3{dv, g.src.view, g.prep.view, fout.view, g.linear, g.linear};
      VkDescriptorSet s3 = MakeSet(cx, b3);
      // 4. composite into the scene colour
      PcPrep pc = pp;
      pc.ms[1] = EnvF("SNAP_BOKEH_DEBUG", 0.f);
      Binds b4{dv, g.src.view, g.blur.view, fout.view, g.nearest, g.nearest};
      VkDescriptorSet s4 = MakeSet(cx, b4);
      if (s2 && s3 && s4) {
        DrawTo(cx, g.prep, g.pPrep, s2, &pp, sizeof pp);
        DrawTo(cx, g.blur, g.pBlur, s3, &pb, sizeof pb);
        FxDrawFullscreen(cx, c, g.pComp, g.pl, s4, &pc, sizeof pc);
        FxNoteRan();
        TmEnd(cx, g_tm[0], tmS);
        static int logged; if (LogOn() && logged++ < 4) fprintf(stderr, "lens: p%d bokeh ran: %ux%u, low %ux%u (ds %u), zoom %.2f, K %.1f, max CoC %.1f px\n", g_presentCount, g.rw, g.rh, lw, lh, g.ds, T.zoom, K, maxCoc);
      }
    }
    g.fcur ^= 1;
  }
  FxDepthDone(cx, z, prev);
}

// ---- motion blur ----------------------------------------------------------------------------------------------------------------------------------------------
bool MbEnabled() { return MblurStrength() > 0.f && !g.failed; }

void PreMb(ReplayCtx& cx) {
  if (g.failed || PhotoGuard("motion blur")) return;
  const FxCam* cam = FxCamera();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  if (!cam || !c || !z || c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  TrackFrame(cam);
  if (!T.rValid && !getenv("SNAP_MBLUR_DEBUG")) return;   // first frame, a gap or a camera cut: nothing to blur along
  if (!snaplight::ab_on((unsigned long long)g_presentCount)) return;   // test aid: the "off" blocks of SNAP_LIGHT_AB
  if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH(), c->fi->vk)) { g.failed = true; fprintf(stderr, "lens: setup failed, disabled\n"); return; }
  const float H = (float)g.rh, hs = H / 1080.f;
  float jx, jy; FxJitter(&jx, &jy);
  PcMb pm{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pm.r[r * 4 + k] = (float)T.R[r][k];
  pm.sz[0] = (float)g.rw; pm.sz[1] = (float)g.rh; pm.sz[2] = jx; pm.sz[3] = jy;
  pm.dp[0] = cam->nearZ; pm.dp[1] = cam->farZ; pm.dp[2] = DepthMode(cam); pm.dp[3] = (jx != 0.f || jy != 0.f) ? (float)(1 + (g_presentCount & 255)) : 0.f;
  const float shutter = EnvF("SNAP_MBLUR_SHUTTER", 1.f / 120.f);
  pm.mp[0] = MblurStrength() * shutter / (float)T.dt;      // velocity (one frame of motion) -> motion over the shutter time
  pm.mp[1] = EnvF("SNAP_MBLUR_MAXLEN", 0.07f) * H;            // longest blur in render pixels
  pm.mp[2] = 0.6f * hs;                                      // shorter than this: untouched
  pm.mp[3] = EnvF("SNAP_MBLUR_DEBUG", 0.f);
  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  Binds bd{dv, g.src.view, g.src.view, g.src.view, g.linear, g.linear};
  VkDescriptorSet s = MakeSet(cx, bd);
  if (dv && s) {
    const int tmS = TmBegin(cx, g_tm[1]);
    CopyScene(cx, c);
    FxDrawFullscreen(cx, c, g.pMb, g.pl, s, &pm, sizeof pm);
    FxNoteRan();
    TmEnd(cx, g_tm[1], tmS);
    static int logged; if (LogOn() && logged++ < 4) fprintf(stderr, "lens: p%d motion blur ran: %ux%u, velocity scale %.2f, max %.0f px, dt %.2f ms\n", g_presentCount, g.rw, g.rh, pm.mp[0], pm.mp[1], T.dt * 1000.0);
  }
  FxDepthDone(cx, z, prev);
}

const FxEffect kDof = {"bokeh_dof", DofEnabled, PreDof, nullptr, false};
const FxEffect kMb = {"motion_blur", MbEnabled, PreMb, nullptr, false};
struct Reg { Reg() { FxRegister(&kDof); FxRegister(&kMb); } } reg;
}  // namespace
}  // namespace nvn
