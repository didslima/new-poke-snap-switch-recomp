// SPDX-License-Identifier: MIT
// Atmosphere effects (project code, ours): six display-only pre-tonemap effects, each with its own switch, all OFF by default.
//   SNAP_ATMOS_SHAFTS=1    light shafts: a half-resolution screen-space radial march toward the sun's screen position over the sky mask (the sun sits at a set angle from the view direction)
//   SNAP_ATMOS_FOG=1       height fog: distance and height falloff, tinted by the sky colour seen this frame, thicker toward the ground under the view
//   SNAP_ATMOS_EXPOSURE=1  auto exposure (eye adaptation): log-average luminance of the scene, smoothed over about 0.7 s, applied as a gain on the HDR colour
//   SNAP_ATMOS_BLOOM=1     bloom: a 6-level mip chain (13-tap downsample, tent upsample) of the HDR highlights above a soft knee, added back at a low strength
//   SNAP_ATMOS_CLOUDS=1    cloud shadows: drifting noise-based shadows on every surface below the clouds (steep faces get less), faded with distance, world-space wind
//   SNAP_ATMOS_SKY=1       sky gradient: warm glow toward the (camera-relative) sun, pale haze at the horizon, a deeper zenith, on sky pixels only
//   SNAP_ATMOS=1           turns all six on (an explicit SNAP_ATMOS_<NAME>=0 still wins)
// Where: the Fx pre-tonemap slot (nvn_fx.cpp), on the linear RGBA16F scene colour before the upscaler and the game's tonemap read it, after the Enhanced
// lighting pass. The slot never runs on photo or Evaluation frames (FxIsPhotoFrame) nor inside an occlusion query, so photos and their scores never see
// any of it; Pre() checks that again.   SNAP_LIGHT_AB=<P> (see lighting_preset.h) turns the effects on in odd blocks of P presents only (same-view A/B).
// Passes per scene frame: bloom chain down (2 levels, or 6 with bloom) + up (5), statistics A (16x9 tiles) + B (2x1 smoothed state), shaft march (half res),
// one composite on the scene colour for all four (blend ONE / ONE_MINUS_SRC_ALPHA, see atmos_comp.frag). Every pass is a full-screen triangle.
// Tunables (env, test aids): SNAP_ATMOS_SHAFT_INTENSITY (0.9), _DECAY (0.95 per step along the ray), _LENGTH (0.45 of the screen height), _STEPS (24), _G (0.55), _AIR (0.03 per unit of distance), _CONTRAST (2, power on the open-sky fraction), _AZIMUTH (25 degrees right of the view), _ELEVATION (45), _SUN (r,g,b);
//   SNAP_ATMOS_FOG_DENSITY (0.002), _FALLOFF (0.12 per unit), _BASE (0, offset of the fog's zero level above the ground), _MAX (0.55), _START (4);
//   SNAP_ATMOS_EXPOSURE_KEY (1, log2 of the mean luminance the exposure aims at; the game's daylight mean is about 2.4 = 1.25), _STRENGTH (0.5), _MIN (0.6), _MAX (2), _TIME (0.7 s);
//   SNAP_ATMOS_BLOOM_STRENGTH (0.2), _THRESHOLD (1.5, times the adapted mean luminance), _KNEE (0.5, same unit);
//   SNAP_ATMOS_CLOUD_STRENGTH (0.3), _SCALE (120 world units per noise cell), _SPEED (15 units/s), _COVER (0.55), _FADE (4000 units);
//   SNAP_ATMOS_SKY_STRENGTH (1, scales all sky terms), _HAZE (0.25), _GLOW (0.35), _ZENITH (0.15, darkening overhead); the sun is the shafts' (_AZIMUTH, _ELEVATION);
//   every tunable is a dev_envf value: with SNAP_DEV=1 it shows in the F1 tuning page and ctl `set`;
//   SNAP_ATMOS_DEBUG=1 shafts alone, 8 shafts diagnosis (red: sun behind the camera, green: no sun, blue: open sky along the ray), 3 bloom alone, 4 fog amount, 5 exposure gain, 6 cloud cover / sky glow; SNAP_ATMOS_LOG=1 state log.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/lighting_preset.h"
#include "nvn_atmos_spv.h"
#include "nvn_dev.h"
#include "nvn_fx.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
constexpr int kLevels = 6;
constexpr uint32_t kStride = 512, kSlots = 16;

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; uint32_t w = 0, h = 0; };
struct U {   // std140, all vec4 (atmos_common.glsl)
  float ivp[16], vp[16], eye[4], light[4], size[4], cam[4], fog[4], fogB[4], shaft[4], shaftB[4], sunCol[4], expo[4], expoB[4], bloom[4], flags[4], pass[4], dbg[4], cloud[4], cloudB[4], sky[4], flags2[4];
};
static_assert(sizeof(U) <= kStride && sizeof(U) % 4 == 0, "uniform slot");

struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule fs[6] = {};
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  VkPipeline pDown = VK_NULL_HANDLE, pUp = VK_NULL_HANDLE, pStats = VK_NULL_HANDLE, pState = VK_NULL_HANDLE, pShaft = VK_NULL_HANDLE, pComp = VK_NULL_HANDLE;
  VkFormat sceneFmt = VK_FORMAT_UNDEFINED;
  VkBuffer ubo = VK_NULL_HANDLE; VkDeviceMemory uboMem = VK_NULL_HANDLE;
  Img chain[kLevels], shaft, tile0, tile1, tile2, state[2];
  uint32_t w = 0, h = 0;
  int cur = 0; bool haveHist = false; int lastPresent = -1000;
  std::chrono::steady_clock::time_point lastT{}; bool haveT = false; double cloudTime = 0.0;
  // SNAP_ATMOS_LOG readback ring
  VkBuffer rb = VK_NULL_HANDLE; VkDeviceMemory rbMem = VK_NULL_HANDLE; float* rbPtr = nullptr; bool rbPend[4] = {}; int rbPresent[4] = {}; int rbNext = 0; int rbCount = 0;
} g;

float EnvF(const char* n, float def) { const char* e = getenv(n); return e && *e ? (float)atof(e) : def; }
bool EnvSet(const char* n) { const char* e = getenv(n); return e && *e; }
bool Want(const char* name) {
  const char* e = getenv(name);
  if (e && *e) return atoi(e) != 0;
  const char* m = getenv("SNAP_ATMOS");
  return m && *m && atoi(m) != 0;
}
struct Wants { bool shafts, fog, expo, bloom, clouds, sky; };
// The four switches are plain bools read every frame; with SNAP_DEV=1 they are live params (host\ab_capture.ps1 -Effect SNAP_ATMOS_SHAFTS -Values 0,1).
bool g_want[6];
Wants GetWants() {
  static std::atomic<unsigned> seen{0};   // live rows (hle/live.h): read at the first use and again when a setting changed
  static bool registered = false;
  if (hle::live::changed(seen)) {
    hle::live::EnvRead r;
    static const char* const names[6] = {"SNAP_ATMOS_SHAFTS", "SNAP_ATMOS_FOG", "SNAP_ATMOS_EXPOSURE", "SNAP_ATMOS_BLOOM", "SNAP_ATMOS_CLOUDS", "SNAP_ATMOS_SKY"};
    for (int i = 0; i < 6; i++) { g_want[i] = Want(names[i]); if (!registered) dev_param_b(names[i], &g_want[i], g_want[i]); }
    registered = true;
  }
  return {g_want[0], g_want[1], g_want[2], g_want[3], g_want[4], g_want[5]};
}
bool Enabled() {
  const Wants w = GetWants();
  return !g.failed && (w.shafts || w.fog || w.expo || w.bloom || w.clouds || w.sky) && snaplight::ab_on((unsigned long long)g_presentCount);
}

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
void MemBar(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}
bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt) {
  if (o.img || o.view || o.mem) { RetireVkImage(h, o.img, o.view, o.mem); o = Img{}; }   // a size change: the old image goes once the GPU is past it
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
bool MakeBuf(Host* h, VkBuffer& b, VkDeviceMemory& m, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(h->device, &bi, nullptr, &b) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, b, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, props);
  return vkAllocateMemory(h->device, &ai, nullptr, &m) == VK_SUCCESS && vkBindBufferMemory(h->device, b, m, 0) == VK_SUCCESS;
}

