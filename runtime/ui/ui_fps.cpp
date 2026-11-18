// SPDX-License-Identifier: MIT
// Performance overlay (settings key fps_overlay -> SNAP_FPS_OVERLAY): Off (default, the original game), FPS (one number, averaged
// over 0.5 s) or Detailed (frame time, a 2.5 s frame-time graph with the target line, 1% low, CPU and GPU ms, render and output size,
// upscaler / AA, frame cap and vsync). Corner: key fps_overlay_corner (tl, tr, bl, br; default tr) -> SNAP_FPS_OVERLAY_CORNER.
// Live: the options rows and the F3 key (when F3 is not bound to a game button) call fps_overlay_set; nothing needs a restart.
//
// Drawn by the host overlay (ui_overlay.cpp) into the swapchain image after the game's frame is composed, like the shiny badges, so
// it never reaches the game's photos, album or any guest-side capture. NVN_FRAME_DUMP_POST dumps (the finished swapchain image) do
// show it; plain NVN_FRAME_DUMP dumps (the guest image before the overlay) do not.
//
// Cost when Off: one relaxed atomic load per present (fps_overlay_tick returns at once; the GPU timestamps stay off).
// CPU and GPU ms come from the SNAP_PERF timing source (runtime/nvn/nvn_perf.cpp): GPU busy = timestamp pairs around every command
// buffer, collected one present late; CPU = the presenting thread's CPU time per frame. They are switched on only in Detailed mode.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "hle/settings.h"
#include "imgui.h"
#include "ui_i18n.h"
#include "ui_internal.h"

extern "C" void snap_perf_overlay_timing(int on);   // runtime/nvn/nvn_perf.cpp
extern "C" void snap_perf_overlay_info(double* gpu_ms, double* cpu_ms, int* render_w, int* render_h, double* pace_hz, char* aa, int aa_len);
extern "C" int snap_window_frame_cap();             // host/window_opts.cpp (0 = no limit)
extern "C" const char* snap_vsync_get();            // runtime/nvn/nvn_vk.cpp

namespace {
namespace st = hle::settings;

std::atomic<int> g_mode{-1};     // -1 = not read yet, 0 off, 1 fps, 2 detailed
std::atomic<int> g_corner{-1};   // 0 tl, 1 tr, 2 bl, 3 br

constexpr int kRing = 1024;
struct Stats {
  std::mutex mu;
  uint64_t last_ns = 0;
  float dt[kRing] = {};         // frame times, ms
  double at[kRing] = {};        // when each was taken, s
  int head = 0, n = 0;
  float fps = 0, avg_ms = 0, low1 = 0, gpu = -1, cpu = -1;
  double next_avg = 0;
} S;

int parse_mode(const std::string& v) {
  if (v == "fps" || v == "1") return 1;
  if (v == "detailed" || v == "2") return 2;
  return 0;
}
int parse_corner(const std::string& v) {
  if (v == "tl") return 0;
  if (v == "bl") return 2;
  if (v == "br") return 3;
  return 1;
}
// the env var (exported from settings.cfg at start, or set by hand) wins at start, as for every other option
void read_initial() {
  if (g_mode.load() >= 0) return;
  const char* e = std::getenv("SNAP_FPS_OVERLAY");
  const int m = parse_mode(e && *e ? e : st::get("fps_overlay", "default"));
  const char* c = std::getenv("SNAP_FPS_OVERLAY_CORNER");
  g_corner.store(parse_corner(c && *c ? c : st::get("fps_overlay_corner", "tr")));
  int x = -1;
  if (g_mode.compare_exchange_strong(x, m) && m == 2) snap_perf_overlay_timing(1);
}

uint64_t steady_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
double secs(uint64_t ns) { return (double)ns / 1e9; }
}  // namespace

