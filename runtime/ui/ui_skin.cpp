// SPDX-License-Identifier: MIT
// The F1 menu's game skin. See ui_skin.h and analysis/ui/f1_skin.md for what it copies from the game's Settings screen.
#include "ui_skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hle/access.h"
#include "hle/input_core.h"
#include "imgui_internal.h"   // ImGuiItemFlags_NoNav on the tab strip and tags (L / R switch pages; the d-pad belongs to the rows)
#include "ui_internal.h"
#include "ui_i18n.h"
#include "ui_theme.h"

namespace snap::ui {

ImVec4 overlay_fb(const ImVec4& c);   // ui_overlay.cpp: the sRGB swapchain pre-conversion

namespace {

float px(float v) { return v * ImGui::GetFontSize() / theme().font_px; }
ImVec4 rgb(unsigned hex, float a = 1.0f) { return ImVec4(((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a); }
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }

double now_s() { return ImGui::GetTime(); }

// Pointed-end bar (the shape of the game's setting rows and pills).
void bar_shape(ImDrawList* dl, ImVec2 a, ImVec2 b, float tip, ImU32 fill, ImU32 edge = 0, float edge_w = 0.0f) {
  const float my = (a.y + b.y) * 0.5f;
  const ImVec2 pts[6] = {{a.x, my}, {a.x + tip, a.y}, {b.x - tip, a.y}, {b.x, my}, {b.x - tip, b.y}, {a.x + tip, b.y}};
  dl->AddConvexPolyFilled(pts, 6, fill);
  if (edge) dl->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, edge_w);
}

// The yellow selection fill: a few rows of small right-pointing triangles a shade lighter than the base.
void yellow_fill(ImDrawList* dl, ImVec2 a, ImVec2 b, float tip, float alpha_mul) {
  const SkinPal& p = skin_pal();
  bar_shape(dl, a, b, tip, skin_col(p.yellow, alpha_mul));
  const float h = b.y - a.y, s = h * 0.5f;
  dl->PushClipRect(ImVec2(a.x + tip * 0.5f, a.y), ImVec2(b.x - tip * 0.5f, b.y), true);
  const ImU32 c = skin_col(p.yellow_tri, alpha_mul);
  for (int row = 0; row < 2; ++row) {
    const float y0 = a.y + h * 0.04f + row * s;
    for (float x = a.x + (row ? s * 0.5f : 0.0f); x < b.x; x += s * 1.1f)
      dl->AddTriangleFilled(ImVec2(x, y0 + s * 0.12f), ImVec2(x, y0 + s * 0.88f), ImVec2(x + s * 0.5f, y0 + s * 0.5f), c);
  }
  dl->PopClipRect();
}

void text_center(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* t, ImU32 c) {
  const ImVec2 ts = ImGui::CalcTextSize(t);
  dl->AddText(ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)), c, t);
}

bool high_contrast() { return hle::access::state().contrast != 0; }

struct Help { std::string text, extra; bool warn = false, set = false; } g_help;
double g_open_t = -10.0;
std::map<ImGuiID, float> g_ease;   // row focus eases

}  // namespace

