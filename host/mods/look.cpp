// Color looks (mod #7 part 2) (project code, ours): a host-side tone/tint pass on the final output image, never on a game render target.
//
//   SNAP_LOOK=<preset>[:<intensity>]   preset neutral | vivid | warm | cool, intensity 0..1.5 (default 1; 0 or neutral = off)
//   SNAP_ACCESS_FLASH=off|gentle|standard|strong   Reduce flashing: one gain per frame limits how fast the frame's mean brightness may change (below)
//   SNAP_ACCESS_CVD / SNAP_ACCESS_CVD_STRENGTH   color-vision correction in the same pass (hle/access.h, docs/accessibility.md); the F1 menu draws after
//                                                this pass, so it is never corrected; photos and Evaluation read other images and are never touched
//
// snap_look_present() is called by the NVN present path once per present, after the scene has been blitted to the swapchain image and before
// the overlay (analysis/mods/nvn_hook_requests.md has the diff). The image is in TRANSFER_DST_OPTIMAL and must be left there. The pass copies it
// to a scratch image and redraws it through a fragment shader (GLSL in shaders/, SPIR-V in look_spv.h). The photo capture and the Evaluation
// pass read the game's own render targets, which this never touches, so scoring and saved photos are unchanged.
#include <windows.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "../../runtime/hle/access.h"
#include "../../runtime/nvn/nvn_dev.h"
#include "../../runtime/ui/ui_present.h"
#include "look_spv.h"
#include "../../runtime/hle/live.h"

namespace {

struct Look { float gain[3], sat, contrast; };
bool g_parsed, g_on;
float g_gain[3] = {1, 1, 1}, g_sat = 1, g_con = 1, g_t = 1;

// Runs at the first present and again when a setting changed (hle/live.h): the Color look and Look strength rows apply with no restart.
void parse() {
  g_parsed = true;
  hle::live::EnvRead env_read;
  g_on = false; g_gain[0] = g_gain[1] = g_gain[2] = 1.0f; g_sat = 1.0f; g_con = 1.0f; g_t = 1.0f;   // from scratch: Neutral really is off
  const char* e = getenv("SNAP_LOOK");
  if (!e || !*e) return;
  std::string s = e, name = s;
  float t = 1.0f;
  size_t c = s.find(':');
  if (c != std::string::npos) { name = s.substr(0, c); t = (float)atof(s.c_str() + c + 1); }
  for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
  if (t < 0) t = 0;
  if (t > 1.5f) t = 1.5f;
  if (name == "vivid") { g_sat = 1.35f; g_con = 1.10f; }
  else if (name == "warm") { g_gain[0] = 1.07f; g_gain[1] = 1.0f; g_gain[2] = 0.88f; g_sat = 1.06f; g_con = 1.03f; }
  else if (name == "cool") { g_gain[0] = 0.90f; g_gain[1] = 1.0f; g_gain[2] = 1.09f; g_sat = 1.04f; g_con = 1.03f; }
  else return;   // neutral or unknown: off
  g_t = t;
  g_on = t > 0.001f;
  static std::string said;   // once per value, not on every unrelated setting
  if (said != s) { said = s; fprintf(stderr, "look: SNAP_LOOK=%s intensity %.2f\n", name.c_str(), t); }
}

struct State {
  VkDevice dev = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  VkExtent2D ext{};
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  VkRenderPass rp = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set[2] = {};     // main pass: (frame, state[i])
  VkDescriptorSet setSt[2] = {};   // state pass: (frame, state[i ^ 1] = last frame's state)
  VkSampler samp = VK_NULL_HANDLE;
  uint32_t mips = 1;
  // Reduce flashing: the state pass writes a 1x1 float image per frame (ping-pong); the main pass reads this frame's
  VkRenderPass rpSt = VK_NULL_HANDLE;
  VkPipeline pipeSt = VK_NULL_HANDLE;
  VkImage stImg[2] = {};
  VkDeviceMemory stMem[2] = {};
  VkImageView stView[2] = {};
  VkFramebuffer stFb[2] = {};
  bool stInit = false, stValid = false;
  int stIdx = 0;
  double lastT = 0;
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  struct Fb { VkImageView v; VkFramebuffer f; };
  std::map<VkImage, Fb> fbs;
  bool failed = false;
} S;

uint32_t findMem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return 0xffffffffu;
}

