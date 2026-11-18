// SPDX-License-Identifier: MIT
// Developer loop UI (project code, ours; SNAP_DEV=1 only, see runtime/nvn/nvn_dev.h and docs/devloop.md):
//   - the F1 "Tuning" tab: every param registered with dev_param_f/i/b as a slider or checkbox, grouped by name; a change is written into the
//     renderer's variable at once and seen on the next frame. "Copy as env line" / "Copy as PowerShell" / "Save env file" keep a setting.
//   - the GPU cost overlay: ms per named pass averaged over 1 s (dev_pass_begin/end and, with "renderer passes", the SNAP_GPU_PASSES marks), a
//     corner readout drawn by the host overlay after the game's frame is composed, so it never reaches the game's photos or its own screenshots
//     (NVN_FRAME_DUMP_POST dumps, which are the finished swapchain image, do show it). F4 toggles it unless F4 is bound to a game button.
//   - shader compile errors of the hot reload, in red at the bottom of the screen until the shader compiles again.
// With SNAP_DEV unset nothing here runs: tune_tick returns false after one cached-bool test and the tab is not listed.
#include "ui_host.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hle/input_core.h"
#include "imgui.h"
#include "nvn/nvn_dev.h"
#include "ui_internal.h"

namespace snap::ui {
namespace {
float px(float v) { return v * ImGui::GetFontSize() / 17.0f; }

struct Snap { std::vector<DevParamView> p; };
std::string g_note;      // last "copied / saved" line of the tab
double g_noteUntil = 0;
char g_filter[64] = "";
bool g_renderer = false;

std::string group_of(const std::string& n) {   // SNAP_LIGHT_AO_RADIUS -> LIGHT
  size_t a = n.compare(0, 5, "SNAP_") == 0 ? 5 : 0;
  const size_t b = n.find('_', a);
  return n.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
std::string fmt_value(const DevParamView& v) {
  char b[48];
  if (v.kind == 'f') snprintf(b, sizeof b, "%.6g", v.value);
  else snprintf(b, sizeof b, "%d", (int)std::lround(v.value));
  return b;
}
bool changed(const DevParamView& v) { return std::fabs(v.value - v.def) > (v.kind == 'f' ? 1e-6 : 0.5 - 1e-9); }

// "NAME=value NAME=value" of the params that differ from their default; ps = $env:NAME='value'; ...
std::string env_line(bool ps, bool all) {
  std::vector<DevParamView> v(1024);
  const int n = dev_param_list(v.data(), (int)v.size());
  std::string o;
  for (int i = 0; i < n; i++) {
    if (!all && !changed(v[i])) continue;
    if (!o.empty()) o += ps ? "; " : " ";
    o += ps ? std::string("$env:") + v[i].name + "='" + fmt_value(v[i]) + "'" : std::string(v[i].name) + "=" + fmt_value(v[i]);
  }
  return o;
}
void note(const std::string& s) { g_note = s; g_noteUntil = ImGui::GetTime() + 6.0; }
}  // namespace

// F4 toggles the overlay unless the player bound F4 to a game button
bool tune_hotkey_free() {
  if (!dev_on()) return false;
  static const int k = hle::input::key_from_name("F4");
  if (!k) return false;
  const hle::input::Source s{hle::input::Source::KeyboardKey, k};
  const hle::input::Mapping m = hle::input::hub().mapping();
  for (const auto& b : m.buttons) if (std::find(b.begin(), b.end(), s) != b.end()) return false;
  for (const hle::input::StickMap* sm : {&m.left, &m.right})
    for (const auto* v : {&sm->up, &sm->down, &sm->left, &sm->right}) if (std::find(v->begin(), v->end(), s) != v->end()) return false;
  return true;
}
void tune_toggle_overlay() { dev_overlay_set(!dev_overlay_get()); }

// Once per present (present thread), before anything is drawn: true when the dev layer wants this frame.
bool tune_tick() {
  if (!dev_on()) return false;
  if (dev_overlay_get()) return true;
  return dev_shader_errors(nullptr, 0) > 0;
}

void draw_tune_overlay(ImVec2 size) {
  if (!dev_on()) return;
  const float k = std::max(0.75f, size.y / 1080.0f);
  ImFont* font = ImGui::GetFont();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float fs = 16.0f * k, pad = 8.0f * k, margin = 12.0f * k, lh = fs * 1.2f;
  if (dev_overlay_get()) {
    DevPassStat st[64];
    const int n = dev_pass_stats(st, 64);
    const double busy = dev_gpu_busy();
    struct Row { std::string name; double ms; };
    std::vector<Row> rows;
    for (int i = 0; i < n && rows.size() < 14; i++) if (st[i].ms >= 0.0005) rows.push_back({st[i].name, st[i].ms});
    char head[64];
    if (busy >= 0) snprintf(head, sizeof head, "GPU %.2f ms (1 s avg)", busy); else snprintf(head, sizeof head, "GPU cost: waiting for frames");
    float w = font->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, head).x;
    float namew = 0;
    for (const Row& r : rows) namew = std::max(namew, font->CalcTextSizeA(fs, FLT_MAX, 0, r.name.c_str()).x);
    const float valw = font->CalcTextSizeA(fs, FLT_MAX, 0, "00.000 ms").x, barw = 70.0f * k;
    if (!rows.empty()) w = std::max(w, namew + valw + barw + pad * 3);
    const float h = fs * 1.1f * 1.25f + (float)rows.size() * lh + (rows.empty() ? lh : 0);
    const float x0 = margin, y0 = margin, bw = w + pad * 2, bh = h + pad * 2;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), IM_COL32(16, 18, 22, 165), 6.0f * k);
    float y = y0 + pad;
    dl->AddText(font, fs * 1.1f, ImVec2(x0 + pad, y), IM_COL32(150, 220, 255, 255), head);
    y += fs * 1.1f * 1.25f;
    if (rows.empty()) dl->AddText(font, fs, ImVec2(x0 + pad, y), IM_COL32(190, 195, 205, 255), busy >= 0 ? "no named passes yet (dev_pass_begin)" : "");
    const double top = rows.empty() ? 1.0 : std::max(rows[0].ms, 0.01);
    for (const Row& r : rows) {
      dl->AddText(font, fs, ImVec2(x0 + pad, y), IM_COL32(235, 238, 242, 255), r.name.c_str());
      char v[32]; snprintf(v, sizeof v, "%.3f ms", r.ms);
      const float vx = x0 + pad + namew + pad;
      dl->AddText(font, fs, ImVec2(vx + valw - font->CalcTextSizeA(fs, FLT_MAX, 0, v).x, y), IM_COL32(250, 214, 90, 255), v);
      const float bx = vx + valw + pad, bwid = barw * (float)(r.ms / top);
      dl->AddRectFilled(ImVec2(bx, y + lh * 0.25f), ImVec2(bx + std::max(1.0f, bwid), y + lh * 0.75f), IM_COL32(120, 200, 255, 190));
      y += lh;
    }
  }
  char err[2048];
  if (dev_shader_errors(err, (int)sizeof err) > 0) {   // compile errors of the hot reload: the old shader keeps running
    const std::string e = std::string("SHADER COMPILE ERROR (previous shader kept)\n") + err;
    const float wrap = size.x * 0.7f;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, wrap, e.c_str());
    const float x0 = margin, y1 = size.y - margin, y0 = y1 - ts.y - pad * 2;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + ts.x + pad * 2, y1), IM_COL32(60, 8, 8, 215), 6.0f * k);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + ts.x + pad * 2, y1), IM_COL32(255, 90, 80, 255), 6.0f * k, 0, 1.5f * k);
    dl->AddText(font, fs, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 200, 195, 255), e.c_str(), nullptr, wrap);
  }
}