const SkinPal& skin_pal() {
  static const SkinPal dark = [] {
    SkinPal p;
    p.backdrop = ImVec4(0.04f, 0.05f, 0.07f, 0.42f);
    p.panel = rgb(0x4f565f, 0.94f);
    p.panel_edge = rgb(0x6b727b, 1.0f);
    p.tab_idle = rgb(0x8c919b, 1.0f);
    p.tab_idle_text = rgb(0xffffff);
    p.tab_sel_text = rgb(0xffffff);
    p.ornament = rgb(0xc4de9a);
    p.row = rgb(0x70747b, 0.96f);
    p.row_text = rgb(0xffffff);
    p.row_focus = rgb(0xb8d593);
    p.row_focus_text = rgb(0x2b3a22);
    p.row_dim_text = rgb(0xdcdde0);
    p.yellow = rgb(0xf8d74d);
    p.yellow_tri = rgb(0xfbe58c, 0.75f);
    p.yellow_text = rgb(0x6a5217);
    p.off_pill = rgb(0xa7a6a7);
    p.off_text = rgb(0x3d3d40);
    p.white_pill = rgb(0xf4f2f0);
    p.white_text = rgb(0x4a4745);
    p.arrow = rgb(0xa09f9d);
    p.bar = rgb(0x3c3f45, 0.95f);
    p.bar_text = rgb(0xffffff);
    p.bar_dim = rgb(0xc9cbd0);
    p.tag = rgb(0xf1aa6a);
    p.tag_edge = rgb(0xd68c4a);
    p.tag_text = rgb(0xb86a2c);
    p.tag_inner = rgb(0xfff4e6);
    p.good = rgb(0xc4e8a8);
    p.warn = rgb(0xffd479);
    p.error = rgb(0xffa597);
    return p;
  }();
  static const SkinPal contrast = [] {
    SkinPal p = dark;
    p.backdrop = ImVec4(0, 0, 0, 0.7f);
    p.panel = rgb(0x000000, 1.0f);
    p.panel_edge = rgb(0xffffff);
    p.tab_idle = rgb(0x2a2a2a);
    p.row = rgb(0x1c1c1c);
    p.row_dim_text = rgb(0xe0e0e0);
    p.row_focus = rgb(0xffd900);
    p.row_focus_text = rgb(0x000000);
    p.off_pill = rgb(0x3a3a3a);
    p.off_text = rgb(0xffffff);
    p.white_pill = rgb(0xffffff);
    p.white_text = rgb(0x000000);
    p.bar = rgb(0x101010);
    p.bar_dim = rgb(0xe8e8e8);
    return p;
  }();
  return high_contrast() ? contrast : dark;
}

ImU32 skin_col(const ImVec4& c, float mul_alpha) {
  ImVec4 v = overlay_fb(c);
  v.w = c.w * mul_alpha;
  return ImGui::GetColorU32(v);
}

float skin_ease_out(float t) { t = std::clamp(t, 0.0f, 1.0f); const float u = 1.0f - t; return 1.0f - u * u * u; }
float skin_approach(float cur, float target, float rate) {
  const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
  return cur + (target - cur) * (1.0f - std::exp(-rate * dt));
}

// ---- style and theme scope -------------------------------------------------------------------------------------------------
struct SkinScope::Saved {
  ImGuiStyle style;
  Theme theme;
};

SkinScope::SkinScope() : s_(new Saved{ImGui::GetStyle(), theme()}) {
  const SkinPal& p = skin_pal();
  Theme& t = theme();
  ImGuiStyle& s = ImGui::GetStyle();
  const float k = ImGui::GetFontSize() / t.font_px;
  const bool hc = high_contrast();
  if (!hc) {   // the high contrast row already replaced the theme colors in place
    t.text = p.row_text;
    t.text_dim = p.bar_dim;
    t.title = rgb(0xf8d547);
    t.accent = p.yellow;
    t.accent_hover = rgb(0xf8ab79);
    t.accent_active = rgb(0xf2c12d);
    t.on_accent = p.yellow_text;
    t.slot = rgb(0x6f737b);
    t.slot_hover = rgb(0x858992);
    t.slot_listening = rgb(0xf1aa6a);
    t.panel = p.panel;
    t.panel_alt = ImVec4(1, 1, 1, 0.06f);
    t.border = rgb(0x7d838c);
    t.good = p.good;
    t.error = p.error;
    t.conflict = p.warn;
    t.toast_bg = rgb(0x3c3f45, 0.97f);
  }
  t.rounding = 17.0f;
  t.window_round = 18.0f;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = overlay_fb(t.text);
  c[ImGuiCol_TextDisabled] = overlay_fb(t.text_dim);
  c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = overlay_fb(rgb(0x474d56, 0.98f));
  c[ImGuiCol_Border] = overlay_fb(hc ? p.panel_edge : rgb(0xa4a9b2));
  c[ImGuiCol_FrameBg] = overlay_fb(rgb(0x7a7e86));
  c[ImGuiCol_FrameBgHovered] = overlay_fb(rgb(0x8d919a));
  c[ImGuiCol_FrameBgActive] = overlay_fb(p.yellow);
  c[ImGuiCol_Button] = overlay_fb(rgb(0x626872));
  c[ImGuiCol_ButtonHovered] = overlay_fb(rgb(0x7b828d));
  c[ImGuiCol_ButtonActive] = overlay_fb(rgb(0xc99a2e));
  c[ImGuiCol_Header] = overlay_fb(rgb(0x7a7e86));
  c[ImGuiCol_HeaderHovered] = overlay_fb(rgb(0xa2a6ad));
  c[ImGuiCol_HeaderActive] = overlay_fb(p.yellow);
  c[ImGuiCol_CheckMark] = overlay_fb(hc ? p.row_focus : p.yellow);
  c[ImGuiCol_SliderGrab] = overlay_fb(p.yellow);
  c[ImGuiCol_SliderGrabActive] = overlay_fb(rgb(0xf8ab79));
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.22f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.05f);
  c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.14f);
  c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.30f);
  c[ImGuiCol_NavCursor] = overlay_fb(hc ? p.row_focus : rgb(0xf8ab79));
  c[ImGuiCol_TextSelectedBg] = ImVec4(p.yellow.x, p.yellow.y, p.yellow.z, 0.45f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.38f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.55f);
  c[ImGuiCol_ScrollbarGrabActive] = overlay_fb(p.yellow);
  s.WindowRounding = t.window_round * k;
  s.ChildRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = s.ScrollbarRounding = t.rounding * k;
  s.WindowBorderSize = 0.0f;
  s.FrameBorderSize = std::max(1.0f, 1.0f * k);
  s.ChildBorderSize = 0.0f;
  s.PopupBorderSize = 1.0f;
  s.ScrollbarSize = 12.0f * k;
}