void dropSized() {
  for (auto& kv : S.fbs) { vkDestroyFramebuffer(S.dev, kv.second.f, nullptr); vkDestroyImageView(S.dev, kv.second.v, nullptr); }
  S.fbs.clear();
  if (S.view) vkDestroyImageView(S.dev, S.view, nullptr);
  if (S.img) vkDestroyImage(S.dev, S.img, nullptr);
  if (S.mem) vkFreeMemory(S.dev, S.mem, nullptr);
  S.view = VK_NULL_HANDLE; S.img = VK_NULL_HANDLE; S.mem = VK_NULL_HANDLE;
}

bool build(const SnapUiFrame* f) {
  // called with the device idle for our resources only: the previous frames' command buffers may still use the old scratch image, so wait.
  if (S.dev) vkDeviceWaitIdle(S.dev);
  if (S.dev && (S.dev != f->device || S.fmt != f->format)) {
    dropSized();
    if (S.pipe) vkDestroyPipeline(S.dev, S.pipe, nullptr);
    if (S.rp) vkDestroyRenderPass(S.dev, S.rp, nullptr);
    S.pipe = VK_NULL_HANDLE; S.rp = VK_NULL_HANDLE;
    // the state objects belong to the old device: leaked rather than destroyed with a possibly lost device; rebuilt below
    S.pipeSt = VK_NULL_HANDLE; S.rpSt = VK_NULL_HANDLE; S.stInit = S.stValid = false;
    for (int i = 0; i < 2; i++) { S.stImg[i] = VK_NULL_HANDLE; S.stMem[i] = VK_NULL_HANDLE; S.stView[i] = VK_NULL_HANDLE; S.stFb[i] = VK_NULL_HANDLE; }
    S.samp = VK_NULL_HANDLE;
  }
  S.dev = f->device;
  VkDevice d = S.dev;
  if (!S.rp) {
    VkAttachmentDescription a{};
    a.format = f->format; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference r{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    if (vkCreateRenderPass(d, &ci, nullptr, &S.rp) != VK_SUCCESS) return false;
    S.fmt = f->format;
  }
  if (!S.samp) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &si, nullptr, &S.samp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &dl, nullptr, &S.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &S.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &S.pl) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 4; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout ls[4] = {S.dsl, S.dsl, S.dsl, S.dsl};
    VkDescriptorSet got[4];
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = S.pool; da.descriptorSetCount = 4; da.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &da, got) != VK_SUCCESS) return false;
    S.set[0] = got[0]; S.set[1] = got[1]; S.setSt[0] = got[2]; S.setSt[1] = got[3];
  }
  auto makePipe = [&](const uint32_t* fragWords, size_t fragBytes, VkRenderPass rp, VkPipeline* out) -> bool {
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof kLook_vert; sm.pCode = kLook_vert;
    if (vkCreateShaderModule(d, &sm, nullptr, &vs) != VK_SUCCESS) return false;
    sm.codeSize = fragBytes; sm.pCode = fragWords;
    if (vkCreateShaderModule(d, &sm, nullptr, &fs) != VK_SUCCESS) { vkDestroyShaderModule(d, vs, nullptr); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = S.pl; gp.renderPass = rp; gp.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, out);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    return r == VK_SUCCESS;
  };
  if (!S.pipe && !makePipe(kLook_frag, sizeof kLook_frag, S.rp, &S.pipe)) return false;
  // Reduce flashing state pass: a 1x1 float render target, two of them ping-ponging (this frame writes one and reads the other)
  if (!S.rpSt) {
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R32G32B32A32_SFLOAT; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference r{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;   // last frame's reads of this image are done before it is rewritten
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;   // the write is visible to the main pass and to next frame's state pass
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp; ci.dependencyCount = 2; ci.pDependencies = dep;
    if (vkCreateRenderPass(d, &ci, nullptr, &S.rpSt) != VK_SUCCESS) return false;
  }
  if (!S.pipeSt && !makePipe(kLookState_frag, sizeof kLookState_frag, S.rpSt, &S.pipeSt)) return false;
  if (!S.stImg[0]) {
    for (int i = 0; i < 2; i++) {
      VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R32G32B32A32_SFLOAT; ic.extent = {1, 1, 1};
      ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
      ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(d, &ic, nullptr, &S.stImg[i]) != VK_SUCCESS) return false;
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(d, S.stImg[i], &mr);
      uint32_t mt = findMem(f->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mt == 0xffffffffu) return false;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
      if (vkAllocateMemory(d, &ai, nullptr, &S.stMem[i]) != VK_SUCCESS || vkBindImageMemory(d, S.stImg[i], S.stMem[i], 0) != VK_SUCCESS) return false;
      VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vc.image = S.stImg[i]; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = VK_FORMAT_R32G32B32A32_SFLOAT;
      vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(d, &vc, nullptr, &S.stView[i]) != VK_SUCCESS) return false;
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = S.rpSt; fi.attachmentCount = 1; fi.pAttachments = &S.stView[i]; fi.width = 1; fi.height = 1; fi.layers = 1;
      if (vkCreateFramebuffer(d, &fi, nullptr, &S.stFb[i]) != VK_SUCCESS) return false;
    }
    S.stInit = false; S.stValid = false;
  }
  if (!S.img || S.ext.width != f->extent.width || S.ext.height != f->extent.height || S.sc != f->swapchain) {
    dropSized();
    S.ext = f->extent; S.sc = f->swapchain;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = f->format; ic.extent = {f->extent.width, f->extent.height, 1};
    S.mips = 1;
    for (uint32_t m = std::max(f->extent.width, f->extent.height); m > 1; m >>= 1) S.mips++;   // down to 1x1: the top level is the frame's mean (Reduce flashing)
    ic.mipLevels = S.mips; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d, &ic, nullptr, &S.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, S.img, &mr);
    uint32_t mt = findMem(f->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == 0xffffffffu) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(d, &ai, nullptr, &S.mem) != VK_SUCCESS || vkBindImageMemory(d, S.img, S.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = S.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, S.mips, 0, 1};
    if (vkCreateImageView(d, &vc, nullptr, &S.view) != VK_SUCCESS) return false;
    // main set i = (frame, state[i]); state-pass set i = (frame, state[i ^ 1])
    VkDescriptorImageInfo tex{S.samp, S.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sti[4] = {{S.samp, S.stView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {S.samp, S.stView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {S.samp, S.stView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {S.samp, S.stView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorSet sets[4] = {S.set[0], S.set[1], S.setSt[0], S.setSt[1]};
    VkWriteDescriptorSet w[8]{};
    for (int i = 0; i < 4; i++) {
      w[2 * i].sType = w[2 * i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[2 * i].dstSet = w[2 * i + 1].dstSet = sets[i];
      w[2 * i].dstBinding = 0; w[2 * i + 1].dstBinding = 1;
      w[2 * i].descriptorCount = w[2 * i + 1].descriptorCount = 1;
      w[2 * i].descriptorType = w[2 * i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[2 * i].pImageInfo = &tex; w[2 * i + 1].pImageInfo = &sti[i];
    }
    vkUpdateDescriptorSets(d, 8, w, 0, nullptr);
  }
  return true;
}

void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, uint32_t base = 0, uint32_t levels = 1) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

extern "C" void snap_look_present(const SnapUiFrame* f) {
  static std::atomic<unsigned> live_seen{0};
  const bool first = !g_parsed;
  if (hle::live::changed(live_seen) || first) parse();
  if (first) {
    // live params for scripts (ab_capture -Effect SNAP_ACCESS_CVD -Values 0,2); the menus change the same fields through hle::access::set
    hle::access::State& a = hle::access::state();
    dev_param_i("SNAP_ACCESS_CVD", &a.cvd, 0, 3, 0);
    dev_param_i("SNAP_ACCESS_CVD_STRENGTH", &a.cvd_strength, 10, 100, 100);
    dev_param_i("SNAP_ACCESS_FLASH", &a.flash, 0, 3, 0);
  }
  const hle::access::State& acc = hle::access::state();
  const int cvd = std::clamp(acc.cvd, 0, 3);
  const int flash = std::clamp(acc.flash, 0, 3);
  if (!(g_on || cvd || flash) || S.failed || !f) return;
  static bool needBuild = true;
  if (needBuild || S.dev != f->device || S.fmt != f->format || S.ext.width != f->extent.width || S.ext.height != f->extent.height || S.sc != f->swapchain) {
    if (!build(f)) { S.failed = true; fprintf(stderr, "look: setup failed, look disabled\n"); return; }
    needBuild = false;
  }
  VkCommandBuffer cb = f->cb;
  // framebuffer for this swapchain image (created on first use)
  auto it = S.fbs.find(f->image);
  if (it == S.fbs.end()) {
    State::Fb fb{};
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = f->image; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(S.dev, &vc, nullptr, &fb.v) != VK_SUCCESS) { S.failed = true; return; }
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = S.rp; fi.attachmentCount = 1; fi.pAttachments = &fb.v; fi.width = f->extent.width; fi.height = f->extent.height; fi.layers = 1;
    if (vkCreateFramebuffer(S.dev, &fi, nullptr, &fb.f) != VK_SUCCESS) { vkDestroyImageView(S.dev, fb.v, nullptr); S.failed = true; return; }
    it = S.fbs.emplace(f->image, fb).first;
  }
  // Reduce flashing: how fast the frame's mean brightness may move (units of sqrt-of-linear luma per second) and how far the gain may go either way
  static const float kRate[4] = {0, 6.0f, 3.0f, 1.6f}, kMaxGain[4] = {1, 1.6f, 2.2f, 3.0f};
  double dt = 1.0 / 60.0;
  {
    LARGE_INTEGER fq, now;
    QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&now);
    const double t = (double)now.QuadPart / (double)fq.QuadPart;
    if (S.lastT > 0) dt = std::clamp(t - S.lastT, 1.0 / 240.0, 1.0 / 15.0);
    S.lastT = t;
  }
  if (!flash) S.stValid = false;   // the next time it is switched on, the first frame starts from itself (no jump from an old value)
  // 1. swapchain image TRANSFER_DST -> TRANSFER_SRC, scratch -> TRANSFER_DST, copy
  barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  barrier(cb, S.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, S.mips);
  VkImageCopy cp{};
  cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cp.extent = {f->extent.width, f->extent.height, 1};
  vkCmdCopyImage(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  if (flash && S.mips > 1) {   // mip chain down to 1x1: the last level is the mean of the frame
    int32_t mw = (int32_t)f->extent.width, mh = (int32_t)f->extent.height;
    for (uint32_t i = 1; i < S.mips; i++) {
      barrier(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, i - 1, 1);
      VkImageBlit bl{};
      bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
      bl.srcOffsets[1] = {mw, mh, 1};
      mw = std::max(1, mw / 2); mh = std::max(1, mh / 2);
      bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
      bl.dstOffsets[1] = {mw, mh, 1};
      vkCmdBlitImage(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, 0, S.mips - 1);
    barrier(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, S.mips - 1, 1);
  } else {
    barrier(cb, S.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, S.mips);
  }
  barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
  const bool srgb = f->format == VK_FORMAT_B8G8R8A8_SRGB || f->format == VK_FORMAT_R8G8B8A8_SRGB;
  VkViewport vp{0, 0, (float)f->extent.width, (float)f->extent.height, 0, 1};
  VkRect2D sc{{0, 0}, f->extent};
  int cur = S.stIdx;
  if (!S.stInit) {   // both state images start in the layout the passes expect (their contents are never read before the first write: reset = 1)
    for (int i = 0; i < 2; i++) barrier(cb, S.stImg[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT);
    S.stInit = true;
  }
  if (flash) {
    cur = S.stIdx ^= 1;
    // 2a. state pass: this frame's mean brightness and last frame's output mean -> this frame's gain, written to state[cur]
    VkRenderPassBeginInfo sb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    sb.renderPass = S.rpSt; sb.framebuffer = S.stFb[cur]; sb.renderArea = {{0, 0}, {1, 1}};
    vkCmdBeginRenderPass(cb, &sb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipeSt);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl, 0, 1, &S.setSt[cur], 0, nullptr);
    VkViewport v1{0, 0, 1, 1, 0, 1};
    VkRect2D s1{{0, 0}, {1, 1}};
    vkCmdSetViewport(cb, 0, 1, &v1);
    vkCmdSetScissor(cb, 0, 1, &s1);
    float sp[12] = {kRate[flash] * (float)dt, S.stValid ? 0.0f : 1.0f, 0.05f, kMaxGain[flash], 0, 0, 0, srgb ? 1.0f : 0.0f, 0, 0, 0, 0};
    vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sp, sp);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    S.stValid = true;
  }
  // 2b. redraw through the look shader; the render pass leaves the image in TRANSFER_DST_OPTIMAL
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = S.rp; rb.framebuffer = it->second.f; rb.renderArea = {{0, 0}, f->extent};
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl, 0, 1, &S.set[cur], 0, nullptr);
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);
  float pc[12] = {g_gain[0], g_gain[1], g_gain[2], 1.0f, g_sat, g_con, g_on ? g_t : 0.0f, srgb ? 1.0f : 0.0f,
                  (float)cvd, std::clamp(acc.cvd_strength, 10, 100) / 100.0f, 0.0f, flash ? 1.0f : 0.0f};
  vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRenderPass(cb);
}
