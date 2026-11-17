// SPDX-License-Identifier: MIT
// Controlled Linux guest runtime initialization (stage 5D-B2). See init_linux.h.
#include "init_linux.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "crash_linux.h"
#include "guest_tls_hooks.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "loader.h"
#include "os/os.h"
#include "preflight_linux.h"

namespace snap_linux {
namespace {

int svc_dispatch(Cpu *c, uint32_t imm) {
    return os_svc(c, imm) ? 1 : 0;
}

void hle_bind_one(const char *sym, GuestFn fn, void *) {
    if (hle::find_override(sym) == fn) loader::add_override(sym, fn);
    else loader::add_weak_override(sym, fn);
}

}  // namespace

const char *init_stage_name(InitStage s) {
    switch (s) {
    case InitStage::None: return "none";
    case InitStage::Preflight: return "preflight";
    case InitStage::Overrides: return "overrides";
    case InitStage::LoaderLoad: return "loader::load";
    case InitStage::OsInit: return "os_init";
    case InitStage::TlsHooks: return "install_guest_tls_hooks";
    case InitStage::NewThread: return "loader::new_thread";
    case InitStage::AdoptMainThread: return "os_adopt_main_thread";
    case InitStage::ReadyBeforeInitializers: return "ready_before_initializers";
    }
    return "unknown";
}

bool init_guest_runtime(const BootOpts &opts, bool validate_archives, InitResult &out) {
    out = InitResult{};

    // 1. Static preflight before mapping any guest address space.
    out.stage = InitStage::Preflight;
    const ArtifactCheck main_elf = validate_guest_elf(opts.main_elf, "main", 0x7100000000ull, 0x10000000ull);
    const ArtifactCheck sdk_elf = validate_guest_elf(opts.sdk_elf, "sdk", 0x7110000000ull, 0x1f0000000ull);
    if (main_elf.status != PreflightStatus::Pass) {
        out.error = "main ELF preflight failed: " + main_elf.detail;
        return false;
    }
    if (sdk_elf.status != PreflightStatus::Pass) {
        out.error = "sdk ELF preflight failed: " + sdk_elf.detail;
        return false;
    }
    if (validate_archives) {
        const ArtifactCheck main_lib = validate_aot_archive(opts.main_lib, "main");
        const ArtifactCheck sdk_lib = validate_aot_archive(opts.sdk_lib, "sdk");
        if (main_lib.status != PreflightStatus::Pass) {
            out.error = "main AOT archive preflight failed: " + main_lib.detail;
            return false;
        }
        if (sdk_lib.status != PreflightStatus::Pass) {
            out.error = "sdk AOT archive preflight failed: " + sdk_lib.detail;
            return false;
        }
    }

    if (shutdown_requested()) {
        out.error = "shutdown requested before loader::load()";
        return false;
    }

    // 2. Register host overrides before loader::load() so relocations and import tables bind to them.
    out.stage = InitStage::Overrides;
    loader::install_libc_overrides();
    hle::init_trace_from_env();
    hle::load_config("");
    hle::bind_all(hle_bind_one, nullptr);
    out.hle_overrides_bound = hle::overrides().size();
    hle::set_log_sink([](const char *l, size_t n) { loader::trace("%.*s", (int)n, l); });
    hle::os_hooks().init_system_event = [](void *ev, bool manual) { os_system_event_create((uint64_t)(uintptr_t)ev, manual ? 0 : 1); };
    hle::os_hooks().signal_system_event = [](void *ev) { os_system_event_signal((uint64_t)(uintptr_t)ev); };
    size_t os_count = 0;
    const OsOverride *os_tab = os_override_table(&os_count);
    for (size_t i = 0; i < os_count; ++i) {
        loader::add_override(os_tab[i].sym, os_tab[i].fn);
    }
    for (const auto &st : opts.stubs) {
        loader::add_probe_stub(st);
    }

    // 3. Map and relocate guest modules (executes zero guest instructions).
    out.stage = InitStage::LoaderLoad;
    if (!loader::load(opts.main_elf.c_str(), opts.sdk_elf.c_str())) {
        out.error = "loader::load() returned false";
        return false;
    }

    if (shutdown_requested()) {
        out.error = "shutdown requested after loader::load()";
        return false;
    }

    // 4. Initialize Horizon OS scheduler with guest TLS hook variable addresses.
    out.stage = InitStage::OsInit;
    OsConfig oc = {};
    oc.trace = std::getenv("SNAP_OS_TRACE") != nullptr ? 1 : 0;
    oc.tp_init_var = loader::lookup("nnmuslThreadPointerInitializer");
    oc.tp_destroy_var = loader::lookup("nnmuslThreadPointerDestroyer");
    os_init(&oc);

    // 5. Bind musl thread-pointer hooks before creating the first guest thread.
    out.stage = InitStage::TlsHooks;
    out.tls_hooks_bound = snap_host::install_guest_tls_hooks(oc);

    loader::g_svc_handler = svc_dispatch;
    loader::g_spin_gate_fn = os_spin_gate;
    loader::g_ticks_fn = os_ticks;

    // 6. Allocate main guest thread (captures main's .tdata image into GuestTlsImage).
    out.stage = InitStage::NewThread;
    loader::GuestThread *th = loader::new_thread();
    if (!th || !th->c) {
        out.error = "loader::new_thread() returned null";
        return false;
    }
    Cpu *c = th->c;

    // 7. Adopt main thread into scheduler and publish ELF thread pointer into TLR+0x1f0.
    out.stage = InitStage::AdoptMainThread;
    os_adopt_main_thread(c, 0, -1);
    *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->tpidrro + 0x1f0)) = c->tpidr;

    out.main_cpu = c;
    out.main_thread_type = os_current_thread_type();
    out.tpidr = c->tpidr;
    out.tlr_tp_word = *reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(c->tpidrro + 0x1f0));
    out.initializers_run = static_cast<uint64_t>(loader::g_init_log.size());

    if (!out.tpidr || out.tlr_tp_word != out.tpidr || os_current_cpu() != c || !out.main_thread_type) {
        out.error = "post-adopt invariant check failed (tpidr/TLR/os_current_cpu)";
        return false;
    }

    // Stop explicitly before run_initializers(), nninitStartup(), and nnMain().
    out.stage = InitStage::ReadyBeforeInitializers;
    out.ok = true;
    std::fprintf(stderr,
                 "init_linux: main=%llu funcs, sdk=%llu funcs, tls_hooks=%d, tpidr=%#llx, tpidrro=%#llx; "
                 "stopping before run_initializers() and nnMain() (stage 5D-B2)\n",
                 static_cast<unsigned long long>(loader::g_main.fn_count),
                 static_cast<unsigned long long>(loader::g_sdk.fn_count),
                 out.tls_hooks_bound,
                 static_cast<unsigned long long>(out.tpidr),
                 static_cast<unsigned long long>(c->tpidrro));
    return true;
}

}  // namespace snap_linux
