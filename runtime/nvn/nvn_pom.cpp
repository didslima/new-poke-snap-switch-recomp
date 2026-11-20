// SPDX-License-Identifier: MIT
// Parallax occlusion mapping (POM) with self-shadowing for ground, rock and bark (docs/pom.md). Off unless SNAP_POM=low|high.
//
// What it does: the game's environment pixel shaders sample baseColor / normal / params with one UV pair. For a draw whose baseColor texture is in
// pom_table.txt (ground, rock or bark, built locally from the game's own textures by scripts/hd/make_pom_table.py, never committed), the fragment SPIR-V is
// patched right after that UV pair is first stored: a steep-parallax ray march through a height field derived from the baseColor texture itself (luminance
// against its own coarse-mip average, so no extra texture and no extra binding) gives a shifted UV, which is stored back into the same two variables. Every
// later fetch (baseColor, normal, params) therefore reads the shifted texel. A second short march from the hit point toward the sun gives a self-shadow
// that darkens the base color sample. Nothing about depth is changed (no FragDepth write, no discard): silhouettes, collision, the depth buffer and the
// photo evaluation are the same as without it; only color changes.
// The view ray needs no tangent frame from the vertex shader: the world position of the pixel is rebuilt from FragCoord and the depth (the camera's inverse
// view-projection, the same math as contact.frag), and the UV gradients come from screen derivatives (Schueler's cotangent frame, no vertex data).
// Fragment push constants (offset 128, 128 bytes): m0..m3 inverse view-projection columns, m4 eye, m5 sun (xyz toward the light, w shadow strength),
// m6 (render size, jitter), m7 (depth scale, fade start, fade end, sign).
// Switches: SNAP_POM=off|low|high  SNAP_POM_DEPTH (fraction of a texture tile, default 0.015)  SNAP_POM_FADE=<start>,<end> world units (default 14,40)
//           SNAP_POM_SHADOW (self-shadow strength 0..1, default 0.6)  SNAP_POM_FLIP=1 (flip the parallax direction)  SNAP_POM_LOG=1  SNAP_POM_TABLE=<file>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "hle/visual_defaults.h"
#include "hle/live.h"   // the Parallax detail row applies with no restart
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_os_host.h"
#include "nvn_pom.h"

