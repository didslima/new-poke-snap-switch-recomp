// Upscaler downloads for the launcher (builder release): AMD FSR, NVIDIA DLSS and Intel XeSS are never shipped in our zip. The launcher
// offers each one, shows the vendor license (downloaded and hash checked first), and downloads the files only after the user clicks
// "Accept license and download". Pins (URL, size, SHA-256, destination) come from launcher_data.json ("upscalers", written by the
// builder from versions.json). Every file is hash checked before it is moved into place; a zip is taken apart with the system tar.exe
// and only the pinned members are kept. Same rules as builder/snapbuild/upscalers.py.
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hle/json_lite.h"
#include "launcher.h"

namespace fs = std::filesystem;

namespace {

std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

struct Sha {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  Sha() {
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (alg) BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
  }
  void add(const void* p, size_t n) { if (h) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
  std::string hex() {
    unsigned char d[32] = {};
    if (h) { BCryptFinishHash(h, d, 32, 0); BCryptDestroyHash(h); h = nullptr; }
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (unsigned char c : d) { o += hx[c >> 4]; o += hx[c & 15]; }
    return o;
  }
  ~Sha() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
};

std::string file_sha(const fs::path& p, long long* size = nullptr) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  Sha s;
  std::vector<char> buf(1 << 20);
  long long total = 0;
  while (f) {
    f.read(buf.data(), (std::streamsize)buf.size());
    std::streamsize n = f.gcount();
    if (n > 0) { s.add(buf.data(), (size_t)n); total += n; }
  }
  if (size) *size = total;
  return s.hex();
}

struct Item { std::string dest, url, member, sha; long long size = 0; };
struct Up {
  std::string name, title, detail, vendor;
  Item lic;                       // dest = license file name
  Item arch;                      // url empty when the files are downloaded one by one
  std::vector<Item> files;
};

std::string S(const hle::Json& j, const char* k) { const hle::Json* p = j.get(k); return p ? p->str() : std::string(); }
long long N(const hle::Json& j, const char* k) { const hle::Json* p = j.get(k); return p ? (long long)p->num() : 0; }

std::vector<Up> parse(const hle::Json& data) {
  std::vector<Up> out;
  const hle::Json* ups = data.get("upscalers");
  if (!ups || ups->type != hle::Json::Obj) return out;
  for (auto& kv : ups->o) {
    const hle::Json& j = kv.second;
    Up u;
    u.name = kv.first; u.title = S(j, "title"); u.detail = S(j, "detail"); u.vendor = S(j, "vendor");
    if (const hle::Json* l = j.get("license")) { u.lic.dest = S(*l, "filename"); u.lic.url = S(*l, "url"); u.lic.sha = S(*l, "sha256"); u.lic.size = N(*l, "size"); }
    if (const hle::Json* a = j.get("archive")) { u.arch.dest = S(*a, "filename"); u.arch.url = S(*a, "url"); u.arch.sha = S(*a, "sha256"); u.arch.size = N(*a, "size"); }
    if (const hle::Json* fl = j.get("files")) for (auto& f : fl->a) {
      Item it; it.dest = S(f, "dest"); it.url = S(f, "url"); it.member = S(f, "member"); it.sha = S(f, "sha256"); it.size = N(f, "size");
      u.files.push_back(it);
    }
    if (u.name.empty() || u.lic.url.empty() || u.files.empty()) continue;
    out.push_back(u);
  }
  return out;
}

fs::path rel(const std::string& d) { return fs::u8path(d); }

bool installed(const Up& u, const fs::path& dir) {
  for (auto& f : u.files) {
    long long sz = 0;
    fs::path p = dir / rel(f.dest);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || (long long)fs::file_size(p, ec) != f.size) return false;
    if (file_sha(p, &sz) != f.sha) return false;
  }
  return true;
}

