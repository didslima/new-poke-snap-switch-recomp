// SPDX-License-Identifier: MIT
// One-run A/B capture (project code, ours; docs/ab-capture.md, driver host\ab_capture.ps1). Inert unless SNAP_ABCAP=<plan file> is set.
//
// One worker thread, no guest hook, no change to the game's code path:
//   1. polls the route condition (the same text a route `wait` line takes: ride>=40, ui=photo_submit, ...) through snap_route_cond
//      (runtime/hle/input_core.cpp);
//   2. FREEZES GAME TIME with os_time_pause(1) (runtime/os/os_kernel.cpp): nn::os::GetSystemTick stands still, so the game's frame delta is 0
//      and animation, camera, rail and particles hold, while the game thread keeps running its frame loop and the renderer keeps drawing
//      (and presenting) the same frame state. The hook is the OS tick on purpose: it is the one clock the guest reads, it is already used
//      for the focus-loss pause (which blocks the presenting thread, so nothing renders there), it needs no guest patch and it resumes with
//      no jump in dt (the paused span is subtracted). Rejected: the game's pause menu (draws a menu over the scene), blocking the frame-step
//      or vsync wait (stops rendering), SNAP_NEO_SPEED=0 (only the rail), a dt patch in the clock update (readers copy the clock by value);
//   3. for each value in the list, sets the effect (snaplight::ab_force for `lighting`, or a live environment variable), lets the temporal
//      state settle, records the present range of a timing window (per-pass GPU ms come from the SNAP_PERF_SHARED / SNAP_GPU_PASSES
//      csv files, read by the driver), then asks NVN_FRAME_DUMP for one full-size frame (g_fdBurst);
//   4. photo-frame check: presses the shutter (snap_input_force_buttons) with each value, counts the photo passes (FxPhotoPassCount) and dumps the presents
//      that FxIsPhotoFrame() holds (Evaluation frames; a ride photo holds none since 2026-11-19); the driver compares their hashes and the gating counters;
//   5. unfreezes, lets the game run for a few presents, writes the result file and exits.
//
// Plan file (key=value lines): moment=<cond> hold=<n presents the condition must hold> effect=<name> values=<a,b,c> settle=<presents>
// window=<presents> photo=<0|1> out=<result file> timeout=<wall seconds>.   Result file: JSON, see docs/ab-capture.md.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hle/lighting_preset.h"
#include "nvn/nvn_dev.h"   // live params and named GPU pass times (docs/devloop.md); needs SNAP_DEV=1

extern "C" void os_time_pause(int on);
extern "C" uint64_t snap_present_count();
extern "C" int snap_route_cond(const char* c, char* why, int n);
extern "C" void snap_input_force_buttons(const char* names);
namespace nvn { extern int g_fdBurst; bool FxIsPhotoFrame(); uint64_t FxRanCount(); uint64_t FxRanOnPhotoCount(); uint64_t FxPhotoPassCount(); }
namespace snap::ui {   // the options rows, set the way the menu sets them (runtime/ui/ui_options.cpp)
bool options_test_set(const std::string& key, const std::string& value);
std::string options_test_get(const std::string& key);
std::string options_test_default(const std::string& key);
bool options_test_option_when(const std::string& key, std::string* when);
}

namespace {

struct Plan {
    std::string moment, effect, out;
    std::vector<std::string> values;
    int hold = 3, settle = 12, window = 24, photo = 1, timeout = 600;
};

struct Req { std::string kind; int state; std::string value; uint64_t present; int photoFlag; };
struct State { std::string value, info; uint64_t setP = 0, winP0 = 0, winP1 = 0; double gpuBusy = -1; std::vector<std::pair<std::string, double>> passes; };
struct PhotoRun { std::string value; int photoPresents = 0, dumps = 0; bool timeRunning = false, filmEmpty = false; uint64_t ran = 0, ranOnPhoto = 0, passes = 0; };
std::vector<PhotoRun> g_photoRuns;
Plan g_plan;
std::vector<Req> g_reqs;
std::vector<State> g_states;
FILE* g_log;
uint64_t g_momentP, g_freezeP, g_endP;
std::vector<std::pair<uint64_t, int>> g_photoSeen;   // (present, FxIsPhotoFrame) samples taken while the shutter was held, one per present
bool g_froze;

uint64_t pc() { return snap_present_count(); }
double wall() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)n.QuadPart / (double)f.QuadPart; }