SkinScope::~SkinScope() {
  ImGui::GetStyle() = s_->style;
  theme() = s_->theme;
  delete s_;
}

// ---- tab strip ---------------------------------------------------------------------------------------------------------------
void skin_shoulder_tag(ImDrawList* dl, ImVec2 pos, float h, bool right, bool pressed) {
  const SkinPal& p = skin_pal();
  using namespace hle::input;
  if (style_pad_glyphs()) {   // Xbox or PlayStation pictures chosen
    const int b = right ? kPadRShoulder : kPadLShoulder;
    const float w = style_pad_glyph_width(b, h * 0.7f);
    style_pad_glyph(dl, ImVec2(pos.x + (h * 1.35f - w) * 0.5f, pos.y + h * 0.15f), h * 0.7f, b);
    return;
  }
  const float w = h * 1.35f, tip = h * 0.38f;
  const ImVec2 a = pos, b(pos.x + w, pos.y + h);
  ImVec2 pts[5];
  if (!right) { pts[0] = ImVec2(a.x, (a.y + b.y) * 0.5f); pts[1] = ImVec2(a.x + tip, a.y); pts[2] = ImVec2(b.x, a.y); pts[3] = ImVec2(b.x, b.y); pts[4] = ImVec2(a.x + tip, b.y); }
  else { pts[0] = ImVec2(b.x, (a.y + b.y) * 0.5f); pts[1] = ImVec2(b.x - tip, b.y); pts[2] = ImVec2(a.x, b.y); pts[3] = ImVec2(a.x, a.y); pts[4] = ImVec2(b.x - tip, a.y); }
  dl->AddConvexPolyFilled(pts, 5, skin_col(pressed ? mix(p.tag, ImVec4(1, 1, 1, 1), 0.3f) : p.tag));
  dl->AddPolyline(pts, 5, skin_col(p.tag_edge), ImDrawFlags_Closed, std::max(1.0f, h * 0.05f));
  const float cx = right ? (a.x + b.x - tip) * 0.5f + h * 0.02f : (a.x + tip + b.x) * 0.5f - h * 0.02f;
  const float s = h * 0.32f;
  dl->AddRectFilled(ImVec2(cx - s, (a.y + b.y) * 0.5f - s), ImVec2(cx + s, (a.y + b.y) * 0.5f + s), skin_col(p.tag_inner), s * 0.45f);
  const char* l = right ? "R" : "L";
  const ImVec2 ts = ImGui::CalcTextSize(l);
  dl->AddText(ImVec2(cx - ts.x * 0.5f, (a.y + b.y) * 0.5f - ts.y * 0.5f), skin_col(p.tag_text), l);
}

static std::map<std::string, ImVec2> g_tab_pos;
bool skin_tab_center(const char* name, float* x, float* y) {
  auto it = g_tab_pos.find(name);
  if (it == g_tab_pos.end()) return false;
  *x = it->second.x; *y = it->second.y;
  return true;
}