// One HTTPS download try. `retry` is set when the file arrived short or the connection broke (worth another try); a wrong hash is not.
bool download_once(const std::string& url, const fs::path& dest, const std::string& want_sha, long long want_size, std::atomic<long long>* progress,
                   std::atomic<bool>* cancel, std::string& err, long long base, bool& retry) {
  retry = false;
  if (url.rfind("https://", 0) != 0) { err = "refusing a download URL that is not https"; return false; }
  std::wstring wurl = W(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
  HINTERNET ses = WinHttpOpen(L"SnapRecompLauncher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!ses) { err = "network unavailable"; return false; }
  DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
  HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  bool ok = false;
  fs::path part = dest; part += L".part";
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
    DWORD code = 0, cl = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl, WINHTTP_NO_HEADER_INDEX);
    if (code != 200) { err = "the server answered " + std::to_string(code); }
    else {
      std::error_code ec;
      fs::create_directories(dest.parent_path(), ec);
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      Sha sha;
      std::vector<char> buf(1 << 18);
      long long total = 0;
      DWORD clen = 0, clsz = sizeof clen;   // Content-Length, when the server sent one
      if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &clen, &clsz, WINHTTP_NO_HEADER_INDEX)) clen = 0;
      for (;;) {
        if (cancel && cancel->load()) { err = "cancelled"; break; }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) { err = "the connection broke"; retry = true; break; }
        if (!got) { ok = true; break; }
        out.write(buf.data(), got);
        sha.add(buf.data(), got);
        total += got;
        if (progress) progress->store(base + total);   // base: bytes of the files before this one, so the bar runs across all of them
      }
      out.close();
      if (ok && ((clen && total != (long long)clen) || (want_size && total != want_size))) {
        ok = false; retry = true;
        err = "the file arrived incomplete (" + std::to_string(total) + " of " + std::to_string(want_size ? want_size : (long long)clen) + " bytes)";
      }
      if (ok && sha.hex() != want_sha) { ok = false; err = "the file does not match the published SHA-256"; }
    }
  } else if (err.empty()) { err = "could not reach the server"; retry = true; }
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  std::error_code ec;
  if (!ok) { fs::remove(part, ec); return false; }
  fs::remove(dest, ec);
  fs::rename(part, dest, ec);
  if (ec) { err = "could not save the file"; return false; }
  return true;
}

// HTTPS download to `dest` (hash checked while streaming; deleted when it does not match). progress/cancel are optional. A short file or a broken
// connection is tried again up to 3 times with a short wait; the final message says in plain words what happened.
bool download(const std::string& url, const fs::path& dest, const std::string& want_sha, long long want_size, std::atomic<long long>* progress,
              std::atomic<bool>* cancel, std::string& err, long long base = 0) {
  const int kTries = 3;
  const DWORD waits[2] = {2000, 5000};
  for (int i = 0; i < kTries; ++i) {
    bool retry = false;
    err.clear();
    if (download_once(url, dest, want_sha, want_size, progress, cancel, err, base, retry)) return true;
    if (!retry || i == kTries - 1) {
      if (retry) err += ". It failed " + std::to_string(kTries) + " times in a row, so nothing was installed. Check your internet connection and try again";
      return false;
    }
    for (DWORD w = 0; w < waits[i]; w += 100) { if (cancel && cancel->load()) { err = "cancelled"; return false; } Sleep(100); }
  }
  return false;
}

bool place(const fs::path& src, const fs::path& dst, const std::string& sha, long long size, std::string& err) {
  long long sz = 0;
  if (file_sha(src, &sz) != sha || sz != size) { err = "an extracted file does not match its published SHA-256"; return false; }
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::path tmp = dst; tmp += L".tmp";
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) { err = "could not write the file"; return false; }
  fs::remove(dst, ec);
  fs::rename(tmp, dst, ec);
  if (ec) { err = "could not move the file into place"; return false; }
  return true;
}

// Pull the pinned members out of a zip with the system tar.exe (bsdtar reads zip). Returns false and sets err on failure.
bool untar_members(const fs::path& zip, const fs::path& dest_dir, const std::vector<std::string>& members, std::string& err) {
  wchar_t sys[MAX_PATH] = {};
  GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" + dest_dir.wstring() + L"\"";
  for (auto& m : members) cmd += L" \"" + W(m) + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
  if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { err = "tar.exe could not start"; return false; }
  WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  if (code != 0) { err = "could not unpack the archive"; return false; }
  return true;
}

struct Job {
  Up u;
  fs::path dir, tmp;
  std::atomic<long long> done{0};
  std::atomic<bool> cancel{false}, finished{false}, ok{false};
  long long total = 0;
  std::string err;
};

void run_job(Job* j) {
  const Up& u = j->u;
  std::error_code ec;
  fs::create_directories(j->tmp, ec);
  bool good = true;
  if (!u.arch.url.empty()) {
    fs::path z = j->tmp / rel(u.arch.dest);
    good = download(u.arch.url, z, u.arch.sha, u.arch.size, &j->done, &j->cancel, j->err);
    if (good) {
      std::vector<std::string> mem;
      for (auto& f : u.files) mem.push_back(f.member);
      fs::path x = j->tmp / "x";
      fs::create_directories(x, ec);
      good = untar_members(z, x, mem, j->err);
      for (auto& f : u.files) if (good) good = place(x / rel(f.member), j->dir / rel(f.dest), f.sha, f.size, j->err);
    }
  } else {
    long long base = 0;
    for (auto& f : u.files) {
      if (!good) break;
      fs::path t = j->tmp / (rel(f.dest).filename().wstring() + L".dl");
      good = download(f.url, t, f.sha, f.size, &j->done, &j->cancel, j->err, base) && place(t, j->dir / rel(f.dest), f.sha, f.size, j->err);
      base += f.size;
    }
  }
  if (good) {   // the license text travels with the files
    fs::path l = j->tmp / rel(u.lic.dest);
    good = download(u.lic.url, l, u.lic.sha, u.lic.size, nullptr, nullptr, j->err) && place(l, j->dir / "licenses" / "upscalers" / rel(u.lic.dest), u.lic.sha, u.lic.size, j->err);
  }
  fs::remove_all(j->tmp, ec);   // the archive and everything else we did not keep
  j->ok = good;
  j->finished = true;
}

