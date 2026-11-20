// SPDX-License-Identifier: MIT
// The Album page of the themed launcher. Included by launcher_ui.cpp inside its anonymous namespace (after help_bar() and draw_home_bg()), so
// it uses the same drawing helpers, the same prompts and the same pad and mouse rules as the Mods page. The save is read through host/album.h
// and is never written; the only files this page creates are the PNG, JSON and CSV files of an export, in a folder the player picks.
// launcher_ui.cpp includes album.h, <shobjidl.h> and <unordered_map> at the top.

// ---------------------------------------------------------------------------------------------------------------- textures
struct ATex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };

void a_free(ATex& t) {
  if (t.srv) t.srv->Release();
  t = ATex();
}

bool a_make(const album::Bitmap& b, ATex& t) {
  a_free(t);
  if (!g.dev || b.w <= 0 || b.h <= 0 || b.w > 16384 || b.h > 16384) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (UINT)b.w; d.Height = (UINT)b.h; d.MipLevels = 1; d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{};
  sd.pSysMem = b.rgba.data(); sd.SysMemPitch = (UINT)b.w * 4;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g.dev->CreateTexture2D(&d, &sd, &tex))) return false;
  const HRESULT hr = g.dev->CreateShaderResourceView(tex, nullptr, &t.srv);
  tex->Release();
  if (FAILED(hr)) { t.srv = nullptr; return false; }
  t.w = b.w; t.h = b.h;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- state
struct AlbumUi {
  bool loaded = false, matched = false;
  album::Library lib;
  std::string key;                 // the save folder the library was read from
  std::vector<int> shown;          // indexes into lib.photos that pass the filters
  std::vector<int> course_opts, species_opts;   // ids that occur in the album
  int f[4] = {0, 0, 0, 0};         // filter positions: course, Pokemon, stars, score
  int fcol = 0;
  bool in_filters = false;
  int sel = 0, top = 0;            // sel indexes shown; top = first visible grid row
  bool detail = false;
  std::unordered_map<int, ATex> thumbs;
  std::unordered_map<int, long> used;
  long frame = 0;
  ATex prev, big;                  // the panel preview and the full-size view
  int prev_slot = -1, prev_wait = 0, big_slot = -1;
  std::string msg;
  double msg_until = 0;
  bool exporting = false;
  album::ExportJob job;
} A;

const int kScoreMin[5] = {0, 1000, 2500, 5000, 10000};
const char* const kFilterName[4] = {"Course", "Pokemon", "Stars", "Score"};

std::string a_num(int n) {
  std::string s = std::to_string(n < 0 ? -n : n), o;
  for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) o += ','; o += s[i]; }
  return n < 0 ? "-" + o : o;
}

std::string a_save_root() {
  const UiInfo& in = *g.info;
  if (!in.profiles_root.empty() && !in.profile_id.empty()) return in.profiles_root + "\\" + in.profile_id + "\\save";
  return in.save_noprofile;
}

void a_msg(const std::string& t) { A.msg = t; A.msg_until = ImGui::GetTime() + 4.0; }

void a_clear_textures() {
  for (auto& kv : A.thumbs) a_free(kv.second);
  A.thumbs.clear(); A.used.clear();
  a_free(A.prev); a_free(A.big);
  A.prev_slot = A.big_slot = -1;
}

int a_opt_count(int col) {
  switch (col) {
    case 0: return 1 + (int)A.course_opts.size();
    case 1: return 1 + (int)A.species_opts.size();
    case 2: return 6;
    default: return 5;
  }
}

std::string a_opt_label(int col, int i) {
  switch (col) {
    case 0: return i == 0 ? "All courses" : album::course_name(A.lib, A.course_opts[(size_t)i - 1]);
    case 1: return i == 0 ? "All Pokemon" : album::species_name(A.lib, A.species_opts[(size_t)i - 1]);
    case 2: return i == 0 ? "Any stars" : i == 5 ? "Not rated" : std::to_string(i) + (i == 1 ? " star" : " stars");
    default: return i == 0 ? "Any score" : a_num(kScoreMin[i]) + " and up";
  }
}

