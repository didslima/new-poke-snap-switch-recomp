// SPDX-License-Identifier: MIT
// Controlled Linux guest runtime initialization (stage 5D-B2).
// Executes: preflight -> overrides -> loader::load -> os_init -> install_guest_tls_hooks ->
// loader::new_thread -> os_adopt_main_thread -> TLR+0x1f0 setup, then stops before
// run_initializers() and nnMain().
#pragma once

#include <cstdint>
#include <string>

#include "runner_linux.h"
#include "window_sdl3.h"

struct Cpu;

namespace snap_linux {

enum class InitStage {
    None = 0,
    Preflight,
    Window,
    Overrides,
    LoaderLoad,
    OsInit,
    TlsHooks,
    NewThread,
    AdoptMainThread,
    ReadyBeforeInitializers,
};

struct InitResult {
    bool ok = false;
    InitStage stage = InitStage::None;
    int tls_hooks_bound = 0;
    size_t hle_overrides_bound = 0;
    int audio_overrides_bound = 0;
    int nvn_procs_bound = 0;
    bool nvn_bound = false;
    bool window_created = false;
    void *native_window = nullptr;
    bool hooks_installed = false;
    Cpu *main_cpu = nullptr;
    uint64_t main_thread_type = 0;
    uint64_t tpidr = 0;
    uint64_t tlr_tp_word = 0;
    uint64_t initializers_run = 0;
    std::string error;
};

// Initializes the Linux guest runtime up to the point immediately before run_initializers().
// Never calls run_initializers(), nninitStartup(), or nnMain().
// If validate_archives is true, also validates opts.main_lib and opts.sdk_lib before load().
bool init_guest_runtime(const BootOpts &opts, bool validate_archives, InitResult &out);

// Non-blocking host window event pump (also invoked from the NVN present hook).
WindowEvents pump_host_window_events();
bool host_window_quit_requested();
HostWindow *host_window();

// Clean shutdown ordering: NVN hooks -> audio -> SDL3 host window.
void shutdown_guest_runtime();

const char *init_stage_name(InitStage s);

}  // namespace snap_linux