void say(const char* fmt, ...) {
    char b[400]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    std::fprintf(stderr, "[abcap] p%llu %s\n", (unsigned long long)pc(), b);
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v; size_t a = 0;
    while (a <= s.size()) { size_t b = s.find(c, a); if (b == std::string::npos) b = s.size(); if (b > a) v.push_back(s.substr(a, b - a)); a = b + 1; }
    return v;
}

bool load_plan(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[4096];
    std::string rawValues;
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#') continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "moment") g_plan.moment = v; else if (k == "effect") g_plan.effect = v; else if (k == "out") g_plan.out = v;
        else if (k == "values") rawValues = v;
        else if (k == "hold") g_plan.hold = std::atoi(v.c_str()); else if (k == "settle") g_plan.settle = std::atoi(v.c_str());
        else if (k == "window") g_plan.window = std::atoi(v.c_str()); else if (k == "photo") g_plan.photo = std::atoi(v.c_str());
        else if (k == "timeout") g_plan.timeout = std::atoi(v.c_str());
    }
    std::fclose(f);
    g_plan.values = split(rawValues, g_plan.effect == "opts" ? '|' : ',');   // option values hold commas ("ssaa2,smaa")
    return !g_plan.moment.empty() && !g_plan.effect.empty() && !g_plan.values.empty();
}

bool truthy(const std::string& v) { return !(v == "0" || v == "off" || v == "Off" || v == "OFF" || v == "false" || v == "no" || v == "none"); }

// The control channel (SNAP_DEV=1, runtime/nvn/nvn_dev.h, docs/devloop.md): any registered live param by its env var name (dev_param_set), plus
//   lighting | all | ab      the SNAP_LIGHT_AB switch every effect already honors (snaplight::ab_on): off = the game's own picture
//   SNAP_*, NVN_*            without SNAP_DEV (or an unregistered name): a live environment variable (effects that read getenv per frame); `unset` removes it
// Values: numbers, or off/on/false/true for a bool param.
bool dev_value(const std::string& v, double* out) {
    if (v == "on" || v == "true" || v == "On" || v == "ON") { *out = 1; return true; }
    if (v == "off" || v == "false" || v == "Off" || v == "OFF") { *out = 0; return true; }
    char* end = nullptr; const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end) return false;
    *out = d; return true;
}
//   opts                     rows of the options menu, set the way the menu sets them: each value is <key>=<value> ("-" = the row's default), values
//                            separated by '|'. Proves a row applies with no restart: the frame after the change differs from the one before.
std::string g_info;   // what the last apply() did to an options row (result file "info")
void apply(const std::string& value) {
    const std::string& e = g_plan.effect;
    g_info.clear();
    if (e == "opts") {
        const size_t eq = value.find('=');
        if (eq == std::string::npos) { g_info = "same"; return; }   // a bare word: change nothing (a control frame)
        const std::string k = value.substr(0, eq);
        std::string v = value.substr(eq + 1), when = "unknown";
        if (v == "-") v = snap::ui::options_test_default(k);
        snap::ui::options_test_option_when(k, &when);
        const bool ok = snap::ui::options_test_set(k, v);
        g_info = when + (ok ? "" : " set-failed") + " now=" + snap::ui::options_test_get(k);
        say("option %s = '%s' (%s)", k.c_str(), v.c_str(), g_info.c_str());
        return;
    }
    if (e == "lighting" || e == "all" || e == "ab") { snaplight::ab_force().store(truthy(value) ? 1 : 0); return; }
    double d;
    if (dev_on() && dev_value(value, &d) && dev_param_set(e.c_str(), d)) { say("dev param %s = %g", e.c_str(), d); return; }
    if (e.rfind("SNAP_", 0) == 0 || e.rfind("NVN_", 0) == 0) _putenv_s(e.c_str(), value == "unset" ? "" : value.c_str());
    else say("unknown effect '%s' (nothing set)", e.c_str());
}