namespace nvn {
namespace {
enum : uint32_t { kOpEntryPoint = 15, kOpExtInstImport = 11, kOpExtInst = 12, kOpMemoryModel = 14, kOpName = 5, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23,
                  kOpTypeArray = 28, kOpTypeStruct = 30, kOpTypePointer = 32, kOpConstant = 43, kOpConstantComposite = 44, kOpFunction = 54, kOpVariable = 59, kOpLoad = 61,
                  kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71, kOpMemberDecorate = 72, kOpVectorShuffle = 79, kOpCompositeConstruct = 80, kOpCompositeExtract = 81,
                  kOpSampledImageT = 27, kOpImageSampleImplicitLod = 87, kOpImageSampleExplicitLod = 88, kOpFNegate = 127, kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133,
                  kOpFDiv = 136, kOpVectorTimesScalar = 142, kOpDot = 148, kOpLogicalOr = 166, kOpLogicalNot = 168, kOpSelect = 169, kOpFOrdLessThanEqual = 188, kOpFOrdGreaterThanEqual = 190, kOpLogicalAnd = 167, kOpConstantFalse = 42,
                  kOpFOrdLessThan = 184, kOpDPdx = 207, kOpDPdy = 208, kOpLabel = 248, kOpGroupDecorateEnd = 76 };
// GLSL.std.450
enum : uint32_t { gFAbs = 4, gFloor = 8, gSqrt = 31, gInvSqrt = 32, gFMin = 37, gFMax = 40, gFClamp = 43, gFMix = 46, gSmoothStep = 49, gLength = 66, gCross = 68, gNormalize = 69 };

void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float EnvF(const char* n, float d) { const char* e = getenv(n); return (e && *e) ? (float)atof(e) : d; }
bool LogOn() { static const bool on = getenv("SNAP_POM_LOG") != nullptr; return on; }

// ---- table ------------------------------------------------------------------------------------------------------------------------------------
uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
std::unordered_map<uint64_t, uint8_t>& Table() {
  static std::unordered_map<uint64_t, uint8_t> t;
  static bool loaded = false;
  if (loaded) return t;
  loaded = true;
  std::vector<std::string> paths;
  if (const char* e = getenv("SNAP_POM_TABLE"); e && *e) paths.push_back(e);
  paths.push_back("build\\pom\\pom_table.txt");
  paths.push_back("pom_table.txt");
  if (const char* a = getenv("APPDATA")) paths.push_back(std::string(a) + "\\SnapRecomp\\mods\\pom\\pom_table.txt");
  for (auto& p : paths) {
    FILE* f = fopen(p.c_str(), "r");
    if (!f) continue;
    char line[512]; int n = 0;
    while (fgets(line, sizeof line, f)) {
      unsigned long long k; int c;
      if (sscanf(line, "%llx %d", &k, &c) == 2 && c >= 1 && c <= 3) { t[(uint64_t)k] = (uint8_t)c; n++; }
    }
    fclose(f);
    fprintf(stderr, "[pom] table %s: %d textures\n", p.c_str(), n);
    break;
  }
  if (t.empty()) fprintf(stderr, "[pom] no pom_table.txt found (SNAP_POM_TABLE, build\\pom\\pom_table.txt): parallax has no textures to act on\n");
  return t;
}

int ParseLevel(const char* s) {
  if (!s || !*s) return 0;
  if (nvn::os_host::ascii_ieq(s, "low") || !strcmp(s, "1")) return 1;
  if (nvn::os_host::ascii_ieq(s, "high") || !strcmp(s, "2") || nvn::os_host::ascii_ieq(s, "on")) return 2;
  return 0;
}

// ---- push constants / state -----------------------------------------------------------------------------------------------------------------
static uint32_t g_layoutRange = 0;
struct State { PomPush pc{}; int present = -1; bool ok = false; } S;

// ---- the SPIR-V patch -----------------------------------------------------------------------------------------------------------------------------
struct Gen {
  std::vector<uint32_t>& o;
  uint32_t& next;
  uint32_t T, V2, V3, V4, Bool, U32, glsl;
  uint32_t N() { return next++; }
  uint32_t Op(uint32_t op, uint32_t type, std::initializer_list<uint32_t> w) {
    const uint32_t r = next++;
    o.push_back(((uint32_t)(w.size() + 3) << 16) | op); o.push_back(type); o.push_back(r);
    o.insert(o.end(), w.begin(), w.end());
    return r;
  }
  uint32_t Ext(uint32_t type, uint32_t inst, std::initializer_list<uint32_t> a) {
    const uint32_t r = next++;
    o.push_back(((uint32_t)(a.size() + 5) << 16) | kOpExtInst); o.push_back(type); o.push_back(r); o.push_back(glsl); o.push_back(inst);
    o.insert(o.end(), a.begin(), a.end());
    return r;
  }
  uint32_t Add(uint32_t t, uint32_t a, uint32_t b) { return Op(kOpFAdd, t, {a, b}); }
  uint32_t Sub(uint32_t t, uint32_t a, uint32_t b) { return Op(kOpFSub, t, {a, b}); }
  uint32_t Mul(uint32_t t, uint32_t a, uint32_t b) { return Op(kOpFMul, t, {a, b}); }
  uint32_t Div(uint32_t a, uint32_t b) { return Op(kOpFDiv, T, {a, b}); }
  uint32_t Scale(uint32_t vt, uint32_t v, uint32_t s) { return Op(kOpVectorTimesScalar, vt, {v, s}); }
  uint32_t Dot(uint32_t a, uint32_t b) { return Op(kOpDot, T, {a, b}); }
  uint32_t Ex(uint32_t v, uint32_t i) { return Op(kOpCompositeExtract, T, {v, i}); }
  uint32_t Sel(uint32_t t, uint32_t c, uint32_t a, uint32_t b) { return Op(kOpSelect, t, {c, a, b}); }
  uint32_t Max(uint32_t a, uint32_t b) { return Ext(T, gFMax, {a, b}); }
  uint32_t Min(uint32_t a, uint32_t b) { return Ext(T, gFMin, {a, b}); }
  uint32_t Clamp(uint32_t a, uint32_t lo, uint32_t hi) { return Ext(T, gFClamp, {a, lo, hi}); }
};

}  // namespace

int PomLevel() {
  static int cached = 0, at = -999;
  if (at == g_presentCount) return cached;
  at = g_presentCount;
  { hle::live::EnvRead env_read; cached = ParseLevel(snapdef::get("SNAP_POM")); }   // once per present: the row is live
  return cached;
}

void PomNoteTexture(TextureImpl* t) {
  if (!t || t->pomTried || t->uploaded) return;   // classified whatever the level is now, so the F1 row can switch it on without a reload
  t->pomTried = true;
  const TextureBuilder& b = t->b;
  const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
  if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR)))
    if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
  if (LogOn()) { static int r = 0; if (r++ < 12) fprintf(stderr, "[pom] saw tex %dx%d src=%p packaged=%p pool=%p scale=%g samples=%d target=%d flags=%x\n", b.width, b.height, (const void*)src, (const void*)b.packaged, (void*)b.pool, (double)t->scale, (int)b.samples, (int)b.target, (unsigned)b.flags); }
  if (!src || t->scale != 1.0f || b.samples > 1 || b.target != TT_2D || b.width < 64 || b.height < 64) return;
  const uint64_t key = Fnv(src, 4096);
  if (LogOn()) { static int h = 0; if (h++ < 30) fprintf(stderr, "[pom] hashed %dx%d key %016" PRIx64 "\n", b.width, b.height, key); }
  auto& tab = Table();
  auto it = tab.find(key);
  if (it != tab.end()) {
    t->pomCls = it->second;
    if (LogOn()) { static int n = 0; if (n++ < 200) fprintf(stderr, "[pom] texture %p %dx%d key %016" PRIx64 " class %d\n", (void*)t, b.width, b.height, key, (int)it->second); }
  }
}

