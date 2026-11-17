// SPDX-License-Identifier: MIT
// Stage 5D-B2: tests init_guest_runtime() controlled initialization sequence,
// TLS hook binding, TLR+0x1f0 setup, preflight failure handling, no-op mod stubs,
// and confirms run_initializers() / nnMain() are NOT executed.
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hle/guest_tls.h"
#include "linux/init_linux.h"
#include "loader.h"
#include "os/os.h"

extern "C" {
extern const uint64_t main_fn_addrs[1] = {0};
extern const GuestFn main_fn_ptrs[1] = {nullptr};
extern const uint64_t main_fn_count = 0;
extern const char *const main_import_names[1] = {""};
extern const uint64_t main_import_stub_addrs[1] = {0};
extern const uint64_t main_import_count = 0;
extern const uint64_t sdk_fn_addrs[1] = {0};
extern const GuestFn sdk_fn_ptrs[1] = {nullptr};
extern const uint64_t sdk_fn_count = 0;

void rt_mod_entry(Cpu *);
void rt_gm_arena7_size(Cpu *);
void rt_gm_ui_enter(Cpu *);
void rt_ui_opt_event(Cpu *);
}

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#pragma pack(push, 1)
struct Elf64Ehdr {
    uint8_t id[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf64Phdr {
    uint32_t type, flags;
    uint64_t off, vaddr, paddr, filesz, memsz, align;
};
struct Elf64Dyn {
    int64_t tag;
    uint64_t val;
};
struct Elf64Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
#pragma pack(pop)

// Builds a minimal ELF64 LE ET_DYN AArch64 image; if `with_tls_vars` is true, exports
// nnmuslThreadPointerInitializer and nnmuslThreadPointerDestroyer pointing into rw data.
static bool write_test_elf(const std::string &path, bool with_tls_vars) {
    constexpr uint64_t kDynStrOff = 0x200;
    constexpr uint64_t kDynSymOff = 0x300;
    constexpr uint64_t kHashOff = 0x400;
    constexpr uint64_t kDynOff = 0x500;
    constexpr uint64_t kVarsOff = 0x600;
    constexpr uint64_t kFileSize = 0x1000;
    // Ensure memsz covers hle::kMainTlsImageVaddr + 8 if this is the main stand-in.
    const uint64_t memsz = with_tls_vars ? 0x2000 : (hle::kMainTlsImageVaddr + 0x100);

    std::vector<uint8_t> buf(kFileSize, 0);
    Elf64Ehdr eh{};
    std::memcpy(eh.id, "\x7f" "ELF\x02\x01\x01", 7);
    eh.type = 3;
    eh.machine = 0xB7;
    eh.version = 1;
    eh.phoff = sizeof(Elf64Ehdr);
    eh.ehsize = sizeof(Elf64Ehdr);
    eh.phentsize = sizeof(Elf64Phdr);
    eh.phnum = 2;
    std::memcpy(buf.data(), &eh, sizeof(eh));

    Elf64Phdr ph[2]{};
    ph[0].type = 1;
    ph[0].flags = 5;   // R|X
    ph[0].off = 0;
    ph[0].vaddr = 0;
    ph[0].filesz = kFileSize;
    ph[0].memsz = memsz;
    ph[0].align = 0x1000;

    ph[1].type = 2;    // PT_DYNAMIC
    ph[1].flags = 6;
    ph[1].off = kDynOff;
    ph[1].vaddr = kDynOff;
    ph[1].filesz = 6 * sizeof(Elf64Dyn);
    ph[1].memsz = 6 * sizeof(Elf64Dyn);
    ph[1].align = 8;
    std::memcpy(buf.data() + sizeof(Elf64Ehdr), ph, sizeof(ph));

    // .dynstr
    uint32_t n_init = 0, n_dest = 0;
    size_t str_pos = 1;
    auto add_str = [&](const char *s) -> uint32_t {
        uint32_t off = (uint32_t)str_pos;
        size_t len = std::strlen(s) + 1;
        std::memcpy(buf.data() + kDynStrOff + str_pos, s, len);
        str_pos += len;
        return off;
    };
    if (with_tls_vars) {
        n_init = add_str("nnmuslThreadPointerInitializer");
        n_dest = add_str("nnmuslThreadPointerDestroyer");
    }

    // .dynsym (index 0 = null, 1 = tp_init, 2 = tp_destroy)
    uint32_t nsyms = with_tls_vars ? 3 : 1;
    if (with_tls_vars) {
        Elf64Sym s1{};
        s1.name = n_init;
        s1.info = (1 << 4) | 1;   // STB_GLOBAL | STT_OBJECT
        s1.shndx = 1;
        s1.value = kVarsOff;
        s1.size = 8;
        std::memcpy(buf.data() + kDynSymOff + 1 * sizeof(Elf64Sym), &s1, sizeof(s1));

        Elf64Sym s2{};
        s2.name = n_dest;
        s2.info = (1 << 4) | 1;
        s2.shndx = 1;
        s2.value = kVarsOff + 8;
        s2.size = 8;
        std::memcpy(buf.data() + kDynSymOff + 2 * sizeof(Elf64Sym), &s2, sizeof(s2));
    }

    // DT_HASH: nbucket=1, nchain=nsyms
    uint32_t hash_hdr[2] = {1, nsyms};
    std::memcpy(buf.data() + kHashOff, hash_hdr, sizeof(hash_hdr));

    Elf64Dyn dyns[6] = {
        {5, kDynStrOff},
        {6, kDynSymOff},
        {4, kHashOff},
        {7, kDynOff},
        {8, 0},
        {0, 0},
    };
    std::memcpy(buf.data() + kDynOff, dyns, sizeof(dyns));

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return ok;
}

int main() {
    // 1. No-op mod hooks must preserve Cpu state untouched.
    {
        Cpu c{};
        c.x[0] = 0x1234;
        c.x[10] = 0x3800000;
        rt_mod_entry(&c);
        rt_gm_arena7_size(&c);
        rt_gm_ui_enter(&c);
        rt_ui_opt_event(&c);
        CHECK(c.x[0] == 0x1234);
        CHECK(c.x[10] == 0x3800000);
    }

    // 2. Missing ELFs fail cleanly at Preflight without calling loader::load().
    {
        snap_linux::BootOpts bad;
        bad.main_elf = "/nonexistent/main.elf";
        bad.sdk_elf = "/nonexistent/sdk.elf";
        bad.cmd = "init";
        snap_linux::InitResult res;
        CHECK(!snap_linux::init_guest_runtime(bad, false, res));
        CHECK(!res.ok);
        CHECK(res.stage == snap_linux::InitStage::Preflight);
        CHECK(os_current_cpu() == nullptr);
    }

    // 3. Controlled initialization sequence with synthetic AArch64 ELFs (stops before run_initializers/nnMain).
    char dir_tmpl[] = "/tmp/snap_init_test_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    CHECK(dir != nullptr);
    if (dir) {
        struct EnvGuard {
            std::string name;
            bool had = false;
            std::string old;
            explicit EnvGuard(const char *n) : name(n) {
                if (const char *v = std::getenv(n)) { had = true; old = v; }
            }
            ~EnvGuard() {
                if (had) setenv(name.c_str(), old.c_str(), 1);
                else unsetenv(name.c_str());
            }
        };
        EnvGuard g_home("HOME"), g_xcfg("XDG_CONFIG_HOME"), g_xdata("XDG_DATA_HOME");
        EnvGuard g_hcfg("SNAP_HLE_CONFIG"), g_romfs("SNAP_ROMFS"), g_sdir("SNAP_SAVE_DIR"), g_sroot("SNAP_SAVE_ROOT");

        const std::string base(dir);
        setenv("HOME", (base + "/home").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/xdg_cfg").c_str(), 1);
        setenv("XDG_DATA_HOME", (base + "/xdg_data").c_str(), 1);
        setenv("SNAP_HLE_CONFIG", (base + "/hle.json").c_str(), 1);
        setenv("SNAP_ROMFS", (base + "/romfs").c_str(), 1);
        setenv("SNAP_SAVE_DIR", (base + "/save").c_str(), 1);
        setenv("SNAP_SAVE_ROOT", (base + "/save").c_str(), 1);

        const std::string main_p = base + "/main.elf";
        const std::string sdk_p = base + "/sdk.elf";
        CHECK(write_test_elf(main_p, false));
        CHECK(write_test_elf(sdk_p, true));

        snap_linux::BootOpts opts;
        opts.main_elf = main_p;
        opts.sdk_elf = sdk_p;
        opts.cmd = "init";
        snap_linux::InitResult res;
        CHECK(snap_linux::init_guest_runtime(opts, false, res));
        CHECK(res.ok);
        CHECK(res.stage == snap_linux::InitStage::ReadyBeforeInitializers);
        CHECK(res.tls_hooks_bound == 2);
        CHECK(res.hle_overrides_bound > 100);
        CHECK(loader::find_override("_ZN2nn2oe16GetOperationModeEv") != nullptr);
        CHECK(loader::find_override("_ZN2nn2fs8MountRomEPKcPvm") != nullptr);
        CHECK(loader::find_override("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj") != nullptr);
        CHECK(res.main_cpu != nullptr);
        CHECK(res.main_thread_type != 0);
        CHECK(res.tpidr != 0);
        CHECK(res.tlr_tp_word == res.tpidr);
        CHECK(res.initializers_run == 0);
        CHECK(loader::g_init_log.empty());
        CHECK(os_current_cpu() == res.main_cpu);
        CHECK(os_current_thread_type() == res.main_thread_type);

        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }

    std::printf("init_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