bool a_passes(const album::Photo& p) {
  if (A.f[0] && p.course != A.course_opts[(size_t)A.f[0] - 1]) return false;
  if (A.f[1] && p.species != A.species_opts[(size_t)A.f[1] - 1]) return false;
  if (A.f[2] == 5) { if (p.rated) return false; }
  else if (A.f[2] && p.stars != A.f[2]) return false;
  if (A.f[3] && p.score < kScoreMin[A.f[3]]) return false;
  return true;
}

void a_rebuild() {
  const int keep = A.sel >= 0 && A.sel < (int)A.shown.size() ? A.shown[(size_t)A.sel] : -1;
  A.shown.clear();
  for (size_t i = 0; i < A.lib.photos.size(); ++i) if (a_passes(A.lib.photos[i])) A.shown.push_back((int)i);
  A.sel = 0;
  for (size_t i = 0; i < A.shown.size(); ++i) if (A.shown[i] == keep) { A.sel = (int)i; break; }
  A.top = std::max(0, A.sel / 4 - 1);
}

// the filter lists follow what the album holds; the chosen value is kept when it is still there
void a_options() {
  int cur_course = A.f[0] ? A.course_opts[(size_t)A.f[0] - 1] : 0, cur_sp = A.f[1] ? A.species_opts[(size_t)A.f[1] - 1] : 0;
  std::vector<int> cs, sp;
  for (const album::Photo& p : A.lib.photos) {
    if (p.course && std::find(cs.begin(), cs.end(), p.course) == cs.end()) cs.push_back(p.course);
    if (p.species && std::find(sp.begin(), sp.end(), p.species) == sp.end()) sp.push_back(p.species);
  }
  std::sort(cs.begin(), cs.end());
  std::sort(sp.begin(), sp.end(), [](int a, int b) { return album::species_name(A.lib, a) < album::species_name(A.lib, b); });
  A.course_opts = cs; A.species_opts = sp;
  A.f[0] = A.f[1] = 0;
  for (size_t i = 0; i < cs.size(); ++i) if (cs[i] == cur_course) A.f[0] = (int)i + 1;
  for (size_t i = 0; i < sp.size(); ++i) if (sp[i] == cur_sp) A.f[1] = (int)i + 1;
}

void a_cycle(int col, int dir) {
  const int n = a_opt_count(col);
  A.f[col] = ((A.f[col] + dir) % n + n) % n;
  a_rebuild();
}

std::string album_home_sub() {
  return A.loaded && A.lib.error.empty() ? std::to_string(A.lib.photos.size()) + (A.lib.photos.size() == 1 ? " photo" : " photos") : "";
}

