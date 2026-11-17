// SPDX-License-Identifier: MIT
// GPU-side half of the SNAP_PERF instrumentation (runtime/os/perf.h): a ring of timestamp pairs around every command buffer the
// layer submits. Slots are collected at the next present once the queue's timeline has passed them. Only active with SNAP_PERF set.
// All entry points except PerfFrameMark are called with Host::queueMutex held (SubmitLists / PresentWindow); it takes the lock itself.
// The performance overlay's Detailed mode (runtime/ui/ui_fps.cpp) turns the same GPU timing on at run time (snap_perf_overlay_timing)
// and reads GPU busy ms and the presenting thread's CPU ms per frame (snap_perf_overlay_info); no CSV is written then.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nvn_aa.h"
#include "nvn_dev.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "os/perf.h"
#include "upscale/upscaler.h"

namespace nvn {

std::atomic<bool> g_overlayTiming{false};
std::atomic<bool> g_devTiming{false};   // nvn_dev.h: the dev overlay or `timing on` wants pass timestamps (SNAP_DEV=1 only)
bool PerfGpuOn() { return snap_perf::on || g_overlayTiming.load(std::memory_order_relaxed) || g_devTiming.load(std::memory_order_relaxed); }
namespace {
std::atomic<double> g_ovGpuMs{-1.0}, g_ovCpuMs{-1.0};
}  // namespace

namespace {
constexpr uint32_t kSlots = 2048;
struct Slot { uint64_t value = 0; bool submitted = false; bool used = false; };
VkQueryPool g_pool = VK_NULL_HANDLE;
Slot g_slots[kSlots];
uint32_t g_next = 0;
bool g_failed = false;
// SNAP_GPU_PASSES: marks per command buffer slot
struct Mark { uint32_t q; uint32_t draws; char label[96]; };
std::vector<Mark> g_marks[kSlots];
uint32_t g_startDraws[kSlots], g_endDraws[kSlots];
int g_curSlot = -1;
VkCommandBuffer g_curCb = VK_NULL_HANDLE;
VkQueryPool g_mpool = VK_NULL_HANDLE;
constexpr uint32_t kMarkQ = 32768;
uint32_t g_markNext = 0;
FILE* g_pf = nullptr;
}  // namespace

bool g_perfPassOn = false;
uint32_t g_perfDraws = 0;

static void PerfMarkRaw(Host* h, VkCommandBuffer cb, const char* label) {
  if (g_curSlot < 0 || cb != g_curCb) return;
  if (!g_mpool) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kMarkQ;
    if (vkCreateQueryPool(h->device, &qi, nullptr, &g_mpool) != VK_SUCCESS) { g_perfPassOn = false; g_devTiming.store(false); return; }
  }
  const uint32_t q = g_markNext++ % kMarkQ;
  vkResetQueryPool(h->device, g_mpool, q, 1);
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_mpool, q);
  Mark m{q, g_perfDraws, {}};
  snprintf(m.label, sizeof m.label, "%s", label);
  g_marks[g_curSlot].push_back(m);
}
void PerfMark(Host* h, VkCommandBuffer cb, const char* label) {
  if (!g_perfPassOn) return;
  PerfMarkRaw(h, cb, label);
}

// nvn_dev.h: the dev pass marks bypass g_perfPassOn (the renderer's own marks stay off unless asked for) but share the timestamp slots
void DevPassBegin(VkCommandBuffer cb, const char* name) { if (g_devTiming.load(std::memory_order_relaxed) && g_host) PerfMarkRaw(g_host, cb, name); }
void DevPassEnd(VkCommandBuffer cb, const char* name) {
  if (!g_devTiming.load(std::memory_order_relaxed) || !g_host) return;
  char lb[96]; snprintf(lb, sizeof lb, "(end %s)", name);
  PerfMarkRaw(g_host, cb, lb);
}

bool PerfGpuReady(Host* h) {
  if (!PerfGpuOn() || g_failed) return false;
  if (g_pool) return true;
  if (const char* pp = getenv("SNAP_GPU_PASSES"); pp && *pp && (g_pf = fopen(pp, "w"))) {
    fprintf(g_pf, "present,slot,idx,ms,draws,label\n");
    g_perfPassOn = true;
  }
  if (!h->hostQueryReset) { g_failed = true; return false; }
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * 2;
  if (vkCreateQueryPool(h->device, &qi, nullptr, &g_pool) != VK_SUCCESS) { g_failed = true; g_pool = VK_NULL_HANDLE; return false; }
  return true;
}

