// SPDX-License-Identifier: MIT
// Ray tracing track: device setup (M0), transform-feedback geometry capture (M1), BLAS/TLAS build and ray-query debug overlay (M2).
// Overview, switches and the design notes: nvn_rt.h and docs/lighting-rt-plan.md section 4. Everything is inert without SNAP_RT=1.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "hle/lighting_preset.h"   // snaplight::ab_on
#include "nvn_dev.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_fx.h"   // photo-frame guard, scene depth, the post-tonemap slot
#include "nvn_rt.h"
#include "nvn_rt_spv.h"

using namespace nvn;

namespace snap_rt {
namespace {

bool EnvOn(const char* n, bool def = false) { const char* e = getenv(n); return e && *e ? e[0] != '0' : def; }
int EnvInt(const char* n, int def) { const char* e = getenv(n); return e && *e ? atoi(e) : def; }
double Now() { return (double)nvn::os_host::monotonic_ns() * 1e-6; }

bool g_on = false, g_capture = false, g_failed = false, g_reflFailed = false, g_giFailed = false, g_aoFailed = false;
int g_reflOn = 1, g_giOn = 1, g_aoOn = 1;   // SNAP_RT_REFLECT live value (dev param), read by the render thread
VkPhysicalDeviceAccelerationStructureFeaturesKHR g_fAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
VkPhysicalDeviceRayQueryFeaturesKHR g_fRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
VkPhysicalDeviceTransformFeedbackFeaturesEXT g_fTf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
uint32_t g_scratchAlign = 128;
uint64_t g_maxPrims = 0; uint32_t g_maxInstances = 0;

PFN_vkCreateAccelerationStructureKHR pCreateAs;
PFN_vkDestroyAccelerationStructureKHR pDestroyAs;
PFN_vkGetAccelerationStructureBuildSizesKHR pBuildSizes;
PFN_vkCmdBuildAccelerationStructuresKHR pCmdBuild;
PFN_vkGetAccelerationStructureDeviceAddressKHR pAsAddr;
PFN_vkCmdBindTransformFeedbackBuffersEXT pBindXfb;
PFN_vkCmdBeginTransformFeedbackEXT pBeginXfb;
PFN_vkCmdEndTransformFeedbackEXT pEndXfb;

// ---- SPIR-V ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16, kOpCapability = 17, kOpMemberDecorate = 72;
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------
struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; uint64_t size = 0; void* map = nullptr; };

bool MakeBuf(Host* h, uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buf* o, bool addr) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage | (addr ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
  if (vkCreateBuffer(h->device, &bi, nullptr, &o->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, o->buf, &mr);
  VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, props);
  if (addr) ai.pNext = &fl;
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(h->device, &ai, nullptr, &o->mem) != VK_SUCCESS) { vkDestroyBuffer(h->device, o->buf, nullptr); *o = Buf{}; return false; }
  auto discard = [&] { vkDestroyBuffer(h->device, o->buf, nullptr); vkFreeMemory(h->device, o->mem, nullptr); *o = Buf{}; return false; };
  if (vkBindBufferMemory(h->device, o->buf, o->mem, 0) != VK_SUCCESS) return discard();
  o->size = size;
  if (addr) { VkBufferDeviceAddressInfo ba{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ba.buffer = o->buf; o->addr = vkGetBufferDeviceAddress(h->device, &ba); }
  if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && vkMapMemory(h->device, o->mem, 0, VK_WHOLE_SIZE, 0, &o->map) != VK_SUCCESS) return discard();
  return true;
}
void FreeBuf(Host* h, Buf& b) {
  if (b.buf) vkDestroyBuffer(h->device, b.buf, nullptr);
  if (b.mem) vkFreeMemory(h->device, b.mem, nullptr);
  b = Buf{};
}

// First-fit allocator over one big buffer (offsets and sizes in 256 byte units).
struct Ranges {
  std::map<uint64_t, uint64_t> freeMap; uint64_t cap = 0, used = 0, hwm = 0;
  void Init(uint64_t c) { cap = c; used = hwm = 0; freeMap.clear(); freeMap[0] = c; }
  static uint64_t Round(uint64_t n) { return (n + 255) & ~255ull; }
  bool Alloc(uint64_t n, uint64_t* off) {
    n = Round(n);
    for (auto it = freeMap.begin(); it != freeMap.end(); ++it) {
      if (it->second < n) continue;
      *off = it->first;
      const uint64_t rest = it->second - n, at = it->first + n;
      freeMap.erase(it);
      if (rest) freeMap[at] = rest;
      used += n; hwm = std::max(hwm, used);
      return true;
    }
    return false;
  }
  void Free(uint64_t off, uint64_t n) {
    n = Round(n); used -= std::min(used, n);
    auto it = freeMap.emplace(off, n).first;
    auto nx = std::next(it);
    if (nx != freeMap.end() && it->first + it->second == nx->first) { it->second += nx->second; freeMap.erase(nx); }
    if (it != freeMap.begin()) { auto pv = std::prev(it); if (pv->first + pv->second == it->first) { pv->second += it->second; freeMap.erase(it); } }
  }
};

struct Mat { double m[4][4]; };   // row-major, clip = M * p
bool Invert(const Mat& a, Mat& r) {
  double m[4][8];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int i = c + 1; i < 4; i++) if (std::fabs(m[i][c]) > std::fabs(m[p][c])) p = i;
    if (std::fabs(m[p][c]) < 1e-30) return false;
    if (p != c) for (int j = 0; j < 8; j++) std::swap(m[p][j], m[c][j]);
    const double inv = 1.0 / m[c][c];
    for (int j = 0; j < 8; j++) m[c][j] *= inv;
    for (int i = 0; i < 4; i++) if (i != c) { const double f = m[i][c]; if (f != 0) for (int j = 0; j < 8; j++) m[i][j] -= f * m[c][j]; }
  }
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = m[i][j + 4];
  return true;
}

// ---- state -----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxInstances = 8192;
constexpr uint32_t kMaxDraws = 4096;   // motion flag words per frame
constexpr uint32_t kQ = 8;             // timestamp queries per slot (5 used)
constexpr uint32_t kSlots = 8;  // stats / timestamp / descriptor ring (frames in flight stay below this)

struct Entry {
  uint32_t vcount = 0;
  uint64_t arenaOff = 0, arenaBytes = 0;
  VkAccelerationStructureKHR as = VK_NULL_HANDLE; uint64_t blasOff = 0, blasBytes = 0, scratchNeed = 0; VkDeviceAddress blasAddr = 0;
  int64_t lastSeen = 0; uint32_t seen = 0; bool built = false;
  int64_t procFrame = 0;          // last frame this draw was processed at the tonemap draw (a gap means it was off screen: its BLAS is rebuilt)
  uint32_t calm = 0; bool isStatic = false;   // consecutive frames the converted vertices stayed within tolerance of the copy taken at the last BLAS build
  uint32_t id = ~0u;              // stable instance custom index while the entry lives (the per-instance colour cache of the RT reflections is addressed by it)
};
struct FrameDraw { uint64_t key; uint32_t vcount; uint64_t arenaOff; float inv[16]; float eye[3]; };
struct Dead { Entry entry; uint64_t token; };   // retired entry: its AS and allocator ranges are released when the queue timeline passes the retire stamp (RetireFn)

struct Stats {   // per-window aggregates for the log line
  uint32_t frames = 0; uint64_t draws = 0, tris = 0, xfbBytes = 0, blasBuilt = 0, blasTris = 0, blasReused = 0, instances = 0, skipped = 0;
  double cpuCapMs = 0, cpuBuildMs = 0, gpuBuildMs = 0, gpuBlasMs = 0, gpuConvMs = 0, gpuOvlMs = 0; uint32_t gpuN = 0, ovlN = 0;
  uint64_t cls[8] = {}; uint32_t clsN = 0;
  uint64_t rdDraws = 0, rdFlag = 0, rdStatic = 0; double rdRatio = 0;   // motion flags read back: draws, flagged draws, entries static at read time, summed ratio of flagged ones
};

struct G {
  std::mutex mu;
  bool ready = false;
  Host* h = nullptr;
  Buf arena, blasStore, scratch, instStore, tlasStore, stats;
  Buf shadow, flags;           // shadow: copy of the arena taken at each BLAS build (same offsets); flags: kSlots x kMaxDraws motion words written by the convert pass
  std::vector<uint64_t> slotKeys[kSlots];   // draw keys of each slot's flag words
  int reuse = 1; bool winReuse = true; uint32_t staticFrames = 4;   // SNAP_RT_REUSE (0 off, 1 on, 2 alternate per log window for A/B timing), SNAP_RT_STATIC_FRAMES
  Ranges arenaR, blasR;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE; VkDeviceAddress tlasAddr = 0; uint64_t tlasScratch = 0;
  VkQueryPool qp = VK_NULL_HANDLE; float tsPeriod = 1.0f;
  VkPipelineLayout convPl = VK_NULL_HANDLE; VkPipeline convPipe = VK_NULL_HANDLE; VkShaderModule convMod = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout ovlPl = VK_NULL_HANDLE; VkShaderModule ovlVs = VK_NULL_HANDLE, ovlFs = VK_NULL_HANDLE;
  VkPipeline ovlPipe = VK_NULL_HANDLE; VkFormat ovlFmt = VK_FORMAT_UNDEFINED;
  VkDescriptorPool dpool = VK_NULL_HANDLE; VkDescriptorSet sets[kSlots] = {}; VkSampler samp = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, Entry> entries;
  std::vector<FrameDraw> frame; int framePresent = -1;
  std::vector<Dead> dead; uint64_t retireNext = 0;
  TextureImpl* sceneDepth = nullptr;   // dereferenced only between a capture and the same frame's tonemap draw
  const TextureImpl* lastDepth = nullptr;   // compared only (a new scene depth target = a new course: cache flushed)
  uint64_t capFrame = 0;       // frames that were built (age unit of the cache)
  uint32_t slot = 0; bool slotUsed[kSlots] = {}; bool slotTs[kSlots] = {}; uint64_t slotFrame[kSlots] = {};
  struct Done { VkSemaphore sem = VK_NULL_HANDLE; uint64_t value = 0; };
  Done slotDone[kSlots];       // the queue timeline value that signals when the commands recorded for each slot have completed
  int debug = 0, logEvery = 120, ttl = 900, flipY = 0;
  Stats st; int lastLogPresent = 0;
  bool loggedCamFail = false, loggedNoPos = false;
  int keyCb = 1;   // SNAP_RT_KEYCB: the draw key includes the per-object vertex constants (default); 0 = mesh identity only
  std::unordered_set<uint64_t> curA, curB, prevA, prevB;   // per-frame key sets for both key forms (identity only / with constants)
  uint64_t kDraws = 0, kA = 0, kB = 0, kStA = 0, kStB = 0, kFrames = 0;
  // RT reflections (ReflectionPass): per-instance colour cache. racc: accumulator (4 words per id), ravg: mean colour per id (vec4); both addressed by Entry::id.
  Buf racc, ravg; VkDescriptorSetLayout rdsl = VK_NULL_HANDLE; VkPipelineLayout rpl = VK_NULL_HANDLE; VkPipeline rpipe = VK_NULL_HANDLE; VkShaderModule rmod = VK_NULL_HANDLE; bool rinit = false;
  std::vector<uint32_t> freeIds; uint32_t nextId = 0; std::set<uint32_t> idReset;   // ids handed out; reused ids whose cached colour must be cleared before the next accumulation
  bool tlasValid = false; uint32_t killsSinceBuild = 0;   // the TLAS was built from live entries and none of its BLASes has been released since
  uint64_t rPasses = 0, rSkip = 0; int rDoneAt = -1; VkShaderModule gmod = VK_NULL_HANDLE, amod = VK_NULL_HANDLE; uint64_t aoPasses = 0, aoSkip = 0;   // rDoneAt: the present the colour cache was last updated at (the reflection and bounce light passes share one update per frame)
} g;

