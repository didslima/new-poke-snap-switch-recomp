// Launcher mode of the game exe (builder release): started with no arguments, next to a config.json, the exe asks Play / Change game
// folder / Open save folder, checks the romfs sentinels at every start, and fills in --main, --sdk and the romfs path from config.json.
// Without a config.json next to the exe nothing here runs (the dev build keeps its usage message).
//
// config.json (written by builder/snapbuild/pipeline.py): romfs_root, main_elf, sdk_elf. launcher_data.json (same folder): the
// published romfs check ("sentinels": SHA-256 of the lowercase relative path and the exact size of 32 files).
// Test switches: SNAP_LAUNCHER_AUTOPLAY=1 skips the dialog; SNAP_BOOT_TRACE=<file> sets the boot trace file.
#include <windows.h>
#include "benchmark.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hle/hle_config.h"
#include "hle/profiles.h"
#include "hle/json_lite.h"
#include "hle/save_backup.h"
#include "hle/save_port.h"
#include "launcher.h"
#include "cli_flags.h"
#include "mods/problem_report.h"
#include "mods/install_check.h"
#include "elf_check.h"
#include "launcher_ui.h"
#include "loader.h"
#include "ui_font.h"

namespace fs = std::filesystem;

namespace {

std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::string sha256_hex(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
  unsigned char h[32];
  BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32);
  BCryptCloseAlgorithmProvider(alg, 0);
  static const char* hx = "0123456789abcdef";
  std::string out;
  for (unsigned char c : h) { out += hx[c >> 4]; out += hx[c & 15]; }
  return out;
}

bool read_json(const fs::path& p, hle::Json& j) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string err;
  return hle::json_parse(ss.str(), j, err);
}

void message(const std::string& title, const std::string& text, UINT icon = MB_ICONINFORMATION) {
  MessageBoxW(nullptr, W(text).c_str(), W(title).c_str(), MB_OK | icon | MB_SETFOREGROUND);
}

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s;
}

}  // namespace

extern "C" void snap_crashlog_rotate(const wchar_t* logs_dir);   // host/mods/crashlog.cpp
extern "C" void snap_crashlog_set_dir(const char* utf8_dir);
extern "C" int snap_launchmark_check(char* why, int cap);   // host/mods/crashlog.cpp: 0 none, 1 the last start never reached the title screen, 2 and it was a safe-mode start

namespace snap_launcher {

// A check file that matched, kept so the next start can stat just these instead of walking the whole folder.
struct Matched { std::string rel; long long size = 0, mtime = 0; };

// Returns an empty string when the romfs folder passes, else the message to show. The walk stops as soon as every check file has matched.
// Modified time in milliseconds. The remembered result stores it as a JSON number (a double), and the file clock's own 100 ns count is too big for that.
static long long mtime_ms(fs::file_time_type t) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

std::string check_romfs(const std::string& root, const hle::Json& data, std::atomic<int>* files_seen = nullptr, std::vector<Matched>* matched = nullptr) {
  std::error_code ec;
  if (!fs::is_directory(fs::u8path(root), ec))
    return "The game folder cannot be found:\n" + root + "\n\nIs the drive or network share connected? Use \"Change game folder\" to pick it again.";
  const hle::Json* sents = data.get("sentinels");
  if (!sents || sents->a.empty()) return "";   // no published check shipped: nothing to compare
  std::map<std::string, long long> want;
  for (auto& s : sents->a) {
    const hle::Json* k = s.get("path_sha256");
    const hle::Json* z = s.get("size");
    if (k && z) want[k->str()] = (long long)z->num();
  }
  size_t ok = 0, files = 0;
  fs::path base = fs::u8path(root);
  for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    ++files;
    if (files_seen && (files & 63) == 0) files_seen->store((int)files, std::memory_order_relaxed);
    const std::string rel_raw = U8(fs::relative(it->path(), base, e2).generic_wstring());
    auto f = want.find(sha256_hex(lower(rel_raw)));
    if (f != want.end() && (long long)it->file_size(e2) == f->second) {
      ++ok;
      if (matched) matched->push_back({rel_raw, f->second, mtime_ms(it->last_write_time(e2))});
      if (ok == want.size()) break;   // every check file is there: the rest of the folder does not matter
    }
  }
  if (ok == want.size()) return "";
  if (files == 0) return "The game folder is empty:\n" + root + "\n\nUse \"Change game folder\" to pick the extracted romfs again.";
  return "The game folder does not look like the 2.0.1 romfs this build was made from: " + std::to_string(want.size() - ok) + " of " +
         std::to_string(want.size()) + " check files are missing or changed.\n" + root + "\n\nUse \"Change game folder\" to pick the right folder.";
}

static std::string pick_folder(const std::string& start, const wchar_t* title = L"Pick the folder with the extracted game romfs") {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  if (!start.empty()) {
    IShellItem* si = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(W(start).c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); }
  }
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

