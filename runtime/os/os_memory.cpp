// nn::os memory as host overrides (written for this project): the virtual address memory manager (VAMM:
// AllocateAddressRegion / AllocateMemoryPages / ...) and the memory heap (SetMemoryHeapSize / AllocateMemoryBlock).
// Guest addresses are host addresses, so both are thin layers over the host's virtual memory (osk::host::mem_*). The sdk implementation keeps its state
// in OsResourceManager (GOT sdk+0xc88838), which nothing constructs on the host; every entry point that reads it
// is replaced here or in os_thread.cpp (see docs/horizon-os.md, "boot").
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "os_host.h"
#include "os_internal.h"

using namespace osk;

namespace {

// Results seen in the sdk: VammManager::AllocateAddressRegion sdk+0x346f48/+0x346f6c, AllocateMemoryPages
// sdk+0x3476fc/+0x347754.
constexpr uint32_t R_OUT_OF_MEMORY = 0x1003, R_OUT_OF_ADDRESS_SPACE = 0x1803, R_INVALID_RANGE = 0xe03;
constexpr uint64_t PAGE = 0x1000;
constexpr uint64_t HEAP_UNIT = 0x200000;           // nn::os::MemoryHeapUnitSize
constexpr uint64_t HEAP_RESERVE = 16ull << 30;     // address space reserved for the heap (committed on demand)

// [perf] Guest reservations carry MEM_WRITE_WATCH so the NVN layer can copy only the pages the game wrote into its mirrored
// buffers (GetWriteWatch, runtime/nvn/nvn_vk.cpp SyncPoolRange; perf pass 4). The flag can only be set at reserve time.
// SNAP_SYNC_FULL=1 or SNAP_WRITE_WATCH=0 reserve without it (old behavior; the NVN layer then copies whole ranges).
// Where the host cannot track writes (host::write_watch_supported() is false, the case on Linux for now) reservations are always plain.
bool ww_flag() {
    static const bool f = host::write_watch_wanted(host::write_watch_supported(), getenv("SNAP_SYNC_FULL"), getenv("SNAP_WRITE_WATCH"));
    return f;
}

std::mutex g_mm;
struct Region { uint64_t size, committed; };
std::map<uint64_t, Region> g_regions;              // VAMM address regions by base
uint64_t g_reserved_total = 0, g_committed_total = 0;
// Save states: while a state exists, a region the game frees keeps its address reserved (pages decommitted) instead of going back to the OS, so a load
// can bring the region back at the same address; without this the host heap tends to take the freed address and the load has to be refused.
std::map<uint64_t, uint64_t> g_limbo;   // base -> size
uint64_t g_limbo_bytes = 0;
bool g_limbo_on = false;
constexpr uint64_t LIMBO_MAX = 8ull << 40;   // address space only (the pages are decommitted); a sanity bound, not a budget

uint64_t page_up(uint64_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

Region *region_of(uint64_t a, uint64_t size) {   // under g_mm: the region that contains [a, a+size)
    auto it = g_regions.upper_bound(a);
    if (it == g_regions.begin()) return nullptr;
    --it;
    if (a + size > it->first + it->second.size || a + size < a) return nullptr;
    return &it->second;
}

// Crash main+0x99c88c / 0x99c910 (2 runs, 2026-11-12): the game's 2 MiB page heap (main+0x8dc340, nu::PageHeap) takes its large
// blocks from AllocateAddressRegion + AllocateMemoryPages and, when either fails, its caller (0x99c7f0) memsets the NULL result.
// On the Switch these never fail inside the game's budget; on the host they fail only under transient Windows memory pressure
// (commit limit reached by other processes). So a failed reserve/commit with a transient error (commit limit, out of memory, no
// system resources) is retried, without holding g_mm, with a backoff of 1, 2, 4 .. 100 ms for up to SNAP_VAMM_RETRY_MS (default 5000;
// 0 = fail at once, the old behavior); any other error fails at once. The guest caller holds the page heap's lock meanwhile, so the
// budget is short: it only has to outlast a brief pressure spike, and the alternative is the crash.
// Test switch SNAP_FAULT_VAMM=<s>:<n>: from <s> seconds after the first VAMM call, fail the next n reserves/commits (simulated
// pressure, off by default).
uint64_t retry_ms() {
    static const uint64_t v = [] { const char *e = getenv("SNAP_VAMM_RETRY_MS"); return e && *e ? strtoull(e, nullptr, 0) : 5000ull; }();
    return v;
}
bool fault_now(host::MemErr *err = nullptr) {
    static long long s = 0, n = 0;
    static std::atomic<long long> hits{0};
    static const bool on = [] { const char *e = getenv("SNAP_FAULT_VAMM"); return e && sscanf(e, "%lld:%lld", &s, &n) == 2 && n > 0; }();
    if (!on) return false;
    static const int64_t t0 = host::counter();
    if (host::counter() - t0 < s * host::counter_frequency()) return false;
    if (hits.fetch_add(1) >= n) return false;
    const host::MemErr e = host::mem_injected_error();
    if (err) *err = e;
    return true;
}
bool vamm_wait(const char *what, uint64_t size, uint64_t &waited, const host::MemErr &err) {   // true = retry after a pause
    const uint64_t lim = err.transient ? retry_ms() : 0;
    if (waited == 0) {
        uint64_t res, com;
        size_t nreg;
        {   // review finding 9: the totals are read under g_mm
            std::lock_guard<std::mutex> lk(g_mm);
            res = g_reserved_total, com = g_committed_total, nreg = g_regions.size();
        }
        fprintf(stderr, "[os-mem] %s(%#llx) failed: error %lu (reserved %#llx, committed %#llx, %zu regions)%s\n", what, (unsigned long long)size, err.code,
                (unsigned long long)res, (unsigned long long)com, nreg, lim ? ": retrying" : "");
    }
    if (waited >= lim) {
        if (lim) fprintf(stderr, "[os-mem] %s(%#llx) still failing after %llu ms: giving up\n", what, (unsigned long long)size, (unsigned long long)waited);
        return false;
    }
    static thread_local uint64_t step = 1;
    if (waited == 0) step = 1;
    os_io_begin();   // the backoff does not hold a virtual core (finding 9; SNAP_SCHED_LEGACY=hostblock keeps it)
    host::sleep_ms((unsigned)step);
    os_io_end();
    waited += step;
    step = std::min<uint64_t>(step * 2, 100);
    return true;
}
// Horizon VAMM aligns a region base by its size class (SDK find-fit): < 2 MB: 64 KB, < 4 MB: 2 MB, < 32 MB: 4 MB, < 1 GB: 32 MB, else 1 GB.
// The game's jemalloc extent hook needs 2 MB-aligned bases; with the plain 64 KB-aligned Windows base it missed ~31 times in 32,
// freed the block and kept an interior pointer of a bigger one, whose later free (exact-base lookup) is a no-op, so 4-8 MB per extent
// stayed committed for good (hub loop leak, 2026-11-14). SNAP_VAMM_ALIGN=0 restores the old 64 KB behavior.
uint64_t vamm_align_for(uint64_t size) {
    static const bool on = [] { const char *e = getenv("SNAP_VAMM_ALIGN"); return !(e && *e == '0'); }();
    if (!on) return PAGE << 4;
    if (size < (2ull << 20)) return 64ull << 10;
    if (size < (4ull << 20)) return 2ull << 20;
    if (size < (32ull << 20)) return 4ull << 20;
    if (size < (1ull << 30)) return 32ull << 20;
    return 1ull << 30;
}
void *reserve_aligned(uint64_t size, uint64_t align, host::MemErr *err) { return host::mem_reserve(size, align, ww_flag(), err); }
uint32_t vamm_alloc_region(uint64_t out, uint64_t size) {
    if (!size) return R_INVALID_RANGE;
    size = page_up(size);
    void *p = nullptr;
    const uint64_t align = vamm_align_for(size);
    for (uint64_t waited = 0;;) {
        host::MemErr err;
        p = fault_now(&err) ? nullptr : reserve_aligned(size, align, &err);
        if (p) {
            if (waited) fprintf(stderr, "[os-mem] AllocateAddressRegion(%#llx) succeeded after %llu ms\n", (unsigned long long)size, (unsigned long long)waited);
            break;
        }
        if (!vamm_wait("AllocateAddressRegion", size, waited, err)) return R_OUT_OF_ADDRESS_SPACE;
    }
    std::lock_guard<std::mutex> lk(g_mm);
    g_regions[(uint64_t)(uintptr_t)p] = Region{size, 0};
    g_reserved_total += size;
    gst<uint64_t>(out, (uint64_t)(uintptr_t)p);
    return R_SUCCESS;
}
uint32_t vamm_alloc_pages(uint64_t a, uint64_t size) {
    if (!size || (a & (PAGE - 1))) return R_INVALID_RANGE;
    size = page_up(size);
    for (uint64_t waited = 0;;) {
        host::MemErr err;
        {
            std::lock_guard<std::mutex> lk(g_mm);
            Region *r = region_of(a, size);
            if (!r) return R_INVALID_RANGE;
            // Count only pages that were not committed yet (committing twice is legal and keeps the contents).
            size_t have = 0, fresh = 0;
            host::mem_count((void *)(uintptr_t)a, size, &have, &fresh);
            if (!fault_now(&err) && host::mem_commit((void *)(uintptr_t)a, size, &err)) {
                r->committed += fresh;
                g_committed_total += fresh;
                if (waited) fprintf(stderr, "[os-mem] AllocateMemoryPages(%#llx) succeeded after %llu ms\n", (unsigned long long)size, (unsigned long long)waited);
                return R_SUCCESS;
            }
        }
        if (!vamm_wait("AllocateMemoryPages", size, waited, err)) return R_OUT_OF_MEMORY;   // waits without g_mm: frees can proceed
    }
}
uint32_t vamm_free_pages(uint64_t a, uint64_t size) {
    if (!size || (a & (PAGE - 1))) return R_INVALID_RANGE;
    size = page_up(size);
    std::lock_guard<std::mutex> lk(g_mm);
    Region *r = region_of(a, size);
    if (!r) return R_INVALID_RANGE;
    size_t gone = 0, rest = 0;
    host::mem_count((void *)(uintptr_t)a, size, &gone, &rest);
    host::mem_decommit((void *)(uintptr_t)a, size);
    r->committed -= std::min(r->committed, gone);
    g_committed_total -= std::min(g_committed_total, gone);
    return R_SUCCESS;
}
uint32_t vamm_free_region(uint64_t a) {
    std::lock_guard<std::mutex> lk(g_mm);
    auto it = g_regions.find(a);
    if (it == g_regions.end()) {
        static std::atomic<int> bad{0};
        const int n = ++bad;
        if (getenv("SNAP_VAMM_TRACE") && (n <= 20 || (n & (n - 1)) == 0)) {   // trace mode: say when the address lies inside a live region
            Region *in = region_of(a, 1);
            fprintf(stderr, "[os-mem] FreeAddressRegion(%#llx) invalid range (#%d)%s\n",(unsigned long long)a, n, in ? ": interior pointer of a live region" : "");
        }
        return R_INVALID_RANGE;
    }
    if (g_limbo_on && g_limbo_bytes + it->second.size <= LIMBO_MAX) {
        host::mem_decommit((void *)(uintptr_t)a, it->second.size);
        g_limbo[a] = it->second.size;
        g_limbo_bytes += it->second.size;
    } else {
        host::mem_release((void *)(uintptr_t)a, it->second.size);
    }
    g_reserved_total -= it->second.size;
    g_committed_total -= std::min(g_committed_total, it->second.committed);
    g_regions.erase(it);
    return R_SUCCESS;
}
uint32_t vamm_alloc_memory(uint64_t out, uint64_t size) {   // reserve + commit
    uint32_t r = vamm_alloc_region(out, size);
    if (r) return r;
    uint64_t a = gld<uint64_t>(out);
    r = vamm_alloc_pages(a, size);
    if (r) {
        vamm_free_region(a);
        gst<uint64_t>(out, 0);
    }
    return r;
}

// SNAP_VAMM_TRACE=1 (soak leak hunt, off by default): one [vamm] line per guest alloc/free call with size and guest return address.
bool vamm_trace() { static const bool on = getenv("SNAP_VAMM_TRACE") && atoi(getenv("SNAP_VAMM_TRACE")) > 0; return on; }
void vtrace(const char *op, uint64_t a, uint64_t size, Cpu *c) {
    if (!vamm_trace()) return;
    char chain[256]; int n = 0;   // guest frame-pointer chain (x29 -> {prev fp, lr}), 8 frames at most; stops at a non-stack-looking pointer
    uint64_t fp = c->x[29];
    for (int i = 0; i < 8 && fp >= c->sp && fp - c->sp < 0x100000 && !(fp & 7) && n < 220; i++) {
        uint64_t lr = gld<uint64_t>(fp + 8), nfp = gld<uint64_t>(fp);
        n += snprintf(chain + n, sizeof chain - n, " %llx", (unsigned long long)(lr & 0xffffffffull));
        if (nfp <= fp || nfp - fp > 0x100000) break;
        fp = nfp;
    }
    chain[n] = 0;
    fprintf(stderr, "[vamm] %s a=%#llx size=%#llx lr=%#llx chain:%s\n", op, (unsigned long long)a, (unsigned long long)size, (unsigned long long)c->x[30], chain);
}
void ov_AllocateAddressRegion(Cpu *c) { const uint64_t out = c->x[0], sz = c->x[1]; const uint32_t r = vamm_alloc_region(out, sz); if (!r) vtrace("allocRegion", gld<uint64_t>(out), sz, c); c->x[0] = r; }
void ov_AllocateMemoryPages(Cpu *c) { vtrace("allocPages", c->x[0], c->x[1], c); c->x[0] = vamm_alloc_pages(c->x[0], c->x[1]); }
void ov_FreeMemoryPages(Cpu *c) { vtrace("freePages", c->x[0], c->x[1], c); c->x[0] = vamm_free_pages(c->x[0], c->x[1]); }
void ov_FreeAddressRegion(Cpu *c) { vtrace("freeRegion", c->x[0], 0, c); c->x[0] = vamm_free_region(c->x[0]); }
void ov_AllocateMemory(Cpu *c) { const uint64_t out = c->x[0], sz = c->x[1]; const uint32_t r = vamm_alloc_memory(out, sz); if (!r) vtrace("allocMemory", gld<uint64_t>(out), sz, c); c->x[0] = r; }
void ov_InitializeVam(Cpu *) {}
void ov_IsVamEnabled(Cpu *c) { c->x[0] = 1; }
void ov_VamUsage(Cpu *c) {   // VirtualAddressMemoryResourceUsage {assigned, used} in x0:x1
    std::lock_guard<std::mutex> lk(g_mm);
    c->x[0] = g_reserved_total;
    c->x[1] = g_committed_total;
}
// VammManager members: x0 = this (unused on the host)
template <GuestFn F> void member(Cpu *c) {
    for (int i = 0; i < 7; i++) c->x[i] = c->x[i + 1];
    F(c);
}

// ---- memory heap: one reservation, committed up to the requested size (svcSetHeapSize semantics)
uint64_t g_heap_base = 0, g_heap_size = 0;
std::map<uint64_t, uint64_t> g_heap_blocks;   // allocated blocks: base -> size

uint32_t heap_set_size(uint64_t size) {
    if (size % HEAP_UNIT || size > HEAP_RESERVE) return R_INVALID_RANGE;
    std::lock_guard<std::mutex> lk(g_mm);
    if (!g_heap_base) {
        void *p = host::mem_reserve(HEAP_RESERVE, PAGE, ww_flag(), nullptr);
        if (!p) return R_OUT_OF_ADDRESS_SPACE;
        g_heap_base = (uint64_t)(uintptr_t)p;
    }
    if (size > g_heap_size) {
        if (!host::mem_commit((void *)(uintptr_t)(g_heap_base + g_heap_size), size - g_heap_size, nullptr))
            return R_OUT_OF_MEMORY;
    } else if (size < g_heap_size) {
        if (!g_heap_blocks.empty()) {
            auto last = std::prev(g_heap_blocks.end());
            if (last->first + last->second > g_heap_base + size) return R_INVALID_STATE;   // blocks still in use
        }
        host::mem_decommit((void *)(uintptr_t)(g_heap_base + size), g_heap_size - size);
    }
    g_heap_size = size;
    return R_SUCCESS;
}
uint32_t heap_alloc_block(uint64_t out, uint64_t size) {   // first fit, page aligned
    if (!size) return R_INVALID_RANGE;
    size = page_up(size);
    std::lock_guard<std::mutex> lk(g_mm);
    uint64_t at = g_heap_base;
    for (auto &b : g_heap_blocks) {
        if (b.first - at >= size) break;
        at = b.first + b.second;
    }
    if (!g_heap_base || at + size > g_heap_base + g_heap_size) {
        fprintf(stderr, "[os-mem] AllocateMemoryBlock(%#llx) failed: heap %#llx bytes, %zu blocks\n", (unsigned long long)size, (unsigned long long)g_heap_size, g_heap_blocks.size());
        return R_OUT_OF_MEMORY;
    }
    g_heap_blocks[at] = size;
    memset((void *)(uintptr_t)at, 0, size);
    gst<uint64_t>(out, at);
    return R_SUCCESS;
}
void ov_SetMemoryHeapSize(Cpu *c) { c->x[0] = heap_set_size(c->x[0]); }
void ov_GetMemoryHeapAddress(Cpu *c) { std::lock_guard<std::mutex> lk(g_mm); c->x[0] = g_heap_base; }
void ov_GetMemoryHeapSize(Cpu *c) { std::lock_guard<std::mutex> lk(g_mm); c->x[0] = g_heap_size; }
void ov_AllocateMemoryBlock(Cpu *c) { c->x[0] = heap_alloc_block(c->x[0], c->x[1]); }
void ov_FreeMemoryBlock(Cpu *c) {
    std::lock_guard<std::mutex> lk(g_mm);
    auto it = g_heap_blocks.find(c->x[0]);
    if (it == g_heap_blocks.end()) fatal("FreeMemoryBlock(%#llx): not a block", (unsigned long long)c->x[0]);
    g_heap_blocks.erase(it);
}

// ---- svcGetInfo memory-region values (see runtime/os/svc_getinfo.md)
constexpr uint64_t ALIAS_RESERVE = 2ull << 30;        // alias region: a real host reservation, VAMM does not allocate from it
constexpr uint64_t STACK_RESERVE = 1ull << 30;        // stack region: same
constexpr uint64_t TOTAL_NON_SYSTEM = 4ull << 30;     // application memory budget
constexpr uint64_t SYSTEM_RESOURCE = 1ull << 30;      // system resource size; nonzero = VAMM enabled (sdk+0x346dbc)
uint64_t g_alias_base = 0, g_stack_base = 0;

uint64_t reserve_once(uint64_t &slot, uint64_t size) {   // under g_mm
    if (!slot) {
        void *p = host::mem_reserve(size, PAGE, false, nullptr);
        if (p) slot = (uint64_t)(uintptr_t)p;
    }
    return slot;
}

}  // namespace

// SNAP_SOAK_LOG (runtime/nvn/nvn_soak.cpp): guest VAMM and memory-heap totals. out = {vamm reserved, vamm committed, region count,
// heap size, heap block count, heap bytes in blocks}.
extern "C" void snap_os_mem_stats(uint64_t *out) {
    std::lock_guard<std::mutex> lk(g_mm);
    out[0] = g_reserved_total; out[1] = g_committed_total; out[2] = g_regions.size();
    out[3] = g_heap_size; out[4] = g_heap_blocks.size();
    uint64_t b = 0;
    for (auto &kv : g_heap_blocks) b += kv.second;
    out[5] = b;
}

// 1 when the guest reservations carry write tracking, 0 when the NVN layer has to copy whole ranges: SNAP_SYNC_FULL=1, SNAP_WRITE_WATCH=0, or a
// host that cannot track writes (Linux for now). runtime/nvn/nvn_vk.cpp (SyncPoolRange) decides from the two environment variables alone today;
// the Linux NVN stage has to ask this function as well, or it would call GetWriteWatch's replacement on memory that is not tracked.
extern "C" int snap_os_write_watch_active() { return ww_flag() ? 1 : 0; }

namespace osk {
uint32_t mem_get_info(uint32_t type, uint64_t *out) {
    uint64_t lo = 0, hi = 0;
    host::mem_app_range(&lo, &hi);
    hi += 1;
    std::lock_guard<std::mutex> lk(g_mm);
    uint64_t used = g_heap_size + g_committed_total, sysused = std::min(g_committed_total, SYSTEM_RESOURCE);
    uint64_t v = 0;
    switch (type) {
    case 2: v = reserve_once(g_alias_base, ALIAS_RESERVE); if (!v) return R_OUT_OF_ADDRESS_SPACE; break;
    case 3: v = ALIAS_RESERVE; break;
    case 4:
        if (!g_heap_base) {
            void *p = host::mem_reserve(HEAP_RESERVE, PAGE, ww_flag(), nullptr);
            if (!p) return R_OUT_OF_ADDRESS_SPACE;
            g_heap_base = (uint64_t)(uintptr_t)p;
        }
        v = g_heap_base;
        break;
    case 5: v = HEAP_RESERVE; break;
    case 6: v = TOTAL_NON_SYSTEM + SYSTEM_RESOURCE; break;
    case 7: v = used + sysused; break;
    case 12: v = lo; break;
    case 13: v = hi - lo; break;
    case 14: v = reserve_once(g_stack_base, STACK_RESERVE); if (!v) return R_OUT_OF_ADDRESS_SPACE; break;
    case 15: v = STACK_RESERVE; break;
    case 16: v = SYSTEM_RESOURCE; break;
    case 17: v = sysused; break;
    case 21: v = TOTAL_NON_SYSTEM; break;
    case 22: v = used; break;
    default: return R_INVALID_ENUM_VALUE;
    }
    *out = v;
    return R_SUCCESS;
}
}  // namespace osk

namespace osk {
extern const OsOverride g_memory_overrides[];
extern const size_t g_memory_override_count;
const OsOverride g_memory_overrides[] = {
    {"_ZN2nn2os21AllocateAddressRegionEPmm", ov_AllocateAddressRegion},
    {"_ZN2nn2os19AllocateMemoryPagesEmm", ov_AllocateMemoryPages},
    {"_ZN2nn2os15FreeMemoryPagesEmm", ov_FreeMemoryPages},
    {"_ZN2nn2os17FreeAddressRegionEm", ov_FreeAddressRegion},
    {"_ZN2nn2os14AllocateMemoryEPmm", ov_AllocateMemory},
    {"_ZN2nn2os30InitializeVirtualAddressMemoryEv", ov_InitializeVam},
    {"_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", ov_IsVamEnabled},
    {"_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv", ov_VamUsage},
    {"_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm", member<ov_AllocateAddressRegion>},
    {"_ZN2nn2os6detail11VammManager19AllocateMemoryPagesEmm", member<ov_AllocateMemoryPages>},
    {"_ZN2nn2os6detail11VammManager15FreeMemoryPagesEmm", member<ov_FreeMemoryPages>},
    {"_ZN2nn2os6detail11VammManager17FreeAddressRegionEm", member<ov_FreeAddressRegion>},
    {"_ZN2nn2os6detail11VammManager14AllocateMemoryEPmm", member<ov_AllocateMemory>},
    {"_ZN2nn2os6detail11VammManager19InitializeIfEnabledEv", ov_InitializeVam},
    {"_ZN2nn2os6detail11VammManager36GetVirtualAddressMemoryResourceUsageEv", member<ov_VamUsage>},
    {"_ZN2nn2os17SetMemoryHeapSizeEm", ov_SetMemoryHeapSize},
    {"_ZN2nn2os20GetMemoryHeapAddressEv", ov_GetMemoryHeapAddress},
    {"_ZN2nn2os17GetMemoryHeapSizeEv", ov_GetMemoryHeapSize},
    {"_ZN2nn2os19AllocateMemoryBlockEPmm", ov_AllocateMemoryBlock},
    {"_ZN2nn2os15FreeMemoryBlockEmm", ov_FreeMemoryBlock},
};
const size_t g_memory_override_count = sizeof g_memory_overrides / sizeof g_memory_overrides[0];

void state_mem_save(MemBook &m) {   // save states
    std::lock_guard<std::mutex> lk(g_mm);
    for (auto &kv : g_regions) m.regions[kv.first] = {kv.second.size, kv.second.committed};
    m.reserved = g_reserved_total;
    m.committed = g_committed_total;
    m.heap_base = g_heap_base;
    m.heap_size = g_heap_size;
    m.blocks = g_heap_blocks;
}
void state_mem_arm() {   // a state was just saved: regions released from now on stay reserved; those kept for an older state are let go
    std::lock_guard<std::mutex> lk(g_mm);
    for (auto &kv : g_limbo) host::mem_release((void *)(uintptr_t)kv.first, kv.second);
    g_limbo.clear();
    g_limbo_bytes = 0;
    g_limbo_on = true;
}
std::string state_mem_check(const MemBook &m) {
    std::lock_guard<std::mutex> lk(g_mm);
    if (g_heap_base != m.heap_base || g_heap_size != m.heap_size) return "the memory heap size changed";
    for (auto &kv : m.regions) {
        auto it = g_regions.find(kv.first);
        if (it != g_regions.end()) {
            if (it->second.size != kv.second.first) return "a VAMM region changed size";
            continue;
        }
        auto lb = g_limbo.find(kv.first);
        if (lb != g_limbo.end() && lb->second == kv.second.first) continue;   // still reserved: the load takes it back as it is
        for (uint64_t a = kv.first; a < kv.first + kv.second.first;) {   // released since the save: its addresses must be free, or held by a region made since (the load releases that first)
            auto nw = g_regions.upper_bound(a);
            if (nw != g_regions.begin()) {
                --nw;
                if (nw->first <= a && a < nw->first + nw->second.size && !m.regions.count(nw->first)) {
                    a = nw->first + nw->second.size;
                    continue;
                }
            }
            uint64_t free_end = 0;
            char detail[160];
            if (!host::mem_find_free((void *)(uintptr_t)a, &free_end, detail, sizeof detail)) {
                char b[400];
                snprintf(b, sizeof b, "the address of a released VAMM region is in use (region %llx+%llx; at %llx: %s; kept %zu regions %llu MiB, %s)", (unsigned long long)kv.first,
                         (unsigned long long)kv.second.first, (unsigned long long)a, detail, g_limbo.size(), (unsigned long long)(g_limbo_bytes >> 20), g_limbo_on ? "armed" : "not armed");
                return b;
            }
            a = free_end;
        }
    }
    return "";
}
std::string state_mem_restore(const MemBook &m) {
    std::lock_guard<std::mutex> lk(g_mm);
    for (auto &kv : g_regions)   // made since the save: the restored world does not know them
        if (!m.regions.count(kv.first)) host::mem_release((void *)(uintptr_t)kv.first, kv.second.size);
    for (auto &kv : m.regions) {
        if (g_regions.count(kv.first)) continue;
        auto lb = g_limbo.find(kv.first);
        if (lb != g_limbo.end() && lb->second == kv.second.first) {   // kept reserved since the game freed it
            g_limbo_bytes -= lb->second;
            g_limbo.erase(lb);
            continue;
        }
        if (!host::mem_reserve_at((void *)(uintptr_t)kv.first, kv.second.first, ww_flag()))
            return "cannot re-reserve a released VAMM region";
    }
    for (auto &kv : g_limbo) host::mem_release((void *)(uintptr_t)kv.first, kv.second);   // freed since the save and never in it
    g_limbo.clear();
    g_limbo_bytes = 0;
    g_regions.clear();
    for (auto &kv : m.regions) g_regions[kv.first] = Region{kv.second.first, kv.second.second};
    g_reserved_total = m.reserved;
    g_committed_total = m.committed;
    g_heap_blocks = m.blocks;
    return "";
}
}  // namespace osk
