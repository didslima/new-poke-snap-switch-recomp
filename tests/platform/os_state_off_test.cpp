// SPDX-License-Identifier: MIT
// Save states are off on Linux (runtime/os/linux/os_state_off.cpp). This test links that file with the real OS layer and checks what the rest
// of the program can observe: the drain phase never leaves 0, no checkpoint thread is started, the snap_state_* entry points refuse and say
// so, and the two scheduler gates are tripwires, not no-ops.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>

#include "os/os.h"
#include "os/os_internal.h"

extern "C" void rt_dispatch(Cpu *c) { ((GuestFn)(uintptr_t)c->pc)(c); }
extern "C" {
struct SnapStateInfo {
    int busy, has_state, last_op, last_ok, saves, loads, done;
    unsigned long long last_done_ms, saved_ms;
    char last_msg[112];
};
void snap_state_request(int op, const char *name);
int snap_state_idle();
void snap_state_info(SnapStateInfo *out);
int snap_state_count(const char *kind);
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int thread_count() {
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    while (dirent *e = readdir(d)) if (atoi(e->d_name) > 0) n++;
    closedir(d);
    return n;
}

// runs fn in a child with stderr captured; returns the terminating signal (or -1000 - exit status) and the text in *err
static int run_child(void (*fn)(), std::string *err) {
    int fds[2];
    if (pipe(fds)) return -1;
    fflush(nullptr);
    pid_t p = fork();
    if (p == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    char b[512];
    ssize_t n;
    while ((n = read(fds[0], b, sizeof b)) > 0) err->append(b, (size_t)n);
    close(fds[0]);
    int st = 0;
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : -1000 - WEXITSTATUS(st);
}
static void child_gate_acq() {
    osk::Lock lk(osk::G);
    osk::state_gate_acq(nullptr, lk);
}
static void child_gate_wait() {
    osk::Lock lk(osk::G);
    osk::state_gate_wait(nullptr, lk);
}

int main() {
    // ask for the checkpoint driver and the test driver: they must be refused, not started
    setenv("SNAP_CHECKPOINT", "/nonexistent/slot", 1);
    setenv("SNAP_STATE_TEST", "census@t1", 1);
    const int threads_before = thread_count();
    int saved_err = dup(2), pfd[2];
    CHECK(pipe(pfd) == 0);
    dup2(pfd[1], 2);
    OsConfig cfg{};
    os_init(&cfg);
    fflush(stderr);
    dup2(saved_err, 2);
    close(pfd[1]);
    char buf[1024] = {};
    const ssize_t n = read(pfd[0], buf, sizeof buf - 1);
    (void)n;
    CHECK(std::strstr(buf, "ignored") != nullptr && std::strstr(buf, "not supported on Linux") != nullptr);
    CHECK(thread_count() == threads_before);   // no driver thread
    usleep(1500000);                           // the Windows drivers start after t seconds: nothing may appear
    CHECK(thread_count() == threads_before);

    CHECK(osk::g_sphase.load() == 0);

    SnapStateInfo si;
    snap_state_info(&si);
    CHECK(!si.busy && !si.has_state && si.saves == 0 && si.loads == 0 && si.done == 0 && si.last_op == 0);
    CHECK(snap_state_idle() == 1);
    CHECK(snap_state_count("saves") == 0 && snap_state_count("loads") == 0 && snap_state_count("done") == 0);

    // a save request is refused, visibly
    snap_state_request(1, "player");
    snap_state_info(&si);
    CHECK(si.last_op == 1 && si.last_ok == 0 && std::strstr(si.last_msg, "not supported on Linux") != nullptr);
    CHECK(!si.has_state && !si.busy && si.saves == 0 && si.done == 0);   // nothing was saved, nothing is pending
    snap_state_request(2, "player");
    snap_state_info(&si);
    CHECK(si.last_op == 2 && si.last_ok == 0 && !si.has_state);
    CHECK(snap_state_idle() == 1 && snap_state_count("loads") == 0);
    CHECK(osk::g_sphase.load() == 0);          // a request does not move the phase either

    // the gates are tripwires
    std::string err;
    CHECK(run_child(child_gate_acq, &err) == SIGABRT);
    CHECK(err.find("state_gate_acq reached on Linux") != std::string::npos);
    err.clear();
    CHECK(run_child(child_gate_wait, &err) == SIGABRT);
    CHECK(err.find("state_gate_wait reached on Linux") != std::string::npos);

    if (g_fail) { std::fprintf(stderr, "os_state_off_test: %d check(s) failed\n", g_fail); return 1; }
    std::printf("os_state_off_test: ok\n");
    return 0;
}