// A slot's mapped words, descriptor set and query results belong to the GPU until its queue timeline reaches the stamp taken when the slot was recorded (g.mu held).
bool SlotReady(uint32_t slot) {
  const auto& m = g.slotDone[slot];
  if (!m.sem) return true;
  uint64_t done = 0;
  return g.h && vkGetSemaphoreCounterValue(g.h->device, m.sem, &done) == VK_SUCCESS && done >= m.value;
}

// ---- live fallback: when RT cannot run (no extension, allocation or creation failure) the session continues on the raster path; one log line, the F1 menu shows the reason
char g_why[160] = {};   // first reason only; written once under g.mu or at device creation, read by the UI thread (a torn read of a short string is harmless, it is only text)
void NoteWhy(const char* why) { if (!g_why[0]) { snprintf(g_why, sizeof g_why, "%s", why); fprintf(stderr, "[rt] fallback to raster: %s\n", why); } }
bool Forced(const char* what) {   // SNAP_RT_FORCE_FAIL=ext,setup,shadow,build (comma list) fails that stage on purpose, to test the fallback
  static const std::string v = [] { const char* e = getenv("SNAP_RT_FORCE_FAIL"); return std::string(e ? e : ""); }();
  return !v.empty() && v.find(what) != std::string::npos;
}
int g_createFails = 0;   // consecutive acceleration structure creation failures; three in a row turn RT off for the session
bool CreateFailed() { if (++g_createFails >= 3) { g_failed = true; NoteWhy("acceleration structure creation keeps failing"); } return false; }

// Scene geometry capture (M1/M2: the debug overlay and the future AO / reflection passes). With SNAP_RT_SHADOWS set and neither SNAP_RT_DEBUG nor SNAP_RT_SCENE=1 only the
// shadow casters are captured (small buffers, no scene BLAS work).
bool ShadowsRequested() { static const bool r = [] { const char* e = getenv("SNAP_RT_SHADOWS"); return e && *e; }(); return r; }
bool AoRequested() { static const bool r = [] { const char* e = getenv("SNAP_RT_AO"); return e && *e && atoi(e) != 0; }(); return r; }   // RT ambient occlusion (nvn_lighting_fx.cpp, replaces the GTAO pass): needs the scene capture only
bool GiRequested() { static const bool r = [] { const char* e = getenv("SNAP_RT_GI"); return e && *e && atoi(e) != 0; }(); return r; }   // RT bounce light (nvn_lighting_fx.cpp, needs SNAP_SSGI=1): needs the scene capture
bool ReflectRequested() { static const bool r = [] { const char* e = getenv("SNAP_RT_REFLECT"); return e && *e && atoi(e) != 0; }(); return r; }   // RT reflections (the water pass, nvn_ssr.cpp): needs the scene capture
bool SceneCapture() {
  static const bool v = [] { const char* e = getenv("SNAP_RT_SHADOWS"); return !(e && *e) || EnvInt("SNAP_RT_DEBUG", 0) != 0 || EnvOn("SNAP_RT_SCENE") || ReflectRequested() || GiRequested() || AoRequested(); }();
  return v;
}

bool IsSceneDepth(const TextureImpl* t) {
  return t && t->b.format == FMT_DEPTH32F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && t->b.samples <= 1;
}
bool IsSceneColor(const TextureImpl* t) {
  return t && t->b.format == FMT_RGBA16F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && !(t->b.flags & TF_DISPLAY);
}
void Bar(VkCommandBuffer cb) {   // everything before -> everything after (a few per frame; the capture, build and trace phases are serialized on purpose)
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}

void Fail(const char* why) { g_failed = true; NoteWhy(why); }

bool Setup(Host* h) {
  if (g.ready) return true;
  if (g_failed) return false;
  g.h = h;   // set before any allocation: DeviceShutdown cleans a half-built setup that belongs to this host
  if (Forced("setup")) { Fail("forced failure (SNAP_RT_FORCE_FAIL=setup)"); return false; }
  g.debug = EnvInt("SNAP_RT_DEBUG", 0);
  g.logEvery = EnvInt("SNAP_RT_LOG", (g.debug || ShadowsRequested()) ? 120 : 0);
  g.ttl = EnvInt("SNAP_RT_TTL", 900);
  g.keyCb = EnvInt("SNAP_RT_KEYCB", 1);
  g.flipY = EnvInt("SNAP_RT_FLIPY", h->originMode == ORIGIN_UPPER_LEFT ? 1 : 0);
  const bool scene = SceneCapture();   // shadows only: tiny scene buffers (the scene arena and its shadow copy are the big ones)
  const uint64_t arenaB = (uint64_t)EnvInt("SNAP_RT_ARENA_MB", scene ? 384 : 4) << 20, blasB = (uint64_t)EnvInt("SNAP_RT_BLAS_MB", scene ? 512 : 4) << 20, scrB = (uint64_t)EnvInt("SNAP_RT_SCRATCH_MB", scene ? 256 : 128) << 20;
  const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  const VkBufferUsageFlags asIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (!MakeBuf(h, arenaB, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | asIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dl, &g.arena, true) ||
      !MakeBuf(h, blasB, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &g.blasStore, true) ||
      !MakeBuf(h, scrB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, dl, &g.scratch, true) ||
      !MakeBuf(h, arenaB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, dl, &g.shadow, true) ||
      !MakeBuf(h, (uint64_t)kSlots * kMaxDraws * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.flags, true) ||
      !MakeBuf(h, (uint64_t)kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR), asIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, dl, &g.instStore, true) ||
      !MakeBuf(h, kSlots * 8 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.stats, false)) {
    Fail("buffer allocation failed"); return false;
  }
  g.arenaR.Init(arenaB); g.blasR.Init(blasB);
  memset(g.stats.map, 0, g.stats.size); memset(g.flags.map, 0, g.flags.size);
  g.reuse = EnvInt("SNAP_RT_REUSE", 1); g.staticFrames = (uint32_t)std::max(1, EnvInt("SNAP_RT_STATIC_FRAMES", 4));
  // TLAS (rebuilt in place every frame; sized for kMaxInstances)
  {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = g.instStore.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const uint32_t n = std::min(kMaxInstances, g_maxInstances ? g_maxInstances : kMaxInstances);
    pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
    g.tlasScratch = sz.buildScratchSize;
    if (!MakeBuf(h, sz.accelerationStructureSize + 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &g.tlasStore, true)) { Fail("TLAS storage allocation failed"); return false; }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = g.tlasStore.buf; ci.offset = 0; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (pCreateAs(h->device, &ci, nullptr, &g.tlas) != VK_SUCCESS) { Fail("vkCreateAccelerationStructureKHR (TLAS) failed"); return false; }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = g.tlas;
    g.tlasAddr = pAsAddr(h->device, &ai);
  }
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(h->phys, &pp); g.tsPeriod = pp.limits.timestampPeriod;
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * kQ;
  if (vkCreateQueryPool(h->device, &qi, nullptr, &g.qp) != VK_SUCCESS) g.qp = VK_NULL_HANDLE;
  // convert pipeline
  {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kRtConvSpv; mi.pCode = kRtConvSpv;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    if (vkCreateShaderModule(h->device, &mi, nullptr, &g.convMod) != VK_SUCCESS || vkCreatePipelineLayout(h->device, &pl, nullptr, &g.convPl) != VK_SUCCESS) { Fail("convert pipeline layout"); return false; }
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, g.convMod, "main", nullptr}; ci.layout = g.convPl;
    if (vkCreateComputePipelines(h->device, VK_NULL_HANDLE, 1, &ci, nullptr, &g.convPipe) != VK_SUCCESS) { Fail("convert pipeline"); return false; }
  }
  if (g.debug) {   // overlay objects (the pipeline is made per target format)
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 3; li.pBindings = b;
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSlots}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
    VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; di.maxSets = kSlots; di.poolSizeCount = 3; di.pPoolSizes = ps;
    VkShaderModuleCreateInfo m1{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}, m2{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    m1.codeSize = sizeof kRtOvlVertSpv; m1.pCode = kRtOvlVertSpv; m2.codeSize = sizeof kRtOvlFragSpv; m2.pCode = kRtOvlFragSpv;
    VkDescriptorSetLayout lay[kSlots]; for (auto& l : lay) l = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(h->device, &li, nullptr, &g.dsl) != VK_SUCCESS) { Fail("overlay set layout"); return false; }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &g.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pl, nullptr, &g.ovlPl) != VK_SUCCESS || vkCreateSampler(h->device, &si, nullptr, &g.samp) != VK_SUCCESS ||
        vkCreateDescriptorPool(h->device, &di, nullptr, &g.dpool) != VK_SUCCESS || vkCreateShaderModule(h->device, &m1, nullptr, &g.ovlVs) != VK_SUCCESS ||
        vkCreateShaderModule(h->device, &m2, nullptr, &g.ovlFs) != VK_SUCCESS) { Fail("overlay objects"); return false; }
    for (auto& l : lay) l = g.dsl;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = g.dpool; ai.descriptorSetCount = kSlots; ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(h->device, &ai, g.sets) != VK_SUCCESS) { Fail("overlay descriptor sets"); return false; }
  }
  g.ready = true;
  fprintf(stderr, "[rt] ready: arena %llu MB, BLAS store %llu MB, scratch %llu MB, TTL %d, debug %d, flipY %d, timestamps %s\n", (unsigned long long)(arenaB >> 20), (unsigned long long)(blasB >> 20),
          (unsigned long long)(scrB >> 20), g.ttl, g.debug, g.flipY, g.qp ? "on" : "off");
  return true;
}

void FreeRetired(Host* h, const Entry& e) {   // the GPU has finished with it (or the device is idle at shutdown); g.mu held
  if (e.as) pDestroyAs(h->device, e.as, nullptr);
  if (e.arenaBytes) g.arenaR.Free(e.arenaOff, e.arenaBytes);
  if (e.blasBytes) g.blasR.Free(e.blasOff, e.blasBytes);
}
void KillEntry(Host* h, Entry& e) {   // the entry's AS and ranges may still be read by recorded or in-flight commands: they are released through the graveyard, which waits for the queue timelines
  if (e.id != ~0u) { g.freeIds.push_back(e.id); g.idReset.insert(e.id); }
  if (e.as) g.killsSinceBuild++;   // the last TLAS still names this BLAS: the reflection pass leaves it alone until the next build
  if (!e.as && !e.arenaBytes && !e.blasBytes) { e = Entry{}; return; }
  const uint64_t token = ++g.retireNext;
  g.dead.push_back({e, token});
  e = Entry{};
  RetireFn([h, token] {
    std::lock_guard<std::mutex> lk(g.mu);
    auto it = std::find_if(g.dead.begin(), g.dead.end(), [token](const Dead& d) { return d.token == token; });
    if (it == g.dead.end()) return;   // DeviceShutdown already released it at device idle
    FreeRetired(h, it->entry);
    g.dead.erase(it);
  });
}
void Evict(Host* h, uint64_t keep = 0) {   // drop entries past their TTL (unstable ones, seen fewer than 3 frames, after 2)
  for (auto it = g.entries.begin(); it != g.entries.end();) {
    Entry& e = it->second;
    const int64_t age = (int64_t)g.capFrame - e.lastSeen, lim = e.seen >= 3 ? (int64_t)g.ttl : 2;
    if (it->first != keep && age > lim) { KillEntry(h, e); it = g.entries.erase(it); } else ++it;
  }
}
void Flush(Host* h) { for (auto& kv : g.entries) KillEntry(h, kv.second); g.entries.clear(); g.frame.clear(); }

uint64_t HashKey(const DrawInfo& di, bool withCb) {
  uint64_t x = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { x ^= v; x *= 1099511628211ull; };
  if (di.vsKey) for (int i = 0; i < 12 && di.vsKey[i]; i++) mix((uint8_t)di.vsKey[i]);
  for (uint64_t v : di.vbAddr) mix(v);
  mix(di.ibAddr); mix((uint64_t)(int64_t)di.first); mix((uint64_t)(int64_t)di.base); mix((uint64_t)(int64_t)di.count); mix(di.instances); mix((uint64_t)di.topo);
  if (withCb) mix(di.cbHash);
  return x ? x : 1;
}

uint32_t TriVerts(VkPrimitiveTopology t, uint32_t n) {
  switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return n / 3 * 3;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return n >= 3 ? (n - 2) * 3 : 0;
    default: return 0;
  }
}

}  // namespace

