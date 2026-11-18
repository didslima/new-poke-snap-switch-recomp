// Two-module loader and runtime boundary (project code, ours). See host/loader.h and docs/runtime-boundary.md.
// The ELF mapping and relocation code follows the host spike (host/spike.cpp), extended to two modules.
#include "loader.h"
#include "elf_check.h"
#include "hle/guest_tls.h"
#include "os/guest_tls_block.h"

#include "os/os_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const uint64_t main_fn_count;
extern const char *const main_import_names[];
extern const uint64_t main_import_stub_addrs[];
extern const uint64_t main_import_count;
extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const uint64_t sdk_fn_count;
}

namespace loader {
GuestModule g_main, g_sdk;
RelocStats g_rstats[2];
std::vector<InitRecord> g_init_log;
std::vector<std::string> g_unbound;
SvcHandler g_svc_handler = nullptr;
bool g_probe = false;
std::vector<BlockerEvent> g_blockers;
uint64_t g_probe_limit = 0;

std::vector<std::string> g_weak_used;   // weak overrides that bound (no definition anywhere)
static std::vector<Thunk *> g_thunks;
static std::unordered_map<std::string, Thunk *> g_by_name;
static std::vector<uint64_t> g_main_import_target;
static std::vector<GuestFn> g_main_import_fn;   // lifted target of each import (null: thunk / host override); dtab never changes after load
extern "C" volatile int g_fnprof;
extern "C" void snap_fn_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_skips_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_neo_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_highfps_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void loader_init_hit(uint64_t a);
static thread_local GuestThread *g_cur = nullptr;
thread_local Cpu *loader::g_tls_cpu = nullptr;   // the Cpu of the last host call on this thread (crash reports)

void (*g_fatal_hook)(const char *) = nullptr;
extern "C" void snap_crashlog_fatal(const char *kind, const char *detail);   // host/mods/crashlog.cpp: _crash.txt
static void die(const char *msg) {
    fprintf(stderr, "loader: %s\n", msg);
    fflush(stderr);
    snap_crashlog_fatal("fatal load error", msg);
    if (g_fatal_hook) g_fatal_hook(msg);   // the launcher shows it in a dialog
    exit(2);
}

// Read/write, zero-filled, committed guest memory: what VirtualAlloc(at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) gave, from the host's virtual
// memory primitives. `at` null: anywhere (64 KiB aligned); otherwise exactly there or null. The size is rounded up to whole pages. Nothing stays
// reserved when the commit fails. These mappings live for the process (modules, tables, thread stacks), as before.
static void *guest_vm(void *at, size_t size) {
    size = (size + 0xfff) & ~(size_t)0xfff;
    void *p = at ? osk::host::mem_reserve_at(at, size, false) : osk::host::mem_reserve(size, 0, false, nullptr);
    if (!p) return nullptr;
    if (!osk::host::mem_commit(p, size, nullptr)) {
        osk::host::mem_release(p, size);
        return nullptr;
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ boot trace

FILE *g_trace_file = nullptr;
bool g_trace_imports = false;
static const bool g_count_imports = std::getenv("SNAP_IMPORT_COUNT") != nullptr;   // debug: per-import call counts, dumped by the route token icount=<tag>
uint64_t g_svc_counts[0x80];
uint64_t g_exc_count = 0;
void (*g_spin_gate_fn)(Cpu *, uint64_t) = nullptr;
uint64_t (*g_ticks_fn)() = nullptr;
static int64_t g_t0;
double trace_now() {
    static const int64_t f = [] { g_t0 = osk::host::counter(); return osk::host::counter_frequency(); }();
    return (double)(osk::host::counter() - g_t0) / (double)f;
}
double g_last_trace_time = 0;
void trace(const char *fmt, ...) {
    static std::mutex mu;
    char buf[1536];
    int n = snprintf(buf, sizeof buf, "[%9.4f] ", trace_now());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    buf[len++] = '\n';
    std::lock_guard<std::mutex> lk(mu);
    g_last_trace_time = trace_now();
    fwrite(buf, 1, len, stderr);
    if (g_trace_file) { fwrite(buf, 1, len, g_trace_file); fflush(g_trace_file); }
    fflush(stderr);
}

// ------------------------------------------------------------------------------------------------ thunks

static Thunk *new_thunk(const std::string &name) {
    if ((g_thunks.size() + 2) * 16 >= THUNK_SPAN) die("out of thunk slots");
    auto *t = new Thunk();
    t->name = name;
    t->addr = THUNK_BASE + 16 * g_thunks.size();
    g_thunks.push_back(t);
    g_by_name[name] = t;
    return t;
}

Thunk *add_override(const std::string &sym, GuestFn fn) {
    Thunk *t = new_thunk("ovr:" + sym);
    t->fn1 = fn;
    g_by_name["sym:" + sym] = t;
    return t;
}
Thunk *add_weak_override(const std::string &sym, GuestFn fn) {
    Thunk *t = new_thunk("weak:" + sym);
    t->fn1 = fn;
    g_by_name["wsym:" + sym] = t;
    return t;
}
Thunk *add_stub(const std::string &name, void (*fn)(Cpu *, Thunk *)) {
    Thunk *t = new_thunk(name);
    t->fn2 = fn;
    return t;
}
Thunk *find_override(const std::string &sym) {
    auto it = g_by_name.find("sym:" + sym);
    return it == g_by_name.end() ? nullptr : it->second;
}
uint64_t thunk_addr(const std::string &name) {
    auto it = g_by_name.find(name);
    return it == g_by_name.end() ? 0 : it->second->addr;
}

// ------------------------------------------------------------------------------------------------ symbolization

static const GuestModule *module_of(uint64_t a) {
    if (a - g_main.base < g_main.span) return &g_main;
    if (a - g_sdk.base < g_sdk.span) return &g_sdk;
    return nullptr;
}

std::string symbolize(uint64_t a) {
    char buf[512];
    if (a - THUNK_BASE < THUNK_SPAN) {
        uint64_t i = (a - THUNK_BASE) >> 4;
        if (i < g_thunks.size()) return "host:" + g_thunks[i]->name;
        return a == HOST_SENTINEL ? "host:sentinel" : "host:?";
    }
    const GuestModule *m = module_of(a);
    if (!m) {
        snprintf(buf, sizeof buf, "?%#llx", (unsigned long long)a);
        return buf;
    }
    uint64_t off = a - m->base;
    auto it = std::upper_bound(m->by_addr.begin(), m->by_addr.end(), std::make_pair(off, (size_t)~0ull));
    if (it != m->by_addr.begin()) {
        --it;
        const SymInfo &s = m->syms[it->second];
        uint64_t d = off - s.value;
        bool inside = s.size ? d < s.size : d < 0x40;
        snprintf(buf, sizeof buf, "%s+%#llx %s%s+%#llx", m->name, (unsigned long long)off, inside ? "" : "~", s.name.c_str(),
                 (unsigned long long)d);
        return buf;
    }
    snprintf(buf, sizeof buf, "%s+%#llx", m->name, (unsigned long long)off);
    return buf;
}

// ------------------------------------------------------------------------------------------------ ELF

#pragma pack(push, 1)
struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
struct Rela { uint64_t off; uint64_t info; int64_t addend; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
#pragma pack(pop)

static std::vector<uint8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { std::string m = std::string("cannot open ") + path; die(m.c_str()); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) die("cannot read a game file");
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v((size_t)n);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) die("short read");
    fclose(f);
    return v;
}

static void map_module(GuestModule &m, const char *name, const char *path, uint64_t base) {
    m.name = name;
    m.path = path;
    m.base = base;
    std::vector<uint8_t> raw = read_file(path);
    {   // an empty, cut-off or wrong file ends here with a plain message, never in a crash below
        std::string why;
        if (!elf_check::check_bytes(raw.data(), raw.size(), why)) {
            std::string m = std::string("The ") + name + " file " + why + ". Run the builder again to convert the game.";
            die(m.c_str());
        }
    }
    auto *eh = (const Ehdr *)raw.data();
    const Phdr *ph = (const Phdr *)(raw.data() + eh->phoff);
    uint64_t hi = 0, dynva = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == 1) hi = std::max<uint64_t>(hi, ph[i].vaddr + ph[i].memsz);
        if (ph[i].type == 2) dynva = ph[i].vaddr;
        if (ph[i].type == 0x6474e550) { m.eh_hdr = ph[i].vaddr; m.eh_hdr_size = ph[i].memsz; }
        if (ph[i].type == 1 && (ph[i].flags & 1)) { m.text_off = ph[i].vaddr; m.text_size = ph[i].filesz; }
    }
    m.span = (hi + 0xFFFF) & ~0xFFFFull;
    void *p = guest_vm((void *)base, m.span);
    if (p != (void *)base) die("cannot map a module at its guest address");
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) memcpy((uint8_t *)base + ph[i].vaddr, raw.data() + ph[i].off, ph[i].filesz);
    std::unordered_map<int64_t, uint64_t> dyn;
    for (const Dyn *d = (const Dyn *)(base + dynva); d->tag != 0; d++) dyn.emplace(d->tag, d->val);
    const Sym *syms = (const Sym *)(base + dyn[6]);
    const char *strs = (const char *)(base + dyn[5]);
    uint32_t nsyms = dyn.count(4) ? ((const uint32_t *)(base + dyn[4]))[1] : 0;   // DT_HASH nchain
    m.syms.reserve(nsyms);
    for (uint32_t i = 0; i < nsyms; i++) {
        if ((uint64_t)((const uint8_t *)&syms[i] - (const uint8_t *)base) + sizeof(Sym) > m.span || dyn[5] + syms[i].name >= m.span) die("a game file has a damaged symbol table. Run the builder again to convert the game.");
        SymInfo s{strs + syms[i].name, syms[i].value, syms[i].size, syms[i].info, syms[i].shndx};
        if (s.shndx && !s.name.empty()) {
            m.defs.emplace(s.name, i);
            if (s.value) m.by_addr.emplace_back(s.value, i);
        }
        m.syms.push_back(std::move(s));
    }
    std::sort(m.by_addr.begin(), m.by_addr.end());
    m.init_fn = dyn.count(12) ? dyn[12] : 0;
    m.init_array = dyn.count(25) ? dyn[25] : 0;
    m.init_count = dyn.count(27) ? dyn[27] / 8 : 0;
    m.rela = dyn[7]; m.relasz = dyn[8];
    m.jmprel = dyn.count(23) ? dyn[23] : 0;
    m.pltrelsz = dyn.count(2) ? dyn[2] : 0;
}

