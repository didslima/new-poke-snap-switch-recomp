// SPDX-License-Identifier: MIT
// Linux runner bootstrap (stage 5B). See runner_linux.h.
#include "runner_linux.h"
#include "cli_flags.h"
#include "crash_linux.h"
#include "preflight_linux.h"
#ifdef SNAP_LINUX_HAS_AOT
#include "audio/audio_out.h"
#include "hle/hle.h"
#include "hle/settings.h"
#include "init_linux.h"
#include "loader.h"
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

#include "os/os_host.h"

namespace snap_linux {

namespace {

bool is_supported_cmd(const std::string &c) {
    return c == "validate" || c == "info" || c == "shared" || c == "init" || c == "run" ||
           c == "eh" || c == "lua" || c == "bench" || c == "all";
}

void print_usage(FILE *out) {
    std::fprintf(out,
        "usage: snap_boot_linux [--main elf] [--sdk elf] [--main-lib lib.a] [--sdk-lib lib.a] "
        "[--play] [--fullscreen|--windowed|--borderless] [--width W] [--height H] [--monitor N] "
        "[--steam-deck] [--safe-mode] [--profile name] [--romfs dir] [--data-dir dir] [--set key=val] [--version] "
        "validate|info|shared|init [--probe N] [--libc]|run [--probe N] [--trace file] [--stop-after-init] [--allow-guest-exec]|"
        "eh|lua|bench [--iters N]|all\n");
}

struct RunnerCall {
    int (*fn)(void *);
    void *ud;
    int rc;
};

void runner_trampoline(void *p) {
    auto *call = static_cast<RunnerCall *>(p);
    osk::host::set_current_thread_name("snap-runner");
    call->rc = call->fn(call->ud);
}

struct SignalGuard {
    bool active = false;
    SignalGuard() : active(install_signal_handlers(true)) {}
    ~SignalGuard() {
        if (active) uninstall_signal_handlers();
    }
};

int bootstrap_runner_entry(void *ud) {
    const auto *opts = static_cast<const BootOpts *>(ud);
    SignalGuard signals;
    if (!signals.active) {
        std::fprintf(stderr, "boot_linux: warning: could not install POSIX signal handlers\n");
    }
    if (shutdown_requested()) {
        std::fprintf(stderr, "boot_linux: shutdown requested by signal %d before startup\n", shutdown_signal());
        return 128 + shutdown_signal();
    }
#ifdef SNAP_LINUX_HAS_AOT
    struct RuntimeShutdownGuard {
        bool active = false;
        ~RuntimeShutdownGuard() {
            if (active) {
                shutdown_guest_runtime();
            }
        }
    } runtime_guard;

    if (opts->cmd == "init" || opts->cmd == "run") {
        runtime_guard.active = true;
        InitResult res;
        if (!init_guest_runtime(*opts, true, res)) {
            std::fprintf(stderr, "boot_linux: init failed at stage '%s': %s\n",
                         init_stage_name(res.stage), res.error.c_str());
            return 1;
        }
        if (opts->cmd == "init") {
            return 0;
        }
        RunPlan plan;
        std::string plan_err;
        if (!prepare_run_pipeline(*opts, res, plan, plan_err)) {
            std::fprintf(stderr, "boot_linux: run pipeline preparation failed: %s\n", plan_err.c_str());
            return 1;
        }
        if (!opts->allow_guest_exec) {
            std::fprintf(stderr,
                         "boot_linux: run pipeline prepared (nninitStartup=%#llx, nnMain=%#llx, stop_after_init=%d); "
                         "guest execution disabled by default (pass --allow-guest-exec to opt in)\n",
                         static_cast<unsigned long long>(plan.startup_addr),
                         static_cast<unsigned long long>(plan.nnmain_addr),
                         plan.stop_after_init ? 1 : 0);
            return plan.stop_after_init ? 0 : 2;
        }
        if (shutdown_requested()) {
            std::fprintf(stderr, "boot_linux: shutdown requested by signal %d before guest execution\n", shutdown_signal());
            return 128 + shutdown_signal();
        }
        std::fprintf(stderr,
                     "boot_linux: WARNING: entering controlled guest execution "
                     "(stop_after_init=%d, nninitStartup=%#llx, nnMain=%#llx)\n",
                     plan.stop_after_init ? 1 : 0,
                     static_cast<unsigned long long>(plan.startup_addr),
                     static_cast<unsigned long long>(plan.nnmain_addr));
        struct TraceFileGuard {
            FILE *fp = nullptr;
            ~TraceFileGuard() {
                if (fp) {
                    loader::g_trace_file = nullptr;
                    std::fclose(fp);
                }
            }
        } trace_guard;
        if (!opts->trace_file.empty()) {
            trace_guard.fp = std::fopen(opts->trace_file.c_str(), "w");
            if (!trace_guard.fp) {
                std::fprintf(stderr, "boot_linux: warning: could not open trace file '%s'\n", opts->trace_file.c_str());
            } else {
                loader::g_trace_file = trace_guard.fp;
            }
        }
        GuestExecutionOps ops{};
        ops.run_initializers = loader::run_initializers;
        ops.call_guest = [](Cpu *c, uint64_t addr) { loader::call_guest(c, addr); };
        ops.stop_vi_vsync = []() { hle::vi_stop_vsync(); };
        RunResult run_res;
        std::string run_err;
        if (!execute_run_pipeline(plan, ops, run_res, run_err)) {
            std::fprintf(stderr, "boot_linux: execute_run_pipeline failed: %s\n", run_err.c_str());
            return 1;
        }
        return 0;
    }
#else
    if (opts->cmd == "init") {
        std::fprintf(stderr,
                     "boot_linux: 'init' requires AOT libraries (libsnap_main.a / libsnap_sdk.a) "
                     "which were not linked in this build\n");
        return 2;
    }
#endif
    // Stage 5D-C / 6+ will wire run_initializers() and nnMain() here.
    std::fprintf(stderr,
                 "boot_linux: command '%s' is not wired yet (stage 5D-B2 stops before run_initializers/nnMain)\n",
                 opts->cmd.c_str());
    return 2;
}

}  // namespace

ParseStatus parse_args(int argc, char **argv, BootOpts &out, std::string &err) {
    out = BootOpts{};
    err.clear();

    std::vector<std::string> raw;
    for (int i = 1; i < argc; i++) raw.emplace_back(argv[i] ? argv[i] : "");
    const snap_cli::Parsed cli = snap_cli::parse(raw);
    if (!cli.error.empty()) {
        err = cli.error;
        return ParseStatus::Error;
    }
    if (cli.help) {
        out.help = true;
        return ParseStatus::Help;
    }
    if (cli.version) {
        out.version = true;
        return ParseStatus::Version;
    }
    if (!cli.action.empty()) {
        err = "unsupported Windows launcher option: --" + cli.action;
        return ParseStatus::Error;
    }
    out.play = cli.play;
    out.safe = cli.safe;
    out.profile = cli.profile;
    out.romfs = cli.romfs;
    out.data_dir = cli.data_dir;
    out.cli_env = cli.env;
    out.cli_sets = cli.sets;

    const auto &rest = cli.rest;
    for (size_t i = 0; i < rest.size(); i++) {
        const std::string &a = rest[i];
        auto need_val = [&](const char *flag) -> const char * {
            if (i + 1 >= rest.size()) {
                err = std::string("missing argument for ") + flag;
                return nullptr;
            }
            return rest[++i].c_str();
        };

        if (a == "--main") {
            const char *v = need_val("--main");
            if (!v) return ParseStatus::Error;
            out.main_elf = v;
        } else if (a == "--sdk") {
            const char *v = need_val("--sdk");
            if (!v) return ParseStatus::Error;
            out.sdk_elf = v;
        } else if (a == "--main-lib") {
            const char *v = need_val("--main-lib");
            if (!v) return ParseStatus::Error;
            out.main_lib = v;
        } else if (a == "--sdk-lib") {
            const char *v = need_val("--sdk-lib");
            if (!v) return ParseStatus::Error;
            out.sdk_lib = v;
        } else if (a == "--probe") {
            const char *v = need_val("--probe");
            if (!v) return ParseStatus::Error;
            out.probe = std::atoi(v);
        } else if (a == "--iters") {
            const char *v = need_val("--iters");
            if (!v) return ParseStatus::Error;
            out.iters = std::atoi(v);
        } else if (a == "--libc") {
            out.libc = true;
        } else if (a == "--stop-after-init") {
            out.stop_after_init = true;
        } else if (a == "--allow-guest-exec") {
            out.allow_guest_exec = true;
        } else if (a == "--no-window") {
            out.no_window = true;
        } else if (a == "--trace") {
            const char *v = need_val("--trace");
            if (!v) return ParseStatus::Error;
            out.trace_file = v;
        } else if (a == "--stub") {
            const char *v = need_val("--stub");
            if (!v) return ParseStatus::Error;
            out.stubs.emplace_back(v);
        } else if (!a.empty() && a[0] == '-') {
            err = "unsupported option: " + a;
            return ParseStatus::Error;
        } else if (out.cmd.empty()) {
            if (!is_supported_cmd(a)) {
                err = "unsupported command: " + a;
                return ParseStatus::Error;
            }
            out.cmd = a;
        } else if (out.arg.empty()) {
            out.arg = a;
        } else {
            err = "unexpected extra argument: " + a;
            return ParseStatus::Error;
        }
    }

    if (out.cmd.empty() && out.play) {
        out.cmd = "run";
    }
    if (out.cmd.empty()) {
        err = "no command specified";
        return ParseStatus::Error;
    }
    return ParseStatus::Ok;
}

bool resolve_and_validate_romfs(const BootOpts &opts, std::string &resolved_out, std::string &err_out) {
    resolved_out.clear();
    err_out.clear();

    const char *env_romfs = std::getenv("SNAP_ROMFS");
    const bool from_cli = !opts.romfs.empty();
    const bool from_env = !from_cli && env_romfs && *env_romfs;
    const std::string raw = from_cli ? opts.romfs : (from_env ? std::string(env_romfs) : std::string("extracted/romfs-v262144"));
    const char *src_label = from_cli ? "--romfs" : (from_env ? "SNAP_ROMFS" : "default fallback");

    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(raw);
    if (!std::filesystem::exists(p, ec) || ec) {
        err_out = std::string("ROMFS directory not found (from ") + src_label + "): '" + raw +
                  "'. Pass --romfs <dir> or set SNAP_ROMFS=<dir> to your extracted New Pokemon Snap 2.0.1 romfs folder.";
        return false;
    }
    if (!std::filesystem::is_directory(p, ec) || ec) {
        err_out = std::string("ROMFS path is not a directory (from ") + src_label + "): '" + raw +
                  "'. Pass --romfs <dir> or set SNAP_ROMFS=<dir>.";
        return false;
    }
    const std::filesystem::path abs_p = std::filesystem::absolute(p, ec);
    const std::string final_p = (!ec && !abs_p.empty()) ? abs_p.string() : raw;
    if (::access(final_p.c_str(), R_OK | X_OK) != 0) {
        err_out = std::string("ROMFS directory is not accessible (from ") + src_label + "): '" + final_p +
                  "' (" + std::strerror(errno) + "). Check directory permissions.";
        return false;
    }
    resolved_out = final_p;
    return true;
}

bool cpu_has_v3() {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") &&
           __builtin_cpu_supports("bmi") &&
           __builtin_cpu_supports("bmi2") &&
           __builtin_cpu_supports("fma");
#else
    return false;
#endif
}

bool run_on_runner_thread(int (*fn)(void *), void *ud, int *out_rc, size_t stack_size) {
    if (!fn) return false;
    RunnerCall call{fn, ud, 1};
    osk::host::HostThread *th = osk::host::thread_create_suspended(stack_size, runner_trampoline, &call);
    if (!th) return false;
    osk::host::thread_resume(th);
    osk::host::thread_join(th);
    if (out_rc) *out_rc = call.rc;
    return true;
}

int bootstrap_main(int argc, char **argv) {
    static char errbuf[1 << 16];
    std::setvbuf(stderr, errbuf, _IOLBF, sizeof errbuf);

    if (!cpu_has_v3()) {
        std::fprintf(stderr, "boot_linux: this build needs an x86-64-v3 CPU (AVX2, BMI2, FMA); this CPU lacks it\n");
        return 2;
    }

    BootOpts opts;
    std::string err;
    const ParseStatus st = parse_args(argc, argv, opts, err);
    if (st == ParseStatus::Help) {
        print_usage(stdout);
        return 0;
    }
    if (st == ParseStatus::Version) {
        std::string v;
        if (FILE *f = std::fopen("config.json", "rb")) {
            std::string text; char buf[2048]; size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
            v = snap_cli::version_from_config(text);
        }
#ifdef SNAP_VERSION
        if (v.empty()) v = SNAP_VERSION;
#endif
        std::printf("Snap Recomp %s (Linux)\n", v.empty() ? "(development build)" : v.c_str());
        return 0;
    }
    if (st == ParseStatus::Error) {
        std::fprintf(stderr, "boot_linux: %s\n", err.c_str());
        print_usage(stderr);
        return 2;
    }

    if (!opts.data_dir.empty()) {
        std::error_code ec;
        const std::filesystem::path dd = std::filesystem::u8path(opts.data_dir);
        std::filesystem::create_directories(dd / "config", ec);
        if (ec) {
            std::fprintf(stderr, "boot_linux: --data-dir '%s' failed: %s\n", opts.data_dir.c_str(), ec.message().c_str());
            return 2;
        }
        std::filesystem::create_directories(dd / "save", ec);
        setenv("SNAP_CONFIG_DIR", (dd / "config").string().c_str(), 1);
        setenv("SNAP_SAVE_ROOT", (dd / "save").string().c_str(), 1);
    }
    if (!opts.profile.empty()) setenv("SNAP_PROFILE", opts.profile.c_str(), 1);
    if (opts.safe) setenv("SNAP_SAFE_MODE", "1", 1);
    for (const auto &kv : opts.cli_env) setenv(kv.first.c_str(), kv.second.c_str(), 1);

    const bool need_romfs = (opts.cmd == "run" && opts.allow_guest_exec && !opts.stop_after_init);
    if (!opts.romfs.empty() || need_romfs) {
        std::string resolved_romfs;
        std::string romfs_err;
        if (!resolve_and_validate_romfs(opts, resolved_romfs, romfs_err)) {
            std::fprintf(stderr, "boot_linux: %s\n", romfs_err.c_str());
            return 2;
        }
        setenv("SNAP_ROMFS", resolved_romfs.c_str(), 1);
    }
#ifdef SNAP_LINUX_HAS_AOT
    for (const auto &kv : opts.cli_sets) {
        if (const char *n = hle::settings::env_name(kv.first)) unsetenv(n);
        hle::settings::override_for_run(kv.first, kv.second);
    }
    hle::settings::load();
#endif

    if (opts.cmd == "validate") {
        const PreflightReport rep = run_preflight(opts.main_elf, opts.sdk_elf, opts.main_lib, opts.sdk_lib);
        print_preflight_report(rep, stdout);
        return (int)rep.overall;
    }

    int rc = 1;
    if (!run_on_runner_thread(bootstrap_runner_entry, &opts, &rc, kRunnerHostStackSize)) {
        std::fprintf(stderr, "boot_linux: failed to create %zu-byte runner thread (errno=%lu: %s)\n",
                     kRunnerHostStackSize, osk::host::last_error(), std::strerror((int)osk::host::last_error()));
        return 1;
    }
    return rc;
}

}  // namespace snap_linux