HRESULT CALLBACK progress_cb(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref) {
  Job* j = (Job*)ref;
  if (msg == TDN_CREATED) { SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000)); }
  if (msg == TDN_TIMER) {
    long long tot = j->total ? j->total : 1;   // the bar counts bytes across every file of the upscaler (the archive, or the files one by one)
    SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(std::min<long long>(1000, j->done.load() * 1000 / tot)), 0);
    if (j->total > 0) {   // "12 of 85 MB" under the bar
      wchar_t t[160];
      swprintf_s(t, L"%lld of %lld MB. The file is checked against its published SHA-256 before it is installed.", j->done.load() >> 20, j->total >> 20);
      SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)t);
    }
    if (j->finished) SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);   // the only button is Cancel; j->finished makes the click harmless
  }
  if (msg == TDN_BUTTON_CLICKED) { if (!j->finished) { j->cancel = true; } }
  return S_OK;
}

// Shows the license (opens the saved copy) and asks. Returns 1 accept, 2 skip, 0 closed.
int ask_license(const Up& u, const fs::path& lic_file) {
  std::wstring title = W(u.title);
  std::wstring content = W(u.detail) + L".\n\nThe files come from " + W(u.vendor) + L"'s official download after you accept " + W(u.vendor) +
                         L"'s license. Snap Recomp does not include them and does not accept the license for you.";
  TASKDIALOG_BUTTON b[3] = {{3001, L"Read the license"}, {3002, L"Accept the license and download"}, {3003, L"Skip"}};
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = title.c_str();
  c.pszContent = content.c_str();
  c.pButtons = b; c.cButtons = 3; c.nDefaultButton = 3001;
  for (;;) {
    int sel = 0;
    if (FAILED(TaskDialogIndirect(&c, &sel, nullptr, nullptr))) return 0;
    if (sel == 3001) { ShellExecuteW(nullptr, L"open", lic_file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); continue; }
    return sel == 3002 ? 1 : sel == 3003 ? 2 : 0;
  }
}

}  // namespace

