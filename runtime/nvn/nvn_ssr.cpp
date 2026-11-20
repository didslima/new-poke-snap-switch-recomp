// SPDX-License-Identifier: MIT
// Water effects (project code, ours): screen-space reflections (SNAP_WATER_SSR), caustics (SNAP_WATER_CAUSTICS), shoreline foam (SNAP_WATER_FOAM).
// All off by default. Display only: nothing runs on photo frames (FxIsPhotoFrame) and nothing is written but the scene colour (FxWritable).
//   * The game draws its sea in a HALF-resolution pass (960x540 colour + depth, soft-particle style: the draws read the full depth), then composites that
//     buffer into the scene colour with one full-screen quad. Water draws are found by fragment program key (kWaterFs below, found with NVN_DRAW_LOG and the
//     SNAP_DRAW_SKIP_GROUPS A/B in nvn_draw.cpp; more via SNAP_WATER_FS=<key12,key12,...>).
//   * WaterBeforeDraw (hook line in nvn_draw.cpp DoDraw):
//       1. first water draw: the pass is closed and the half-resolution depth is copied (hdPre). The water draws then run with depth writes forced on
//          (WaterForceDepthWrite), so the half-resolution depth afterwards holds the water surface wherever the sea is visible.
//       2. first draw after the water: that depth is copied (hdPost) and hdPre is copied back, so the rest of the frame sees the game's own depth.
//       3. first draw into the scene colour after that (the composite): the scene colour is copied (what the reflections sample), the composite runs.
//       4. the next draw: ONE full-screen pass over the scene colour, blended   dst = src.rgb + dst.rgb * src.a   so it never reads its own target.
//          Ray misses leave the game's own cubemap reflection untouched (alpha 1, rgb 0).
//   * Tunables (env, test aids): SNAP_WATER_SSR_STRENGTH (1), SNAP_WATER_SSR_DIST (45 world units), SNAP_WATER_RIPPLE (0.02, ripple amount on the
//     reflection normal), SNAP_WATER_CAUSTICS_STRENGTH (1), SNAP_WATER_FOAM_STRENGTH (1), SNAP_WATER_DEBUG (1 water mask, 2 water height above the floor),
//     SNAP_WATER_LOG=1 (probe: every distinct scene fragment key with blend / depth-write state, and pass stats), SNAP_WATER_TIME=1 (GPU ms of the copies + pass).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "hle/lighting_preset.h"
#include "nvn_dev.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_ocean.h"
#include "nvn_rt.h"
#include "nvn_ssr.h"
#include "nvn_water_spv.h"

namespace nvn {
namespace {
// Fragment program keys (first 12 hex digits of the data-section hash, as in NVN_DRAW_LOG "fs=") of the water pixel shaders. Filled from captures; see
// analysis/water/water_programs.md.
const char* const kWaterFs[] = {
    "81169ded7ee5", "2b28bfe1dbdb", "6a0d10e3dfbb", "beae698d6f0b",   // the sea: four consecutive blended draws in the half-resolution pass (core_beach_0 p9300); a693e48a9428 after them is a small overlay
    nullptr,
};

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; };
struct Pc { float vp[16]; float size[4]; float k[4]; float s[4]; uint64_t tlas, avg; };   // tlas, avg: the RT reflection variant only (scene TLAS and per-instance colour cache, device addresses)
static_assert(sizeof(Pc) == 128, "push constants must fit the 128 byte guarantee");

struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkShaderModule fs = VK_NULL_HANDLE;
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE, repeat = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE, pipeRt = VK_NULL_HANDLE; VkShaderModule fsRt = VK_NULL_HANDLE; bool rtFailed = false; VkFormat fmt = VK_FORMAT_UNDEFINED;
  Img col, hdPre, hdPost; uint32_t w = 0, h = 0, hw = 0, hh = 0; uint32_t seenSpv = 0;   // col: full-resolution scene colour copy; hdPre / hdPost: half-resolution depth copies
  std::unordered_set<std::string> keys;
} g;