// Direct-indexed dispatch table: one slot per text word (main 22 MB, sdk 12 MB of host memory).
static void build_dtab(GuestModule &m, const uint64_t *addrs, const GuestFn *ptrs, uint64_t n) {
    m.dtab_count = m.text_size >> 2;
    m.dtab = (GuestFn *)guest_vm(nullptr, m.dtab_count * sizeof(GuestFn));
    if (!m.dtab) die("cannot allocate a dispatch table");
    m.fn_count = n;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = addrs[i] - m.base - m.text_off;
        if ((off >> 2) < m.dtab_count) m.dtab[off >> 2] = ptrs[i];
    }
}

// ------------------------------------------------------------------------------------------------ symbol resolution

static void missing_stub(Cpu *c, Thunk *t);

static uint64_t special_symbol(const std::string &n) {
    // rtld's own data, read by the sdk's runtime support: the relocation tables of the main module
    if (n == "__rel_dyn_start") return g_main.base + g_main.rela;
    if (n == "__rel_dyn_end") return g_main.base + g_main.rela + g_main.relasz;
    if (n == "__rel_plt_start") return g_main.base + g_main.jmprel;
    if (n == "__rel_plt_end") return g_main.base + g_main.jmprel + g_main.pltrelsz;
    return 0;
}

// Precedence (rtld load order, main first): host override, then special rtld symbols, then main's definition, then sdk's. `weak`: an unresolved weak reference is 0. Missing strong references get an aborting stub.
static uint64_t resolve(const std::string &name, bool weak, int who) {
    RelocStats &rs = g_rstats[who];
    if (Thunk *t = find_override(name)) { rs.res_override++; return t->addr; }
    if (uint64_t v = special_symbol(name)) { rs.res_special++; return v; }
    auto it = g_main.defs.find(name);
    if (it != g_main.defs.end()) { rs.res_main++; return g_main.base + g_main.syms[it->second].value; }
    it = g_sdk.defs.find(name);
    if (it != g_sdk.defs.end()) { rs.res_sdk++; return g_sdk.base + g_sdk.syms[it->second].value; }
    auto wt = g_by_name.find("wsym:" + name);
    if (wt != g_by_name.end()) { rs.res_override++; g_weak_used.push_back(name); return wt->second->addr; }
    if (weak) { rs.res_weak0++; g_unbound.push_back(std::string(who ? "sdk" : "main") + " weak->0: " + name); return 0; }
    rs.res_missing++;
    g_unbound.push_back(std::string(who ? "sdk" : "main") + " missing: " + name);
    auto bt = g_by_name.find("missing:" + name);
    Thunk *t = bt != g_by_name.end() ? bt->second : add_stub("missing:" + name, missing_stub);
    return t->addr;
}

uint64_t lookup(const std::string &name) {
    auto it = g_main.defs.find(name);
    if (it != g_main.defs.end()) return g_main.base + g_main.syms[it->second].value;
    it = g_sdk.defs.find(name);
    if (it != g_sdk.defs.end()) return g_sdk.base + g_sdk.syms[it->second].value;
    return 0;
}

std::vector<std::string> shared_symbols() {
    std::vector<std::string> v;
    for (auto &kv : g_main.defs) if (g_sdk.defs.count(kv.first)) v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

static void relocate(GuestModule &m, int who) {
    RelocStats &rs = g_rstats[who];
    const std::pair<uint64_t, uint64_t> tabs[2] = {{m.rela, m.relasz}, {m.jmprel, m.pltrelsz}};
    for (auto &t : tabs) {
        if (!t.first) continue;
        const Rela *r = (const Rela *)(m.base + t.first);
        for (uint64_t i = 0; i < t.second / sizeof(Rela); i++) {
            uint32_t type = (uint32_t)(r[i].info & 0xFFFFFFFF), sym = (uint32_t)(r[i].info >> 32);
            if (r[i].off + 8 > m.span || (sym && sym >= m.syms.size())) die("a game file has a damaged relocation table. Run the builder again to convert the game.");
            uint64_t *where = (uint64_t *)(m.base + r[i].off);
            if (type == 1027) { *where = m.base + r[i].addend; rs.relative++; continue; }
            if (type != 257 && type != 1025 && type != 1026) { rs.other++; continue; }
            if (!sym) { *where = m.base + r[i].addend; rs.other++; continue; }
            const SymInfo &s = m.syms[sym];
            uint64_t v = resolve(s.name, (s.info >> 4) == 2, who);
            if (s.shndx) rs.own_defined++;
            if (type == 257) { v += r[i].addend; rs.abs64++; }
            else if (type == 1025) rs.glob_dat++;
            else rs.jump_slot++;
            *where = v;
        }
    }
}

bool load(const char *main_elf, const char *sdk_elf) {
    // the thunk range (readable so that guest code can compare against it)
    if (!guest_vm((void *)THUNK_BASE, THUNK_SPAN)) die("cannot map the thunk range");
    map_module(g_main, "main", main_elf, GUEST_MAIN_BASE);
    map_module(g_sdk, "sdk", sdk_elf, GUEST_SDK_BASE);
    build_dtab(g_main, main_fn_addrs, main_fn_ptrs, main_fn_count);
    build_dtab(g_sdk, sdk_fn_addrs, sdk_fn_ptrs, sdk_fn_count);
    // the order of the two passes does not matter to the result, all symbols are known
    relocate(g_sdk, 1);
    relocate(g_main, 0);
    // main's PLT stubs call the loader-resolved target of their import (generated imp_<name> -> rt_import_call)
    g_main_import_target.resize(main_import_count);
    RelocStats saved = g_rstats[0];
    for (uint64_t i = 0; i < main_import_count; i++) {
        const char *nm = main_import_names[i];
        bool weak = false;
        for (const SymInfo &sy : g_main.syms) if (!sy.shndx && sy.name == nm) { weak = (sy.info >> 4) == 2; break; }
        size_t before = g_unbound.size();
        g_main_import_target[i] = resolve(nm, weak, 0);
        g_unbound.resize(before);   // already reported by the relocation pass
    }
    // opt 2026-11-13: resolve lifted import targets once, so a PLT call into sdk skips rt_dispatch's range checks and dtab load
    g_main_import_fn.resize(main_import_count);
    for (uint64_t i = 0; i < main_import_count; i++) g_main_import_fn[i] = lookup_fn(g_main_import_target[i]);
    g_rstats[0] = saved;
    return true;
}

// ------------------------------------------------------------------------------------------------ dispatch

GuestFn lookup_fn(uint64_t pc) {
    uint64_t o = pc - g_main.base - g_main.text_off;
    if (o < g_main.text_size) return g_main.dtab[o >> 2];
    o = pc - g_sdk.base - g_sdk.text_off;
    if (o < g_sdk.text_size) return g_sdk.dtab[o >> 2];
    return nullptr;
}

// Mod hooks (runtime/hle/modhooks.h): the dispatch-table cell of the lifted function at pc, or null. Writing it redirects every call that goes
// through rt_dispatch (indirect calls, vtables, function pointers); a direct lifted call (`f_<addr>(c)`) does not read the table.
GuestFn *dtab_cell(uint64_t pc) {
    GuestModule *m = pc - g_main.base - g_main.text_off < g_main.text_size ? &g_main : pc - g_sdk.base - g_sdk.text_off < g_sdk.text_size ? &g_sdk : nullptr;
    if (!m || (pc & 3)) return nullptr;
    GuestFn *p = &m->dtab[(pc - m->base - m->text_off) >> 2];
    return *p ? p : nullptr;
}

// After a dtab_cell write: main's PLT stubs cache the lifted target of each import (g_main_import_fn), so refresh them.
void refresh_import_fns() {
    for (size_t i = 0; i < g_main_import_fn.size(); i++) g_main_import_fn[i] = lookup_fn(g_main_import_target[i]);
}

GuestFn lookup_fn_bsearch(uint64_t pc) {
    const uint64_t *a; const GuestFn *p; uint64_t n;
    if (pc - g_main.base < g_main.span) { a = main_fn_addrs; p = main_fn_ptrs; n = main_fn_count; }
    else { a = sdk_fn_addrs; p = sdk_fn_ptrs; n = sdk_fn_count; }
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (a[mid] < pc) lo = mid + 1; else hi = mid;
    }
    return lo < n && a[lo] == pc ? p[lo] : nullptr;
}

// ------------------------------------------------------------------------------------------------ guest threads and calls

// main's TLS image is read once, at the first thread (before any guest code runs, so it is the loaded bytes), and copied
// into every guest thread's TLS block after that (runtime/hle/guest_tls.h says why it matters: the scan finds Pokemon).
// The capture is the shared module's (runtime/os/guest_tls_block.h): once, thread safe, immutable.
static const hle::GuestTlsImage *main_tls_image() {
    const hle::GuestTlsImage *img = osk::guest_tls_main_image((const uint8_t *)g_main.base, g_main.span);
    static std::atomic<bool> checked{false};
    if (img && !checked.exchange(true) && *(const int32_t *)img->data() != -1)
        trace("guest TLS: main image at +%#llx is not the 2.0.1 one (slot word %08x)", (unsigned long long)hle::kMainTlsImageVaddr, *(const uint32_t *)img->data());
    return img;
}
// Fill a fresh (zeroed) TLS block made by the caller (host/boot.cpp's tp_init_hook): the image only, nothing else is written.
void init_guest_tls(uint64_t tp) {
    if (const hle::GuestTlsImage *img = main_tls_image()) hle::guest_tls_init((uint8_t *)tp, img->data(), img->size());
}

