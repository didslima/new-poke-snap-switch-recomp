// SPDX-License-Identifier: MIT
// "Enhanced lighting" screen-space pass (project code, ours): GTAO ambient occlusion plus denoised contact shadows as one Fx pre-tonemap effect.
//   SNAP_LIGHTING=enhanced|maximum turns it on (runtime/hle/lighting_preset.h); SNAP_LIGHT_FX=1|0 forces it on / off whatever the preset says.
//   Three full-screen passes per scene frame at render resolution, on the linear HDR scene colour before the upscaler and the tonemap:
//     1. lfx_ao.frag      GTAO (horizon search in screen-space slices) + the contact-shadow march, written noisy into an RG8 image
//     2. lfx_resolve.frag depth-aware 5x5 bilateral blur + temporal accumulation (camera-motion reprojection, view-distance rejection, neighbourhood clamp)
//     3. lfx_comp.frag    colour *= AO factor * contact factor (multiply blend; the distance fade lives here)
//   The game draws no ambient occlusion in the course (its HBAO only runs near the photo select), so this adds a look the original does not have. Photo and
//   Evaluation frames are skipped by the Fx plumbing (nvn_fx.cpp), so photo scores do not depend on it. While it is on, the older SNAP_CONTACT_SHADOWS effect
//   (nvn_contact.cpp, the same march without the denoise) stays off; its sky gate (outdoors only) is reused here.
//   Tunables (env, test aids): SNAP_LIGHT_AO=0 (AO off), SNAP_LIGHT_CONTACT=0 (contact shadows off), SNAP_LIGHT_AO_RADIUS world units (1.0),
//   SNAP_LIGHT_AO_STRENGTH (0.85), SNAP_LIGHT_AO_POWER (1.6), SNAP_LIGHT_CONTACT_STRENGTH (0.6), SNAP_LIGHT_CONTACT_LEN (0.5), SNAP_LIGHT_CONTACT_THICK (0.2),
//   SNAP_LIGHT_FADE view distance (60), SNAP_LIGHT_ALPHA temporal weight of the newest frame (0.1), SNAP_LIGHT_DEBUG=1 AO only, 2 contact only,
//   3 raw (unfiltered) AO, SNAP_LIGHT_AB=<P> (see lighting_preset.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/lighting_preset.h"
#include "nvn_dev.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_lighting_spv.h"
#include "nvn_rt.h"
#include "nvn_ssgi_spv.h"