namespace snap::ui {

int fps_overlay_mode() { read_initial(); return g_mode.load(std::memory_order_relaxed); }
std::string fps_overlay_value() { const int m = fps_overlay_mode(); return m == 1 ? "fps" : m == 2 ? "detailed" : "default"; }
std::string fps_overlay_corner_value() { read_initial(); const char* k[4] = {"tl", "tr", "bl", "br"}; return k[std::clamp(g_corner.load(), 0, 3)]; }

// Live switch; also stores the choice ("default" = Off, the key erased)
void fps_overlay_set(const std::string& v) {
  read_initial();
  const int m = parse_mode(v);
  if (m == 0) st::erase("fps_overlay"); else st::set("fps_overlay", m == 1 ? "fps" : "detailed");
  const int old = g_mode.exchange(m);
  if (old != m) {
    snap_perf_overlay_timing(m == 2);
    { std::lock_guard<std::mutex> lk(S.mu); S.n = 0; S.last_ns = 0; S.gpu = S.cpu = -1; S.fps = 0; }
    hle::log("ui: performance overlay %s", m == 1 ? "fps" : m == 2 ? "detailed" : "off");
  }
}
void fps_overlay_set_corner(const std::string& v) {
  read_initial();
  const int c = parse_corner(v);
  if (c == 1) st::erase("fps_overlay_corner"); else st::set("fps_overlay_corner", v);
  g_corner.store(c);
}
void fps_overlay_cycle() { const int m = fps_overlay_mode(); fps_overlay_set(m == 0 ? "fps" : m == 1 ? "detailed" : "default"); }

// F3 cycles the overlay unless the player bound F3 to a game button (then F3 stays the game's)
bool fps_overlay_hotkey_free() {
  static const int k = hle::input::key_from_name("F3");
  if (!k) return false;
  const hle::input::Source s{hle::input::Source::KeyboardKey, k};
  const hle::input::Mapping m = hle::input::hub().mapping();
  for (const auto& b : m.buttons) if (std::find(b.begin(), b.end(), s) != b.end()) return false;
  for (const hle::input::StickMap* sm : {&m.left, &m.right})
    for (const auto* v : {&sm->up, &sm->down, &sm->left, &sm->right}) if (std::find(v->begin(), v->end(), s) != v->end()) return false;
  return true;
}

// Once per present (present thread), before anything is drawn. Returns true when the overlay wants a frame.
bool fps_overlay_tick() {
  if (g_mode.load(std::memory_order_relaxed) == 0) return false;
  const int mode = fps_overlay_mode();
  if (mode == 0) return false;
  const uint64_t t = steady_ns();
  std::lock_guard<std::mutex> lk(S.mu);
  const double now = secs(t);
  if (S.last_ns) {
    const float d = (float)((now - secs(S.last_ns)) * 1000.0);
    S.dt[S.head] = d; S.at[S.head] = now; S.head = (S.head + 1) % kRing; S.n = std::min(S.n + 1, kRing);
  }
  S.last_ns = t;
  if (now >= S.next_avg && S.n > 0) {   // the shown numbers refresh twice a second
    S.next_avg = now + 0.5;
    double sum = 0; int c = 0;
    float worst[kRing]; int w = 0;
    for (int i = 0; i < S.n; i++) {
      const int k = (S.head - 1 - i + kRing) % kRing;
      const double age = now - S.at[k];
      if (age <= 0.5) { sum += S.dt[k]; c++; }
      if (age <= 10.0) worst[w++] = S.dt[k];
    }
    if (c) { S.avg_ms = (float)(sum / c); S.fps = S.avg_ms > 0 ? 1000.0f / S.avg_ms : 0; }
    if (w) {   // 1% low: the average frame rate of the slowest 1% of frames over the last 10 s
      const int m = std::max(1, w / 100);
      std::partial_sort(worst, worst + m, worst + w, [](float a, float b) { return a > b; });
      double s = 0; for (int i = 0; i < m; i++) s += worst[i];
      S.low1 = s > 0 ? (float)(1000.0 * m / s) : 0;
    }
    if (mode == 2) {
      double g = -1, cp = -1; int rw = 0, rh = 0; double hz = 0; char aa[8];
      snap_perf_overlay_info(&g, &cp, &rw, &rh, &hz, aa, 0);
      S.gpu = (float)g; S.cpu = (float)cp;
    }
  }
  return true;
}

bool safe_banner_on() { return st::safe_mode(); }

// One line at the top centre, drawn by the host overlay (never in the game's photos).
void draw_safe_banner(ImVec2 size) {
  const float k = std::max(0.75f, size.y / 1080.0f);
  ImFont* font = ImGui::GetFont();
  const float fs = 16.0f * k, pad = 7.0f * k;
  const char* t = tr("SAFE MODE: mods, HD packs, enhanced effects, frame generation, ray tracing and the shader cache are off for this run");
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t);
  const float x = (size.x - ts.x) * 0.5f, y = 8.0f * k;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(x - pad * 1.5f, y), ImVec2(x + ts.x + pad * 1.5f, y + ts.y + pad * 2), IM_COL32(20, 20, 24, 200), 6.0f * k);
  dl->AddRect(ImVec2(x - pad * 1.5f, y), ImVec2(x + ts.x + pad * 1.5f, y + ts.y + pad * 2), IM_COL32(242, 168, 38, 230), 6.0f * k, 0, 1.5f);
  dl->AddText(font, fs, ImVec2(x, y + pad), IM_COL32(255, 232, 170, 255), t);
}

