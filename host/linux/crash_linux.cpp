// SPDX-License-Identifier: MIT
// POSIX signal handling and async-signal-safe crash diagnostics for the Linux runner (stage 5C).
#include "crash_linux.h"

#include <cstdint>
#include <cstring>
#include <errno.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace snap_linux {
namespace {

struct SignalSpec {
    int sig;
    bool sync;
    const char *name;
};

constexpr SignalSpec kSignals[] = {
    {SIGSEGV, true,  "SIGSEGV"},
    {SIGBUS,  true,  "SIGBUS"},
    {SIGILL,  true,  "SIGILL"},
    {SIGFPE,  true,  "SIGFPE"},
    {SIGINT,  false, "SIGINT"},
    {SIGTERM, false, "SIGTERM"},
    {SIGHUP,  false, "SIGHUP"},
};
constexpr int kNumSignals = (int)(sizeof(kSignals) / sizeof(kSignals[0]));

bool g_installed = false;
bool g_altstack_installed = false;
stack_t g_old_altstack{};
struct sigaction g_old_actions[kNumSignals]{};
bool g_slot_saved[kNumSignals]{};

alignas(16) uint8_t g_altstack_buf[kSignalAltStackSize];

volatile sig_atomic_t g_shutdown_sig = 0;
volatile sig_atomic_t g_in_fatal = 0;

// ---- async-signal-safe formatting helpers (no libc printf, no heap, no locks) ----

void write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

struct SafeBuf {
    char data[384];
    size_t len = 0;