// Tunables in one struct (registered with the dev loop API: F1 Tuning page, ctl `set`, SNAP_DEV=1). Defaults come from the env vars (test aids).
struct Tun {
  float ssrStrength = 1.f, ssrDist = 45.f, ripple = 0.02f, causticsStrength = 1.f, foamStrength = 1.f, oceanStrength = 1.f;
  int debug = 0;   // 1 water mask, 2 water height above the floor, 3 depth classes, 4 view distances (R water pre, G water post, B depth), 6 ocean maps (R crest foam, G/B slope; needs the ocean on), 5 reflection classes (red: screen-space hit, green: RT hit seen on screen, blue: RT hit from the colour cache, yellow: ray left the scene)
} tun;
float EnvF(const char* n, float def) { const char* e = getenv(n); return e && *e ? (float)atof(e) : def; }
void RegisterTunables() {
  static bool done = false;
  if (done) return;
  done = true;
  tun.ssrStrength = EnvF("SNAP_WATER_SSR_STRENGTH", tun.ssrStrength); tun.ssrDist = EnvF("SNAP_WATER_SSR_DIST", tun.ssrDist);
  tun.ripple = EnvF("SNAP_WATER_RIPPLE", tun.ripple); tun.causticsStrength = EnvF("SNAP_WATER_CAUSTICS_STRENGTH", tun.causticsStrength);
  tun.foamStrength = EnvF("SNAP_WATER_FOAM_STRENGTH", tun.foamStrength); tun.oceanStrength = EnvF("SNAP_WATER_OCEAN_STRENGTH", tun.oceanStrength); tun.debug = (int)EnvF("SNAP_WATER_DEBUG", 0.f);
  dev_param_f("SNAP_WATER_SSR_STRENGTH", &tun.ssrStrength, 0.f, 2.f, 1.f);
  dev_param_f("SNAP_WATER_SSR_DIST", &tun.ssrDist, 5.f, 200.f, 45.f);
  dev_param_f("SNAP_WATER_RIPPLE", &tun.ripple, 0.f, 0.2f, 0.02f);
  dev_param_f("SNAP_WATER_CAUSTICS_STRENGTH", &tun.causticsStrength, 0.f, 3.f, 1.f);
  dev_param_f("SNAP_WATER_FOAM_STRENGTH", &tun.foamStrength, 0.f, 2.f, 1.f);
  dev_param_f("SNAP_WATER_OCEAN_STRENGTH", &tun.oceanStrength, 0.f, 8.f, 1.f);
  dev_param_i("SNAP_WATER_DEBUG", &tun.debug, 0, 6, 0);
}

// per scene frame
struct S {
  int frame = -1;
  bool inWater = false, haveMask = false, armed = false, skip = false;
  TextureImpl* hz = nullptr;   // the half-resolution water pass depth
  int ranPasses = 0, waterDraws = 0, draws = 0;
  const char* why = "";   // SNAP_WATER_LOG=2: last reason the hook did nothing
} st;

