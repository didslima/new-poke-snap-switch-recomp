// SPDX-License-Identifier: MIT
// Stage 5C: isolated tests for POSIX signal handling, sigaltstack, rollback,
// async shutdown request observation, and fatal synchronous signal subprocess behavior.
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "linux/crash_linux.h"

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void dummy_third_party(int) {}

static bool sigaction_equal(const struct sigaction &a, const struct sigaction &b) {
    // glibc's sigaction() sets SA_RESTORER (0x04000000) internally when installing a handler,
    // which is absent on untouched default dispositions.
    constexpr int kGlibcRestorer = 0x04000000;
    if ((a.sa_flags & ~kGlibcRestorer) != (b.sa_flags & ~kGlibcRestorer)) return false;
    if (a.sa_flags & SA_SIGINFO) return a.sa_sigaction == b.sa_sigaction;
    return a.sa_handler == b.sa_handler;
}

static void run_fatal_child(int sig) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    std::fflush(nullptr);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!snap_linux::install_signal_handlers(true)) _exit(99);
        if (sig == SIGSEGV) {
            void *p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                volatile int *vp = static_cast<volatile int *>(p);
                *vp = 1;
            }
            raise(SIGSEGV);
        } else if (sig == SIGILL) {
#if defined(__x86_64__)
            __asm__ volatile("ud2");
#endif
            raise(SIGILL);
        } else if (sig == SIGBUS) {
            raise(SIGBUS);
        } else if (sig == SIGFPE) {
            raise(SIGFPE);
        }
        _exit(98);
    }

    close(pipefd[1]);
    std::string err_out;
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        err_out.append(buf, (size_t)n);
    }
    close(pipefd[0]);

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == sig);
    CHECK(err_out.find("[crash_linux] fatal signal ") != std::string::npos);
    CHECK(err_out.find("addr=") != std::string::npos);
    CHECK(err_out.find("rip=") != std::string::npos);
}