static std::string pick_zip() {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  COMDLG_FILTERSPEC ft[1] = {{L"Save backup (.zip)", L"*.zip"}};
  dlg->SetFileTypes(1, ft);
  dlg->SetTitle(L"Pick the save backup (.zip)");
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

static std::string pick_save_zip() {
  std::string out;
  IFileSaveDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
  COMDLG_FILTERSPEC ft[1] = {{L"Save backup (.zip)", L"*.zip"}};
  dlg->SetFileTypes(1, ft);
  dlg->SetDefaultExtension(L"zip");
  dlg->SetFileName(L"New Pokemon Snap save.zip");
  dlg->SetTitle(L"Export the save as a .zip");
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// A task dialog of command links. Returns the index of the link picked, -1 on cancel.
static int choose(const std::wstring& title, const std::wstring& content, const std::vector<std::wstring>& links) {
  using TDI = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE cc = LoadLibraryW(L"comctl32.dll");
  TDI td = cc ? (TDI)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
  if (!td) return -1;
  std::vector<TASKDIALOG_BUTTON> b;
  for (size_t i = 0; i < links.size(); ++i) b.push_back({(int)(3000 + i), links[i].c_str()});
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = title.c_str();
  c.pszContent = content.c_str();
  c.pButtons = b.data();
  c.cButtons = (UINT)b.size();
  int sel = 0;
  if (FAILED(td(&c, &sel, nullptr, nullptr))) return -1;
  return sel >= 3000 && sel < 3000 + (int)links.size() ? sel - 3000 : -1;
}

// Loads the game's own config (the launcher's config.json is the same file) so the save folder is the one the game will use.
static bool save_target(const fs::path& cfg_path, std::string* root, std::string* label) {
  hle::load_config(U8(cfg_path.wstring()));
  *root = hle::saveport::target_save_root("", label);
  if (root->empty()) { message("Snap Recomp", "No save folder could be chosen: " + *label, MB_ICONWARNING); return false; }
  return true;
}

static void import_save_flow(const fs::path& cfg_path) {
  std::string root, label;
  if (!save_target(cfg_path, &root, &label)) return;
  auto cands = hle::saveport::detect_emulator_saves();
  std::vector<std::wstring> links;
  const size_t shown = std::min<size_t>(cands.size(), 6);
  for (size_t i = 0; i < shown; ++i) {
    std::string when = hle::profiles::format_time(cands[i].mtime);
    links.push_back(W(cands[i].label + "\nSaved " + when + (cands[i].old_version ? " (older game version)" : "")));
  }
  links.push_back(L"Pick a folder of save files...\nA JKSV or Checkpoint backup folder, or an emulator save folder");
  links.push_back(L"Pick a .zip backup...\nA JKSV backup zip");
  int sel = choose(L"Import a save into " + W(label),
                   shown ? L"Found saves from emulators on this PC. Your current save is kept as a backup." : L"Your current save is kept as a backup.", links);
  if (sel < 0) return;
  std::string path;
  if ((size_t)sel < shown) path = cands[(size_t)sel].path;
  else if ((size_t)sel == shown) path = pick_folder("", L"Pick the folder with the save files");
  else path = pick_zip();
  if (path.empty()) return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  hle::saveport::Report r;
  std::string err;
  bool running = false;
  { std::string be; if (!hle::savebackup::make(root, "import", nullptr, &be)) std::fprintf(stderr, "Backup before the import failed (%s); the import still keeps the current save as a backup generation.\n", be.c_str()); }
  bool ok = hle::saveport::import_save(path, root, hle::saveport::game_uid_dir(), &r, &err, &running);
  SetCursor(old);
  if (ok) message("Snap Recomp", hle::saveport::format_report(r, true), r.warnings.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
  else message("Snap Recomp", "The save was not imported.\n\n" + err, MB_ICONWARNING);
}

// "Restore a backup": the whole-save backups of runtime/hle/save_backup.h (one per start, one before each import or restore), newest first.
static void restore_backup_flow(const fs::path& cfg_path) {
  std::string root, label;
  if (!save_target(cfg_path, &root, &label)) return;
  const auto backups = hle::savebackup::list(root);
  if (backups.empty()) {
    message("Snap Recomp", "There are no backups of " + label + " yet.\n\nA backup is made each time the game starts and before every import or restore.", MB_ICONINFORMATION);
    return;
  }
  std::vector<std::wstring> links;
  const size_t shown = std::min<size_t>(backups.size(), 8);
  for (size_t i = 0; i < shown; ++i) {
    char mb[40];
    snprintf(mb, sizeof mb, "%.1f MB", (double)backups[i].bytes / (1024.0 * 1024.0));
    links.push_back(W(hle::savebackup::describe(backups[i]) + "\n" + mb));
  }
  const int sel = choose(L"Restore a backup of " + W(label),
                         L"Pick the backup to put back. Your current save is backed up first, and it stays available as an older generation.", links);
  if (sel < 0 || (size_t)sel >= shown) return;
  if (MessageBoxW(nullptr, W("Put back the backup from " + hle::savebackup::describe(backups[(size_t)sel]) + "?\n\nYour current save is backed up first.").c_str(), L"Snap Recomp",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  hle::saveport::Report r;
  std::string err;
  bool running = false;
  const bool ok = hle::savebackup::restore(root, backups[(size_t)sel].folder, &r, &err, &running);
  SetCursor(old);
  if (ok) message("Snap Recomp", "The backup was restored.\n\n" + hle::saveport::format_report(r, true), MB_ICONINFORMATION);
  else message("Snap Recomp", "The backup was not restored.\n\n" + err, MB_ICONWARNING);
}

static void export_save_flow(const fs::path& cfg_path) {
  std::string root, label;
  if (!save_target(cfg_path, &root, &label)) return;
  int how = choose(L"Export the save", L"Both are the plain save files, named the way the game names them.",
                   {L"To a folder...\nFor Checkpoint, or to copy into an emulator", L"To a .zip file...\nFor JKSV"});
  if (how < 0) return;
  std::string dest = how == 0 ? pick_folder("", L"Pick the folder to export the save to") : pick_save_zip();
  if (dest.empty()) return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  hle::saveport::Report r;
  std::string err;
  bool ok = hle::saveport::export_save(root, "", dest, &r, &err);
  SetCursor(old);
  if (ok) message("Snap Recomp", hle::saveport::format_report(r, false), MB_ICONINFORMATION);
  else message("Snap Recomp", "The save was not exported.\n\n" + err, MB_ICONWARNING);
}

// Portable mode (host/portable.cpp sets SNAP_PORTABLE=1 when portable.txt sits next to the exe).
static bool portable_mode() {
  const char* e = std::getenv("SNAP_PORTABLE");
  return e && e[0] == '1';
}
static fs::path g_work_dir;   // config.json "work" (the builder's work folder; the exe lives in <work>/out); set by prepare()

static std::string save_folder(const hle::Json& cfg) {
  if (const char* e = std::getenv("SNAP_SAVE_DIR")) if (*e) return e;
  if (const hle::Json* s = cfg.get("save_root")) if (!s->str().empty()) return s->str();
  if (portable_mode()) {   // portable.txt: the runtime reads the redirected %APPDATA% (host/portable.cpp), so the saves are under portable_data
    wchar_t env_ad[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", env_ad, MAX_PATH) && env_ad[0]) return U8(env_ad) + "\\SnapRecomp\\save";
  }
  wchar_t* ad = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad))) { out = U8(ad) + "\\SnapRecomp\\save"; CoTaskMemFree(ad); }
  return out;
}

// ---- Updates. builder/snapbuild/update.py does all the work (check, download, signature, rebuild, rollback); the launcher only starts
// the builder and shows what it found. The background check never waits: it writes startup_check.new, the next start picks it up.
struct UpdateNotice {
  bool available = false;
  std::string latest, notes, size_text;
  int low = 0, high = 0;
};

static fs::path updates_dir() {
  if (portable_mode()) {   // same folder as builder/snapbuild/userdata.py update_state_dir: out/ is swapped by an update, so the state sits beside it
    fs::path base = g_work_dir;
    if (base.empty()) {
      wchar_t mp[MAX_PATH * 2] = {};
      GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
      base = fs::path(mp).parent_path().parent_path();
    }
    return base / L"updates";
  }
  wchar_t env_la[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", env_la, MAX_PATH) && env_la[0]) return fs::path(env_la) / L"SnapRecomp" / L"updates";
  fs::path p;
  wchar_t* la = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) { p = fs::path(la) / L"SnapRecomp" / L"updates"; CoTaskMemFree(la); }
  return p;
}

static bool testing_channel(const hle::Json& cfg) {   // "update_channel": "testing" in config.json; anything else is Stable
  const hle::Json* j = cfg.get("update_channel");
  return j && j->str() == "testing";
}
static std::wstring channel_arg(const hle::Json& cfg) { return testing_channel(cfg) ? L" --channel testing" : L" --channel stable"; }

static bool update_enabled(const hle::Json& cfg) {   // on unless config.json says "update_check": false
  const hle::Json* j = cfg.get("update_check");
  return !(j && j->type == hle::Json::Bool && !j->b);
}

static void save_config(const fs::path& cfg_path, const hle::Json& cfg) {
  std::string text;
  hle::json_dump(cfg, text);
  std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
  f << text << "\n";
}

// "python" -I "<builder>\snap_builder.py" <args>; empty when config.json does not say where the builder is.
static std::wstring builder_cmd(const hle::Json& cfg, const std::wstring& args) {
  auto s = [&](const char* k) { return cfg.get(k) ? cfg.get(k)->str() : std::string(); };
  std::string py = s("python"), bd = s("builder_dir");
  std::error_code ec;
  if (py.empty() || bd.empty() || !fs::exists(fs::u8path(py), ec) || !fs::exists(fs::u8path(bd) / "snap_builder.py", ec)) return L"";
  return L"\"" + W(py) + L"\" -I \"" + (fs::u8path(bd) / "snap_builder.py").wstring() + L"\" " + args;
}

// Starts the builder. Hidden with stdout in out_file, or in its own console window. Returns the process handle (caller closes), or null.
static HANDLE spawn(const std::wstring& cmd, bool visible, const fs::path& out_file) {
  if (cmd.empty()) return nullptr;
  STARTUPINFOW si{};
  si.cb = sizeof si;
  HANDLE out = INVALID_HANDLE_VALUE;
  DWORD flags = CREATE_NO_WINDOW;
  if (visible) {
    flags = CREATE_NEW_CONSOLE;
  } else {
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    out = CreateFileW(out_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return nullptr;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = INVALID_HANDLE_VALUE;
  }
  std::wstring mutable_cmd = cmd;
  PROCESS_INFORMATION pi{};
  // The builder never stands in the game's folder: an update renames out\, and Windows refuses that while a process has it as its
  // working directory (2026-11-18: "used by another process" at the switch). The work folder is its parent.
  const std::wstring cwd = g_work_dir.empty() ? std::wstring() : g_work_dir.wstring();
  BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, !visible, flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (!ok) return nullptr;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

static UpdateNotice read_notice(const fs::path& f) {
  UpdateNotice n;
  hle::Json j;
  if (!read_json(f, j) || j.type != hle::Json::Obj) return n;
  const hle::Json* a = j.get("update_available");
  if (!a || !a->boolean() || !j.get("latest")) return n;
  n.available = true;
  n.latest = j.get("latest")->str();
  if (const hle::Json* x = j.get("notes")) n.notes = x->str();
  if (const hle::Json* x = j.get("size_text")) n.size_text = x->str();
  if (const hle::Json* e = j.get("estimate")) {
    if (const hle::Json* lo = e->get("low_min")) n.low = (int)lo->num();
    if (const hle::Json* hi = e->get("high_min")) n.high = (int)hi->num();
  }
  return n;
}

static std::wstring describe(const UpdateNotice& n) {
  std::wstring t = L"Download " + W(n.size_text.empty() ? "a small file" : n.size_text);
  if (n.low > 0) t += L", then your copy is rebuilt from your own game files (about " + std::to_wstring(n.low) + L" to " + std::to_wstring(n.high) + L" minutes)";
  return t;
}

// Last start's background check, if it finished and did not fail, replaces the stored answer; a failed or offline try leaves the old one.
static UpdateNotice load_notice() {
  fs::path dir = updates_dir();
  if (dir.empty()) return {};
  std::error_code ec;
  fs::path nw = dir / L"startup_check.new", cur = dir / L"startup_check.json";
  if (fs::exists(nw, ec)) {
    hle::Json j;
    if (read_json(nw, j) && j.type == hle::Json::Obj && !j.get("error") && !j.get("disabled")) {
      fs::remove(cur, ec);
      fs::rename(nw, cur, ec);
    } else {
      fs::remove(nw, ec);
    }
  }
  return read_notice(cur);
}

static void start_background_check(const hle::Json& cfg, const fs::path& cfg_path) {
  if (!update_enabled(cfg)) return;
  fs::path dir = updates_dir();
  auto s = [&](const char* k) { return cfg.get(k) ? cfg.get(k)->str() : std::string(); };
  std::string work = s("work");
  if (dir.empty() || work.empty()) return;
  std::wstring cmd = builder_cmd(cfg, L"update check --startup --json" + channel_arg(cfg) + L" --config \"" + cfg_path.wstring() + L"\" --work \"" + W(work) + L"\"");
  HANDLE h = spawn(cmd, false, dir / L"startup_check.new");
  if (h) CloseHandle(h);   // never waited for: the answer is read at the next start
}

// The Updates page. Returns true when an install or rollback was started (it replaces the exe, so the launcher must close).
static bool updates_flow(const fs::path& cfg_path, hle::Json& cfg, UpdateNotice& notice) {
  auto s = [&](const char* k) { return cfg.get(k) ? cfg.get(k)->str() : std::string(); };
  const std::string work = s("work");
  const std::wstring wpid = std::to_wstring(GetCurrentProcessId());
  for (;;) {
    const bool on = update_enabled(cfg);
    std::error_code ec;
    const bool can_back = !work.empty() && fs::exists(fs::u8path(work) / "builds" / "previous", ec);
    std::vector<int> ids;
    std::vector<std::wstring> links;
    if (notice.available) {
      ids.push_back(1);
      links.push_back(L"Install version " + W(notice.latest) + L"...\n" + describe(notice));
    }
    ids.push_back(2);
    links.push_back(L"Check for updates now\nThis is version " + W(s("builder_version")));
    ids.push_back(3);
    links.push_back(on ? L"Turn off the daily check\nIt looks once a day when the launcher starts and only tells you; nothing installs without you"
                       : L"Turn on the daily check\nIt looks once a day when the launcher starts and only tells you; nothing installs without you");
    ids.push_back(5);
    links.push_back(testing_channel(cfg) ? L"Update channel: Testing (click for Stable)\nTesting also offers early builds that are still being tried out and may have bugs; Stable only offers finished releases"
                                         : L"Update channel: Stable (click for Testing)\nStable only offers finished releases; Testing also offers early builds that may have bugs");
    if (can_back) {
      ids.push_back(4);
      links.push_back(L"Go back to the previous version\nA copy of your saves and settings is kept first; the game's own saves are not touched");
    }
    int sel = choose(L"Updates", notice.available ? L"Version " + W(notice.latest) + L" is available." : L"Nothing new is waiting.", links);
    if (sel < 0) return false;
    const int what = ids[(size_t)sel];
    if (what == 3) {
      cfg.set("update_check", hle::Json::boolean_(!on));
      save_config(cfg_path, cfg);
      continue;
    }
    if (what == 5) {
      cfg.set("update_channel", hle::Json::string(testing_channel(cfg) ? "stable" : "testing"));
      save_config(cfg_path, cfg);
      std::error_code rec;
      fs::remove(updates_dir() / L"startup_check.json", rec);   // the saved answer belongs to the other channel
      fs::remove(updates_dir() / L"startup_check.new", rec);
      notice = UpdateNotice{};
      continue;
    }
    if (what == 2) {
      fs::path dir = updates_dir();
      fs::path out = dir / L"check_now.json";
      HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
      HANDLE h = spawn(builder_cmd(cfg, L"update check --json" + channel_arg(cfg) + (work.empty() ? std::wstring() : L" --work \"" + W(work) + L"\"")), false, out);
      bool done = h && WaitForSingleObject(h, 40000) == WAIT_OBJECT_0;
      if (h) CloseHandle(h);
      SetCursor(old);
      hle::Json j;
      if (!done || !read_json(out, j) || j.type != hle::Json::Obj) { message("Snap Recomp", "Could not check for updates right now. Try again later.", MB_ICONWARNING); continue; }
      if (const hle::Json* e = j.get("error")) { message("Snap Recomp", "Could not check for updates: " + e->str(), MB_ICONWARNING); continue; }
      notice = read_notice(out);
      if (notice.available) message("Snap Recomp", "Version " + notice.latest + " is available.");
      else message("Snap Recomp", "You are up to date.");
      continue;
    }
    if (what == 1) {
      std::string notes = notice.notes.size() > 900 ? notice.notes.substr(0, 900) + "..." : notice.notes;
      std::wstring q = L"Install version " + W(notice.latest) + L"?\n\n" + describe(notice) +
                       L". A dated copy of your saves and settings is made first (the newest 3 are kept), and this version stays here so you can go back.\n\nWhat changed:\n" + W(notes);
      if (MessageBoxW(nullptr, q.c_str(), L"Snap Recomp", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
      HANDLE h = spawn(builder_cmd(cfg, L"update apply" + channel_arg(cfg) + L" --work \"" + W(work) + L"\" --yes --wait-pid " + wpid), true, fs::path());
      if (!h) { message("Snap Recomp", "The updater could not be started (the builder folder is missing). Run the builder yourself: snap_builder.py update apply", MB_ICONWARNING); continue; }
      CloseHandle(h);
      message("Snap Recomp", "The update runs in a new window. This launcher closes now; start it again when that window says it is done.");
      return true;
    }
    if (what == 4) {
      if (MessageBoxW(nullptr, L"Go back to the previous version? A dated copy of your saves and settings is made first; your saves are not touched.", L"Snap Recomp", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
      HANDLE h = spawn(builder_cmd(cfg, L"update rollback --work \"" + W(work) + L"\" --wait-pid " + wpid), true, fs::path());
      if (!h) { message("Snap Recomp", "The updater could not be started (the builder folder is missing).", MB_ICONWARNING); continue; }
      CloseHandle(h);
      message("Snap Recomp", "The rollback runs in a new window. This launcher closes now; start it again when that window says it is done.");
      return true;
    }
  }
}

// ---- HD textures. builder/snapbuild/hdtex_cli.py does the work (capture, plan, licenses, AI upscale, pack); with --json its stdout is one JSON
// object per line and the human text goes to stderr. The launcher asks the questions and starts the builder; the long build runs in its own console.
static bool hd_json(const hle::Json& cfg, const std::wstring& args, const char* key, hle::Json& out, DWORD wait_ms = 120000) {
  fs::path f = updates_dir() / L"hdtex_out.json";
  HANDLE h = spawn(builder_cmd(cfg, L"hdtex " + args + L" --json"), false, f);
  if (!h) return false;
  bool done = WaitForSingleObject(h, wait_ms) == WAIT_OBJECT_0;
  if (!done) TerminateProcess(h, 1);
  CloseHandle(h);
  if (!done) return false;
  std::ifstream in(f, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] != '{') continue;
    hle::Json j;
    std::string err;
    if (hle::json_parse(line, j, err) && j.type == hle::Json::Obj && j.get(key)) { out = *j.get(key); return true; }
  }
  return false;
}

static std::wstring gb_text(double bytes) {
  wchar_t b[32];
  swprintf_s(b, L"%.1f GB", bytes / 1073741824.0);
  return b;
}

static std::wstring minutes_text(double seconds) {
  int m = (int)(seconds / 60.0 + 0.5);
  if (m < 1) return L"under a minute";
  if (m < 90) return L"about " + std::to_wstring(m) + L" minutes";
  wchar_t b[32];
  swprintf_s(b, L"about %.1f hours", m / 60.0);
  return b;
}

// Shows each license that is not accepted yet and records the ones the user accepts (the builder keeps them in tools\hdtex\licenses.json).
// Nothing is accepted for the user. Returns false when one is declined.
static bool hd_licenses(const hle::Json& cfg, const std::string& level) {
  hle::Json lic;
  std::wstring base = L"--licenses --level " + W(level);
  if (!hd_json(cfg, base, "licenses", lic) || lic.type != hle::Json::Arr) { message("Snap Recomp", "The license list could not be read. Check that the builder folder is still where config.json says.", MB_ICONWARNING); return false; }
  std::wstring accept_args;
  for (const hle::Json& l : lic.a) {
    if (l.get("accepted") && l.get("accepted")->boolean()) continue;
    std::string key = l.get("key") ? l.get("key")->str() : "";
    std::string title = l.get("title") ? l.get("title")->str() : key;
    std::string name = l.get("name") ? l.get("name")->str() : "";
    std::string author = l.get("author") ? l.get("author")->str() : "";
    std::string url = l.get("url") ? l.get("url")->str() : "";
    bool personal = l.get("personal_use_only") && l.get("personal_use_only")->boolean();
    for (;;) {
      std::wstring text = W(title + " is made by " + (author.empty() ? "its author" : author) + " and used under the " + name + " license." +
                            (personal ? "\nIts license allows personal, non-commercial use only." : "") +
                            "\nIt is downloaded from its author onto this PC; nothing is uploaded.");
      int sel = choose(L"License: " + W(title), text, {L"Read the license (opens your browser)", L"I accept this license", L"No, do not use it"});
      if (sel == 0 && url.rfind("https://", 0) == 0) { ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); continue; }
      if (sel == 1) { accept_args += L" --accept-license " + W(key); break; }
      return false;
    }
  }
  if (!accept_args.empty()) {
    hle::Json after;
    if (!hd_json(cfg, base + accept_args, "licenses", after)) { message("Snap Recomp", "The licenses could not be recorded.", MB_ICONWARNING); return false; }
  }
  return true;
}

// The HD build runs hidden; this dialog shows a real progress bar fed by the builder's --json lines ({"progress": {done, total, minutes_left}}) and
// its last status line. Cancel stops the build (the textures finished so far are kept; the next run continues). Returns the exit code, or -1 when
// it could not start, or 1223 (ERROR_CANCELLED) when the user cancelled.
struct HdRun {
  HANDLE proc = nullptr, job = nullptr;
  fs::path out;
  bool cancelled = false, finished = false, bar = false;
  DWORD code = 1;
  std::wstring instruction;
};

static void hd_scan(const fs::path& f, double& frac, std::wstring& status) {
  frac = -1;
  std::ifstream in(f, std::ios::binary);
  if (!in) return;
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  in.seekg(len > 65536 ? len - 65536 : 0);
  std::string line, last_text;
  long long done = 0, total = 0, left = -1;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    if (line[0] == '{') {
      hle::Json j;
      std::string err;
      if (hle::json_parse(line, j, err) && j.type == hle::Json::Obj)
        if (const hle::Json* p = j.get("progress")) {
          done = p->get("done") ? (long long)p->get("done")->num() : 0;
          total = p->get("total") ? (long long)p->get("total")->num() : 0;
          left = p->get("minutes_left") ? (long long)p->get("minutes_left")->num() : -1;
        }
      continue;
    }
    last_text = line;   // the builder's own human line (stderr shares the file)
  }
  if (total > 0) {
    frac = std::min(1.0, (double)done / (double)total);
    status = std::to_wstring(done) + L" of " + std::to_wstring(total) + L" textures";
    if (left >= 0) status += L", about " + std::to_wstring(left) + L" min left";
  } else if (!last_text.empty()) {
    status = W(last_text.size() > 140 ? last_text.substr(0, 140) + "..." : last_text);
  }
}

static HRESULT CALLBACK hd_run_cb(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref) {
  HdRun* r = (HdRun*)ref;
  if (msg == TDN_CREATED) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);   // moving bar until the first count arrives
  if (msg == TDN_TIMER) {
    double frac;
    std::wstring status;
    hd_scan(r->out, frac, status);
    if (frac >= 0) {
      if (!r->bar) {
        r->bar = true;
        SendMessageW(hwnd, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0);
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
      }
      SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(frac * 1000), 0);
    }
    if (!status.empty()) SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)status.c_str());
    if (WaitForSingleObject(r->proc, 0) == WAIT_OBJECT_0) {
      GetExitCodeProcess(r->proc, &r->code);
      r->finished = true;
      SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);   // the dialog has only Cancel; r->finished makes the click harmless
    }
  }
  if (msg == TDN_BUTTON_CLICKED && !r->finished && (int)wp == IDCANCEL) {
    r->cancelled = true;
    if (r->job) TerminateJobObject(r->job, 1223);   // the builder and the AI tools it started
    else TerminateProcess(r->proc, 1223);
  }
  return S_OK;
}