namespace nvn {
namespace {
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule fsAo = VK_NULL_HANDLE, fsRes = VK_NULL_HANDLE, fsComp = VK_NULL_HANDLE;
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  VkPipeline pipeAo = VK_NULL_HANDLE, pipeRes = VK_NULL_HANDLE, pipeComp = VK_NULL_HANDLE, pipeDbg = VK_NULL_HANDLE; VkFormat sceneFmt = VK_FORMAT_UNDEFINED;
  Img raw, hist[2]; uint32_t w = 0, h = 0; int cur = 0;
  Img rawHalf; uint32_t halfW = 0, halfH = 0;   // SNAP_LIGHT_HALF: the AO / contact pass at half resolution (made at first use)
  bool haveHist = false; int lastPresent = -1000; double prevVp[4][4] = {};
  // bounce light (SNAP_SSGI): own pipelines and images, same helpers
  bool giReady = false, giFailed = false;
  VkShaderModule fsGi = VK_NULL_HANDLE, fsGiRes = VK_NULL_HANDLE, fsGiComp = VK_NULL_HANDLE;
  VkPipeline pipeGi = VK_NULL_HANDLE, pipeGiRes = VK_NULL_HANDLE, pipeGiComp = VK_NULL_HANDLE, pipeGiDbg = VK_NULL_HANDLE;
  VkPipeline pipeAoRt = VK_NULL_HANDLE;   // SNAP_RT_AO: the ray traced AO (nvn_rt.cpp AoModule); null = GTAO runs
  VkPipeline pipeGiRt = VK_NULL_HANDLE;   // SNAP_RT_GI: the ray traced gather (nvn_rt.cpp GiModule); null = the screen-space gather runs
  Img giRaw, giHist[2]; uint32_t giW = 0, giH = 0; int giCur = 0;
  bool giHaveHist = false; int giLastPresent = -1000; double giPrevVp[4][4] = {};
} g;

// per-frame tunables: plain getenv when SNAP_DEV is off, a live param (F1 > Tuning, nvn_dev.h) when it is on; range 0..max(1, 4*def) unless given
float EnvF(const char* n, float def, float mn = 0.f, float mx = -1.f) { return dev_envf(n, def, mn, mx >= mn ? mx : std::max(1.f, def * 4.f)); }
bool EnvOff(const char* n) { const char* e = getenv(n); return e && *e && atoi(e) == 0; }
bool Configured() {   // the preset / SNAP_LIGHT_FX say yes (not looking at the A/B test aid or a failed setup)
  static const int mode = [] { const char* e = getenv("SNAP_LIGHT_FX"); return e && *e ? (atoi(e) != 0 ? 1 : 0) : -1; }();
  return mode >= 0 ? mode == 1 : snaplight::enhanced();
}
bool SsgiOn() {   // SNAP_SSGI=1: screen-space bounce light, its own switch (the "Bounce light" row), independent of the lighting preset
  const bool v = SNAP_LIVE(bool, ([] { const char* e = getenv("SNAP_SSGI"); return e && *e && atoi(e) != 0; }()));   // live row (hle/live.h)
  return v && !g.giFailed;
}
bool Enabled() { return (Configured() || SsgiOn()) && !g.failed && snaplight::ab_on((unsigned long long)g_presentCount); }

struct PcAo { float ivp[16]; float dclip[4]; float light[4]; float size[4]; float q[4]; };
struct PcRes { float reproj[16]; float size[4]; float q[4]; float h[4]; };
struct PcComp { float k[4]; float size[4]; };
struct PcGi { float ivp[16]; float size[4]; float q[4]; uint64_t tlas, avg; };   // tlas, avg: the RT gather only (scene TLAS and per-instance colour cache, device addresses)
struct PcGiComp { float k[4]; float size[4]; };
static_assert(sizeof(PcGi) == 112 && sizeof(PcGiComp) <= 128, "push constants must fit the 128 byte guarantee");
static_assert(sizeof(PcAo) == 128 && sizeof(PcRes) <= 128 && sizeof(PcComp) <= 128, "push constants must fit the 128 byte guarantee");

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
bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt) {
  if (o.img || o.view || o.mem) { RetireVkImage(h, o.img, o.view, o.mem); o = Img{}; }   // a size change: the old image goes once the GPU is past it
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  o = Img{};
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}

// the shader module for effect shader `name`: the embedded SPIR-V, or with SNAP_DEV=1 the live copy from SNAP_SHADER_DIR (nvn_dev.h dev_spv)
VkShaderModule LfxMod(Host* h, const char* name, const uint32_t* emb, size_t bytes) { const DevSpv s = dev_spv(name, emb, bytes); return FxMakeModule(h, s.words, s.bytes); }

bool Setup(Host* h, VkFormat sceneFmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(h->device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 3; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    g.fsAo = LfxMod(h, "lfx_ao", kLfxAoFrag, sizeof kLfxAoFrag);
    g.fsRes = LfxMod(h, "lfx_resolve", kLfxResolveFrag, sizeof kLfxResolveFrag);
    g.fsComp = LfxMod(h, "lfx_comp", kLfxCompFrag, sizeof kLfxCompFrag);
    if (!g.fsAo || !g.fsRes || !g.fsComp) return false;
    g.pipeAo = FxMakeFullscreenPipeline(h, g.pl, g.fsAo, VK_FORMAT_R8G8_UNORM, false);
    g.pipeRes = FxMakeFullscreenPipeline(h, g.pl, g.fsRes, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    if (!g.pipeAo || !g.pipeRes) return false;
    if (snap_rt::On() && snap_rt::AoOn()) {   // the RT AO needs the ray query device: only with SNAP_RT_AO at launch. Any failure keeps GTAO.
      VkShaderModule m = snap_rt::AoModule(h);
      g.pipeAoRt = m ? FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R8G8_UNORM, false) : VK_NULL_HANDLE;
      if (!g.pipeAoRt && m) fprintf(stderr, "[rt] ambient occlusion falls back to GTAO: the RT AO pipeline could not be made\n");
    }
    g.ready = true;
  }
  if (g.sceneFmt != sceneFmt || !g.pipeComp) {   // the scene colour format is fixed (RGBA16F), so this runs once
    g.pipeComp = FxMakeFullscreenPipeline(h, g.pl, g.fsComp, sceneFmt, true);
    g.pipeDbg = FxMakeFullscreenPipeline(h, g.pl, g.fsComp, sceneFmt, false);
    g.sceneFmt = sceneFmt;
  }
  return g.pipeComp != VK_NULL_HANDLE;
}