int skin_tabs(const char* const* names, int n, int cur, float width) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // pages switch with L / R (or a click); the pad cursor stays in the page
  const float th = px(48), tagw = th * 1.35f, gap = px(5), padx = px(13), minw = px(78);
  const float avail = width - 2 * (tagw + px(10));
  std::vector<float> tw((size_t)n);
  for (int i = 0; i < n; ++i) tw[(size_t)i] = std::max(minw, ImGui::CalcTextSize(names[i]).x + 2 * padx);
  static int first = 0;
  first = std::clamp(first, 0, n - 1);
  if (cur < first) first = cur;
  auto span = [&](int a, int b) { float s = 0; for (int i = a; i <= b; ++i) s += tw[(size_t)i] + (i > a ? gap : 0.0f); return s; };
  while (first < cur && span(first, cur) > avail) ++first;
  int last = first;
  while (last + 1 < n && span(first, last + 1) <= avail) ++last;
  const float used = span(first, last);
  float x = o.x + tagw + px(10) + std::max(0.0f, (avail - used) * 0.5f);
  int clicked = -1;
  const float rest = o.y + th;
  for (int i = first; i <= last; ++i) {
    const bool sel = i == cur;
    const float top = o.y + (sel ? 0.0f : px(6));
    const ImVec2 a(x, top), b(x + tw[(size_t)i], rest + (sel ? px(2) : 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(a.x, o.y));
    ImGui::PushID(i);
    const bool hit = ImGui::InvisibleButton("##tab", ImVec2(b.x - a.x, th));
    g_tab_pos[names[i]] = ImVec2((a.x + b.x) * 0.5f, o.y + th * 0.5f);
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hit && !sel) clicked = i;
    const float r = px(13);
    const ImVec4 body = sel ? mix(p.panel, ImVec4(1, 1, 1, 1), 0.13f) : (hov ? mix(p.tab_idle, ImVec4(1, 1, 1, 1), 0.18f) : p.tab_idle);
    dl->AddRectFilled(a, b, skin_col(ImVec4(body.x, body.y, body.z, 1.0f)), r, ImDrawFlags_RoundCornersTop);
    text_center(dl, a, ImVec2(b.x, b.y - (sel ? px(8) : 0.0f)), names[i], skin_col(sel ? p.tab_sel_text : p.tab_idle_text));
    if (sel) {   // green ornament under the selected tab: a line with an arrowhead at each end
      const float yy = b.y - px(8), half = std::min((b.x - a.x) * 0.36f, px(60)), cx = (a.x + b.x) * 0.5f, s = px(4);
      const ImU32 g = skin_col(p.ornament);
      dl->AddLine(ImVec2(cx - half, yy), ImVec2(cx + half, yy), g, std::max(1.0f, px(1.5f)));
      dl->AddTriangleFilled(ImVec2(cx - half - s, yy), ImVec2(cx - half + s * 0.6f, yy - s), ImVec2(cx - half + s * 0.6f, yy + s), g);
      dl->AddTriangleFilled(ImVec2(cx + half + s, yy), ImVec2(cx + half - s * 0.6f, yy - s), ImVec2(cx + half - s * 0.6f, yy + s), g);
    }
    x += tw[(size_t)i] + gap;
  }
  // L and R tags (also buttons for the mouse)
  const float tagh = th * 0.82f, ty = o.y + (th - tagh) * 0.5f;
  const bool l_on = ImGui::IsKeyDown(ImGuiKey_GamepadL1), r_on = ImGui::IsKeyDown(ImGuiKey_GamepadR1);
  ImGui::SetCursorScreenPos(ImVec2(o.x, ty));
  ImGui::PushID("tagL");
  if (ImGui::InvisibleButton("##l", ImVec2(tagh * 1.35f, tagh)) && n > 0) clicked = (cur + n - 1) % n;
  ImGui::PopID();
  skin_shoulder_tag(dl, ImVec2(o.x, ty), tagh, false, l_on || ImGui::IsItemActive());
  const float rx = o.x + width - tagh * 1.35f;
  ImGui::SetCursorScreenPos(ImVec2(rx, ty));
  ImGui::PushID("tagR");
  if (ImGui::InvisibleButton("##r", ImVec2(tagh * 1.35f, tagh)) && n > 0) clicked = (cur + 1) % n;
  ImGui::PopID();
  skin_shoulder_tag(dl, ImVec2(rx, ty), tagh, true, r_on || ImGui::IsItemActive());
  ImGui::PopItemFlag();
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + th));
  return clicked;
}