int PerfGpuBegin(Host* h, VkCommandBuffer cb) {
  if (!PerfGpuReady(h)) return -1;
  const uint32_t s = g_next++ % kSlots;
  if (g_slots[s].used) return -1;   // ring full of uncollected slots: skip this one
  g_slots[s] = {0, false, true};
  vkResetQueryPool(h->device, g_pool, s * 2, 2);
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_pool, s * 2);
  g_marks[s].clear(); g_startDraws[s] = g_perfDraws; g_curSlot = (int)s; g_curCb = cb;
  return (int)s;
}

void PerfGpuEnd(Host*, VkCommandBuffer cb, int slot) {
  if (slot < 0) return;
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pool, (uint32_t)slot * 2 + 1);
  g_endDraws[slot] = g_perfDraws; g_curSlot = -1; g_curCb = VK_NULL_HANDLE;
}

void PerfGpuSubmitted(int slot, uint64_t value) {
  if (slot < 0) return;
  g_slots[slot].value = value; g_slots[slot].submitted = true;
}

static char g_devName[256], g_devDriver[64];   // for the benchmark's settings summary (snap_perf_device_info); written once, from the present thread

void PerfFrameMark(QueueImpl* q, uint64_t presentNo) {
  if (!g_devName[0]) {
    const VkPhysicalDeviceProperties& p = q->host->props;
    snprintf(g_devName, sizeof g_devName, "%s", p.deviceName);
    if (p.vendorID == 0x10de) snprintf(g_devDriver, sizeof g_devDriver, "%u.%u", (p.driverVersion >> 22) & 0x3ff, (p.driverVersion >> 14) & 0xff);   // NVIDIA's packing
    else snprintf(g_devDriver, sizeof g_devDriver, "%u.%u.%u", VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion));
  }
  if (!PerfGpuOn()) return;
  std::lock_guard<std::mutex> lk(q->host->queueMutex);
  double busy = -1, span = -1; int cbs = 0;
  const bool devOn = g_devTiming.load(std::memory_order_relaxed);
  if (g_pool) {
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(q->host->device, q->timeline, &done);
    const double period = q->host->props.limits.timestampPeriod;
    uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
    for (uint32_t s = 0; s < kSlots; s++) {
      Slot& sl = g_slots[s];
      if (!sl.used || !sl.submitted || sl.value > done) continue;
      uint64_t t[2] = {0, 0};
      if (vkGetQueryPoolResults(q->host->device, g_pool, s * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        sum += t[1] - t[0]; lo = std::min(lo, t[0]); hi = std::max(hi, t[1]); cbs++;
        if ((g_pf || devOn) && !g_marks[s].empty()) {   // per-pass rows: each segment runs from its mark to the next one (or the end)
          const auto& ms = g_marks[s];
          const double per = q->host->props.limits.timestampPeriod / 1e6;
          uint64_t prev = t[0]; uint32_t prevDraws = g_startDraws[s]; const char* prevLabel = "cb-start"; int idx = 0;
          for (size_t k = 0; k <= ms.size(); k++) {
            uint64_t at = t[1]; uint32_t dr = g_endDraws[s];
            if (k < ms.size()) {
              uint64_t mt = 0;
              if (vkGetQueryPoolResults(q->host->device, g_mpool, ms[k].q, 1, 8, &mt, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) mt = prev;
              at = mt; dr = ms[k].draws;
            }
            if (at < prev) at = prev;
            if (g_pf) fprintf(g_pf, "%llu,%u,%d,%.4f,%u,%s\n", (unsigned long long)presentNo, s, idx, (double)(at - prev) * per, dr - prevDraws, prevLabel);
            if (devOn) dev_pass_sample(prevLabel, (double)(at - prev) * per);
            idx++;
            prev = at; prevDraws = dr;
            if (k < ms.size()) prevLabel = ms[k].label;
          }
        }
      }
      sl.used = false; sl.submitted = false;
    }
    busy = (double)sum * period / 1e6;
    span = hi > lo ? (double)(hi - lo) * period / 1e6 : 0;
  }
  if (g_overlayTiming.load(std::memory_order_relaxed)) {
    // CPU: the presenting thread's cycle count per frame (the SNAP_PERF cpu_mcycles source), in ms via the TSC rate measured
    // against QueryPerformanceCounter (QueryThreadCycleTime counts TSC ticks)
    // presents can come from different guest worker threads: a delta counts only when the previous present was on this thread too
    static uint64_t prevCyc = 0; static uint64_t prevTid = 0;
    const uint64_t tid = nvn::os_host::current_thread_id();
    if (tid != prevTid) { prevTid = tid; prevCyc = 0; }
    static uint64_t tsc0 = 0, ns0 = 0;
    static double tscPerMs = 0;
    uint64_t cyc = 0;
    if (nvn::os_host::thread_cycle_time(&cyc)) {
      const uint64_t tsc = nvn::os_host::rdtsc();
      const uint64_t nowNs = nvn::os_host::monotonic_ns();
      if (!ns0) { tsc0 = tsc; ns0 = nowNs; }
      const double ms = (double)(nowNs - ns0) * 1e-6;
      if (ms > 200.0) { tscPerMs = (double)(tsc - tsc0) / ms; if (ms > 5000.0) { tsc0 = tsc; ns0 = nowNs; } }
      if (prevCyc && tscPerMs > 0 && cyc >= prevCyc) g_ovCpuMs.store((double)(cyc - prevCyc) / tscPerMs);
      prevCyc = cyc;
    }
    if (busy >= 0 && cbs > 0) g_ovGpuMs.store(busy);
  }
  if (g_pf) fflush(g_pf);
  if (devOn && busy >= 0 && cbs > 0) dev_gpu_frame(busy);
  if (snap_perf::on) snap_perf::frame_mark(presentNo, busy, span, cbs);
}

}  // namespace nvn