// Opens (or re-reads) the album of the chosen profile. The library is kept while the save has not changed.
void album_open() {
  const std::string root = a_save_root();
  A.detail = false; A.in_filters = false;
  std::string stamp = root;
  {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::u8path(root), ec)) {
      if (!e.is_directory(ec)) continue;
      if (GetFileAttributesExW((e.path() / L"CURRENT").c_str(), GetFileExInfoStandard, &fa))
        stamp += "|" + std::to_string(((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
      break;
    }
  }
  if (A.loaded && A.key == stamp && A.lib.error.empty()) { a_rebuild(); return; }
  a_clear_textures();
  A = AlbumUi();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the folder picker and the picture decoder; harmless when already set up
  album::Inputs in;
  const UiInfo& ui = *g.info;
  in.save_root = root; in.main_elf = ui.main_elf; in.romfs_root = ui.romfs_root;
  const char* pd = std::getenv("SNAP_PHOTO_DIR");
  in.png_dir = pd && *pd ? pd : ui.data_dir + "\\photos";
  A.loaded = true;
  A.key = stamp;
  if (!album::load(in, A.lib)) { A.matched = true; return; }
  a_options();
  a_rebuild();
}

// ---------------------------------------------------------------------------------------------------------------- pictures
ATex* a_thumb(int slot, int& budget) {
  auto it = A.thumbs.find(slot);
  if (it != A.thumbs.end()) { A.used[slot] = A.frame; return &it->second; }
  if (budget <= 0) return nullptr;
  --budget;
  ATex t;
  std::vector<uint8_t> j;
  album::Bitmap b;
  if (album::read_jpeg(A.lib, slot, true, j) && album::decode_image(j.data(), j.size(), b)) a_make(b, t);
  A.used[slot] = A.frame;
  return &(A.thumbs[slot] = t);
}

void a_prune() {
  if (A.thumbs.size() < 200) return;
  for (auto it = A.thumbs.begin(); it != A.thumbs.end();) {
    if (A.used[it->first] < A.frame - 300) { a_free(it->second); A.used.erase(it->first); it = A.thumbs.erase(it); }
    else ++it;
  }
}

bool a_load_full(int slot, ATex& t, bool best) {
  const album::Photo* p = nullptr;
  for (const album::Photo& q : A.lib.photos) if (q.slot == slot) { p = &q; break; }
  if (!p) return false;
  std::vector<uint8_t> j;
  album::Bitmap b;
  bool ok = false;
  if (best) ok = album::best_image(A.lib, *p, j, nullptr);
  else ok = album::read_jpeg(A.lib, slot, false, j);
  return ok && album::decode_image(j.data(), j.size(), b) && a_make(b, t);
}

void a_image(const ATex& t, float x, float y, float w, float h, float r, ImU32 col = IM_COL32_WHITE) {
  if (!t.srv) return;
  ImGui::GetForegroundDrawList()->AddImageRounded(t.srv, V(x, y), V(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), col, r * g.s);
}

void a_star(float cx, float cy, float r, ImU32 col) {
  ImVec2 p[10];
  for (int i = 0; i < 10; ++i) {
    const float a = -1.5707963f + (float)i * 0.62831853f, rr = (i & 1) ? r * 0.45f : r;
    p[i] = V(cx + std::cos(a) * rr, cy + std::sin(a) * rr);
  }
  ImGui::GetForegroundDrawList()->AddConcavePolyFilled(p, 10, col);
}

void a_stars(float x, float cy, float r, int have, ImU32 on, ImU32 off) {
  for (int k = 0; k < 4; ++k) a_star(x + r + (float)k * (r * 2.3f), cy, r, k < have ? on : off);
}

// ---------------------------------------------------------------------------------------------------------------- export
bool a_pick_folder(std::string& out) {
  IFileDialog* d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
  DWORD fl = 0;
  d->GetOptions(&fl);
  d->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(L"Choose a folder for the exported photos");
  bool ok = false;
  if (SUCCEEDED(d->Show(g.hwnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->GetResult(&it))) {
      PWSTR w = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        out.assign((size_t)std::max(n - 1, 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
        CoTaskMemFree(w);
        ok = !out.empty();
      }
      it->Release();
    }
  }
  d->Release();
  return ok;
}

void a_export(const std::vector<int>& slots) {
  if (A.exporting || slots.empty()) return;
  std::string dest;
  const char* e = std::getenv("SNAP_ALBUM_EXPORT_DIR");   // tests: skips the folder picker
  if (e && *e) dest = e;
  else if (!a_pick_folder(dest)) return;
  if (!album::export_begin(A.job, dest, slots)) { a_msg(A.job.error.empty() ? "The export folder could not be used." : A.job.error); return; }
  A.exporting = true;
}

void a_export_shown() {
  std::vector<int> s;
  for (int i : A.shown) s.push_back(A.lib.photos[(size_t)i].slot);
  a_export(s);
}
void a_export_selected() {
  if (A.sel >= 0 && A.sel < (int)A.shown.size()) a_export({A.lib.photos[(size_t)A.shown[(size_t)A.sel]].slot});
}

void a_export_tick() {
  if (!A.exporting) return;
  if (!album::export_step(A.lib, A.job, 30.0)) return;
  A.exporting = false;
  a_msg("Exported " + std::to_string(A.job.exported) + (A.job.exported == 1 ? " photo" : " photos") + (A.job.failed ? " (" + std::to_string(A.job.failed) + " failed)" : "") +
        " to " + A.job.dest);
}

// ---------------------------------------------------------------------------------------------------------------- drawing
const album::Photo* a_cur() {
  if (A.sel < 0 || A.sel >= (int)A.shown.size()) return nullptr;
  return &A.lib.photos[(size_t)A.shown[(size_t)A.sel]];
}

std::string a_sources(const album::Photo& p) {
  std::string s = "Album";
  if (!p.hd_path.empty()) s += ", HD copy";
  if (!p.png_path.empty()) s += ", PNG export";
  return s;
}

void a_info_lines(const album::Photo& p, float x, float y, float w, bool big) {
  const float s1 = big ? 30 : 25;
  const bool known = p.species != 0;
  text_clip(x, y, s1, C(known ? 0x5a5550 : kGrey), known ? album::species_name(A.lib, p.species).c_str() : "Not rated", w);
  y += big ? 34 : 30;
  if (p.course) {
    const std::string isl = album::island_name(A.lib, p.course), cn = album::course_name(A.lib, p.course);
    text_clip(x, y, 17, C(0x6a6561), (isl.empty() ? cn : isl + "  -  " + cn).c_str(), w);
  } else text(x, y, 17, C(kGrey), known ? "Course not recorded" : "No course recorded");
  y += big ? 38 : 32;
  a_stars(x, y, big ? 13.0f : 10.0f, p.stars, C(kYellow), C(0xd6d3d0));
  if (!p.stars) text(x + (big ? 130 : 100), y, 15, C(kGrey), "Stars not recorded");
  y += big ? 40 : 30;
  text(x, y, 15, C(kGrey), "Score");
  text(x + 56, y, big ? 30 : 23, C(p.score >= 0 ? 0x5a5550 : kGrey), p.score >= 0 ? a_num(p.score).c_str() : "not recorded");
}

void draw_album_detail(bool click, bool moved) {
  const album::Photo* p = a_cur();
  if (!p) { A.detail = false; return; }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (A.big_slot != p->slot) { A.big_slot = p->slot; if (!a_load_full(p->slot, A.big, true)) a_free(A.big); }
  const float ix = 40, iy = 34, iw = 870, ih = 580;
  shadow(ix, iy, iw, ih, 18);
  rrect(ix, iy, iw, ih, 18, C(0xdedcda));
  a_image(A.big, ix, iy, iw, ih, 18);
  if (!A.big.srv) text(ix + iw / 2, iy + ih / 2, 22, C(kGrey), "This photo could not be read", 1);
  // previous and next
  const int n = (int)A.shown.size();
  text(ix + iw / 2, iy + ih + 22, 16, C(kGrey), ("Photo " + std::to_string(A.sel + 1) + " of " + std::to_string(n) + "   -   left and right for the others").c_str(), 1);
  for (int side = 0; side < 2; ++side) {
    const float bx = side ? ix + iw - 54 : ix + 14, by = iy + ih / 2 - 28;
    if (hot(bx, by, 40, 56) && click) { A.sel = (A.sel + (side ? 1 : n - 1)) % n; }
    if (hot(bx, by, 40, 56)) dl->AddRectFilled(V(bx, by), V(bx + 40, by + 56), C(0x000000, 70), 10 * g.s);
    ImVec2 t[3] = {V(side ? bx + 12 : bx + 28, by + 14), V(side ? bx + 28 : bx + 12, by + 28), V(side ? bx + 12 : bx + 28, by + 42)};
    dl->AddPolyline(t, 3, C(0xffffff, 230), 0, 4 * g.s);
  }
  // the facts
  const float px = 934, pw = 306;
  shadow(px, 34, pw, 580, 22);
  rrect(px, 34, pw, 580, 22, C(0xffffff));
  rrect(px, 34, pw, 56, 22, C(0x96c283), ImDrawFlags_RoundCornersTop);
  text(px + 24, 62, 22, C(0xffffff), "Photo details");
  a_info_lines(*p, px + 24, 128, pw - 48, true);
  float y = 328;
  text(px + 24, y, 15, C(kGrey), "Score breakdown");
  y += 14;
  text_wrap2(px + 24, y + 22, 15, C(0x6a6561),
             p->rated ? "The save keeps only the total score of a best shot, not the points for size, pose, direction and placement."
                      : "This photo is not one of your Photodex best shots, so the save holds no score for it.",
             pw - 48);
  y = 436;
  std::string when = p->taken.empty() ? "Order " + std::to_string((int)A.lib.photos.size() - (int)(p - &A.lib.photos[0])) + " of " + std::to_string(A.lib.photos.size()) + " (the save has no date)" : "Taken " + p->taken;
  text_clip(px + 24, y, 15, C(0x6a6561), when.c_str(), pw - 48);
  text_clip(px + 24, y + 26, 15, C(0x6a6561), ("Slot " + std::to_string(p->slot + 1) + "  -  " + a_sources(*p)).c_str(), pw - 48);
  if (!p->caption.empty()) text_wrap2(px + 24, y + 70, 15, C(0x7a746e), "\"" + p->caption + "\"", pw - 48);
  // buttons
  const float by = 548;
  for (int b = 0; b < 2; ++b) {
    const float bx = px + 24 + b * 150, bw = b ? 108 : 142;
    const bool h = hot(bx, by, bw, 40);
    rrect(bx, by, bw, 40, 20, C(h ? kYellow : 0xeeece9));
    text(bx + bw / 2, by + 20, 16, C(kBrown), b ? "Back" : "Export photo", 1);
    if (h && click) { if (b) A.detail = false; else a_export_selected(); }
  }
  (void)moved;
  help_bar("Left and right show the other photos. X exports this photo as a PNG with a JSON and CSV list. B goes back to the grid.");
}

void draw_album() {
  draw_home_bg();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ++A.frame;
  const bool moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
  const bool click = ImGui::IsMouseClicked(0) && !A.exporting;
  const bool rclick = ImGui::IsMouseClicked(1) && !A.exporting;
  if (A.loaded && !A.matched && A.lib.error.empty() && album::step(A.lib, 8.0)) { A.matched = true; a_options(); a_rebuild(); }
  a_export_tick();
  if (A.exporting) g.block_hot = true;
  if (A.detail) {
    draw_album_detail(click, moved);
  } else {
    diamond(122, 82, 46, C(0x8f8e90));
    aperture(122, 82, 22, C(kCream));
    text(180, 70, 42, C(0x6a6561), "Album");
    std::string sub;
    if (!A.lib.error.empty()) sub = "Your photos from the game's album";
    else {
      sub = std::to_string(A.lib.photos.size()) + (A.lib.photos.size() == 1 ? " photo" : " photos") + " in the save";
      if (!A.matched && A.lib.total > 0) sub += "  -  reading ratings " + std::to_string(A.lib.done * 100 / A.lib.total) + "%";
    }
    text(182, 106, 17, C(kGrey), sub.c_str());
    const int n = (int)A.shown.size();
    if (!A.lib.error.empty() || A.lib.photos.empty()) {
      shadow(84, 150, 740, 170, 22);
      rrect(84, 150, 740, 170, 22, C(0xffffff));
      text(114, 190, 24, C(0x6a6561), !A.lib.error.empty() ? "The album could not be read" : "No photos yet");
      text_wrap2(114, 236, 18, C(kGrey), !A.lib.error.empty() ? A.lib.error : "Take photos in the game and keep them in your album. They show up here, with their rating when they are a Photodex best shot.", 690);
    } else {
      // filters
      for (int c = 0; c < 4; ++c) {
        const float x = 84 + c * 188, y = 128, w = 176, h = 48;
        const bool sel = A.in_filters && A.fcol == c;
        if (hot(x, y, w, h)) {
          if (moved) { A.in_filters = true; A.fcol = c; }
          if (click) { A.in_filters = true; A.fcol = c; a_cycle(c, +1); }
          if (rclick) { A.in_filters = true; A.fcol = c; a_cycle(c, -1); }
        }
        shadow(x, y, w, h, 18);
        rrect(x, y, w, h, 18, C(sel ? kYellow : 0xffffff));
        if (sel) { pattern(x + 18, y, w - 36, h); dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 18 * g.s, 0, 3.5f * g.s); }
        text(x + 18, y + 14, 12, C(sel ? 0x7a6440 : 0x9a9795), kFilterName[c]);
        text_clip(x + 18, y + 33, 17, C(A.f[c] ? 0x4f7f55 : sel ? kBrown : 0x6a6561), a_opt_label(c, A.f[c]).c_str(), w - 32);
      }
      // grid
      const int cols = 4, rows = 3;
      const float gx = 84, gy = 190, cw = 176, ch = 118, gap = 12;
      const int total_rows = (n + cols - 1) / cols;
      if (!A.in_filters && !A.detail && hot(84, 190, 740, 400)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0) A.top -= 1;
        if (wheel < 0) A.top += 1;
      }
      if (!A.in_filters && A.sel >= 0) { const int r = A.sel / cols; if (r < A.top) A.top = r; if (r >= A.top + rows) A.top = r - rows + 1; }
      A.top = std::clamp(A.top, 0, std::max(0, total_rows - rows));
      int budget = 8;
      for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
          const int idx = (A.top + r) * cols + c;
          if (idx >= n) continue;
          const album::Photo& p = A.lib.photos[(size_t)A.shown[(size_t)idx]];
          const float x = gx + c * (cw + gap), y = gy + r * (ch + gap);
          const bool sel = !A.in_filters && idx == A.sel;
          if (hot(x, y, cw, ch)) {
            if (moved) { A.sel = idx; A.in_filters = false; }
            if (click) { A.sel = idx; A.in_filters = false; A.detail = true; }
          }
          shadow(x, y, cw, ch, 12);
          rrect(x, y, cw, ch, 12, C(0xdedcda));
          if (ATex* t = a_thumb(p.slot, budget)) a_image(*t, x, y, cw, ch, 12);
          if (p.rated) {
            dl->AddRectFilled(V(x, y + ch - 26), V(x + cw, y + ch), C(0x2c2a28, 150), 12 * g.s, ImDrawFlags_RoundCornersBottom);
            a_stars(x + 8, y + ch - 13, 6.0f, p.stars, C(kYellow), C(0xffffff, 70));
            text(x + cw - 10, y + ch - 13, 14, C(0xffffff), a_num(p.score).c_str(), 2);
          }
          if (!p.hd_path.empty() || !p.png_path.empty()) {
            rrect(x + 8, y + 8, 28, 17, 8, C(0x2c2a28, 150));
            text(x + 22, y + 16.5f, 11, C(0xffffff), "HD", 1);
          }
          if (sel) dl->AddRect(V(x - 2, y - 2), V(x + cw + 2, y + ch + 2), C(kOrange), 14 * g.s, 0, 4 * g.s);
        }
      if (n == 0) {
        shadow(84, 190, 740, 150, 22);
        rrect(84, 190, 740, 150, 22, C(0xffffff));
        text(114, 230, 24, C(0x6a6561), "No photo matches these filters");
        text_wrap2(114, 276, 18, C(kGrey), "Click a filter or press Q or E on it to change it.", 690);
      }
      // scroll bar and page text
      if (total_rows > rows) {
        const float th = 3 * (ch + gap) - gap, bh = std::max(30.0f, th * rows / total_rows), by = gy + (th - bh) * ((float)A.top / (float)(total_rows - rows));
        rrect(832, gy, 6, th, 3, C(0xdad7d3));
        rrect(832, by, 6, bh, 3, C(0x9a9795));
      }
      if (n > 0) {
        const int a = A.top * cols + 1, b = std::min(n, (A.top + rows) * cols);
        text(84, 596, 15, C(kGrey), ("Showing " + std::to_string(a) + " to " + std::to_string(b) + " of " + std::to_string(n)).c_str());
      }
      // the panel
      const album::Photo* p = a_cur();
      const float px = 856, py = 128, pw = 340, ph = 508;
      shadow(px, py, pw, ph, 26);
      rrect(px, py, pw, ph, 26, C(0xffffff));
      if (p) {
        if (A.prev_slot != p->slot) { if (++A.prev_wait > 6) { A.prev_slot = p->slot; A.prev_wait = 0; if (!a_load_full(p->slot, A.prev, false)) a_free(A.prev); } }
        else A.prev_wait = 0;
        rrect(px + 20, py + 20, 300, 200, 14, C(0xdedcda));
        if (A.prev_slot == p->slot && A.prev.srv) a_image(A.prev, px + 20, py + 20, 300, 200, 14);
        else { int b0 = 1; if (ATex* t = a_thumb(p->slot, b0)) a_image(*t, px + 20, py + 20, 300, 200, 14); }
        a_info_lines(*p, px + 22, py + 248, pw - 44, false);
        std::string when = p->taken.empty() ? "Order " + std::to_string((int)A.lib.photos.size() - (int)(p - &A.lib.photos[0])) + " of " + std::to_string(A.lib.photos.size()) : "Taken " + p->taken;
        text_clip(px + 22, py + 372, 15, C(0x6a6561), (when + "  -  slot " + std::to_string(p->slot + 1)).c_str(), pw - 44);
        text_clip(px + 22, py + 396, 15, C(kGrey), a_sources(*p).c_str(), pw - 44);
        if (!p->caption.empty()) text_wrap2(px + 22, py + 428, 15, C(0x7a746e), "\"" + p->caption + "\"", pw - 44);
      }
      const char* const bl[3] = {"Open", "Export photo", "Export all shown"};
      const float bw[3] = {70, 118, 124};
      float bx = px + 20;
      for (int b = 0; b < 3; ++b) {
        const float by = py + ph - 52;
        const bool h = hot(bx, by, bw[b], 36);
        rrect(bx, by, bw[b], 36, 18, C(h ? kYellow : 0xeeece9));
        text(bx + bw[b] / 2, by + 18, 15, C(kBrown), bl[b], 1);
        if (h && click) { if (b == 0 && p) A.detail = true; else if (b == 1) a_export_selected(); else if (b == 2) a_export_shown(); }
        bx += bw[b] + 8;
      }
    }
    a_prune();
    help_bar(A.in_filters ? "Left and right pick a filter. Enter or E shows the next value, Q the one before. Down goes to the photos."
                          : "Enter opens a photo. X exports it, Y exports every photo shown, as PNG files with a JSON and CSV list. Q and E turn the page.");
  }
  // messages and the export progress card
  if (A.exporting) {
    g.block_hot = false;
    const float w = 560, h = 120, x = 360, y = 300;
    dl->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, C(0x000000, 90));
    shadow(x, y, w, h, 22);
    rrect(x, y, w, h, 22, C(0xffffff));
    text(x + 28, y + 36, 22, C(0x6a6561), "Exporting photos");
    text_clip(x + 28, y + 70, 15, C(kGrey), A.job.dest.c_str(), w - 56);
    text(x + w - 28, y + 36, 20, C(0x5f8a62), (std::to_string(A.job.next) + " of " + std::to_string(A.job.slots.size())).c_str(), 2);
    rrect(x + 28, y + 90, w - 56, 8, 4, C(0xe3e0dc));
    rrect(x + 28, y + 90, (w - 56) * (A.job.slots.empty() ? 1.0f : (float)A.job.next / (float)A.job.slots.size()), 8, 4, C(0x96c283));
  }
  if (ImGui::GetTime() < A.msg_until) {
    const float w = std::min(1100.0f, text_w(18, A.msg.c_str()) + 48);
    rrect(640 - w / 2, 600, w, 40, 20, C(0x4f3d29, 235));
    text_clip(640, 620, 18, C(0xffffff), A.msg.c_str(), w - 40, 1);
  }
}