bool EnsureImages(Host* h, uint32_t w, uint32_t hh) {
  if (g.w == w && g.h == hh && g.raw.img) return true;
  if (g.raw.img) fprintf(stderr, "lighting: render size changed to %ux%u\n", w, hh);   // a command buffer may still read them
  if (!MakeImg(h, g.raw, w, hh, VK_FORMAT_R8G8_UNORM) || !MakeImg(h, g.hist[0], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT) || !MakeImg(h, g.hist[1], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
  g.w = w; g.h = hh; g.haveHist = false; g.cur = 0;
  RetireVkImage(h, g.rawHalf.img, g.rawHalf.view, g.rawHalf.mem); g.rawHalf = Img{}; g.halfW = g.halfH = 0;   // made again at the new size
  return true;
}
bool EnsureHalf(Host* h) {
  if (g.rawHalf.img) return true;
  static bool failed = false;
  if (failed) return false;
  g.halfW = (g.w + 1) / 2; g.halfH = (g.h + 1) / 2;
  if (!MakeImg(h, g.rawHalf, g.halfW, g.halfH, VK_FORMAT_R8G8_UNORM)) { failed = true; g.rawHalf = Img{}; fprintf(stderr, "lighting: the half-resolution AO image could not be made, full resolution stays\n"); return false; }
  fprintf(stderr, "lighting: AO and contact shadows at half resolution (%ux%u of %ux%u), SNAP_LIGHT_HALF=0 for full\n", g.halfW, g.halfH, g.w, g.h);
  return true;
}

// One full-screen pass into one of our own images (whole image, contents not needed), then a memory barrier like EndRendering.
void DrawTo(ReplayCtx& cx, Img& t, VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcBytes, uint32_t tw = 0, uint32_t th = 0) {
  VkCommandBuffer cb = cx.cb;
  if (!tw) { tw = g.w; th = g.h; }
  ImgBar(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = t.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {tw, th}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cb, &ri);
  VkViewport vp{0, 0, (float)tw, (float)th, 0, 1}; VkRect2D sc{{0, 0}, {tw, th}};
  vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcBytes, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  ImgBar(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Bind(Host* h, VkDescriptorSet set, uint32_t binding, VkSampler s, VkImageView v, VkImageLayout l) {
  VkDescriptorImageInfo ii{s, v, l};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
  vkUpdateDescriptorSets(h->device, 1, &w, 0, nullptr);
}

bool SetupGi(Host* h, VkFormat sceneFmt) {
  if (g.giReady) return true;
  if (!g.ready && !Setup(h, sceneFmt)) return false;   // samplers, set layout and pipeline layout are shared with AO
  g.fsGi = LfxMod(h, "lfx_gi", kLfxGiFrag, sizeof kLfxGiFrag);
  g.fsGiRes = LfxMod(h, "lfx_gires", kLfxGiResFrag, sizeof kLfxGiResFrag);
  g.fsGiComp = LfxMod(h, "lfx_gicomp", kLfxGiCompFrag, sizeof kLfxGiCompFrag);
  if (!g.fsGi || !g.fsGiRes || !g.fsGiComp) return false;
  g.pipeGi = FxMakeFullscreenPipeline(h, g.pl, g.fsGi, VK_FORMAT_R16G16B16A16_SFLOAT, false);
  g.pipeGiRes = FxMakeFullscreenPipeline(h, g.pl, g.fsGiRes, VK_FORMAT_R16G16B16A16_SFLOAT, false);
  g.pipeGiComp = FxMakeFullscreenPipeline(h, g.pl, g.fsGiComp, sceneFmt, true);
  g.pipeGiDbg = FxMakeFullscreenPipeline(h, g.pl, g.fsGiComp, sceneFmt, false);
  if (!g.pipeGi || !g.pipeGiRes || !g.pipeGiComp) return false;
  if (snap_rt::On() && snap_rt::GiOn()) {   // the RT gather needs the ray query device: only with SNAP_RT_GI at launch. Any failure keeps the screen-space gather.
    VkShaderModule m = snap_rt::GiModule(h);
    g.pipeGiRt = m ? FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R16G16B16A16_SFLOAT, false) : VK_NULL_HANDLE;
    if (!g.pipeGiRt && m) fprintf(stderr, "[rt] bounce light falls back to screen-space: the RT gather pipeline could not be made\n");
  }
  g.giReady = true;
  return true;
}

bool EnsureGiImages(Host* h, uint32_t w, uint32_t hh) {
  if (g.giW == w && g.giH == hh && g.giRaw.img) return true;
  if (g.giRaw.img) fprintf(stderr, "lighting: bounce light render size changed to %ux%u\n", w, hh);
  if (!MakeImg(h, g.giRaw, w, hh, VK_FORMAT_R16G16B16A16_SFLOAT) || !MakeImg(h, g.giHist[0], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT) || !MakeImg(h, g.giHist[1], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
  g.giW = w; g.giH = hh; g.giHaveHist = false; g.giCur = 0;
  return true;
}

// Bounce light: gather (reads depth and the scene colour), denoise (the same bilateral + temporal filter as AO), multiply composite onto the scene colour.
// Runs after the AO / contact composite so the bounce sees the occluded colour. DrawTo draws into g.w x g.h, which is kept equal to the scene size here.
void GiPass(ReplayCtx& cx, const FxCam* cam, TextureImpl* c, TextureImpl* z) {
  if (!SetupGi(cx.h, c->fi->vk) || !EnsureGiImages(cx.h, c->HW(), c->HH())) { g.giFailed = true; fprintf(stderr, "lighting: bounce light setup failed, disabled\n"); return; }
  g.w = g.giW; g.h = g.giH;
  const bool histOk = g.giHaveHist && g_presentCount - g.giLastPresent <= 4;
  PcGi pg{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pg.ivp[k * 4 + r] = (float)cam->ivp[r][k];
  float jx, jy; FxJitter(&jx, &jy);
  pg.size[0] = (float)g.giW; pg.size[1] = (float)g.giH; pg.size[2] = jx; pg.size[3] = jy;
  pg.q[0] = EnvF("SNAP_SSGI_RADIUS", 2.0f); pg.q[1] = (float)(int)EnvF("SNAP_SSGI_SAMPLES", 12.f); pg.q[2] = (float)(g_presentCount & 1023); pg.q[3] = EnvF("SNAP_SSGI_CLAMP", 3.0f);
  PcRes pr{};
  {
    double R[4][4];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { double s = 0; for (int j = 0; j < 4; j++) s += g.giPrevVp[r][j] * cam->ivp[j][k]; R[r][k] = s; }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pr.reproj[k * 4 + r] = (float)R[r][k];
  }
  pr.size[0] = (float)g.giW; pr.size[1] = (float)g.giH; pr.size[2] = histOk ? 1.f : 0.f; pr.size[3] = cam->nearZ;
  pr.q[0] = EnvF("SNAP_SSGI_ALPHA", 0.08f);
  PcGiComp pk{};
  pk.k[0] = EnvF("SNAP_SSGI_STRENGTH", 0.6f); pk.k[1] = EnvF("SNAP_SSGI_FADE", 60.f); pk.k[2] = EnvF("SNAP_SSGI_MAX", 1.6f); pk.k[3] = EnvF("SNAP_SSGI_DEBUG", 0.f, 0.f, 1.f);
  pk.size[0] = (float)g.giW; pk.size[1] = (float)g.giH;
  static int logged; if (logged++ < 3) fprintf(stderr, "lighting: p%d bounce light %ux%u radius %.2f samples %d history %d\n", g_presentCount, g.giW, g.giH, pg.q[0], (int)pg.q[1], (int)histOk);

  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  Transition(cx.cb, c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkImageView cv = c->View(nullptr, true);
  Img& out = g.giHist[g.giCur]; Img& old = g.giHist[g.giCur ^ 1];
  VkDescriptorSet s1 = FxAllocSet(cx, g.dsl), s2 = FxAllocSet(cx, g.dsl), s3 = FxAllocSet(cx, g.dsl);
  if (dv && cv && s1 && s2 && s3) {
    if (old.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, old, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Bind(cx.h, s1, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Bind(cx.h, s1, 1, g.nearest, cv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    snap_rt::ReflInfo ri;   // RT gather: updates the shared per-instance colour cache (compute, outside any render pass), then the gather shader traces against the TLAS
    const bool rt = g.pipeGiRt && snap_rt::GiPass(cx, cv, g.linear, cam->vp, &ri);
    if (rt) { pg.tlas = ri.tlas; pg.avg = ri.avg; pg.q[0] = EnvF("SNAP_RT_GI_RADIUS", 6.0f); pg.q[1] = (float)(int)EnvF("SNAP_RT_GI_RAYS", 1.f); }
    dev_pass_begin(cx.cb, rt ? "lfx gi rt" : "lfx gi"); DrawTo(cx, g.giRaw, rt ? g.pipeGiRt : g.pipeGi, s1, &pg, sizeof pg);
    Bind(cx.h, s2, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 1, g.nearest, g.giRaw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 2, g.linear, old.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dev_pass_begin(cx.cb, "lfx gi resolve"); DrawTo(cx, out, g.pipeGiRes, s2, &pr, sizeof pr);
    Bind(cx.h, s3, 0, g.nearest, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const bool dbg = pk.k[3] > 0.5f;
    dev_pass_begin(cx.cb, "lfx gi comp"); FxDrawFullscreen(cx, c, dbg && g.pipeGiDbg ? g.pipeGiDbg : g.pipeGiComp, g.pl, s3, &pk, sizeof pk); dev_pass_end(cx.cb, "lfx gi comp");
    FxNoteRan();
    g.giHaveHist = true; g.giLastPresent = g_presentCount; g.giCur ^= 1;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) g.giPrevVp[r][k] = cam->vp[r][k];
  }
  FxDepthDone(cx, z, prev);
}

// SNAP_DEV=1: a shader edited in SNAP_SHADER_DIR was recompiled (nvn_dev.h): swap the module and rebuild only the pipelines that use it. The old
// pipelines are not destroyed (an earlier command buffer may still be running them); a failed rebuild keeps them.
void DevReload(Host* h) {
  static uint32_t seen[6];
  struct S { const char* name; const uint32_t* emb; size_t bytes; VkShaderModule* mod; bool gi; };
  const S list[6] = {{"lfx_ao", kLfxAoFrag, sizeof kLfxAoFrag, &g.fsAo, false}, {"lfx_resolve", kLfxResolveFrag, sizeof kLfxResolveFrag, &g.fsRes, false},
                     {"lfx_comp", kLfxCompFrag, sizeof kLfxCompFrag, &g.fsComp, false}, {"lfx_gi", kLfxGiFrag, sizeof kLfxGiFrag, &g.fsGi, true},
                     {"lfx_gires", kLfxGiResFrag, sizeof kLfxGiResFrag, &g.fsGiRes, true}, {"lfx_gicomp", kLfxGiCompFrag, sizeof kLfxGiCompFrag, &g.fsGiComp, true}};
  for (int i = 0; i < 6; i++) {
    if (list[i].gi ? !g.giReady : !g.ready) continue;
    if (!dev_spv_changed(list[i].name, &seen[i])) continue;
    VkShaderModule m = LfxMod(h, list[i].name, list[i].emb, list[i].bytes);
    if (!m) continue;
    VkPipeline a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    switch (i) {
      case 0: a = FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R8G8_UNORM, false); break;
      case 1: a = FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R16G16B16A16_SFLOAT, false); break;
      case 2: a = FxMakeFullscreenPipeline(h, g.pl, m, g.sceneFmt, true); b = FxMakeFullscreenPipeline(h, g.pl, m, g.sceneFmt, false); break;
      case 3: a = FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R16G16B16A16_SFLOAT, false); break;
      case 4: a = FxMakeFullscreenPipeline(h, g.pl, m, VK_FORMAT_R16G16B16A16_SFLOAT, false); break;
      default: a = FxMakeFullscreenPipeline(h, g.pl, m, g.sceneFmt, true); b = FxMakeFullscreenPipeline(h, g.pl, m, g.sceneFmt, false); break;
    }
    if (!a) { fprintf(stderr, "[dev] lighting: pipeline for reloaded shader %s failed, keeping the old one\n", list[i].name); continue; }
    *list[i].mod = m;
    switch (i) {
      case 0: g.pipeAo = a; break;
      case 1: g.pipeRes = a; break;
      case 2: g.pipeComp = a; if (b) g.pipeDbg = b; break;
      case 3: g.pipeGi = a; break;
      case 4: g.pipeGiRes = a; break;
      default: g.pipeGiComp = a; if (b) g.pipeGiDbg = b; break;
    }
    fprintf(stderr, "[dev] lighting: shader %s applied (pipelines rebuilt)\n", list[i].name); fflush(stderr);
  }
}

void Pre(ReplayCtx& cx) {
  if (g.failed) return;
  if (dev_on()) DevReload(cx.h);
  const FxCam* cam = FxCamera();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  if (!cam || !c || !z || c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  const bool wantAo = !EnvOff("SNAP_LIGHT_AO"), wantCt = !EnvOff("SNAP_LIGHT_CONTACT");
  const bool lightOn = Configured() && (wantAo || wantCt), giOn = SsgiOn();
  if (!lightOn && !giOn) return;
  if (!lightOn) { GiPass(cx, cam, c, z); return; }
  if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH())) { g.failed = true; fprintf(stderr, "lighting: setup failed, disabled\n"); return; }

  // the sun, for contact shadows: only where there is a dynamic shadow pass and the sky is in view
  const FxCaster* cs = FxCasterNow();
  bool contact = wantCt && cs && ContactSkyGate(cx, z, cam->nearZ);
  float L[3] = {0, 1, 0};
  if (cs) {   // toward the light: the caster matrix' depth axis points away from the light (or toward it): take the sign that points up
    float sgn = EnvF("SNAP_CONTACT_LSIGN", 0.f, -1.f, 1.f);
    if (sgn == 0.f) sgn = cs->axis[1] > 0.f ? 1.f : -1.f;
    for (int i = 0; i < 3; i++) L[i] = cs->axis[i] * (sgn < 0 ? -1.f : 1.f);
  }
  static unsigned long long nTot = 0, nContact = 0;
  nTot++; nContact += contact;
  if (nTot % 600 == 0 && getenv("SNAP_FX_LOG")) fprintf(stderr, "lighting: p%d stats: %llu scene frames, contact shadows on %llu\n", g_presentCount, nTot, nContact);

  float jx, jy; FxJitter(&jx, &jy);
  const int lvl = snaplight::level();
  PcAo pa{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pa.ivp[k * 4 + r] = (float)cam->ivp[r][k];
  for (int r = 0; r < 4; r++) pa.dclip[r] = (float)(cam->vp[r][0] * L[0] + cam->vp[r][1] * L[1] + cam->vp[r][2] * L[2]);
  pa.light[0] = L[0]; pa.light[1] = L[1]; pa.light[2] = L[2]; pa.light[3] = (float)(g_presentCount & 1023);
  pa.size[0] = (float)g.w; pa.size[1] = (float)g.h; pa.size[2] = jx; pa.size[3] = jy;
  pa.q[0] = wantAo ? EnvF("SNAP_LIGHT_AO_RADIUS", 1.0f) : 0.f;
  pa.q[1] = contact ? EnvF("SNAP_LIGHT_CONTACT_LEN", 0.5f) : 0.f;
  pa.q[2] = EnvF("SNAP_LIGHT_CONTACT_THICK", 0.2f);
  pa.q[3] = (float)(lvl >= 2 ? 3 : 2);

  // history: usable when the previous result is from a recent scene frame at the same size (photo / menu gaps reset it)
  const bool histOk = g.haveHist && g_presentCount - g.lastPresent <= 4;
  // a dropped history is a visible pop (the AO starts over): logged so a run shows how often the scene pass went missing (photo holds are 3 presents, under the limit)
  { static int n, runs; runs++; if (g.haveHist && !histOk && n++ < 40) fprintf(stderr, "lighting: AO history dropped at p%d (gap %d presents, pass %d)\n", g_presentCount, g_presentCount - g.lastPresent, runs); }
  PcRes pr{};
  double viewMove = 0, reprojOff = 0;   // largest change of the view matrix since the last scene frame; how far reproj is from the identity
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) viewMove = std::max(viewMove, fabs(g.prevVp[r][k] - cam->vp[r][k]));
  {
    double R[4][4];   // reproj = prevVP * ivp: this frame's (ndc, depth, 1) -> the previous frame's clip space
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { double s = 0; for (int j = 0; j < 4; j++) s += g.prevVp[r][j] * cam->ivp[j][k]; R[r][k] = s; }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pr.reproj[k * 4 + r] = (float)R[r][k];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) reprojOff = std::max(reprojOff, fabs(R[r][k] / R[3][3] - (r == k ? 1.0 : 0.0)));
  }
  if (getenv("SNAP_FX_LOG") && nTot % 120 < 3) fprintf(stderr, "lighting: p%d view change since the last scene frame %.3g, reprojection off the identity by %.3g\n", g_presentCount, viewMove, reprojOff);
  pr.size[0] = (float)g.w; pr.size[1] = (float)g.h; pr.size[2] = histOk ? 1.f : 0.f; pr.size[3] = cam->nearZ;
  pr.q[0] = EnvF("SNAP_LIGHT_ALPHA", 0.1f);
  pr.q[1] = EnvF("SNAP_LIGHT_STILL", 256.f, 1.f, 2047.f);   // frames a still view accumulates before its AO stands (a still picture stops flickering); 10 = the old fixed weight
  pr.q[2] = viewMove == 0.0 ? 1.f : 0.f;   // the view did not move: the history is read at the same pixel (no reprojection, no resampling)
  PcComp pk{};
  pk.k[0] = wantAo ? EnvF("SNAP_LIGHT_AO_STRENGTH", 0.85f) : 0.f; pk.k[1] = contact ? EnvF("SNAP_LIGHT_CONTACT_STRENGTH", 0.6f) : 0.f;
  pk.k[2] = EnvF("SNAP_LIGHT_FADE", 60.f); pk.k[3] = EnvF("SNAP_LIGHT_AO_POWER", 1.6f);
  pk.size[0] = (float)g.w; pk.size[1] = (float)g.h; pk.size[2] = EnvF("SNAP_LIGHT_DEBUG", 0.f, 0.f, 4.f);
  const bool rawDbg = pk.size[2] > 2.5f && pk.size[2] < 3.5f;   // 3: the raw AO factor, no resolve (shows what the denoise removes)
  if (rawDbg) pk.size[2] = 1.f;

  static int logged; if (logged++ < 3) fprintf(stderr, "lighting: p%d %ux%u AO %d contact %d light toward (%.3f %.3f %.3f) history %d\n", g_presentCount, g.w, g.h, (int)wantAo, (int)contact, L[0], L[1], L[2], (int)histOk);

  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  Img& out = g.hist[g.cur]; Img& old = g.hist[g.cur ^ 1];
  VkDescriptorSet s1 = FxAllocSet(cx, g.dsl), s2 = FxAllocSet(cx, g.dsl), s3 = FxAllocSet(cx, g.dsl);
  if (dv && s1 && s2 && s3) {
    // history image never written yet: give it a defined layout (its contents are not read while pr.size[2] is 0)
    if (old.layout == VK_IMAGE_LAYOUT_UNDEFINED) { ImgBar(cx.cb, old, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
    Bind(cx.h, s1, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    uint64_t aoTlas = 0;   // RT AO: the pass takes rays from the scene TLAS; q.zw carry its address, light.w the ray count (the shader's push block, lfx_ao_rt.frag)
    const bool rtAo = wantAo && g.pipeAoRt && snap_rt::AoPass(&aoTlas);
    if (rtAo) {
      uint32_t lo = (uint32_t)aoTlas, hi = (uint32_t)(aoTlas >> 32);
      memcpy(&pa.q[2], &lo, 4); memcpy(&pa.q[3], &hi, 4);
      pa.q[0] = EnvF("SNAP_RT_AO_RADIUS", 1.5f);
      pa.light[3] = (float)((g_presentCount & 1023) + 1024 * (int)EnvF("SNAP_RT_AO_RAYS", 2.f, 1.f, 16.f));
    }
    // Half resolution (default; SNAP_LIGHT_HALF=0 = one result per pixel as before): the pass works out one pixel of every 2x2 block per frame, a quarter
    // of the work, and the resolve spreads it by depth and accumulates. Not for the ray traced AO (its push block has no room for the block pixel).
    const bool half = !rtAo && EnvF("SNAP_LIGHT_HALF", 1.f, 0.f, 1.f) > 0.5f && EnsureHalf(cx.h);
    Img& raw = half ? g.rawHalf : g.raw;
    if (half) {
      static unsigned seq; static const int order[4] = {0, 3, 1, 2};   // (0,0) (1,1) (1,0) (0,1): the diagonal first
      const int o = order[seq++ & 3];
      pa.q[3] += 16.f * (float)(o + 1);
      pr.h[0] = 1.f; pr.h[1] = (float)(o & 1); pr.h[2] = (float)(o >> 1);
    }
    dev_pass_begin(cx.cb, rtAo ? "lfx ao rt" : "lfx ao");
    if (half) DrawTo(cx, raw, g.pipeAo, s1, &pa, sizeof pa, g.halfW, g.halfH); else DrawTo(cx, raw, rtAo ? g.pipeAoRt : g.pipeAo, s1, &pa, sizeof pa);
    Bind(cx.h, s2, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 1, g.nearest, raw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 2, g.linear, old.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dev_pass_begin(cx.cb, "lfx resolve"); DrawTo(cx, out, g.pipeRes, s2, &pr, sizeof pr);
    Bind(cx.h, s3, 0, g.nearest, rawDbg && !half ? g.raw.view : out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (rawDbg) pk.k[2] = 1e9f;
    const bool dbg = pk.size[2] > 0.5f;
    dev_pass_begin(cx.cb, "lfx comp"); FxDrawFullscreen(cx, c, dbg && g.pipeDbg ? g.pipeDbg : g.pipeComp, g.pl, s3, &pk, sizeof pk); dev_pass_end(cx.cb, "lfx comp");
    FxNoteRan();
    g.haveHist = true; g.lastPresent = g_presentCount; g.cur ^= 1;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) g.prevVp[r][k] = cam->vp[r][k];
  }
  FxDepthDone(cx, z, prev);
  if (giOn) GiPass(cx, cam, c, z);
}

const FxEffect kEffect = {"enhanced_lighting", Enabled, Pre, nullptr, true};
struct Reg { Reg() { FxRegister(&kEffect); } } reg;
}  // namespace

bool LightingFxEnabled() { return Configured(); }
}  // namespace nvn