GuestThread *new_thread(size_t stack_size) {
    auto *t = new GuestThread();
    t->c = (Cpu *)osk::host::heap_aligned_alloc(sizeof(Cpu), 64);
    memset(t->c, 0, sizeof(Cpu));
    uint8_t *stack = (uint8_t *)guest_vm(nullptr, stack_size);
    uint8_t *tlr = (uint8_t *)guest_vm(nullptr, 0x1000);    // Thread Local Region
    uint8_t *tt = (uint8_t *)guest_vm(nullptr, 0x1000);     // ThreadType
    // no PT_TLS in the ELFs: the TLS block is the shared module's (reserve + commit, zeroed, main's .tdata image at tp + 0x10), owned by this ThreadType
    const uint64_t tls_tp = tt ? osk::guest_tls_block_create(main_tls_image(), (uint64_t)(uintptr_t)tt) : 0;
    if (!stack || !tls_tp || !tlr || !tt) die("cannot allocate a guest thread");
    t->stack_lo = (uint64_t)stack;
    t->stack_hi = (uint64_t)stack + stack_size;
    // runtime/os/os_layout.h: TLR+0x1f0 = ELF thread pointer, TLR+0x1f8 = ThreadType*; ThreadType state byte at +0x40
    *(uint64_t *)(tlr + 0x1f0) = tls_tp;
    *(uint64_t *)(tlr + 0x1f8) = (uint64_t)tt;
    tt[0x40] = 3;                                   // TS_STARTED
    *(uint64_t *)(tt + 0x50) = (uint64_t)stack;     // stack, stack size
    *(uint64_t *)(tt + 0x58) = stack_size;
    t->c->tpidr = tls_tp;
    t->c->tpidrro = (uint64_t)tlr;
    t->c->sp = (t->stack_hi - 64) & ~15ull;
    t->c->excl_addr = ~0ull;
    rt_fpcr_sync(t->c);
    g_cur = t;
    return t;
}

// The guest state a nested call may change, restored after it (loader.h call_guest_nested).
struct NestedSave {
    uint64_t x[31], sp, pc, nzcv, excl_addr, excl_val, excl_val_hi, fpcr, fpsr;
    __uint128_t v[32];
    explicit NestedSave(const Cpu *c)
        : sp(c->sp), pc(c->pc), nzcv(c->nzcv), excl_addr(c->excl_addr), excl_val(c->excl_val), excl_val_hi(c->excl_val_hi),
          fpcr(c->fpcr), fpsr(c->fpsr) {
        memcpy(x, c->x, sizeof x);
        memcpy(v, c->v, sizeof v);
    }
    void restore(Cpu *c) const {
        memcpy(c->x, x, sizeof x);
        memcpy(c->v, v, sizeof v);
        c->sp = sp; c->pc = pc; c->nzcv = nzcv;
        c->excl_addr = excl_addr; c->excl_val = excl_val; c->excl_val_hi = excl_val_hi;
        const bool fp = c->fpcr != fpcr;
        c->fpcr = fpcr; c->fpsr = fpsr;
        if (fp) rt_fpcr_sync(c);
    }
};

uint64_t call_guest_nested(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args) {
    const NestedSave saved(c);
    // Inside rt_inline_hook c->pc carries HOOK_RA_TAG | the hook site's return address: the callee returns there, so the
    // guest unwinder walks on into the hooked function (its LSDA) instead of stopping at HOST_SENTINEL.
    const uint64_t ra = (saved.pc & HOOK_RA_TAG) ? saved.pc & ~HOOK_RA_TAG : 0;
    static bool trace = getenv("SNAP_TRACE") != nullptr;
    if (trace) { fprintf(stderr, "call_guest %s"+0, symbolize(fn).c_str()); fprintf(stderr, " args"); int k = 0; for (uint64_t a : args) { if (k++ < 4) fprintf(stderr, " %#llx", (unsigned long long)a); } fprintf(stderr, "%c", 10); fflush(stderr); }
    int i = 0;
    for (uint64_t a : args) c->x[i++] = a;
    c->x[30] = ra ? ra : HOST_SENTINEL;
    g_tls_cpu = c;
    c->pc = fn;
    rt_dispatch(c);
    if (c->resuming) {
        if (ra) throw GuestResume{};   // c->pc / c->sp name the resume target; rt_inline_hook returns to the generated site
        fprintf(stderr, "loader: an exception or longjmp left guest code for the host (resume target pc=%#llx sp=%#llx)\n",
                (unsigned long long)c->pc, (unsigned long long)c->sp);
        fflush(stderr);
        abort();
    }
    uint64_t r = c->x[0];
    saved.restore(c);
    return r;
}

uint64_t call_guest(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args) { return call_guest_nested(c, fn, args); }

}  // namespace loader

// Every MAIN_INLINE_HOOKS site (recomp/gen.py Emitter.function inline_hook) runs its hook through this. ra: the return
// address nested guest calls get (0: HOST_SENTINEL). A guest exception or longjmp out of a nested call unwinds the hook
// (GuestResume) and returns with c->resuming set; the site then takes its landing pad or returns, as after a call.
extern "C" void rt_inline_hook(Cpu *c, uint64_t ra, void (*fn)(Cpu *c)) {
    const uint64_t pc = c->pc;
    c->pc = ra ? ra | loader::HOOK_RA_TAG : 0;
    try {
        fn(c);
    } catch (const loader::GuestResume &) {
        return;
    }
    c->pc = pc;
}

namespace loader {

// ------------------------------------------------------------------------------------------------ initializers

// Initializer entries seen by the FN_ENTER hook while DT_INIT runs them: address -> (module, index).
static std::unordered_map<uint64_t, std::pair<const char *, int>> g_init_watch;
static std::unordered_map<uint64_t, int> g_init_seen;

// rtld order: sdk, then main. Each module's DT_INIT (`_init`) walks that module's init_array itself (checked on main: the
// generated f_7100000030 loops over the table and dispatches each entry, guarded by a flag word), so calling the array again
// afterwards would run every static initializer twice (and corrupt lists that assume one run). With log, each entry is
// reported with a timestamp the first time it is entered.
int run_initializers(Cpu *c, bool log) {
    int n = 0;
    GuestModule *mods[2] = {&g_sdk, &g_main};
    for (GuestModule *m : mods) {
        const uint64_t *arr = (const uint64_t *)(m->base + m->init_array);
        for (uint64_t i = 0; i < m->init_count; i++) {
            uint64_t fn = arr[i];
            if (fn == 0 || fn == ~0ull) continue;
            g_init_watch[fn] = {m->name, (int)i};
        }
    }
    const int fnprof_before = g_fnprof;
    if (log) g_fnprof = 1;
    for (GuestModule *m : mods) {
        if (m->init_fn) {
            g_init_log.push_back({m->name, -1, m->base + m->init_fn});
            trace("init %s DT_INIT %#llx %s (%llu init_array entries)", m->name, (unsigned long long)(m->base + m->init_fn),
                  symbolize(m->base + m->init_fn).c_str(), (unsigned long long)m->init_count);
            call_guest(c, m->base + m->init_fn);
            n++;
        } else {
            const uint64_t *arr = (const uint64_t *)(m->base + m->init_array);   // no DT_INIT: call the array
            for (uint64_t i = 0; i < m->init_count; i++) if (arr[i] && arr[i] != ~0ull) { call_guest(c, arr[i]); n++; }
        }
    }
    int seen = (int)g_init_seen.size();
    trace("init: DT_INIT of sdk and main done; %d of %zu init_array entries were entered", seen, g_init_watch.size());
    // Load pass (2026-11-13): the init log turned the FN_ENTER hook on for the whole run, so every lifted function entry paid
    // fnprof_hit (a hash lookup in g_init_watch plus three observer calls), about a fifth of guest CPU in a host profile. Turn it
    // off again unless a tracer had it on before (SNAP_FNTRACE, SNAP_TRACE_LOD, SNAP_SKIPS_REC, SNAP_NEO_CAMFN set it at start).
    // SNAP_FNPROF_KEEP=1 restores the old always-on hook.
    if (log && !fnprof_before && !getenv("SNAP_FNPROF_KEEP")) {
        // Only switch the hook off; leave g_init_watch in place so a guest thread still inside
        // loader_init_hit never sees the unordered_map being cleared under it.
        g_fnprof = 0;
    }
    return n + seen;
}

}  // namespace loader

// ------------------------------------------------------------------------------------------------ extern "C" runtime hooks

using namespace loader;

static void record_blocker(const char *kind, const std::string &what, Cpu *c, uint64_t pc) {
    BlockerEvent e;
    e.kind = kind;
    e.what = what;
    e.caller = symbolize(c->x[30]);
    for (int i = 0; i < 4; i++) e.args[i] = c->x[i];
    e.chain = guest_chain(c, pc, 14);
    e.api = blocker_api(c, pc);
    g_blockers.push_back(e);
    fprintf(stderr, "BLOCKER #%zu %s %s\n  args x0..x3 = %#llx %#llx %#llx %#llx\n", g_blockers.size(), kind, what.c_str(),
            (unsigned long long)e.args[0], (unsigned long long)e.args[1], (unsigned long long)e.args[2], (unsigned long long)e.args[3]);
    for (size_t i = 0; i < e.chain.size(); i++) fprintf(stderr, "    #%zu %s\n", i, e.chain[i].c_str());
    fprintf(stderr, "NEXT-BLOCKER %s %s\n", kind, e.api.c_str());
    fflush(stderr);
    if (g_probe_limit && g_blockers.size() >= g_probe_limit) {
        fprintf(stderr, "loader: probe limit of %llu events reached\n", (unsigned long long)g_probe_limit);
        fflush(stderr);
        exit(3);
    }
}

