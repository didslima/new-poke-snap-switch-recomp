// SPDX-License-Identifier: MIT
// Save states on Linux: not supported. This file stands where runtime/os/os_state.cpp stands on Windows and defines the symbols the rest of
// the program links against, with a different meaning: the feature does not exist, and every request says so.
//
//  * g_sphase is the phase of a save-state drain. Nothing on Linux ever sets it, so it stays 0 for the whole run.
//  * os_kernel.cpp calls state_gate_wait / state_gate_acq only when g_sphase != 0 (and at phase 0 the Windows versions return at once
//    without doing anything). Here they cannot be reached; if one ever is, that is a bug, and the process stops with a message instead of
//    carrying on as if a drain had happened.
//  * state_init starts no checkpoint or test-driver thread; asking for one with SNAP_CHECKPOINT or SNAP_STATE_TEST is reported.
//  * snap_state_request / snap_state_info / snap_state_idle / snap_state_count are what the UI (ui_savestate.cpp) and the route conditions
//    (input_core.cpp) call. A request is refused and logged; the info says there is no state, the last operation failed and why; no counter
//    ever moves.
// Windows is not affected: its os_state.cpp is unchanged. Docs: docs/linux-port.md, "Stage 3 part B".
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../os_internal.h"

// The layouts below are the ones of runtime/os/os_state.cpp and runtime/ui/ui_savestate.cpp (each declares them itself).
extern "C" {
struct SnapStateHooks {
    void (*pre_save)();
    int (*pre_load)(char *why, int cap);
    void (*post_load)();
    void (*shot)();
};
struct SnapStateInfo {
    int busy, has_state, last_op, last_ok, saves, loads, done;
    unsigned long long last_done_ms, saved_ms;
    char last_msg[112];
};
}

namespace {
const char kWhy[] = "save states are not supported on Linux";
std::mutex g_mu;
SnapStateInfo g_info;   // zero: no state, nothing busy, no counters
}  // namespace

namespace osk {

std::atomic<int> g_sphase{0};

void state_gate_wait(Thread *, Lock &) { fatal("state_gate_wait reached on Linux: save states are not supported, g_sphase must stay 0"); }
bool state_gate_acq(Thread *, Lock &) { fatal("state_gate_acq reached on Linux: save states are not supported, g_sphase must stay 0"); }

void state_init() {
    if (getenv("SNAP_CHECKPOINT") || getenv("SNAP_STATE_TEST")) log("SNAP_CHECKPOINT / SNAP_STATE_TEST ignored: %s", kWhy);
}

}  // namespace osk

// Host layers that own state a save would have to capture register here (NVN, audio). Nothing ever calls them on Linux.
extern "C" void snap_state_set_hooks(const SnapStateHooks *) {}

extern "C" void snap_state_request(int op, const char *name) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_info.last_op = op;
    g_info.last_ok = 0;
    snprintf(g_info.last_msg, sizeof g_info.last_msg, "%s", kWhy);
    fprintf(stderr, "[os] %s (%s %s: request refused)\n", kWhy, op == 1 ? "save" : op == 2 ? "load" : "request", name ? name : "");
}
extern "C" int snap_state_idle() { return 1; }   // nothing is queued or running, and nothing can be
extern "C" void snap_state_info(SnapStateInfo *out) {
    std::lock_guard<std::mutex> lk(g_mu);
    *out = g_info;
}
extern "C" int snap_state_count(const char *) { return 0; }   // no save or load ever finishes
