// Player command-line flags (host/cli_flags.h). Build and run: tests\hle\run_cli_flags_test.ps1
#include "../../host/cli_flags.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

using V = std::vector<std::string>;

static std::string env_of(const snap_cli::Parsed& p, const char* name) {
  std::string v = "<unset>";
  for (auto& kv : p.env) if (kv.first == name) v = kv.second;   // the last one wins, the same as the _putenv_s loop in boot.cpp
  return v;
}

int main() {
  {   // nothing: the launcher
    auto p = snap_cli::parse({});
    CHECK(p.error.empty() && p.rest.empty() && !p.play && !p.help && !p.version && !p.safe && p.env.empty() && p.sets.empty());
  }
  {   // every player flag at once leaves nothing for the older handlers, so launcher mode still runs
    auto p = snap_cli::parse({"--play", "--fullscreen", "--monitor", "1", "--width", "2560", "--height", "1440", "--profile", "Ash",
                              "--set", "fps_target=120", "--set", "SNAP_VOL_MASTER=0", "--data-dir", "D:\\snap data", "--safe-mode"});
    CHECK(p.error.empty());
    CHECK(p.rest.empty());
    CHECK(p.play && p.safe);
    CHECK(env_of(p, "SNAP_WINDOW_MODE") == "borderless");
    CHECK(env_of(p, "SNAP_WINDOW_MONITOR") == "1");
    CHECK(env_of(p, "SNAP_WINDOW_W") == "2560");
    CHECK(env_of(p, "SNAP_WINDOW_H") == "1440");
    CHECK(env_of(p, "SNAP_VOL_MASTER") == "0");
    CHECK(p.profile == "Ash");
    CHECK(p.data_dir == "D:\\snap data");
    CHECK(p.sets.size() == 1 && p.sets[0].first == "fps_target" && p.sets[0].second == "120");
  }
  {   // --steam-deck: 1280 x 800 borderless, the handheld rows; flags after it win
    auto p = snap_cli::parse({"--play", "--steam-deck", "--set", "hud_scale=200", "--width", "1920"});
    CHECK(p.error.empty() && p.rest.empty() && p.play);
    CHECK(env_of(p, "SNAP_WINDOW_MODE") == "borderless");
    CHECK(env_of(p, "SNAP_WINDOW_W") == "1920" && env_of(p, "SNAP_WINDOW_H") == "800");
    std::string hud, icons, scale;
    for (auto& kv : p.sets) { if (kv.first == "hud_scale") hud = kv.second; if (kv.first == "button_icons") icons = kv.second; if (kv.first == "gfx_render_scale") scale = kv.second; }
    CHECK(hud == "200" && icons == "xbox" && scale == "0.75");   // the last hud_scale (the player's own) is the one boot.cpp leaves in force
    auto q = snap_cli::parse({"--steam-deck"});
    CHECK(q.error.empty() && env_of(q, "SNAP_WINDOW_W") == "1280" && env_of(q, "SNAP_WINDOW_H") == "800" && q.sets.size() == snapdeck::cfg().size());
  }
  {   // window mode: the last flag wins; --borderless is the same as --fullscreen
    auto p = snap_cli::parse({"--fullscreen", "--windowed"});
    CHECK(env_of(p, "SNAP_WINDOW_MODE") == "windowed");
    CHECK(env_of(snap_cli::parse({"--borderless"}), "SNAP_WINDOW_MODE") == "borderless");
  }
  {   // --set: the value may hold '=' and may be empty; a missing key or '=' is an error
    auto p = snap_cli::parse({"--set", "look=warm=1", "--set", "photo_dir="});
    CHECK(p.error.empty() && p.sets.size() == 2);
    CHECK(p.sets[0].first == "look" && p.sets[0].second == "warm=1");
    CHECK(p.sets[1].first == "photo_dir" && p.sets[1].second.empty());
    CHECK(!snap_cli::parse({"--set", "fps_target"}).error.empty());
    CHECK(!snap_cli::parse({"--set", "=120"}).error.empty());
    CHECK(!snap_cli::parse({"--set"}).error.empty());
  }
  {   // values that are missing or not numbers
    CHECK(!snap_cli::parse({"--width"}).error.empty());
    CHECK(!snap_cli::parse({"--width", "wide"}).error.empty());
    CHECK(!snap_cli::parse({"--monitor", "-1"}).error.empty());
    CHECK(!snap_cli::parse({"--profile"}).error.empty());
    CHECK(!snap_cli::parse({"--data-dir"}).error.empty());
    CHECK(!snap_cli::parse({"--data-dir", ""}).error.empty());
  }
  {   // help and version
    CHECK(snap_cli::parse({"--help"}).help && snap_cli::parse({"-h"}).help && snap_cli::parse({"/?"}).help);
    CHECK(snap_cli::parse({"--version"}).version);
    CHECK(std::strstr(snap_cli::help_text(), "--play") && std::strstr(snap_cli::help_text(), "--data-dir") && std::strstr(snap_cli::help_text(), "--set"));
  }
  {   // older commands pass through untouched and in order, with player flags taken out from between them
    auto p = snap_cli::parse({"--import-save", "C:\\s.zip", "--profile", "Ash", "--data-dir", "D:\\d"});
    CHECK(p.error.empty() && p.rest == V({"--import-save", "C:\\s.zip"}) && p.profile == "Ash" && p.data_dir == "D:\\d");
    p = snap_cli::parse({"--main", "a.elf", "--sdk", "b.elf", "--set", "vsync=0", "run", "--timeout", "60"});
    CHECK(p.rest == V({"--main", "a.elf", "--sdk", "b.elf", "run", "--timeout", "60"}));
    CHECK(snap_cli::first_unknown(p.rest).empty());
    p = snap_cli::parse({"--benchmark", "--benchmark-uncapped", "--benchmark-view", "r.json"});
    CHECK(p.rest.size() == 4 && snap_cli::first_unknown(p.rest).empty());
    p = snap_cli::parse({"--prepare-shaders", "--force", "--visible", "--list", "--stop", "--courses", "florio,beach"});
    CHECK(p.rest.size() == 7 && snap_cli::first_unknown(p.rest).empty());
    CHECK(snap_cli::first_unknown({"pads", "5"}).empty());
  }
  {   // a typo is named instead of falling through to the developer usage line
    CHECK(snap_cli::first_unknown(snap_cli::parse({"--fullscren"}).rest) == "--fullscren");
    CHECK(snap_cli::first_unknown(snap_cli::parse({"--play", "--nope", "--windowed"}).rest) == "--nope");
    CHECK(snap_cli::first_unknown({"--trace", "--looks-like-a-flag"}).empty());   // the value of an option that takes one is not judged
  }
  {   // the version line reads config.json text
    CHECK(snap_cli::version_from_config("{\"romfs_root\": \"x\", \"builder_version\": \"1.3.0\", \"work\": \"w\"}") == "1.3.0");
    CHECK(snap_cli::version_from_config("{\"builder_version\":\"2.0\"}") == "2.0");
    CHECK(snap_cli::version_from_config("").empty());
    CHECK(snap_cli::version_from_config("{\"builder_version\": 3}").empty());
    CHECK(snap_cli::version_from_config("{\"work\": \"w\"}").empty());
  }
  {   // launcher actions: one at a time, never with --play or another command; the other player flags still apply
    for (const std::string& n : snap_cli::action_names()) {
      auto p = snap_cli::parse({"--" + n});
      CHECK(p.error.empty() && p.action == n && p.rest.empty());
      CHECK(std::strstr(snap_cli::help_text(), ("--" + n).c_str()) != nullptr);
    }
    auto p = snap_cli::parse({"--data-dir", "D:\\Snap", "--check-install", "--profile", "Ash"});
    CHECK(p.error.empty() && p.action == "check-install" && p.data_dir == "D:\\Snap" && p.profile == "Ash");
    CHECK(!snap_cli::parse({"--update", "--report"}).error.empty());
    CHECK(!snap_cli::parse({"--update", "--play"}).error.empty());
    CHECK(!snap_cli::parse({"--benchmark", "--hdtex"}).error.empty());
    CHECK(snap_cli::parse({"--updates"}).action.empty());                       // a near miss is not an action...
    CHECK(snap_cli::first_unknown(snap_cli::parse({"--updates"}).rest) == "--updates");   // ...and is named as unknown
    CHECK(snap_cli::parse({"--play"}).action.empty());
  }
  {   // --restore-backup, --upscalers and --hdtex run without a window: what follows them is theirs, word for word
    auto p = snap_cli::parse({"--data-dir", "D:\\Snap", "--restore-backup", "latest"});
    CHECK(p.error.empty() && p.action == "restore-backup" && p.action_args == V({"latest"}) && p.data_dir == "D:\\Snap" && p.rest.empty());
    p = snap_cli::parse({"--upscalers", "fsr", "--accept-license", "fsr"});
    CHECK(p.error.empty() && p.action == "upscalers" && p.action_args == V({"fsr", "--accept-license", "fsr"}));
    p = snap_cli::parse({"--hdtex", "--level", "hd", "--set", "creature.scale=4", "--dry-run"});   // --set here is the HD tool's, not the game's
    CHECK(p.error.empty() && p.action == "hdtex" && p.action_args.size() == 5 && p.sets.empty() && p.rest.empty());
    p = snap_cli::parse({"--hdtex"});
    CHECK(p.error.empty() && p.action_args.empty());
    CHECK(!snap_cli::parse({"--play", "--restore-backup", "latest"}).error.empty());
    CHECK(!snap_cli::parse({"--check-install", "latest"}).error.empty());   // the other actions still take nothing
    CHECK(snap_cli::action_takes_args("hdtex") && !snap_cli::action_takes_args("update"));
  }
  if (g_fail) { std::printf("cli_flags_test: %d FAILED\n", g_fail); return 1; }
  std::printf("cli_flags_test: all passed\n");
  return 0;
}