// The graphics card name and driver version of the running game (empty until the first present).
extern "C" void snap_perf_device_info(char* name, int name_len, char* driver, int driver_len) {
  if (name && name_len > 0) snprintf(name, (size_t)name_len, "%s", nvn::g_devName);
  if (driver && driver_len > 0) snprintf(driver, (size_t)driver_len, "%s", nvn::g_devDriver);
}

extern "C" void snap_perf_overlay_timing(int on) {
  nvn::g_overlayTiming.store(on != 0);
  if (!on) { nvn::g_ovGpuMs.store(-1.0); nvn::g_ovCpuMs.store(-1.0); }
}

// Values for the Detailed overlay: GPU busy and CPU ms of the last collected frame (-1 = none yet), the 3D scene size, the game's
// pace rate and a one-line upscaler / anti-aliasing description (aa may be null or aa_len 0).
extern "C" void snap_perf_overlay_info(double* gpu_ms, double* cpu_ms, int* render_w, int* render_h, double* pace_hz, char* aa, int aa_len) {
  using namespace nvn;
  if (gpu_ms) *gpu_ms = g_ovGpuMs.load();
  if (cpu_ms) *cpu_ms = g_ovCpuMs.load();
  if (render_w) *render_w = (int)(1920.0f * g_scale.scale + 0.5f);
  if (render_h) *render_h = (int)(1080.0f * g_scale.scale + 0.5f);
  if (pace_hz) *pace_hz = g_paceHz;
  if (aa && aa_len > 0) {
    const char* up = snap_upscale::Name();
    const char* m = g_aaMode == AA_SMAA ? "SMAA" : g_aaMode == AA_OFF ? "AA off" : "FXAA (original)";
    char sh[24] = "";
    if (g_aaSharpen > 0.f) snprintf(sh, sizeof sh, "  CAS %.2f", g_aaSharpen);
    if (up && strcmp(up, "off")) snprintf(aa, (size_t)aa_len, "%s  %s%s", up, m, sh);
    else snprintf(aa, (size_t)aa_len, "%s%s", m, sh);
  }
}

// ---- nvn_dev.h glue (SNAP_DEV=1 only) ----
void dev_pass_begin_impl(VkCommandBuffer cb, const char* name) { nvn::DevPassBegin(cb, name); }
void dev_pass_end_impl(VkCommandBuffer cb, const char* name) { nvn::DevPassEnd(cb, name); }
// Turns the dev timestamp marks on or off (present thread: the overlay toggle and ctl `timing` / `renderer`). The renderer's own marks (rp, cs, aa, fx...)
// are booked too only with `renderer on`; they are many per frame and cost GPU time themselves.
void dev_timing_apply(bool on) {
  nvn::g_devTiming.store(on);
  if (!nvn::g_pf) nvn::g_perfPassOn = on && dev_renderer_marks();
}
