// SPDX-License-Identifier: MIT
// HD photos: see nvn_hdphoto.h and analysis/core/hd_photos.md.
//
// Chain of one photo (all host side; the guest reads nothing from here):
//   shutter     CapturePS draws the scene into the 360x240 RGBA8 target T: HdPhotoCapture blits the region the draw samples
//               (from its texture coordinates) into a 2160x1440 RGBA8_SRGB image (the blit does the sRGB encode CapturePS
//               does), copies it to a host buffer (readback kind 4) and links T -> HD. The BC1 pass that packs T into the
//               thumbnail B links B -> HD (HdPhotoCompress); copies pass the link on. Photo select's large preview samples B:
//               HdPhotoSubstitute swaps in the HD texture.
//   submit      the game re-renders the photo (whole frame, 3:2 projection) into the 1080x720 target C: a new capture,
//   album save  linked C -> HD. The game copies C to a buffer and encodes the JPEG; snap_hdphoto_encoded finds the capture by
//               content (box averages) and the JPEG's CRC32 names the HD file, written q95 to <save root>\photos_hd\<crc>.jpg
//               (tmp file, flush, rename).
//   album       JpegDecoder::DecodeWithPreconfiguredDecoder (host/mods/shiny.cpp) reports (buffer, CRC); the texture made from
//               that buffer (CopyBufferToTexture, or a texture whose storage is the buffer) is linked to the CRC's HD copy,
//               loaded lazily from disk by a worker thread.
// Display textures are created on first large draw (sprite >= kMinDrawW UI units wide; small grid cells keep the game's texture,
// the HD image has no mip chain) and kept in an LRU of about kGpuBudget bytes. Missing or corrupt files fall back silently.
#include <chrono>
#include <smmintrin.h>
#include <windows.h>
#include <wincodec.h>
#include <objidl.h>

#include "hle/visual_defaults.h"   // HD photos default (SNAP_VISUALS)
#include "nvn_internal.h"
#include "nvn_hdphoto.h"
#include "nvn_capture.h"
#include "hle/hle_config.h"
#include "hle/settings.h"          // hires_photos
#include "nvn_shot.h"              // PNG writer (full-size photo PNGs)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowscodecs")

namespace nvn {
namespace {
constexpr int kW = 2160, kH = 1440;
constexpr uint64_t kBytes = (uint64_t)kW * kH * 4;
constexpr uint64_t kGpuBudget = 256ull << 20;
constexpr float kMinDrawW = 480.0f;   // UI units (1920 wide): the photo select preview is ~1150, grid cells ~200
constexpr size_t kMaxFiles = 1200;        // well above the album's own limit; older HD files beyond it are deleted (oldest first)
constexpr int kMiniW = 135, kMiniH = 90;   // 16x16 HD texels = 8x8 photo texels per cell
constexpr float kMatchMax = 10.0f;        // mean abs difference (0..255) accepted as the same picture

// Full-size photo PNGs (SNAP_HIRES_PHOTOS): the 1080x720 re-render's scene region at full height in the photo's 3:2 shape (sRGB-encoded), in a host buffer of
// its own, written as a PNG on the screenshot writer thread (nvn_shot.cpp) to photos_hd\pending, then named <crc>.png when the
// album JPEG's CRC ties to the capture. At most kHiLive buffers (~63 MB each at 4860x3240) at a time.
constexpr int kHiLive = 3;
std::atomic<int> g_hiLive{0};
struct HiBuf {
  VkDevice dev = VK_NULL_HANDLE; VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr;
  uint32_t w = 0, h = 0;
  HiBuf() { g_hiLive++; }
  ~HiBuf() { if (buf) vkDestroyBuffer(dev, buf, nullptr); if (mem) vkFreeMemory(dev, mem, nullptr); g_hiLive--; }
};

struct Hd {
  uint64_t id = 0;
  int present = 0;
  std::vector<uint8_t> rgba;   // kW x kH, sRGB-encoded R,G,B,A bytes (empty while only the JPEG is held)
  std::vector<uint8_t> jpg;    // q95 copy of a session capture (written to disk once its album CRC is known)
  uint32_t crc = 0;            // album JPEG CRC32 (0 = not saved)
  bool fromDisk = false, busy = false, bad = false, written = false;
  TextureImpl* gpu[2] = {};    // display textures: [0] RGBA8 UNORM, [1] RGBA8 SRGB (matches the replaced texture's encoding)
  uint64_t lastUse = 0;
  // A box-filtered level (kW >> midLevel wide) for UI draws too small for the full copy, such as photo select's strip of small
  // photos: the game's 360x240 texture drawn at 4K is a 1.3x upscale, the level is not. Own small LRU (EvictMid).
  std::vector<uint8_t> midPx;
  int midW = 0, midH = 0, midLevel = 0, midRetry = 0;
  bool midBusy = false;
  TextureImpl* mid[2] = {};
  std::vector<uint8_t> mini;   // kMiniW x kMiniH RGB box average: finds the capture an album JPEG was made from
  bool hiOn = false;               // a full-size PNG was taken of this capture
  std::shared_ptr<HiBuf> hi;       // its GPU copy, until the capture's readback
  std::string hiFile;              // the PNG once written (pending, then the first <crc>.png)
  std::vector<uint32_t> hiCrcs;    // album photos that are this capture
  size_t hiPlaced = 0;
};
using HdP = std::shared_ptr<Hd>;

bool g_log = false, g_noMid = false;
int g_minH = 720;   // the smallest sampled region (host pixels tall) that gets an HD copy; below 1440 the copy is a linear upscale of the region (SNAP_HD_PHOTOS_MIN_H, 1411 = the old 4K-only gate)
std::mutex g_mu;
std::atomic<int> g_active{0};                            // links + decoded buffers: zero keeps HdPhotoSubstitute a load + compare
std::unordered_map<const TextureImpl*, HdP> g_link;     // guest texture -> HD copy of what it shows
std::map<uint64_t, HdP> g_pending;                      // capture id -> HD waiting for its readback
std::deque<HdP> g_recent;                               // the session's captures (shots and photo re-renders), newest last
struct EncPix { const uint8_t* data; int w, h; };
std::unordered_map<uint64_t, EncPix> g_encBuf;          // JpegEncoder object -> its pixel buffer
std::unordered_map<uint32_t, HdP> g_byCrc;
std::unordered_set<uint32_t> g_noFile;                  // CRCs checked and without an HD file
struct Dec { uint64_t size; uint32_t crc; HdP hd; };
std::map<const uint8_t*, Dec> g_dec;                    // decoded album photo buffers (latest decode per buffer)
// The album copies the decoded pixels into another buffer before the texture upload: decoded photos are also found by the
// hash of their first kHeadBytes (15 rows of 1080 RGBA pixels), checked when a 1080x720 texture is uploaded.
constexpr size_t kHeadBytes = 64 * 1024;
std::unordered_map<uint64_t, HdP> g_decHead;
uint64_t HeadHash(const uint8_t* p) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < kHeadBytes; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
  return h;
}
std::vector<HdP> g_gpuLive;                             // HDs holding display textures (LRU by lastUse)
std::vector<HdP> g_midLive;                             // HDs holding level textures (LRU by lastUse)
constexpr uint64_t kMidBudget = 48ull << 20;            // a 540x360 level is 0.8 MB
uint64_t g_nextId = 1, g_tick = 0;
TextureImpl* g_cap[2] = {};                             // the 2160x1440 blit targets (RGBA8, RGBA8_SRGB), reused (copies are ordered on the queue)

// ---- album grid thumbnails (state; code below ThumbSubstitute) ----
// The grid draws the game's own small thumbnail JPEGs (204x136; 240x200 elsewhere), encoded a moment before or after their
// 1080x720 photo. At encode a thumbnail is matched by content to a recent photo (a region search finds its crop) and the pair is
// appended to <save root>\photos_hd\thumbs.txt ("<thumbnail crc> <photo crc> x0 y0 x1 y1", the crop in 0..1 of the photo), so the
// link survives restarts. On screen the cell shows one level of the HD crop's mip pyramid (2^L x 2^L box averages), the smallest
// level at least as wide as the cell's host pixels (the game's UI samplers have no mip filtering, so the level is picked here).
constexpr int kSmW = 270, kSmH = 180;           // photo comparison image: 4x4 box averages of the 1080x720 photo
constexpr uint64_t kThumbBudget = 64ull << 20;  // grid display textures (a 4K grid cell level is about 0.8 MB)
struct ThumbRef { uint32_t photo = 0; float r[4] = {0, 0, 1, 1}; };
struct Thumb {
  uint32_t crc = 0; ThumbRef ref; HdP photo;
  int w = 0, h = 0;                    // the game's thumbnail size
  std::vector<uint8_t> px; int pw = 0, ph = 0, level = 0;   // the chosen pyramid level (RGBA), kept until uploaded
  bool busy = false, bad = false;
  int retryAt = 0;                     // present count before which a failed load is not retried
  TextureImpl* gpu[2] = {};
  uint64_t lastUse = 0;
};
using ThumbP = std::shared_ptr<Thumb>;
std::unordered_map<uint32_t, ThumbRef> g_thumbIdx;      // thumbnail JPEG CRC -> photo CRC + crop (thumbs.txt)
bool g_thumbIdxLoaded = false;
std::unordered_map<uint32_t, ThumbP> g_thumbs;           // by thumbnail JPEG CRC
std::unordered_map<const TextureImpl*, ThumbP> g_tlink;  // guest texture -> grid thumbnail
struct TDec { uint64_t size; ThumbP th; };
std::map<const uint8_t*, TDec> g_tdec;                   // decoded thumbnail buffers
std::unordered_map<uint64_t, ThumbP> g_tHead;            // decoded thumbnails by HeadHash (the album copies the pixels first)
std::vector<ThumbP> g_tLive;                             // thumbnails holding display textures (LRU by lastUse)
struct PhotoSm { uint32_t crc; std::vector<uint8_t> rgb; ULONGLONG t; };              // kSmW x kSmH RGB of an encoded photo
struct ThumbSm { uint32_t crc; int w, h; std::vector<uint8_t> rgba; ULONGLONG t; };   // an encoded thumbnail not matched yet
std::deque<PhotoSm> g_photoSm;
std::deque<ThumbSm> g_thumbPend;
bool g_matchQueued = false;

// Box average of an RGBA image into kMiniW x kMiniH RGB (w, h multiples of the grid).
void Mini(const uint8_t* rgba, int w, int h, size_t stride, std::vector<uint8_t>& out) {
  const int bx = w / kMiniW, by = h / kMiniH;
  out.assign((size_t)kMiniW * kMiniH * 3, 0);
  for (int my = 0; my < kMiniH; my++)
    for (int mx = 0; mx < kMiniW; mx++) {
      uint32_t acc[3] = {};
      for (int y = my * by; y < (my + 1) * by; y++) {
        const uint8_t* r = rgba + (size_t)y * stride + (size_t)mx * bx * 4;
        for (int x = 0; x < bx; x++) { acc[0] += r[x * 4]; acc[1] += r[x * 4 + 1]; acc[2] += r[x * 4 + 2]; }
      }
      for (int c = 0; c < 3; c++) out[((size_t)my * kMiniW + mx) * 3 + c] = (uint8_t)(acc[c] / (uint32_t)(bx * by));
    }
}

std::atomic<int> g_tActive{0};   // grid thumbnail links + decoded buffers: zero keeps small UI textures off the lock
void UpdateActive() {
  g_active.store((int)(g_link.size() + g_dec.size() + g_tlink.size() + g_tdec.size()), std::memory_order_relaxed);
  g_tActive.store((int)(g_tlink.size() + g_tdec.size()), std::memory_order_relaxed);
}

const std::string& Dir() {
  static const std::string d = [] {
    std::string r = hle::config().save_root;
    if (r.empty()) { const char* a = getenv("APPDATA"); r = std::string(a ? a : ".") + "\\SnapRecomp\\save"; }
    return r + "\\photos_hd";
  }();
  return d;
}
std::string PathOf(uint32_t crc) { char n[32]; snprintf(n, sizeof n, "\\%08x.jpg", crc); return Dir() + n; }

uint32_t Crc32(const uint8_t* p, size_t n) {
  static uint32_t t[256]; static std::once_flag once;
  std::call_once(once, [] { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } });
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}