// -1 = not read yet. bit order: 0 ssr, 1 caustics, 2 foam, 3 ocean (FFT waves, nvn_ocean.cpp). Live: the options rows call snap_water_fx_set.
std::atomic<int> g_on[4] = {{-1}, {-1}, {-1}, {-1}};
const char* const kEnvName[4] = {"SNAP_WATER_SSR", "SNAP_WATER_CAUSTICS", "SNAP_WATER_FOAM", "SNAP_WATER_OCEAN"};
int g_dev[4] = {-1, -1, -1, -1};   // SNAP_DEV=1: the switches as live parameters (ab_capture.ps1 -Effect SNAP_WATER_SSR -Values 0,1); -1 = not registered
bool BaseOn(int i) {
  int v = g_on[i].load(std::memory_order_relaxed);
  if (v < 0) { const char* e = getenv(kEnvName[i]); v = e && *e && atoi(e) != 0 ? 1 : 0; int x = -1; g_on[i].compare_exchange_strong(x, v); v = g_on[i].load(); }
  return v != 0;
}
bool On(int i) {
  if (i == 0 && snap_rt::ReflectOn()) return true;   // RT reflections (SNAP_RT_REFLECT) are the second stage of the water reflection: the screen-space march runs first
  if (g_dev[i] >= 0) return g_dev[i] != 0;
  return BaseOn(i);
}
void RegisterSwitches() {   // after the tunables: the three switches as dev params (the effect must be on at launch, then the params flip it)
  static bool done = false;
  if (done) return;
  done = true;
  if (dev_on()) for (int i = 0; i < 4; i++) { g_dev[i] = BaseOn(i) ? 1 : 0; dev_param_i(kEnvName[i], &g_dev[i], 0, 1, g_dev[i]); }
}
bool AnyOn() { return !g.failed && (On(0) || On(1) || On(2) || On(3)); }
bool Enabled() { return AnyOn() && snaplight::ab_on((unsigned long long)g_presentCount); }   // SNAP_LIGHT_AB: same-view A/B
bool Log2() { static const bool on = getenv("SNAP_WATER_LOG") && getenv("SNAP_WATER_LOG")[0] == '2'; return on; }
bool Log() { static const bool on = getenv("SNAP_WATER_LOG") && getenv("SNAP_WATER_LOG")[0] != '0'; return on; }

bool IsWaterFs(const char* key) {
  static bool init = false;
  if (!init) {
    init = true;
    for (const char* const* k = kWaterFs; *k; k++) g.keys.insert(*k);
    if (const char* e = getenv("SNAP_WATER_FS")) {
      std::string s = e;
      for (size_t i = 0; i < s.size();) { size_t j = s.find(',', i); if (j == std::string::npos) j = s.size(); if (j > i) g.keys.insert(s.substr(i, std::min<size_t>(j - i, 12))); i = j + 1; }
    }
  }
  return key && g.keys.count(key) != 0;
}

// ---- GPU time (SNAP_WATER_TIME=1): the copies and the pass, two intervals per frame, summed ------------------------------------------------
struct Timing {
  bool init = false, on = false; VkQueryPool pool = VK_NULL_HANDLE; float period = 1.f;
  static constexpr int kSlots = 16;
  bool pending[kSlots] = {}; int issued[kSlots] = {}; int next = 0, cur = -1;
  double sumMs = 0, maxMs = 0; unsigned long long n = 0;
} T;
void TimeInit(ReplayCtx& cx) {
  if (T.init) return;
  T.init = true; const char* e = getenv("SNAP_WATER_TIME"); T.on = e && *e && atoi(e) != 0;
  if (!T.on) return;
  VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(cx.h->phys, &pr); T.period = pr.limits.timestampPeriod;
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = Timing::kSlots * 4;
  if (pr.limits.timestampComputeAndGraphics != VK_TRUE || vkCreateQueryPool(cx.h->device, &qi, nullptr, &T.pool) != VK_SUCCESS) T.on = false;
}
void TimeHarvest(ReplayCtx& cx) {
  for (int i = 0; i < Timing::kSlots; i++) {
    if (!T.pending[i] || g_presentCount - T.issued[i] < 3) continue;
    uint64_t v[8] = {};
    if (vkGetQueryPoolResults(cx.h->device, T.pool, (uint32_t)i * 4, 4, sizeof v, v, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS) continue;
    if (!v[1] || !v[3] || !v[5] || !v[7]) continue;
    T.pending[i] = false;
    const double ms = (double)((v[2] - v[0]) + (v[6] - v[4])) * T.period * 1e-6;
    T.sumMs += ms; T.maxMs = std::max(T.maxMs, ms);
    if (++T.n % 300 == 0) fprintf(stderr, "water: GPU time of copies + pass: avg %.3f ms, max %.3f ms over %llu frames\n", T.sumMs / (double)T.n, T.maxMs, T.n);
  }
}
void TimeStamp(ReplayCtx& cx, int which) {   // 0/1: copies begin/end, 2/3: pass begin/end
  if (!T.on) return;
  if (which == 0) {
    TimeHarvest(cx);
    const int s = T.next++ % Timing::kSlots;
    if (T.pending[s]) { T.cur = -1; return; }
    T.cur = s; vkCmdResetQueryPool(cx.cb, T.pool, (uint32_t)s * 4, 4);
  }
  if (T.cur < 0) return;
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, (uint32_t)T.cur * 4 + (uint32_t)which);
  if (which == 3) { T.pending[T.cur] = true; T.issued[T.cur] = g_presentCount; T.cur = -1; }
}

