// SPDX-License-Identifier: MIT
// Linux runner bootstrap (stage 5B). See runner_linux.h.
#include "runner_linux.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "os/os_host.h"

namespace snap_linux {

namespace {

bool is_supported_cmd(const std::string &c) {
    return c == "info" || c == "shared" || c == "init" || c == "run" ||
           c == "eh" || c == "lua" || c == "bench" || c == "all";
}

void print_usage(FILE *out) {
    std::fprintf(out,
        "usage: snap_boot_linux [--main elf] [--sdk elf] "
        "info|shared|init [--probe N] [--libc]|run [--probe N] [--trace file] [--stop-after-init]|"
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

int bootstrap_runner_entry(void *ud) {
    const auto *opts = static_cast<const BootOpts *>(ud);
    // Stage 5C/5D will execute the confirmed initialization sequence here:
    //   1. install POSIX signal handlers (5C)
    //   2. loader::load(opts->main_elf.c_str(), opts->sdk_elf.c_str())
    //   3. os_init(&oc) with tp_init_var / tp_destroy_var
    //   4. snap_host::install_guest_tls_hooks(oc)
    //   5. loader::new_thread() for the main guest thread
    //   6. os_adopt_main_thread(c, 0, -1)
    //   7. run_initializers() / nninitStartup / nnMain
    std::fprintf(stderr,
                 "boot_linux: bootstrap ready (cmd=%s, main=%s, sdk=%s); "
                 "ELF loading and guest execution are not wired in stage 5B (deferred to 5D)\n",
                 opts->cmd.c_str(), opts->main_elf.c_str(), opts->sdk_elf.c_str());
    return 2;
}

}  // namespace

ParseStatus parse_args(int argc, char **argv, BootOpts &out, std::string &err) {
    out = BootOpts{};
    err.clear();

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--help" || a == "-h") {
            out.help = true;
            return ParseStatus::Help;
        }
        auto need_val = [&](const char *flag) -> const char * {
            if (i + 1 >= argc || !argv[i + 1]) {
                err = std::string("missing argument for ") + flag;
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--main") {
            const char *v = need_val("--main");
            if (!v) return ParseStatus::Error;
            out.main_elf = v;
        } else if (a == "--sdk") {
            const char *v = need_val("--sdk");
            if (!v) return ParseStatus::Error;
            out.sdk_elf = v;
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

    if (out.cmd.empty()) {
        err = "no command specified";
        return ParseStatus::Error;
    }
    return ParseStatus::Ok;
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
    if (st == ParseStatus::Error) {
        std::fprintf(stderr, "boot_linux: %s\n", err.c_str());
        print_usage(stderr);
        return 2;
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