namespace snap_launcher {

// Test switch: SNAP_UPSCALER_AUTOACCEPT=<name,name> accepts those licenses without the dialog (an explicit act by whoever runs the test).
static bool auto_accepted(const std::string& n) {
  const char* e = std::getenv("SNAP_UPSCALER_AUTOACCEPT");
  if (!e) return false;
  std::string s = std::string(",") + e + ",";
  return s.find("," + n + ",") != std::string::npos;
}

// The receipt next to the exe: the user accepted this upscaler's license (a button in the window, or --accept-license on the command line).
static void write_receipt(const fs::path& exe_dir, const std::string& name) {
  fs::path rc = exe_dir / "upscalers.json";
  hle::Json r;
  { std::ifstream f(rc, std::ios::binary); if (f) { std::stringstream ss; ss << f.rdbuf(); std::string e; hle::json_parse(ss.str(), r, e); } }
  if (r.type != hle::Json::Obj) r = hle::Json::object();
  hle::Json one = hle::Json::object();
  one.set("license_accepted_by_user", hle::Json::boolean_(true));
  r.set(name, one);
  std::string text; hle::json_dump(r, text);
  std::ofstream(rc, std::ios::binary | std::ios::trunc) << text << "\n";
}

int upscalers_cli(const fs::path& exe_dir, const hle::Json& data, const std::vector<std::string>& args, const std::function<void(const std::string&)>& say) {
  const std::vector<Up> ups = parse(data);
  auto find = [&](const std::string& n) -> const Up* { for (auto& u : ups) if (u.name == n) return &u; return nullptr; };
  std::string all;
  for (auto& u : ups) all += (all.empty() ? "" : ", ") + u.name;
  std::vector<std::string> want, accepted;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--accept-license") {
      if (i + 1 >= args.size()) { say("--accept-license needs an upscaler name (" + all + ")"); return 2; }
      accepted.push_back(args[++i]);
    } else if (args[i].compare(0, 2, "--") == 0) { say("--upscalers does not know " + args[i]); return 2; }
    else want.push_back(args[i]);
  }
  for (auto& n : want) if (!find(n)) { say("There is no upscaler named \"" + n + "\". The names are: " + (all.empty() ? "(none in this build)" : all)); return 2; }
  for (auto& n : accepted) if (!find(n)) { say("--accept-license: there is no upscaler named \"" + n + "\". The names are: " + (all.empty() ? "(none in this build)" : all)); return 2; }
  if (want.empty()) {
    if (!accepted.empty()) { say("Name the upscaler to download as well, for example: --upscalers " + accepted[0] + " --accept-license " + accepted[0]); return 2; }
    if (ups.empty()) say("This build lists no upscalers.");
    for (auto& u : ups) say(u.name + "  " + u.title + "  " + (installed(u, exe_dir) ? "installed" : "not installed") + "  license: " + u.lic.url);
    if (!ups.empty()) say("Download one with: --upscalers <name> --accept-license <name>. The files come from the vendor's official download; you accept the vendor's license yourself.");
    return 0;
  }
  int rc = 0;
  for (auto& n : want) {
    const Up& u = *find(n);
    if (installed(u, exe_dir)) { say(u.title + " is already installed."); continue; }
    if (std::find(accepted.begin(), accepted.end(), n) == accepted.end()) {
      say(u.title + " was not downloaded: read " + u.vendor + "'s license at " + u.lic.url + " and, if you accept it, add --accept-license " + n + ".");
      rc = 1;
      continue;
    }
    Job job;
    job.u = u; job.dir = exe_dir; job.tmp = exe_dir / "downloads" / u.name;
    say("Downloading " + u.title + " from " + u.vendor + "'s official source...");
    run_job(&job);
    if (job.ok) { write_receipt(exe_dir, u.name); say(u.title + " is installed."); }
    else { say(u.title + " was not installed: " + job.err + ". Nothing was changed."); rc = 1; }
  }
  return rc;
}

// One pass over every upscaler that is not installed: license dialog, then the download with a progress bar. `only_default_unasked`
// restricts the pass to the pre-selected upscaler (FSR) when the launcher offers it at the first start. Returns the number installed.
int offer_upscalers(const fs::path& exe_dir, const hle::Json& data, bool first_start) {
  std::vector<Up> ups = parse(data);
  int installed_now = 0;
  for (auto& u : ups) {
    if (installed(u, exe_dir)) continue;
    if (first_start && u.name != "fsr" && !auto_accepted(u.name)) continue;   // FSR is offered up front; DLSS and XeSS wait for the menu
    fs::path tmp = exe_dir / "downloads" / u.name;
    std::string err;
    fs::path lic = tmp / rel(u.lic.dest);
    if (!download(u.lic.url, lic, u.lic.sha, u.lic.size, nullptr, nullptr, err)) {
      MessageBoxW(nullptr, W("The " + u.vendor + " license could not be downloaded: " + err).c_str(), L"Snap Recomp", MB_OK | MB_ICONWARNING);
      continue;
    }
    int a = auto_accepted(u.name) ? 1 : ask_license(u, lic);
    if (a != 1) { std::error_code ec; fs::remove_all(tmp, ec); continue; }
    auto job = std::make_unique<Job>();
    job->u = u; job->dir = exe_dir; job->tmp = tmp;
    job->total = u.arch.url.empty() ? 0 : u.arch.size;
    if (!u.arch.url.empty()) job->total = u.arch.size;
    else { for (auto& f : u.files) job->total += f.size; }   // the bar counts the bytes of all the files together (download() adds the earlier files as its base)
    std::thread th(run_job, job.get());
    std::wstring ti = W("Downloading " + u.title);
    std::wstring ct = L"From " + W(u.vendor) + L"'s official source. The file is checked against its published SHA-256 before it is installed.";
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = L"Snap Recomp";
    c.pszMainInstruction = ti.c_str();
    c.pszContent = ct.c_str();
    c.pfCallback = progress_cb;
    c.lpCallbackData = (LONG_PTR)job.get();
    if (!auto_accepted(u.name)) TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    else while (!job->finished) Sleep(200);
    if (!job->finished) job->cancel = true;
    th.join();
    if (job->ok) {
      ++installed_now;
      write_receipt(exe_dir, u.name);   // the user accepted this license by clicking the button above
    } else if (job->err != "cancelled" && !job->err.empty()) {
      MessageBoxW(nullptr, W(u.title + " was not installed: " + job->err + ".\nNothing was changed. The game works without it.").c_str(), L"Snap Recomp", MB_OK | MB_ICONWARNING);
    }
  }
  return installed_now;
}

}  // namespace snap_launcher
