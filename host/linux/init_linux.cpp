// SPDX-License-Identifier: MIT
// Controlled Linux guest runtime initialization (stage 5D-B2). See init_linux.h.
#include "init_linux.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/audio_out.h"
#include "crash_linux.h"
#include "guest_tls_hooks.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "loader.h"
#include "nvn/nvn_host.h"
#include "os/os.h"
#include "preflight_linux.h"

int snap_bind_audio();
void snap_bind_nvn();
int snap_nvn_bridge_count();
extern "C" void nvnHostNotifyWindowResize(void *nativeWindow);
extern "C" void snap_fs_drain_for_exit(int ms);

namespace snap_linux {
namespace {

HostWindow g_host_win;
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<bool> g_hooks_active{false};

bool ultrawide_disabled_by_env() {
    const char *uw = std::getenv("SNAP_ULTRAWIDE");
    return uw && (std::strcmp(uw, "off") == 0 || std::strcmp(uw, "OFF") == 0 || std::strcmp(uw, "0") == 0);
}

void apply_ultrawide_from_window(const HostWindow &win) {
    if (ultrawide_disabled_by_env() || !win.is_open()) {
        nvnHostSetUltrawide(1.0f);
        return;
    }
    nvnHostSetUltrawide(HostWindow::compute_ultrawide_factor(win.pixel_width(), win.pixel_height()));
}

void vi_get_native_window_host(Cpu *c) {
    if (c && c->x[0]) {
        *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_host_win.native_handle()));
    }
    if (c) c->x[0] = 0;
}

void hook_present(void *, int slot, int w, int h) {
    const WindowEvents ev = pump_host_window_events();
    const uint64_t n = ++g_presents;
    if (n <= 3 || n % 30 == 0) {
        loader::trace("nvn: present #%llu slot %d %dx%d (submits %llu)",
                      static_cast<unsigned long long>(n), slot, w, h,
                      static_cast<unsigned long long>(g_submits.load(std::memory_order_relaxed)));
    }
    if (ev.quit_requested) {
        snap_audio_log_checks();
        snap_fs_drain_for_exit(5000);
        g_host_win.destroy();
        std::_Exit(0);
    }
}

void hook_submit(void *, int, int) {
    ++g_submits;
}

int svc_dispatch(Cpu *c, uint32_t imm) {
    return os_svc(c, imm) ? 1 : 0;
}

void hle_bind_one(const char *sym, GuestFn fn, void *) {
    if (hle::find_override(sym) == fn) loader::add_override(sym, fn);
    else loader::add_weak_override(sym, fn);
}

uint64_t g_opus_vtbl[4];
void opus_ctor(Cpu *c) {
    if (c && c->x[0]) *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) = reinterpret_cast<uint64_t>(g_opus_vtbl);
}
void opus_nop(Cpu *) {}
void opus_work_size(Cpu *c) {
    if (c) c->x[0] = 0x20000ull * (c->x[2] ? c->x[2] : 1);
}
uint64_t g_ngc_vtbl[4];
void ngc_ctor(Cpu *c) {
    if (c && c->x[0]) *reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(c->x[0])) = reinterpret_cast<uint64_t>(g_ngc_vtbl);
}