namespace {
std::mutex g_patchedMu; std::set<std::string> g_patched;   // first 12 hex digits of the vertex programs whose module carries the Xfb decorations
bool IsPatched(const char* k) { if (!k) return false; std::lock_guard<std::mutex> lk(g_patchedMu); return g_patched.count(std::string(k, 12)) != 0; }
}
// Demand gate (SNAP_RT_DEMAND=0 turns it off): the scene capture (transform feedback on every opaque scene draw, the convert pass, the BLAS and TLAS
// builds) is only worth its cost while something traces against the TLAS. The reflection, bounce light and AO passes say so each frame they run;
// with no such call for kDemandHold presents (no water in view, Bounce light off) the capture stops, and it starts again on the next call. The first
// frames after a restart use the screen-space path until a TLAS is built (a stale one is never traced). The debug views keep the capture on always.
std::atomic<int> g_demandAt{-1000000};
std::atomic<bool> g_demandLapsed{false};
constexpr int kDemandHold = 120;
void NoteDemand() { g_demandAt.store(g_presentCount, std::memory_order_relaxed); }
bool SceneDemand() {
  static const bool gated = (ReflectRequested() || GiRequested() || AoRequested()) && EnvInt("SNAP_RT_DEBUG", 0) == 0 && !EnvOn("SNAP_RT_SCENE") && EnvOn("SNAP_RT_DEMAND", true);
  if (!gated || g_presentCount - g_demandAt.load(std::memory_order_relaxed) <= kDemandHold) return true;
  if (!g_demandLapsed.exchange(true)) { static int n; if (n++ < 6) fprintf(stderr, "[rt] p%d scene capture paused: nothing has traced the scene for %d presents\n", g_presentCount, kDemandHold); }
  return false;
}
// g.mu held. After a pause the old TLAS describes a scene from before it: dropped, so the passes wait for a fresh build.
void DemandResume() { if (g_demandLapsed.exchange(false)) { g.tlasValid = false; static int n; if (n++ < 6) fprintf(stderr, "[rt] p%d scene capture resumed\n", g_presentCount); } }

bool Wanted() { static const bool w = EnvOn("SNAP_RT") || ShadowsRequested() || ReflectRequested() || GiRequested() || AoRequested(); return w; }
bool On() { return g_on; }
bool Capture() { return g_on && g_capture && !g_failed; }

// ======================================== M3: RT sun shadows ========================================
// Cast pass capture -> world-space triangles -> pooled BLAS per draw -> one shadow TLAS, built lazily at the first receiver draw of the frame (the cast pass
// is over by then and nothing of the scene pass has been drawn that needs the result). nvn_shadowrt.cpp patches the receivers.
namespace {
struct ShCfg { int mode = 0, softRays = 4, dbg = 0; float cone = 1.2f, bias = 0.0015f, margin = 0.0f; };
ShCfg& Cfg() {
  static ShCfg c = [] {
    ShCfg k;
    if (const char* e = getenv("SNAP_RT_SHADOWS")) {
      if (nvn::os_host::ascii_ieq(e, "off") || !strcmp(e, "0")) k.mode = 0; else if (nvn::os_host::ascii_ieq(e, "hard") || nvn::os_host::ascii_ieq(e, "on") || !strcmp(e, "1")) k.mode = 1;
      else if (nvn::os_host::ascii_ieq(e, "soft") || !strcmp(e, "2")) k.mode = 2; else if (atoi(e) >= 3) k.mode = 3;
    }
    k.softRays = std::max(2, std::min(16, EnvInt("SNAP_RT_SHADOW_RAYS", 4)));
    k.dbg = EnvInt("SNAP_RT_SHADOW_DBG", 0);   // bring-up bits (captured clip -> map coordinates): 1 flip v, 2 depth is NDC (z * 0.5 + 0.5), 4 xy already in 0..1
    if (const char* e = getenv("SNAP_RT_SHADOW_CONE")) k.cone = std::max(0.0f, std::min(10.0f, (float)atof(e)));
    if (const char* e = getenv("SNAP_RT_SHADOW_BIAS")) k.bias = std::max(0.0f, std::min(0.05f, (float)atof(e)));
    if (const char* e = getenv("SNAP_RT_SHADOW_MARGIN")) k.margin = std::max(0.0f, std::min(4.0f, (float)atof(e)));
    return k;
  }();
  static bool reg = false;
  if (!reg) {
    reg = true;
    dev_param_i("SNAP_RT_SHADOWS", &c.mode, 0, 3, 0);
    dev_param_i("SNAP_RT_SHADOW_DBG", &c.dbg, 0, 15, 0);
    dev_param_f("SNAP_RT_SHADOW_CONE", &c.cone, 0.0f, 10.0f, 1.2f);
    dev_param_f("SNAP_RT_SHADOW_BIAS", &c.bias, 0.0f, 0.05f, 0.0015f);
    dev_param_f("SNAP_RT_SHADOW_MARGIN", &c.margin, 0.0f, 4.0f, 0.0f);
  }
  return c;
}

struct PooledAs { VkAccelerationStructureKHR as = VK_NULL_HANDLE; VkDeviceAddress addr = 0; uint64_t scratch = 0, off = 0, bytes = 0; };
struct CastDraw { uint32_t vcount; uint64_t arenaOff; float inv[16]; };
struct Sh {
  bool ready = false, failed = false;
  Buf arena, blasStore, inst, tlasStore;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE; VkDeviceAddress tlasAddr = 0; bool tlasBuilt = false;
  uint64_t arenaTop = 0, blasTop = 0;
  std::vector<CastDraw> draws;
  int present = -1;                       // the present the cast draws belong to
  bool built = false; uint32_t ninst = 0; // the shadow TLAS of that present was built (ninst instances)
  float m[16] = {}; float sunL[3] = {0, 1, 0}, sunT[3] = {1, 0, 0}, tmax = 100.0f;
  std::map<uint32_t, std::vector<PooledAs>> pool; std::map<uint32_t, uint32_t> used;
  float recvKey[16] = {}; float recvInv[16] = {}; bool recvOk = false, recvHave = false; float recvRatio = 1.0f;   // map xy units per depth unit of the receivers' matrix (the cone spread)
  // stats (log line: SNAP_RT_LOG)
  uint64_t frames = 0, draws_n = 0, tris = 0, blasBuilt = 0, skipped = 0, noRecvM = 0, receivers = 0;
  double gpuMs = 0; uint32_t gpuN = 0; bool tsPending[kSlots] = {}; uint32_t tsSlot = 0;
} S;

uint64_t Align256(uint64_t v) { return (v + 255) & ~255ull; }

bool ShSetup(Host* h) {
  if (S.ready) return true;
  if (S.failed) return false;
  if (Forced("shadow")) { S.failed = true; NoteWhy("forced failure (SNAP_RT_FORCE_FAIL=shadow)"); return false; }
  const uint64_t arenaB = (uint64_t)EnvInt("SNAP_RT_CAST_MB", 64) << 20, blasB = (uint64_t)EnvInt("SNAP_RT_CAST_BLAS_MB", 192) << 20;
  const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  const VkBufferUsageFlags asIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (!MakeBuf(h, arenaB, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | asIn, dl, &S.arena, true) ||
      !MakeBuf(h, blasB, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &S.blasStore, true) ||
      !MakeBuf(h, (uint64_t)kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR), asIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, dl, &S.inst, true)) { S.failed = true; NoteWhy("RT shadow buffers could not be allocated"); fprintf(stderr, "[rt] shadows: buffer allocation failed\n"); return false; }
  VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geo.geometry.instances.data.deviceAddress = S.inst.addr;
  VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
  VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  const uint32_t n = std::min(kMaxInstances, g_maxInstances ? g_maxInstances : kMaxInstances);
  pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
  if (!MakeBuf(h, sz.accelerationStructureSize + 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &S.tlasStore, true)) { S.failed = true; NoteWhy("RT shadow TLAS storage could not be allocated"); return false; }
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = S.tlasStore.buf; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (pCreateAs(h->device, &ci, nullptr, &S.tlas) != VK_SUCCESS) { S.failed = true; NoteWhy("RT shadow TLAS could not be created"); return false; }
  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = S.tlas;
  S.tlasAddr = pAsAddr(h->device, &ai);
  S.ready = true;
  fprintf(stderr, "[rt] shadows ready: cast arena %llu MB, cast BLAS store %llu MB, TLAS for %u instances, mode %d\n", (unsigned long long)(arenaB >> 20), (unsigned long long)(blasB >> 20), n, Cfg().mode);
  return true;
}

void ShShutdown(Host* h) {   // also cleans a half-built setup (every destroy below is null safe)
  for (auto& kv : S.pool) for (auto& p : kv.second) if (p.as) pDestroyAs(h->device, p.as, nullptr);
  S.pool.clear();
  if (S.tlas) pDestroyAs(h->device, S.tlas, nullptr);
  for (Buf* b : {&S.arena, &S.blasStore, &S.inst, &S.tlasStore}) FreeBuf(h, *b);
  S.ready = false; S.tlas = VK_NULL_HANDLE;
}

// A pooled BLAS able to hold `prims` triangles: classes are powers of two (at least 128 triangles); an AS built with fewer triangles than it was sized for is valid.
bool PoolGet(Host* h, uint32_t prims, PooledAs* out) {
  uint32_t cls = 128; while (cls < prims) cls <<= 1;
  auto& v = S.pool[cls]; uint32_t& u = S.used[cls];
  if (u < v.size()) { *out = v[u++]; return true; }
  VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; geo.geometry.triangles.vertexStride = 16; geo.geometry.triangles.maxVertex = cls * 3 - 1; geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
  VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
  VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &cls, &sz);
  const uint64_t bytes = Align256(sz.accelerationStructureSize);
  if (S.blasTop + bytes > S.blasStore.size) return false;
  if (Forced("build")) { Fail("forced failure (SNAP_RT_FORCE_FAIL=build)"); return false; }
  PooledAs p; p.off = S.blasTop; p.bytes = bytes; p.scratch = sz.buildScratchSize;
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = S.blasStore.buf; ci.offset = p.off; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (pCreateAs(h->device, &ci, nullptr, &p.as) != VK_SUCCESS) return CreateFailed();
  g_createFails = 0;
  S.blasTop += bytes;
  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = p.as;
  p.addr = pAsAddr(h->device, &ai);
  v.push_back(p); u++;
  *out = p;
  return true;
}

void ShNewFrame() {   // first cast draw of a present
  S.draws.clear(); S.arenaTop = 0; S.built = false; S.ninst = 0; S.present = g_presentCount;
  for (auto& kv : S.used) kv.second = 0;
}

// Toward-the-sun unit vector and a perpendicular, from the caster matrix: its depth row (m[2], m[6], m[10]) is the light's axis (sign: the one pointing up, as nvn_lighting_fx.cpp).
void ShSun(const float* m) {
  double a[3] = {m[2], m[6], m[10]};
  const double len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (len < 1e-12) return;
  for (double& x : a) x /= len;
  const double sg = a[1] >= 0 ? 1.0 : -1.0;
  for (int i = 0; i < 3; i++) S.sunL[i] = (float)(a[i] * sg);
  const double hx = std::fabs(S.sunL[1]) > 0.9 ? 1.0 : 0.0, hy = hx > 0 ? 0.0 : 1.0;   // any vector not parallel to L
  double t[3] = {S.sunL[1] * 0.0 - S.sunL[2] * hy, S.sunL[2] * hx - S.sunL[0] * 0.0, S.sunL[0] * hy - S.sunL[1] * hx};
  const double tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
  for (int i = 0; i < 3; i++) S.sunT[i] = (float)(t[i] / std::max(tl, 1e-12));
  S.tmax = (float)(2.0 / len);   // the light volume's full depth along the axis (z spans 2 clip units at most)
}