// ---- worker thread (JPEG encode / decode / file IO; BelowNormal) ----
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::function<void()>> g_q;
void Post(std::function<void()> f) {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      for (;;) {
        std::function<void()> j;
        { std::unique_lock<std::mutex> lk(g_qmu); g_qcv.wait(lk, [] { return !g_q.empty(); }); j = std::move(g_q.front()); g_q.pop_front(); }
        j();
      }
    }).detach();
  });
  { std::lock_guard<std::mutex> lk(g_qmu); g_q.push_back(std::move(f)); }
  g_qcv.notify_one();
}

IWICImagingFactory* Wic() {   // worker thread only
  static IWICImagingFactory* f = nullptr;
  if (!f && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) f = nullptr;
  return f;
}

bool EncodeJpeg(const std::vector<uint8_t>& rgba, std::vector<uint8_t>& out) {
  IWICImagingFactory* f = Wic();
  if (!f) return false;
  IStream* ms = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &ms))) return false;
  IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
  bool ok = false;
  std::vector<uint8_t> bgr((size_t)kW * kH * 3);
  for (size_t i = 0, n = (size_t)kW * kH; i < n; i++) { bgr[i * 3] = rgba[i * 4 + 2]; bgr[i * 3 + 1] = rgba[i * 4 + 1]; bgr[i * 3 + 2] = rgba[i * 4]; }
  if (SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(ms, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&fr, &pb))) {
    PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.95f;
    pb->Write(1, &opt, &v);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(kW, kH)) && SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat24bppBGR &&
        SUCCEEDED(fr->WritePixels(kH, kW * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
      STATSTG st{};
      LARGE_INTEGER z{};
      if (SUCCEEDED(ms->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 && st.cbSize.QuadPart < (64ull << 20) && SUCCEEDED(ms->Seek(z, STREAM_SEEK_SET, nullptr))) {
        out.resize((size_t)st.cbSize.QuadPart);
        ULONG got = 0;
        ok = SUCCEEDED(ms->Read(out.data(), (ULONG)out.size(), &got)) && got == out.size();
      }
    }
  }
  if (pb) pb->Release();
  if (fr) fr->Release();
  if (enc) enc->Release();
  ms->Release();
  return ok;
}

bool DecodeJpeg(const std::vector<uint8_t>& jpg, std::vector<uint8_t>& rgba) {
  IWICImagingFactory* f = Wic();
  if (!f || jpg.size() < 4) return false;
  IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
  bool ok = false;
  UINT w = 0, h = 0;
  if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(jpg.data()), (DWORD)jpg.size())) &&
      SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
      SUCCEEDED(fr->GetSize(&w, &h)) && w == (UINT)kW && h == (UINT)kH && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    rgba.resize(kBytes);
    ok = SUCCEEDED(cv->CopyPixels(nullptr, kW * 4, (UINT)rgba.size(), rgba.data()));
  }
  if (cv) cv->Release();
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  if (!ok) rgba.clear();
  return ok;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
  bool ok = n > 0 && n < (64l << 20);
  if (ok) { out.resize((size_t)n); ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n; }
  fclose(f);
  return ok;
}

// Same as the saves: a temp file, flushed to disk, then renamed over the target.
bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& data) {
  CreateDirectoryA(Dir().c_str(), nullptr);
  const std::string tmp = path + ".tmp";
  HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wr = 0;
  bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size() && FlushFileBuffers(h);
  CloseHandle(h);
  ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileA(tmp.c_str());
  return ok;
}

