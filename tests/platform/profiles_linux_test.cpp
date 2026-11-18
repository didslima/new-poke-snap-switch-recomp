// SPDX-License-Identifier: MIT
// Stage 6C-C2: isolated unit tests for POSIX Profiles & Configuration (profiles.cpp + hle_host_posix.cpp).
// Uses exclusively isolated temporary directories under /tmp; never touches real saves, ROMFS, or user config.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/hle_host.h"
#include "hle/profiles.h"
#include "hle/save_gen.h"
#include "hle/settings.h"

namespace fs = std::filesystem;

extern "C" void os_get_stats(void*) {}
extern "C" int snap_ui_pick_profile(const hle::profiles::PickItem*, int);

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(cond) do { g_checks++; if (!(cond)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/snap_profiles_test_XXXXXX";
        char* p = mkdtemp(tmpl);
        if (p) path = p;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

struct EnvGuard {
    std::string name;
    bool had = false;
    std::string old;
    explicit EnvGuard(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) {
            had = true;
            old = v;
        }
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), old.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

static void write_synthetic_save(const fs::path& save_root, const std::string& payload) {
    const fs::path uid_dir = save_root / "534e4150524543310000000000000001";
    const fs::path gen1 = uid_dir / "gens" / "1";
    fs::create_directories(gen1);
    {
        std::ofstream df(gen1 / "save.bin", std::ios::binary);
        df << payload;
    }
    {
        std::ofstream okf(uid_dir / "gens" / "1.ok", std::ios::binary);
        okf << "ok\n";
    }
    {
        std::ofstream cur(uid_dir / "CURRENT", std::ios::binary);
        cur << hle::savegen::pointer_text(1);
    }
}

static void test_store_persistence_and_recovery() {
    TempDir tmp;
    CHECK(!tmp.path.empty());
    const fs::path root = tmp.path / "profiles";

    std::string id_a, id_b, err;
    {
        hle::profiles::Store s(root.string());
        CHECK(s.load());
        CHECK(s.list().empty());

        id_a = s.create("Alpha", &err);
        CHECK(!id_a.empty() && err.empty());
        id_b = s.create("Beta", &err);
        CHECK(!id_b.empty() && err.empty());
        CHECK(s.list().size() == 2);

        CHECK(s.rename(id_a, "Alpha Renamed", &err));
        CHECK(fs::exists(root / "profiles.json"));
        CHECK(!fs::exists(root / "profiles.json.tmp"));
    }

    // Reopen Store(root) and verify persistence + remove + restore.
    {
        hle::profiles::Store s(root.string());
        CHECK(s.load());
        CHECK(s.list().size() == 2);
        const auto* pa = s.find("Alpha Renamed");
        CHECK(pa != nullptr && pa->id == id_a);
        CHECK(s.find(id_b) != nullptr);

        CHECK(s.remove(id_b, &err));
        CHECK(s.list().size() == 1);
        CHECK(s.find(id_b) == nullptr);

        const auto trash = s.trash_list();
        CHECK(trash.size() == 1);
        const std::string restored_id = s.restore(trash[0].folder, &err);
        CHECK(!restored_id.empty());
        CHECK(s.list().size() == 2);
    }

    // Corrupted profiles.json -> moved aside to profiles.json.invalid and rebuilt from folders.
    {
        std::ofstream bad(root / "profiles.json", std::ios::binary | std::ios::trunc);
        bad << "{broken json";
    }
    {
        hle::profiles::Store s(root.string());
        CHECK(!s.load());
        CHECK(!s.message().empty());
        CHECK(fs::exists(root / "profiles.json.invalid"));
        CHECK(s.list().size() == 2);
    }
}

static void test_startup_and_selection() {
    TempDir tmp;
    const std::string root = (tmp.path / "profiles").string();

    // 1. Empty root creates "Profile 1".
    auto a1 = hle::profiles::startup(root, "", "", nullptr, false);
    CHECK(a1.on);
    CHECK(a1.name == "Profile 1");
    const std::string p1_id = a1.id;

    // 2. Selecting non-existent "want" creates it and stamps last_used.
    auto a2 = hle::profiles::startup(root, "", "Custom", nullptr, false);
    CHECK(a2.on);
    CHECK(a2.name == "Custom");
    CHECK(a2.id != p1_id);

    // 3. Empty want selects last_used ("Custom").
    auto a3 = hle::profiles::startup(root, "", "", nullptr, false);
    CHECK(a3.on && a3.id == a2.id);

    // 4. Weak snap_ui_pick_profile returns -1 and falls back to last_used.
    CHECK(snap_ui_pick_profile(nullptr, 0) == -1);
    auto a4 = hle::profiles::startup(root, "", "", snap_ui_pick_profile, true);
    CHECK(a4.on && a4.id == a2.id);

    // 5. Explicit picker selecting index 0 picks Profile 1.
    auto pick_first = [](const hle::profiles::PickItem*, int count) -> int { return count > 0 ? 0 : -1; };
    auto a5 = hle::profiles::startup(root, "", "", pick_first, true);
    CHECK(a5.on && a5.id == p1_id);
}

static void test_migration_and_trash_recovery() {
    TempDir tmp;
    const fs::path root = tmp.path / "profiles";
    const fs::path legacy = tmp.path / "legacy_save";
    write_synthetic_save(legacy, "SNAP_SYNTH_PAYLOAD_V1");

    // 1. Legacy migration into Profile 1 preserves save data and renames legacy folder.
    auto a = hle::profiles::startup(root.string(), legacy.string(), "", nullptr, false);
    CHECK(a.on && a.name == "Profile 1");
    CHECK(!a.note.empty());
    const fs::path migrated_file = fs::path(a.save_dir) / "534e4150524543310000000000000001" / "gens" / "1" / "save.bin";
    CHECK(fs::exists(migrated_file));
    {
        std::ifstream f(migrated_file, std::ios::binary);
        std::string content;
        std::getline(f, content);
        CHECK(content == "SNAP_SYNTH_PAYLOAD_V1");
    }
    CHECK(!fs::exists(legacy));

    // 2. Empty legacy source is ignored by startup(); incomplete folder is rejected by import_folder().
    const fs::path empty_legacy = tmp.path / "empty_legacy";
    fs::create_directories(empty_legacy);
    const fs::path profiles2 = tmp.path / "profiles2";
    auto a_empty = hle::profiles::startup(profiles2.string(), empty_legacy.string(), "", nullptr, false);
    CHECK(a_empty.on && a_empty.note.empty());
    CHECK(fs::exists(empty_legacy));

    const fs::path bad_legacy = tmp.path / "incomplete_legacy";
    fs::create_directories(bad_legacy / "534e4150524543310000000000000001");
    hle::profiles::Store s2((tmp.path / "profiles3").string());
    std::string err;
    CHECK(s2.import_folder(bad_legacy.string(), "Bad", &err).empty());
    CHECK(!err.empty());

    // 3. Interrupted (.copying) vs completed pending trash entries in _trash/.
    const fs::path trash = root / "_trash";
    const fs::path interrupted = trash / ".pending-Aborted-111";
    fs::create_directories(interrupted / "save");
    { std::ofstream f(interrupted / ".copying"); f << "copy in progress\n"; }

    const fs::path completed = trash / ".pending-Recovered-222";
    fs::create_directories(completed / "save");
    { std::ofstream sf(completed / "save" / "data.bin"); sf << "ok"; }
    {
        std::ofstream f(completed / "meta.json");
        f << R"({"name":"Recovered","kind":"delete","when":1700000000,"profile_id":"p999"})";
    }

    hle::profiles::Store s(root.string());
    CHECK(s.load());
    s.prune_trash();
    CHECK(!fs::exists(interrupted));
    CHECK(!fs::exists(completed));
    const auto tlist = s.trash_list();
    CHECK(tlist.size() == 1 && tlist[0].name == "Recovered");
}

static void test_locks_and_concurrency() {
    TempDir tmp;
    const fs::path root = tmp.path / "profiles";
    hle::profiles::Store s(root.string());
    CHECK(s.load());
    std::string err;
    const std::string id = s.create("LockedProfile", &err);
    CHECK(!id.empty());
    const fs::path save_dir = fs::path(s.save_dir(id));
    write_synthetic_save(save_dir, "LOCK_TEST_SAVE");
    const std::string lock_file = (save_dir / ".snap.lock").string();

    // 1. No .snap.lock -> in_use() false and does not create .snap.lock.
    CHECK(!fs::exists(lock_file));
    CHECK(!s.in_use(id));
    CHECK(!fs::exists(lock_file));

    // 2. Cross-process lock via fork(): while child holds lock, in_use() is true and remove() refuses.
    int p_ready[2] = {-1, -1}, p_release[2] = {-1, -1};
    CHECK(pipe(p_ready) == 0 && pipe(p_release) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(p_ready[0]);
        close(p_release[1]);
        void* lk = nullptr;
        int rc = hle::host::try_acquire_save_lock(lock_file, &lk);
        char b = (rc == 0) ? '1' : '0';
        (void)!write(p_ready[1], &b, 1);
        char go = 0;
        (void)!read(p_release[0], &go, 1);
        hle::host::release_save_lock(lk);
        _exit(rc == 0 ? 0 : 1);
    }
    close(p_ready[1]);
    close(p_release[0]);
    char ready = 0;
    CHECK(read(p_ready[0], &ready, 1) == 1 && ready == '1');
    close(p_ready[0]);

    CHECK(s.in_use(id));
    CHECK(!s.remove(id, &err));
    CHECK(!err.empty());
    CHECK(fs::exists(save_dir));

    // Release child lock and wait.
    CHECK(write(p_release[1], "x", 1) == 1);
    close(p_release[1]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    CHECK(!s.in_use(id));
    CHECK(s.remove(id, &err));
    CHECK(!fs::exists(save_dir));

    // 3. Process death without explicit release still drops flock cleanly.
    const std::string id2 = s.create("CrashDrop", &err);
    const fs::path save_dir2 = fs::path(s.save_dir(id2));
    fs::create_directories(save_dir2);
    const std::string lock_file2 = (save_dir2 / ".snap.lock").string();
    pid_t cpid = fork();
    CHECK(cpid >= 0);
    if (cpid == 0) {
        void* lk = nullptr;
        (void)hle::host::try_acquire_save_lock(lock_file2, &lk);
        _exit(0);  // exit without calling release_save_lock
    }
    CHECK(waitpid(cpid, &status, 0) == cpid);
    CHECK(!s.in_use(id2));

    // 4. PendingLock cross-process exclusivity.
    const std::string pend_lock = (tmp.path / "pend.lock").string();
    int pr1[2] = {-1, -1}, pr2[2] = {-1, -1};
    CHECK(pipe(pr1) == 0 && pipe(pr2) == 0);
    pid_t ppid = fork();
    CHECK(ppid >= 0);
    if (ppid == 0) {
        close(pr1[0]);
        close(pr2[1]);
        void* pl = hle::host::pending_lock_begin(pend_lock);
        char b = pl ? '1' : '0';
        (void)!write(pr1[1], &b, 1);
        char go = 0;
        (void)!read(pr2[0], &go, 1);
        hle::host::pending_lock_end(pl, pend_lock);
        _exit(pl ? 0 : 1);
    }
    close(pr1[1]);
    close(pr2[0]);
    char pb = 0;
    CHECK(read(pr1[0], &pb, 1) == 1 && pb == '1');
    close(pr1[0]);
    CHECK(hle::host::pending_lock_begin(pend_lock) == nullptr);
    CHECK(write(pr2[1], "x", 1) == 1);
    close(pr2[1]);
    CHECK(waitpid(ppid, &status, 0) == ppid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    void* pl_parent = hle::host::pending_lock_begin(pend_lock);
    CHECK(pl_parent != nullptr);
    hle::host::pending_lock_end(pl_parent, pend_lock);
    CHECK(!fs::exists(pend_lock));

    // 5. Non-lock I/O error returns rc == 2 (distinct from lock busy rc == 1).
    void* bad_lk = nullptr;
    CHECK(hle::host::try_acquire_save_lock((tmp.path / "missing_dir" / ".snap.lock").string(), &bad_lk) == 2);
    CHECK(bad_lk == nullptr);

    // 6. wait_process_exit() waits for an external PID.
    pid_t wpid = fork();
    CHECK(wpid >= 0);
    if (wpid == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        _exit(0);
    }
    // Wait up to 2s while child sleeps 40ms; reap zombie afterwards.
    hle::host::wait_process_exit(static_cast<uint64_t>(wpid), 100);
    CHECK(waitpid(wpid, &status, 0) == wpid);
}

static void test_settings_and_xdg_integration() {
    EnvGuard g_home("HOME"), g_xcfg("XDG_CONFIG_HOME"), g_xdata("XDG_DATA_HOME");
    EnvGuard g_hcfg("SNAP_HLE_CONFIG"), g_pdir("SNAP_PROFILES_DIR");
    EnvGuard g_sdir("SNAP_SAVE_DIR"), g_sroot("SNAP_SAVE_ROOT");
    EnvGuard g_leg("SNAP_LEGACY_SAVE_DIR"), g_prof("SNAP_PROFILE"), g_pick("SNAP_PROFILE_PICKER");

    TempDir tmp;
    const fs::path home = tmp.path / "home";
    const fs::path xcfg = tmp.path / "xdg_cfg";
    const fs::path xdata = tmp.path / "xdg_data";
    const fs::path pdir = tmp.path / "custom_profiles";
    fs::create_directories(home);
    fs::create_directories(xcfg);
    fs::create_directories(xdata);

    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", xcfg.c_str(), 1);
    setenv("XDG_DATA_HOME", xdata.c_str(), 1);
    unsetenv("SNAP_HLE_CONFIG");
    setenv("SNAP_PROFILES_DIR", pdir.c_str(), 1);
    unsetenv("SNAP_SAVE_DIR");
    unsetenv("SNAP_SAVE_ROOT");
    unsetenv("SNAP_LEGACY_SAVE_DIR");
    setenv("SNAP_PROFILE", "LinuxRunner", 1);
    setenv("SNAP_PROFILE_PICKER", "0", 1);

    CHECK(hle::load_config(""));
    hle::activate_profiles();

    const auto& act = hle::profiles::active();
    CHECK(act.on);
    CHECK(act.name == "LinuxRunner");
    CHECK(act.root == pdir.string());
    CHECK(hle::config().save_root == act.save_dir);
    CHECK(hle::settings::file_path() == (pdir / act.id / "settings.cfg").string());

    hle::settings::set("game_language", "es");
    CHECK(fs::exists(pdir / act.id / "settings.cfg"));
    CHECK(hle::settings::get("game_language") == "es");
}

int main() {
    test_store_persistence_and_recovery();
    test_startup_and_selection();
    test_migration_and_trash_recovery();
    test_locks_and_concurrency();
    test_settings_and_xdg_integration();
    std::printf("profiles_linux_test: %d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