// blend: 0 none, 1 add (ONE, ONE), 2 composite (ONE, ONE_MINUS_SRC_ALPHA on rgb, alpha untouched)
VkPipeline MakePipe(Host* h, VkShaderModule fs, const VkFormat* fmts, int n, int blend) {
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
  VkPipelineColorBlendAttachmentState cba[3] = {};
  for (int i = 0; i < n; i++) {
    cba[i].colorWriteMask = 0xF;
    if (blend) {
      cba[i].blendEnable = VK_TRUE;
      cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      cba[i].dstColorBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba[i].colorBlendOp = VK_BLEND_OP_ADD;
      cba[i].srcAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO; cba[i].dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE; cba[i].alphaBlendOp = VK_BLEND_OP_ADD;
      if (blend == 2) cba[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    }
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = (uint32_t)n; cb.pAttachments = cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = (uint32_t)n; ri.pColorAttachmentFormats = fmts;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
  gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = g.pl;
  VkPipeline p = VK_NULL_HANDLE;
  const VkResult r = vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &p);
  if (r != VK_SUCCESS) { fprintf(stderr, "atmos: vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
  return p;
}

bool Setup(Host* h, VkFormat sceneFmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(h->device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 4; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 5; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    // dev_spv: with SNAP_DEV=1 and SNAP_SHADER_DIR set, <dir>/atmos_*.frag wins over the embedded copy (read at start; no live pipeline rebuild)
    auto mod = [&](const char* n, const uint32_t* w, size_t b) { const DevSpv s = dev_spv(n, w, b); return FxMakeModule(h, s.words, s.bytes); };
    g.fs[0] = mod("atmos_down", kAtmosDownFrag, sizeof kAtmosDownFrag);
    g.fs[1] = mod("atmos_up", kAtmosUpFrag, sizeof kAtmosUpFrag);
    g.fs[2] = mod("atmos_stats", kAtmosStatsFrag, sizeof kAtmosStatsFrag);
    g.fs[3] = mod("atmos_state", kAtmosStateFrag, sizeof kAtmosStateFrag);
    g.fs[4] = mod("atmos_shaft", kAtmosShaftFrag, sizeof kAtmosShaftFrag);
    g.fs[5] = mod("atmos_comp", kAtmosCompFrag, sizeof kAtmosCompFrag);
    for (VkShaderModule m : g.fs) if (!m) return false;
    const VkFormat f16 = VK_FORMAT_R16G16B16A16_SFLOAT, f32 = VK_FORMAT_R32G32B32A32_SFLOAT;
    const VkFormat statsF[3] = {f16, f32, f16};
    g.pDown = MakePipe(h, g.fs[0], &f16, 1, 0);
    g.pUp = MakePipe(h, g.fs[1], &f16, 1, 1);
    g.pStats = MakePipe(h, g.fs[2], statsF, 3, 0);
    g.pState = MakePipe(h, g.fs[3], &f32, 1, 0);
    g.pShaft = MakePipe(h, g.fs[4], &f16, 1, 0);
    if (!g.pDown || !g.pUp || !g.pStats || !g.pState || !g.pShaft) return false;
    if (!MakeBuf(h, g.ubo, g.uboMem, kStride * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return false;
    if (EnvSet("SNAP_ATMOS_LOG")) {
      if (!MakeBuf(h, g.rb, g.rbMem, 4 * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
          vkMapMemory(h->device, g.rbMem, 0, VK_WHOLE_SIZE, 0, (void**)&g.rbPtr) != VK_SUCCESS) g.rbPtr = nullptr;
    }
    g.ready = true;
  }
  if (g.sceneFmt != sceneFmt || !g.pComp) {   // the scene colour format is fixed (RGBA16F), so this runs once
    g.pComp = MakePipe(h, g.fs[5], &sceneFmt, 1, 2);
    g.sceneFmt = sceneFmt;
  }
  return g.pComp != VK_NULL_HANDLE;
}

bool EnsureImages(Host* h, uint32_t w, uint32_t hh) {
  if (g.w == w && g.h == hh && g.chain[0].img) return true;
  if (g.chain[0].img) fprintf(stderr, "atmos: render size changed to %ux%u\n", w, hh);   // a command buffer may still read them
  const VkFormat f16 = VK_FORMAT_R16G16B16A16_SFLOAT, f32 = VK_FORMAT_R32G32B32A32_SFLOAT;
  for (int i = 0; i < kLevels; i++) {
    const uint32_t s = 2u << i;
    if (!MakeImg(h, g.chain[i], std::max(1u, (w + s - 1) / s), std::max(1u, (hh + s - 1) / s), f16)) return false;
  }
  if (!MakeImg(h, g.shaft, g.chain[0].w, g.chain[0].h, f16) || !MakeImg(h, g.tile0, 16, 9, f16) || !MakeImg(h, g.tile1, 16, 9, f32) || !MakeImg(h, g.tile2, 16, 9, f16) ||
      !MakeImg(h, g.state[0], 2, 1, f32) || !MakeImg(h, g.state[1], 2, 1, f32)) return false;
  g.w = w; g.h = hh; g.haveHist = false; g.cur = 0;
  return true;
}

void WriteU(ReplayCtx& cx, uint32_t slot, const U& u) { vkCmdUpdateBuffer(cx.cb, g.ubo, (VkDeviceSize)slot * kStride, sizeof u, &u); }

struct Bnd { VkSampler s; VkImageView v; VkImageLayout l; };
VkDescriptorSet MakeSet(ReplayCtx& cx, std::initializer_list<Bnd> binds, uint32_t slot) {
  VkDescriptorSet set = FxAllocSet(cx, g.dsl);
  if (!set) return VK_NULL_HANDLE;
  VkDescriptorImageInfo ii[4]; VkWriteDescriptorSet w[5] = {}; uint32_t n = 0;
  for (const Bnd& b : binds) {
    ii[n] = {b.s, b.v, b.l};
    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[n].dstSet = set; w[n].dstBinding = n; w[n].descriptorCount = 1; w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ii[n];
    n++;
  }
  VkDescriptorBufferInfo bi{g.ubo, (VkDeviceSize)slot * kStride, sizeof(U)};
  w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[n].dstSet = set; w[n].dstBinding = 4; w[n].descriptorCount = 1; w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &bi;
  vkUpdateDescriptorSets(cx.h->device, n + 1, w, 0, nullptr);
  return set;
}

// One full-screen pass into 1..3 of our own images. load: keep the contents (additive passes), else they are not needed. The targets end in SHADER_READ_ONLY.
void DrawPass(ReplayCtx& cx, std::initializer_list<Img*> targets, bool load, VkPipeline pipe, VkDescriptorSet set) {
  if (!pipe || !set) return;
  VkCommandBuffer cb = cx.cb;
  VkRenderingAttachmentInfo ca[3]; uint32_t n = 0; const Img* t0 = nullptr;
  for (Img* t : targets) {
    ImgBar(cb, *t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ca[n] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca[n].imageView = t->view; ca[n].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca[n].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (!t0) t0 = t;
    n++;
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {t0->w, t0->h}}; ri.layerCount = 1; ri.colorAttachmentCount = n; ri.pColorAttachments = ca;
  vkCmdBeginRendering(cb, &ri);
  VkViewport vp{0, 0, (float)t0->w, (float)t0->h, 0, 1}; VkRect2D sc{{0, 0}, {t0->w, t0->h}};
  vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, nullptr);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  for (Img* t : targets) ImgBar(cb, *t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Harvest() {
  if (!g.rbPtr) return;
  for (int i = 0; i < 4; i++) {
    if (!g.rbPend[i] || g_presentCount - g.rbPresent[i] < 4) continue;
    g.rbPend[i] = false;
    if (g.rbCount++ % 20 != 0) continue;
    const float* v = g.rbPtr + i * 8;   // texel 0: fog rgb + adapted log2 lum; texel 1: log2 lum now, brightest sample, sky weight, ground height
    fprintf(stderr, "atmos: p%d fog colour (%.3f %.3f %.3f)  adapted log2 lum %.2f (%.3f)  now %.2f  brightest %.2f  sky weight %.0f  ground y %.2f\n", g.rbPresent[i], v[0], v[1], v[2], v[3], exp2f(v[3]), v[4], v[5], v[6], v[7]);
  }
}

void Pre(ReplayCtx& cx) {
  if (g.failed || FxIsPhotoFrame()) return;
  const Wants wn = GetWants();
  const FxCam* cam = FxCamera();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  if (!cam || !c || !z || !cam->invDepth || c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH())) { g.failed = true; fprintf(stderr, "atmos: setup failed, disabled\n"); return; }
  Harvest();

  // the sun's direction (shafts only). The game's shadow caster axis is not a fixed world sun (it changes with the camera's yaw), so the sun is
  // placed relative to the camera: SNAP_ATMOS_SHAFT_AZIMUTH degrees to the right of the view direction (0 = straight ahead), SNAP_ATMOS_SHAFT_ELEVATION above the horizon.
  const bool doShafts = wn.shafts, doClouds = wn.clouds, doSky = wn.sky;
  const int dbgMode = dev_envi("SNAP_ATMOS_DEBUG", 0, 0, 8);
  float L[3] = {0, 1, 0};
  if (doShafts || doSky) {   // the camera-relative sun (shafts and the sky gradient share it)
    double h[4]; const double pin[4] = {0.0, 0.0, 1e-6, 1.0};
    for (int r = 0; r < 4; r++) h[r] = cam->ivp[r][0] * pin[0] + cam->ivp[r][1] * pin[1] + cam->ivp[r][2] * pin[2] + cam->ivp[r][3] * pin[3];
    double fx = h[0] / h[3] - cam->eye[0], fz = h[2] / h[3] - cam->eye[2];
    const double fl = std::sqrt(fx * fx + fz * fz);
    if (fl > 1e-6) { fx /= fl; fz /= fl; } else { fx = 0.0; fz = 1.0; }
    const double az = (double)dev_envf("SNAP_ATMOS_SHAFT_AZIMUTH", 25.f, -180.f, 180.f) * 0.0174532925, el = (double)dev_envf("SNAP_ATMOS_SHAFT_ELEVATION", 45.f, 5.f, 85.f) * 0.0174532925;
    const double hx = fx * std::cos(az) - fz * std::sin(az), hz = fx * std::sin(az) + fz * std::cos(az);
    L[0] = (float)(std::cos(el) * hx); L[1] = (float)std::sin(el); L[2] = (float)(std::cos(el) * hz);
  }
  const bool doFog = wn.fog, doExpo = wn.expo, doBloom = wn.bloom;
  if (!doShafts && !doFog && !doExpo && !doBloom && !doClouds && !doSky) return;
  const bool needStats = true;   // every effect reads the mean luminance or the sky colour state   // bloom and shafts need the mean luminance (relative threshold, scene scale)
  const int nLev = doBloom ? kLevels : (needStats ? 2 : 0);

  const auto now = std::chrono::steady_clock::now();
  float dt = g.haveT ? std::chrono::duration<float>(now - g.lastT).count() : 0.05f;
  dt = std::min(0.25f, std::max(0.001f, dt));
  g.lastT = now; g.haveT = true; g.cloudTime += dt;
  const bool histOk = g.haveHist && g_presentCount - g.lastPresent <= 120;

  float jx, jy; FxJitter(&jx, &jy);
  U u{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) u.ivp[k * 4 + r] = (float)cam->ivp[r][k];
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) u.vp[k * 4 + r] = (float)cam->vp[r][k];
  for (int i = 0; i < 3; i++) { u.eye[i] = (float)cam->eye[i]; u.light[i] = L[i]; }
  u.light[3] = (float)(g_presentCount & 1023);
  u.size[0] = (float)g.w; u.size[1] = (float)g.h; u.size[2] = jx; u.size[3] = jy;
  u.cam[0] = cam->nearZ; u.cam[1] = (doShafts || doSky) ? 1.f : 0.f; u.cam[2] = dt; u.cam[3] = histOk ? 1.f : 0.f;
  // every tunable goes through dev_envf: plain getenv without SNAP_DEV, a live param (F1 Tuning page, ctl `set`) with it
  u.fog[0] = dev_envf("SNAP_ATMOS_FOG_DENSITY", 0.002f, 0.f, 0.1f); u.fog[1] = dev_envf("SNAP_ATMOS_FOG_FALLOFF", 0.12f, 0.f, 1.f);
  u.fog[2] = dev_envf("SNAP_ATMOS_FOG_BASE", 0.f, -50.f, 50.f); u.fog[3] = dev_envf("SNAP_ATMOS_FOG_MAX", 0.55f, 0.f, 1.f);
  u.fogB[0] = dev_envf("SNAP_ATMOS_FOG_START", 4.f, 0.f, 100.f);
  u.shaft[0] = dev_envf("SNAP_ATMOS_SHAFT_INTENSITY", 0.9f, 0.f, 8.f); u.shaft[1] = dev_envf("SNAP_ATMOS_SHAFT_DECAY", 0.95f, 0.5f, 1.f);
  u.shaft[2] = dev_envf("SNAP_ATMOS_SHAFT_LENGTH", 0.45f, 0.05f, 1.5f);
  u.shaft[3] = (float)dev_envi("SNAP_ATMOS_SHAFT_STEPS", 24, 4, 64);
  u.shaftB[0] = dev_envf("SNAP_ATMOS_SHAFT_G", 0.55f, 0.f, 0.95f); u.shaftB[1] = dev_envf("SNAP_ATMOS_SHAFT_AIR", 0.03f, 0.f, 1.f);
  u.shaftB[2] = dev_envf("SNAP_ATMOS_SHAFT_CONTRAST", 2.0f, 0.5f, 6.f);
  u.shaftB[3] = dev_envf("SNAP_ATMOS_SHAFT_NIGHT", 0.08f, -1.f, 4.f);   // sky luminance below which there is no sun to make shafts (night courses); 0 = never fade
  u.sunCol[0] = 1.0f; u.sunCol[1] = 0.88f; u.sunCol[2] = 0.65f;
  if (const char* sc = getenv("SNAP_ATMOS_SUN")) { float a, b, d; if (sscanf(sc, "%f,%f,%f", &a, &b, &d) == 3) { u.sunCol[0] = a; u.sunCol[1] = b; u.sunCol[2] = d; } }
  u.expo[0] = dev_envf("SNAP_ATMOS_EXPOSURE_KEY", 1.f, -8.f, 4.f); u.expo[1] = dev_envf("SNAP_ATMOS_EXPOSURE_STRENGTH", 0.5f, 0.f, 1.f);
  u.expo[2] = dev_envf("SNAP_ATMOS_EXPOSURE_MIN", 0.6f, 0.1f, 1.f); u.expo[3] = dev_envf("SNAP_ATMOS_EXPOSURE_MAX", 2.f, 1.f, 8.f);
  u.expoB[0] = dev_envf("SNAP_ATMOS_EXPOSURE_TIME", 0.7f, 0.05f, 5.f);
  u.bloom[0] = dev_envf("SNAP_ATMOS_BLOOM_STRENGTH", 0.2f, 0.f, 1.f); u.bloom[1] = dev_envf("SNAP_ATMOS_BLOOM_THRESHOLD", 1.5f, 0.f, 16.f); u.bloom[2] = dev_envf("SNAP_ATMOS_BLOOM_KNEE", 0.5f, 0.f, 1.f);
  u.flags[0] = doFog; u.flags[1] = doShafts; u.flags[2] = doExpo; u.flags[3] = doBloom;
  u.dbg[0] = (float)dbgMode;
  u.cloud[0] = dev_envf("SNAP_ATMOS_CLOUD_STRENGTH", 0.3f, 0.f, 1.f); u.cloud[1] = dev_envf("SNAP_ATMOS_CLOUD_SCALE", 120.f, 5.f, 5000.f);
  u.cloud[2] = dev_envf("SNAP_ATMOS_CLOUD_SPEED", 15.f, 0.f, 500.f); u.cloud[3] = dev_envf("SNAP_ATMOS_CLOUD_COVER", 0.55f, 0.f, 1.f);
  u.cloudB[0] = (float)std::fmod(g.cloudTime, 100000.0); u.cloudB[1] = dev_envf("SNAP_ATMOS_CLOUD_FADE", 4000.f, 20.f, 40000.f);
  u.sky[0] = dev_envf("SNAP_ATMOS_SKY_STRENGTH", 1.0f, 0.f, 3.f); u.sky[1] = dev_envf("SNAP_ATMOS_SKY_HAZE", 0.25f, 0.f, 2.f);
  u.sky[2] = dev_envf("SNAP_ATMOS_SKY_GLOW", 0.35f, 0.f, 3.f); u.sky[3] = dev_envf("SNAP_ATMOS_SKY_ZENITH", 0.15f, 0.f, 0.8f);
  u.flags2[0] = doClouds; u.flags2[1] = doSky;

  // all uniform slots first (slot 0 main, 1+i downsample i, 7+i upsample i), one barrier, then the passes
  MemBar(cx.cb);
  WriteU(cx, 0, u);
  for (int i = 0; i < nLev; i++) { U d = u; d.pass[0] = i == 0 ? 1.f : 0.f; d.pass[2] = (float)g.chain[i].w; d.pass[3] = (float)g.chain[i].h; WriteU(cx, 1 + i, d); }
  if (doBloom) for (int i = 0; i < nLev - 1; i++) { U d = u; d.pass[2] = (float)g.chain[i].w; d.pass[3] = (float)g.chain[i].h; WriteU(cx, 7 + i, d); }
  MemBar(cx.cb);

  VkImageLayout prevZ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageView dv = FxDepthRead(cx, z, &prevZ);
  Transition(cx.cb, c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkImageView cv = c->View(nullptr, true);
  const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, DRO = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  bool ok = dv && cv;
  if (ok) {
    dev_pass_begin(cx.cb, "atmos bloom");
    {   // the bloom prefilter reads last frame's statistics (its threshold is relative to the mean luminance); bound to every level, only the first reads it
      Img& prevSt = g.state[g.cur ^ 1];
      if (prevSt.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, prevSt, RO);
    }
    for (int i = 0; i < nLev; i++) {   // chain down
      VkDescriptorSet s = MakeSet(cx, {{g.linear, i == 0 ? cv : g.chain[i - 1].view, RO}, {g.nearest, g.state[g.cur ^ 1].view, RO}}, 1 + i);
      DrawPass(cx, {&g.chain[i]}, false, g.pDown, s);
    }
    if (doBloom) for (int i = nLev - 2; i >= 0; i--) {   // chain up, additive into the larger level
      VkDescriptorSet s = MakeSet(cx, {{g.linear, g.chain[i + 1].view, RO}}, 7 + i);
      DrawPass(cx, {&g.chain[i]}, true, g.pUp, s);
    }
    dev_pass_begin(cx.cb, "atmos stats");
    Img& st = g.state[g.cur]; Img& old = g.state[g.cur ^ 1];
    if (needStats) {
      if (old.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, old, RO);   // never written yet: its contents are not read while history is off
      DrawPass(cx, {&g.tile0, &g.tile1, &g.tile2}, false, g.pStats, MakeSet(cx, {{g.nearest, dv, DRO}, {g.linear, cv, RO}}, 0));   // the scene colour itself: the chain's first level is thresholded
      DrawPass(cx, {&st}, false, g.pState, MakeSet(cx, {{g.nearest, g.tile0.view, RO}, {g.nearest, g.tile1.view, RO}, {g.nearest, g.tile2.view, RO}, {g.nearest, old.view, RO}}, 0));
      if (g.rbPtr) {
        const int slot = g.rbNext++ & 3;
        if (!g.rbPend[slot]) {
          ImgBar(cx.cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          VkBufferImageCopy cp{}; cp.bufferOffset = (VkDeviceSize)slot * 32; cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.imageExtent = {2, 1, 1};
          vkCmdCopyImageToBuffer(cx.cb, st.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.rb, 1, &cp);
          ImgBar(cx.cb, st, RO);
          g.rbPend[slot] = true; g.rbPresent[slot] = g_presentCount;
        }
      }
    } else {
      if (st.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, st, RO);   // the composite always binds a state image
    }
    dev_pass_begin(cx.cb, "atmos shafts");
    if (doShafts) DrawPass(cx, {&g.shaft}, false, g.pShaft, MakeSet(cx, {{g.nearest, dv, DRO}}, 0));
    dev_pass_begin(cx.cb, "atmos composite");
    if (g.shaft.layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, g.shaft, RO);
    if (g.chain[0].layout == VK_IMAGE_LAYOUT_UNDEFINED) ImgBar(cx.cb, g.chain[0], RO);
    VkDescriptorSet sc = MakeSet(cx, {{g.nearest, dv, DRO}, {g.nearest, st.view, RO}, {g.nearest, g.shaft.view, RO}, {g.linear, g.chain[0].view, RO}}, 0);
    FxDrawFullscreen(cx, c, g.pComp, g.pl, sc, nullptr, 0);   // moves the scene colour back to COLOR_ATTACHMENT
    dev_pass_end(cx.cb, "atmos composite");
    FxNoteRan();
    if (needStats) { g.haveHist = true; g.lastPresent = g_presentCount; g.cur ^= 1; }
    if (doShafts) {
      double cw = 0, cxn = 0, cyn = 0, ex[3] = {cam->eye[0] + L[0] * 1000.0, cam->eye[1] + L[1] * 1000.0, cam->eye[2] + L[2] * 1000.0};
      cxn = cam->vp[0][0] * ex[0] + cam->vp[0][1] * ex[1] + cam->vp[0][2] * ex[2] + cam->vp[0][3];
      cyn = cam->vp[1][0] * ex[0] + cam->vp[1][1] * ex[1] + cam->vp[1][2] * ex[2] + cam->vp[1][3];
      cw = cam->vp[3][0] * ex[0] + cam->vp[3][1] * ex[1] + cam->vp[3][2] * ex[2] + cam->vp[3][3];
      static int sl; if (sl++ % 90 == 0) fprintf(stderr, "atmos: sun clip w %.3f ndc (%.3f %.3f) L (%.3f %.3f %.3f) eye (%.1f %.1f %.1f)\n", cw, cw != 0 ? cxn / cw : 0.0, cw != 0 ? cyn / cw : 0.0, L[0], L[1], L[2], cam->eye[0], cam->eye[1], cam->eye[2]);
    }
    static int logged; if (logged++ < 3) fprintf(stderr, "atmos: p%d %ux%u shafts %d fog %d exposure %d bloom %d clouds %d sky %d (levels %d) debug %d light toward (%.3f %.3f %.3f) history %d dt %.1f ms\n", g_presentCount, g.w, g.h, (int)doShafts, (int)doFog, (int)doExpo, (int)doBloom, (int)doClouds, (int)doSky, nLev, dbgMode, L[0], L[1], L[2], (int)histOk, dt * 1000.f);
  }
  FxDepthDone(cx, z, prevZ);
}

const FxEffect kEffect = {"atmosphere", Enabled, Pre, nullptr, false};
struct Reg { Reg() { FxRegister(&kEffect); } } reg;
}  // namespace
}  // namespace nvn
