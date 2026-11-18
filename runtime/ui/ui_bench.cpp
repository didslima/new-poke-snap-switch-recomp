// SPDX-License-Identifier: MIT
// "Benchmark" tab of the F1 overlay: starts the built-in ride benchmark (host/benchmark.cpp). The benchmark runs in its own copy of the game on a
// scratch save profile, so this game closes first (the player's save is committed atomically; unsaved progress is lost). The tab also shows the
// newest saved result.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ui_host.h"
#include "ui_internal.h"
#include "ui_i18n.h"
#include "ui_theme.h"

namespace snap::ui {
namespace {

namespace fs = std::filesystem;

struct Last {
  double read_at = -100;
  bool have = false;
  std::string date, file;
  double avg = 0, p1 = 0, p01 = 0, dur = 0;
};
Last g_last;
std::string g_msg;

fs::path results_dir() {
  if (const char* e = std::getenv("SNAP_BENCH_DIR")) if (*e) return fs::u8path(e);
  if (const char* pm = std::getenv("SNAP_PORTABLE"); pm && pm[0] == '1')   // portable or --data-dir: the same folder host/benchmark.cpp writes to
    if (const char* a = std::getenv("APPDATA"); a && *a) return fs::u8path(a) / "SnapRecomp" / "benchmarks";
  const std::string docs = host::documents_dir();
  if (docs.empty()) return {};
  return fs::u8path(docs) / "Snap Recomp" / "benchmarks";
}

double field(const std::string& t, const char* key) {
  const size_t p = t.find(std::string("\"") + key + "\":");
  return p == std::string::npos ? 0.0 : std::atof(t.c_str() + p + std::strlen(key) + 3);
}

void refresh() {
  if (ImGui::GetTime() - g_last.read_at < 3.0) return;
  g_last.read_at = ImGui::GetTime();
  g_last.have = false;
  std::error_code ec;
  fs::path best;
  for (auto& e : fs::directory_iterator(results_dir(), ec)) {
    const std::string n = e.path().filename().string();
    if (e.path().extension() == ".json" && n.rfind("benchmark-", 0) == 0 && (best.empty() || n > best.filename().string())) best = e.path();
  }
  if (best.empty()) return;
  std::ifstream f(best, std::ios::binary);
  std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t cut = t.find("\"frame_ms\"");
  if (cut != std::string::npos) t.resize(cut);   // the header only; the frame list can be long
  g_last.avg = field(t, "avg_fps");
  g_last.p1 = field(t, "low_1pct_fps");
  g_last.p01 = field(t, "low_0_1pct_fps");
  g_last.dur = field(t, "duration_s");
  const size_t d = t.find("\"date\": \"");
  g_last.date = d == std::string::npos ? "" : t.substr(d + 9, t.find('"', d + 9) - d - 9);
  g_last.file = best.filename().string();
  g_last.have = g_last.avg > 0;
}

}  // namespace

void tab_benchmark() {
  const Theme& t = theme();
  ImGui::TextWrapped(tr("Plays one scripted ride through Florio Nature Park (about two minutes) in a second copy of the game and then shows the average "
                     "frame rate, the 1%% and 0.1%% lows, a frame-time graph and the GPU time of the slowest passes. It uses a scratch save, so your "
                     "own save is never touched. It runs with vsync and the frame limit off and the sound muted; your picture settings are used."));
  ImGui::Spacing();
  ImGui::TextColored(t.text_dim, "%s", tr("This game closes first; unsaved progress is lost. Run it twice on the same settings to see how steady the numbers are."));
  ImGui::Spacing();
  if (hold_confirm("Hold to start the benchmark", 1.0f, t.good)) {
    if (!host::benchmark_launch_from_game()) g_msg = "The benchmark could not be started.";
  }
  if (!g_msg.empty()) ImGui::TextColored(t.error, "%s", tr(g_msg));
  ImGui::Spacing();
  ImGui::Separator();
  refresh();
  if (g_last.have) {
    ImGui::Text(tr("Last result (%s)"), g_last.date.c_str());
    ImGui::Text(tr("Average %.1f fps   1%% low %.1f   0.1%% low %.1f   (%.0f s)"), g_last.avg, g_last.p1, g_last.p01, g_last.dur);
    ImGui::TextColored(t.text_dim, "%s", tr("Saved in Documents\\Snap Recomp\\benchmarks. Start the game with --benchmark-view <file>.json to see the full screen again."));
  } else {
    ImGui::TextColored(t.text_dim, "%s", tr("No saved result yet. Results are kept in Documents\\Snap Recomp\\benchmarks."));
  }
}

}  // namespace snap::ui