// "Starting the game": the launcher's Play button hands over to the game window, which stays black until the first picture (the first start can take
// a minute or more while the shader cache fills and the files load). SNAP_LAUNCHER_STARTING=1 (set by the launcher) turns the note on; the NVN
// present path reports the first picture through snap_ui_first_picture(). Gives up by itself after five minutes.
static std::atomic<int> g_start_note{-1};   // -1: not read yet (the launcher sets the variable after static initialisation); 0 off; 1 on
static uint64_t g_start_t0_ns = 0;

bool start_note_on() {
  int on = g_start_note.load(std::memory_order_relaxed);
  if (on < 0) { const char* e = std::getenv("SNAP_LAUNCHER_STARTING"); on = e && e[0] == '1'; g_start_note.store(on); }
  if (!on) return false;
  const uint64_t now = steady_ns();
  if (!g_start_t0_ns) g_start_t0_ns = now;
  if (now - g_start_t0_ns > 300000000000ull) { g_start_note.store(0); return false; }
  return true;
}

void start_note_done() { g_start_note.store(0); }

void draw_start_note(ImVec2 size) {
  const float k = std::max(0.75f, size.y / 1080.0f);
  ImFont* font = ImGui::GetFont();
  const float fs = 26.0f * k, fs2 = 17.0f * k, pad = 16.0f * k;
  const char* t1 = "Starting the game...";
  const char* t2 = "The screen stays black for a little while. The first start can take a minute or more.";
  const ImVec2 s1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t1), s2 = font->CalcTextSizeA(fs2, FLT_MAX, 0.0f, t2);
  const float w = std::max(s1.x, s2.x), h = s1.y + s2.y + 8.0f * k;
  const float x = (size.x - w) * 0.5f, y = size.y * 0.5f - h * 0.5f;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(x - pad, y - pad), ImVec2(x + w + pad, y + h + pad), IM_COL32(20, 20, 24, 200), 8.0f * k);
  dl->AddText(font, fs, ImVec2(x + (w - s1.x) * 0.5f, y), IM_COL32(255, 255, 255, 255), t1);
  dl->AddText(font, fs2, ImVec2(x + (w - s2.x) * 0.5f, y + s1.y + 8.0f * k), IM_COL32(220, 220, 224, 255), t2);
}