bool CastBegin(ReplayCtx& c, const DrawInfo& di) {
  if (!g_on || !g_capture || g_failed || ShadowRays() == 0 || di.hasGeomTess || FxIsPhotoFrame()) return false;
  if (!IsPatched(di.vsKey) || !di.s9 || di.s9Range < 64) return false;
  float m[16]; memcpy(m, di.s9, 64);
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;   // an orthographic caster matrix (the dynamic map's); anything else is not it
  const uint64_t nv64 = (uint64_t)TriVerts(di.topo, di.vertexCount) * std::max(di.instances, 1u);
  if (nv64 == 0) return false;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!Setup(c.h) || !ShSetup(c.h)) return false;
  if (S.present != g_presentCount) ShNewFrame();
  if (S.built) return false;   // receivers already traced this frame: a late cast draw cannot join (never seen)
  const uint64_t bytes = Align256(nv64 * 16);
  if (nv64 > 0xFFFFFFF0ull || S.arenaTop + bytes > S.arena.size || S.draws.size() >= kMaxInstances) { S.skipped++; return false; }
  Mat M;
  for (int r = 0; r < 4; r++) for (int cc = 0; cc < 4; cc++) M.m[r][cc] = m[cc * 4 + r];
  Mat inv;
  if (!Invert(M, inv)) return false;
  if (S.draws.empty()) { memcpy(S.m, m, 64); ShSun(m); }
  CastDraw d; d.vcount = (uint32_t)nv64; d.arenaOff = S.arenaTop;
  for (int cc = 0; cc < 4; cc++) for (int r = 0; r < 4; r++) d.inv[cc * 4 + r] = (float)inv.m[r][cc];
  VkDeviceSize off = S.arenaTop, size = (VkDeviceSize)nv64 * 16;
  pBindXfb(c.cb, 0, 1, &S.arena.buf, &off, &size);
  pBeginXfb(c.cb, 0, 0, nullptr, nullptr);
  S.arenaTop += bytes;
  S.draws.push_back(d);
  S.draws_n++; S.tris += nv64 / 3;
  return true;
}

// Convert, build and publish the shadow TLAS (g.mu held). Ends the render pass: the caller re-emits the view state.
bool ShBuild(ReplayCtx& cx) {
  Host* h = cx.h; VkCommandBuffer cb = cx.cb;
  S.built = true; S.ninst = 0;
  if (S.draws.empty() && S.tlasBuilt) return false;
  EndRendering(cx);
  const bool timed = g.qp != VK_NULL_HANDLE; uint32_t q = 0;
  if (timed) { q = (S.tsSlot++ % kSlots) * kQ + 5; vkCmdResetQueryPool(cb, g.qp, q, 2); vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, q); }
  Bar(cb);   // the cast pass' transform feedback writes -> convert; the previous frame's trace reads -> this frame's AS writes
  std::vector<VkDeviceAddress> addrs;
  if (!S.draws.empty()) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.convPipe);
    for (const CastDraw& d : S.draws) {
      struct { float m[16]; uint64_t addr, shadow, flags; uint32_t count, idx; float eye[4]; } pc;
      {   // captured clip -> shadow-map coordinates (u, v, depth): the same space the receivers' taps use, so no world space is needed
        const int dbg = Cfg().dbg; const float xy = (dbg & 4) ? 1.0f : 0.5f, xo = (dbg & 4) ? 0.0f : 0.5f, zs = (dbg & 2) ? 0.5f : 1.0f, zo = (dbg & 2) ? 0.5f : 0.0f;
        memset(pc.m, 0, 64); pc.m[0] = xy; pc.m[5] = (dbg & 1) ? -xy : xy; pc.m[10] = zs; pc.m[12] = xo; pc.m[13] = xo; pc.m[14] = zo; pc.m[15] = 1.0f;
        if (dbg & 1) pc.m[13] = (dbg & 4) ? 1.0f : 0.5f; }
      pc.addr = S.arena.addr + d.arenaOff; pc.shadow = pc.addr;   // shadow == verts: the motion test sees no motion (nothing is flagged)
      pc.flags = g.flags.addr; pc.count = d.vcount; pc.idx = kMaxDraws - 1; pc.eye[0] = pc.eye[1] = pc.eye[2] = pc.eye[3] = 0;
      vkCmdPushConstants(cb, g.convPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
      vkCmdDispatch(cb, (d.vcount + 63) / 64, 1, 1);
    }
    Bar(cb);
    std::vector<VkAccelerationStructureGeometryKHR> geos(S.draws.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos; std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    uint64_t scratchUsed = 0;
    auto flush = [&]() {
      if (infos.empty()) return;
      std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp(infos.size());
      for (size_t i = 0; i < infos.size(); i++) rp[i] = &ranges[i];
      pCmdBuild(cb, (uint32_t)infos.size(), infos.data(), rp.data());
      Bar(cb);
      infos.clear(); ranges.clear(); scratchUsed = 0;
    };
    infos.reserve(S.draws.size()); ranges.reserve(S.draws.size());
    for (size_t i = 0; i < S.draws.size(); i++) {
      const CastDraw& d = S.draws[i];
      PooledAs p;
      if (!PoolGet(h, d.vcount / 3, &p)) { S.skipped++; continue; }
      VkAccelerationStructureGeometryKHR& geo = geos[i];
      geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
      geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; geo.geometry.triangles.vertexData.deviceAddress = S.arena.addr + d.arenaOff;
      geo.geometry.triangles.vertexStride = 16; geo.geometry.triangles.maxVertex = d.vcount - 1; geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
      VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
      bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      bi.geometryCount = 1; bi.pGeometries = &geo; bi.dstAccelerationStructure = p.as;
      const uint64_t need = (p.scratch + g_scratchAlign - 1) / g_scratchAlign * g_scratchAlign;
      if (need > g.scratch.size) { S.skipped++; continue; }
      if (scratchUsed + need > g.scratch.size) flush();
      bi.scratchData.deviceAddress = g.scratch.addr + scratchUsed; scratchUsed += need;
      infos.push_back(bi); ranges.push_back({d.vcount / 3, 0, 0, 0});
      addrs.push_back(p.addr); S.blasBuilt++;
    }
    flush();
  }
  // TLAS: identity instances of the pooled BLASes (an empty one the first time, so the address is always a built acceleration structure)
  uint32_t ninst = (uint32_t)std::min<size_t>(addrs.size(), kMaxInstances);
  if (ninst) {
    VkBuffer src = VK_NULL_HANDLE;
    uint8_t* mp = NewScratch(cx, (uint64_t)ninst * sizeof(VkAccelerationStructureInstanceKHR), &src, 0);
    if (!mp) ninst = 0;
    else {
      auto* inst = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(mp);
      for (uint32_t i = 0; i < ninst; i++) {
        VkAccelerationStructureInstanceKHR& I = inst[i]; memset(&I, 0, sizeof I);
        I.transform.matrix[0][0] = I.transform.matrix[1][1] = I.transform.matrix[2][2] = 1.0f;
        I.instanceCustomIndex = i; I.mask = 0xFF;
        I.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        I.accelerationStructureReference = addrs[i];
      }
      VkBufferCopy cp{0, 0, (uint64_t)ninst * sizeof(VkAccelerationStructureInstanceKHR)}; vkCmdCopyBuffer(cb, src, S.inst.buf, 1, &cp);
      Bar(cb);
    }
  }
  {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geo.geometry.instances.data.deviceAddress = S.inst.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo; bi.dstAccelerationStructure = S.tlas; bi.scratchData.deviceAddress = g.scratch.addr;
    VkAccelerationStructureBuildRangeInfoKHR rg{ninst, 0, 0, 0}; const VkAccelerationStructureBuildRangeInfoKHR* rgp = &rg;
    pCmdBuild(cb, 1, &bi, &rgp);
    Bar(cb);   // the receivers' ray queries read it
  }
  S.tlasBuilt = true; S.ninst = ninst; S.frames++;
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, q + 1);
  if (timed) {   // read an older stamp pair (4 builds back; the GPU has long finished it)
    const uint32_t old = (S.tsSlot + kSlots - 4) % kSlots * kQ + 5;
    uint64_t r[4] = {};
    if (S.frames > 4 && vkGetQueryPoolResults(h->device, g.qp, old, 2, sizeof r, r, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && r[1] && r[3]) { S.gpuMs += (double)(r[2] - r[0]) * (double)g.tsPeriod * 1e-6; S.gpuN++; }
  }
  return true;
}
}  // namespace

int ShadowRays() {
  if (!g_on || !g_capture || g_failed || S.failed) return 0;
  const ShCfg& c = Cfg();
  if (c.mode <= 0 || FxIsPhotoFrame() || !snaplight::ab_on((unsigned long long)g_presentCount)) return 0;
  return c.mode == 1 ? 1 : c.mode == 2 ? c.softRays : std::max(8, c.softRays);
}

bool ShadowReceiverPrepare(ReplayCtx& cx, const uint8_t* recv, uint64_t recvRange, ShadowPush* out, bool* reemit) {
  *reemit = false;
  if (!g_on || !g_capture || g_failed) return false;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!g.ready || !S.ready) {
    if (!Setup(cx.h) || !ShSetup(cx.h)) return false;
  }
  const bool cur = S.present == g_presentCount;   // a cast pass was recorded this present
  if (!S.tlasBuilt || (cur && !S.built)) *reemit = ShBuild(cx);
  memset(out, 0, sizeof *out);
  const ShCfg& c = Cfg();
  const bool live = cur && S.ninst > 0 && S.built;
  out->tlas = S.tlasAddr;
  out->L[3] = c.bias;   // depth units
  out->T[3] = std::tan(c.cone * 3.14159265f / 180.0f) * S.recvRatio * (ShadowRays() > 1 ? 1.0f : 0.0f);
  out->P[0] = live ? 1.0f : 0.0f; out->P[1] = (float)(g_presentCount & 63); out->P[2] = c.margin; out->P[3] = S.tmax;

  // the receiver matrix (world -> (u, v, depth), column-major): floats 0..15 of the fragment stage's 132-byte binding-9 buffer (the shader's c12; floats 16..31 are a second matrix); its inverse turns a tap's operands back into the world position
  bool ok = false;
  if (recv && recvRange >= 128) {
    float r[16]; memcpy(r, recv, 64);
    if (S.recvHave && !memcmp(r, S.recvKey, 64)) ok = S.recvOk;
    else {
      Mat M; for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) M.m[rr][cc] = r[cc * 4 + rr];
      Mat inv; ok = Invert(M, inv);
      if (ok) for (int cc = 0; cc < 4; cc++) for (int rr = 0; rr < 4; rr++) S.recvInv[cc * 4 + rr] = (float)inv.m[rr][cc];
      memcpy(S.recvKey, r, 64); S.recvHave = true; S.recvOk = ok;
      auto row = [&](int j) { return std::sqrt((double)r[j] * r[j] + (double)r[4 + j] * r[4 + j] + (double)r[8 + j] * r[8 + j]); };   // the (u, v, depth) rows of the world -> map matrix
      if (row(2) > 1e-12) S.recvRatio = (float)(std::sqrt(row(0) * row(1)) / row(2));
    }
  }
  if (ok) memcpy(out->invR, S.recvInv, 64); else { S.noRecvM++; out->P[0] = 0.0f; for (int i = 0; i < 4; i++) out->invR[i * 5] = 1.0f; }
  S.receivers++;
  return true;
}

void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12) {
  if (!Wanted()) return;
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ep(n);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, ep.data());
  auto has = [&](const char* s) { for (auto& e : ep) if (!strcmp(e.extensionName, s)) return true; return false; };
  const char* need[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
  if (Forced("ext")) { NoteWhy("forced failure (SNAP_RT_FORCE_FAIL=ext)"); return; }
  for (const char* s : need) if (!has(s)) { char b[120]; snprintf(b, sizeof b, "this GPU lacks %s", s); NoteWhy(b); return; }
  VkPhysicalDeviceAccelerationStructureFeaturesKHR qa{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR qr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceTransformFeedbackFeaturesEXT qt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
  VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  qa.pNext = &qr; qr.pNext = &qt; qt.pNext = &q12;
  VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qa};
  vkGetPhysicalDeviceFeatures2(phys, &q2);
  if (!qa.accelerationStructure || !qr.rayQuery || !qt.transformFeedback || !q12.bufferDeviceAddress) {
    fprintf(stderr, "[rt] off: features accelerationStructure=%d rayQuery=%d transformFeedback=%d bufferDeviceAddress=%d\n", (int)qa.accelerationStructure, (int)qr.rayQuery, (int)qt.transformFeedback, (int)q12.bufferDeviceAddress);
    return;
  }
  for (const char* s : need) dext.push_back(s);
  g_fAs.accelerationStructure = VK_TRUE; g_fRq.rayQuery = VK_TRUE; g_fTf.transformFeedback = VK_TRUE;
  g_fAs.pNext = &g_fRq; g_fRq.pNext = &g_fTf; g_fTf.pNext = f11->pNext; f11->pNext = &g_fAs;
  f12->bufferDeviceAddress = VK_TRUE;
  g_on = true;
  g_capture = EnvOn("SNAP_RT_CAPTURE", true);
}

