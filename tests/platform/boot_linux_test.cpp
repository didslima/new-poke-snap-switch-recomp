// SPDX-License-Identifier: MIT
// Stage 5B: tests Linux runner bootstrap, CLI parsing, CPU check, and 256 MiB runner thread stack.
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "linux/runner_linux.h"
#include "os/os_host.h"

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

struct StackProbe {
    uint32_t caller_tid = 0;
    uint32_t runner_tid = 0;
    size_t actual_stack_size = 0;
    int getattr_rc = -1;
};

static int inspect_runner_thread(void *ud) {
    auto *p = static_cast<StackProbe *>(ud);
    p->runner_tid = osk::host::thread_id();
    pthread_attr_t attr;
    p->getattr_rc = pthread_getattr_np(pthread_self(), &attr);
    if (p->getattr_rc == 0) {
        pthread_attr_getstacksize(&attr, &p->actual_stack_size);
        pthread_attr_destroy(&attr);
    }
    return 77;
}

static snap_linux::ParseStatus run_parse(std::vector<std::string> args, snap_linux::BootOpts &out, std::string &err) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &s : args) argv.push_back(s.data());
    return snap_linux::parse_args((int)argv.size(), argv.data(), out, err);
}

int main() {
    // 1. Default paths and valid command parsing.
    {
        snap_linux::BootOpts o;
        std::string err;
        auto st = run_parse({"snap_boot_linux", "run", "--probe", "12", "--stop-after-init", "--trace", "t.log", "--stub", "foo"}, o, err);
        CHECK(st == snap_linux::ParseStatus::Ok);
        CHECK(err.empty());
        CHECK(o.cmd == "run");
        CHECK(o.main_elf == "analysis/import/v262144-main.elf");
        CHECK(o.sdk_elf == "analysis/import/v262144-sdk.elf");
        CHECK(o.probe == 12);
        CHECK(o.stop_after_init);
        CHECK(o.trace_file == "t.log");
        CHECK(o.stubs.size() == 1 && o.stubs[0] == "foo");
    }

    // 2. Custom ELF paths and --help.
    {
        snap_linux::BootOpts o;
        std::string err;
        auto st = run_parse({"snap_boot_linux", "--main", "/tmp/m.elf", "--sdk", "/tmp/s.elf", "init", "--libc"}, o, err);
        CHECK(st == snap_linux::ParseStatus::Ok);
        CHECK(o.main_elf == "/tmp/m.elf");
        CHECK(o.sdk_elf == "/tmp/s.elf");
        CHECK(o.cmd == "init");
        CHECK(o.libc);

        st = run_parse({"snap_boot_linux", "--help"}, o, err);
        CHECK(st == snap_linux::ParseStatus::Help);
        CHECK(o.help);
    }

    // 3. Unsupported options, missing option values, and unsupported commands fail clearly.
    {
        snap_linux::BootOpts o;
        std::string err;
        CHECK(run_parse({"snap_boot_linux"}, o, err) == snap_linux::ParseStatus::Error);
        CHECK(!err.empty());

        CHECK(run_parse({"snap_boot_linux", "run", "--timeout", "10"}, o, err) == snap_linux::ParseStatus::Error);
        CHECK(err.find("--timeout") != std::string::npos);

        CHECK(run_parse({"snap_boot_linux", "--import-save", "x"}, o, err) == snap_linux::ParseStatus::Error);
        CHECK(run_parse({"snap_boot_linux", "--main"}, o, err) == snap_linux::ParseStatus::Error);
        CHECK(run_parse({"snap_boot_linux", "pads"}, o, err) == snap_linux::ParseStatus::Error);
    }

    // 4. CPU capability check (x86-64-v3 host).
    CHECK(snap_linux::cpu_has_v3());

    // 5. Runner thread creation with 256 MiB stack (without loading ELFs or allocating 256 MiB eagerly).
    {
        StackProbe probe;
        probe.caller_tid = osk::host::thread_id();
        int rc = -1;
        const bool ok = snap_linux::run_on_runner_thread(inspect_runner_thread, &probe, &rc);
        CHECK(ok);
        CHECK(rc == 77);
        CHECK(probe.runner_tid != 0 && probe.runner_tid != probe.caller_tid);
        CHECK(probe.getattr_rc == 0);
        CHECK(probe.actual_stack_size >= snap_linux::kRunnerHostStackSize);
    }

    std::printf("boot_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