bool PomFillPush(PomPush* out) {
  if (S.present == g_presentCount && S.ok) { *out = S.pc; return true; }
  S.ok = false;
  const FxCam* cam = FxCamera();
  TextureImpl* z = FxSceneDepth();
  if (!cam || !z) return false;
  PomPush p{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) p.m[k][r] = (float)cam->ivp[r][k];   // column k of the inverse view-projection
  for (int i = 0; i < 3; i++) p.m[4][i] = (float)cam->eye[i];
  float L[3] = {0.35f, 0.82f, 0.45f}; float strength = 0.35f;   // no dynamic shadow pass this frame (static-lit course): a fixed high sun, half strength
  if (const FxCaster* cs = FxCasterNow()) {
    float sgn = cs->axis[1] > 0.f ? 1.f : -1.f;   // the caster matrix' depth axis points away from the light or toward it: take the sign that points up (as contact shadows)
    for (int i = 0; i < 3; i++) L[i] = cs->axis[i] * sgn;
    strength = 1.0f;
  }
  const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
  for (int i = 0; i < 3; i++) p.m[5][i] = L[i] / std::max(len, 1e-6f);
  p.m[5][3] = EnvF("SNAP_POM_SHADOW", 0.6f) * strength;
  float jx = 0, jy = 0; FxJitter(&jx, &jy);
  p.m[6][0] = (float)z->HW(); p.m[6][1] = (float)z->HH(); p.m[6][2] = jx; p.m[6][3] = jy;
  float f0 = 14.f, f1 = 40.f;
  if (const char* e = getenv("SNAP_POM_FADE")) sscanf(e, "%f,%f", &f0, &f1);
  p.m[7][0] = EnvF("SNAP_POM_DEPTH", 0.015f); p.m[7][1] = f0; p.m[7][2] = std::max(f1, f0 + 0.01f); p.m[7][3] = getenv("SNAP_POM_FLIP") ? -1.f : 1.f;
  S.pc = p; S.ok = true; S.present = g_presentCount; *out = p;
  return true;
}

uint32_t PomPushRangeBytes(uint32_t deviceLimit) { g_layoutRange = deviceLimit >= 256 ? 256u : 0u; return g_layoutRange; }

uint32_t PomPipeBits(TextureImpl* rt0, int level, int handleIndex, uint8_t texClass) {
  if (level <= 0 || !texClass || handleIndex < 0 || handleIndex > 127 || g_layoutRange < 256) return 0;
  TextureImpl* color = FxSceneColor();
  if (LogOn()) { static int n = 0; if (n++ < 20) fprintf(stderr, "[pom] pipebits level %d handle %d cls %d rt0 %p color %p photo %d\n", level, handleIndex, (int)texClass, (void*)rt0, (void*)color, (int)FxIsPhotoFrame()); }
  if (!color || rt0 != color || FxIsPhotoFrame()) return 0;   // only the scene's main color pass (not thumbnails, evaluation or secondary passes)
  PomPush tmp;
  if (!PomFillPush(&tmp)) return 0;
  return ((uint32_t)level << 8) | ((uint32_t)handleIndex << 10);
}