bool wait_presents(uint64_t n, double maxSec = 40) {
    const uint64_t target = pc() + n;
    const double t0 = wall();
    while (pc() < target) { Sleep(1); if (wall() - t0 > maxSec) { say("stalled waiting for %llu presents", (unsigned long long)n); return false; } }
    return true;
}

// One full-size frame: g_fdBurst = 4 dumps the next real present (nvn_vk.cpp). Waits until it was taken.
bool dump(const char* kind, int state, const std::string& value, int photoFlag = -1, bool drain = true) {
    const double t0 = wall();
    while (nvn::g_fdBurst > 0) { Sleep(1); if (wall() - t0 > 10) return false; }
    g_reqs.push_back({kind, state, value, pc(), photoFlag});
    nvn::g_fdBurst = 4;
    while (nvn::g_fdBurst > 3) { Sleep(1); if (wall() - t0 > 20) { say("dump not taken"); return false; } }
    while (drain && nvn::g_fdBurst > 0) { Sleep(1); if (wall() - t0 > 30) break; }
    return true;
}

void write_result(const char* status) {
    if (g_plan.out.empty()) return;
    FILE* f = std::fopen(g_plan.out.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "{\n \"status\": \"%s\",\n \"effect\": \"%s\",\n \"moment\": \"%s\",\n \"settle\": %d, \"window\": %d,\n", status, g_plan.effect.c_str(), g_plan.moment.c_str(), g_plan.settle, g_plan.window);
    std::fprintf(f, " \"moment_present\": %llu, \"freeze_present\": %llu, \"end_present\": %llu,\n \"states\": [", (unsigned long long)g_momentP, (unsigned long long)g_freezeP, (unsigned long long)g_endP);
    for (size_t i = 0; i < g_states.size(); i++) {
        std::fprintf(f, "%s\n  {\"i\": %zu, \"value\": \"%s\", \"info\": \"%s\", \"set_present\": %llu, \"win_from\": %llu, \"win_to\": %llu, \"dev_gpu_busy_ms\": %.4f, \"dev_passes\": {", i ? "," : "", i, g_states[i].value.c_str(), g_states[i].info.c_str(),
                     (unsigned long long)g_states[i].setP, (unsigned long long)g_states[i].winP0, (unsigned long long)g_states[i].winP1, g_states[i].gpuBusy);
        for (size_t k = 0; k < g_states[i].passes.size(); k++) std::fprintf(f, "%s\"%s\": %.4f", k ? ", " : "", g_states[i].passes[k].first.c_str(), g_states[i].passes[k].second);
        std::fprintf(f, "}}");
    }
    std::fprintf(f, "\n ],\n \"dumps\": [");
    for (size_t i = 0; i < g_reqs.size(); i++)
        std::fprintf(f, "%s\n  {\"n\": %zu, \"kind\": \"%s\", \"state\": %d, \"value\": \"%s\", \"request_present\": %llu, \"photo_flag\": %d}", i ? "," : "", i, g_reqs[i].kind.c_str(), g_reqs[i].state,
                     g_reqs[i].value.c_str(), (unsigned long long)g_reqs[i].present, g_reqs[i].photoFlag);
    std::fprintf(f, "\n ],\n \"photo_frames_seen\": [");
    for (size_t i = 0; i < g_photoSeen.size(); i++) std::fprintf(f, "%s[%llu,%d]", i ? "," : "", (unsigned long long)g_photoSeen[i].first, g_photoSeen[i].second);
    std::fprintf(f, "],\n \"photo_runs\": [");
    for (size_t i = 0; i < g_photoRuns.size(); i++)
        std::fprintf(f, "%s\n  {\"value\": \"%s\", \"photo_presents\": %d, \"dumps\": %d, \"time_running\": %d, \"film_empty\": %d, \"effect_draws\": %llu, \"effect_draws_on_photo_frames\": %llu, \"photo_passes\": %llu}", i ? "," : "",
                     g_photoRuns[i].value.c_str(), g_photoRuns[i].photoPresents, g_photoRuns[i].dumps, g_photoRuns[i].timeRunning ? 1 : 0, g_photoRuns[i].filmEmpty ? 1 : 0, (unsigned long long)g_photoRuns[i].ran, (unsigned long long)g_photoRuns[i].ranOnPhoto, (unsigned long long)g_photoRuns[i].passes);
    std::fprintf(f, "\n ]\n}\n");
    std::fclose(f);
}