namespace loader {
// A frame pointer is plausible if it lies in this thread's guest stack: the thread created by new_thread, or (threads made by
// runtime/os, whose stacks come from the guest) within 64 MB above the current sp.
static bool fp_ok(Cpu *c, uint64_t fp) {
    if (fp & 7) return false;
    // a thread that has not run guest code yet has sp = 0 and fp = 0: the stall watchdog's thread dump read [0+8] and crashed
    // (2026-11-12/13, 6 runs, "CRASH on guest thread type 0" right after a STALL dump)
    if (fp < 0x10000 || c->sp < 0x10000) return false;
    if (g_cur) return fp >= g_cur->stack_lo && fp + 16 <= g_cur->stack_hi;
    return fp >= c->sp && fp - c->sp < (64ull << 20);
}
std::vector<std::string> guest_chain(Cpu *c, uint64_t pc, int depth) {
    std::vector<std::string> out;
    out.push_back(symbolize(pc));
    out.push_back(symbolize(c->x[30]) + " (lr)");
    uint64_t fp = c->x[29];
    for (int i = 0; i < depth; i++) {
        if (!fp_ok(c, fp)) break;
        if (!osk::host::mem_readable((void *)fp, 16)) break;   // another thread's stack read by the stall dump can be gone (thread exited)
        uint64_t lr = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (lr == 0 || lr == HOST_SENTINEL) { out.push_back(lr ? "host:sentinel" : "(end)"); break; }
        out.push_back(symbolize(lr));
        if (next <= fp) break;
        fp = next;
    }
    return out;
}
void print_chain(Cpu *c, uint64_t pc, int depth) {
    for (auto &s : guest_chain(c, pc, depth)) fprintf(stderr, "    %s\n", s.c_str());
}

static bool exported_inside(uint64_t a, std::string &name) {
    const GuestModule *m = module_of(a);
    if (!m) return false;
    uint64_t off = a - m->base;
    auto it = std::upper_bound(m->by_addr.begin(), m->by_addr.end(), std::make_pair(off, (size_t)~0ull));
    if (it == m->by_addr.begin()) return false;
    --it;
    const SymInfo &sy = m->syms[it->second];
    if (!sy.size || off - sy.value >= sy.size) return false;
    name = sy.name;
    return true;
}

std::string blocker_api(Cpu *c, uint64_t pc) {
    std::vector<uint64_t> frames{pc, c->x[30]};
    uint64_t fp = c->x[29];
    for (int i = 0; i < 24; i++) {
        if (!fp_ok(c, fp)) break;
        uint64_t lr = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (!lr || lr == HOST_SENTINEL) break;
        frames.push_back(lr);
        if (next <= fp) break;
        fp = next;
    }
    std::string any, n;
    for (uint64_t f : frames) {
        if (!exported_inside(f, n)) continue;
        if (n.rfind("_ZN2nn", 0) == 0 || n.rfind("nn", 0) == 0 && n.size() > 2 && n[2] >= 'a' && n[2] <= 'z' && n.find('_') == std::string::npos) return n;
        if (any.empty() && n.rfind("_ZNSt3__1", 0) != 0) any = n;
    }
    return any;
}

static void probe_stub_fn(Cpu *c, Thunk *t);
void add_probe_stub(const std::string &sym) {
    Thunk *t = add_stub("probe:" + sym, probe_stub_fn);
    g_by_name["sym:" + sym] = t;
}

const char *svc_name(uint32_t imm) {
    static const char *names[0x80] = {};
    static bool init = false;
    if (!init) {
        init = true;
        struct { uint32_t n; const char *s; } t[] = {
            {1, "SetHeapSize"}, {2, "SetMemoryPermission"}, {3, "SetMemoryAttribute"}, {4, "MapMemory"}, {5, "UnmapMemory"},
            {6, "QueryMemory"}, {7, "ExitProcess"}, {8, "CreateThread"}, {9, "StartThread"}, {10, "ExitThread"},
            {11, "SleepThread"}, {12, "GetThreadPriority"}, {13, "SetThreadPriority"}, {14, "GetThreadCoreMask"},
            {15, "SetThreadCoreMask"}, {16, "GetCurrentProcessorNumber"}, {17, "SignalEvent"}, {18, "ClearEvent"},
            {19, "MapSharedMemory"}, {20, "UnmapSharedMemory"}, {21, "CreateTransferMemory"}, {22, "CloseHandle"},
            {23, "ResetSignal"}, {24, "WaitSynchronization"}, {25, "CancelSynchronization"}, {26, "ArbitrateLock"},
            {27, "ArbitrateUnlock"}, {28, "WaitProcessWideKeyAtomic"}, {29, "SignalProcessWideKey"}, {30, "GetSystemTick"},
            {31, "ConnectToNamedPort"}, {33, "SendSyncRequest"}, {34, "SendSyncRequestWithUserBuffer"},
            {35, "SendAsyncRequestWithUserBuffer"}, {37, "GetThreadId"}, {38, "Break"}, {39, "OutputDebugString"},
            {41, "GetInfo"}, {42, "FlushEntireDataCache"}, {44, "MapPhysicalMemory"}, {45, "UnmapPhysicalMemory"},
            {47, "GetLastThreadInfo"}, {50, "SetThreadActivity"}, {51, "GetThreadContext3"}, {52, "WaitForAddress"},
            {53, "SignalToAddress"}, {54, "SynchronizePreemptionState"}, {64, "CreateSession"}, {65, "AcceptSession"},
            {67, "ReplyAndReceive"}, {68, "ReplyAndReceiveWithUserBuffer"}, {69, "CreateEvent"}, {72, "MapPhysicalMemoryUnsafe?"},
            {73, "UnmapPhysicalMemoryUnsafe?"}, {75, "?"}, {80, "CreateDeviceAddressSpace"}, {81, "AttachDeviceAddressSpace"},
            {82, "DetachDeviceAddressSpace"}, {83, "MapDeviceAddressSpaceByForce"}, {112, "GetSystemInfo?"}, {114, "?"}};
        for (auto &e : t) if (e.n < 0x80) names[e.n] = e.s;
    }
    return imm < 0x80 && names[imm] ? names[imm] : "svc";
}
}  // namespace loader