// ---- rows ----------------------------------------------------------------------------------------------------------------------
float skin_row_height() { return px(46); }
float skin_control_height() { return px(34); }

void skin_row_begin(SkinRow& r, ImGuiID id, bool focused) {
  r.dl = ImGui::GetWindowDrawList();
  r.p = ImGui::GetCursorScreenPos();
  r.w = ImGui::GetContentRegionAvail().x;
  r.h = skin_row_height();
  r.id = id;
  float& e = g_ease[id];
  e = skin_approach(e, focused ? 1.0f : 0.0f, 24.0f);
  if (std::fabs(e - (focused ? 1.0f : 0.0f)) < 0.003f) e = focused ? 1.0f : 0.0f;
  r.e = skin_ease_out(e);
  r.dl->ChannelsSplit(2);
  r.dl->ChannelsSetCurrent(1);
}

ImU32 skin_row_text(const SkinRow& r, float mul_alpha) {
  const SkinPal& p = skin_pal();
  return skin_col(mix(p.row_text, p.row_focus_text, r.e), mul_alpha);
}

void skin_row_end(SkinRow& r, bool disabled) {
  const SkinPal& p = skin_pal();
  r.dl->ChannelsSetCurrent(0);
  const bool hov = ImGui::IsMouseHoveringRect(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  const float lift = hov && r.e < 0.5f ? 0.10f : 0.0f;
  ImVec4 c = mix(mix(p.row, ImVec4(1, 1, 1, 1), lift), p.row_focus, r.e);
  if (disabled) c.w *= 0.55f;
  bar_shape(r.dl, r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), r.h * 0.42f, skin_col(c));
  r.dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + r.h));
  ImGui::Dummy(ImVec2(r.w, px(8)));
}

int skin_segmented(const char* id, const char* const* labels, int n, int sel, bool disabled) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = skin_control_height(), gap = px(8), padx = px(26), minw = px(110);
  float total = 0;
  std::vector<float> w((size_t)n);
  for (int i = 0; i < n; ++i) { w[(size_t)i] = std::max(minw, ImGui::CalcTextSize(labels[i]).x + 2 * padx); total += w[(size_t)i] + (i ? gap : 0.0f); }
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the row's green bar is the focus cursor
  const bool act = ImGui::InvisibleButton(id, ImVec2(total, h), ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  const bool by_mouse = act && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  // the pad's A does not change a value (Left / Right do); Enter and Space on the keyboard step to the next label
  const bool by_key = act && !by_mouse && !ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
  const float mx = ImGui::GetIO().MousePos.x;
  float x = o.x;
  int hit = -1;
  const float alpha = disabled ? 0.55f : 1.0f;
  for (int i = 0; i < n; ++i) {
    const ImVec2 a(x, o.y), b(x + w[(size_t)i], o.y + h);
    if (mx >= a.x && mx < b.x + gap * 0.5f) hit = i;
    if (i == sel) {
      yellow_fill(dl, a, b, h * 0.42f, alpha);
      text_center(dl, a, b, labels[i], skin_col(p.yellow_text, alpha));
    } else {
      bar_shape(dl, a, b, h * 0.42f, skin_col(p.off_pill, alpha), skin_col(mix(p.off_pill, ImVec4(0.3f, 0.3f, 0.3f, 1), 0.3f), alpha), std::max(1.0f, px(1.2f)));
      text_center(dl, a, b, labels[i], skin_col(p.off_text, alpha));
    }
    x = b.x + gap;
  }
  if (by_mouse) return hit;
  if (by_key && n > 0) return (std::max(sel, 0) + 1) % n;
  return -1;
}