void finish(int code, const char* status) {
    snap_input_force_buttons("");
    if (g_froze) os_time_pause(0);
    snaplight::ab_force().store(-1);
    g_endP = pc();
    write_result(status);
    say("done (%s), exiting with %d", status, code);
    std::fflush(stderr); std::fflush(stdout);
    std::_Exit(code);
}

void worker() {
    const double t0 = wall();
    // 1. wait for the moment
    while (pc() < 30) Sleep(50);
    int held = 0; uint64_t lastP = 0; char why[160] = "";
    for (;;) {
        Sleep(2);
        const uint64_t p = pc();
        if (p == lastP) continue;
        lastP = p;
        if (snap_route_cond(g_plan.moment.c_str(), why, sizeof why)) { if (++held >= g_plan.hold) break; } else held = 0;
        if (wall() - t0 > g_plan.timeout) { say("moment '%s' not reached in %d s (%s)", g_plan.moment.c_str(), g_plan.timeout, why); finish(3, "moment-timeout"); }
    }
    g_momentP = pc();
    say("moment '%s' met (%s)", g_plan.moment.c_str(), why);
    // 2. freeze guest time; the renderer keeps presenting the same frame state
    os_time_pause(1); g_froze = true; g_freezeP = pc();
    say("guest time frozen");
    if (!wait_presents(8)) finish(4, "stall");
    // 3. each value: set, settle, timing window, one frame
    for (size_t i = 0; i < g_plan.values.size(); i++) {
        State st; st.value = g_plan.values[i];
        apply(st.value); st.setP = pc(); st.info = g_info;
        say("state %zu: %s = %s", i, g_plan.effect.c_str(), st.value.c_str());
        if (!wait_presents((uint64_t)g_plan.settle)) finish(4, "stall");
        st.winP0 = pc();
        if (!wait_presents((uint64_t)g_plan.window)) finish(4, "stall");
        st.winP1 = pc();
        if (dev_on() && dev_timing_wanted()) {   // the named pass times are 1 s averages: let a full second of the held frame pass
            const double tw = wall();
            while (wall() - tw < 1.4) Sleep(10);
            st.gpuBusy = dev_gpu_busy();
            DevPassStat ps[64]; const int n = dev_pass_stats(ps, 64);
            for (int k = 0; k < n; k++) st.passes.push_back({ps[k].name, ps[k].ms});
        }
        g_states.push_back(st);
        if (!dump("scene", (int)i, st.value)) finish(5, "dump-failed");
        if (!wait_presents(2)) finish(4, "stall");
    }
    // 4. photo frames: the shutter with each value (after every scene frame was taken: the photo flow draws UI over the scene)
    if (g_plan.photo) {
        for (size_t i = 0; i < g_plan.values.size(); i++) {
            apply(g_plan.values[i]);
            say("photo %zu: %s = %s, shutter", i, g_plan.effect.c_str(), g_plan.values[i].c_str());
            if (!wait_presents((uint64_t)g_plan.settle / 2 + 2)) finish(4, "stall");
            PhotoRun pr; pr.value = g_plan.values[i];
            pr.filmEmpty = snap_route_cond("film<=0", nullptr, 0) != 0;   // the shutter does nothing with no film: the route's A taps may have used it up before the moment
            const uint64_t ran0 = nvn::FxRanCount(), photoRan0 = nvn::FxRanOnPhotoCount(), pass0 = nvn::FxPhotoPassCount();
            // the shutter with time frozen first (identical scene for every value: the photo hashes compare); the game's photo flow needs game time,
            // so when no photo frame shows up, once more with time running (the scenes then differ: only the gating counters mean something)
            for (int attempt = 0; attempt < 2 && pr.photoPresents == 0 && nvn::FxPhotoPassCount() == pass0; attempt++) {   // a photo pass: held presents, or (ride photo, effects stay on) the pass count
                const bool run = attempt == 1;
                if (run) { os_time_pause(0); g_froze = false; wait_presents(4); }
                char fw[96] = ""; snap_route_cond("film<=99999", fw, sizeof fw);
                snap_input_force_buttons("A");
                const uint64_t p0 = pc(); uint64_t lastSeen = 0; int dumps = 0, rounds = 0;
                const uint64_t span = run ? 90 : 40;
                while (pc() < p0 + span && dumps < (run ? 1 : 4) && rounds < 20000) {   // dump the first presents the Fx plumbing marks as photo frames
                    Sleep(1); rounds++;
                    const uint64_t p = pc();
                    if (p == lastSeen) continue;
                    lastSeen = p;
                    const int fp = nvn::FxIsPhotoFrame() ? 1 : 0;
                    g_photoSeen.push_back({p, fp});
                    if (fp) pr.photoPresents++;
                    if (pc() >= p0 + (run ? 4 : 8)) snap_input_force_buttons("");
                    if (fp && !run && nvn::g_fdBurst == 0) { dump("photo", (int)i, g_plan.values[i], fp, false); dumps++; }
                }
                snap_input_force_buttons("");
                pr.timeRunning = run; if (!run) pr.dumps = dumps;
                char fa[96] = ""; snap_route_cond("film<=99999", fa, sizeof fa);
                say("photo %zu (%s): %d photo presents, %d dumps; %s -> %s", i, run ? "time running" : "time frozen", pr.photoPresents, dumps, fw, fa);
                if (run) { wait_presents(10); os_time_pause(1); g_froze = true; }
            }
            pr.ran = nvn::FxRanCount() - ran0; pr.ranOnPhoto = nvn::FxRanOnPhotoCount() - photoRan0; pr.passes = nvn::FxPhotoPassCount() - pass0;
            say("photo %zu: %llu photo passes; effect passes drew %llu times, %llu of them on held frames", i, (unsigned long long)pr.passes, (unsigned long long)pr.ran, (unsigned long long)pr.ranOnPhoto);
            g_photoRuns.push_back(pr);
            if (!wait_presents(30)) finish(4, "stall");
        }
    }
    // 5. unfreeze, run on, exit
    snaplight::ab_force().store(-1);
    os_time_pause(0); g_froze = false;
    say("guest time running again");
    wait_presents(30, 20);
    finish(0, "ok");
}

struct Init {
    Init() {
        const char* e = std::getenv("SNAP_ABCAP");
        if (!e || !*e) return;
        if (!load_plan(e)) { std::fprintf(stderr, "[abcap] bad plan file %s\n", e); return; }
        std::fprintf(stderr, "[abcap] plan: moment '%s', effect %s, %zu values, settle %d window %d photo %d\n", g_plan.moment.c_str(), g_plan.effect.c_str(), g_plan.values.size(), g_plan.settle, g_plan.window, g_plan.photo);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { worker(); return 0; }, nullptr, 0, nullptr);
    }
} g_init;

}  // namespace