static const nvn::FxEffect kFxEffect = {"rt", [] { return snap_rt::Capture(); }, nullptr, [](nvn::ReplayCtx& cx, const char* k) { snap_rt::OnTonemap(cx, k); }};

void DeviceCreated(Host* h) {
  if (!g_on) return;
  nvn::FxRegister(&kFxEffect);   // the build runs from the Fx post-tonemap slot (photo frames and occlusion scopes are filtered there)
  VkDevice d = h->device;
#define L(v, n) v = (decltype(v))vkGetDeviceProcAddr(d, n)
  L(pCreateAs, "vkCreateAccelerationStructureKHR"); L(pDestroyAs, "vkDestroyAccelerationStructureKHR"); L(pBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
  L(pCmdBuild, "vkCmdBuildAccelerationStructuresKHR"); L(pAsAddr, "vkGetAccelerationStructureDeviceAddressKHR");
  L(pBindXfb, "vkCmdBindTransformFeedbackBuffersEXT"); L(pBeginXfb, "vkCmdBeginTransformFeedbackEXT"); L(pEndXfb, "vkCmdEndTransformFeedbackEXT");
#undef L
  if (!pCreateAs || !pDestroyAs || !pBuildSizes || !pCmdBuild || !pAsAddr || !pBindXfb || !pBeginXfb || !pEndXfb) { NoteWhy("the driver does not export the ray tracing entry points"); g_on = false; return; }
  VkPhysicalDeviceAccelerationStructurePropertiesKHR pa{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceTransformFeedbackPropertiesEXT pt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
  pa.pNext = &pt;
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pa};
  vkGetPhysicalDeviceProperties2(h->phys, &p2);
  g_scratchAlign = std::max(1u, pa.minAccelerationStructureScratchOffsetAlignment);
  g_maxPrims = pa.maxPrimitiveCount; g_maxInstances = (uint32_t)std::min<uint64_t>(pa.maxInstanceCount, 0xFFFFFFFFu);
  fprintf(stderr, "[rt] device: ray query + acceleration structure + transform feedback on; scratch offset alignment %u, max geometries %llu, max instances %llu, max primitives %llu; xfb buffers %u, max xfb buffer size %llu, stride %u, queries %d\n",
          g_scratchAlign, (unsigned long long)pa.maxGeometryCount, (unsigned long long)pa.maxInstanceCount, (unsigned long long)pa.maxPrimitiveCount, pt.maxTransformFeedbackBuffers,
          (unsigned long long)pt.maxTransformFeedbackBufferSize, pt.maxTransformFeedbackBufferDataStride, (int)pt.transformFeedbackQueries);
}

void DeviceShutdown(Host* h) {   // the device is idle here; also cleans a half-built setup (g.h is set before the first allocation)
  std::lock_guard<std::mutex> lk(g.mu);
  if (g.h != h) return;
  VkDevice d = h->device;
  Flush(h);
  ShShutdown(h);
  for (auto& x : g.dead) FreeRetired(h, x.entry);
  g.dead.clear();
  if (g.tlas) pDestroyAs(d, g.tlas, nullptr);
  for (Buf* b : {&g.arena, &g.shadow, &g.flags, &g.blasStore, &g.scratch, &g.instStore, &g.tlasStore, &g.stats, &g.racc, &g.ravg}) FreeBuf(h, *b);
  if (g.rpipe) vkDestroyPipeline(d, g.rpipe, nullptr);
  if (g.rpl) vkDestroyPipelineLayout(d, g.rpl, nullptr);
  if (g.rdsl) vkDestroyDescriptorSetLayout(d, g.rdsl, nullptr);
  if (g.rmod) vkDestroyShaderModule(d, g.rmod, nullptr);
  if (g.gmod) vkDestroyShaderModule(d, g.gmod, nullptr);
  g.gmod = VK_NULL_HANDLE;
  if (g.amod) vkDestroyShaderModule(d, g.amod, nullptr);
  g.amod = VK_NULL_HANDLE;
  g.rpipe = VK_NULL_HANDLE; g.rpl = VK_NULL_HANDLE; g.rdsl = VK_NULL_HANDLE; g.rmod = VK_NULL_HANDLE; g.rinit = false; g.tlasValid = false;
  if (g.qp) vkDestroyQueryPool(d, g.qp, nullptr);
  if (g.convPipe) vkDestroyPipeline(d, g.convPipe, nullptr);
  if (g.convPl) vkDestroyPipelineLayout(d, g.convPl, nullptr);
  if (g.convMod) vkDestroyShaderModule(d, g.convMod, nullptr);
  if (g.ovlPipe) vkDestroyPipeline(d, g.ovlPipe, nullptr);
  if (g.ovlPl) vkDestroyPipelineLayout(d, g.ovlPl, nullptr);
  if (g.dpool) vkDestroyDescriptorPool(d, g.dpool, nullptr);
  if (g.dsl) vkDestroyDescriptorSetLayout(d, g.dsl, nullptr);
  if (g.samp) vkDestroySampler(d, g.samp, nullptr);
  if (g.ovlVs) vkDestroyShaderModule(d, g.ovlVs, nullptr);
  if (g.ovlFs) vkDestroyShaderModule(d, g.ovlFs, nullptr);
  g.ready = false; g.h = nullptr; g.tlas = VK_NULL_HANDLE;
  for (auto& m : g.slotDone) m = G::Done{};
}

void QueueDestroyed(QueueImpl* q) {   // after the queue finished its work, before its timeline semaphore is destroyed
  std::lock_guard<std::mutex> lk(g.mu);
  for (auto& m : g.slotDone) if (m.sem == q->timeline) m = G::Done{};
}

const char* FallbackNotice() {
  if (!Wanted() || !g_why[0]) return nullptr;
  return g_why;
}

// Vertex modules: gl_Position (member 0 of the gl_PerVertex block) gets Xfb decorations (buffer 0, offset 0, stride 16) plus the TransformFeedback
// capability and the Xfb execution mode. A pipeline with these decorations behaves as before unless transform feedback is begun on the command buffer.
bool PatchVertexXfb(std::vector<uint32_t>& spv, const char* key12) {
  if (!g_on || !g_capture || spv.size() < 6 || spv[0] != 0x07230203) return false;
  size_t capEnd = 0, epEnd = 0, posDecl = 0; uint32_t entry = 0, blk = 0;
  for (size_t i = 5; i < spv.size();) {
    const uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
    if (!wc || i + wc > spv.size()) return false;
    if (op == kOpCapability) { capEnd = i + wc; if (spv[i + 1] == 53) return false; }
    if (op == kOpEntryPoint) { epEnd = i + wc; if (spv[i + 1] == 0 && !entry) entry = spv[i + 2]; }
    if (op == kOpExecutionMode && spv[i + 2] == 11) return false;   // already Xfb
    if (op == kOpMemberDecorate && wc == 5 && spv[i + 2] == 0 && spv[i + 3] == 11 && spv[i + 4] == 0) { blk = spv[i + 1]; posDecl = i + wc; }
    i += wc;
  }
  if (!entry || !capEnd || !epEnd || !posDecl) return false;
  std::vector<uint32_t> ann, cap, em;
  Emit(ann, kOpMemberDecorate, {blk, 0, 36, 0});    // XfbBuffer 0
  Emit(ann, kOpMemberDecorate, {blk, 0, 37, 16});   // XfbStride 16
  Emit(ann, kOpMemberDecorate, {blk, 0, 35, 0});    // Offset 0
  Emit(cap, kOpCapability, {53});                   // TransformFeedback
  Emit(em, kOpExecutionMode, {entry, 11});          // Xfb
  // later positions first so the earlier indices stay valid: annotations > entry points > capabilities (capabilities come first in a module)
  spv.insert(spv.begin() + posDecl, ann.begin(), ann.end());
  spv.insert(spv.begin() + epEnd, em.begin(), em.end());
  spv.insert(spv.begin() + capEnd, cap.begin(), cap.end());
  if (key12) { std::lock_guard<std::mutex> lk(g_patchedMu); g_patched.emplace(key12, 12); }
  return true;
}

void DrawCapture::Begin(ReplayCtx& c, const DrawInfo& di) {
  open = false;
  if (!Capture() || di.hasGeomTess || FxIsPhotoFrame()) return;   // photo / Evaluation frames get no capture work
  if (di.cast) { if (CastBegin(c, di)) { open = true; cx = &c; } return; }   // M3: the dynamic shadow map's caster draws (nvn_rt.cpp, "M3")
  if (!SceneCapture()) return;                                    // shadows only: no scene geometry is captured
  if (!SceneDemand()) return;                                     // nothing traces the scene now: no capture, so no convert or build at the tonemap draw
  TextureImpl* t8 = c.rt[8]; TextureImpl* t0 = c.rt[0];
  if (!IsSceneDepth(t8) || !IsSceneColor(t0) || !di.depthTest || !di.depthWrite || di.blend0) return;   // scene opaque draws only; never the Evaluation (DEPTH32F_STENCIL8) or photo targets
  if (!IsPatched(di.vsKey)) { static int n; if (n++ < 8) fprintf(stderr, "[rt] scene draw with vertex program %.12s that has no gl_Position block member to decorate: not captured\n", di.vsKey ? di.vsKey : "?"); return; }   // a draw whose module lacks the Xfb decorations must not begin transform feedback
  if (!di.cam || di.camRange < 0x250) { if (!g.loggedNoPos) { g.loggedNoPos = true; fprintf(stderr, "[rt] scene draw without a camera bank (range %llu): not captured\n", (unsigned long long)di.camRange); } return; }
  const uint64_t nv64 = (uint64_t)TriVerts(di.topo, di.vertexCount) * std::max(di.instances, 1u);
  if (nv64 == 0) return;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!Setup(c.h)) return;
  if (nv64 * 16 > g.arena.size / 4) { g.st.skipped++; return; }
  const uint32_t nv = (uint32_t)nv64;
  const double t0ms = Now();
  if (g.framePresent != g_presentCount) { g.frame.clear(); g.framePresent = g_presentCount; }
  if (g.lastDepth != t8) { if (g.lastDepth) { fprintf(stderr, "[rt] scene depth target changed: capture cache flushed (%zu entries)\n", g.entries.size()); Flush(c.h); } g.lastDepth = t8; }
  g.sceneDepth = t8;
  const uint64_t keyA = HashKey(di, false), keyB = HashKey(di, true), key = g.keyCb ? keyB : keyA;
  g.curA.insert(keyA); g.curB.insert(keyB);
  Entry& e = g.entries[key];
  e.lastSeen = (int64_t)g.capFrame + 1;   // the frame being captured (capFrame advances at the tonemap draw)
  if (e.vcount != nv) {
    if (e.vcount) { const uint64_t seen = e.seen; KillEntry(c.h, e); e.seen = (uint32_t)seen; }
    uint64_t off = 0;
    if (!g.arenaR.Alloc((uint64_t)nv * 16, &off)) { Evict(c.h, key); if (!g.arenaR.Alloc((uint64_t)nv * 16, &off)) { g.st.skipped++; if (!e.vcount) g.entries.erase(key); return; } }
    e.vcount = nv; e.arenaOff = off; e.arenaBytes = (uint64_t)nv * 16;
  }
  // camera: world = inverse(viewProjection) * clip. The matrix is column-major at +0x100.
  Mat m;
  { float f[16]; memcpy(f, di.cam + 0x100, 64); for (int r = 0; r < 4; r++) for (int cc = 0; cc < 4; cc++) m.m[r][cc] = f[cc * 4 + r]; }
  Mat inv;
  if (!Invert(m, inv)) { if (!g.loggedCamFail) { g.loggedCamFail = true; fprintf(stderr, "[rt] camera viewProjection is singular: draw not captured\n"); } return; }
  bool dup = false;
  for (auto& fd : g.frame) if (fd.key == key) dup = true;
  if (!dup) {
    FrameDraw fd{key, nv, e.arenaOff, {}, {}};
    for (int cc = 0; cc < 4; cc++) for (int r = 0; r < 4; r++) fd.inv[cc * 4 + r] = (float)inv.m[r][cc];
    memcpy(fd.eye, di.cam + 0x240, 12);
    g.frame.push_back(fd);
  }
  VkDeviceSize off = e.arenaOff, size = (VkDeviceSize)nv * 16;
  pBindXfb(c.cb, 0, 1, &g.arena.buf, &off, &size);
  pBeginXfb(c.cb, 0, 0, nullptr, nullptr);
  open = true; cx = &c;
  g.st.draws++; g.st.tris += nv / 3; g.st.xfbBytes += size;
  g.st.cpuCapMs += Now() - t0ms;
}

