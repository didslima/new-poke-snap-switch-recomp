// SPDX-License-Identifier: MIT
// Linux runner bootstrap interface (stage 5B).
// Establishes CLI parsing, x86-64-v3 capability check, and dedicated 256 MiB runner thread
// creation without invoking loader::load, os_init, guest threads, or signal handlers yet.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace snap_linux {

// Matches the host stack reservation used by host/boot.cpp (256 MiB) for AOT guest call chains.
inline constexpr size_t kRunnerHostStackSize = 256u << 20;

struct BootOpts {
    std::string main_elf = "analysis/import/v262144-main.elf";
    std::string sdk_elf = "analysis/import/v262144-sdk.elf";
    std::string main_lib = "build/aot/main/lib/libsnap_main.a";
    std::string sdk_lib = "build/aot/sdk/lib/libsnap_sdk.a";
    std::string cmd;
    std::string arg;
    std::string trace_file;
    std::vector<std::string> stubs;
    int probe = 0;
    int iters = 5;
    bool libc = false;
    bool stop_after_init = false;
    bool allow_guest_exec = false;
    bool no_window = false;
    bool help = false;
    bool version = false;
    bool play = false;
    bool safe = false;
    std::string profile;
    std::string data_dir;
    std::vector<std::pair<std::string, std::string>> cli_env;
    std::vector<std::pair<std::string, std::string>> cli_sets;
};

enum class ParseStatus {
    Ok,
    Help,
    Version,
    Error,
};

// Parses essential runner CLI arguments; rejects unsupported/unknown options and commands.
ParseStatus parse_args(int argc, char **argv, BootOpts &out, std::string &err);

// Returns true when the host CPU supports x86-64-v3 (AVX2, BMI1, BMI2, FMA).
bool cpu_has_v3();

// Runs `fn(ud)` on a dedicated host thread created with `stack_size` bytes of stack
// (default kRunnerHostStackSize = 256 MiB). Returns false if thread creation fails.
bool run_on_runner_thread(int (*fn)(void *), void *ud, int *out_rc, size_t stack_size = kRunnerHostStackSize);

// Entry point called by host/linux/boot_linux.cpp main().
int bootstrap_main(int argc, char **argv);

}  // namespace snap_linux