// ---- images and pipeline ---------------------------------------------------------------------------------------------------------------------
void ImgBar(VkCommandBuffer cb, Img& im, VkImageLayout to) {
  if (im.layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = im.layout; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img; b.subresourceRange = {im.aspect, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  im.layout = to;
}
bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt, VkImageAspectFlags aspect) {
  if (o.img || o.view || o.mem) { RetireVkImage(h, o.img, o.view, o.mem); o = Img{}; }   // a size change: the old image goes once the GPU is past it
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  o = Img{}; o.aspect = aspect;
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {aspect, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}

VkPipeline MakePipeline(Host* h, VkFormat fmt, VkShaderModule fsMod) {
  VkShaderModule vs = FxFullscreenVs(h);
  if (!vs || !fsMod) return VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo st2[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsMod, "main", nullptr}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};   // dst.rgb = src.rgb + dst.rgb * src.a, alpha untouched (RGBA16F: factors are not clamped)
  cba.blendEnable = VK_TRUE; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st2; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
  gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = g.pl;
  VkPipeline p = VK_NULL_HANDLE;
  const VkResult r = vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &p);
  if (r != VK_SUCCESS) { fprintf(stderr, "water: vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
  return p;
}

bool Setup(Host* h, VkFormat fmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(h->device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // the ocean maps tile
    if (vkCreateSampler(h->device, &si, nullptr, &g.repeat) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 6; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Pc)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    const DevSpv sp = dev_spv("water_fx", kWaterFxFrag, sizeof kWaterFxFrag);
    g.fs = FxMakeModule(h, sp.words, sp.bytes);
    if (!g.fs) return false;
    g.ready = true;
  }
  if (g.ready && dev_spv_changed("water_fx", &g.seenSpv)) {   // hot reload (SNAP_DEV): the old module and pipeline are leaked, a frame in flight may still use them
    const DevSpv sp = dev_spv("water_fx", kWaterFxFrag, sizeof kWaterFxFrag);
    if (VkShaderModule m = FxMakeModule(h, sp.words, sp.bytes)) { g.fs = m; g.pipe = VK_NULL_HANDLE; }
  }
  if (g.fmt != fmt || !g.pipe) { g.pipe = MakePipeline(h, fmt, g.fs); g.pipeRt = VK_NULL_HANDLE; g.fmt = fmt; }
  if (!g.pipeRt && !g.rtFailed && snap_rt::On() && snap_rt::ReflectOn()) {   // the RT reflection variant (needs the ray query device feature: only with SNAP_RT_REFLECT at launch)
    if (!g.fsRt) { VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kWaterFxRtFrag; mi.pCode = kWaterFxRtFrag; if (vkCreateShaderModule(h->device, &mi, nullptr, &g.fsRt) != VK_SUCCESS) g.fsRt = VK_NULL_HANDLE; }
    g.pipeRt = g.fsRt ? MakePipeline(h, fmt, g.fsRt) : VK_NULL_HANDLE;
    if (!g.pipeRt) { g.rtFailed = true; fprintf(stderr, "[rt] reflections fall back to screen-space: the water pass RT pipeline could not be made\n"); }
  }
  return g.pipe != VK_NULL_HANDLE;
}

bool EnsureImages(Host* h, uint32_t w, uint32_t hh, uint32_t hw, uint32_t hhh, VkFormat colFmt) {
  if (g.w == w && g.h == hh && g.hw == hw && g.hh == hhh && g.col.img) return true;
  if (g.col.img) fprintf(stderr, "water: render size changed to %ux%u\n", w, hh);   // a command buffer may still read them
  if (!MakeImg(h, g.col, w, hh, colFmt, VK_IMAGE_ASPECT_COLOR_BIT) || !MakeImg(h, g.hdPre, hw, hhh, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
      !MakeImg(h, g.hdPost, hw, hhh, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
  g.w = w; g.h = hh; g.hw = hw; g.hh = hhh;
  return true;
}

// ---- the steps ---------------------------------------------------------------------------------------------------------------------------------
VkImageLayout OrigLayout(TextureImpl* t, VkImageLayout attach) { return t->subLayout.empty() ? t->layout : attach; }

// Copy the half-resolution water-pass depth hz into dst (SHADER_READ_ONLY afterwards).
void CopyDepthOut(ReplayCtx& cx, TextureImpl* hz, Img& dst) {
  VkCommandBuffer cb = cx.cb;
  const VkImageLayout orig = OrigLayout(hz, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  Transition(cb, hz, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ImgBar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy rd{}; rd.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; rd.dstSubresource = rd.srcSubresource; rd.extent = {g.hw, g.hh, 1};
  vkCmdCopyImage(cb, hz->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rd);
  ImgBar(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(cb, hz, orig);
}

// Copy g.hdPre back over hz: only water draws wrote depth since hdPre was taken, so this removes exactly their (forced) depth writes.
void RestoreDepth(ReplayCtx& cx, TextureImpl* hz) {
  VkCommandBuffer cb = cx.cb;
  const VkImageLayout orig = OrigLayout(hz, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  Transition(cb, hz, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  ImgBar(cb, g.hdPre, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkImageCopy rd{}; rd.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; rd.dstSubresource = rd.srcSubresource; rd.extent = {g.hw, g.hh, 1};
  vkCmdCopyImage(cb, g.hdPre.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hz->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rd);
  ImgBar(cb, g.hdPre, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(cb, hz, orig);
}

// Copy the scene colour (what the reflections sample) into g.col.
void CopySceneColour(ReplayCtx& cx, TextureImpl* c) {
  VkCommandBuffer cb = cx.cb;
  const VkImageLayout orig = OrigLayout(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  Transition(cb, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ImgBar(cb, g.col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy rc{}; rc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rc.dstSubresource = rc.srcSubresource; rc.extent = {g.w, g.h, 1};
  vkCmdCopyImage(cb, c->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.col.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
  ImgBar(cb, g.col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(cb, c, orig);
}

// The effect pass over the scene colour c; z is the full-resolution scene depth (what is behind the sea).
void RunPass(ReplayCtx& cx, TextureImpl* c, TextureImpl* z) {
  const FxCam* cam = FxCamera();
  if (!cam) return;
  TimeStamp(cx, 2);
  dev_pass_begin(cx.cb, "water fx");
  VkImageView oceanN = VK_NULL_HANDLE, oceanF = VK_NULL_HANDLE;   // FFT ocean maps (nvn_ocean.cpp), or 1x1 dummies when the ocean is off
  bool ocean = false;
  if (On(3)) { EndRendering(cx); ocean = OceanMaps(cx, &oceanN, &oceanF); }
  if (!ocean) OceanDummyViews(cx, &oceanN, &oceanF);
  Pc pc{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pc.vp[k * 4 + r] = (float)cam->vp[r][k];
  float jx, jy; FxJitter(&jx, &jy);
  pc.size[0] = (float)c->HW(); pc.size[1] = (float)c->HH(); pc.size[2] = jx; pc.size[3] = jy;
  int flags = (On(0) ? 1 : 0) | (On(1) ? 2 : 0) | (On(2) ? 4 : 0) | (ocean ? 1024 : 0);
  snap_rt::ReflInfo rinfo;   // RT reflections: the colour cache update is recorded here (outside any render pass), the shader traces the rays
  const bool rt = (flags & 1) != 0 && g.pipeRt && snap_rt::ReflectionPass(cx, g.col.view, g.linear, cam->vp, &rinfo);
  if (rt) flags |= 256;
  const int dbg = tun.debug;
  if (dbg == 1) flags = 8; else if (dbg == 2) flags = 16; else if (dbg == 3) flags = 32; else if (dbg == 4) flags = 64; else if (dbg == 5) flags = 128; else if (dbg == 6) flags = 2048 | (ocean ? 1024 : 0);
  pc.tlas = rt ? rinfo.tlas : 0; pc.avg = rt ? rinfo.avg : 0;
  pc.k[0] = cam->nearZ; pc.k[1] = (float)g_presentCount / 60.0f; pc.k[2] = (float)flags; pc.k[3] = ocean ? tun.oceanStrength : tun.ripple;
  pc.s[0] = tun.ssrStrength; pc.s[1] = tun.causticsStrength; pc.s[2] = tun.foamStrength; pc.s[3] = tun.ssrDist;
  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  VkDescriptorSet set = FxAllocSet(cx, g.dsl);
  if (set && dv && oceanN && oceanF) {
    VkDescriptorImageInfo ii[6] = {{g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {g.nearest, g.hdPost.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {g.linear, g.col.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {g.nearest, g.hdPre.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {g.repeat, oceanN, VK_IMAGE_LAYOUT_GENERAL}, {g.repeat, oceanF, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < 6; i++) { w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(cx.h->device, 6, w, 0, nullptr);
    FxDrawFullscreen(cx, c, rt ? g.pipeRt : g.pipe, g.pl, set, &pc, sizeof pc);
    FxNoteRan();
    st.ranPasses++;
  }
  FxDepthDone(cx, z, prev);
  dev_pass_end(cx.cb, "water fx");
  TimeStamp(cx, 3);
}

bool g_forceDw = false;   // set by WaterBeforeDraw for the draw being recorded

void NewFrame() {
  if (st.frame == g_presentCount) return;
  if (Log() && st.frame >= 0 && st.waterDraws) { static int n; if (n++ % 120 == 0) fprintf(stderr, "water: p%d %d water draws, %d pass(es)\n", st.frame, st.waterDraws, st.ranPasses); }
  if (Log2() && st.frame >= 0) fprintf(stderr, "water: p%d draws=%d water=%d passes=%d mask=%d armed=%d why=%s\n", st.frame, st.draws, st.waterDraws, st.ranPasses, (int)st.haveMask, (int)st.armed, st.why);
  st = S{}; st.frame = g_presentCount;
}

// Preconditions shared by both hooks: a live scene frame, the effect configured and Fx-eligible, the course camera (reversed infinite depth).
bool SceneReady(ReplayCtx& cx, TextureImpl** c, TextureImpl** z) {
  *z = FxSceneDepth(); *c = FxSceneColor();
  if (!*z || !*c) { st.why = "nodepth/colour"; return false; }
  if (FxIsPhotoFrame() || cx.occOpen) { st.why = "photo/occ"; return false; }
  const FxCam* cam = FxCamera();
  if (!cam || !cam->invDepth || !cam->infFar) { st.why = "camera"; return false; }
  if ((*c)->HW() != (*z)->HW() || (*c)->HH() != (*z)->HH() || !FxWritable(*c)) { st.why = "size/writable"; return false; }
  return true;
}

void Pre(ReplayCtx& cx) {   // Fx pre-tonemap slot: the composite happened but no later draw ran the pass
  NewFrame();
  if (!st.armed) return;
  st.armed = false;
  TextureImpl *c, *z;
  if (!SceneReady(cx, &c, &z)) return;
  RunPass(cx, c, z);
}

const FxEffect kEffect = {"water_fx", Enabled, Pre, nullptr, false};
struct Reg { Reg() { FxRegister(&kEffect); } } reg;
}  // namespace

bool WaterBeforeDraw(ReplayCtx& cx, const char* fsKey, bool blend, bool depthWrite) {
  g_forceDw = false;
  if (!AnyOn()) return false;
  RegisterTunables();
  RegisterSwitches();
  NewFrame();
  if (st.skip) return false;
  TextureImpl *c, *z;
  st.draws++;
  if (!SceneReady(cx, &c, &z)) return false;
  if (!snaplight::ab_on((unsigned long long)g_presentCount)) return false;
  if (Log()) {   // every distinct scene fragment key once per 600 presents, with its state and target
    static std::unordered_map<std::string, int> seen;
    const std::string k = fsKey ? fsKey : "-";
    auto it = seen.find(k);
    if (it == seen.end() || g_presentCount - it->second >= 600) {
      seen[k] = g_presentCount;
      fprintf(stderr, "water: p%d draw fs=%s blend=%d depthWrite=%d water=%d rt0=%ux%u\n", g_presentCount, k.c_str(), (int)blend, (int)depthWrite, (int)IsWaterFs(fsKey), cx.rt[0] ? cx.rt[0]->HW() : 0u, cx.rt[0] ? cx.rt[0]->HH() : 0u);
    }
  }
  // a pending pass from the composite one draw ago
  if (st.armed) {
    st.armed = false;
    EndRendering(cx);
    RunPass(cx, c, z);
    return true;
  }
  TextureImpl* t0 = cx.rt[0]; TextureImpl* hz = cx.rt[8];
  const bool halfPass = t0 && hz && t0 != c && hz != z && t0->HW() * 2 + 1 >= c->HW() && t0->HW() * 2 <= c->HW() + 1 && t0->HH() * 2 + 1 >= c->HH() && t0->HH() * 2 <= c->HH() + 1 &&
                        hz->HW() == t0->HW() && hz->HH() == t0->HH();
  const bool water = IsWaterFs(fsKey) && halfPass;
  if (water) {
    st.waterDraws++;
    if (st.inWater) { g_forceDw = true; return false; }
    if (!hz->EnsureImage() || hz->fi->vk != VK_FORMAT_D32_SFLOAT || !c->EnsureImage()) return false;
    TimeInit(cx);
    if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH(), hz->HW(), hz->HH(), c->fi->vk)) { g.failed = true; fprintf(stderr, "water: setup failed, disabled\n"); return false; }
    EndRendering(cx);
    { VkImageView dn, df; OceanDummyViews(cx, &dn, &df); }   // the 1x1 placeholder maps for the ocean bindings, made once (outside a render pass)
    TimeStamp(cx, 0);
    dev_pass_begin(cx.cb, "water copy");
    CopyDepthOut(cx, hz, g.hdPre);
    dev_pass_end(cx.cb, "water copy");
    TimeStamp(cx, 1);
    st.inWater = true; st.hz = hz; g_forceDw = true;
    return true;
  }
  bool rec = false;
  if (st.inWater) {   // the first draw after the water: keep the water surface depth, put the game's depth back
    st.inWater = false;
    EndRendering(cx);
    dev_pass_begin(cx.cb, "water depth");
    CopyDepthOut(cx, st.hz, g.hdPost);
    RestoreDepth(cx, st.hz);
    dev_pass_end(cx.cb, "water depth");
    st.haveMask = true; rec = true;
  }
  if (st.haveMask && t0 == c) {   // the composite of the half-resolution buffer into the scene: copy the scene colour first, run the pass after it
    st.haveMask = false;
    if (!rec) EndRendering(cx);
    CopySceneColour(cx, c);
    st.armed = true;
    return true;
  }
  return rec;
}
bool WaterForceDepthWrite() { return g_forceDw; }
}  // namespace nvn

extern "C" void snap_water_fx_set(int which, int on) {
  if (which < 0 || which > 3) return;
  nvn::g_on[which].store(on ? 1 : 0);
  fprintf(stderr, "water: %s %s (live)\n", nvn::kEnvName[which], on ? "on" : "off");
}
extern "C" int snap_water_fx_get(int which) { return which >= 0 && which <= 3 && nvn::On(which) ? 1 : 0; }