void DrawCapture::End() {
  if (!open) return;
  open = false;
  pEndXfb(cx->cb, 0, 0, nullptr, nullptr);
}

void OnTonemap(ReplayCtx& cx, const char*) {   // called by FxAfterDraw: tonemap draws only, never on photo frames or inside an occlusion scope
  if (!Capture() || FxIsPhotoFrame()) return;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!g.ready || g.frame.empty()) return;
  if (!cx.q || !SlotReady(g.slot % kSlots)) {   // the slot's descriptors, query results and mapped words still belong to the GPU: skip this frame's RT work (the capture is dropped, entries rebuild)
    g.frame.clear(); g.curA.clear(); g.curB.clear(); g.st.skipped++;
    return;
  }
  Host* h = cx.h; VkCommandBuffer cb = cx.cb;
  TextureImpl* t = cx.rt[0];
  std::vector<FrameDraw> draws; draws.swap(g.frame);
  { uint64_t sa = 0, sb = 0; for (uint64_t k : g.curA) sa += g.prevA.count(k); for (uint64_t k : g.curB) sb += g.prevB.count(k);   // key stability against the previous frame
    g.kA += g.curA.size(); g.kB += g.curB.size(); g.kStA += sa; g.kStB += sb; g.kFrames++;
    g.prevA.swap(g.curA); g.prevB.swap(g.curB); g.curA.clear(); g.curB.clear(); }
  if (!t || t->b.width < 640 || t->b.height < 360) return;   // photo / thumbnail targets: no work
  const double tc0 = Now();
  EndRendering(cx);
  g.capFrame++;
  const uint32_t slot = g.slot++ % kSlots;
  const bool timed = g.qp != VK_NULL_HANDLE;
  if (timed) { vkCmdResetQueryPool(cb, g.qp, slot * kQ, 5); g.slotTs[slot] = false; }
  Bar(cb);   // transform feedback writes -> convert; previous frame's trace/build reads -> this frame's writes
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * kQ + 0);
  // 1. clip -> world, in place; the same pass compares each vertex with the copy taken at the draw's last BLAS build (motion flag word per draw)
  uint32_t* flagWords = static_cast<uint32_t*>(g.flags.map) + (size_t)slot * kMaxDraws;   // this slot was read back 4 frames ago: free to rewrite
  memset(flagWords, 0, kMaxDraws * 4);
  g.slotKeys[slot].clear();
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.convPipe);
  for (size_t di = 0; di < draws.size(); di++) {
    const FrameDraw& d = draws[di];
    struct { float m[16]; uint64_t addr, shadow, flags; uint32_t count, idx; float eye[4]; } pc;
    memcpy(pc.m, d.inv, 64); pc.addr = g.arena.addr + d.arenaOff; pc.shadow = g.shadow.addr + d.arenaOff;
    pc.idx = (uint32_t)std::min<size_t>(di, kMaxDraws - 1); pc.flags = g.flags.addr + ((uint64_t)slot * kMaxDraws) * 4; pc.count = d.vcount;
    pc.eye[0] = d.eye[0]; pc.eye[1] = d.eye[1]; pc.eye[2] = d.eye[2]; pc.eye[3] = 0;
    vkCmdPushConstants(cb, g.convPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (d.vcount + 63) / 64, 1, 1);
    if (di < kMaxDraws) g.slotKeys[slot].push_back(d.key);
  }
  Bar(cb);
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * kQ + 4);   // end of the convert phase
  // 2. BLAS for every draw captured this frame (rebuilt in place when the size is unchanged)
  std::vector<VkAccelerationStructureGeometryKHR> geos(draws.size());
  std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  std::vector<uint64_t> keys;
  infos.reserve(draws.size()); ranges.reserve(draws.size()); keys.reserve(draws.size());
  uint64_t scratchUsed = 0, blasBuilt = 0, blasTris = 0, blasReused = 0;
  std::vector<VkBufferCopy> shadowCopies;   // arena -> shadow for every BLAS built this frame (the reference for the motion test)
  auto flush = [&]() {
    if (infos.empty()) return;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp(infos.size());
    for (size_t i = 0; i < infos.size(); i++) rp[i] = &ranges[i];
    pCmdBuild(cb, (uint32_t)infos.size(), infos.data(), rp.data());
    Bar(cb);   // the next batch reuses the scratch buffer
    infos.clear(); ranges.clear(); scratchUsed = 0;
  };
  for (size_t di = 0; di < draws.size(); di++) {
    auto it = g.entries.find(draws[di].key);
    if (it == g.entries.end()) continue;
    Entry& e = it->second;
    const bool contiguous = e.procFrame == (int64_t)g.capFrame - 1;   // a draw that was off screen in between may have moved: no reuse
    e.procFrame = (int64_t)g.capFrame;
    if (!contiguous) { e.calm = 0; e.isStatic = false; }
    e.lastSeen = (int64_t)g.capFrame;
    e.seen++;   // frames this draw was captured (stable = 3 or more)
    if ((g.reuse == 2 ? g.winReuse : g.reuse != 0) && e.isStatic && e.built && e.as) { blasReused++; continue; }   // unchanged since its BLAS was built: keep that BLAS
    VkAccelerationStructureGeometryKHR& geo = geos[di];
    geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; geo.geometry.triangles.vertexData.deviceAddress = g.arena.addr + e.arenaOff;
    geo.geometry.triangles.vertexStride = 16; geo.geometry.triangles.maxVertex = e.vcount - 1; geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo;
    const uint32_t prims = e.vcount / 3;
    if (!e.as) {
      VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
      pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
      uint64_t off = 0;
      if (!g.blasR.Alloc(sz.accelerationStructureSize, &off)) { Evict(h, draws[di].key); if (!g.blasR.Alloc(sz.accelerationStructureSize, &off)) { g.st.skipped++; continue; } }
      VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
      ci.buffer = g.blasStore.buf; ci.offset = off; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      if (Forced("build") || pCreateAs(h->device, &ci, nullptr, &e.as) != VK_SUCCESS) { g.blasR.Free(off, sz.accelerationStructureSize); e.as = VK_NULL_HANDLE; g.st.skipped++; if (Forced("build")) Fail("forced failure (SNAP_RT_FORCE_FAIL=build)"); else CreateFailed(); continue; }
      e.blasOff = off; e.blasBytes = sz.accelerationStructureSize; e.scratchNeed = sz.buildScratchSize;
      VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = e.as;
      e.blasAddr = pAsAddr(h->device, &ai);
    }
    const uint64_t need = (e.scratchNeed + g_scratchAlign - 1) / g_scratchAlign * g_scratchAlign;
    if (scratchUsed + need > g.scratch.size) flush();
    if (need > g.scratch.size) { g.st.skipped++; continue; }
    bi.dstAccelerationStructure = e.as; bi.scratchData.deviceAddress = g.scratch.addr + scratchUsed;
    scratchUsed += need;
    infos.push_back(bi); ranges.push_back({prims, 0, 0, 0}); keys.push_back(draws[di].key);
    e.built = true; blasBuilt++; blasTris += prims;
    shadowCopies.push_back({e.arenaOff, e.arenaOff, e.arenaBytes});
  }
  // infos hold pointers into geos (stable: sized up front); build the rest
  flush();
  if (!shadowCopies.empty()) vkCmdCopyBuffer(cb, g.arena.buf, g.shadow.buf, (uint32_t)shadowCopies.size(), shadowCopies.data());   // builds read the arena, not the shadow: no ordering needed here
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * kQ + 1);
  // 3. TLAS over everything alive
  Evict(h);
  uint32_t ninst = 0;
  {
    uint64_t bytes = (uint64_t)std::min<size_t>(g.entries.size(), kMaxInstances) * sizeof(VkAccelerationStructureInstanceKHR);
    VkBuffer src = VK_NULL_HANDLE;
    uint8_t* mp = bytes ? NewScratch(cx, bytes, &src, 0) : nullptr;
    if (mp) {
      auto* inst = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(mp);
      for (auto& kv : g.entries) {
        Entry& e = kv.second;
        if (!e.built || !e.as || ninst >= kMaxInstances) continue;
        if (e.id == ~0u) {   // a stable id (the colour cache key): a released one first, else the next unused
          if (!g.freeIds.empty()) { e.id = g.freeIds.back(); g.freeIds.pop_back(); }
          else if (g.nextId < kMaxInstances) e.id = g.nextId++;
          if (e.id == ~0u) continue;
        }
        VkAccelerationStructureInstanceKHR& I = inst[ninst];
        memset(&I, 0, sizeof I);
        I.transform.matrix[0][0] = I.transform.matrix[1][1] = I.transform.matrix[2][2] = 1.0f;
        I.instanceCustomIndex = e.id; I.mask = 0xFF; I.instanceShaderBindingTableRecordOffset = 0;
        I.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        I.accelerationStructureReference = e.blasAddr;
        ninst++;
      }
      if (ninst) { VkBufferCopy cp{0, 0, (uint64_t)ninst * sizeof(VkAccelerationStructureInstanceKHR)}; vkCmdCopyBuffer(cb, src, g.instStore.buf, 1, &cp); }
    }
  }
  if (ninst) {
    Bar(cb);
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geo.geometry.instances.data.deviceAddress = g.instStore.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo; bi.dstAccelerationStructure = g.tlas; bi.scratchData.deviceAddress = g.scratch.addr;
    VkAccelerationStructureBuildRangeInfoKHR rg{ninst, 0, 0, 0}; const VkAccelerationStructureBuildRangeInfoKHR* rgp = &rg;
    pCmdBuild(cb, 1, &bi, &rgp);
    Bar(cb);
  }
  g.tlasValid = ninst != 0; g.killsSinceBuild = 0;   // every BLAS the new TLAS names is alive (the kills above happened before it was built)
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * kQ + 2);   // end of the build phase
  g.st.blasReused += blasReused; g.st.blasBuilt += blasBuilt; g.st.blasTris += blasTris; g.st.instances += ninst; g.st.frames++;
  // 4. debug overlay (and its statistics)
  bool ovl = false;
  TextureImpl* z = FxSceneDepth();
  g.sceneDepth = nullptr;
  g.slotUsed[slot] = false;
  if (g.debug && ninst && z && z->EnsureImage() && t->EnsureImage() && t->fi && FxWritable(t)) {
    const VkFormat fmt = t->fi->vk;
    if (g.ovlFmt != fmt && g.ovlPipe) { vkDeviceWaitIdle(h->device); vkDestroyPipeline(h->device, g.ovlPipe, nullptr); g.ovlPipe = VK_NULL_HANDLE; }
    if (!g.ovlPipe) {
      VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g.ovlVs, "main", nullptr},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g.ovlFs, "main", nullptr}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState ba{}; ba.blendEnable = VK_TRUE; ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = VK_BLEND_OP_ADD; ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
      VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cbs.attachmentCount = 1; cbs.pAttachments = &ba;
      const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
      VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
      VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
      gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
      gp.pColorBlendState = &cbs; gp.pDynamicState = &ds; gp.layout = g.ovlPl;
      if (vkCreateGraphicsPipelines(h->device, VK_NULL_HANDLE, 1, &gp, nullptr, &g.ovlPipe) != VK_SUCCESS) { g.ovlPipe = VK_NULL_HANDLE; Fail("overlay pipeline"); }
      g.ovlFmt = fmt;
    }
    // camera of the first captured draw this frame (every scene draw shares the camera bank)
    SubRange rr; VkImageView rtv = t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
    const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
    if (g.ovlPipe && !g_failed && rr.yn == 1 && w >= 64 && hh >= 64) {
      VkImageLayout prevZ = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImageView zv = FxDepthRead(cx, z, &prevZ);   // never written: sampled in DEPTH_STENCIL_READ_ONLY, the tracked layout is restored below
      VkDescriptorSet set = g.sets[slot];
      VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &g.tlas;
      VkDescriptorImageInfo ii{g.samp, zv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo bi{g.stats.buf, slot * 32ull, 32};
      VkWriteDescriptorSet wr[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      wr[0].pNext = &wa; wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      wr[1].dstSet = set; wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &ii;
      wr[2].dstSet = set; wr[2].dstBinding = 2; wr[2].descriptorCount = 1; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].pBufferInfo = &bi;
      vkUpdateDescriptorSets(h->device, 3, wr, 0, nullptr);
      vkCmdFillBuffer(cb, g.stats.buf, slot * 32ull, 32, 0);
      Bar(cb);
      TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
      struct { float inv[16]; float eye[4]; float p0[4]; float size[4]; } pc{};
      memcpy(pc.inv, draws[0].inv, 64);
      memcpy(pc.eye, draws[0].eye, 12); pc.p0[0] = (float)g.debug; pc.p0[1] = (float)g.flipY; pc.p0[2] = 0.01f; pc.p0[3] = 0.05f; pc.size[0] = (float)w; pc.size[1] = (float)hh;
      g.slotFrame[slot] = g.capFrame;
      VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      ca.imageView = rtv; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, {w, hh}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
      vkCmdBeginRendering(cb, &ri);
      VkViewport vpt{0, 0, (float)w, (float)hh, 0, 1}; VkRect2D sc{{0, 0}, {w, hh}};
      vkCmdSetViewport(cb, 0, 1, &vpt); vkCmdSetScissor(cb, 0, 1, &sc);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ovlPipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ovlPl, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cb, g.ovlPl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      FxDepthDone(cx, z, prevZ);
      t->uploaded = true;
      ovl = true;
      g.slotUsed[slot] = true;
    }
  }
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * kQ + 3);
  Bar(cb);   // the capture of the next frame writes the arena again
  {   // the mapped flag and stat words written by this frame's GPU work become host visible; the slot is free again once the batch's timeline value passes
    VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    hb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; hb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; hd.memoryBarrierCount = 1; hd.pMemoryBarriers = &hb;
    vkCmdPipelineBarrier2(cb, &hd);
    g.slotDone[slot] = {cx.q->timeline, cx.q->value + 1};   // SubmitLists signals value + 1 once after the whole batch
  }
  if (ovl) {   // put back the viewport and scissor the game had set (nvn_aa.cpp does the same after its passes)
    if (cx.vpHave & 1) { const float* v = cx.vpGuest[0]; VkViewport vp = MakeNvnViewport(h->originMode, v[0], v[1], v[2], v[3], t->scale); vkCmdSetViewport(cb, 0, 1, &vp); }
    else cx.vpSet = false;
    if (cx.scHave & 1) {
      int x = cx.scGuest[0][0], y = cx.scGuest[0][1], sw = cx.scGuest[0][2], sh = cx.scGuest[0][3];
      if (x < 0) { sw += x; x = 0; }
      if (y < 0) { sh += y; y = 0; }
      sw = std::max(sw, 0); sh = std::max(sh, 0);
      const float sc = t->scale;
      if (sc != 1.0f) { x = (int)std::floor(x * sc); y = (int)std::floor(y * sc); sw = (int)std::ceil(sw * sc); sh = (int)std::ceil(sh * sc); }
      VkRect2D r{{x, y}, {(uint32_t)sw, (uint32_t)sh}}; vkCmdSetScissor(cb, 0, 1, &r);
    } else cx.scSet = false;
  }
  g.st.cpuBuildMs += Now() - tc0;
  // read back an older slot (the GPU has long finished it)
  if (g.qp) {
    const uint32_t old = (slot + kSlots - 4) % kSlots;
    uint64_t r[10] = {};
    g.slotTs[slot] = true;   // all stamps of this slot are recorded by now
    if (g.slotTs[old] && SlotReady(old) && vkGetQueryPoolResults(h->device, g.qp, old * kQ, 5, sizeof r, r, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && r[1] && r[3] && r[5] && r[7] && r[9]) {
      // stamps: r[0] start, r[8] end of convert, r[2] end of BLAS builds, r[4] end of TLAS, r[6] end of overlay
      const double ms = (double)g.tsPeriod * 1e-6;
      g.st.gpuBuildMs += (double)(r[4] - r[0]) * ms; g.st.gpuBlasMs += (double)(r[2] - r[8]) * ms; g.st.gpuConvMs += (double)(r[8] - r[0]) * ms; g.st.gpuN++;
      if (g.slotUsed[old]) { g.st.gpuOvlMs += (double)(r[6] - r[4]) * ms; g.st.ovlN++; }
    }
    if (g.slotUsed[old] && SlotReady(old)) {
      const uint32_t* s = static_cast<const uint32_t*>(g.stats.map) + old * 8;
      if (s[0]) { for (int i = 0; i < 8; i++) g.st.cls[i] += s[i]; g.st.clsN++; }
    }
  }
  // motion flags of the frame 4 slots back: a draw whose vertices stayed within tolerance of its BLAS copy for staticFrames frames keeps its BLAS
  if (SlotReady((slot + kSlots - 4) % kSlots)) {
    const uint32_t old = (slot + kSlots - 4) % kSlots;
    const uint32_t* fw = static_cast<const uint32_t*>(g.flags.map) + (size_t)old * kMaxDraws;
    const std::vector<uint64_t>& ks = g.slotKeys[old];
    for (size_t i = 0; i < ks.size(); i++) {
      auto it = g.entries.find(ks[i]);
      if (it == g.entries.end()) continue;
      Entry& e = it->second;
      g.st.rdDraws++; if (e.isStatic) g.st.rdStatic++;
      if (fw[i]) { float rv; memcpy(&rv, &fw[i], 4); g.st.rdFlag++; g.st.rdRatio += std::min(rv, 1000.0f); e.calm = 0; e.isStatic = false; }
      else if (++e.calm >= g.staticFrames) e.isStatic = true;
    }
    g.slotKeys[old].clear();
  }
}