// keys and the pad; returns after handling one page's worth of input
void album_input(bool up, bool dn, bool lf, bool rt, bool ok, bool back, bool prev, bool next, bool x, bool y) {
  if (A.exporting) return;
  const int n = (int)A.shown.size();
  if (A.detail) {
    if (back) { A.detail = false; return; }
    if (n > 0 && (lf || prev)) A.sel = (A.sel + n - 1) % n;
    if (n > 0 && (rt || next)) A.sel = (A.sel + 1) % n;
    if (x || y || ok) { if (ok && !x && !y) A.detail = false; else a_export_selected(); }
    return;
  }
  if (back) { g.view = G::HOME; return; }
  if (A.lib.photos.empty()) return;
  if (A.in_filters) {
    if (lf) A.fcol = (A.fcol + 3) % 4;
    if (rt) A.fcol = (A.fcol + 1) % 4;
    if (ok || next) a_cycle(A.fcol, +1);
    if (prev) a_cycle(A.fcol, -1);
    if (dn && n > 0) A.in_filters = false;
    if (x) a_export_selected();
    if (y) a_export_shown();
    return;
  }
  if (up) { if (A.sel < 4) A.in_filters = true; else A.sel -= 4; }
  if (dn && n > 0) { if (A.sel + 4 < n) A.sel += 4; else if ((A.sel / 4) < (n - 1) / 4) A.sel = n - 1; }
  if (lf && A.sel > 0) --A.sel;
  if (rt && A.sel + 1 < n) ++A.sel;
  if (prev) A.sel = std::max(0, A.sel - 12);
  if (next && n > 0) A.sel = std::min(n - 1, A.sel + 12);
  if (ok && n > 0) A.detail = true;
  if (x) a_export_selected();
  if (y) a_export_shown();
}

// test pictures: album | album:detail:<n> | album:sel:<n> | album:filter:<column>:<value>
void album_test_view(const std::string& v) {
  album_open();
  while (A.loaded && A.lib.error.empty() && !A.matched) { if (album::step(A.lib, 1e9)) { A.matched = true; a_options(); a_rebuild(); } }
  int a = 0, b = 0;
  g.view = G::ALBUM;
  A.prev_wait = 7;   // load the panel's full-size preview on the first frame
  if (std::sscanf(v.c_str(), "album:filter:%d:%d", &a, &b) == 2 && a >= 0 && a < 4) { A.f[a] = std::clamp(b, 0, a_opt_count(a) - 1); a_rebuild(); A.in_filters = true; A.fcol = a; }
  else if (std::sscanf(v.c_str(), "album:detail:%d", &a) == 1) { A.sel = std::clamp(a, 0, std::max(0, (int)A.shown.size() - 1)); A.detail = true; }
  else if (std::sscanf(v.c_str(), "album:sel:%d", &a) == 1) A.sel = std::clamp(a, 0, std::max(0, (int)A.shown.size() - 1));
}