// True when `sig` is ignored in this environment (nohup, some CI); the installer keeps SIG_IGN for the asynchronous signals, so they are untestable.
static bool async_ignored(int sig) {
    struct sigaction sa{};
    if (sigaction(sig, nullptr, &sa) != 0) return true;
    return !(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == SIG_IGN;
}

// Runs a child that installs the handlers, delivers `sigs` to itself in order, then reports through its exit status:
//   exits with the handler's own _exit(128 + sig) when the second request forced it out,
//   97 when it survived and shutdown_requested() was false after the first signal (handler did not record it),
//   0 when it survived all signals with the request recorded.
static int run_signal_child(const int *sigs, int n) {
    std::fflush(nullptr);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        if (!snap_linux::install_signal_handlers(true)) _exit(99);
        for (int i = 0; i < n; ++i) {
            raise(sigs[i]);
            if (!snap_linux::shutdown_requested()) _exit(97);
        }
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main() {
    const int kAllSigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGINT, SIGTERM, SIGHUP};
    struct sigaction orig_sa[7]{};
    for (int i = 0; i < 7; ++i) CHECK(sigaction(kAllSigs[i], nullptr, &orig_sa[i]) == 0);
    stack_t orig_ss{};
    CHECK(sigaltstack(nullptr, &orig_ss) == 0);

    // 1. Install, verify sigaltstack, reject duplicate, uninstall, verify restoration.
    CHECK(!snap_linux::signal_handlers_installed());
    CHECK(snap_linux::install_signal_handlers(true));
    CHECK(snap_linux::signal_handlers_installed());

    stack_t cur_ss{};
    CHECK(sigaltstack(nullptr, &cur_ss) == 0);
    CHECK((cur_ss.ss_flags & SS_DISABLE) == 0);
    CHECK(cur_ss.ss_size == snap_linux::kSignalAltStackSize);

    // Duplicate install must fail cleanly without changing installed state.
    CHECK(!snap_linux::install_signal_handlers(true));
    CHECK(snap_linux::signal_handlers_installed());

    snap_linux::uninstall_signal_handlers();
    CHECK(!snap_linux::signal_handlers_installed());

    for (int i = 0; i < 7; ++i) {
        struct sigaction after{};
        CHECK(sigaction(kAllSigs[i], nullptr, &after) == 0);
        CHECK(sigaction_equal(orig_sa[i], after));
    }
    stack_t after_ss{};
    CHECK(sigaltstack(nullptr, &after_ss) == 0);
    CHECK(after_ss.ss_flags == orig_ss.ss_flags);
    CHECK(after_ss.ss_sp == orig_ss.ss_sp);

    // 2. Partial installation failure rollback.
    CHECK(!snap_linux::install_signal_handlers_for_test(true, 3));
    CHECK(!snap_linux::signal_handlers_installed());
    for (int i = 0; i < 7; ++i) {
        struct sigaction after{};
        CHECK(sigaction(kAllSigs[i], nullptr, &after) == 0);
        CHECK(sigaction_equal(orig_sa[i], after));
    }
    CHECK(sigaltstack(nullptr, &after_ss) == 0);
    CHECK(after_ss.ss_flags == orig_ss.ss_flags);

    // 3. Do not silently overwrite a third-party handler (save/restore SIGTERM).
    {
        struct sigaction custom{}, prev_term{};
        custom.sa_handler = dummy_third_party;
        sigemptyset(&custom.sa_mask);
        CHECK(sigaction(SIGTERM, &custom, &prev_term) == 0);

        CHECK(!snap_linux::install_signal_handlers(true));
        CHECK(!snap_linux::signal_handlers_installed());

        struct sigaction check_term{}, check_segv{};
        CHECK(sigaction(SIGTERM, nullptr, &check_term) == 0);
        CHECK(check_term.sa_handler == dummy_third_party);
        CHECK(sigaction(SIGSEGV, nullptr, &check_segv) == 0);
        CHECK(sigaction_equal(orig_sa[0], check_segv));

        CHECK(sigaction(SIGTERM, &prev_term, nullptr) == 0);
    }

    // 4. Asynchronous shutdown signals (SIGINT, SIGTERM, SIGHUP).
    {
        snap_linux::clear_shutdown_request();
        CHECK(!snap_linux::shutdown_requested());
        CHECK(snap_linux::install_signal_handlers(true));

        // Only test signals that were not SIG_IGN in the parent environment.
        struct sigaction sa_check{};
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
            CHECK(sigaction(sig, nullptr, &sa_check) == 0);
            if (!(sa_check.sa_flags & SA_SIGINFO) && sa_check.sa_handler == SIG_IGN) continue;
            snap_linux::clear_shutdown_request();
            CHECK(raise(sig) == 0);
            CHECK(snap_linux::shutdown_requested());
            CHECK(snap_linux::shutdown_signal() == sig);
        }
        snap_linux::clear_shutdown_request();
        snap_linux::uninstall_signal_handlers();
    }

    // 4b. check_present_exit (the present loop's stop test): nothing pending keeps running, a window close exits 0, and a pending signal exits
    //     128 + signal and wins over a window close.
    {
        snap_linux::clear_shutdown_request();
        int code = -1;
        CHECK(!snap_linux::check_present_exit(false, &code));
        CHECK(!snap_linux::check_present_exit(false, nullptr));
        CHECK(snap_linux::check_present_exit(true, &code) && code == 0);
        CHECK(snap_linux::check_present_exit(true, nullptr));
        if (!async_ignored(SIGTERM)) {
            CHECK(snap_linux::install_signal_handlers(true));
            CHECK(raise(SIGTERM) == 0);
            code = -1;
            CHECK(snap_linux::check_present_exit(false, &code) && code == 128 + SIGTERM);
            code = -1;
            CHECK(snap_linux::check_present_exit(true, &code) && code == 128 + SIGTERM);
            snap_linux::clear_shutdown_request();
            CHECK(!snap_linux::check_present_exit(false, &code));
            snap_linux::uninstall_signal_handlers();
        }
    }

    // 4c. First request only records; a second one before it is honored leaves at once with 128 + signal (nobody is polling).
    if (!async_ignored(SIGTERM) && !async_ignored(SIGINT) && !async_ignored(SIGHUP)) {
        const int one[] = {SIGTERM};
        CHECK(run_signal_child(one, 1) == 0);
        const int twice[] = {SIGTERM, SIGTERM};
        CHECK(run_signal_child(twice, 2) == 128 + SIGTERM);
        const int mixed[] = {SIGINT, SIGTERM};
        CHECK(run_signal_child(mixed, 2) == 128 + SIGTERM);
        const int hup[] = {SIGHUP, SIGHUP};
        CHECK(run_signal_child(hup, 2) == 128 + SIGHUP);
    }

    // 5. Synchronous fatal signals in isolated child subprocesses.
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
        run_fatal_child(sig);
    }

    std::printf("crash_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