// ======================================== RT reflections ========================================
// The water pass (nvn_ssr.cpp RunPass) calls ReflectionPass once per frame with the scene colour copy it already made. This records the per-instance colour cache update: a coarse
// grid of primary rays into the captured scene TLAS (the one built at the previous frame's tonemap draw) adds the scene colour under each ray to the hit instance, and every
// instance seen folds its mean into ravg. The water shader (RT_REFL variant) does its own reflection rays from the TLAS and reads that cache; both are passed by address.
// Fallback: any failure here only turns the RT part of the reflections off (the water pass goes on with the screen-space march alone); SNAP_RT_FORCE_FAIL=reflect tests it.
char g_reflWhy[160] = {};
void ReflFail(const char* why) { g_reflFailed = true; snprintf(g_reflWhy, sizeof g_reflWhy, "%s", why); fprintf(stderr, "[rt] reflections fall back to screen-space: %s\n", why); }
bool RSetup(Host* h) {   // g.mu held
  if (g.rpipe) return true;
  if (g_reflFailed) return false;
  if (Forced("reflect")) { ReflFail("forced failure (SNAP_RT_FORCE_FAIL=reflect)"); return false; }
  const VkBufferUsageFlags us = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (!MakeBuf(h, (uint64_t)kMaxInstances * 16, us, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g.racc, true) || !MakeBuf(h, (uint64_t)kMaxInstances * 16, us, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g.ravg, true)) { ReflFail("buffer allocation failed"); return false; }
  VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &b;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kRtAccSpv; mi.pCode = kRtAccSpv;
  if (vkCreateDescriptorSetLayout(h->device, &li, nullptr, &g.rdsl) != VK_SUCCESS || vkCreateShaderModule(h->device, &mi, nullptr, &g.rmod) != VK_SUCCESS) { ReflFail("reflection pipeline objects"); return false; }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &g.rdsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  if (vkCreatePipelineLayout(h->device, &pl, nullptr, &g.rpl) != VK_SUCCESS) { ReflFail("reflection pipeline layout"); return false; }
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, g.rmod, "main", nullptr}; ci.layout = g.rpl;
  if (vkCreateComputePipelines(h->device, h->pipelineCache, 1, &ci, nullptr, &g.rpipe) != VK_SUCCESS) { g.rpipe = VK_NULL_HANDLE; ReflFail("reflection pipeline"); return false; }
  g.rinit = false;
  return true;
}

bool ReflectOn() {
  if (!ReflectRequested() || g_reflFailed || g_failed || !g_on) return false;
  static bool reg = false;
  if (!reg) { reg = true; g_reflOn = 1; dev_param_i("SNAP_RT_REFLECT", &g_reflOn, 0, 1, 1); }   // live (ab_capture -Effect SNAP_RT_REFLECT -Values 0,1): the capture stays on, only the pass and the shader path stop
  return g_reflOn != 0;
}
const char* ReflectionNotice() { return ReflectRequested() && g_reflWhy[0] ? g_reflWhy : nullptr; }