int bind_codec_stubs() {
    g_opus_vtbl[0] = loader::add_override("host:HardwareOpusDecoder::dtor", opus_nop)->addr;
    g_opus_vtbl[1] = loader::add_override("host:HardwareOpusDecoder::deleting_dtor", opus_nop)->addr;
    g_opus_vtbl[2] = loader::add_override("host:HardwareOpusDecoder::GetWorkBufferSize", opus_work_size)->addr;
    loader::add_override("_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_ctor);

    g_ngc_vtbl[0] = loader::add_override("host:ProfanityFilter::dtor", opus_nop)->addr;
    g_ngc_vtbl[1] = loader::add_override("host:ProfanityFilter::deleting_dtor", opus_nop)->addr;
    loader::add_override("_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor);
    return 2;
}

}  // namespace

const char *init_stage_name(InitStage s) {
    switch (s) {
    case InitStage::None: return "none";
    case InitStage::Preflight: return "preflight";
    case InitStage::Window: return "window";
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
        out.error = "shutdown requested before host window creation";
        return false;
    }

    // 2. Initialize SDL3 host window BEFORE loader::load / NVN so nn::vi::GetNativeWindow
    // has a valid SDL_Window* ready before guest code ever touches NVN.
    out.stage = InitStage::Window;
    if (!opts.no_window && !std::getenv("SNAP_HEADLESS")) {
        WindowConfig wcfg;
        wcfg.hidden = (opts.cmd == "init" || opts.stop_after_init);
        if (g_host_win.create(wcfg)) {
            out.window_created = true;
            out.native_window = g_host_win.native_handle();
            apply_ultrawide_from_window(g_host_win);
        } else {
            std::fprintf(stderr, "init_linux: warning: SDL3 window creation failed; continuing headless\n");
        }
    }

    // 3. Register host overrides before loader::load() so relocations and import tables bind to them.
    out.stage = InitStage::Overrides;
    loader::install_libc_overrides();
    hle::init_trace_from_env();
    hle::load_config("");
    hle::bind_all(hle_bind_one, nullptr);
    out.hle_overrides_bound = hle::overrides().size();
    hle::set_log_sink([](const char *l, size_t n) { loader::trace("%.*s", (int)n, l); });
    hle::os_hooks().init_system_event = [](void *ev, bool manual) { os_system_event_create((uint64_t)(uintptr_t)ev, manual ? 0 : 1); };
    hle::os_hooks().signal_system_event = [](void *ev) { os_system_event_signal((uint64_t)(uintptr_t)ev); };
    out.audio_overrides_bound = snap_bind_audio();
    snap_audio_set_os_hooks([](void *ev) { os_system_event_signal(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev))); });
    snap_audio_set_event_init_hook([](void *ev) { os_system_event_create(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ev)), 1); });
    snap_bind_nvn();
    out.nvn_bound = true;
    out.nvn_procs_bound = snap_nvn_bridge_count();
    out.codec_stubs_bound = bind_codec_stubs();
    loader::add_override("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window_host);
    {
        static const NvnHostHooks hk{nullptr, hook_present, hook_submit};
        nvnHostSetHooks(&hk);
        g_hooks_active.store(true, std::memory_order_release);
        out.hooks_installed = true;
    }
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

WindowEvents pump_host_window_events() {
    if (!g_host_win.is_open()) return {};
    const WindowEvents ev = g_host_win.poll_events();
    if (ev.resized || ev.display_changed) {
        apply_ultrawide_from_window(g_host_win);
        nvnHostNotifyWindowResize(g_host_win.native_handle());
    }
    return ev;
}

bool host_window_quit_requested() {
    return g_host_win.quit_requested();
}

HostWindow *host_window() {
    return g_host_win.is_open() ? &g_host_win : nullptr;
}

void shutdown_guest_runtime() {
    hle::vi_stop_vsync();
    if (g_hooks_active.exchange(false, std::memory_order_acq_rel)) {
        nvnHostSetHooks(nullptr);
    }
    snap_audio_log_checks();
    snap_audio_shutdown();
    g_host_win.destroy();
}

bool prepare_run_pipeline(const BootOpts &opts, const InitResult &init, RunPlan &out, std::string &err) {
    out = RunPlan{};
    err.clear();
    if (!init.ok || init.stage != InitStage::ReadyBeforeInitializers || !init.main_cpu || !init.tpidr ||
        init.tlr_tp_word != init.tpidr) {
        err = "init_guest_runtime has not reached ReadyBeforeInitializers";
        return false;
    }
    out.main_cpu = init.main_cpu;
    out.startup_addr = loader::lookup("nninitStartup");
    out.nnmain_addr = loader::lookup("nnMain");
    out.stop_after_init = opts.stop_after_init;
    out.trace_enabled = !opts.trace_file.empty();
    out.valid = true;
    return true;
}

bool execute_run_pipeline(const RunPlan &plan, const GuestExecutionOps &ops, RunResult &out, std::string &err) {
    out = RunResult{};
    err.clear();
    if (!plan.valid || !plan.main_cpu) {
        err = "invalid RunPlan";
        return false;
    }
    if (!ops.run_initializers || !ops.call_guest) {
        err = "GuestExecutionOps missing required entry points";
        return false;
    }
    out.initializers_run = ops.run_initializers(plan.main_cpu, true);
    if (plan.stop_after_init) {
        out.stopped_after_init = true;
        if (ops.stop_vi_vsync) {
            ops.stop_vi_vsync();
            out.stopped_vsync = true;
        }
        out.ok = true;
        return true;
    }
    if (plan.startup_addr) {
        ops.call_guest(plan.main_cpu, plan.startup_addr);
        out.called_startup = true;
    }
    if (plan.nnmain_addr) {
        ops.call_guest(plan.main_cpu, plan.nnmain_addr);
        out.called_nnmain = true;
    }
    if (ops.stop_vi_vsync) {
        ops.stop_vi_vsync();
        out.stopped_vsync = true;
    }
    out.ok = true;
    return true;
}

}  // namespace snap_linux