extern "C" {
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t n = imm < 0x80 ? ++g_svc_counts[imm] : 1;
    if (g_trace_imports && n <= 3)
        trace("svc %#x %s #%llu at %s x0=%#llx x1=%#llx x2=%#llx", imm, svc_name(imm), (unsigned long long)n, symbolize(pc).c_str(),
              (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    if (g_svc_handler && g_svc_handler(c, imm)) return;
    char what[96];
    snprintf(what, sizeof what, "SVC %#x %s (stub at %s)", imm, svc_name(imm), symbolize(pc).c_str());
    if (g_probe) {
        record_blocker("svc", what, c, pc);
        c->x[0] = 0; c->x[1] = 0; c->x[2] = 0; c->x[3] = 0;
        return;
    }
    fprintf(stderr, "SVC backstop: %s, caller lr=%#llx\n", what, (unsigned long long)c->x[30]);
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    fprintf(stderr, "BRK %#x at %s lr=%s\n", imm, symbolize(pc).c_str(), symbolize(c->x[30]).c_str());
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_udf(Cpu *c, uint64_t pc) {
    fprintf(stderr, "UDF trap at %s lr=%s\n", symbolize(pc).c_str(), symbolize(c->x[30]).c_str());
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_unresolved(Cpu *c, uint64_t addr, const char *what) {
    // feed-back list for re-lifting: one TSV line per miss (target, its module offset, caller, what)
    if (FILE *f = fopen("analysis/recomp/indirect_misses.tsv", "ab")) {
        const GuestModule *m = addr - g_main.base < g_main.span ? &g_main : addr - g_sdk.base < g_sdk.span ? &g_sdk : nullptr;
        fprintf(f, "%#llx%s%s%s%#llx%s%s%s%s%s", (unsigned long long)addr, "\t", m ? m->name : "-", "\t", (unsigned long long)(m ? addr - m->base : 0), "\t", symbolize(c->x[30]).c_str(), "\t", what, "\n");
        fclose(f);
    }
    fprintf(stderr, "UNRESOLVED call to %#llx (%s), lr=%s\n", (unsigned long long)addr, what, symbolize(c->x[30]).c_str());
    print_chain(c, addr, 14);
    fflush(stderr);
    abort();
}
volatile int g_fnprof;
static void unused_fnprof_marker() {}
// Debug: SNAP_FNTRACE=0xaddr[,0xaddr...] logs every entry (and its caller) of those lifted functions (FN_ENTER hook).
static uint64_t g_fntrace[8];
static int g_fntrace_n;
static void fnpeek_parse();
void loader_enable_fntrace() {
    fnpeek_parse();
    const char *e = getenv("SNAP_FNTRACE");
    while (e && *e && g_fntrace_n < 8) {
        char *end;
        g_fntrace[g_fntrace_n++] = strtoull(e, &end, 0);
        e = *end == ',' ? end + 1 : nullptr;
    }
    if (g_fntrace_n) g_fnprof = 1;
}
// Debug: SNAP_FNPEEK=<fn addr>@<path>|<path>... logs guest values read from x0 at each entry of one lifted function, only when they
// change (first 4000 lines). A path is hex offsets joined by '>' (load a pointer at p+off, go on) and an optional ':n' (read n qwords
// at the last address, default 1). Example: 0x71003853f0@2bc|48>70>0>30:4 . Guest memory is identity mapped.
static uint64_t g_fnpeek_fn;
static std::vector<std::pair<std::vector<uint64_t>, int>> g_fnpeek_paths;
static std::vector<uint64_t> g_fnwatch_path;   // SNAP_FNWATCH, below
static bool g_hwwatch;                          // SNAP_HWWATCH, below
static uint64_t g_collptr;                      // SNAP_COLLARGS: the Volcarona-area collider whose box-update args get logged
static bool g_collwatch;                        // SNAP_COLLWATCH=1 (with SNAP_HWWATCH, SNAP_PAIRTRACK): watch the first tracked collider's box (+0x70)
static void fnpeek_parse() {
    const char *e = getenv("SNAP_FNPEEK");
    if (!e || !*e) return;
    char *end;
    g_fnpeek_fn = strtoull(e, &end, 0);
    if (*end != '@') { g_fnpeek_fn = 0; return; }
    e = end + 1;
    while (*e) {
        std::vector<uint64_t> offs;
        int n = 1;
        for (;;) {
            offs.push_back(strtoull(e, &end, 16));
            e = end;
            if (*e == '>') { e++; continue; }
            if (*e == ':') { n = (int)strtol(e + 1, &end, 10); e = end; }
            break;
        }
        if (n < 1) n = 1;
        if (n > 16) n = 16;
        g_fnpeek_paths.push_back({offs, n});
        if (*e != '|') break;
        e++;
    }
    if (g_fnpeek_fn) g_fnprof = 1;
    if (const char *w = getenv("SNAP_FNWATCH")) {
        while (*w) {
            g_fnwatch_path.push_back(strtoull(w, &end, 16));
            if (*end != '>') break;
            w = end + 1;
        }
    }
    g_hwwatch = getenv("SNAP_HWWATCH") != nullptr;
    g_collwatch = getenv("SNAP_COLLWATCH") != nullptr;
}
static void fnpeek_hit(uint64_t x0) {
    static std::string last;
    static int lines;
    if (lines >= 4000) return;
    std::string s;
    char buf[64];
    for (auto &pp : g_fnpeek_paths) {
        uint64_t p = x0;
        bool ok = true;
        for (size_t i = 0; i + 1 < pp.first.size() && ok; i++) {
            uint64_t q = p + pp.first[i];
            if (!p || !osk::host::mem_readable((void *)q, 8)) ok = false; else p = *(const uint64_t *)q;
        }
        uint64_t q = p + pp.first.back();
        s += " [";
        if (!ok || !p || !osk::host::mem_readable((void *)q, 8 * pp.second)) s += "bad";
        else for (int k = 0; k < pp.second; k++) { snprintf(buf, sizeof buf, k ? " %llx" : "%llx", (unsigned long long)((const uint64_t *)q)[k]); s += buf; }
        s += "]";
    }
    if (s == last) return;
    last = s;
    lines++;
    loader::trace("fnpeek x0=%#llx%s", (unsigned long long)x0, s.c_str());
}
// Debug: SNAP_FNWATCH=<path> (with SNAP_FNPEEK set): the address the path names (from the SNAP_FNPEEK function's x0, re-read at each of
// its entries) is watched, 16 bytes; at every lifted function entry the bytes are compared and a change is logged with the function
// entered and its caller (first 2000 changes). Function-entry granularity: the writer is the logged function or the one that called it.
static uint64_t g_fnwatch_addr;
static uint8_t g_fnwatch_last[16];
// SNAP_HWWATCH=1 (with SNAP_FNWATCH): x64 debug-register write breakpoints instead of the entry compare: DR0 = addr (8 bytes),
// DR1 = addr+8 (8 bytes), DR2 = addr-0x20 (4 bytes), armed on every thread of the process; each hit logs the bytes and the lifted
// function last entered on the writing thread.
static thread_local uint64_t t_lastfn, t_lastlr, t_lrring[8];
static thread_local unsigned t_lrpos;
static void hwwatch_on_hit(uint64_t dr6, uint64_t rip) {
    static std::atomic<long> hits{0}, logged{0};
    uint64_t q = g_fnwatch_addr;
    uint64_t v40 = q ? *(const uint64_t *)q : 0, v48 = q ? *(const uint64_t *)(q + 8) : 0;
    uint32_t v20 = q ? *(const uint32_t *)(q - 0x20) : 0;
    // The first 40 hits, then only unusual values (a flag set or a state other than 0/1), at most 2000 lines.
    bool odd = (v40 & 0xff) || (v48 & 0xffffffffull) || v20 > 1;
    if (q && (hits.fetch_add(1) < 40 || odd) && logged.fetch_add(1) < 2000) {
        std::string ring;
        for (unsigned i = 1; i <= 8; i++) {
            uint64_t lr = t_lrring[(t_lrpos - i) & 7];
            if (lr) { ring += " < "; ring += loader::symbolize(lr); }
        }
        loader::trace("hwwatch dr6=%llx rip=%#llx +40=%016llx +48=%016llx +20=%08x in fn %#llx callers%s", (unsigned long long)dr6,
                      (unsigned long long)rip, (unsigned long long)v40, (unsigned long long)v48, v20,
                      (unsigned long long)t_lastfn, ring.c_str());
    }
}
static void hwwatch_request(uint64_t q) { osk::host::hwwatch_request(q, hwwatch_on_hit); }
static void fnwatch_resolve(uint64_t x0) {
    if (g_fnwatch_path.empty()) return;
    uint64_t p = x0;
    for (size_t i = 0; i + 1 < g_fnwatch_path.size(); i++) {
        uint64_t q = p + g_fnwatch_path[i];
        if (!p || !osk::host::mem_readable((void *)q, 8)) return;
        p = *(const uint64_t *)q;
    }
    uint64_t q = p ? p + g_fnwatch_path.back() : 0;
    static unsigned entries;
    if (g_hwwatch && q && q == g_fnwatch_addr && ++entries % 600 == 0) hwwatch_request(q);   // threads started since the last arm
    if (q == g_fnwatch_addr) return;
    if (!q || !osk::host::mem_readable((void *)q, 16)) { g_fnwatch_addr = 0; return; }
    g_fnwatch_addr = q;
    if (g_hwwatch) hwwatch_request(q);
    memcpy(g_fnwatch_last, (const void *)q, 16);
    loader::trace("fnwatch: watching %#llx", (unsigned long long)q);
}
static void fnwatch_check(uint64_t a, uint64_t lr) {
    static int changes;
    uint64_t q = g_fnwatch_addr;
    if (!q || changes >= 2000) return;
    if (memcmp(g_fnwatch_last, (const void *)q, 16) == 0) return;
    uint8_t now[16];
    memcpy(now, (const void *)q, 16);
    char o[40], n[40];
    for (int i = 0; i < 16; i++) { snprintf(o + 2 * i, 3, "%02x", g_fnwatch_last[i]); snprintf(n + 2 * i, 3, "%02x", now[i]); }
    memcpy(g_fnwatch_last, now, 16);
    changes++;
    loader::trace("fnwatch %#llx: %s -> %s at entry of %#llx from %s", (unsigned long long)q, o, n, (unsigned long long)a,
                  loader::symbolize(lr).c_str());
}
// Debug: SNAP_SONARPROBE=1 reads the tutorial condition evaluator (main+0x29d6d0) at each entry: its first condition and the
// "Sonar" / "System_PlayerRay" entries of the owner's system map (+0x250), logged on change; with SNAP_HWWATCH the Sonar state
// word (+0x18 of the map value) gets the debug-register watch.
static bool g_sonarprobe = getenv("SNAP_SONARPROBE") != nullptr;
static uint64_t loader_now_ms() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool rd(uint64_t a, uint64_t &v) { if (!a || !osk::host::mem_readable((void *)a, 8)) return false; v = *(const uint64_t *)a; return true; }
static void sonarprobe_hit(uint64_t x0) {
    static std::map<uint64_t, std::string> lastby;
    static int lines;
    if (lines >= 3000) return;
    std::string &last = lastby[x0];
    uint64_t map, start, size;
    if (!rd(x0 + 8, map) || !rd(x0 + 0x20, start) || !rd(x0 + 0x28, size) || !size) return;
    for (uint64_t i = start; i < start + size && i < start + 32; i++) {
    uint64_t blk, c;
    if (!rd(map + ((i >> 8) << 3), blk) || !rd(blk + (i & 0xff) * 16, c) || !c) return;
    char buf[512];
    uint64_t t0, t4, t40, t48, owner = 0;
    rd(c, t0); rd(c + 0x40, t40); rd(c + 0x48, t48); rd(c + 0x10, owner); rd(c + 4, t4);
    if ((uint32_t)t0 != 5) continue;   // the search (scan) condition only
    uint64_t tc = 0; rd(c + 0xc, tc);
    int n = snprintf(buf, sizeof buf, "x0=%llx n=%llu cond type=%u id=%08x r=%08x need=%08x t44=%08x f48=%02x", (unsigned long long)x0,
                     (unsigned long long)size, (unsigned)t0, (unsigned)(t0 >> 32), (unsigned)(t4 >> 32), (unsigned)tc,
                     (unsigned)(t40 >> 32), (unsigned)(t48 & 0xff));
    uint64_t node = 0, sonar_state = 0, msz = 0, vt = 0;
    rd(owner + 0x268, msz); rd(owner, vt);
    n += snprintf(buf + n, sizeof buf - n, " owner=%llx vt=%s msz=%llu", (unsigned long long)owner, loader::symbolize(vt).c_str(), (unsigned long long)msz);
    // SNAP_SONARPROBE=watch: the debug-register watch goes on the map's first-node/size words of the condition named by SNAP_SONARPROBE_ID
    static const uint32_t watch_id = getenv("SNAP_SONARPROBE_ID") ? (uint32_t)strtoul(getenv("SNAP_SONARPROBE_ID"), nullptr, 16) : 0;
    if (g_hwwatch && !g_collwatch && watch_id && (uint32_t)(t0 >> 32) == watch_id && owner && owner + 0x260 != g_fnwatch_addr) {
        g_fnwatch_addr = owner + 0x260; hwwatch_request(owner + 0x260);
    }
    if (owner && rd(owner + 0x260, node))
        for (int k = 0; node && k < 64; k++) {
            uint64_t key, val, st = 0;
            if (!rd(node + 0x10, key) || !rd(node + 0x20, val)) break;
            uint32_t h = (uint32_t)key;
            if (k < 12) n += snprintf(buf + n, sizeof buf - n, " k%08x", h);
            if (h == 0x5a70098fu || h == 0xacf0713eu) {
                rd(val + 0x18, st);
                n += snprintf(buf + n, sizeof buf - n, " %s@%llx=%u", h == 0x5a70098fu ? "Sonar" : "Ray", (unsigned long long)val, (unsigned)st);
                if (h == 0x5a70098fu) sonar_state = val + 0x18;
            }
            if (!rd(node, node)) break;
        }
    if (g_hwwatch && !g_collwatch && !watch_id && sonar_state && sonar_state != g_fnwatch_addr) { g_fnwatch_addr = sonar_state; hwwatch_request(sonar_state); }
    if (last == buf) continue;
    last = buf;
    lines++;
    loader::trace("sonarprobe %s", buf);
    }
}
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (g_sonarprobe && a == 0x710029d6d0ull) sonarprobe_hit(x0);
    if (g_sonarprobe && (a == 0x71002a2970ull || a == 0x710036f160ull)) {   // world bounds add (x1 = box max, min) and its caller
        static int n;
        Cpu *cc = loader::g_tls_cpu;
        uint64_t x1 = cc ? cc->x[1] : 0;
        if (n++ < 400) {
            if (a == 0x71002a2970ull && x1 && osk::host::mem_readable((void *)x1, 32)) {
                const float *f = (const float *)x1;
                loader::trace("addbounds world=%llx box=(%g %g %g)..(%g %g %g) from %s", (unsigned long long)x0, f[0], f[1], f[2], f[4], f[5], f[6], loader::symbolize(lr).c_str());
            } else loader::trace("fn %#llx x0=%llx x1=%llx from %s", (unsigned long long)a, (unsigned long long)x0, (unsigned long long)x1, loader::symbolize(lr).c_str());
        }
    }
    if (g_sonarprobe && a == 0x71002a0810ull) {   // broadphase pair walker: [x0+0x20] = {records (0x108 each), count}; first chunk only
        static int n;
        static std::string last;
        uint64_t g = 0, arr = 0, cnt = 0, first = 0;
        rd(x0 + 0x28, first);
        if ((uint32_t)first == 0 && rd(x0 + 0x20, g) && rd(g, arr) && rd(g + 8, cnt) && cnt < 100000 && n < 1500) {
            std::string s;
            char b[256];
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t r = arr + i * 0x108, col = 0, h = 0, c0 = 0, c1 = 0, ex = 0;
                if (!rd(r + 8, col) || !rd(col + 0x68, h)) continue;
                rd(r + 0xf0, c0); rd(r + 0xf8, c1); rd(r + 0x20, ex);
                uint64_t ty = 0; rd(col + 0x60, ty);
                const float *bb = (const float *)(col + 0x70);
                bool sonar = (uint32_t)h == 0x5a70098fu;
                snprintf(b, sizeof b, " [%llu/%llu %s h=%08x id=%08x type=%u x=%d..%d y=%d..%d extra=%llu", (unsigned long long)i, (unsigned long long)cnt,
                         sonar ? "SONAR" : "", (unsigned)h, (unsigned)ty, (unsigned)(ty >> 32), (int)c0, (int)(c0 >> 32), (int)c1, (int)(c1 >> 32), (unsigned long long)ex);
                s += b;
                if (sonar && osk::host::mem_readable(bb, 32)) {
                    snprintf(b, sizeof b, " aabb=(%g %g %g)..(%g %g %g)", bb[0], bb[1], bb[2], bb[4], bb[5], bb[6]);
                    s += b;
                }
                s += "]";
            }
            if (s.find("SONAR") == std::string::npos) s = " none";
            else if (osk::host::mem_readable((void *)(g + 0x680), 32)) {   // world bounds the grid spans: max at +0x680, min at +0x690
                const float *wb = (const float *)(g + 0x680);
                uint64_t lb = 0, le = 0;
                rd(0x7100cfb1d8ull, lb); rd(0x7100cfb1e0ull, le);
                snprintf(b, sizeof b, " world max=(%g %g %g %g) min=(%g %g %g %g) boxes=%lld", wb[0], wb[1], wb[2], wb[3], wb[4], wb[5], wb[6], wb[7],
                         (long long)((le - lb) / 32));
                s += b;
                for (uint64_t p = lb; p && p < le && p < lb + 32 * 8; p += 32) {
                    const float *f = (const float *)p;
                    snprintf(b, sizeof b, " box(%g %g %g | %g %g %g)", f[0], f[1], f[2], f[4], f[5], f[6]);
                    s += b;
                }
            }
            if (s != last) { last = s; n++; loader::trace("sonarproxy x0=%llx%s", (unsigned long long)x0, s.c_str()); }
        }
    }
    if (g_sonarprobe && a == 0x71002a0d30ull) {   // collider pair dispatcher: a = [[[x0]]+8], b = [x1+8]; collider +0x60 id, +0x64 type, +0x68 name hash
        static int n;
        Cpu *cc = loader::g_tls_cpu;
        uint64_t p, q, ca = 0, cb = 0, x1 = cc ? cc->x[1] : 0;
        if (rd(x0, p) && rd(p, q) && rd(q + 8, ca) && rd(x1 + 8, cb) && ca && cb) {
            uint64_t ta = 0, tb = 0, ha = 0, hb = 0;
            rd(ca + 0x60, ta); rd(cb + 0x60, tb); rd(ca + 0x68, ha); rd(cb + 0x68, hb);
            static const bool allc = getenv("SNAP_PAIRKEY") && !strcmp(getenv("SNAP_PAIRKEY"), "all");
            if (allc) {   // SNAP_PAIRKEY=all: log each collider the first time it shows up in any pair (pointer, id, type, name hash, flags)
                static std::unordered_set<uint64_t> seenc; static int nall;
                for (int side = 0; side < 2 && nall < 20000; side++) {
                    uint64_t c = side ? cb : ca, t = side ? tb : ta, h = side ? hb : ha, fl = 0;
                    if (seenc.insert(c).second) { rd(c + 0x90, fl); nall++; loader::trace("collider c=%llx id=%08x type=%u hash=%08x f=%08x", (unsigned long long)c, (unsigned)t, (unsigned)(t >> 32), (unsigned)h, (unsigned)(fl >> 24)); }
                }
            }
            // SNAP_PAIRTRACK=1: after the Player's ray collider (acf0713e) first pairs (course start), log the pk-side collider's
            // floats at most once per second per collider, to see whether a collider moves with its actor over the ride
            static const bool track = getenv("SNAP_PAIRTRACK") != nullptr; static bool started;
            if (track && ((uint32_t)ha == 0xacf0713eu || (uint32_t)hb == 0xacf0713eu)) started = true;
            if (track) {
                // SNAP_PAIRKEY = comma list of name hashes; logs each matching collider's box (collider +0x70..+0x88: two xyz corners)
                static uint32_t tks[8]; static int ntk = -1;
                if (ntk < 0) { ntk = 0; const char *e = getenv("SNAP_PAIRKEY"); while (e && *e && ntk < 8) { tks[ntk++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; else break; } if (!ntk) tks[ntk++] = 0x486b73a0u; }
                static std::unordered_map<uint64_t, uint64_t> lastt; static int nt; static std::mutex lk;
                for (int side = 0; started && side < 2 && nt < 40000; side++) {
                    uint64_t c = side ? cb : ca, h = side ? hb : ha, t = side ? tb : ta, now = loader_now_ms();
                    bool want = false; for (int i = 0; i < ntk; i++) if ((uint32_t)h == tks[i]) want = true;
                    if (!want) continue;
                    uint32_t per = ((uint32_t)h == 0x740a86c9u || (uint32_t)h == 0x42abde07u) ? 100 : 1000;   // fruit/orb boxes 10x per second
                    bool skip; { std::lock_guard<std::mutex> g(lk); uint64_t &lt = lastt[c]; skip = lt && now - lt < per; if (!skip) { lt = now; nt++; } }
                    if (skip) continue;
                    float f[8]; for (int k = 0; k < 8; k++) { uint64_t v = 0; rd(c + 0x70 + k * 4, v); uint32_t u = (uint32_t)v; memcpy(&f[k], &u, 4); }
                    uint64_t oh = side ? ha : hb;
                    // SNAP_COLLWATCH: arm the debug-register watch on the first Sight (486b73a0) collider seen after course start
                    static const bool cw = getenv("SNAP_COLLWATCH") != nullptr, ca2 = getenv("SNAP_COLLARGS") != nullptr; if (cw || ca2) g_hwwatch = g_collwatch = true;
                    if (ca2 && (uint32_t)h == 0x486b73a0u && c != g_collptr && fabsf((f[0] + f[4]) / 2 + 11) < 8 && fabsf((f[2] + f[6]) / 2 + 30) < 8 && fabsf((f[1] + f[5]) / 2 - 11) < 3) { g_collptr = c; loader::trace("collargs: collider %llx", (unsigned long long)c); }
                    if (cw && (uint32_t)h == 0x486b73a0u && c + 0x70 != g_fnwatch_addr && fabsf((f[0] + f[4]) / 2 + 11) < 8 && fabsf((f[2] + f[6]) / 2 + 30) < 8 && fabsf((f[1] + f[5]) / 2 - 11) < 3) {   /* Volcano Illumina Volcarona box near (-11,11,-30) */ g_fnwatch_addr = c + 0x70; hwwatch_request(c + 0x70); loader::trace("collwatch: watching %llx", (unsigned long long)(c + 0x70)); }
                    loader::trace("track c=%llx id=%08x h=%08x other=%08x box %.2f %.2f %.2f | %.2f %.2f %.2f", (unsigned long long)c, (unsigned)t, (unsigned)h, (unsigned)oh, f[0], f[1], f[2], f[4], f[5], f[6]);
                }
            }
            static const uint32_t pk = getenv("SNAP_PAIRKEY") ? (uint32_t)strtoul(getenv("SNAP_PAIRKEY"), nullptr, 16) : 0x5a70098fu;   // name hash to log pairs for (default "Sonar")
            if (!track && ((uint32_t)ha == pk || (uint32_t)hb == pk) && (uint32_t)ha != (uint32_t)hb && (uint32_t)ha != 0x9fb57f53u && (uint32_t)hb != 0x9fb57f53u && n < 5000) {
                // dedupe per other-side collider: first 4 pairs each; m = type matrix main+0xcfca04 [type_a][type_b] (0 = never contacts)
                static uint64_t combo[512]; static uint8_t cnt[512]; static int nc;
                uint64_t ck = ((uint32_t)ha == pk) ? cb : ca; int ci = -1;   // per other-side collider (many actors share one name hash)
                for (int i = 0; i < nc; i++) if (combo[i] == ck) { ci = i; break; }
                if (ci < 0 && nc < 512) { ci = nc++; combo[ci] = ck; cnt[ci] = 0; }
                static bool dumped;
                if (!dumped) {   // one-time dump of the collider type matrix main+0xcfca04 (row = type a, column = type b)
                    dumped = true;
                    for (int r = 0; r < 16; r++) {
                        char line[160]; int o = 0;
                        for (int c = 0; c < 16; c++) { uint64_t v = 0; rd(0x7100cfca04ull + c * 4 + r * 0x40, v); o += snprintf(line + o, sizeof line - o, " %d", (int)(uint32_t)v); }
                        loader::trace("typematrix row %2d:%s", r, line);
                    }
                }
                if (ci >= 0 && cnt[ci] < (getenv("SNAP_PAIRMAX") ? atoi(getenv("SNAP_PAIRMAX")) : 4)) {
                    cnt[ci]++; n++;
                    uint32_t tya = (uint32_t)(ta >> 32), tyb = (uint32_t)(tb >> 32); uint64_t mv = 0, fa = 0, fb = 0;
                    if (tya < 16 && tyb < 16) rd(0x7100cfca04ull + tyb * 4 + tya * 0x40, mv);
                    rd(ca + 0x90, fa); rd(cb + 0x90, fb);   // bytes +0x93 trigger?, +0x94, +0x96 narrowphase
                    {   // raw floats of the pk-side collider (+0x00..+0x5c), to find its position / bounds
                        uint64_t pc = ((uint32_t)ha == pk) ? ca : cb; char fl[400]; int o = 0;
                        for (int k = 0; k < 0x60; k += 4) { uint64_t v = 0; rd(pc + k, v); float fv; uint32_t u = (uint32_t)v; memcpy(&fv, &u, 4); o += snprintf(fl + o, sizeof fl - o, " %g", fv); }
                        loader::trace("pairfloats c=%llx:%s", (unsigned long long)pc, fl);
                    }
                    loader::trace("sonarpair a=%llx id=%08x type=%u hash=%08x f=%08x | b=%llx id=%08x type=%u hash=%08x f=%08x | m=%d", (unsigned long long)ca, (unsigned)ta,
                                  tya, (unsigned)ha, (unsigned)(fa >> 24), (unsigned long long)cb, (unsigned)tb, tyb, (unsigned)hb, (unsigned)(fb >> 24), (int)(uint32_t)mv);
                }
            }
        }
    }
    if (g_sonarprobe && a == 0x710036f160ull && getenv("SNAP_COLLOWNERS")) {   // every collider component update: owner, matrix offset, world translation (once per 2 s per owner)
        static std::unordered_map<uint64_t, uint64_t> lo; static std::mutex lk2; static int nn;
        Cpu *cc = loader::g_tls_cpu; uint64_t x2 = cc ? cc->x[2] : 0, own = 0, col = 0, now = loader_now_ms(); rd(x0 + 0x20, own); rd(x0 + 0x48, col);
        bool go; { std::lock_guard<std::mutex> g(lk2); uint64_t &l = lo[own]; go = (!l || now - l >= 2000) && nn < 30000; if (go) { l = now; nn++; } }
        if (go && x2) { float t3[3]; for (int k = 0; k < 3; k++) { uint64_t v = 0; rd(x2 + 0x30 + k * 4, v); uint32_t u = (uint32_t)v; memcpy(&t3[k], &u, 4); } uint64_t h = 0; rd(col + 0x68, h);
            loader::trace("collowner own=%llx off=%lld col=%llx h=%08x t=%.2f %.2f %.2f", (unsigned long long)own, (long long)(x2 - own), (unsigned long long)col, (unsigned)h, t3[0], t3[1], t3[2]); }
    }
    if (g_sonarprobe && a == 0x710036f160ull && g_collptr) {   // collider component update: x0 = component (+0x48 collider, +0x20/+0x28 owner weak ptr), x2 = world matrix
        uint64_t col = 0; rd(x0 + 0x48, col);
        if (col == g_collptr) {
            static uint64_t lastt; static int nl; uint64_t now = loader_now_ms();
            if (now - lastt >= 2000 && nl < 400) {
                lastt = now; nl++;
                Cpu *cc = loader::g_tls_cpu; uint64_t x1 = cc ? cc->x[1] : 0, x2 = cc ? cc->x[2] : 0, own = 0; rd(x0 + 0x20, own);
                std::string ring; for (unsigned i = 1; i <= 6; i++) { uint64_t l = t_lrring[(t_lrpos - i) & 7]; if (l) { ring += " < "; ring += loader::symbolize(l); } }
                { static char ab[24000]; int o = 0; for (int k = 0; k < 0x800; k += 4) { uint64_t v = 0; rd(own + k, v); float fv; uint32_t u = (uint32_t)v; memcpy(&fv, &u, 4); o += snprintf(ab + o, sizeof ab - o, " %.4g", fv); }
                  loader::trace("collactor %llx:%s", (unsigned long long)own, ab); }
                loader::trace("collcomp comp=%llx owner=%llx x1=%llx x2=%llx (x2-owner=%lld) lr=%s ring%s", (unsigned long long)x0, (unsigned long long)own, (unsigned long long)x1,
                              (unsigned long long)x2, (long long)(x2 - own), loader::symbolize(lr).c_str(), ring.c_str());
            }
        }
    }
    if (g_sonarprobe && a == 0x71004eb690ull && g_collptr && x0 == g_collptr) {   // collider shape/box update (x1 actor world matrix, x2/x3 joint matrices or 0)
        static uint64_t lastt; static int nl; uint64_t now = loader_now_ms();
        if (now - lastt >= 1000 && nl < 2000) {
            lastt = now; nl++;
            Cpu *cc = loader::g_tls_cpu; uint64_t x1 = cc ? cc->x[1] : 0, x2 = cc ? cc->x[2] : 0, x3 = cc ? cc->x[3] : 0, x4 = cc ? cc->x[4] : 0;
            float m[16] = {}; for (int k = 0; k < 16; k++) { uint64_t v = 0; rd(x1 + k * 4, v); uint32_t u = (uint32_t)v; memcpy(&m[k], &u, 4); }
            uint64_t map = 0, st = 0, sz = 0; rd(x0 + 8, map); rd(x0 + 0x20, st); rd(x0 + 0x28, sz);
            char sh[256]; int o = 0;
            for (uint64_t i = st; i < st + sz && i < st + 6; i++) { uint64_t blk = 0, ji = 0; rd(map + (i / 0x13) * 8, blk); rd(blk + (i % 0x13) * 0xd0 + 0x28, ji); o += snprintf(sh + o, sizeof sh - o, " j%d/k%u", (int)(uint32_t)ji, (unsigned)(ji >> 32)); }
            float j[4] = {}; if (x2) { uint64_t ji = 0, blk = 0; rd(map + (st / 0x13) * 8, blk); rd(blk + (st % 0x13) * 0xd0 + 0x28, ji); if ((int)(uint32_t)ji >= 0) for (int k = 0; k < 4; k++) { uint64_t v = 0; rd(x2 + (uint32_t)ji * 0x40 + 0x30 + k * 4, v); uint32_t u = (uint32_t)v; memcpy(&j[k], &u, 4); } }
            if (x2) { static uint64_t lp; if (now - lp >= 3000) { lp = now; static char pb[16000]; int o = 0;
                for (int i = 0; i < 160; i++) { float v3[3]; for (int k = 0; k < 3; k++) { uint64_t v = 0; rd(x2 + i * 0x40 + 0x30 + k * 4, v); uint32_t u = (uint32_t)v; memcpy(&v3[k], &u, 4); } o += snprintf(pb + o, sizeof pb - o, " %d:%.1f,%.1f,%.1f", i, v3[0], v3[1], v3[2]); }
                loader::trace("collpalette %llx:%s", (unsigned long long)x2, pb); } }
            loader::trace("collargs c=%llx world t=(%.2f %.2f %.2f) r=(%.2f %.2f %.2f) x2=%llx x3=%llx x4=%llx shapes=%llu%s joint0 t=(%.2f %.2f %.2f)", (unsigned long long)x0, m[12], m[13], m[14], m[3], m[7], m[11],
                          (unsigned long long)x2, (unsigned long long)x3, (unsigned long long)x4, (unsigned long long)sz, sh, j[0], j[1], j[2]);
        }
    }
    if (g_sonarprobe && a == 0x71004043c0ull) {   // actor contact callback (x0 = actor, x1 = event: +0x10 other's name hash, +0x18 phase, +0x1c id)
        static int n;
        static uint64_t seen[256];
        Cpu *cc = loader::g_tls_cpu;
        uint64_t ev = cc ? cc->x[1] : 0, k = 0, ph = 0;
        rd(ev + 0x10, k); rd(ev + 0x18, ph);
        uint64_t sig = x0 ^ (k << 20) ^ (ph << 52);
        bool dup = false;
        for (auto s : seen) if (s == sig) { dup = true; break; }
        if (((uint32_t)k == 0x740a86c9u || (uint32_t)k == 0x42abde07u) && (uint32_t)ph == 0) dup = false;   // "Food" (fluffruit) / "LightBall" (orb): log every hit start
        if (!dup && n < 20000) { seen[n & 255] = sig; n++; loader::trace("contact actor=%llx key=%08x self=%08x phase=%u id=%08x from %s", (unsigned long long)x0, (unsigned)k, (unsigned)(k >> 32), (unsigned)ph, (unsigned)(ph >> 32), loader::symbolize(lr).c_str()); }
    }
    if (g_hwwatch) { t_lastfn = a; t_lastlr = lr; t_lrring[t_lrpos++ & 7] = lr; }
    else if (g_fnwatch_addr) fnwatch_check(a, lr);
    if (g_fnpeek_fn == a) { fnpeek_hit(x0); fnwatch_resolve(x0); }
    loader_init_hit(a);
    snap_fn_observe(a, lr, x0);   // host/mods/lodanim.cpp (SNAP_TRACE_LOD); returns at once when unused
    snap_skips_observe(a, lr, x0);   // host/mods/skips.cpp (SNAP_SKIPS_REC)
    snap_neo_observe(a, lr, x0);   // host/mods/neofilm.cpp (dev tracer SNAP_NEO_CAMFN)
    snap_highfps_observe(a, lr, x0);   // host/mods/highfps.cpp (SNAP_FPS_TRACE / SNAP_FPS_MSWATCH)
    for (int i = 0; i < g_fntrace_n; i++)
        if (g_fntrace[i] == a) {
            // SNAP_FNTRACE_STACK=n (debug): also thread id, x1 and the guest frame-pointer chain (n return addresses as main offsets; the callee has not
            // run its prologue yet, so x29 is the caller's frame record).
            static const int stack_n = [] { const char *e = getenv("SNAP_FNTRACE_STACK"); return e ? atoi(e) : 0; }();
            if (stack_n > 0) {
                Cpu *cc = loader::g_tls_cpu;
                char chain[512];
                const unsigned long tid = (unsigned long)osk::host::thread_id();
                int len = snprintf(chain, sizeof chain, "tid=%lu x1=%#llx lr=m+%#llx chain:", tid, (unsigned long long)(cc ? cc->x[1] : 0),
                                   (unsigned long long)(lr - 0x7100000000ull));
                uint64_t fp = cc ? cc->x[29] : 0, prev = 0;
                for (int k = 0; k < stack_n && fp && !(fp & 7) && fp > prev && len < 480; k++) {
                    uint64_t nfp = 0, nlr = 0;
                    if (!rd(fp, nfp) || !rd(fp + 8, nlr)) break;
                    len += snprintf(chain + len, sizeof chain - len, " m+%#llx", (unsigned long long)(nlr - 0x7100000000ull));
                    prev = fp;
                    fp = nfp;
                }
                loader::trace("fn %#llx %s x0=%#llx", (unsigned long long)a, chain, (unsigned long long)x0);
                return;
            }
            loader::trace("fn %#llx entered from %s x0=%#llx", (unsigned long long)a, loader::symbolize(lr).c_str(), (unsigned long long)x0);
            return;
        }
}
void rt_spin_gate(Cpu *c, uint64_t head) { if (g_spin_gate_fn) g_spin_gate_fn(c, head); }
uint64_t rt_ticks(void) {
    if (g_ticks_fn) return g_ticks_fn();
    static const int64_t f = osk::host::counter_frequency();
    uint64_t q = (uint64_t)osk::host::counter(), fq = (uint64_t)f;   // cntpct_el0: 19.2 MHz
    return (q / fq) * 19200000ull + (q % fq) * 19200000ull / fq;
}

// SNAP_MXCSR_CHECK=1 (debug, off by default): lifted FP code relies on MXCSR matching the guest FPCR (cpu.h
// rt_fpcr_sync: FTZ/DAZ = FPCR.FZ, RC = nearest, exceptions masked). Every host override (HLE, NVN bridge, audio, os)
// returns to guest code through the thunk call below; this logs each override that comes back with a different
// MXCSR control word (status flags 0-5 ignored), once per override, and restores the value it had on entry.
static int g_mxcsr_check = [] { const char *e = getenv("SNAP_MXCSR_CHECK"); return e && *e == '1'; }();
static void mxcsr_checked_call(Cpu *c, Thunk *th) {
    unsigned before = __builtin_ia32_stmxcsr();
    unsigned want = (c->fpcr & (1u << 24) ? 0x8040u : 0) | 0x1F80u;   // FTZ|DAZ per FPCR.FZ, RC nearest, all masked
    if ((before & 0xFFC0u) != want) {
        static std::atomic<int> once{0};
        if (!once.exchange(1)) {
            fprintf(stderr, "mxcsr: guest code reached %s with MXCSR %#x, want %#x\n", th->name.c_str(), before, want);
            fflush(stderr);
        }
    }
    if (th->fn1) th->fn1(c); else th->fn2(c, th);
    unsigned after = __builtin_ia32_stmxcsr();
    if ((after ^ before) & ~0x3Fu) {
        static std::mutex mu;
        static std::unordered_set<std::string> seen;
        std::lock_guard<std::mutex> lk(mu);
        if (seen.insert(th->name).second) {
            fprintf(stderr, "mxcsr: %s changed MXCSR %#x -> %#x (guest fpcr %#llx); restored\n", th->name.c_str(), before, after,
                    (unsigned long long)c->fpcr);
            fflush(stderr);
        }
        __builtin_ia32_ldmxcsr((after & 0x3Fu) | (before & ~0x3Fu));
    }
}
void rt_dispatch(Cpu *c) {
    uint64_t pc = c->pc;
    GuestFn f = lookup_fn(pc);
    if (__builtin_expect(f != nullptr, 1)) { f(c); return; }
    uint64_t t = pc - THUNK_BASE;
    if (t < g_thunks.size() * 16 && !(t & 15)) {
        Thunk *th = g_thunks[t >> 4];
        g_tls_cpu = c;
        // the count saturates at 2 (only "first call" is ever read: here and probe_stub_fn's calls == 1), so a hot import no longer
        // writes its Thunk on every call from several threads (opt 2026-11-13)
        uint64_t n = th->calls;
        if (__builtin_expect(n < 2, 0)) th->calls = n + 1;
        if (__builtin_expect(g_count_imports, 0)) __atomic_fetch_add(&th->total, 1, __ATOMIC_RELAXED);
        if (n == 0 && g_trace_imports)
            trace("import %s called from %s x0=%#llx x1=%#llx x2=%#llx x3=%#llx", th->name.c_str(), symbolize(c->x[30]).c_str(),
                  (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
        if (__builtin_expect(g_mxcsr_check, 0)) { mxcsr_checked_call(c, th); return; }
        if (th->fn1) th->fn1(c); else th->fn2(c, th);
        return;
    }
    rt_unresolved(c, pc, "indirect branch/call (rt_dispatch): not a function start");
}

// Debug (SNAP_IMPORT_COUNT=1, route token icount=<tag>): every host import and svc called since the last dump, with its call count, to stderr.
void snap_import_counts_dump(const char *tag) {
    static std::vector<uint64_t> last;
    static uint64_t last_svc[0x80];
    if (!g_count_imports) { trace("[icount %s] SNAP_IMPORT_COUNT is not set\n", tag); return; }
    if (last.size() < g_thunks.size()) last.resize(g_thunks.size(), 0);
    std::vector<std::pair<uint64_t, size_t>> rows;
    for (size_t i = 0; i < g_thunks.size(); i++) {
        uint64_t t = __atomic_load_n(&g_thunks[i]->total, __ATOMIC_RELAXED);
        if (t != last[i]) rows.push_back({t - last[i], i});
        last[i] = t;
    }
    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.first > b.first; });
    trace("[icount %s] %zu imports called since the last dump\n", tag, rows.size());
    for (auto &r : rows) trace("[icount %s] %8llu %s\n", tag, (unsigned long long)r.first, g_thunks[r.second]->name.c_str());
    for (int i = 0; i < 0x80; i++)
        if (g_svc_counts[i] != last_svc[i]) { trace("[icount %s] %8llu svc %#x %s\n", tag, (unsigned long long)(g_svc_counts[i] - last_svc[i]), i, svc_name(i)); last_svc[i] = g_svc_counts[i]; }
}

void rt_import_call(Cpu *c, uint32_t idx) {
    c->pc = g_main_import_target[idx];
    if (GuestFn f = g_main_import_fn[idx]) { f(c); return; }
    rt_dispatch(c);
}
}

// a missing override: the import has no definition anywhere (strong, undefined in both modules)
namespace loader {
static void missing_stub(Cpu *c, Thunk *t) {
    std::string what = "missing override " + t->name.substr(8);
    if (g_probe) {
        record_blocker("missing", what, c, t->addr);
        c->x[0] = 0;
        return;
    }
    fprintf(stderr, "MISSING OVERRIDE %s called, caller %s, args %#llx %#llx %#llx %#llx\n", t->name.c_str() + 8, symbolize(c->x[30]).c_str(),
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    print_chain(c, t->addr, 14);
    fflush(stderr);
    abort();
}
}  // namespace loader

namespace loader {
static void probe_stub_fn(Cpu *c, Thunk *t) {
    if (t->calls == 1) record_blocker("stub-hit", t->name.substr(6), c, t->addr);
    c->x[0] = 0;
}
}  // namespace loader

extern "C" void loader_init_hit(uint64_t a) {
    using namespace loader;
    if (g_init_watch.empty()) return;
    auto it = g_init_watch.find(a);
    if (it == g_init_watch.end()) return;
    int k = ++g_init_seen[a];
    trace("init %s[%d] %#llx %s%s", it->second.first, it->second.second, (unsigned long long)a, symbolize(a).c_str(), k > 1 ? " (AGAIN)" : "");
}