// Keeps at most kMaxFiles HD files (newest by write time) and removes stale temp files. Worker thread, once per session.
void TrimDir() {
  struct F { std::string name; FILETIME t; };
  std::vector<F> fs;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((Dir() + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string n = fd.cFileName;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) { DeleteFileA((Dir() + "\\" + n).c_str()); continue; }
    if (n.size() == 12 && n.compare(8, 4, ".jpg") == 0) fs.push_back({n, fd.ftLastWriteTime});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  if (fs.size() <= kMaxFiles) return;
  std::sort(fs.begin(), fs.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
  for (size_t i = kMaxFiles; i < fs.size(); i++) {
    DeleteFileA((Dir() + "\\" + fs[i].name).c_str());
    DeleteFileA((Dir() + "\\" + fs[i].name.substr(0, 8) + ".png").c_str());   // its full-size PNG, if any
  }
  fprintf(stderr, "[hdphoto] trimmed %zu old HD files (limit %zu)\n", fs.size() - kMaxFiles, kMaxFiles);
}

void WriteIfReady(const HdP& hd) {   // g_mu held
  if (!hd->crc || hd->written || hd->jpg.empty() || hd->fromDisk) return;
  hd->written = true;
  const uint32_t crc = hd->crc;
  std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>(hd->jpg);
  Post([crc, data] {
    static std::once_flag trim;
    std::call_once(trim, TrimDir);
    const std::string p = PathOf(crc);
    const bool ok = WriteAtomic(p, *data);
    fprintf(stderr, "[hdphoto] %s %s (%zu bytes)\n", ok ? "wrote" : "FAILED to write", p.c_str(), data->size());
  });
}

// ---- full-size photo PNGs (SNAP_HIRES_PHOTOS) ----
bool HiresOn() {
  static const bool on = [] {
    const char* e = getenv("SNAP_HIRES_PHOTOS");
    const bool v = e && *e ? (*e == '1' || !_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes")) : hle::settings::get_bool("hires_photos", false);
    if (v) fprintf(stderr, "[hdphoto] full-size photo PNGs on: <crc>.png in %s\n", Dir().c_str());
    return v;
  }();
  return on;
}
std::string PendingDir() { return Dir() + "\\pending"; }

// g_mu held. Names the written PNG <crc>.png for each album photo tied to this capture: the first by a rename, any later one
// (a second JPEG of the same picture) by a copy on the writer thread.
void PlaceHires(const HdP& hd) {
  if (hd->hiFile.empty()) return;
  for (; hd->hiPlaced < hd->hiCrcs.size(); hd->hiPlaced++) {
    char n[32]; snprintf(n, sizeof n, "\\%08x.png", hd->hiCrcs[hd->hiPlaced]);
    const std::string dst = Dir() + n;
    if (hd->hiPlaced == 0) {
      const bool ok = MoveFileExA(hd->hiFile.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
      fprintf(stderr, "[hdphoto] full-size PNG %s %s (capture %llu)\n", ok ? "saved as" : "FAILED to name", dst.c_str(), (unsigned long long)hd->id);
      if (ok) hd->hiFile = dst;
      else return;
    } else {
      const std::string from = hd->hiFile;
      ShotPost([from, dst] {
        const std::string tmp = dst + ".tmp";   // copy beside the final name, then rename over it: a crash never leaves a truncated PNG
        bool ok = CopyFileA(from.c_str(), tmp.c_str(), FALSE) != 0;
        if (ok) ok = MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        else DeleteFileA(tmp.c_str());
        fprintf(stderr, "[hdphoto] full-size PNG %s %s\n", ok ? "copied to" : "FAILED to copy to", dst.c_str());
      });
    }
  }
}

// g_mu held, at the capture's readback (the full-size copy was recorded in the same command buffer, so it is complete too).
void PostHires(const HdP& hd) {
  std::shared_ptr<HiBuf> hb = std::move(hd->hi);
  hd->hi.reset();
  ShotPost([hd, hb]() mutable {
    static std::once_flag clean;   // PNGs of an earlier session that no album photo claimed
    std::call_once(clean, [] {
      WIN32_FIND_DATAA fd;
      HANDLE h = FindFirstFileA((PendingDir() + "\\*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) return;
      do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((PendingDir() + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
      FindClose(h);
    });
    CreateDirectoryA(Dir().c_str(), nullptr);
    CreateDirectoryA(PendingDir().c_str(), nullptr);
    char n[48]; snprintf(n, sizeof n, "\\capture%llu.png", (unsigned long long)hd->id);
    const std::string p = PendingDir() + n;
    const uint32_t w = hb->w, h = hb->h;
    const bool ok = ShotWritePng(p, static_cast<const uint8_t*>(hb->map), w, h, false);
    hb.reset();
    std::lock_guard<std::mutex> g(g_mu);
    if (!ok) { fprintf(stderr, "[hdphoto] full-size PNG of capture %llu FAILED (%ux%u)\n", (unsigned long long)hd->id, w, h); return; }
    if (g_log) fprintf(stderr, "[hdphoto] full-size PNG of capture %llu written (%ux%u)\n", (unsigned long long)hd->id, w, h);
    hd->hiFile = p;
    PlaceHires(hd);
  });
}

// Worker: session capture -> q95 JPEG (kept in memory; the raw pixels are dropped unless a display texture still needs them).
void PostEncode(const HdP& hd) {
  hd->busy = true;
  Post([hd] {
    std::vector<uint8_t> px;
    { std::lock_guard<std::mutex> g(g_mu); px = hd->rgba; }
    std::vector<uint8_t> jpg;
    const bool ok = !px.empty() && EncodeJpeg(px, jpg);
    std::lock_guard<std::mutex> g(g_mu);
    hd->busy = false;
    if (!ok) { hd->bad = true; fprintf(stderr, "[hdphoto] JPEG encode failed (capture %llu)\n", (unsigned long long)hd->id); return; }
    hd->jpg = std::move(jpg);
    if (!hd->gpu[0] && !hd->gpu[1]) { hd->rgba.clear(); hd->rgba.shrink_to_fit(); }
    if (g_log) fprintf(stderr, "[hdphoto] capture %llu encoded: %zu bytes\n", (unsigned long long)hd->id, hd->jpg.size());
    WriteIfReady(hd);
  });
}

// Worker: pixels for a display texture, from the in-memory JPEG or the file. Failure marks the HD bad (the game's texture stays).
void PostLoad(const HdP& hd) {
  hd->busy = true;
  Post([hd] {
    std::vector<uint8_t> jpg;
    bool disk;
    { std::lock_guard<std::mutex> g(g_mu); jpg = hd->jpg; disk = hd->fromDisk; }
    if (jpg.empty() && disk) ReadFile(PathOf(hd->crc), jpg);
    std::vector<uint8_t> px;
    const bool ok = DecodeJpeg(jpg, px);
    std::lock_guard<std::mutex> g(g_mu);
    hd->busy = false;
    if (!ok) { hd->bad = true; fprintf(stderr, "[hdphoto] HD copy %08x unreadable: showing the game's photo\n", hd->crc); return; }
    hd->rgba = std::move(px);
    if (g_log) fprintf(stderr, "[hdphoto] loaded HD copy %08x (%zu byte JPEG)\n", hd->crc, jpg.size());
  });
}

// Per-channel tone fit y = a*x + b from a capture's mini (x) to a game photo's mini (y), least squares over unclipped samples.
// The game's photo can differ from the shutter capture by a brightness/contrast step (CapturePS applies multiply/add after the
// sRGB encode); the fit removes it. Returns the mean absolute difference left after the fit (0..255).
struct Tone { float a[3] = {1, 1, 1}, b[3] = {0, 0, 0}; };
float FitDiff(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, Tone& t) {
  const size_t n = x.size() / 3;
  double s = 0;
  for (int c = 0; c < 3; c++) {
    double k = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
      const int xi = x[i * 3 + c], yi = y[i * 3 + c];
      if (yi < 4 || yi > 251 || xi < 4 || xi > 251) continue;
      k++; sx += xi; sy += yi; sxx += (double)xi * xi; sxy += (double)xi * yi;
    }
    float a = 1.0f, b = 0.0f;
    const double den = k * sxx - sx * sx;
    if (k > (double)n / 4 && den > 1e-6) { a = (float)((k * sxy - sx * sy) / den); b = (float)((sy - a * sx) / k); }
    a = std::clamp(a, 0.5f, 2.0f); b = std::clamp(b, -96.0f, 96.0f);
    t.a[c] = a; t.b[c] = b;
    for (size_t i = 0; i < n; i++) s += std::fabs(std::clamp(a * x[i * 3 + c] + b, 0.0f, 255.0f) - (float)y[i * 3 + c]);
  }
  return (float)(s / (double)x.size());
}
bool ToneIsIdentity(const Tone& t) {
  for (int c = 0; c < 3; c++) if (std::fabs(t.a[c] - 1.0f) > 0.02f || std::fabs(t.b[c]) > 1.5f) return false;
  return true;
}

// Worker: the HD copy of a game photo whose tone differs from the shutter capture: src's pixels with the fitted tone, re-encoded
// into dst (dst->busy until done; the file is written when ready).
void PostTone(const HdP& src, const HdP& dst, const Tone& t) {
  dst->busy = true;
  Post([src, dst, t] {
    std::vector<uint8_t> px, jpg;
    { std::lock_guard<std::mutex> g(g_mu); if (!src->rgba.empty()) px = src->rgba; else jpg = src->jpg; }
    bool ok = !px.empty() || DecodeJpeg(jpg, px);
    if (ok) {
      uint8_t lut[3][256];
      for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) lut[c][v] = (uint8_t)std::lround(std::clamp(t.a[c] * v + t.b[c], 0.0f, 255.0f));
      for (size_t i = 0; i + 3 < px.size(); i += 4) { px[i] = lut[0][px[i]]; px[i + 1] = lut[1][px[i + 1]]; px[i + 2] = lut[2][px[i + 2]]; }
      jpg.clear();
      ok = EncodeJpeg(px, jpg);
    }
    std::lock_guard<std::mutex> g(g_mu);
    dst->busy = false;
    if (!ok) { dst->bad = true; fprintf(stderr, "[hdphoto] tone-matched copy %08x failed: no HD copy\n", dst->crc); return; }
    dst->jpg = std::move(jpg);
    WriteIfReady(dst);
  });
}

// A game photo JPEG (1080x720) to tie to a session capture.
struct Waiting { uint32_t crc; size_t bytes; std::vector<uint8_t> mini; };
std::deque<Waiting> g_waiting;   // encoded before the readback of the capture that made them

// g_mu held. Finds the capture a game photo shows: closest after a per-channel tone fit (the game's photo can be a brightness
// step away from the capture). Accepted when the fitted difference is tiny, or small and clearly below every other capture's
// (a shot and its re-render can both be close). On success the HD file is written (re-toned when the fit is not identity).
// `final`: no more captures are coming, so a miss is logged.
bool MatchPhoto(const Waiting& w, bool final) {
  const std::vector<uint8_t>& m = w.mini;
  HdP best; float bd = 1e9f, second = 1e9f, raw = 0.0f; Tone bt;
  for (auto& hd : g_recent) {
    if (hd->mini.size() != m.size()) continue;
    Tone t;
    const float d = FitDiff(hd->mini, m, t);
    if (d < bd) {
      second = bd; bd = d; best = hd; bt = t;
      uint64_t sum = 0;
      for (size_t i = 0; i < m.size(); i++) sum += (uint64_t)std::abs((int)m[i] - (int)hd->mini[i]);
      raw = (float)sum / (float)m.size();
    } else if (d < second) second = d;
  }
  if (!best || !(bd <= 4.0f || (bd <= kMatchMax && bd <= 0.7f * second))) {
    if (final) {
      fprintf(stderr, "[hdphoto] album photo %08x: no matching capture (closest capture %llu: %.1f, %.1f after tone fit, next %.1f): no HD copy\n", w.crc,
              best ? (unsigned long long)best->id : 0ull, raw, bd, second);
      if (g_log) {   // diagnosis: the closest and the newest capture, next to the game's photo export
        for (const HdP& c : {best, g_recent.empty() ? HdP() : g_recent.back()}) {
          if (!c) continue;
          char n[64]; snprintf(n, sizeof n, "\\nomatch_%08x_capture%llu.jpg", w.crc, (unsigned long long)c->id);
          const std::string p = Dir() + n;
          Post([p, c] {
            std::vector<uint8_t> j, px;
            { std::lock_guard<std::mutex> g(g_mu); j = c->jpg; px = c->rgba; }
            if (j.empty() && !px.empty()) EncodeJpeg(px, j);
            CreateDirectoryA(Dir().c_str(), nullptr);
            if (!j.empty()) WriteAtomic(p, j);
          });
        }
      }
    }
    return false;
  }
  const bool tone = !ToneIsIdentity(bt);
  HdP hd = best;
  if (hd->crc || tone) {   // a second JPEG of one picture, or a re-toned one: its own HD object and file
    auto c = std::make_shared<Hd>();
    c->id = best->id; c->present = best->present; c->mini = best->mini;
    if (!tone) c->jpg = best->jpg;
    hd = c;
  }
  hd->crc = w.crc;
  g_byCrc[w.crc] = hd;
  g_noFile.erase(w.crc);
  fprintf(stderr, "[hdphoto] album photo %08x (%zu byte JPEG) is capture %llu (difference %.1f, %.1f after tone fit, next %.1f; tone R %.2f%+.0f G %.2f%+.0f B %.2f%+.0f)\n",
          w.crc, w.bytes, (unsigned long long)hd->id, raw, bd, second, bt.a[0], bt.b[0], bt.a[1], bt.b[1], bt.a[2], bt.b[2]);
  if (tone || (hd != best && hd->jpg.empty())) PostTone(best, hd, bt);   // re-toned, or the capture is still encoding
  else WriteIfReady(hd);
  if (best->hiOn && std::find(best->hiCrcs.begin(), best->hiCrcs.end(), w.crc) == best->hiCrcs.end()) {   // its full-size PNG
    if (tone) fprintf(stderr, "[hdphoto] album photo %08x: tone differs from the capture; the full-size PNG is the capture as rendered\n", w.crc);
    best->hiCrcs.push_back(w.crc);
    PlaceHires(best);
  }
  return true;
}

HdP ByCrc(uint32_t crc) {   // g_mu held; nullptr when no HD file exists
  auto it = g_byCrc.find(crc);
  if (it != g_byCrc.end()) return it->second;
  if (g_noFile.count(crc)) return nullptr;
  if (GetFileAttributesA(PathOf(crc).c_str()) == INVALID_FILE_ATTRIBUTES) { g_noFile.insert(crc); return nullptr; }
  auto hd = std::make_shared<Hd>();
  hd->id = g_nextId++; hd->crc = crc; hd->fromDisk = true; hd->written = true;
  g_byCrc[crc] = hd;
  return hd;
}

void SetLink(const TextureImpl* t, const HdP& hd) { if (hd) g_link[t] = hd; else g_link.erase(t); UpdateActive(); }

void EvictLocked(const HdP& keep) {
  uint64_t bytes = 0;
  for (auto& h : g_gpuLive) for (auto* t : h->gpu) if (t) bytes += kBytes;
  while (bytes > kGpuBudget && g_gpuLive.size() > 1) {
    auto lru = std::min_element(g_gpuLive.begin(), g_gpuLive.end(), [&](const HdP& a, const HdP& b) { return (a == keep ? UINT64_MAX : a->lastUse) < (b == keep ? UINT64_MAX : b->lastUse); });
    if (*lru == keep) break;
    for (auto*& t : (*lru)->gpu) if (t) { Retire(GK_TEXTURE, t); t = nullptr; bytes -= kBytes; }   // freed once the GPU is past it
    if (g_log) fprintf(stderr, "[hdphoto] evicted display texture of capture %llu\n", (unsigned long long)(*lru)->id);
    g_gpuLive.erase(lru);
  }
}

// ---- album grid thumbnails: index, matching, pyramid level ----
std::string ThumbIdxPath() { return Dir() + "\\thumbs.txt"; }
bool RegionOk(const float* r) { return r[0] >= 0.0f && r[1] >= 0.0f && r[2] <= 1.0f && r[3] <= 1.0f && r[2] - r[0] >= 0.1f && r[3] - r[1] >= 0.1f; }

void LoadThumbIdx() {   // g_mu held; once per session (a small text file)
  if (g_thumbIdxLoaded) return;
  g_thumbIdxLoaded = true;
  FILE* f = fopen(ThumbIdxPath().c_str(), "rb");
  if (!f) return;
  char line[200];
  while (fgets(line, sizeof line, f)) {
    unsigned a = 0, b = 0; ThumbRef r;
    if (sscanf(line, "%8x %8x %f %f %f %f", &a, &b, &r.r[0], &r.r[1], &r.r[2], &r.r[3]) == 6 && RegionOk(r.r)) { r.photo = b; g_thumbIdx[a] = r; }
  }
  fclose(f);
  if (g_log) fprintf(stderr, "[hdphoto] thumbnail index: %zu entries from %s\n", g_thumbIdx.size(), ThumbIdxPath().c_str());
}

// Area average of the region (x0,y0)-(x1,y1) of an RGBA image (pixel units) into gw x gh RGB cells.
void SampleRgba(const uint8_t* p, int w, int h, size_t stride, float x0, float y0, float x1, float y1, int gw, int gh, std::vector<uint8_t>& out) {
  out.assign((size_t)gw * gh * 3, 0);
  for (int gy = 0; gy < gh; gy++) {
    const int ya = std::clamp((int)std::lround(y0 + (y1 - y0) * gy / gh), 0, h - 1), yb = std::clamp((int)std::lround(y0 + (y1 - y0) * (gy + 1) / gh), ya + 1, h);
    for (int gx = 0; gx < gw; gx++) {
      const int xa = std::clamp((int)std::lround(x0 + (x1 - x0) * gx / gw), 0, w - 1), xb = std::clamp((int)std::lround(x0 + (x1 - x0) * (gx + 1) / gw), xa + 1, w);
      uint32_t acc[3] = {};
      for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) { const uint8_t* q = p + (size_t)y * stride + (size_t)x * 4; acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; }
      const uint32_t n = (uint32_t)((yb - ya) * (xb - xa));
      for (int c = 0; c < 3; c++) out[((size_t)gy * gw + gx) * 3 + c] = (uint8_t)(acc[c] / n);
    }
  }
}

// Summed-area table of a kSmW x kSmH RGB image: region averages in O(1) for the crop search.
struct Sat {
  std::vector<uint32_t> s;   // (kSmW + 1) x (kSmH + 1) x 3
  void Build(const std::vector<uint8_t>& rgb) {
    s.assign((size_t)(kSmW + 1) * (kSmH + 1) * 3, 0);
    for (int y = 1; y <= kSmH; y++)
      for (int x = 1; x <= kSmW; x++)
        for (int c = 0; c < 3; c++)
          s[((size_t)y * (kSmW + 1) + x) * 3 + c] = rgb[((size_t)(y - 1) * kSmW + (x - 1)) * 3 + c] + s[((size_t)(y - 1) * (kSmW + 1) + x) * 3 + c] +
                                                    s[((size_t)y * (kSmW + 1) + x - 1) * 3 + c] - s[((size_t)(y - 1) * (kSmW + 1) + x - 1) * 3 + c];
  }
  uint32_t At(int x, int y, int c) const { return s[((size_t)y * (kSmW + 1) + x) * 3 + c]; }
  // mean absolute difference between the region (sm pixels) sampled into gw x gh cells and `ref` (gw x gh RGB)
  float Diff(float x0, float y0, float x1, float y1, int gw, int gh, const std::vector<uint8_t>& ref, std::vector<uint8_t>* out = nullptr) const {
    uint64_t d = 0;
    if (out) out->assign((size_t)gw * gh * 3, 0);
    for (int gy = 0; gy < gh; gy++) {
      const int ya = std::clamp((int)std::lround(y0 + (y1 - y0) * gy / gh), 0, kSmH - 1), yb = std::clamp((int)std::lround(y0 + (y1 - y0) * (gy + 1) / gh), ya + 1, kSmH);
      for (int gx = 0; gx < gw; gx++) {
        const int xa = std::clamp((int)std::lround(x0 + (x1 - x0) * gx / gw), 0, kSmW - 1), xb = std::clamp((int)std::lround(x0 + (x1 - x0) * (gx + 1) / gw), xa + 1, kSmW);
        const uint32_t n = (uint32_t)((yb - ya) * (xb - xa));
        for (int c = 0; c < 3; c++) {
          const int v = (int)((At(xb, yb, c) - At(xa, yb, c) - At(xb, ya, c) + At(xa, ya, c)) / n);
          if (out) (*out)[((size_t)gy * gw + gx) * 3 + c] = (uint8_t)v;
          d += (uint64_t)std::abs(v - (int)ref[((size_t)gy * gw + gx) * 3 + c]);
        }
      }
    }
    return (float)d / (float)(gw * gh * 3);
  }
};

// The crop of a photo (sm image) that a thumbnail shows: a coarse search over scale and position, then a fine one around the best.
// Returns the difference after a tone fit (0..255) and the region in 0..1 of the photo.
float FindCrop(const Sat& sat, const ThumbSm& t, float r[4]) {
  const float a = (float)t.w / (float)t.h;
  const float rwMax = std::min((float)kSmW, kSmH * a);
  const int cw = 16, ch = std::max(6, (int)std::lround(16 / a)), fw = 40, fh = std::max(12, (int)std::lround(40 / a));
  std::vector<uint8_t> refC, refF;
  SampleRgba(t.rgba.data(), t.w, t.h, (size_t)t.w * 4, 0, 0, (float)t.w, (float)t.h, cw, ch, refC);
  SampleRgba(t.rgba.data(), t.w, t.h, (size_t)t.w * 4, 0, 0, (float)t.w, (float)t.h, fw, fh, refF);
  float best = 1e9f, bx = 0, by = 0, bs = 1;
  for (float s = 1.0f; s >= 0.499f; s -= 0.05f) {
    const float rw = rwMax * s, rh = rw / a;
    for (float y = 0; y <= kSmH - rh + 0.01f; y += 3.0f)
      for (float x = 0; x <= kSmW - rw + 0.01f; x += 3.0f) {
        const float d = sat.Diff(x, y, x + rw, y + rh, cw, ch, refC);
        if (d < best) { best = d; bx = x; by = y; bs = s; }
      }
    if (kSmW - rw > 0.01f || kSmH - rh > 0.01f) {   // the far edges too
      const float d = sat.Diff(kSmW - rw, kSmH - rh, (float)kSmW, (float)kSmH, cw, ch, refC);
      if (d < best) { best = d; bx = kSmW - rw; by = kSmH - rh; bs = s; }
    }
  }
  best = 1e9f;
  float fx = bx, fy = by, fs = bs;
  for (float s = bs - 0.05f; s <= bs + 0.051f; s += 0.0125f) {
    if (s > 1.0001f || s < 0.45f) continue;
    const float rw = rwMax * s, rh = rw / a;
    for (float y = by - 3.0f; y <= by + 3.01f; y += 0.5f)
      for (float x = bx - 3.0f; x <= bx + 3.01f; x += 0.5f) {
        if (x < -0.01f || y < -0.01f || x + rw > kSmW + 0.01f || y + rh > kSmH + 0.01f) continue;
        const float d = sat.Diff(x, y, x + rw, y + rh, fw, fh, refF);
        if (d < best) { best = d; fx = x; fy = y; fs = s; }
      }
  }
  const float rw = rwMax * fs, rh = rw / a;
  std::vector<uint8_t> got;
  sat.Diff(fx, fy, fx + rw, fy + rh, fw, fh, refF, &got);
  Tone tone;
  const float fit = FitDiff(got, refF, tone);
  r[0] = std::max(0.0f, fx / kSmW); r[1] = std::max(0.0f, fy / kSmH); r[2] = std::min(1.0f, (fx + rw) / kSmW); r[3] = std::min(1.0f, (fy + rh) / kSmH);
  return fit;
}

// Worker: matches the waiting thumbnails against the recent photos; matched pairs go to the index (memory and thumbs.txt).
void PostThumbMatch() {   // g_mu held
  if (g_matchQueued || g_thumbPend.empty() || g_photoSm.empty()) return;
  g_matchQueued = true;
  Post([] {
    std::deque<ThumbSm> th; std::deque<PhotoSm> ph;
    { std::lock_guard<std::mutex> g(g_mu); g_matchQueued = false; th = g_thumbPend; ph = g_photoSm; }
    std::vector<Sat> sats(ph.size());
    for (size_t i = 0; i < ph.size(); i++) sats[i].Build(ph[i].rgb);
    for (const ThumbSm& t : th) {
      float best = 1e9f, second = 1e9f, br[4] = {}; uint32_t bp = 0;
      for (size_t i = 0; i < ph.size(); i++) {
        if (ph[i].t + 20000 < t.t || t.t + 20000 < ph[i].t) continue;   // encoded within 20 s of each other
        float r[4];
        const float d = FindCrop(sats[i], t, r);
        if (d < best) { second = best; best = d; bp = ph[i].crc; memcpy(br, r, sizeof br); }
        else if (d < second) second = d;
      }
      const bool ok = bp && RegionOk(br) && (best <= 6.0f || (best <= 12.0f && best <= 0.7f * second));
      std::lock_guard<std::mutex> g(g_mu);
      if (ok) {
        g_thumbPend.erase(std::remove_if(g_thumbPend.begin(), g_thumbPend.end(), [&](const ThumbSm& p) { return p.crc == t.crc; }), g_thumbPend.end());
        ThumbRef ref; ref.photo = bp; memcpy(ref.r, br, sizeof br);
        g_thumbIdx[t.crc] = ref;
        char line[160];
        snprintf(line, sizeof line, "%08x %08x %.5f %.5f %.5f %.5f\n", t.crc, bp, br[0], br[1], br[2], br[3]);
        CreateDirectoryA(Dir().c_str(), nullptr);
        if (FILE* f = fopen(ThumbIdxPath().c_str(), "ab")) { fputs(line, f); fflush(f); fclose(f); }
        fprintf(stderr, "[hdphoto] thumbnail %08x (%dx%d) shows photo %08x region (%.3f,%.3f)-(%.3f,%.3f) (difference %.1f after tone fit, next photo %.1f)\n",
                t.crc, t.w, t.h, bp, br[0], br[1], br[2], br[3], best, second);
      } else if (g_log) {
        fprintf(stderr, "[hdphoto] thumbnail %08x (%dx%d): no photo match yet (best %.1f, next %.1f)\n", t.crc, t.w, t.h, best, second);
      }
    }
    std::lock_guard<std::mutex> g(g_mu);   // thumbnails older than 20 s found no photo: dropped
    const ULONGLONG now = GetTickCount64();
    g_thumbPend.erase(std::remove_if(g_thumbPend.begin(), g_thumbPend.end(), [&](const ThumbSm& p) { return p.t + 20000 < now; }), g_thumbPend.end());
  });
}

// Encode hooks (g_mu held). Photo: its comparison image. Thumbnail: queued for matching unless the index knows it.
void PhotoEncodedSm(uint32_t crc, const uint8_t* rgba) {
  for (const PhotoSm& p : g_photoSm) if (p.crc == crc) return;
  PhotoSm p{crc, {}, GetTickCount64()};
  SampleRgba(rgba, 1080, 720, 1080 * 4, 0, 0, 1080, 720, kSmW, kSmH, p.rgb);
  g_photoSm.push_back(std::move(p));
  while (g_photoSm.size() > 6) g_photoSm.pop_front();
  PostThumbMatch();
}
void ThumbEncoded(uint32_t crc, const uint8_t* rgba, int w, int h) {
  LoadThumbIdx();
  if (g_thumbIdx.count(crc)) return;
  for (const ThumbSm& p : g_thumbPend) if (p.crc == crc) return;
  ThumbSm t{crc, w, h, std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4), GetTickCount64()};
  g_thumbPend.push_back(std::move(t));
  while (g_thumbPend.size() > 8) g_thumbPend.pop_front();
  if (g_log) fprintf(stderr, "[hdphoto] thumbnail encoded %dx%d crc %08x\n", w, h, crc);
  PostThumbMatch();
}

// Decode hook (g_mu held): a thumbnail JPEG decoded into a guest buffer; records the buffer when the index links it to an HD photo.
void ThumbDecoded(const uint8_t* p, uint64_t size, uint32_t crc, int w, int h) {
  bool changed = g_dec.erase(p) > 0;
  ThumbP th;
  if (w >= 32 && h >= 32 && w <= 512 && h <= 512 && size >= (uint64_t)w * h * 4) {
    LoadThumbIdx();
    auto it = g_thumbIdx.find(crc);
    if (it != g_thumbIdx.end()) {
      auto t2 = g_thumbs.find(crc);
      if (t2 != g_thumbs.end() && t2->second->w == w && t2->second->h == h) th = t2->second;
      else if (HdP hd = ByCrc(it->second.photo)) {
        th = std::make_shared<Thumb>();
        th->crc = crc; th->ref = it->second; th->photo = hd; th->w = w; th->h = h;
        if (g_thumbs.size() > 4096) g_thumbs.clear();
        g_thumbs[crc] = th;
      }
    }
  }
  if (g_log) fprintf(stderr, "[hdphoto] thumbnail decode %dx%d crc %08x into %p%s\n", w, h, crc, (const void*)p, th ? ": has an HD copy" : "");
  if (!th) { if (g_tdec.erase(p) || changed) UpdateActive(); return; }
  g_tdec[p] = {size, th};
  if (g_tdec.size() > 256) g_tdec.erase(g_tdec.begin());
  if ((uint64_t)w * h * 4 >= kHeadBytes) {
    if (g_tHead.size() > 1024) g_tHead.clear();
    g_tHead[HeadHash(p)] = th;
  }
  UpdateActive();
}

// Worker: the pyramid level of the thumbnail's HD crop for a cell `want` host pixels wide.
void PostThumb(const ThumbP& th, int want) {
  th->busy = true;
  Post([th, want] {
    std::vector<uint8_t> px, jpg;
    bool disk = false; uint32_t pcrc = 0; ThumbRef ref;
    {
      std::lock_guard<std::mutex> g(g_mu);
      const HdP& hd = th->photo;
      if (!hd->rgba.empty()) px = hd->rgba; else { jpg = hd->jpg; disk = hd->fromDisk; }
      pcrc = hd->crc; ref = th->ref;
    }
    if (px.empty()) { if (jpg.empty() && disk) ReadFile(PathOf(pcrc), jpg); if (!jpg.empty()) DecodeJpeg(jpg, px); }
    std::vector<uint8_t> out; int ow = 0, oh = 0, level = 0;
    if (px.size() == kBytes) {
      const int x0 = std::clamp((int)std::lround(ref.r[0] * kW), 0, kW - 1), y0 = std::clamp((int)std::lround(ref.r[1] * kH), 0, kH - 1);
      const int x1 = std::clamp((int)std::lround(ref.r[2] * kW), x0 + 1, kW), y1 = std::clamp((int)std::lround(ref.r[3] * kH), y0 + 1, kH);
      const int cw = x1 - x0, ch = y1 - y0;
      while (level < 6 && (cw >> (level + 1)) >= want && (ch >> (level + 1)) >= 8) level++;
      const int k = 1 << level;
      ow = cw >> level; oh = ch >> level;
      out.resize((size_t)ow * oh * 4);
      for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
          uint32_t acc[4] = {};
          for (int yy = 0; yy < k; yy++) {
            const uint8_t* q = px.data() + ((size_t)(y0 + y * k + yy) * kW + x0 + x * k) * 4;
            for (int xx = 0; xx < k; xx++) for (int c = 0; c < 4; c++) acc[c] += q[xx * 4 + c];
          }
          uint8_t* o = out.data() + ((size_t)y * ow + x) * 4;
          for (int c = 0; c < 3; c++) o[c] = (uint8_t)((acc[c] + (uint32_t)(k * k) / 2) / (uint32_t)(k * k));
          o[3] = 255;
        }
    }
    std::lock_guard<std::mutex> g(g_mu);
    th->busy = false;
    if (out.empty()) {
      th->retryAt = g_presentCount + 120;   // the session copy may still be encoding; a missing file stays missing
      if (disk) th->bad = true;
      if (g_log || disk) fprintf(stderr, "[hdphoto] grid thumbnail %08x: HD copy %08x not available%s\n", th->crc, pcrc, disk ? ": showing the game's thumbnail" : " yet");
      return;
    }
    th->px = std::move(out); th->pw = ow; th->ph = oh; th->level = level;
    if (g_log) fprintf(stderr, "[hdphoto] grid thumbnail %08x: level %d of the HD crop, %dx%d for a cell %d px wide\n", th->crc, level, ow, oh, want);
  });
}

void EvictThumbs(const ThumbP& keep) {   // g_mu held
  uint64_t bytes = 0;
  for (auto& t : g_tLive) for (auto* s : t->gpu) if (s) bytes += (uint64_t)s->b.width * s->b.height * 4;
  while (bytes > kThumbBudget && g_tLive.size() > 1) {
    auto lru = std::min_element(g_tLive.begin(), g_tLive.end(), [&](const ThumbP& a, const ThumbP& b) { return (a == keep ? UINT64_MAX : a->lastUse) < (b == keep ? UINT64_MAX : b->lastUse); });
    if (*lru == keep) break;
    for (auto*& s : (*lru)->gpu) if (s) { bytes -= (uint64_t)s->b.width * s->b.height * 4; Retire(GK_TEXTURE, s); s = nullptr; }
    g_tLive.erase(lru);
  }
}

// The grid display texture for a thumbnail texture drawn drawnW UI units wide, or nullptr (the game's texture stays).
TextureImpl* ThumbSubstitute(TextureImpl* t, float drawnW, float hostPerUnit) {
  const TextureBuilder& b = t->b;
  if (g_tActive.load(std::memory_order_relaxed) == 0 || b.width > 512 || b.height > 512 || (b.width == 360 && b.height == 240) || drawnW <= 0.0f) return nullptr;
  std::lock_guard<std::mutex> g(g_mu);
  ThumbP th;
  auto it = g_tlink.find(t);
  if (it != g_tlink.end()) th = it->second;
  else if (!g_tdec.empty() && b.pool && !b.packaged) {   // a texture whose storage is a decoded thumbnail buffer
    if (auto* pl = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (pl->storage) {
      auto d = g_tdec.find(static_cast<const uint8_t*>(pl->storage) + b.poolOffset);
      if (d != g_tdec.end()) th = d->second.th;
    }
  }
  if (!th || th->bad || b.width != th->w || b.height != th->h) return nullptr;
  th->lastUse = ++g_tick;
  const int fi = (t->fi && (t->fi->vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC1_RGB_SRGB_BLOCK || t->fi->vk == VK_FORMAT_R8G8B8A8_SRGB ||
                            t->fi->vk == VK_FORMAT_B8G8R8A8_SRGB || t->fi->vk == VK_FORMAT_BC3_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC7_SRGB_BLOCK)) ? 1 : 0;
  if (TextureImpl* s = th->gpu[fi]) {
    if (s->uploaded && s->b.packaged) { s->b.packaged = nullptr; if (!th->gpu[fi ^ 1]) { th->px.clear(); th->px.shrink_to_fit(); } }
    return s;
  }
  if (th->px.empty()) {
    if (!th->busy && g_presentCount >= th->retryAt) PostThumb(th, std::max(16, (int)std::ceil(drawnW * hostPerUnit)));
    return nullptr;
  }
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  s->b.device = b.device; s->b.target = TT_2D; s->b.width = th->pw; s->b.height = th->ph; s->b.depth = 1; s->b.levels = 1; s->b.samples = 1;
  s->b.format = fi ? FMT_RGBA8_SRGB : FMT_RGBA8;
  s->b.flags = TF_LINEAR; s->b.stride = th->pw * 4; s->b.packaged = th->px.data();
  s->shTried = true;
  if (!s->EnsureImage()) { delete s; th->bad = true; return nullptr; }
  th->gpu[fi] = s;
  if (std::find(g_tLive.begin(), g_tLive.end(), th) == g_tLive.end()) g_tLive.push_back(th);
  EvictThumbs(th);
  static int n = 0;
  if (g_log || n++ < 6) fprintf(stderr, "[hdphoto] showing HD grid thumbnail %08x (photo %08x, level %d, %dx%d) for %p %dx%d drawn %.0f wide (%.0f px) at present %d\n", th->crc,
                                th->ref.photo, th->level, th->pw, th->ph, (const void*)t, b.width, b.height, drawnW, drawnW * hostPerUnit, g_presentCount);
  return s;
}

// ---- level textures: photos drawn small (photo select's strip) ----
// Worker: the photo box-filtered to kW >> level wide, from the held pixels, the in-memory JPEG or the file.
void PostMid(const HdP& hd, int level) {
  hd->midBusy = true;
  Post([hd, level] {
    std::vector<uint8_t> px, jpg;
    bool disk = false;
    {
      std::lock_guard<std::mutex> g(g_mu);
      if (!hd->rgba.empty()) px = hd->rgba; else { jpg = hd->jpg; disk = hd->fromDisk; }
    }
    if (px.empty()) { if (jpg.empty() && disk) ReadFile(PathOf(hd->crc), jpg); if (!jpg.empty()) DecodeJpeg(jpg, px); }
    std::vector<uint8_t> out;
    const int ow = kW >> level, oh = kH >> level, k = 1 << level;
    if (px.size() == kBytes) {
      out.resize((size_t)ow * oh * 4);
      for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
          uint32_t acc[3] = {};
          for (int yy = 0; yy < k; yy++) {
            const uint8_t* q = px.data() + ((size_t)(y * k + yy) * kW + (size_t)x * k) * 4;
            for (int xx = 0; xx < k; xx++) for (int c = 0; c < 3; c++) acc[c] += q[xx * 4 + c];
          }
          uint8_t* o = out.data() + ((size_t)y * ow + x) * 4;
          for (int c = 0; c < 3; c++) o[c] = (uint8_t)((acc[c] + (uint32_t)(k * k) / 2) / (uint32_t)(k * k));
          o[3] = 255;
        }
    }
    std::lock_guard<std::mutex> g(g_mu);
    hd->midBusy = false;
    if (out.empty()) { hd->midRetry = g_presentCount + 120; return; }   // the capture may still be encoding
    hd->midPx = std::move(out); hd->midW = ow; hd->midH = oh; hd->midLevel = level;
    if (g_log) fprintf(stderr, "[hdphoto] level %d (%dx%d) of capture %llu ready\n", level, ow, oh, (unsigned long long)hd->id);
  });
}

void ReleaseMid(const HdP& hd) {   // g_mu held
  for (auto*& s : hd->mid) if (s) { Retire(GK_TEXTURE, s); s = nullptr; }
  hd->midPx.clear(); hd->midPx.shrink_to_fit(); hd->midLevel = 0;
}

void EvictMid(const HdP& keep) {   // g_mu held
  uint64_t bytes = 0;
  for (auto& h : g_midLive) for (auto* s : h->mid) if (s) bytes += (uint64_t)s->b.width * s->b.height * 4;
  while (bytes > kMidBudget && g_midLive.size() > 1) {
    auto lru = std::min_element(g_midLive.begin(), g_midLive.end(), [&](const HdP& a, const HdP& b) { return (a == keep ? UINT64_MAX : a->lastUse) < (b == keep ? UINT64_MAX : b->lastUse); });
    if (*lru == keep) break;
    for (auto* s : (*lru)->mid) if (s) bytes -= (uint64_t)s->b.width * s->b.height * 4;
    ReleaseMid(*lru);
    g_midLive.erase(lru);
  }
}

// The level texture for a photo texture drawn `want` render-target pixels wide (<= kW / 2), or nullptr (the game's texture stays:
// the level would be less than 1.2x the game's own width).
TextureImpl* MidSubstitute(TextureImpl* t, const HdP& hd, int fi, int want) {   // g_mu held
  const TextureBuilder& b = t->b;
  int level = 1;
  while (level < 3 && (kW >> (level + 1)) >= want) level++;
  if ((kW >> level) * 5 < b.width * 6) return nullptr;
  hd->lastUse = ++g_tick;
  if (hd->midLevel != level && (hd->mid[0] || hd->mid[1] || !hd->midPx.empty())) ReleaseMid(hd);   // drawn at another size: rebuild
  if (TextureImpl* s = hd->mid[fi]) {
    if (s->uploaded && s->b.packaged) { s->b.packaged = nullptr; if (!hd->mid[fi ^ 1]) { hd->midPx.clear(); hd->midPx.shrink_to_fit(); } }
    return s;
  }
  if (hd->midPx.empty() || hd->midLevel != level) {
    if (!hd->midBusy && g_presentCount >= hd->midRetry) PostMid(hd, level);
    return nullptr;
  }
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  s->b.device = b.device; s->b.target = TT_2D; s->b.width = hd->midW; s->b.height = hd->midH; s->b.depth = 1; s->b.levels = 1; s->b.samples = 1;
  s->b.format = fi ? FMT_RGBA8_SRGB : FMT_RGBA8;
  s->b.flags = TF_LINEAR; s->b.stride = hd->midW * 4; s->b.packaged = hd->midPx.data();
  s->shTried = true;
  if (!s->EnsureImage()) { delete s; hd->bad = true; return nullptr; }
  hd->mid[fi] = s;
  if (std::find(g_midLive.begin(), g_midLive.end(), hd) == g_midLive.end()) g_midLive.push_back(hd);
  EvictMid(hd);
  static int n = 0;
  if (g_log || n++ < 6) fprintf(stderr, "[hdphoto] showing level %d (%dx%d) of %s %08x for %p %dx%d drawn %d px wide at present %d\n", level, hd->midW, hd->midH, hd->crc ? "photo" : "shot", hd->crc,
                                (const void*)t, b.width, b.height, want, g_presentCount);
  return s;
}

void HostReadBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}
// Blits host region (x0,y0)-(x1,y1) of src into the 2160x1440 capture image and queues its readback (kind 4). srgbDst: the
// capture image's format encodes (linear scene -> sRGB bytes, as CapturePS); otherwise the bytes are copied as they are.
HdP Grab(ReplayCtx& cx, TextureImpl* src, int x0, int y0, int x1, int y1, bool srgbDst) {
  if (!src->subLayout.empty() || src->b.samples > 1 || src->fi->bw != 1 || !(src->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return nullptr;
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(src->host->phys, src->fi->vk, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] scene format %d cannot be blitted\n", (int)src->fi->vk); return nullptr; }
  const bool linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  TextureImpl*& cap = g_cap[srgbDst ? 1 : 0];
  if (!cap) {
    auto* s = new TextureImpl();
    s->host = src->host;
    s->b = TextureBuilder{};
    s->b.device = src->b.device; s->b.target = TT_2D; s->b.width = kW; s->b.height = kH; s->b.depth = 1; s->b.levels = 1;
    s->b.format = srgbDst ? FMT_RGBA8_SRGB : FMT_RGBA8; s->b.samples = 1;
    if (!s->EnsureImage()) { delete s; static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] capture image creation failed\n"); return nullptr; }
    s->uploaded = true;
    cap = s;
  }
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, kBytes, &buf);
  if (!map) return nullptr;
  EndRendering(cx);
  const VkImageLayout before = src->layout;
  Transition(cx.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit ib{};
  ib.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.srcOffsets[0] = {x0, y0, 0}; ib.srcOffsets[1] = {x1, y1, 1};
  ib.dstOffsets[0] = {0, 0, 0}; ib.dstOffsets[1] = {kW, kH, 1};
  vkCmdBlitImage(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  Transition(cx.cb, cap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {(uint32_t)kW, (uint32_t)kH, 1};
  vkCmdCopyImageToBuffer(cx.cb, cap->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  if (before != VK_IMAGE_LAYOUT_UNDEFINED) Transition(cx.cb, src, before);
  auto hd = std::make_shared<Hd>();
  {
    std::lock_guard<std::mutex> g(g_mu);
    hd->id = g_nextId++; hd->present = g_presentCount;
    g_pending[hd->id] = hd;
    if (g_pending.size() > 64) g_pending.erase(g_pending.begin());   // readbacks lost with a device error
  }
  Readback rb;
  rb.map = map; rb.dst = nullptr; rb.size = kBytes; rb.kind = 4; rb.poolOff = hd->id;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();
  cx.readbacks->push_back(rb);
  return hd;
}

// Full-size photo PNG: host region (x0,y0)-(x1,y1) of src blitted into an RGBA8_SRGB image (the sRGB encode, as Grab) at the
// photo's 3:2 shape and the region's full height (the game squeezes its 16:9 re-render into the 3:2 photo, so a 1:1 copy would
// look stretched: 5760x3240 -> 4860x3240), copied into its own host buffer. Recorded right after Grab, so the capture's
// readback means this copy is done too. Reads src only: the game's photo, its JPEG and everything scoring reads are untouched.
void GrabHires(ReplayCtx& cx, TextureImpl* src, int x0, int y0, int x1, int y1, const HdP& hd) {
  const uint32_t rw = (uint32_t)(x1 - x0), rh = (uint32_t)(y1 - y0);
  uint32_t w = (uint32_t)(((uint64_t)rh * kW + kH / 2) / kH), h = rh;   // 3:2 at the region's height
  if (w > rw) { w = rw; h = (uint32_t)(((uint64_t)rw * kH + kW / 2) / kW); }   // region narrower than 3:2: keep its width
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(src->host->phys, src->fi->vk, &fp);
  const VkFilter filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  if (g_hiLive.load() >= kHiLive) { fprintf(stderr, "[hdphoto] %d full-size PNGs still being written: none for capture %llu\n", kHiLive, (unsigned long long)hd->id); return; }
  static std::map<uint64_t, TextureImpl*> imgs;   // by size (one or two sizes per session); render thread
  const uint64_t key = (uint64_t)w << 32 | h;
  auto it = imgs.find(key);
  if (it == imgs.end()) {
    if (imgs.size() >= 4) { static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] too many capture sizes: no full-size PNG\n"); return; }
    auto* s = new TextureImpl();
    s->host = src->host;
    s->b = TextureBuilder{};
    s->b.device = src->b.device; s->b.target = TT_2D; s->b.width = (int)w; s->b.height = (int)h; s->b.depth = 1; s->b.levels = 1;
    s->b.format = FMT_RGBA8_SRGB; s->b.samples = 1;
    if (!s->EnsureImage()) { delete s; static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] full-size image creation failed (%ux%u)\n", w, h); return; }
    s->uploaded = true;
    it = imgs.emplace(key, s).first;
  }
  TextureImpl* img = it->second;
  auto hb = std::make_shared<HiBuf>();
  hb->dev = cx.h->device; hb->w = w; hb->h = h;
  if (!CreateHostBuffer(cx.h, (uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &hb->buf, &hb->mem, &hb->map)) {
    fprintf(stderr, "[hdphoto] full-size buffer allocation failed (%ux%u)\n", w, h);
    return;
  }
  EndRendering(cx);
  const VkImageLayout before = src->layout;
  Transition(cx.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit ib{};
  ib.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.srcOffsets[0] = {x0, y0, 0}; ib.srcOffsets[1] = {x1, y1, 1};
  ib.dstOffsets[0] = {0, 0, 0}; ib.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
  vkCmdBlitImage(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, filter);
  Transition(cx.cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cx.cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb->buf, 1, &bc);
  HostReadBarrier(cx.cb);
  if (before != VK_IMAGE_LAYOUT_UNDEFINED) Transition(cx.cb, src, before);
  std::lock_guard<std::mutex> g(g_mu);
  hd->hiOn = true;
  hd->hi = std::move(hb);
}
}  // namespace

bool HdPhotoOn() {
  static const bool on = [] {
    const char* e = snapdef::get("SNAP_HD_PHOTOS");   // env or the saved choice, else the Enhanced / Original look (hle/visual_defaults.h)
    const bool v = e && (*e == '1' || !_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes"));
    g_log = getenv("SNAP_HD_PHOTOS_LOG") != nullptr;
    if (const char* m = getenv("SNAP_HD_PHOTOS_MIN_H")) g_minH = std::min(1440, std::max(360, atoi(m)));
    if (const char* m = getenv("SNAP_HD_PHOTOS_SMALL")) g_noMid = m[0] == '0';   // 0: small photos (the strip in photo select) keep the game's texture
    if (v) fprintf(stderr, "[hdphoto] HD photos on: 2160x1440 copies in %s\n", Dir().c_str());
    return v;
  }();
  return on;
}

void HdPhotoCapture(ReplayCtx& cx, ProgramImpl* p, TextureImpl* src, TextureImpl* rt0, const float* c4, const float* uv) {
  const bool t360 = rt0->b.width == 360 && rt0->b.height == 240, t1080 = rt0->b.width == 1080 && rt0->b.height == 720;
  if (!src || !src->fi) return;
  if (!t360 && !t1080) {
    if (g_log && rt0->b.width >= 256 && rt0->b.width <= 2048) {   // diagnosis: capture passes into other targets
      const int k = CaptureProgKind(p);
      static int n = 0;
      if (k && n++ < 200) fprintf(stderr, "[hdphoto] capture kind %d into %dx%d (layers %d, target %d, host %ux%u) from %dx%d at present %d\n", k, rt0->b.width, rt0->b.height,
                                  rt0->b.depth, rt0->b.target, rt0->HW(), rt0->HH(), src->b.width, src->b.height, g_presentCount);
    }
    return;
  }
  const int kind = CaptureProgKind(p);
  if (kind != 2 && kind != 3) {
    static int n = 0;
    if (g_log && n++ < 300) fprintf(stderr, "[hdphoto] other draw into %dx%d fs=%s from %dx%d (host %ux%u f%d) at present %d\n", rt0->b.width, rt0->b.height, p->st[1].valid ? p->st[1].key : "-",
                                    src->b.width, src->b.height, src->HW(), src->HH(), (int)src->fi->vk, g_presentCount);
    return;
  }
  if (c4 && (c4[0] != 0.0f || c4[1] != 1.0f)) {   // {addValue, multiplyValue}: a faded capture; the plain blit cannot apply it
    static int n = 0; if (g_log || n++ < 4) fprintf(stderr, "[hdphoto] capture skipped: add %g multiply %g (%dx%d at present %d)\n", c4[0], c4[1], rt0->b.width, rt0->b.height, g_presentCount);
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  // The sampled region: from the draw's texture coordinates when readable (the shutter samples the centered 3:2 part of the 16:9
  // scene; the album-save re-render samples a wider region), else the centered 3:2 crop.
  int x0, y0, x1, y1;
  const float hw = (float)src->HW(), hh = (float)src->HH();
  if (uv && uv[0] >= -0.001f && uv[1] >= -0.001f && uv[2] <= 1.001f && uv[3] <= 1.001f && uv[2] - uv[0] > 0.05f && uv[3] - uv[1] > 0.05f) {
    x0 = (int)std::lround(std::max(uv[0], 0.0f) * hw); y0 = (int)std::lround(std::max(uv[1], 0.0f) * hh);
    x1 = (int)std::lround(std::min(uv[2], 1.0f) * hw); y1 = (int)std::lround(std::min(uv[3], 1.0f) * hh);
  } else {
    if (uv && g_log) fprintf(stderr, "[hdphoto] capture texture coordinates (%.3f,%.3f)-(%.3f,%.3f) unusable: centered crop\n", uv[0], uv[1], uv[2], uv[3]);
    uv = nullptr;
    const float gw = (float)src->b.width, gh = (float)src->b.height;
    float cw = gw, ch = gw * 2.0f / 3.0f;
    if (ch > gh) { ch = gh; cw = gh * 1.5f; }
    const float sx = hw / gw, sy = hh / gh;
    x0 = (int)std::lround((gw - cw) * 0.5f * sx); y0 = (int)std::lround((gh - ch) * 0.5f * sy);
    x1 = (int)std::lround((gw + cw) * 0.5f * sx); y1 = (int)std::lround((gh + ch) * 0.5f * sy);
  }
  if (y1 - y0 < g_minH) {   // region shorter than the game's own 720-pixel photo: a pure upscale; between 720 and 1440 the copy is a linear upscale of the region, still far sharper than the 360x240 thumbnail
    static int n = 0; if (g_log || n++ < 2) fprintf(stderr, "[hdphoto] capture region %dx%d of %ux%u under %d tall: no HD copy\n", x1 - x0, y1 - y0, src->HW(), src->HH(), g_minH);
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  HdP hd = Grab(cx, src, x0, y0, x1, y1, true);
  if (!hd) return;
  if (t1080 && HiresOn()) GrabHires(cx, src, x0, y0, x1, y1, hd);   // the re-render the album JPEG is made from: one per kept photo
  {
    std::lock_guard<std::mutex> g(g_mu);
    // The target shows this capture: the shutter's 360x240 thumbnail, or the 1080x720 photo the game re-renders for the score
    // screen and the album save (its JPEG matches this capture at 0.4/255 when the region comes from the draw's texture coordinates).
    SetLink(rt0, hd);
  }
  static int n = 0;
  if (g_log || n++ < 3)
    fprintf(stderr, "[hdphoto] capture %llu: %ux%u scene (%ux%u guest) region (%d,%d)-(%d,%d)%s -> 2160x1440 for the %dx%d %s %p at present %d\n", (unsigned long long)hd->id, src->HW(), src->HH(),
            src->b.width, src->b.height, x0, y0, x1, y1, uv ? " from the draw's texture coordinates" : "", rt0->b.width, rt0->b.height, t360 ? "thumbnail" : "photo", (const void*)rt0, g_presentCount);
}

// CopyTextureToTexture into a 1080x720 texture from a larger one (guest regions): a match candidate made the same way.
void HdPhotoCopyRegion(ReplayCtx& cx, TextureImpl* s, TextureImpl* d, int sx, int sy, int sw, int sh) {
  if (d->b.width != 1080 || d->b.height != 720 || !s->fi || s->b.width < 1080 || sw <= 0 || sh <= 0) return;
  const float kx = (float)s->HW() / (float)s->b.width, ky = (float)s->HH() / (float)s->b.height;
  const int x0 = (int)std::lround(sx * kx), y0 = (int)std::lround(sy * ky), x1 = (int)std::lround((sx + sw) * kx), y1 = (int)std::lround((sy + sh) * ky);
  if (g_log) fprintf(stderr, "[hdphoto] copy %p %dx%d (host %ux%u f%d) region (%d,%d %dx%d) -> 1080x720 %p at present %d\n", (const void*)s, s->b.width, s->b.height, s->HW(), s->HH(),
                     (int)s->fi->vk, sx, sy, sw, sh, (const void*)d, g_presentCount);
  if (y1 - y0 < g_minH) return;
  const VkFormat f = s->fi->vk;
  const bool srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
  const bool unorm8 = f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  if (!srgb && !unorm8) return;   // a float scene copy would need the sRGB encode; only display-encoded sources are taken
  if (HdP hd = Grab(cx, s, x0, y0, x1, y1, srgb))
    if (g_log) fprintf(stderr, "[hdphoto] capture %llu from the copy\n", (unsigned long long)hd->id);
}

// The readback buffer is mapped GPU memory of a write-combined type: a plain read of it gets about 200 MB/s, so copying the 12 MB capture stalled
// the game thread for 60 to 80 ms on every shot (it runs inside the guest's sync wait). Streaming loads read write-combined memory at full speed
// and are ordinary loads on cached memory. SNAP_HDPHOTO_PLAIN_COPY=1 restores the plain copy.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("sse4.1")))
#endif
static void CopyFromMapped(uint8_t* dst, const uint8_t* src, size_t n) {
  static const bool plain = [] { const char* e = getenv("SNAP_HDPHOTO_PLAIN_COPY"); return e && *e && *e != '0'; }();
  size_t i = 0;
  if (!plain && ((uintptr_t)src & 15) == 0) {
    for (; i + 64 <= n; i += 64) {
      const __m128i a = _mm_stream_load_si128((__m128i*)(src + i)), b = _mm_stream_load_si128((__m128i*)(src + i + 16));
      const __m128i c = _mm_stream_load_si128((__m128i*)(src + i + 32)), d = _mm_stream_load_si128((__m128i*)(src + i + 48));
      _mm_storeu_si128((__m128i*)(dst + i), a); _mm_storeu_si128((__m128i*)(dst + i + 16), b);
      _mm_storeu_si128((__m128i*)(dst + i + 32), c); _mm_storeu_si128((__m128i*)(dst + i + 48), d);
    }
  }
  if (i < n) memcpy(dst + i, src + i, n - i);
}

void HdPhotoReadback(uint64_t id, const uint8_t* map, uint64_t size) {
  std::lock_guard<std::mutex> g(g_mu);
  auto it = g_pending.find(id);
  if (it == g_pending.end()) return;
  HdP hd = it->second;
  g_pending.erase(it);
  if (size != kBytes) return;
  if (hd->hi) PostHires(hd);
  const auto t0 = std::chrono::steady_clock::now();
  hd->rgba.resize((size_t)size);
  CopyFromMapped(hd->rgba.data(), map, (size_t)size);
  const auto t1 = std::chrono::steady_clock::now();
  Mini(hd->rgba.data(), kW, kH, (size_t)kW * 4, hd->mini);
  { static int n; if (n++ < 3 || g_log) fprintf(stderr, "[hdphoto] capture %llu read back: copy %.1f ms, mini %.1f ms\n", (unsigned long long)id, std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count()); }
  g_recent.push_back(hd);
  if (g_recent.size() > 160) g_recent.pop_front();   // more than one expedition of shots
  PostEncode(hd);
  for (auto w = g_waiting.begin(); w != g_waiting.end();) {   // photos encoded before this readback arrived
    if (MatchPhoto(*w, g_pending.empty()) || g_pending.empty()) w = g_waiting.erase(w);
    else ++w;
  }
}

void HdPhotoCompress(const TextureImpl* bc, const TextureImpl* src) {
  std::lock_guard<std::mutex> g(g_mu);
  HdP hd;
  if (src) { auto it = g_link.find(src); if (it != g_link.end()) hd = it->second; }
  if (!hd && !g_link.count(bc)) return;
  SetLink(bc, hd);
  if (g_log && hd) fprintf(stderr, "[hdphoto] thumbnail %p (BC %dx%d) shows capture %llu\n", (const void*)bc, bc->b.width, bc->b.height, (unsigned long long)hd->id);
}

void HdPhotoCopyTex(const TextureImpl* s, const TextureImpl* d) {
  std::lock_guard<std::mutex> g(g_mu);
  if (!g_tlink.empty()) {   // grid thumbnails: a copy shows what its source shows
    auto t = g_tlink.find(s);
    if (t != g_tlink.end() && d->b.width == t->second->w && d->b.height == t->second->h) g_tlink[d] = t->second;
    else g_tlink.erase(d);
    UpdateActive();
  }
  auto it = g_link.find(s);
  if (it == g_link.end()) { if (g_link.count(d)) SetLink(d, nullptr); return; }
  SetLink(d, it->second);
  if (g_log) fprintf(stderr, "[hdphoto] copy %p -> %p (%dx%d) keeps capture %llu\n", (const void*)s, (const void*)d, d->b.width, d->b.height, (unsigned long long)it->second->id);
}

void HdPhotoCopyToBuffer(const TextureImpl* t, const uint8_t* dst) {   // log only: the encoder reads another buffer (content match below)
  if (g_log && t->b.width == 1080 && t->b.height == 720) fprintf(stderr, "[hdphoto] photo copy %p -> buffer %p at present %d\n", (const void*)t, (const void*)dst, g_presentCount);
}

void HdPhotoUploadFrom(const TextureImpl* t, const uint8_t* src) {
  std::lock_guard<std::mutex> g(g_mu);
  if (t->b.width <= 512 && t->b.height <= 512 && (!g_tdec.empty() || !g_tHead.empty() || !g_tlink.empty())) {   // grid thumbnails
    ThumbP th;
    if (src) { auto it = g_tdec.find(src); if (it != g_tdec.end()) th = it->second.th; }
    if (!th && src && !g_tHead.empty() && t->fi && t->fi->bytes == 4 && t->fi->bw == 1 && (uint64_t)t->b.width * t->b.height * 4 >= kHeadBytes) {
      auto it = g_tHead.find(HeadHash(src));
      if (it != g_tHead.end()) th = it->second;
    }
    if (th && (th->w != t->b.width || th->h != t->b.height)) th = nullptr;
    if (th) { g_tlink[t] = th; UpdateActive(); if (g_log) fprintf(stderr, "[hdphoto] texture %p (%dx%d) uploaded from decoded thumbnail %08x\n", (const void*)t, t->b.width, t->b.height, th->crc); }
    else if (g_tlink.erase(t)) UpdateActive();
  }
  HdP hd;
  if (src) { auto it = g_dec.find(src); if (it != g_dec.end()) hd = it->second.hd; }
  if (!hd && src && t->b.width == 1080 && t->b.height == 720 && !g_decHead.empty()) {   // a copy of a decoded photo's pixels
    auto it = g_decHead.find(HeadHash(src));
    if (it != g_decHead.end()) hd = it->second;
  }
  if (!hd && g_log && t->b.width == 1080 && t->b.height == 720 && !g_dec.empty()) {
    static int n = 0;
    if (n++ < 40) fprintf(stderr, "[hdphoto] 1080x720 texture %p uploaded from %p (no decoded photo there) at present %d\n", (const void*)t, (const void*)src, g_presentCount);
  }
  if (!hd && !g_link.count(t)) return;
  SetLink(t, hd);
  if (g_log && hd) fprintf(stderr, "[hdphoto] texture %p (%dx%d) uploaded from decoded photo %08x\n", (const void*)t, t->b.width, t->b.height, hd->crc);
}

void HdPhotoTexGone(const TextureImpl* t) {
  std::lock_guard<std::mutex> g(g_mu);
  const bool a = g_link.erase(t) > 0, b = g_tlink.erase(t) > 0;
  if (a || b) UpdateActive();
}

TextureImpl* HdPhotoSubstitute(TextureImpl* t, float drawnW, float hostPerUnit) {
  if (g_active.load(std::memory_order_relaxed) == 0) return nullptr;
  const TextureBuilder& b = t->b;
  if (b.width <= 512 && b.height <= 512 && !(b.width == 360 && b.height == 240)) return ThumbSubstitute(t, drawnW, hostPerUnit);
  if (!((b.width == 360 && b.height == 240) || (b.width == 1080 && b.height == 720))) return nullptr;
  std::lock_guard<std::mutex> g(g_mu);
  HdP hd;
  auto it = g_link.find(t);
  if (it != g_link.end()) hd = it->second;
  else if (!g_dec.empty() && b.pool && !b.packaged) {   // a texture whose storage is a decoded album photo buffer
    if (auto* pl = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (pl->storage) {
      auto d = g_dec.find(static_cast<const uint8_t*>(pl->storage) + b.poolOffset);
      if (d != g_dec.end()) hd = d->second.hd;
    }
  }
  if (!hd && g_log && drawnW >= kMinDrawW && b.width == 1080) {   // diagnosis: a large photo without an HD link
    static std::unordered_set<const TextureImpl*> seen;
    if (seen.size() < 64 && seen.insert(t).second) {
      const uint8_t* addr = nullptr;
      if (b.pool) if (auto* pl = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (pl->storage) addr = static_cast<const uint8_t*>(pl->storage) + b.poolOffset;
      long long best = -1; const uint8_t* bk = nullptr;
      for (auto& [k, v] : g_dec) { const long long dd = std::llabs((long long)(k - addr)); if (addr && (best < 0 || dd < best)) { best = dd; bk = k; } }
      fprintf(stderr, "[hdphoto] large photo %p 1080x720 (fmt %d flags %x stride %d packaged %d) at %p without HD; nearest decoded buffer %p (%lld bytes away) at present %d\n",
              (const void*)t, (int)b.format, b.flags, b.stride, b.packaged != nullptr, (const void*)addr, (const void*)bk, best, g_presentCount);
    }
  }
  if (!hd || hd->bad) return nullptr;
  const int fi = (t->fi && (t->fi->vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC1_RGB_SRGB_BLOCK || t->fi->vk == VK_FORMAT_R8G8B8A8_SRGB ||
                            t->fi->vk == VK_FORMAT_B8G8R8A8_SRGB || t->fi->vk == VK_FORMAT_BC3_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC7_SRGB_BLOCK)) ? 1 : 0;
  if (drawnW < kMinDrawW) {   // a small photo (the strip in photo select): a level sized to the draw, when it beats the game's texture
    const int want = (drawnW > 0.0f && hostPerUnit > 0.0f) ? (int)std::ceil(drawnW * hostPerUnit) : 0;
    return (want > 0 && want <= kW / 2 && !g_noMid) ? MidSubstitute(t, hd, fi, want) : nullptr;
  }
  hd->lastUse = ++g_tick;
  if (TextureImpl* s = hd->gpu[fi]) {
    if (s->uploaded && s->b.packaged) {   // uploaded: the pixels are no longer needed
      s->b.packaged = nullptr;
      if (!hd->busy && !hd->gpu[fi ^ 1] && !hd->jpg.empty()) { hd->rgba.clear(); hd->rgba.shrink_to_fit(); }
    }
    return s;
  }
  if (hd->rgba.empty()) { if (!hd->busy) PostLoad(hd); return nullptr; }
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  s->b.device = b.device; s->b.target = TT_2D; s->b.width = kW; s->b.height = kH; s->b.depth = 1; s->b.levels = 1; s->b.samples = 1;
  s->b.format = fi ? FMT_RGBA8_SRGB : FMT_RGBA8;
  s->b.flags = TF_LINEAR; s->b.stride = kW * 4; s->b.packaged = hd->rgba.data();
  s->shTried = true;
  if (!s->EnsureImage()) { delete s; hd->bad = true; return nullptr; }
  hd->gpu[fi] = s;
  if (std::find(g_gpuLive.begin(), g_gpuLive.end(), hd) == g_gpuLive.end()) g_gpuLive.push_back(hd);
  EvictLocked(hd);
  static int n = 0;
  if (g_log || n++ < 6) fprintf(stderr, "[hdphoto] showing HD copy of %s %08x (capture %llu) for %p %dx%d drawn %.0f wide at present %d\n", hd->crc ? "photo" : "shot", hd->crc,
                                (unsigned long long)hd->id, (const void*)t, b.width, b.height, drawnW, g_presentCount);
  return s;
}
}  // namespace nvn

// host/mods/photoexport.cpp: JpegEncoder::SetPixelData / Encode (every call; returns at once when HD photos are off).
extern "C" void snap_hdphoto_pixels(uint64_t enc, uint64_t data, const int* dim) {
  if (!nvn::HdPhotoOn()) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  nvn::g_encBuf[enc] = {reinterpret_cast<const uint8_t*>(data), dim ? dim[0] : 0, dim ? dim[1] : 0};
}
// A successful Encode of a 1080x720 photo: the session capture whose picture it is (closest box average, under kMatchMax) gets
// the JPEG's CRC32 as its file name. Shots match their shutter capture; the submit re-render is a candidate too.
extern "C" void snap_hdphoto_encoded(uint64_t enc, const uint8_t* jpg, size_t n) {
  if (!nvn::HdPhotoOn() || !jpg || n < 4 || n > (64u << 20)) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  auto e = nvn::g_encBuf.find(enc);
  if (e == nvn::g_encBuf.end() || !e->second.data) return;
  const int ew = e->second.w, eh = e->second.h;
  if (!(ew == 1080 && eh == 720)) {   // an album thumbnail: matched to its photo for the grid (read only, like the photo)
    if (ew >= 32 && eh >= 32 && ew <= 512 && eh <= 512) nvn::ThumbEncoded(nvn::Crc32(jpg, n), e->second.data, ew, eh);
    return;
  }
  const uint32_t crc = nvn::Crc32(jpg, n);
  nvn::PhotoEncodedSm(crc, e->second.data);
  if (nvn::g_byCrc.count(crc)) return;   // the same photo encoded again
  nvn::Waiting w{crc, n, {}};
  nvn::Mini(e->second.data, 1080, 720, 1080 * 4, w.mini);
  if (nvn::MatchPhoto(w, nvn::g_pending.empty()) || nvn::g_pending.empty()) return;
  // The game often encodes right after its own capture draw, before that capture's readback arrives: retry on each readback.
  nvn::g_waiting.push_back(std::move(w));
  if (nvn::g_waiting.size() > 8) nvn::g_waiting.pop_front();
  if (nvn::g_log) fprintf(stderr, "[hdphoto] album photo %08x: waiting for %zu capture readbacks\n", crc, nvn::g_pending.size());
}
// host/mods/shiny.cpp: a photo JPEG decoded into a guest buffer (the album). Records the buffer for the texture made from it.
extern "C" void snap_hdphoto_decoded(uint64_t dst, uint64_t size, uint32_t crc, int w, int h) {
  if (!nvn::HdPhotoOn() || !dst) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(dst);
  if (!(w == 1080 && h == 720)) { nvn::ThumbDecoded(p, size, crc, w, h); return; }   // album grid thumbnails
  if (nvn::g_tdec.erase(p)) nvn::UpdateActive();
  nvn::HdP hd = nvn::ByCrc(crc);
  if (nvn::g_log) fprintf(stderr, "[hdphoto] decode %dx%d crc %08x into %p (%llu bytes)%s\n", w, h, crc, (const void*)p, (unsigned long long)size, hd ? ": has an HD copy" : "");
  if (!hd) { if (nvn::g_dec.erase(p)) nvn::UpdateActive(); return; }
  nvn::g_dec[p] = {size, crc, hd};
  if (nvn::g_dec.size() > 64) nvn::g_dec.erase(nvn::g_dec.begin());
  if (size >= nvn::kHeadBytes) {
    if (nvn::g_decHead.size() > 256) nvn::g_decHead.clear();
    nvn::g_decHead[nvn::HeadHash(p)] = hd;
  }
  nvn::UpdateActive();
  if (nvn::g_log) fprintf(stderr, "[hdphoto] decoded photo %08x into %p (%llu bytes): HD copy available\n", crc, (const void*)p, (unsigned long long)size);
}
extern "C" int snap_hdphoto_on() { return nvn::HdPhotoOn() ? 1 : 0; }