static bool PomFail(int line) { if (LogOn()) fprintf(stderr, "[pom] patch failed at line %d\n", line); return false; }
bool PatchPom(const std::vector<uint32_t>& in, int handleIndex, int level, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203 || level < 1) return PomFail(__LINE__);
  char want[32]; snprintf(want, sizeof want, "fp_t_tcb_%X", (unsigned)handleIndex);
  std::map<uint32_t, std::string> nameOf;
  std::map<uint32_t, uint32_t> kindOfVar;                       // variable id -> storage class
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecType;    // (component type, count) -> id
  std::map<uint32_t, uint32_t> varPtrType, ptrPointee;
  uint32_t T = 0, Bool = 0, U32 = 0, glsl = 0, fragVar = 0, fragPtrType = 0, entryIdx = 0, memModelAt = 0, capEnd = 0, firstFn = 0, lastDecEnd = 0;
  std::map<uint32_t, uint32_t> ptrOf;                           // (storage class << 8 ^ pointee) -> id (key packed below)
  std::map<uint64_t, uint32_t> ptrKey;
  bool hasPush = false;
  std::map<uint32_t, uint32_t> builtin;                         // var id -> builtin
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return PomFail(__LINE__);
    if (op == 17) capEnd = (uint32_t)(i + wc);
    else if (op == kOpEntryPoint && !entryIdx) entryIdx = (uint32_t)i;
    else if (op == kOpExtInstImport && wc >= 3 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glsl = in[i + 1];
    else if (op == kOpMemoryModel) memModelAt = (uint32_t)i;
    else if (op == kOpName && wc >= 3) nameOf[in[i + 1]] = std::string((const char*)&in[i + 2], strnlen((const char*)&in[i + 2], (wc - 2) * 4));
    else if (op == kOpDecorate && wc >= 4 && in[i + 2] == 11) builtin[in[i + 1]] = in[i + 3];
    if (op >= kOpDecorate && op <= kOpGroupDecorateEnd) lastDecEnd = (uint32_t)(i + wc);
    if (op == kOpTypeFloat && wc >= 3 && in[i + 2] == 32 && !T) T = in[i + 1];
    else if (op == kOpTypeBool) Bool = in[i + 1];
    else if (op == kOpTypeInt && wc >= 4 && in[i + 2] == 32 && in[i + 3] == 0) U32 = in[i + 1];
    else if (op == kOpTypeVector && wc >= 4) vecType[{in[i + 2], in[i + 3]}] = in[i + 1];
    else if (op == kOpTypePointer && wc >= 4) { ptrPointee[in[i + 1]] = in[i + 3]; ptrKey[((uint64_t)in[i + 2] << 32) | in[i + 3]] = in[i + 1]; }
    else if (op == kOpVariable && !firstFn && wc >= 4) { kindOfVar[in[i + 2]] = in[i + 3]; varPtrType[in[i + 2]] = in[i + 1]; if (in[i + 3] == 9) hasPush = true; }
    if (op == kOpFunction && !firstFn) firstFn = (uint32_t)i;
    i += wc;
  }
  if (!T || !firstFn || !entryIdx || !memModelAt || hasPush) return PomFail(__LINE__);   // (a shader that already has a push block, e.g. the RT receivers, is left alone)
  if (!vecType.count({T, 2}) || !vecType.count({T, 3}) || !vecType.count({T, 4})) return PomFail(__LINE__);
  uint32_t samplerVar = 0;
  for (auto& n : nameOf) if (n.second == want && kindOfVar.count(n.first) && kindOfVar[n.first] == 0) samplerVar = n.first;
  if (!samplerVar) return PomFail(__LINE__);
  for (auto& b : builtin) if (b.second == 15 && kindOfVar.count(b.first) && kindOfVar[b.first] == 1) fragVar = b.first;

  // function body scan: definitions, first block, the first sample of the sampler and the stores of its UV pair
  struct Ins { size_t at; uint32_t wc, op; };
  std::vector<Ins> body;
  std::map<uint32_t, size_t> defIdx;           // result id -> index in body
  for (size_t i = firstFn; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc) return PomFail(__LINE__);
    body.push_back({i, wc, op});
    i += wc;
  }
  auto resOf = [&](const Ins& s) -> uint32_t {   // result id for ops with (type, result) leading words
    switch (s.op) { case kOpLoad: case kOpAccessChain: case kOpCompositeConstruct: case kOpCompositeExtract: case kOpSelect: case kOpImageSampleImplicitLod:
                    case kOpImageSampleExplicitLod: case kOpVariable: return in[s.at + 2]; default: return 0; }
  };
  for (size_t k = 0; k < body.size(); k++) if (uint32_t r = resOf(body[k])) defIdx[r] = k;
  // loads of the sampler variable
  std::map<uint32_t, bool> imgLoad;
  for (auto& s : body) if (s.op == kOpLoad && in[s.at + 3] == samplerVar) imgLoad[in[s.at + 2]] = true;
  if (imgLoad.empty()) return PomFail(__LINE__);
  size_t firstSample = SIZE_MAX;
  for (size_t k = 0; k < body.size(); k++) if (body[k].op == kOpImageSampleImplicitLod && imgLoad.count(in[body[k].at + 3])) { firstSample = k; break; }
  if (firstSample == SIZE_MAX) return PomFail(__LINE__);
  uint32_t coord = in[body[firstSample].at + 4];
  auto defOf = [&](uint32_t id) -> const Ins* { auto it = defIdx.find(id); return it == defIdx.end() ? nullptr : &body[it->second]; };
  const Ins* d = defOf(coord);
  if (d && d->op == kOpSelect && d->wc == 6) d = defOf(in[d->at + 5]);   // the game guards the coordinate: select(isnan(uv), 0, uv)
  if (!d || d->op != kOpCompositeConstruct || d->wc != 5) return PomFail(__LINE__);
  const Ins* l1 = defOf(in[d->at + 3]); const Ins* l2 = defOf(in[d->at + 4]);
  if (!l1 || !l2 || l1->op != kOpLoad || l2->op != kOpLoad) return PomFail(__LINE__);
  const uint32_t varA = in[l1->at + 3], varB = in[l2->at + 3];
  std::map<uint32_t, bool> fnVar;
  for (auto& s2 : body) if (s2.op == kOpVariable && in[s2.at + 3] == 7) fnVar[in[s2.at + 2]] = true;
  if (!fnVar.count(varA) || !fnVar.count(varB) || varA == varB) return PomFail(__LINE__);
  // stores of A and B before the sample: exactly one each, in the first block
  size_t stA = SIZE_MAX, stB = SIZE_MAX; int nA = 0, nB = 0;
  size_t firstLabelEnd = 0; bool seenLabel = false; size_t afterFirstBlock = body.size();
  for (size_t k = 0; k < firstSample; k++) {
    if (body[k].op == kOpLabel) { if (seenLabel) { afterFirstBlock = k; break; } seenLabel = true; }
    if (body[k].op == kOpStore && in[body[k].at + 1] == varA) { stA = k; nA++; }
    if (body[k].op == kOpStore && in[body[k].at + 1] == varB) { stB = k; nB++; }
  }
  (void)firstLabelEnd;
  if (nA != 1 || nB != 1 || afterFirstBlock != body.size()) return PomFail(__LINE__);
  // no other store to A / B anywhere (the pair must hold the interpolated UV for the whole shader)
  for (size_t k = firstSample; k < body.size(); k++) if (body[k].op == kOpStore && (in[body[k].at + 1] == varA || in[body[k].at + 1] == varB)) return PomFail(__LINE__);
  const size_t insertAfter = std::max(stA, stB);
  if (insertAfter >= firstSample) return PomFail(__LINE__);

  uint32_t next = in[3];
  std::vector<uint32_t> annot, types, code;
  auto newType = [&](std::vector<uint32_t>& blk, uint32_t op, std::initializer_list<uint32_t> w) { const uint32_t id = next++; Emit(blk, op, {}); blk.pop_back(); (void)id; return id; };
  (void)newType;
  // ---- missing types
  if (!Bool) { Bool = next++; Emit(types, kOpTypeBool, {Bool}); }
  if (!U32) { U32 = next++; Emit(types, kOpTypeInt, {U32, 32, 0}); }
  const uint32_t V2 = vecType[{T, 2}], V3 = vecType[{T, 3}], V4 = vecType[{T, 4}];
  uint32_t vec2b = 0;
  { auto it = vecType.find({Bool, 2}); if (it != vecType.end()) vec2b = it->second; }
  (void)vec2b;
  auto ptrTo = [&](uint32_t sc, uint32_t pointee) -> uint32_t {
    auto it = ptrKey.find(((uint64_t)sc << 32) | pointee);
    if (it != ptrKey.end()) return it->second;
    const uint32_t id = next++; Emit(types, kOpTypePointer, {id, sc, pointee}); ptrKey[((uint64_t)sc << 32) | pointee] = id; return id;
  };
  uint32_t glslId = glsl;
  std::vector<uint32_t> importBlock;
  if (!glslId) { glslId = next++; uint32_t buf[4] = {0, 0, 0, 0}; memcpy(buf, "GLSL.std.450", 12); std::vector<uint32_t> w; w.push_back(glslId); w.insert(w.end(), buf, buf + 4);
                 importBlock.push_back(((uint32_t)(w.size() + 1) << 16) | kOpExtInstImport); importBlock.insert(importBlock.end(), w.begin(), w.end()); }
  // push block: struct { vec4 m[8]; } at offset 128
  const uint32_t c8 = next++; Emit(types, kOpConstant, {U32, c8, 8});
  const uint32_t arrT = next++; Emit(types, kOpTypeArray, {arrT, V4, c8});
  const uint32_t strT = next++; Emit(types, kOpTypeStruct, {strT, arrT});
  Emit(annot, kOpDecorate, {arrT, 6, 16});          // ArrayStride 16
  Emit(annot, kOpDecorate, {strT, 2});              // Block
  Emit(annot, kOpMemberDecorate, {strT, 0, 35, 128});   // Offset 128
  const uint32_t strPtr = ptrTo(9, strT);
  const uint32_t pushVar = next++; Emit(types, kOpVariable, {strPtr, pushVar, 9});
  const uint32_t pushV4Ptr = ptrTo(9, V4);
  uint32_t fragV = fragVar;
  bool addFrag = false;
  if (!fragV) {
    fragPtrType = ptrTo(1, V4);
    fragV = next++; Emit(types, kOpVariable, {fragPtrType, fragV, 1}); Emit(annot, kOpDecorate, {fragV, 11, 15}); addFrag = true;
  }
  auto cf = [&](float v) { const uint32_t id = next++; Emit(types, kOpConstant, {T, id, FBits(v)}); return id; };
  auto cu = [&](uint32_t v) { const uint32_t id = next++; Emit(types, kOpConstant, {U32, id, v}); return id; };
  const bool high = level >= 2;
  const int steps = high ? 16 : 8, lsteps = high ? 6 : 3;
  const uint32_t cFalse = next++; Emit(types, kOpConstantFalse, {Bool, cFalse});
  const uint32_t f0 = cf(0.f), f1 = cf(1.f), f2 = cf(2.f), fN1 = cf(-1.f), fHalf = cf(0.5f), fTiny = cf(1e-12f), f15 = cf(0.15f), f25 = cf(0.25f), f05 = cf(0.05f);
  const uint32_t fLum[3] = {cf(0.299f), cf(0.587f), cf(0.114f)}, fAvgLod = cf(5.0f), fGain = cf(2.6f), fOffLim = cf(0.12f), f8 = cf(8.0f), fN2 = cf(-2.0f);
  const uint32_t lumV = next++; Emit(types, kOpConstantComposite, {V3, lumV, fLum[0], fLum[1], fLum[2]});
  uint32_t uIdx[8]; for (int i = 0; i < 8; i++) uIdx[i] = cu((uint32_t)i);
  const uint32_t u0 = uIdx[0];
  std::vector<uint32_t> tc(steps + 1), sc(lsteps + 1);
  for (int i = 0; i <= steps; i++) tc[i] = cf((float)i / (float)steps);
  for (int j = 0; j <= lsteps; j++) sc[j] = cf((float)j / (float)lsteps);
  const uint32_t invSteps = cf(1.0f / (float)steps);

  Gen g{code, next, T, V2, V3, V4, Bool, U32, glslId};
  auto loadPush = [&](int i) { const uint32_t p = g.Op(kOpAccessChain, pushV4Ptr, {pushVar, u0, uIdx[i]}); return g.Op(kOpLoad, V4, {p}); };
  auto ex4 = [&](uint32_t v, int i) { return g.Ex(v, (uint32_t)i); };
  auto xyz = [&](uint32_t v) { return g.Op(kOpVectorShuffle, V3, {v, v, 0, 1, 2}); };
  auto mk2 = [&](uint32_t a, uint32_t b) { return g.Op(kOpCompositeConstruct, V2, {a, b}); };
  auto mk4 = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return g.Op(kOpCompositeConstruct, V4, {a, b, c, d}); };
  auto mk3 = [&](uint32_t a, uint32_t b, uint32_t c) { return g.Op(kOpCompositeConstruct, V3, {a, b, c}); };
  // 1. world position of this pixel from FragCoord + depth
  const uint32_t frag = g.Op(kOpLoad, V4, {fragV});
  const uint32_t fx = ex4(frag, 0), fy = ex4(frag, 1), fz = ex4(frag, 2);
  const uint32_t m0 = loadPush(0), m1 = loadPush(1), m2 = loadPush(2), m3 = loadPush(3), eye4 = loadPush(4), sun4 = loadPush(5), sz4 = loadPush(6), pr4 = loadPush(7);
  const uint32_t sx = ex4(sz4, 0), sy = ex4(sz4, 1), jx = ex4(sz4, 2), jy = ex4(sz4, 3);
  const uint32_t ndcX = g.Add(T, g.Mul(T, g.Div(g.Sub(T, fx, jx), sx), f2), fN1);
  const uint32_t ndcY = g.Add(T, g.Mul(T, g.Div(g.Sub(T, fy, jy), sy), fN2), f1);
  uint32_t h = g.Add(V4, g.Add(V4, g.Scale(V4, m0, ndcX), g.Scale(V4, m1, ndcY)), g.Add(V4, g.Scale(V4, m2, fz), m3));
  const uint32_t hw = ex4(h, 3);
  const uint32_t P = g.Scale(V3, xyz(h), g.Div(f1, g.Max(g.Ext(T, gFAbs, {hw}), fTiny)));
  const uint32_t Pw = g.Scale(V3, xyz(h), g.Div(f1, hw));
  const uint32_t eye = xyz(eye4);
  const uint32_t V = g.Sub(V3, eye, Pw);
  const uint32_t dist = g.Ext(T, gLength, {V});
  // 2. UV gradients: cotangent frame from screen derivatives
  const uint32_t uvA = g.Op(kOpLoad, T, {varA}), uvB = g.Op(kOpLoad, T, {varB});
  const uint32_t uv = mk2(uvA, uvB);
  const uint32_t dp1 = g.Op(kOpDPdx, V3, {Pw}), dp2 = g.Op(kOpDPdy, V3, {Pw});
  const uint32_t duv1 = g.Op(kOpDPdx, V2, {uv}), duv2 = g.Op(kOpDPdy, V2, {uv});
  const uint32_t Cn = g.Ext(V3, gCross, {dp1, dp2});
  const uint32_t Dn = g.Max(g.Ext(T, gLength, {Cn}), fTiny);
  const uint32_t Nrm = g.Scale(V3, Cn, g.Div(f1, Dn));
  const uint32_t aV = g.Ext(V3, gCross, {dp2, Nrm}), bV = g.Ext(V3, gCross, {Nrm, dp1});
  const uint32_t invD = g.Div(f1, Dn);
  const uint32_t gu = g.Scale(V3, g.Add(V3, g.Scale(V3, aV, ex4(duv1, 0)), g.Scale(V3, bV, ex4(duv2, 0))), invD);
  const uint32_t gv = g.Scale(V3, g.Add(V3, g.Scale(V3, aV, ex4(duv1, 1)), g.Scale(V3, bV, ex4(duv2, 1))), invD);
  // 3. view-ray uv offset for the full depth
  const uint32_t kDepth = ex4(pr4, 0), fadeA = ex4(pr4, 1), fadeB = ex4(pr4, 2), flipS = ex4(pr4, 3);
  const uint32_t Gx = g.Dot(gu, V), Gy = g.Dot(gv, V);
  const uint32_t Vn = g.Max(g.Ext(T, gFAbs, {g.Dot(Nrm, V)}), g.Mul(T, f15, dist));
  const uint32_t gm2 = g.Mul(T, fHalf, g.Add(T, g.Dot(gu, gu), g.Dot(gv, gv)));
  const uint32_t dW = g.Mul(T, kDepth, g.Ext(T, gInvSqrt, {g.Max(gm2, cf(1e-20f))}));
  const uint32_t fade = g.Sub(T, f1, g.Ext(T, gSmoothStep, {fadeA, fadeB, dist}));
  const uint32_t scl = g.Mul(T, g.Mul(T, g.Div(g.Sub(T, f0, dW), Vn), fade), flipS);
  uint32_t off = g.Scale(V2, mk2(Gx, Gy), scl);
  const uint32_t offLen = g.Ext(T, gLength, {off});
  off = g.Scale(V2, off, g.Min(f1, g.Div(fOffLim, g.Max(offLen, cf(1e-6f)))));
  // 4. height field: luminance against the coarse-mip average, sampled from the same sampler (a fresh load of its variable)
  const uint32_t imgPtr = varPtrType[samplerVar], imgType = ptrPointee[imgPtr];
  if (!imgType || in[body[firstSample].at + 1] != V4) return PomFail(__LINE__);
  const uint32_t img = g.Op(kOpLoad, imgType, {samplerVar});
  auto lumAt = [&](uint32_t u, uint32_t v, bool lod) {
    const uint32_t uvv = mk2(u, v);
    const uint32_t c = lod ? g.Op(kOpImageSampleExplicitLod, V4, {img, uvv, 2, fAvgLod}) : g.Op(kOpImageSampleImplicitLod, V4, {img, uvv});
    return g.Dot(xyz(c), lumV);
  };
  const uint32_t avg = lumAt(uvA, uvB, true);
  auto heightAt = [&](uint32_t u, uint32_t v) { return g.Clamp(g.Add(T, fHalf, g.Mul(T, fGain, g.Sub(T, lumAt(u, v, false), avg))), f0, f1); };
  // 5. steep parallax, unrolled: first layer where the ray is below the surface, linearly interpolated with the layer before it
  const uint32_t offX = g.Ex(off, 0), offY = g.Ex(off, 1);
  const uint32_t h0 = heightAt(uvA, uvB);
  uint32_t prevU = uvA, prevV = uvB, prevD = g.Sub(T, h0, f1), prevT = f0;
  uint32_t hitU = g.Add(T, uvA, offX), hitV = g.Add(T, uvB, offY), hitT = f1, found = cFalse;
  for (int i = 1; i <= steps; i++) {
    const uint32_t ui = g.Add(T, uvA, g.Mul(T, offX, tc[i])), vi = g.Add(T, uvB, g.Mul(T, offY, tc[i]));
    const uint32_t hh = heightAt(ui, vi);
    const uint32_t diff = g.Sub(T, g.Add(T, tc[i], hh), f1);
    const uint32_t cond = g.Op(kOpFOrdGreaterThanEqual, Bool, {diff, f0});
    const uint32_t hitNow = g.Op(kOpLogicalAnd, Bool, {g.Op(kOpLogicalNot, Bool, {found}), cond});
    const uint32_t w = g.Clamp(g.Div(g.Sub(T, f0, prevD), g.Max(g.Sub(T, diff, prevD), cf(1e-6f))), f0, f1);
    const uint32_t nu = g.Ext(T, gFMix, {prevU, ui, w}), nv = g.Ext(T, gFMix, {prevV, vi, w}), nt = g.Ext(T, gFMix, {prevT, tc[i], w});
    hitU = g.Sel(T, hitNow, nu, hitU); hitV = g.Sel(T, hitNow, nv, hitV); hitT = g.Sel(T, hitNow, nt, hitT);
    found = g.Op(kOpLogicalOr, Bool, {found, cond});
    prevU = ui; prevV = vi; prevD = diff; prevT = tc[i];
  }
  // 6. self-shadow: march from the hit point toward the sun, in UV space
  const uint32_t Lw = xyz(sun4);
  const uint32_t Ln = g.Dot(g.Scale(V3, Nrm, g.Sel(T, g.Op(kOpFOrdLessThan, Bool, {g.Dot(Nrm, V), f0}), fN1, f1)), Lw);
  const uint32_t Lu = g.Dot(gu, Lw), Lv = g.Dot(gv, Lw);
  const uint32_t hs = g.Sub(T, f1, hitT);
  const uint32_t rise = g.Sub(T, f1, hs);                                           // fraction of the depth still above the hit point
  const uint32_t lk = g.Mul(T, dW, g.Div(f1, g.Clamp(Ln, f25, f1)));               // uv per unit of height fraction along the light
  uint32_t occ = f0;
  for (int j = 1; j <= lsteps; j++) {
    const uint32_t sj = cf(((float)j - 0.5f) / (float)lsteps);
    const uint32_t k = g.Mul(T, g.Mul(T, sj, rise), lk);
    const uint32_t hj = heightAt(g.Add(T, hitU, g.Mul(T, Lu, k)), g.Add(T, hitV, g.Mul(T, Lv, k)));
    const uint32_t rayH = g.Add(T, hs, g.Mul(T, sj, rise));
    occ = g.Max(occ, g.Clamp(g.Mul(T, g.Sub(T, hj, rayH), cf(4.0f)), f0, f1));
  }
  const uint32_t sStr = ex4(sun4, 3);
  const uint32_t shade = g.Sub(T, f1, g.Mul(T, g.Mul(T, sStr, occ), fade));
  // 7. write the shifted UV back; darken the first base-color fetch (its result id moves to a new instruction that multiplies it)
  Emit(code, kOpStore, {varA, hitU});
  Emit(code, kOpStore, {varB, hitV});
  const uint32_t mult = mk4(shade, shade, shade, f1);
  const uint32_t R = in[body[firstSample].at + 2], Rp = next++;
  std::vector<uint32_t> post; Emit(post, kOpFMul, {V4, R, Rp, mult});
  // 8. assemble: header, annotations, import, types, entry-point interface, code
  std::vector<uint32_t> w = in;
  w[body[firstSample].at + 2] = Rp;
  uint32_t firstTypeAt = 0;
  for (size_t i = 5; i < in.size() && !firstTypeAt;) { const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF; if ((op >= 19 && op <= 39) || (op >= 41 && op <= 46) || op == kOpVariable) firstTypeAt = (uint32_t)i; i += wc; }
  struct Ins2 { size_t pos, erase; std::vector<uint32_t> w; };
  std::vector<Ins2> ins;
  ins.push_back({body[firstSample].at + body[firstSample].wc, 0, post});
  ins.push_back({body[insertAfter].at + body[insertAfter].wc, 0, code});
  ins.push_back({firstFn, 0, types});
  ins.push_back({lastDecEnd ? lastDecEnd : firstTypeAt, 0, annot});
  if (!importBlock.empty()) ins.push_back({memModelAt, 0, importBlock});
  {
    const uint32_t wc = in[entryIdx] >> 16;
    std::vector<uint32_t> ep(in.begin() + entryIdx, in.begin() + entryIdx + wc);
    if (addFrag) ep.push_back(fragV);
    if (in[1] >= 0x10400) ep.push_back(pushVar);
    ep[0] = ((uint32_t)ep.size() << 16) | kOpEntryPoint;
    ins.push_back({entryIdx, wc, ep});
  }
  std::sort(ins.begin(), ins.end(), [](const Ins2& a, const Ins2& b) { return a.pos > b.pos; });
  for (auto& x : ins) { w.erase(w.begin() + x.pos, w.begin() + x.pos + x.erase); w.insert(w.begin() + x.pos, x.w.begin(), x.w.end()); }
  w[3] = next;
  out.swap(w);
  return true;
}
namespace {
// Registered only so the Fx layer tracks the scene camera, color and depth while parallax is on (it does no pass of its own).
bool PomEnabled() { return PomLevel() > 0; }
const FxEffect kPomEffect = {"parallax", PomEnabled, nullptr, nullptr, false};
struct PomReg { PomReg() { FxRegister(&kPomEffect); } } pomReg;
}  // namespace
}  // namespace nvn