// ---- the F1 "Tuning" tab -----------------------------------------------------------------------------------------------
void tab_tune() {
  if (!dev_on()) { ImGui::TextWrapped("The developer loop is off. Start the game with SNAP_DEV=1."); return; }
  ImGui::TextWrapped("Live-tunable values registered by the renderer (dev_param_*). A change applies on the next frame. Names are the env vars that set the "
                     "same value: copy a setting as an env line to keep it. F4 toggles the GPU cost overlay.");
  ImGui::Spacing();
  bool ov = dev_overlay_get();
  if (ImGui::Checkbox("GPU cost overlay (ms per named pass, 1 s average)", &ov)) dev_overlay_set(ov);
  if (ImGui::Checkbox("Include the renderer's own marks (rp, cs, aa, fx; many, they cost GPU time)", &g_renderer)) dev_renderer_set(g_renderer);
  if (ImGui::Button("Reload all shaders")) dev_shader_reload_all();
  ImGui::SameLine();
  if (ImGui::Button("Reset all")) { std::vector<DevParamView> v(1024); const int n = dev_param_list(v.data(), (int)v.size()); for (int i = 0; i < n; i++) dev_param_reset(v[i].name); }
  ImGui::SameLine();
  if (ImGui::Button("Copy changed as env line")) { ImGui::SetClipboardText(env_line(false, false).c_str()); note("Copied: " + env_line(false, false)); }
  ImGui::SameLine();
  if (ImGui::Button("Copy as PowerShell")) { ImGui::SetClipboardText(env_line(true, false).c_str()); note("Copied: " + env_line(true, false)); }
  ImGui::SameLine();
  if (ImGui::Button("Save env file")) {
    const std::string path = std::string(dev_ctl_dir()) + std::string(1, host::path_sep()) + "tuned_" + std::to_string(host::current_process_id()) + ".env";
    std::vector<DevParamView> v(1024); const int n = dev_param_list(v.data(), (int)v.size());
    std::string body;
    for (int i = 0; i < n; i++) if (changed(v[i])) body += std::string(v[i].name) + "=" + fmt_value(v[i]) + "\n";
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(body.data(), 1, body.size(), f); fclose(f); note("Saved " + path); } else note("Could not write " + path);
  }
  if (!g_note.empty() && ImGui::GetTime() < g_noteUntil) ImGui::TextWrapped("%s", g_note.c_str());
  char err[2048];
  if (dev_shader_errors(err, (int)sizeof err) > 0) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1)); ImGui::TextWrapped("Shader compile error:\n%s", err); ImGui::PopStyleColor(); }
  ImGui::Separator();
  ImGui::SetNextItemWidth(px(260));
  ImGui::InputTextWithHint("##filter", "filter by name", g_filter, sizeof g_filter);

  std::vector<DevParamView> v(1024);
  const int n = dev_param_list(v.data(), (int)v.size());
  if (n == 0) { ImGui::TextDisabled("No params registered yet (they register when the effect first runs)."); return; }
  std::vector<std::string> groups;
  for (int i = 0; i < n; i++) { const std::string g = group_of(v[i].name); if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g); }
  std::sort(groups.begin(), groups.end());
  std::string flt = g_filter; for (char& c : flt) c = (char)toupper((unsigned char)c);
  for (const std::string& g : groups) {
    bool any = false;
    for (int i = 0; i < n; i++) {
      std::string nm = v[i].name; for (char& c : nm) c = (char)toupper((unsigned char)c);
      if (group_of(v[i].name) == g && (flt.empty() || nm.find(flt) != std::string::npos)) { any = true; break; }
    }
    if (!any) continue;
    if (!ImGui::CollapsingHeader(g.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
    ImGui::PushID(g.c_str());
    for (int i = 0; i < n; i++) {
      DevParamView& p = v[i];
      if (group_of(p.name) != g) continue;
      std::string nm = p.name; for (char& c : nm) c = (char)toupper((unsigned char)c);
      if (!flt.empty() && nm.find(flt) == std::string::npos) continue;
      ImGui::PushID(p.name);
      const bool ch = changed(p);
      if (ch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1));
      const char* label = p.name + (strncmp(p.name, "SNAP_", 5) == 0 ? 5 : 0);
      if (p.kind == 'b') {
        bool b = p.value >= 0.5;
        if (ImGui::Checkbox(label, &b)) dev_param_set(p.name, b ? 1 : 0);
      } else {
        ImGui::SetNextItemWidth(px(300));
        if (p.kind == 'f') {
          float f = (float)p.value;
          if (ImGui::SliderFloat("##v", &f, (float)p.mn, (float)p.mx, "%.4g")) dev_param_set(p.name, f);
        } else {
          int iv = (int)std::lround(p.value);
          if (ImGui::SliderInt("##v", &iv, (int)p.mn, (int)p.mx)) dev_param_set(p.name, iv);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
      }
      if (ch) ImGui::PopStyleColor();
      if (ch) { ImGui::SameLine(); if (ImGui::SmallButton("reset")) dev_param_reset(p.name); }
      ImGui::PopID();
    }
    ImGui::PopID();
  }
}

}  // namespace snap::ui
