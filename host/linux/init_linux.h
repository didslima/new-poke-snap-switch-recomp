// SPDX-License-Identifier: MIT
// Controlled Linux guest runtime initialization (stage 5D-B2).
// Executes: preflight -> overrides -> loader::load -> os_init -> install_guest_tls_hooks ->
// loader::new_thread -> os_adopt_main_thread -> TLR+0x1f0 setup, then stops before
// run_initializers() and nnMain().
#pragma once

#include <cstdint>
#include <string>

#include "runner_linux.h"

struct Cpu;

namespace snap_linux {

enum class InitStage {
    None = 0,
    Preflight,
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

const char *init_stage_name(InitStage s);

}  // namespace snap_linux