bool skin_dropdown(const char* id, const char* text, float width, bool disabled) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = skin_control_height();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
  const bool act = ImGui::InvisibleButton(id, ImVec2(width, h), ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  const float alpha = disabled ? 0.55f : 1.0f;
  const ImVec2 a = o, b(o.x + width, o.y + h);
  bar_shape(dl, a, b, h * 0.42f, skin_col(mix(p.white_pill, ImVec4(1, 1, 1, 1), ImGui::IsItemHovered() ? 0.5f : 0.0f), alpha));
  // little grey box with a down arrow on the left, like the game's drop-down pills
  const float s = h * 0.62f;
  const ImVec2 ba(a.x + h * 0.5f, a.y + (h - s) * 0.5f);
  dl->AddRectFilled(ba, ImVec2(ba.x + s * 1.1f, ba.y + s), skin_col(mix(p.arrow, ImVec4(1, 1, 1, 1), 0.55f), alpha), s * 0.2f);
  const ImVec2 c(ba.x + s * 0.55f, ba.y + s * 0.5f);
  dl->AddTriangleFilled(ImVec2(c.x - s * 0.24f, c.y - s * 0.12f), ImVec2(c.x + s * 0.24f, c.y - s * 0.12f), ImVec2(c.x, c.y + s * 0.2f), skin_col(p.arrow, alpha));
  // too long for the pill: cut at a character boundary and end with dots
  std::string shown = text;
  const float room = (b.x - h * 0.4f) - (ba.x + s * 1.1f) - px(8);
  if (ImGui::CalcTextSize(shown.c_str()).x > room) {
    for (size_t n = shown.size(); n > 1;) {
      --n;
      while (n > 0 && ((unsigned char)shown[n] & 0xC0) == 0x80) --n;   // never cut inside a UTF-8 character
      const std::string cut = shown.substr(0, n) + "...";
      if (ImGui::CalcTextSize(cut.c_str()).x <= room) { shown = cut; break; }
    }
  }
  text_center(dl, ImVec2(ba.x + s * 1.1f, a.y), ImVec2(b.x - h * 0.4f, b.y), shown.c_str(), skin_col(p.white_text, alpha));
  return act && !disabled;
}

bool skin_slider(const char* id, float* frac_io, const char* text, float width, bool disabled, bool* released) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = skin_control_height();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
  ImGui::InvisibleButton(id, ImVec2(width, h), ImGuiButtonFlags_EnableNav);   // InvisibleButton is NoNav unless asked: the d-pad must reach the control
  ImGui::PopStyleColor();
  bool changed = false;
  if (!disabled && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float f = std::clamp((ImGui::GetIO().MousePos.x - o.x - h * 0.5f) / std::max(1.0f, width - h), 0.0f, 1.0f);
    if (std::fabs(f - *frac_io) > 1e-4f) { *frac_io = f; changed = true; }
  }
  if (released) *released = ImGui::IsItemDeactivated();
  const float alpha = disabled ? 0.55f : 1.0f;
  const ImVec2 a = o, b(o.x + width, o.y + h);
  const float fx = a.x + h * 0.5f + (width - h) * std::clamp(*frac_io, 0.0f, 1.0f);
  bar_shape(dl, a, b, h * 0.42f, skin_col(mix(p.white_pill, p.off_pill, 0.25f), alpha));
  if (fx > a.x + h * 0.6f) { dl->PushClipRect(a, ImVec2(fx, b.y), true); yellow_fill(dl, a, b, h * 0.42f, alpha); dl->PopClipRect(); }
  text_center(dl, a, b, text, skin_col(mix(p.yellow_text, p.white_text, *frac_io > 0.5f ? 0.0f : 0.6f), alpha));
  return changed;
}

// ---- description bar and prompts ----------------------------------------------------------------------------------------------
void skin_help_set(const std::string& text, const std::string& extra, bool extra_warn) { g_help.text = text; g_help.extra = extra; g_help.warn = extra_warn; g_help.set = true; }
bool skin_help_pending() { return g_help.set; }

void skin_help_draw(ImVec2 pos, float w, float h) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), skin_col(p.bar), ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersBottom);
  const float pad = px(18), padv = px(8), maxw = w - 2 * pad - px(215);
  if (g_help.set && maxw > 40.0f) {
    ImFont* font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    float fs = base, th = 0, eh = 0;
    for (int i = 0; i < 8; ++i) {   // largest size (down to about 80 %, still easy to read) at which the description and its small second line fit the bar
      fs = base * (1.0f - 0.03f * (float)i);
      th = font->CalcTextSizeA(fs, 1e9f, maxw, g_help.text.c_str()).y;
      eh = g_help.extra.empty() ? 0.0f : font->CalcTextSizeA(fs * 0.84f, 1e9f, maxw, g_help.extra.c_str()).y + px(3);
      if (th + eh <= h - 2 * padv) break;
    }
    dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
    const float y = pos.y + std::max(padv * 0.5f, (h - th - eh) * 0.5f);
    dl->AddText(font, fs, ImVec2(pos.x + pad, y), skin_col(p.bar_text), g_help.text.c_str(), nullptr, maxw);
    if (!g_help.extra.empty())
      dl->AddText(font, fs * 0.84f, ImVec2(pos.x + pad, y + th + px(3)), skin_col(g_help.warn ? p.warn : p.bar_dim), g_help.extra.c_str(), nullptr, maxw);
    dl->PopClipRect();
  }
  g_help = Help{};
}