    void put_char(char c) {
        if (len < sizeof(data)) data[len++] = c;
    }
    void put_str(const char *s) {
        if (!s) return;
        while (*s && len < sizeof(data)) data[len++] = *s++;
    }
    void put_dec(long long v) {
        if (v < 0) {
            put_char('-');
            // Avoid UB on INT64_MIN.
            uint64_t uv = (uint64_t)(-(v + 1)) + 1ull;
            put_udec(uv);
            return;
        }
        put_udec((uint64_t)v);
    }
    void put_udec(uint64_t v) {
        char tmp[24];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        while (n > 0) put_char(tmp[--n]);
    }
    void put_hex(uint64_t v) {
        static const char kHex[] = "0123456789abcdef";
        put_str("0x");
        char tmp[16];
        int n = 0;
        do {
            tmp[n++] = kHex[v & 0xf];
            v >>= 4;
        } while (v != 0);
        while (n > 0) put_char(tmp[--n]);
    }
};

const char *signal_name(int sig) {
    for (const auto &s : kSignals) {
        if (s.sig == sig) return s.name;
    }
    return "SIG?";
}

bool has_third_party_handler(const struct sigaction &old) {
    if (old.sa_flags & SA_SIGINFO) {
        return old.sa_sigaction != nullptr;
    }
    return old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN;
}

void sync_signal_handler(int sig, siginfo_t *info, void *uctx) {
    if (g_in_fatal) {
        _exit(128 + sig);
    }
    g_in_fatal = 1;

    SafeBuf b;
    b.put_str("[crash_linux] fatal signal ");
    b.put_str(signal_name(sig));
    b.put_str(" (");
    b.put_dec(sig);
    b.put_str(")");
    if (info) {
        b.put_str(" code=");
        b.put_dec(info->si_code);
        b.put_str(" addr=");
        b.put_hex((uint64_t)(uintptr_t)info->si_addr);
    }
#if defined(__x86_64__)
    if (uctx) {
        const auto *uc = static_cast<const ucontext_t *>(uctx);
        b.put_str(" rip=");
        b.put_hex((uint64_t)uc->uc_mcontext.gregs[REG_RIP]);
        b.put_str(" rsp=");
        b.put_hex((uint64_t)uc->uc_mcontext.gregs[REG_RSP]);
    }
#else
    (void)uctx;
#endif
    b.put_char('\n');
    write_all(STDERR_FILENO, b.data, b.len);

    // SA_RESETHAND already reset `sig` to SIG_DFL; unblock and re-raise so the parent
    // observes the exact terminating signal without running atexit/destructors.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, nullptr);
    raise(sig);
    _exit(128 + sig);
}

void async_signal_handler(int sig) {
    if (g_shutdown_sig != 0) {
        // Second request while the first is still unanswered: nobody is polling shutdown_requested(). Only async-signal-safe calls here.
        _exit(128 + sig);
    }
    g_shutdown_sig = sig;
}

void rollback_partial(int installed_count) {
    for (int i = installed_count - 1; i >= 0; --i) {
        if (g_slot_saved[i]) {
            sigaction(kSignals[i].sig, &g_old_actions[i], nullptr);
            g_slot_saved[i] = false;
        }
    }
    if (g_altstack_installed) {
        sigaltstack(&g_old_altstack, nullptr);
        g_altstack_installed = false;
    }
}

bool install_impl(bool install_altstack, int fail_at_step) {
    if (g_installed) return false;

    std::memset(g_old_actions, 0, sizeof(g_old_actions));
    std::memset(g_slot_saved, 0, sizeof(g_slot_saved));
    g_old_altstack = {};
    g_altstack_installed = false;

    if (install_altstack) {
        stack_t ss{};
        ss.ss_sp = g_altstack_buf;
        ss.ss_size = sizeof(g_altstack_buf);
        ss.ss_flags = 0;
        if (sigaltstack(&ss, &g_old_altstack) != 0) {
            return false;
        }
        g_altstack_installed = true;
    }

    for (int i = 0; i < kNumSignals; ++i) {
        if (fail_at_step >= 0 && i == fail_at_step) {
            rollback_partial(i);
            return false;
        }
        struct sigaction old{};
        if (sigaction(kSignals[i].sig, nullptr, &old) != 0) {
            rollback_partial(i);
            return false;
        }
        if (has_third_party_handler(old)) {
            rollback_partial(i);
            return false;
        }
        g_old_actions[i] = old;
        g_slot_saved[i] = true;

        // Preserve SIG_IGN on asynchronous signals (e.g. background/nohup processes).
        if (!kSignals[i].sync && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_IGN) {
            continue;
        }

        struct sigaction sa{};
        sigemptyset(&sa.sa_mask);
        if (kSignals[i].sync) {
            sa.sa_sigaction = sync_signal_handler;
            sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
            if (g_altstack_installed) sa.sa_flags |= SA_ONSTACK;
        } else {
            sa.sa_handler = async_signal_handler;
            sa.sa_flags = SA_RESTART;
        }
        if (sigaction(kSignals[i].sig, &sa, nullptr) != 0) {
            rollback_partial(i + 1);
            return false;
        }
    }

    g_installed = true;
    return true;
}

}  // namespace

bool install_signal_handlers(bool install_altstack) {
    return install_impl(install_altstack, -1);
}

bool install_signal_handlers_for_test(bool install_altstack, int fail_at_step) {
    return install_impl(install_altstack, fail_at_step);
}

void uninstall_signal_handlers() {
    if (!g_installed) return;
    rollback_partial(kNumSignals);
    g_installed = false;
}

bool signal_handlers_installed() { return g_installed; }

bool shutdown_requested() { return g_shutdown_sig != 0; }

int shutdown_signal() { return (int)g_shutdown_sig; }

void clear_shutdown_request() { g_shutdown_sig = 0; }

bool check_present_exit(bool quit_requested, int *out_exit_code) {
    const int sig = (int)g_shutdown_sig;
    if (sig != 0) {
        if (out_exit_code) *out_exit_code = 128 + sig;
        return true;
    }
    if (quit_requested) {
        if (out_exit_code) *out_exit_code = 0;
        return true;
    }
    return false;
}

}  // namespace snap_linux
