// SPDX-License-Identifier: MIT
// POSIX signal handling and minimal async-signal-safe crash diagnostics for the Linux runner (stage 5C).
// Handles synchronous fatal faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE) on a dedicated sigaltstack and
// asynchronous shutdown requests (SIGINT, SIGTERM, SIGHUP) via volatile sig_atomic_t.
// Note: os_current_cpu() is intentionally NOT called from signal handlers in this stage because
// runtime/os/os_kernel.cpp's t_self is a C++ thread_local and t->cur transitions non-atomically in do_switch.
#pragma once

#include <cstddef>

namespace snap_linux {

// Dedicated alternate signal stack size (64 KiB, well above Linux x86-64 SIGSTKSZ).
// Separate from the 256 MiB runner thread stack.
inline constexpr size_t kSignalAltStackSize = 64u * 1024u;

// Installs handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE (sync) and SIGINT, SIGTERM, SIGHUP (async),
// plus an alternate signal stack when install_altstack is true.
// Returns false if already installed, if any third-party handler is already registered on those signals,
// or if sigaltstack/sigaction fails (rolling back any partial installation).
bool install_signal_handlers(bool install_altstack = true);

// Restores previous signal handlers and alternate signal stack. Safe to call when not installed (no-op).
void uninstall_signal_handlers();

// Returns true when handlers are currently installed.
bool signal_handlers_installed();

// Asynchronous shutdown observation (safe to poll from normal runner code).
bool shutdown_requested();
int shutdown_signal();
void clear_shutdown_request();

// Test-only hook: injects a simulated sigaction failure at step `fail_at_step` (>= 0) to verify rollback.
bool install_signal_handlers_for_test(bool install_altstack, int fail_at_step);

}  // namespace snap_linux