bool CachePass(ReplayCtx& cx, VkImageView col, VkSampler samp, const double vp[4][4], ReflInfo* out) {   // shared by ReflectionPass and GiPass
  if (!col || !samp || FxIsPhotoFrame()) return false;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!g.ready || !g.tlas || !Capture()) return false;
  DemandResume();
  if (g.rDoneAt == g_presentCount) { out->tlas = g.tlasAddr; out->avg = g.ravg.addr; return true; }   // the other feature already updated the cache this frame
  if (!g.tlasValid || g.killsSinceBuild) { g.rSkip++; return false; }   // no TLAS yet, or a BLAS it names was released since it was built
  if (!RSetup(cx.h)) return false;
  VkCommandBuffer cb = cx.cb;
  // M = rows 0, 1 and 3 of the view-projection (clip.xyw = M * world + off); its inverse and the eye position for the shader
  const double M[3][3] = {{vp[0][0], vp[0][1], vp[0][2]}, {vp[1][0], vp[1][1], vp[1][2]}, {vp[3][0], vp[3][1], vp[3][2]}};
  const double off[3] = {vp[0][3], vp[1][3], vp[3][3]};
  const double c00 = M[1][1] * M[2][2] - M[1][2] * M[2][1], c01 = M[1][2] * M[2][0] - M[1][0] * M[2][2], c02 = M[1][0] * M[2][1] - M[1][1] * M[2][0];
  const double det = M[0][0] * c00 + M[0][1] * c01 + M[0][2] * c02;
  if (!(std::fabs(det) > 1e-30)) return false;
  const double id = 1.0 / det;
  double I[3][3];   // inverse of M (row-major)
  I[0][0] = c00 * id; I[1][0] = c01 * id; I[2][0] = c02 * id;
  I[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) * id; I[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * id; I[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) * id;
  I[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * id; I[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) * id; I[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * id;
  struct { uint64_t tlas, acc, avg, pad; float c0[4], c1[4], c2[4], off[4], eye[4]; uint32_t q[4]; } pc;
  static_assert(sizeof pc == 128, "push block");
  pc.tlas = g.tlasAddr; pc.acc = g.racc.addr; pc.avg = g.ravg.addr; pc.pad = 0;
  for (int r = 0; r < 3; r++) { pc.c0[r] = (float)I[r][0]; pc.c1[r] = (float)I[r][1]; pc.c2[r] = (float)I[r][2]; pc.off[r] = (float)off[r]; }
  pc.c0[3] = pc.c1[3] = pc.c2[3] = pc.off[3] = 0; pc.eye[3] = 0;
  for (int r = 0; r < 3; r++) pc.eye[r] = (float)-(I[r][0] * off[0] + I[r][1] * off[1] + I[r][2] * off[2]);
  VkDescriptorSet set = FxAllocSet(cx, g.rdsl);
  if (!set) return false;
  VkDescriptorImageInfo ii{samp, col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
  vkUpdateDescriptorSets(cx.h->device, 1, &w, 0, nullptr);
  // clear: everything on first use, later only the ids that were handed to a new draw
  if (!g.rinit) { vkCmdFillBuffer(cb, g.racc.buf, 0, VK_WHOLE_SIZE, 0); vkCmdFillBuffer(cb, g.ravg.buf, 0, VK_WHOLE_SIZE, 0); g.rinit = true; g.idReset.clear(); }
  for (uint32_t i : g.idReset) { vkCmdFillBuffer(cb, g.racc.buf, (VkDeviceSize)i * 16, 16, 0); vkCmdFillBuffer(cb, g.ravg.buf, (VkDeviceSize)i * 16, 16, 0); }
  g.idReset.clear();
  Bar(cb);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.rpipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.rpl, 0, 1, &set, 0, nullptr);
  const uint32_t gw = 256, gh = 144;
  pc.q[0] = 0; pc.q[1] = gw; pc.q[2] = gh; pc.q[3] = (uint32_t)g_presentCount;
  vkCmdPushConstants(cb, g.rpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  vkCmdDispatch(cb, (gw * gh + 63) / 64, 1, 1);
  Bar(cb);
  pc.q[0] = 1;
  vkCmdPushConstants(cb, g.rpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  vkCmdDispatch(cb, kMaxInstances / 64, 1, 1);
  Bar(cb);   // the fragment reads of ravg and the next frame's accumulation
  g.rPasses++; g.rDoneAt = g_presentCount;
  out->tlas = g.tlasAddr; out->avg = g.ravg.addr;
  return true;
}
bool ReflectionPass(ReplayCtx& cx, VkImageView col, VkSampler samp, const double vp[4][4], ReflInfo* out) { if (!ReflectOn()) return false; NoteDemand(); return CachePass(cx, col, samp, vp, out); }

// ---- RT bounce light (SNAP_RT_GI): the ray traced gather of the bounce-light chain (nvn_lighting_fx.cpp). The gather shader (lfx_gi_rt.frag) is ours; the chain, the denoiser and the
// composite are the lighting file's. Same fallback shape as the reflections: a failure only returns the chain to its screen-space gather.
char g_giWhy[160] = {};
void GiFail(const char* why) { g_giFailed = true; snprintf(g_giWhy, sizeof g_giWhy, "%s", why); fprintf(stderr, "[rt] bounce light falls back to screen-space: %s\n", why); }
bool GiOn() {
  if (!GiRequested() || g_giFailed || g_failed || !g_on) return false;
  static bool reg = false;
  if (!reg) { reg = true; g_giOn = 1; dev_param_i("SNAP_RT_GI", &g_giOn, 0, 1, 1); }
  return g_giOn != 0;
}
const char* GiNotice() { return GiRequested() && g_giWhy[0] ? g_giWhy : nullptr; }
VkShaderModule GiModule(Host* h) {
  if (g.gmod) return g.gmod;
  if (Forced("gi")) { GiFail("forced failure (SNAP_RT_FORCE_FAIL=gi)"); return VK_NULL_HANDLE; }
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kLfxGiRtSpv; mi.pCode = kLfxGiRtSpv;
  if (vkCreateShaderModule(h->device, &mi, nullptr, &g.gmod) != VK_SUCCESS) { g.gmod = VK_NULL_HANDLE; GiFail("gather shader module"); }
  return g.gmod;
}
bool GiPass(ReplayCtx& cx, VkImageView col, VkSampler samp, const double vp[4][4], ReflInfo* out) { if (!GiOn()) return false; NoteDemand(); return CachePass(cx, col, samp, vp, out); }

// ---- RT ambient occlusion (SNAP_RT_AO): the ray traced variant of the GTAO pass of the enhanced-lighting chain (nvn_lighting_fx.cpp). The shader (lfx_ao_rt.frag) only needs the
// TLAS, not the colour cache. A failure returns the chain to GTAO.
char g_aoWhy[160] = {};
void AoFail(const char* why) { g_aoFailed = true; snprintf(g_aoWhy, sizeof g_aoWhy, "%s", why); fprintf(stderr, "[rt] ambient occlusion falls back to GTAO: %s\n", why); }
bool AoOn() {
  if (!AoRequested() || g_aoFailed || g_failed || !g_on) return false;
  static bool reg = false;
  if (!reg) { reg = true; g_aoOn = 1; dev_param_i("SNAP_RT_AO", &g_aoOn, 0, 1, 1); }
  return g_aoOn != 0;
}
const char* AoNotice() { return AoRequested() && g_aoWhy[0] ? g_aoWhy : nullptr; }
VkShaderModule AoModule(Host* h) {
  if (g.amod) return g.amod;
  if (Forced("ao")) { AoFail("forced failure (SNAP_RT_FORCE_FAIL=ao)"); return VK_NULL_HANDLE; }
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kLfxAoRtSpv; mi.pCode = kLfxAoRtSpv;
  if (vkCreateShaderModule(h->device, &mi, nullptr, &g.amod) != VK_SUCCESS) { g.amod = VK_NULL_HANDLE; AoFail("AO shader module"); }
  return g.amod;
}
bool AoPass(uint64_t* tlas) {
  if (!AoOn() || FxIsPhotoFrame()) return false;
  NoteDemand();
  std::lock_guard<std::mutex> lk(g.mu);
  if (!g.ready || !g.tlas || !Capture()) return false;
  DemandResume();
  if (!g.tlasValid || g.killsSinceBuild) { g.aoSkip++; return false; }
  g.aoPasses++;
  *tlas = g.tlasAddr;
  return true;
}

void OnPresent() {
  if (!g.ready || g.logEvery <= 0 || g_presentCount - g.lastLogPresent < g.logEvery) return;
  g.lastLogPresent = g_presentCount;
  std::lock_guard<std::mutex> lk(g.mu);
  if (S.frames) {   // M3
    fprintf(stderr, "[rt] p%d shadows: %llu builds in the window; per build %.1f cast draws, %.0f tris, %u instances; GPU convert+BLAS+TLAS %.3f ms (%u samples); receivers prepared %llu (%llu without a receiver matrix), skipped draws %llu; sun L (%.3f %.3f %.3f) range %.1f, receiver xy/depth %.3f, mode %d dbg %d rays %d\n",
            g_presentCount, (unsigned long long)S.frames, (double)S.draws_n / (double)S.frames, (double)S.tris / (double)S.frames, S.ninst, S.gpuN ? S.gpuMs / S.gpuN : 0.0, S.gpuN, (unsigned long long)S.receivers,
            (unsigned long long)S.noRecvM, (unsigned long long)S.skipped, S.sunL[0], S.sunL[1], S.sunL[2], S.tmax, S.recvRatio, Cfg().mode, Cfg().dbg, ShadowRays());
    S.frames = S.draws_n = S.tris = S.receivers = S.noRecvM = 0; S.gpuMs = 0; S.gpuN = 0;
  }
  if (AoRequested() && (g.aoPasses || g.aoSkip)) { fprintf(stderr, "[rt] p%d ambient occlusion: %llu RT passes, %llu skipped (no TLAS yet or a BLAS was released)\n", g_presentCount, (unsigned long long)g.aoPasses, (unsigned long long)g.aoSkip); g.aoPasses = g.aoSkip = 0; }
  if ((ReflectRequested() || GiRequested()) && (g.rPasses || g.rSkip)) { fprintf(stderr, "[rt] p%d colour cache (reflections, bounce light): %llu passes, %llu skipped (no TLAS yet or a BLAS was released since it was built); %u instance ids in use, reflections %s, bounce light %s\n", g_presentCount, (unsigned long long)g.rPasses, (unsigned long long)g.rSkip, g.nextId - (uint32_t)g.freeIds.size(), !ReflectRequested() ? "not asked" : g_reflFailed ? "fell back" : g_reflOn ? "on" : "off (live)", !GiRequested() ? "not asked" : g_giFailed ? "fell back" : g_giOn ? "on" : "off (live)"); g.rPasses = g.rSkip = 0; }
  Stats& s = g.st;
  if (!s.frames) return;
  const double f = (double)s.frames;
  fprintf(stderr, "[rt] p%d: %u frames: %.0f draws, %.0f tris captured (%.1f MB xfb) per frame; BLAS built %.0f (%.0f tris), reused %.0f per frame; %.0f instances; live entries %zu, arena %.1f/%llu MB (hwm %.1f), BLAS %.1f/%llu MB (hwm %.1f)%s\n",
          g_presentCount, s.frames, (double)s.draws / f, (double)s.tris / f, (double)s.xfbBytes / f / 1048576.0, (double)s.blasBuilt / f, (double)s.blasTris / f, (double)s.blasReused / f, (double)s.instances / f, g.entries.size(),
          (double)g.arenaR.used / 1048576.0, (unsigned long long)(g.arenaR.cap >> 20), (double)g.arenaR.hwm / 1048576.0, (double)g.blasR.used / 1048576.0, (unsigned long long)(g.blasR.cap >> 20), (double)g.blasR.hwm / 1048576.0,
          s.skipped ? " (some draws skipped: raise SNAP_RT_ARENA_MB/BLAS_MB/SCRATCH_MB)" : "");
  fprintf(stderr, "[rt]   cost: CPU capture %.3f ms + build record %.3f ms per frame; GPU convert+BLAS+TLAS %.3f ms (convert %.3f, BLAS %.3f, %u samples)", s.cpuCapMs / f, s.cpuBuildMs / f, s.gpuN ? s.gpuBuildMs / s.gpuN : 0.0, s.gpuN ? s.gpuConvMs / s.gpuN : 0.0, s.gpuN ? s.gpuBlasMs / s.gpuN : 0.0, s.gpuN);
  if (g.reuse == 2) fprintf(stderr, "; BLAS reuse %s", g.winReuse ? "ON" : "off");
  if (s.rdDraws) fprintf(stderr, "; motion test: %.1f%% of draws moved (mean ratio %.1f), %.1f%% were static", 100.0 * s.rdFlag / s.rdDraws, s.rdFlag ? s.rdRatio / s.rdFlag : 0.0, 100.0 * s.rdStatic / s.rdDraws);
  if (s.ovlN) fprintf(stderr, "; GPU overlay %.3f ms (%u samples)", s.gpuOvlMs / s.ovlN, s.ovlN);
  fprintf(stderr, "\n");
  if (g.reuse == 2) g.winReuse = !g.winReuse;   // A/B: the next window runs the other way
  if (g.kFrames) { const double kf = (double)g.kFrames; fprintf(stderr, "[rt]   draw keys per frame: %.1f draws; %.1f distinct by mesh identity (%.0f%% also in the previous frame), %.1f distinct with per-object constants (%.0f%% also in the previous frame); key in use: %s\n", (double)s.draws / f, (double)g.kA / kf, g.kA ? 100.0 * g.kStA / g.kA : 0.0, (double)g.kB / kf, g.kB ? 100.0 * g.kStB / g.kB : 0.0, g.keyCb ? "with constants" : "identity"); g.kA = g.kB = g.kStA = g.kStB = g.kFrames = 0; }
  if (s.clsN && s.cls[0]) {
    const double n = (double)s.cls[0];
    fprintf(stderr, "[rt]   sanity (%u frames, %.0f sampled px): match %.1f%%, rt-in-front %.1f%%, rt-behind %.1f%%, depth-no-hit %.1f%%, sky %.1f%%, hit-on-sky %.1f%%\n", s.clsN, n,
            100.0 * s.cls[1] / n, 100.0 * s.cls[2] / n, 100.0 * s.cls[3] / n, 100.0 * s.cls[4] / n, 100.0 * s.cls[5] / n, 100.0 * s.cls[6] / n);
  }
  s = Stats{};
}

}  // namespace snap_rt