void draw_fps_overlay(ImVec2 size) {
  const int mode = fps_overlay_mode();
  if (mode == 0) return;
  const int corner = std::clamp(g_corner.load(), 0, 3);
  float fps, avg, low1, gpu, cpu; float graph[kRing]; int gn = 0; double now;
  {
    std::lock_guard<std::mutex> lk(S.mu);
    fps = S.fps; avg = S.avg_ms; low1 = S.low1; gpu = S.gpu; cpu = S.cpu;
    now = S.last_ns ? secs(S.last_ns) : 0;
    for (int i = S.n - 1; i >= 0; i--) {   // oldest first, the last 2.5 s
      const int k = (S.head - 1 - i + kRing) % kRing;
      if (now - S.at[k] <= 2.5) graph[gn++] = S.dt[k];
    }
  }
  // readable at 4K: the size follows the output height (1x at 1080p), independent of the F1 menu scale
  const float k = std::max(0.75f, size.y / 1080.0f);
  ImFont* font = ImGui::GetFont();
  const float fs = 17.0f * k, pad = 8.0f * k, margin = 12.0f * k, lh = fs * 1.18f;
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  char head[48];
  snprintf(head, sizeof head, "%.0f FPS", fps);
  std::string lines[8]; int nl = 0;
  double hz = 0; int rw = 0, rh = 0; char aa[96] = "";
  if (mode == 2) {
    double g, c; snap_perf_overlay_info(&g, &c, &rw, &rh, &hz, aa, (int)sizeof aa);
    char b[128];
    snprintf(b, sizeof b, tr("%.2f ms  1%% low %.0f"), avg, low1); lines[nl++] = b;
    if (cpu >= 0 || gpu >= 0) { snprintf(b, sizeof b, "CPU %.2f ms  GPU %.2f ms", cpu, gpu); lines[nl++] = b; }
    snprintf(b, sizeof b, tr("Render %dx%d  Out %dx%d"), rw, rh, (int)size.x, (int)size.y); lines[nl++] = b;
    lines[nl++] = aa;
    const int cap = snap_window_frame_cap();
    const char* vs = snap_vsync_get();
    if (cap > 0) snprintf(b, sizeof b, tr("Cap %d  Vsync %s"), cap, vs ? vs : "on");
    else snprintf(b, sizeof b, tr("Cap none  Vsync %s"), vs ? vs : "on");
    lines[nl++] = b;
  }
  const float headfs = mode == 2 ? fs * 1.25f : fs * 1.15f;
  float w = font->CalcTextSizeA(headfs, FLT_MAX, 0, head).x;
  for (int i = 0; i < nl; i++) w = std::max(w, font->CalcTextSizeA(fs, FLT_MAX, 0, lines[i].c_str()).x);
  const float gh = mode == 2 ? 46.0f * k : 0, gw = std::max(w, 200.0f * k);
  if (mode == 2) w = gw;
  const float h = headfs * 1.15f + nl * lh + (mode == 2 ? gh + pad : 0);
  const float bw = w + pad * 2, bh = h + pad * 2;
  const float x0 = (corner == 0 || corner == 2) ? margin : size.x - margin - bw;
  const float y0 = (corner == 0 || corner == 1) ? margin : size.y - margin - bh;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), IM_COL32(16, 18, 22, 150), 6.0f * k);
  const ImU32 fpsCol = fps >= 55 ? IM_COL32(140, 235, 140, 255) : fps >= 28 ? IM_COL32(250, 214, 90, 255) : IM_COL32(255, 120, 110, 255);
  float y = y0 + pad;
  dl->AddText(font, headfs, ImVec2(x0 + pad + 1, y + 1), IM_COL32(0, 0, 0, 160), head);
  dl->AddText(font, headfs, ImVec2(x0 + pad, y), fpsCol, head);
  y += headfs * 1.15f;
  for (int i = 0; i < nl; i++) {
    dl->AddText(font, fs, ImVec2(x0 + pad + 1, y + 1), IM_COL32(0, 0, 0, 150), lines[i].c_str());
    dl->AddText(font, fs, ImVec2(x0 + pad, y), IM_COL32(235, 238, 242, 255), lines[i].c_str());
    y += lh;
  }
  if (mode == 2) {   // frame-time graph, last 2.5 s; the line is the target frame time (frame cap or the game's pace)
    y += pad * 0.5f;
    const float gx0 = x0 + pad, gy1 = y + gh;
    dl->AddRectFilled(ImVec2(gx0, y), ImVec2(gx0 + gw, gy1), IM_COL32(0, 0, 0, 90), 3.0f * k);
    const int cap = snap_window_frame_cap();
    const double target_hz = cap > 0 && (hz <= 0 || cap < hz) ? cap : hz;
    const float target = target_hz > 0 ? (float)(1000.0 / target_hz) : 16.67f;
    const float top = std::max(target * 2.0f, 1.0f);   // scale: 0 .. twice the target
    auto ypos = [&](float ms) { return gy1 - std::clamp(ms / top, 0.0f, 1.0f) * gh; };
    dl->AddLine(ImVec2(gx0, ypos(target)), ImVec2(gx0 + gw, ypos(target)), IM_COL32(120, 200, 255, 200), 1.0f * k);
    if (gn > 1) {
      const float step = gw / (float)(gn - 1);
      for (int i = 1; i < gn; i++) {
        const ImU32 c = graph[i] > target * 1.5f ? IM_COL32(255, 120, 110, 255) : IM_COL32(140, 235, 140, 255);
        dl->AddLine(ImVec2(gx0 + (i - 1) * step, ypos(graph[i - 1])), ImVec2(gx0 + i * step, ypos(graph[i])), c, 1.5f * k);
      }
    }
  }
}

}  // namespace snap::ui