bool skin_bar_prompts(ImVec2 r_min, ImVec2 r_max, bool back, const char* status, bool status_good) {
  const SkinPal& p = skin_pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float bh = r_max.y - r_min.y;
  const float h = bh * 0.40f, w = h * 1.45f;
  const ImVec2 o(r_max.x - w - px(18), r_min.y + bh * 0.12f);
  ImGui::SetCursorScreenPos(o);
  ImGui::PushID("barB");
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // B is the pad's Back
  const bool hit = back && ImGui::InvisibleButton("##back", ImVec2(w, h));
  ImGui::PopItemFlag();
  const bool down = back && ImGui::IsItemActive();
  ImGui::PopID();
  // the game's orange back tag: chevron with the B disc and a return arrow; Xbox / PlayStation pictures when chosen
  if (style_pad_glyphs()) {
    style_pad_glyph(dl, ImVec2(o.x + (w - h) * 0.5f, o.y), h, hle::input::kPadEast);
  } else {
    const float nose = h * 0.42f;
    const ImVec2 pts[5] = {{o.x + nose, o.y}, {o.x + w, o.y}, {o.x + w, o.y + h}, {o.x + nose, o.y + h}, {o.x, o.y + h * 0.5f}};
    dl->AddConvexPolyFilled(pts, 5, skin_col(down ? mix(p.tag, ImVec4(1, 1, 1, 1), 0.3f) : p.tag));
    dl->AddPolyline(pts, 5, skin_col(p.tag_edge), ImDrawFlags_Closed, std::max(1.0f, h * 0.05f));
    const ImVec2 c(o.x + nose + (w - nose) * 0.28f, o.y + h * 0.5f);
    dl->AddCircleFilled(c, h * 0.3f, skin_col(p.tag_inner), 20);
    const ImVec2 ts = ImGui::CalcTextSize("B");
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), skin_col(p.tag_text), "B");
    const ImVec2 a(o.x + w * 0.68f, o.y + h * 0.5f);
    dl->AddLine(ImVec2(a.x - h * 0.16f, a.y), ImVec2(a.x + h * 0.14f, a.y), skin_col(p.tag_inner), std::max(1.5f, h * 0.09f));
    dl->AddTriangleFilled(ImVec2(a.x - h * 0.28f, a.y), ImVec2(a.x - h * 0.10f, a.y - h * 0.13f), ImVec2(a.x - h * 0.10f, a.y + h * 0.13f), skin_col(p.tag_inner));
  }
  const char* label = tr("Back");
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(o.x - ts.x - px(10), o.y + (h - ts.y) * 0.5f), skin_col(p.bar_text), label);
  if (status && *status) {
    const float s = ImGui::GetFontSize() * 0.82f;
    const ImVec2 ss = ImGui::GetFont()->CalcTextSizeA(s, 1e9f, 0.0f, status);
    dl->AddText(ImGui::GetFont(), s, ImVec2(r_max.x - px(18) - ss.x, r_max.y - ss.y - px(10)), skin_col(status_good ? p.good : p.bar_dim), status);
  }
  return hit;
}

void skin_open_started() { g_open_t = now_s(); }
// After L / R: the pad cursor goes to the new page's first row. Called right after the menu window's Begin, before any item is submitted, so ImGui's own
// nav init picks the first navigable item (the tab strip is NoNav); the next d-pad move then starts from a normal row.
static bool g_cursor_first = false;
void skin_cursor_first_row() { g_cursor_first = true; }
void skin_cursor_apply() {
  if (!g_cursor_first) return;
  g_cursor_first = false;
  ImGuiContext& g = *GImGui;
  if (g.NavWindow != g.CurrentWindow) ImGui::SetNavWindow(g.CurrentWindow);
  ImGui::NavInitWindow(g.CurrentWindow, true);
  ImGui::SetNavCursorVisible(true);
}
float skin_open_factor() { return skin_ease_out((float)((now_s() - g_open_t) / 0.16)); }

}  // namespace snap::ui