static int hd_run_with_bar(const hle::Json& cfg, const std::wstring& args, const std::wstring& title, const std::wstring& first_line) {
  HdRun r;
  r.out = updates_dir() / L"hdtex_run.log";
  r.instruction = title;
  r.proc = spawn(builder_cmd(cfg, L"hdtex " + args + L" --json"), false, r.out);
  if (!r.proc) return -1;
  r.job = CreateJobObjectW(nullptr, nullptr);
  if (r.job) AssignProcessToJobObject(r.job, r.proc);
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = title.c_str();
  c.pszContent = first_line.c_str();
  c.pfCallback = hd_run_cb;
  c.lpCallbackData = (LONG_PTR)&r;
  HRESULT hr = TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
  if (FAILED(hr) && !r.finished) WaitForSingleObject(r.proc, INFINITE);   // no task dialog (very old system): wait without a bar
  if (!r.finished && !r.cancelled) { WaitForSingleObject(r.proc, INFINITE); GetExitCodeProcess(r.proc, &r.code); }
  else if (r.cancelled) { WaitForSingleObject(r.proc, 5000); r.code = 1223; }
  CloseHandle(r.proc);
  if (r.job) CloseHandle(r.job);
  return (int)r.code;
}

// HD shadows: sharper shadow textures for every course, built from the game archives of the player's own romfs (no play-through), as its own pack
// (hd-shadows) next to the HD textures pack. Same builder, other arguments: --cats shadow --from-romfs <folder> --pack-id hd-shadows. The build is long
// (about 27 minutes at HD), resumable, and waits while the game runs (--pause-game). Its stdout JSON lines feed the same real progress bar.
static void hd_shadows_flow(const hle::Json& cfg) {
  const std::string romfs = cfg.get("romfs_root") ? cfg.get("romfs_root")->str() : std::string();
  if (romfs.empty()) { message("Snap Recomp", "The game folder is not set, so the shadows cannot be read. Pick the game folder first.", MB_ICONWARNING); return; }
  const std::wstring common = L" --cats shadow --from-romfs \"" + W(romfs) + L"\" --pack-id hd-shadows";
  for (;;) {
    int sel = choose(L"HD shadows", L"Sharper shadows on every course, made from your own game files on this PC. Nothing is uploaded. It runs as its own pack, so your HD textures are not touched.",
                     {L"Sharp\nQuickest",
                      L"HD (suggested)\nAbout 5 GB of disk and about 27 minutes, once",
                      L"Ultra\nMost detail. Slowest and the largest",
                      L"Off\nTurn the HD shadows off. Nothing is deleted"});
    if (sel < 0) return;
    if (sel == 3) {
      hle::Json d;
      if (hd_json(cfg, L"--level off --pack-id hd-shadows", "done", d)) message("Snap Recomp", "The HD shadows are off. Nothing was deleted; pick a level again to turn them back on.");
      else message("Snap Recomp", "Could not turn the HD shadows off.", MB_ICONWARNING);
      continue;
    }
    const std::string level = sel == 0 ? "sharp" : sel == 1 ? "hd" : "ultra";
    hle::Json plan;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = hd_json(cfg, L"--level " + W(level) + common + L" --dry-run", "plan", plan, 300000);   // reading the course archives takes a minute or two
    SetCursor(old);
    if (!ok) { message("Snap Recomp", "The plan could not be made. Run the builder's hdtex command in a window to see why.", MB_ICONWARNING); continue; }
    auto n = [&](const char* k) { return plan.get(k) ? plan.get(k)->num() : 0.0; };
    if (plan.get("fits") && !plan.get("fits")->boolean(true)) { message("Snap Recomp", "That level does not fit on this PC. Pick a lower level.", MB_ICONWARNING); continue; }
    std::wstring q = L"Level " + W(level) + L": " + std::to_wstring((long long)n("textures")) + L" shadow textures, " + std::to_wstring((long long)n("to_build")) +
                     L" to build, " + minutes_text(n("seconds")) + L", " + gb_text(n("disk_bytes")) + L" of disk.\n\nThe build waits while the game runs, and a rebuild only redoes what changed.\n\nStart?";
    if (MessageBoxW(nullptr, q.c_str(), L"Snap Recomp", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
    const int code = hd_run_with_bar(cfg, L"--level " + W(level) + common + L" --pause-game --yes", L"Making the HD shadows",
                                    L"Starting. Reading the course archives takes a minute or two, then the shadows are built. You can cancel; what is finished is kept.");
    if (code < 0) { message("Snap Recomp", "The HD shadows build could not be started.", MB_ICONWARNING); continue; }
    if (code == 1223) { message("Snap Recomp", "The HD shadows build was cancelled. What was finished is kept; pick the level again to continue."); continue; }
    if (code == 0) message("Snap Recomp", "The HD shadows are ready and on. Press Play to use them. Pick Off here to turn them off.");
    else message("Snap Recomp", "The HD shadows build stopped before it finished. What was finished is kept; run it again to continue. The details are in " + U8((updates_dir() / L"hdtex_run.log").wstring()) + ".", MB_ICONWARNING);
  }
}

// TODO(hd light maps): add an "HD light maps" toggle to this flow once the hdtex session ships a runtime settings key for it. Today the light-map
// level is builder-only (--set lighting.scale=), so there is nothing for the launcher to read or write.
static void hd_textures_flow(const fs::path& exe_dir, const fs::path& cfg_path, hle::Json& cfg) {
  if (builder_cmd(cfg, L"").empty()) {
    message("Snap Recomp", "The builder folder was not found, so the HD texture tool cannot start. Run the builder again (snap_builder.py build).", MB_ICONWARNING);
    return;
  }
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  for (;;) {
    hle::Json st;
    std::wstring state = L"Makes sharper textures from your own game files on this PC with a local AI model. Nothing is uploaded.";
    double captured = -1;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool have = hd_json(cfg, L"--status", "status", st, 60000);
    SetCursor(old);
    if (have) {
      captured = st.get("captured") ? st.get("captured")->num() : 0;
      const hle::Json* en = st.get("enabled");
      const hle::Json* pt = st.get("pack_textures");
      if (en && en->type == hle::Json::Bool)
        state += en->b ? L"\n\nThe HD pack is on (" + std::to_wstring((long long)(pt ? pt->num() : 0)) + L" textures)." : L"\n\nThe HD pack is off.";
      else
        state += L"\n\nNo HD pack has been made yet.";
      state += L"\nTextures captured from your game so far: " + std::to_wstring((long long)captured) + L".";
    }
    int sel = choose(L"HD textures", state,
                     {L"Off\nTurn the HD pack off. Nothing is deleted",
                      L"Sharp\nQuickest. Courses and menus at 2x",
                      L"HD (suggested)\nCourses 4x, Pokemon and menus 2x",
                      L"Ultra\nCourses and Pokemon at 4x with extra detail. Slowest and the most video memory",
                      L"Capture textures from your game...\nPlays each unlocked course hidden and muted to collect the textures to upscale",
                      L"Custom...\nChoose a model and size for each kind of texture",
                      L"HD shadows...\nSharper shadows on every course, built from your game files"});
    if (sel < 0) return;
    if (sel == 6) { hd_shadows_flow(cfg); continue; }
    if (sel == 5) {
      message("Snap Recomp", "Custom levels are set from a command window, for example:\n\n  python builder\\snap_builder.py hdtex --level custom --set creature.scale=4\n\nSee builder\\HD-TEXTURES.md for every setting.");
      continue;
    }
    if (sel == 0) {
      hle::Json d;
      if (hd_json(cfg, L"--level off", "done", d)) message("Snap Recomp", "The HD pack is off. Nothing was deleted; pick a level again to turn it back on.");
      else message("Snap Recomp", "Could not turn the HD pack off.", MB_ICONWARNING);
      continue;
    }
    if (sel == 4) {
      std::string root, label;
      if (!save_target(cfg_path, &root, &label)) continue;
      if (MessageBoxW(nullptr, L"The game will run hidden and muted through each course your save has unlocked and save the textures it loads. This can take a while (up to about 7 minutes per course) and uses the GPU. Continue?",
                      L"Snap Recomp", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
      HANDLE h = spawn(builder_cmd(cfg, L"hdtex --collect \"" + std::wstring(mp) + L"\" --save-dir \"" + W(root) + L"\""), true, fs::path());
      if (!h) { message("Snap Recomp", "The capture could not be started.", MB_ICONWARNING); continue; }
      WaitForSingleObject(h, INFINITE);
      DWORD code = 1;
      GetExitCodeProcess(h, &code);
      CloseHandle(h);
      message("Snap Recomp", code == 0 ? "Capture finished. Pick a level to build the HD pack from the captured textures." : "The capture stopped before it finished. The textures captured so far are kept.", code == 0 ? MB_ICONINFORMATION : MB_ICONWARNING);
      continue;
    }
    const std::string level = sel == 1 ? "sharp" : sel == 2 ? "hd" : "ultra";
    if (have && captured <= 0) {
      message("Snap Recomp", "No textures have been captured from your game yet. Use \"Capture textures from your game\" first, or play with capture on (hdtex --capture on).", MB_ICONINFORMATION);
      continue;
    }
    hle::Json plan;
    old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = hd_json(cfg, L"--level " + W(level) + L" --dry-run", "plan", plan, 180000);
    SetCursor(old);
    if (!ok) { message("Snap Recomp", "The plan could not be made. Run the builder's hdtex command in a window to see why.", MB_ICONWARNING); continue; }
    auto n = [&](const char* k) { return plan.get(k) ? plan.get(k)->num() : 0.0; };
    const bool fits = !plan.get("fits") || plan.get("fits")->boolean(true);
    std::wstring q = L"Level " + W(level) + L": " + std::to_wstring((long long)n("textures")) + L" textures, " + minutes_text(n("seconds")) +
                     L", " + gb_text(n("disk_bytes")) + L" of disk, up to " + gb_text(n("vram_peak_bytes")) + L" of video memory in the busiest course.";
    if (!fits) {
      message("Snap Recomp", U8(q) + "\n\nThat does not fit your graphics card. Pick a lower level.", MB_ICONWARNING);
      continue;
    }
    q += L"\n\nThe AI tools and models are downloaded from their authors. A rebuild only redoes what changed.\n\nStart?";
    if (MessageBoxW(nullptr, q.c_str(), L"Snap Recomp", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) continue;
    if (!hd_licenses(cfg, level)) { message("Snap Recomp", "The HD pack was not built because a license was not accepted. The game is unchanged."); continue; }
    const int code = hd_run_with_bar(cfg, L"--level " + W(level) + L" --yes", L"Making the HD textures",
                                    L"Starting. The first run downloads the AI tools and models from their authors. A rebuild only redoes what changed.");
    if (code < 0) { message("Snap Recomp", "The HD build could not be started.", MB_ICONWARNING); continue; }
    if (code == 1223) { message("Snap Recomp", "The HD build was cancelled. The textures finished so far are kept; pick the level again to continue."); continue; }
    if (code == 0) message("Snap Recomp", "The HD pack is ready and on. Press Play to use it. Pick Off here to turn it off.");
    else if (code == 3) message("Snap Recomp", "A license was not accepted, so nothing was built.", MB_ICONWARNING);
    else message("Snap Recomp", "The HD build stopped before it finished. The textures finished so far are kept; run it again to continue. The details are in " + U8((updates_dir() / L"hdtex_run.log").wstring()) + ".", MB_ICONWARNING);
  }
}

// Dialog result: 1 Play, 2 Change game folder, 3 Open save folder, 4 Upscalers, 5 Import save, 6 Export save, 7 Updates, 8 HD textures, 0 Quit.
static int ask(const UpdateNotice& notice) {
  using TDI = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE cc = LoadLibraryW(L"comctl32.dll");
  TDI td = cc ? (TDI)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
  if (!td) return 1;   // no task dialog (old system): just play
  std::wstring upd = notice.available ? L"Updates...\nVersion " + W(notice.latest) + L" is available" : std::wstring(L"Updates...\nCheck for a new version, or go back to the previous one");
  TASKDIALOG_BUTTON b[8] = {{1001, L"Play"}, {1002, L"Change game folder..."}, {1003, L"Open save folder"}, {1005, L"Import save...\nFrom a console backup (JKSV, Checkpoint) or an emulator"}, {1006, L"Export save...\nCopy your save out for a save manager or an emulator"}, {1004, L"Upscalers: FSR, DLSS, XeSS..."}, {1008, L"HD textures...\nSharper textures made from your own game files with a local AI model"}, {1007, upd.c_str()}};
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = 0;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = L"Snap Recomp";
  std::wstring content = L"Works with your own copy of the game.";
  if (notice.available) content += L"\n\nUpdate available: version " + W(notice.latest) + L". " + describe(notice) + L". Open Updates to read what changed.";
  c.pszContent = content.c_str();
  c.pButtons = b;
  c.cButtons = 8;
  c.nDefaultButton = 1001;
  int sel = 0;
  if (FAILED(td(&c, &sel, nullptr, nullptr))) return 1;
  return sel == 1001 ? 1 : sel == 1002 ? 2 : sel == 1003 ? 3 : sel == 1004 ? 4 : sel == 1005 ? 5 : sel == 1006 ? 6 : sel == 1007 ? 7 : sel == 1008 ? 8 : 0;
}

// The themed launcher window (host/launcher_ui.cpp). Returns the same codes as ask(), or -1 when the window could not be made (then the task dialog is used).
// The window also owns the save-profile choice and writes the pre-launch settings to the files the game reads, so they apply at Play.
// The game-folder check of the themed window: it runs on a worker thread so the window opens at once, and Play waits for it (the check only
// compares the published file names and sizes, one directory pass). It is done once per folder; the chip on the home page starts it again.
static std::shared_ptr<snap_launcher::UiVerify> s_verify;
static std::string s_verify_root;

// What a passed check leaves behind (<data dir>\verify_cache.json): the folder, the exe it passed under, when, and the check files with their size and
// modified time. The next start only looks at those few files; the folder is walked again when the folder, the exe (an update) or any of them changed,
// or when the player asks. Nothing in the game folder is read for the check, only names, sizes and times.
struct VerifyCfg { std::string cache_file, exe_stamp; };

static std::string launcher_data_dir() {
  auto env = [](const char* n) { const char* e = std::getenv(n); return std::string(e && *e ? e : ""); };
  return !env("SNAP_CONFIG_DIR").empty() ? env("SNAP_CONFIG_DIR") : (!env("APPDATA").empty() ? env("APPDATA") + "\\SnapRecomp" : hle::config().data_root);
}

static VerifyCfg verify_cfg(const std::string& data_dir, const hle::Json& cfg) {
  VerifyCfg c;
  c.cache_file = data_dir + "\\verify_cache.json";
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  std::error_code ec;
  const auto sz = fs::file_size(mp, ec);
  const auto mt = fs::last_write_time(mp, ec).time_since_epoch().count();
  c.exe_stamp = (cfg.get("builder_version") ? cfg.get("builder_version")->str() : std::string()) + "|" + std::to_string((long long)sz) + "|" + std::to_string((long long)mt);
  return c;
}

static std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
  return o + "\"";
}

// The whole check for one folder, on the calling thread: the remembered result first, the walk when it does not hold. Returns the message ("" = passed).
static std::string verify_now(snap_launcher::UiVerify& v, const std::string& root, const hle::Json& data, const VerifyCfg& vc, bool force) {
  const auto t0 = std::chrono::steady_clock::now();
  auto done_ms = [&]() { return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
  if (const hle::Json* fc = data.get("file_count")) v.total.store((int)fc->num());
  const hle::Json* sents = data.get("sentinels");
  const size_t nwant = sents ? sents->a.size() : 0;
  int why = force ? 5 : 1;
  if (!force && nwant && !vc.cache_file.empty()) {
    std::ifstream in(vc.cache_file, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), e;
    hle::Json j;
    if (!t.empty() && hle::json_parse(t, j, e) && j.type == hle::Json::Obj) {
      auto str = [&](const char* k) { return j.get(k) ? j.get(k)->str() : std::string(); };
      const hle::Json* files = j.get("files");
      if (_stricmp(str("root").c_str(), root.c_str()) != 0) why = 3;
      else if (str("exe") != vc.exe_stamp || !files || files->a.size() != nwant) why = 2;
      else {
        bool same = true;
        std::error_code ec;
        for (const auto& f : files->a) {
          const fs::path p = fs::u8path(root) / fs::u8path(f.get("p") ? f.get("p")->str() : std::string());
          const auto sz = fs::file_size(p, ec);
          const bool bad1 = (bool)ec;
          ec.clear();
          const long long mt = snap_launcher::mtime_ms(fs::last_write_time(p, ec));
          if (bad1 || ec || (long long)sz != (f.get("s") ? (long long)f.get("s")->num() : -1) || mt != (f.get("m") ? (long long)f.get("m")->num() : -1)) { same = false; break; }
        }
        if (same) {
          v.at.store(j.get("at") ? (long long)j.get("at")->num() : 0);
          v.ms.store(done_ms());
          return "";
        }
        why = 4;
      }
    }
  }
  v.why.store(why);
  std::vector<snap_launcher::Matched> m;
  const std::string msg = snap_launcher::check_romfs(root, data, &v.files, &m);
  v.ms.store(done_ms());
  if (msg.empty()) {
    const long long now = (long long)std::time(nullptr);
    v.at.store(now);
    if (!vc.cache_file.empty() && !m.empty()) {
      std::string o = "{\"v\":1,\"root\":" + jstr(root) + ",\"exe\":" + jstr(vc.exe_stamp) + ",\"at\":" + std::to_string(now) + ",\"files\":[";
      for (size_t i = 0; i < m.size(); ++i)
        o += std::string(i ? "," : "") + "{\"p\":" + jstr(m[i].rel) + ",\"s\":" + std::to_string(m[i].size) + ",\"m\":" + std::to_string(m[i].mtime) + "}";
      o += "]}\n";
      std::error_code ec;
      fs::create_directories(fs::u8path(vc.cache_file).parent_path(), ec);
      std::ofstream f(fs::u8path(vc.cache_file), std::ios::binary | std::ios::trunc);
      f << o;
    }
  } else if (!vc.cache_file.empty()) {
    std::error_code ec;
    fs::remove(fs::u8path(vc.cache_file), ec);   // a folder that failed is not remembered
  }
  return msg;
}

static std::shared_ptr<snap_launcher::UiVerify> start_verify(const std::string& root, const hle::Json& data, const VerifyCfg& vc, bool force) {
  auto v = std::make_shared<snap_launcher::UiVerify>();
  hle::Json d = data;   // the worker outlives the caller's reference
  std::thread([v, root, d, vc, force]() {
    v->msg = verify_now(*v, root, d, vc, force);
    v->state.store(v->msg.empty() ? 1 : 2, std::memory_order_release);
  }).detach();
  return v;
}

static int ask_themed(const UpdateNotice& notice, const fs::path& cfg_path, const fs::path& exe_dir, const std::string& romfs, const hle::Json& cfg, const hle::Json& data) {
  static std::string s_profile;                      // the choice survives going to a sub-flow and back
  hle::load_config(U8(cfg_path.wstring()));
  snap_launcher::UiInfo ui;
  auto env = [](const char* n) { const char* e = std::getenv(n); return std::string(e && *e ? e : ""); };
  ui.data_dir = !env("SNAP_CONFIG_DIR").empty() ? env("SNAP_CONFIG_DIR") : (!env("APPDATA").empty() ? env("APPDATA") + "\\SnapRecomp" : hle::config().data_root);
  const std::string data_root = hle::config().data_root;
  ui.settings_noprofile = data_root + "\\settings.cfg";
  const bool explicit_save = !env("SNAP_SAVE_DIR").empty() || (cfg.get("save_root") && !cfg.get("save_root")->str().empty());
  const bool profiles_on = !env("SNAP_PROFILES_DIR").empty() || !explicit_save;
  ui.exe_dir = U8(exe_dir.wstring());
  ui.romfs_root = romfs;
  ui.main_elf = cfg.get("main_elf") ? cfg.get("main_elf")->str() : "";
  ui.save_noprofile = save_folder(cfg);
  ui.version = cfg.get("builder_version") ? cfg.get("builder_version")->str() : "";
  ui.update_available = notice.available;
  ui.update_version = notice.latest;
  const VerifyCfg vcfg = verify_cfg(ui.data_dir, cfg);
  if (!s_verify || romfs != s_verify_root) { s_verify = start_verify(romfs, data, vcfg, false); s_verify_root = romfs; }
  ui.verify = s_verify;
  ui.verify_again = [&ui, romfs, data, vcfg]() { s_verify = start_verify(romfs, data, vcfg, true); ui.verify = s_verify; };
  ui.game_ok = s_verify->state.load() != 2;
  ui.game_msg = s_verify->state.load() == 2 ? s_verify->msg : std::string();
  std::string root;
  if (profiles_on) {
    root = !env("SNAP_PROFILES_DIR").empty() ? env("SNAP_PROFILES_DIR") : data_root + "\\profiles";
    ui.profiles_root = root;
    hle::profiles::Store st(root);
    st.load();
    if (st.list().empty()) {   // the same first-start steps as hle::profiles::startup, so the settings have a profile folder to live in
      std::string legacy = !env("SNAP_LEGACY_SAVE_DIR").empty() ? env("SNAP_LEGACY_SAVE_DIR") : data_root + "\\save", e;
      std::error_code ec;
      if (fs::is_directory(fs::u8path(legacy), ec) && !fs::is_empty(fs::u8path(legacy), ec)) st.migrate_legacy(legacy, &e);
      if (st.list().empty()) st.create("Profile 1", &e);
    }
    for (auto& p : st.list()) ui.profiles.push_back({p.id, p.name, (long long)p.last_played});
    std::string want = env("SNAP_PROFILE");
    const hle::profiles::Profile* pf = want.empty() ? nullptr : st.find(want);
    ui.profile_id = pf ? pf->id : (!s_profile.empty() && st.find(s_profile)) ? s_profile : st.last_used();
    ui.new_profile = [&ui, root]() -> std::string {
      hle::profiles::Store s2(root);
      s2.load();
      std::string err;
      std::string id = s2.create(s2.next_free_name(), &err);
      ui.profiles.clear();
      for (auto& p : s2.list()) ui.profiles.push_back({p.id, p.name, (long long)p.last_played});
      return id;
    };
  }
  snap::ui::game_font_ttf(ui.font_ttf);   // the game's own UI font, decoded now from the user's files; empty when missing
  using R = snap_launcher::UiResult;
  const R res = snap_launcher::show(ui);
  if (res == R::Failed) return -1;
  if (!ui.profile_id.empty()) {
    s_profile = ui.profile_id;
    if (env("SNAP_PROFILE").empty()) _putenv_s("SNAP_PROFILE", ui.profile_id.c_str());   // skips the game's own picker and sends saves here
  }
  switch (res) {
    case R::Play: return 1;
    case R::GameFolder: return 2;
    case R::OpenSaves: return 3;
    case R::Upscalers: return 4;
    case R::ImportSave: return 5;
    case R::ExportSave: return 6;
    case R::Updates: return 7;
    case R::HdTextures: return 8;
    case R::Benchmark: return 9;
    case R::ProblemReport: return 10;
    case R::CheckInstall: return 11;
    case R::RestoreBackup: return 12;
    case R::SafeMode: return 13;
    default: return 0;
  }
}

// %LOCALAPPDATA%\SnapRecomp\logs (the LOCALAPPDATA variable first: the folder the shader cache uses, which a test run can redirect)
static fs::path logs_dir() {
  wchar_t* la = nullptr;
  fs::path logs;
  wchar_t env_la[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", env_la, MAX_PATH) && env_la[0]) logs = fs::path(env_la) / L"SnapRecomp" / L"logs";
  else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) { logs = fs::path(la) / L"SnapRecomp" / L"logs"; CoTaskMemFree(la); }
  return logs;
}

// One launcher action from the command line (host/cli_flags.h), without the launcher window. Returns the process exit code. A test run
// (SNAP_LAUNCHER_AUTOPLAY) names what a window or the updater would do instead of opening it.
// --restore-backup without a window. No argument: the list, newest first. "latest" or a number from that list: puts that backup back
// (the current save is backed up first, as in the window). Exit code 0 done, 1 not restored, 2 the argument is wrong.
static int restore_backup_cli(const fs::path& cfg_path, const std::vector<std::string>& args, const std::function<void(const std::string&)>& say) {
  hle::load_config(U8(cfg_path.wstring()));
  std::string label;
  const std::string root = hle::saveport::target_save_root("", &label);
  if (root.empty()) { say("No save folder could be chosen: " + label); return 1; }
  const auto backups = hle::savebackup::list(root);
  if (args.empty()) {
    if (backups.empty()) { say("There are no backups of " + label + " yet. A backup is made each time the game starts and before every import or restore."); return 0; }
    say("Backups of " + label + ", newest first:");
    for (size_t i = 0; i < backups.size(); ++i) {
      char line[64];
      snprintf(line, sizeof line, "  %zu  %.1f MB  ", i + 1, (double)backups[i].bytes / (1024.0 * 1024.0));
      say(line + hle::savebackup::describe(backups[i]));
    }
    say("Put one back with --restore-backup <number> or --restore-backup latest. Your current save is backed up first.");
    return 0;
  }
  size_t pick = 0;
  if (args.size() != 1) { say("--restore-backup takes one argument: latest, or a number from its list"); return 2; }
  if (args[0] != "latest") {
    if (!snap_cli::is_number(args[0]) || std::atoi(args[0].c_str()) < 1) { say("--restore-backup takes latest or a number from its list, got \"" + args[0] + "\""); return 2; }
    pick = (size_t)std::atoi(args[0].c_str()) - 1;
  }
  if (pick >= backups.size()) { say(backups.empty() ? "There are no backups of " + label + " yet." : "There is no backup number " + args[0] + "; the list has " + std::to_string(backups.size()) + "."); return 1; }
  hle::saveport::Report r;
  std::string err;
  if (!hle::savebackup::restore(root, backups[pick].folder, &r, &err, nullptr)) { say("The backup was not restored. " + err); return 1; }
  say("The backup from " + hle::savebackup::describe(backups[pick]) + " was restored.\n" + hle::saveport::format_report(r, true));
  return 0;
}

// One command-line argument, quoted for CreateProcess when it needs it.
static std::wstring quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return W(a);
  std::string q = "\"";
  size_t bs = 0;
  for (char c : a) {
    if (c == '\\') { bs++; q += c; continue; }
    if (c == '"') q.append(bs + 1, '\\');
    bs = 0;
    q += c;
  }
  q.append(bs, '\\');
  return W(q + "\"");
}

// --hdtex without a window: the builder's hdtex command (builder/snapbuild/hdtex_cli.py) with the arguments as they came, its output on
// this process's output, its exit code as ours. No arguments: --status.
static int hdtex_cli(const hle::Json& cfg, const std::vector<std::string>& args, bool test, const std::function<void(const std::string&)>& say) {
  std::wstring a = L"hdtex";
  if (args.empty()) a += L" --status";
  for (const std::string& s : args) a += L" " + quote_arg(s);
  std::wstring cmd = builder_cmd(cfg, a);
  if (cmd.empty()) { say("The HD texture tool could not be started (the builder folder is missing)."); return 1; }
  if (test) { say("launcher: would run: " + U8(cmd)); return 0; }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = INVALID_HANDLE_VALUE;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  for (HANDLE h : {si.hStdOutput, si.hStdError}) if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = g_work_dir.empty() ? std::wstring() : g_work_dir.wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    say("The HD texture tool could not be started.");
    return 1;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return (int)code;
}

static int run_action(const std::string& action, const std::vector<std::string>& args, const fs::path& exe_dir, const fs::path& cfg_path, hle::Json& cfg, const hle::Json& data) {
  const bool test = std::getenv("SNAP_LAUNCHER_AUTOPLAY") != nullptr;
  auto say = [](const std::string& s) { std::fprintf(stdout, "%s\n", s.c_str()); std::fflush(stdout); };
  if (action == "check-install") {   // host/mods/install_check.cpp; reads only
    const snap_install::Report rep = snap_install::run(exe_dir);
    say(snap_install::text(rep));
    return rep.ok ? 0 : 1;
  }
  if (action == "report") {   // host/mods/problem_report.cpp: one zip on the Desktop; never uploads
    char path[1024] = {}, why[512] = {};
    if (!snap_problem_report("command line", path, (int)sizeof path, why, (int)sizeof why)) { say(std::string("The problem report could not be made: ") + why); return 1; }
    say(std::string("The problem report is here: ") + path + "\nNothing was uploaded.");
    return 0;
  }
  if (action == "update") {   // the same step as Updates > Install in the launcher; the builder waits for this process to end, then rebuilds
    const std::string work = cfg.get("work") ? cfg.get("work")->str() : std::string();
    const std::wstring cmd = builder_cmd(cfg, L"update apply" + channel_arg(cfg) + L" --work \"" + W(work) + L"\" --yes --wait-pid " + std::to_wstring(GetCurrentProcessId()));
    if (cmd.empty()) { say("The updater could not be started (the builder folder is missing)."); return 1; }
    if (test) { say("launcher: would run: " + U8(cmd)); return 0; }
    HANDLE h = spawn(cmd, true, fs::path());
    if (!h) { say("The updater could not be started."); return 1; }
    CloseHandle(h);
    say("The update runs in its own window.");
    return 0;
  }
  // These three never open a window from the command line (release standard: every option has a flag another launcher can use).
  if (action == "restore-backup") return restore_backup_cli(cfg_path, args, say);
  if (action == "upscalers") return upscalers_cli(exe_dir, data, args, say);
  if (action == "hdtex") return hdtex_cli(cfg, args, test, say);
  say("unknown action " + action);
  return 2;
}

bool prepare(Result& r) {
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  fs::path exe_dir = fs::path(mp).parent_path();
  fs::path cfg_path = exe_dir / L"config.json";
  std::error_code ec;
  if (!fs::exists(cfg_path, ec)) return false;   // not launcher mode
  r.active = true;
  hle::Json cfg, data;
  if (!read_json(cfg_path, cfg) || cfg.type != hle::Json::Obj) {
    if (r.play) { std::fprintf(stderr, "launcher: config.json next to the game could not be read. Run the builder again.\n"); r.exit_code = 2; }
    else message("Snap Recomp", "config.json next to the game could not be read. Run the builder again.", MB_ICONERROR);
    return true;
  }
  read_json(exe_dir / L"launcher_data.json", data);   // optional
  if (const hle::Json* w = cfg.get("work")) if (!w->str().empty()) g_work_dir = fs::u8path(w->str());
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!r.action.empty()) { r.exit_code = run_action(r.action, r.action_args, exe_dir, cfg_path, cfg, data); return true; }
  const bool autoplay = std::getenv("SNAP_LAUNCHER_AUTOPLAY") != nullptr;
  const bool quiet = autoplay || r.play;   // no message boxes: a test run, or --play from another launcher (the reason goes to stderr)
  // First start: offer FSR (pre-selected, never accepted for the user). A test run skips this unless SNAP_UPSCALER_AUTOACCEPT is set.
  // --play shows no window of ours (release standard 4: another launcher starts the game), so the offer waits for the first start
  // without it; "upscalers_offered" stays unset until then.
  if (!cfg.get("upscalers_offered") && ((!autoplay && !r.play) || std::getenv("SNAP_UPSCALER_AUTOACCEPT"))) {
    offer_upscalers(exe_dir, data, true);
    cfg.set("upscalers_offered", hle::Json::boolean_(true));
    std::string text;
    hle::json_dump(cfg, text);
    std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
    f << text << "\n";
  }
  if (std::getenv("SNAP_LAUNCHER_FETCH_ONLY")) return true;   // test switch: only the upscaler offer, never start the game
  // Update notice from the last background check, then start the next one (at most one GitHub request a day, none when switched off;
  // a test run never checks unless SNAP_LAUNCHER_UPDATE_CHECK is set). Play is never delayed by it.
  UpdateNotice notice = load_notice();
  if (!autoplay || std::getenv("SNAP_LAUNCHER_UPDATE_CHECK")) start_background_check(cfg, cfg_path);
  // Safe mode (docs/safe-mode.md): --safe-mode, the launcher's button, or an offer after a start that never reached the title screen.
  bool safe = [] { const char* e = std::getenv("SNAP_SAFE_MODE"); return e && e[0] == '1'; }();
  bool play_now = r.play;   // --play, or the safe-mode offer was accepted: skip the launcher window once
  if (!r.notice.empty()) {   // where the saves live now (host/portable.h): said once, before the window; never blocks a --play or test start
    if (autoplay || r.play) std::fprintf(stderr, "launcher: %s\n", r.notice.c_str());
    else message("Snap Recomp", r.notice);
  }
  {
    const fs::path lg = logs_dir();
    if (!lg.empty()) {
      std::error_code lec;
      fs::create_directories(lg, lec);
      snap_crashlog_set_dir(U8(lg.wstring()).c_str());   // the launch marker and _crash.txt live in this folder
    }
    char why[400] = {};
    const int last = safe ? 0 : snap_launchmark_check(why, (int)sizeof why);
    if (last) {
      if (autoplay) {   // a test run: the answer comes from SNAP_LAUNCHER_SAFE_ANSWER (yes | no), and the offer is logged
        std::fprintf(stderr, "launcher: safe mode offered: %s\n", why);
        if (const char* a = std::getenv("SNAP_LAUNCHER_SAFE_ANSWER")) safe = play_now = (a[0] == 'y' || a[0] == 'Y');
      } else if (r.play) {   // --play never asks: say it and start normally (--safe-mode is the flag for it)
        std::fprintf(stderr, "launcher: %s Starting normally because of --play; add --safe-mode to start in safe mode.\n", why);
      } else {
        const std::string q = std::string(why) + "\n\nStart in safe mode? Mods, HD packs, enhanced effects, frame generation, ray tracing and the shader cache are switched off for this one start. "
                              "Your saved settings and your save are not changed.";
        if (MessageBoxW(nullptr, W(q).c_str(), L"Snap Recomp - safe mode", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_SETFOREGROUND) == IDYES) safe = play_now = true;
      }
    }
  }
  for (;;) {
    std::string romfs = cfg.get("romfs_root") ? cfg.get("romfs_root")->str() : "";
    int choice = 1;
    if (play_now) play_now = false;   // straight to the game-folder check and the start
    else if (!autoplay) {
      choice = std::getenv("SNAP_LAUNCHER_CLASSIC") ? -1 : ask_themed(notice, cfg_path, exe_dir, romfs, cfg, data);   // -1: no themed window, use the task dialog
      if (choice < 0) choice = ask(notice);
    }
    if (choice == 13) { safe = true; choice = 1; }   // the launcher's "Start in safe mode" button: the same as Play, with safe mode on for this start
    if (choice == 0) return true;   // quit
    if (choice == 7) { if (updates_flow(cfg_path, cfg, notice)) return true; continue; }
    if (choice == 8) { hd_textures_flow(exe_dir, cfg_path, cfg); continue; }
    if (choice == 10) {   // host/mods/problem_report.cpp: one zip on the Desktop; never uploads
      char path[1024] = {}, why[512] = {};
      if (snap_problem_report("launcher", path, (int)sizeof path, why, (int)sizeof why)) {
        message("Snap Recomp", std::string("The problem report is here:\n") + path + "\n\nOpen the zip to see exactly what is in it, then attach it to your bug report. Nothing was uploaded.", MB_ICONINFORMATION);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + W(path) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
      } else {
        message("Snap Recomp", std::string("The problem report could not be made: ") + why, MB_ICONWARNING);
      }
      continue;
    }
    if (choice == 11) {   // host/mods/install_check.cpp: hashes the RomFS listing, the exe, DLLs and converted images; reads only
      const snap_install::Report rep = snap_install::run(exe_dir);
      message("Snap Recomp - Check install", snap_install::text(rep), rep.ok ? MB_ICONINFORMATION : MB_ICONWARNING);
      continue;
    }
    if (choice == 9) { snap_benchmark::run_from_launcher(true); continue; }   // host/benchmark.cpp: plays the scripted ride in a second copy of the game
    if (choice == 4) { offer_upscalers(exe_dir, data, false); continue; }
    if (choice == 5) { import_save_flow(cfg_path); continue; }
    if (choice == 12) { restore_backup_flow(cfg_path); continue; }
    if (choice == 6) { export_save_flow(cfg_path); continue; }
    if (choice == 3) {   // the active profile's save folder (the one the game would use), else the legacy folder
      std::string sf, label;
      if (!save_target(cfg_path, &sf, &label)) sf = save_folder(cfg);
      std::error_code e3;
      fs::create_directories(fs::u8path(sf), e3);
      ShellExecuteW(nullptr, L"explore", W(sf).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      continue;
    }
    if (choice == 2) {
      std::string p = pick_folder(romfs);
      if (p.empty()) continue;
      auto uv = std::make_shared<snap_launcher::UiVerify>();
      std::string bad = verify_now(*uv, p, data, verify_cfg(launcher_data_dir(), cfg), true);
      if (!bad.empty()) { message("Snap Recomp", bad, MB_ICONWARNING); continue; }
      cfg.set("romfs_root", hle::Json::string(p));
      s_verify = uv;   // that folder was just checked: no second pass
      s_verify->state.store(1);
      s_verify_root = p;
      std::string text;
      hle::json_dump(cfg, text);
      std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
      f << text << "\n";
      continue;
    }
    std::string bad;
    if (!(s_verify && s_verify_root == romfs && s_verify->state.load() == 1)) {   // the themed window already passed this folder: not twice
      snap_launcher::UiVerify uv;
      bad = verify_now(uv, romfs, data, verify_cfg(launcher_data_dir(), cfg), false);
      if (autoplay) std::fprintf(stderr, "launcher: game folder %s in %d ms (%s)\n", bad.empty() ? "passed" : "failed", uv.ms.load(), uv.why.load() ? "walked the folder" : "remembered result");
    }
    if (!bad.empty()) {
      if (quiet) { std::fprintf(stderr, "launcher: %s\n", bad.c_str()); if (r.play) r.exit_code = 1; return true; }
      message("Snap Recomp", bad, MB_ICONWARNING);
      continue;
    }
    if (autoplay && std::getenv("SNAP_LAUNCHER_VERIFY_ONLY")) return true;   // test switch: time the game folder check, then stop without starting the game
    break;
  }
  auto s = [&](const char* k) { return cfg.get(k) ? cfg.get(k)->str() : std::string(); };
  r.main_elf = s("main_elf");
  r.sdk_elf = s("sdk_elf");
  if (r.main_elf.empty() || r.sdk_elf.empty() || !fs::exists(fs::u8path(r.main_elf), ec) || !fs::exists(fs::u8path(r.sdk_elf), ec)) {
    const std::string why = "The converted game files are missing (" + (r.main_elf.empty() ? "main_elf" : r.main_elf) + "). Run the builder again.";
    if (quiet) { std::fprintf(stderr, "launcher: %s\n", why.c_str()); r.exit_code = 2; }
    else message("Snap Recomp", why, MB_ICONERROR);
    return true;
  }
  for (const char* which : {"main", "sdk"}) {   // an empty or cut-off converted file gets a plain message here, not a crash in the loader
    std::string why;
    if (elf_check::check_file(std::string(which) == "main" ? r.main_elf.c_str() : r.sdk_elf.c_str(), which, why)) continue;
    if (quiet) { std::fprintf(stderr, "launcher: %s\n", why.c_str()); r.exit_code = 2; }
    else message("Snap Recomp", why, MB_ICONERROR);
    return true;
  }
  if (!quiet) loader::g_fatal_hook = [](const char* m) { message("Snap Recomp", std::string(m), MB_ICONERROR); };   // a failure while loading shows the same way
  r.config_path = U8(cfg_path.wstring());
  r.proceed = true;
  if (!autoplay) _putenv_s("SNAP_LAUNCHER_STARTING", "1");   // the game window shows "Starting the game..." until its first picture (runtime/ui/ui_fps.cpp)
  if (safe) _putenv_s("SNAP_SAFE_MODE", "1");   // read by settings (trimmed copy, no writes), mods (no packs), the shader cache (own folder) and the banner
  // logs: %LOCALAPPDATA%\SnapRecomp\logs (stderr to a file; the trace file for the boot log)
  const fs::path logs = logs_dir();
  if (!logs.empty()) {
    fs::create_directories(logs, ec);
    snap_crashlog_rotate(logs.c_str());   // host/mods/crashlog.cpp: last run's stderr.log and trace.log become *.prev
    snap_crashlog_set_dir(U8(logs.wstring()).c_str());   // _crash.txt goes next to the log
    FILE* f = nullptr;
    if (_wfreopen_s(&f, (logs / L"stderr.log").c_str(), L"w", stderr) == 0 && f) setvbuf(stderr, nullptr, _IOLBF, 1 << 16);
    r.trace_file = std::getenv("SNAP_BOOT_TRACE") ? std::getenv("SNAP_BOOT_TRACE") : U8((logs / L"trace.log").wstring());
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) <= 1) FreeConsole();   // a console-subsystem build (snap_boot.exe) started by double-click: no console window (SnapRecomp.exe is Windows-subsystem and has none)
  }
  return true;
}

}  // namespace snap_launcher
