// SPDX-License-Identifier: MIT
// P5 (lighting plan): wider shadow filtering. Every depth-compare tap of a fragment shader (OpImageSampleDrefExplicitLod, Lod 0, a 2D
// sampler2DShadow: 177 programs have one tap and 679 two, none rely on anything else; census in analysis/lighting/p5_census.md) is replaced by
// N taps on a Vogel (golden-angle) disk around the original coordinate and their average. The kernel radius is a fixed UV distance, so the softness of
// an edge is the same at 1024, 2048 and 4096 (a larger map just resolves it more smoothly). The coordinate and reference depth operands are reused as they
// are, so the game's NaN guard stays and a 1x1 "no shadow map" stand-in view (always lit) still returns 1 for every tap.
// SNAP_SHADOW_FILTER=0|8|16 (0 = off: this function is not called and every pipeline key is the original one).
#include <cmath>
#include <string>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "hle/lighting_preset.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
enum : uint32_t { kOpTypeFloat = 22, kOpTypeVector = 23, kOpConstant = 43, kOpConstantComposite = 44, kOpFunction = 54, kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133,
                  kOpImageSampleDrefExplicitLod = 90, kOpImageFetch = 95, kOpImage = 100, kOpImageQuerySizeLod = 103, kOpCapability = 17, kOpExtInstImport = 11,
                  kOpExtInst = 12, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeSampledImage = 27, kOpMemoryModel = 14, kOpCompositeExtract = 81, kOpSelect = 169,
                  kOpConvertFToS = 110, kOpConvertSToF = 111, kOpISub = 130, kOpFDiv = 136, kOpVectorTimesScalar = 142, kOpFOrdLessThan = 184, kOpFOrdGreaterThan = 186 };
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
}  // namespace

// SNAP_SHADOW_FILTER = 0 (off) | 8 | 16 (taps); SNAP_SHADOW_SOFTNESS = kernel radius in texels of the game's own 1024 map (default 1.5).
// SNAP_SHADOW_BIAS=<n>: receiver-side bias, n * 1e-4 subtracted from the reference depth of every tap (the game applies none and draws its casters without polygon offset).
// SNAP_SHADOW_PCSS=0|1 (default: on with the Enhanced lighting preset, off otherwise). PCSS: every tap first searches the map around itself for blockers
// (texel fetches), converts the average receiver-to-blocker depth gap into a filter radius (contact hardening: sharp where the shadow starts, soft far
// from the caster) and then runs the Vogel PCF at that radius. Needs ShadowFilterTaps() > 0 (the enhanced preset sets 16 unless SNAP_SHADOW_FILTER says otherwise).
static bool PcssWanted() {
  return SNAP_LIVE(bool, ([] {   // follows the Lighting preset row with no restart (hle/live.h)
    const char* e = getenv("SNAP_SHADOW_PCSS");
    if (e && *e) return atoi(e) != 0;
    return snaplight::enhanced();
  }()));
}
bool ShadowPcss() { return PcssWanted() && snaplight::ab_on((unsigned long long)g_presentCount); }
PcssParams ShadowPcssParams() {
  static const PcssParams p = [] {
    PcssParams q;
    const char* e;
    if ((e = getenv("SNAP_SHADOW_LIGHT")) && *e) q.lightSize = (float)atof(e);
    if ((e = getenv("SNAP_SHADOW_PCSS_MAX")) && *e) q.maxTexels = (float)atof(e);
    if ((e = getenv("SNAP_SHADOW_PCSS_SEARCH")) && *e) q.searchTaps = std::max(4, std::min(16, atoi(e)));
    q.flip = (e = getenv("SNAP_SHADOW_PCSS_FLIP")) && *e && atoi(e) != 0;
    return q;
  }();
  return p;
}
float ShadowFilterBias() {
  static const float b = [] { const char* e = getenv("SNAP_SHADOW_BIAS"); const float f = e ? (float)atof(e) : 0.0f; return (f > 0.0f && f <= 1000.0f) ? f * 1.0e-4f : 0.0f; }();
  return b;
}
int ShadowFilterTaps() {
  const int taps = SNAP_LIVE(int, ([] {
    const char* e = getenv("SNAP_SHADOW_FILTER");
    const int v = (!e || !*e || !strcmp(e, "default")) ? 0 : atoi(e);
    if (v == 4 || v == 8 || v == 16 || v == 32) return v;
    if ((!e || !*e || !strcmp(e, "default")) && PcssWanted()) return 16;   // PCSS (and so the Enhanced preset) filters with 16 taps
    return ShadowFilterBias() > 0.0f ? 1 : 0;   // a bias alone: one tap at the original coordinate, reference depth lowered
  }()));
  return snaplight::ab_on((unsigned long long)g_presentCount) ? taps : 0;   // SNAP_LIGHT_AB
}
float ShadowFilterRadius() {
  static const float r = [] {
    float t = 1.5f;
    if (const char* e = getenv("SNAP_SHADOW_SOFTNESS")) { const float f = (float)atof(e); if (f >= 0.25f && f <= 16.0f) t = f; }
    return t / 1024.0f;
  }();
  return r;
}
bool ShadowFilterApplies(const StageProg& fs) {
  static std::mutex mu;
  static std::unordered_map<std::string, bool> cache;
  const std::string key = fs.key;
  { std::lock_guard<std::mutex> l(mu); auto it = cache.find(key); if (it != cache.end()) return it->second; }
  bool has = false;
  const std::vector<uint32_t>& w = fs.spv;
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16;
    if (!wc) break;
    if ((w[i] & 0xFFFF) == kOpImageSampleDrefExplicitLod) { has = true; break; }
    i += wc;
  }
  std::lock_guard<std::mutex> l(mu);
  cache[key] = has;
  return has;
}

// taps: 1 (bias only) or 4..32. radiusUv: kernel radius in texture-coordinate units. Returns false (out untouched) when the module has no patchable tap.
bool PatchShadowPcf(const std::vector<uint32_t>& in, int taps, float radiusUv, float bias, float remapK, std::vector<uint32_t>& out, const PcssParams* pcss) {
  if (in.size() < 6 || in[0] != 0x07230203 || taps < 1 || taps > 64) return false;
  // pass 1: float32 types, vec2-of-float32 types, result types of the instructions that define the taps' coordinates, first function, taps
  std::map<uint32_t, bool> isF32;
  std::map<uint32_t, uint32_t> vec2Of;           // vec2 type id -> component type id
  std::map<uint32_t, uint32_t> resType;          // result id -> result type id (only instructions with word1 = type, word2 = result)
  size_t firstFn = 0;
  std::vector<size_t> tapAt;
  std::map<uint32_t, std::string> nameOf;        // OpName
  std::map<uint32_t, uint32_t> loadPtr, sampImgOf;   // OpLoad result -> pointer; OpSampledImage result -> image
  // PCSS needs: ImageQuery, GLSL.std.450, int32 / bool / vec4 types, the image type behind each tap's sampled image
  std::map<uint32_t, uint32_t> sampledToImage;                        // OpTypeSampledImage id -> image type id
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecType;          // (component type, count) -> id
  uint32_t i32T = 0, boolT = 0, glslSet = 0;
  size_t capEnd = 0, memModelAt = 0;
  bool hasImageQuery = false;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    if (op == kOpCapability) { capEnd = i + wc; if (wc >= 2 && in[i + 1] == 50) hasImageQuery = true; }
    else if (op == kOpExtInstImport && wc >= 3 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glslSet = in[i + 1];
    else if (op == kOpMemoryModel && !memModelAt) memModelAt = i;
    else if (op == kOpTypeInt && wc >= 4 && in[i + 2] == 32 && in[i + 3] == 1) i32T = in[i + 1];
    else if (op == kOpTypeBool) boolT = in[i + 1];
    else if (op == kOpTypeSampledImage && wc >= 3) sampledToImage[in[i + 1]] = in[i + 2];
    if (op == kOpTypeVector && wc >= 4) vecType[{in[i + 2], in[i + 3]}] = in[i + 1];
    if (op == 5 && wc >= 3) nameOf[in[i + 1]] = std::string((const char*)&in[i + 2], strnlen((const char*)&in[i + 2], (wc - 2) * 4));
    else if (op == 61 && wc >= 4) loadPtr[in[i + 2]] = in[i + 3];
    else if (op == 86 && wc >= 5) sampImgOf[in[i + 2]] = in[i + 3];
    if (op == kOpTypeFloat && wc >= 3 && in[i + 2] == 32) isF32[in[i + 1]] = true;
    else if (op == kOpTypeVector && wc >= 4 && in[i + 3] == 2) vec2Of[in[i + 1]] = in[i + 2];
    else if (op == kOpFunction) { if (!firstFn) firstFn = i; }
    else if (firstFn && wc >= 3) resType[in[i + 2]] = in[i + 1];
    if (op == kOpImageSampleDrefExplicitLod && wc >= 8 && firstFn) tapAt.push_back(i);
    i += wc;
  }
  if (!firstFn || tapAt.empty()) return false;
  // vec2 float32 type
  // the dynamic shadow map is texture control block slot C (fp_t_tcb_C); only its taps are remapped when remapK > 1
  auto isDyn = [&](uint32_t img) {
    uint32_t id = img;
    auto si = sampImgOf.find(id); if (si != sampImgOf.end()) id = si->second;
    auto lp = loadPtr.find(id); if (lp != loadPtr.end()) id = lp->second;
    auto n = nameOf.find(id);
    return n != nameOf.end() && n->second.size() > 2 && n->second.compare(n->second.size() - 2, 2, "_C") == 0;
  };
  std::vector<bool> dynTap;
  std::vector<std::pair<uint32_t, uint32_t>> usable;   // (tap index into tapAt, vec2 type)
  for (size_t t = 0; t < tapAt.size(); t++) {
    const size_t i = tapAt[t];
    const uint32_t resT = in[i + 1], coord = in[i + 4];
    auto ct = resType.find(coord);
    if (!isF32.count(resT) || ct == resType.end() || !vec2Of.count(ct->second) || vec2Of[ct->second] != resT) continue;
    if (in[i + 6] != 2) continue;   // operands: only "Lod" (mask 2) is understood
    usable.push_back({(uint32_t)t, ct->second}); dynTap.push_back(remapK > 1.0f && isDyn(in[i + 3]));
  }
  if (usable.empty()) return false;
  // PCSS only when every usable tap shares one float type and vec2 type and its sampled-image type is known
  bool pcssOn = pcss != nullptr;
  const uint32_t fT0 = in[tapAt[usable[0].first] + 1], v2T0 = usable[0].second;
  std::vector<uint32_t> tapImgT(usable.size(), 0);
  if (pcssOn) {
    for (size_t u = 0; u < usable.size(); u++) {
      const size_t i = tapAt[usable[u].first];
      auto st = resType.find(in[i + 3]);
      if (in[i + 1] != fT0 || usable[u].second != v2T0 || st == resType.end() || !sampledToImage.count(st->second)) { pcssOn = false; break; }
      tapImgT[u] = sampledToImage[st->second];
    }
  }

  uint32_t next = in[3];
  // constants: one scalar per distinct value, one vec2 constant per offset, 1/N
  struct Off { float x, y; };
  std::vector<Off> offs;
  const float golden = 2.39996323f;
  const float diskR = pcssOn ? 1.0f : radiusUv;   // PCSS: unit disk, scaled by the penumbra radius at run time
  for (int k = 0; k < taps; k++) {
    const float r = taps == 1 ? 0.0f : diskR * std::sqrt(((float)k + 0.5f) / (float)taps), a = (float)k * golden;
    offs.push_back({r * std::cos(a), r * std::sin(a)});
  }
  // The float type of the taps: all taps of one module use the same one in practice; constants are made per (float type, vec2 type) pair.
  std::vector<uint32_t> constBlock, body;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> offIds;   // (float, vec2) -> composite ids per offset
  std::map<uint32_t, uint32_t> invN, biasId;
  std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> remapIds;   // (float, vec2) -> (half2, invK2) constants
  auto fbits = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
  for (auto& u : usable) {
    const uint32_t fT = in[tapAt[u.first] + 1], v2T = u.second;
    if (!offIds.count({fT, v2T})) {
      std::vector<uint32_t> ids;
      std::map<uint32_t, uint32_t> sc;   // float bits -> scalar constant id
      auto scalar = [&](float f) {
        const uint32_t key = fbits(f);
        auto it = sc.find(key);
        if (it != sc.end()) return it->second;
        const uint32_t id = next++;
        Emit(constBlock, kOpConstant, {fT, id, key});
        sc[key] = id;
        return id;
      };
      for (auto& o : offs) {
        const uint32_t cx = scalar(o.x), cy = scalar(o.y), id = next++;
        Emit(constBlock, kOpConstantComposite, {v2T, id, cx, cy});
        ids.push_back(id);
      }
      offIds[{fT, v2T}] = ids;
    }
    if (remapK > 1.0f && !remapIds.count({fT, v2T})) {
      auto sc = [&](float f) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT, id, fbits(f)}); return id; };
      const uint32_t h = sc(0.5f), ik = sc(1.0f / remapK), h2 = next++, k2 = next++;
      Emit(constBlock, kOpConstantComposite, {v2T, h2, h, h});
      Emit(constBlock, kOpConstantComposite, {v2T, k2, ik, ik});
      remapIds[{fT, v2T}] = {h2, k2};
    }
    if (!invN.count(fT)) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT, id, fbits(1.0f / (float)taps)}); invN[fT] = id; }
    if (bias > 0.0f && !biasId.count(fT)) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT, id, fbits(bias)}); biasId[fT] = id; }
  }
  // PCSS ids: types, constants, blocker-search offsets
  struct Pc { uint32_t i32 = 0, v2i = 0, boolT = 0, v4f = 0, i0 = 0, v2i0 = 0, v2i1 = 0, f0 = 0, f1 = 0, fhalf = 0, light = 0, rmin = 0, rmax = 0, glsl = 0; std::vector<uint32_t> sOff; } pc;
  std::vector<uint32_t> capBlock, importBlock;
  int nSearch = 0;
  if (pcssOn) {
    nSearch = pcss->searchTaps;
    auto vtype = [&](uint32_t comp, uint32_t n, uint32_t& slot) {
      auto it = vecType.find({comp, n});
      if (it != vecType.end()) { slot = it->second; return; }
      slot = next++; Emit(constBlock, kOpTypeVector, {slot, comp, n});
    };
    if (!(pc.i32 = i32T)) { pc.i32 = next++; Emit(constBlock, kOpTypeInt, {pc.i32, 32, 1}); }
    if (!(pc.boolT = boolT)) { pc.boolT = next++; Emit(constBlock, kOpTypeBool, {pc.boolT}); }
    vtype(pc.i32, 2, pc.v2i);
    vtype(fT0, 4, pc.v4f);
    auto icst = [&](uint32_t v) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {pc.i32, id, v}); return id; };
    auto fcst = [&](float f) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT0, id, fbits(f)}); return id; };
    pc.i0 = icst(0);
    const uint32_t i1 = icst(1);
    pc.v2i0 = next++; Emit(constBlock, kOpConstantComposite, {pc.v2i, pc.v2i0, pc.i0, pc.i0});
    pc.v2i1 = next++; Emit(constBlock, kOpConstantComposite, {pc.v2i, pc.v2i1, i1, i1});
    pc.f0 = fcst(0.0f); pc.f1 = fcst(1.0f); pc.fhalf = fcst(0.5f);
    pc.light = fcst(pcss->lightSize);
    pc.rmin = fcst(radiusUv);
    pc.rmax = fcst(std::max(radiusUv, pcss->maxTexels / 1024.0f));
    const float searchR = std::max(radiusUv, pcss->maxTexels / 1024.0f);
    for (int k = 0; k < nSearch; k++) {
      const float r = searchR * std::sqrt(((float)k + 0.5f) / (float)nSearch), a = (float)k * golden;
      const uint32_t cx = fcst(r * std::cos(a)), cy = fcst(r * std::sin(a)), id = next++;
      Emit(constBlock, kOpConstantComposite, {v2T0, id, cx, cy});
      pc.sOff.push_back(id);
    }
    if (!(pc.glsl = glslSet)) {
      pc.glsl = next++;
      std::vector<uint32_t> w(1, pc.glsl); const char* nm = "GLSL.std.450"; uint32_t buf[4] = {0, 0, 0, 0}; memcpy(buf, nm, 12); w.insert(w.end(), buf, buf + 4);
      importBlock.push_back(((uint32_t)(w.size() + 1) << 16) | kOpExtInstImport); importBlock.insert(importBlock.end(), w.begin(), w.end());
    }
    if (!hasImageQuery) Emit(capBlock, kOpCapability, {50});
  }
  // pass 2: copy, inserting the constants before the first function and expanding every usable tap
  out.assign(in.begin(), in.begin() + 5);
  size_t ui = 0;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16;
    if (i == capEnd && !capBlock.empty()) out.insert(out.end(), capBlock.begin(), capBlock.end());
    if (i == memModelAt && !importBlock.empty()) out.insert(out.end(), importBlock.begin(), importBlock.end());
    if (i == firstFn) out.insert(out.end(), constBlock.begin(), constBlock.end());
    if (ui < usable.size() && tapAt[usable[ui].first] == i) {
      const uint32_t fT = in[i + 1], res = in[i + 2], img = in[i + 3], coord = in[i + 4], dref = in[i + 5], lod = in[i + 7];
      const uint32_t v2T = usable[ui].second;
      const std::vector<uint32_t>& ids = offIds[{fT, v2T}];
      uint32_t acc = 0, drefUse = dref, coordUse = coord;
      if (dynTap[ui]) {   // (uv - 0.5) / k + 0.5: the caster covers k times more ground, so the receiver looks up the matching sub-rectangle
        auto& r = remapIds[{fT, v2T}];
        const uint32_t a = next++, b = next++; coordUse = next++;
        Emit(out, kOpFSub, {v2T, a, coord, r.first});
        Emit(out, kOpFMul, {v2T, b, a, r.second});
        Emit(out, kOpFAdd, {v2T, coordUse, b, r.first});
      }
      if (bias > 0.0f) { drefUse = next++; Emit(out, kOpFSub, {fT, drefUse, dref, biasId[fT]}); }
      uint32_t rId = 0;
      if (pcssOn) {   // blocker search around the tap, then the filter radius from the average receiver-minus-blocker depth gap
        const uint32_t im = next++, sz = next++, szf = next++, szm1 = next++;
        Emit(out, kOpImage, {tapImgT[ui], im, img});
        Emit(out, kOpImageQuerySizeLod, {pc.v2i, sz, im, pc.i0});
        Emit(out, kOpConvertSToF, {v2T, szf, sz});
        Emit(out, kOpISub, {pc.v2i, szm1, sz, pc.v2i1});
        uint32_t sum = pc.f0, cnt = pc.f0;
        for (int k = 0; k < nSearch; k++) {
          const uint32_t u = next++, t = next++, ti = next++, tc = next++, d4 = next++, d = next++, isB = next++, wd = next++, wcn = next++, ns = next++, nc = next++;
          Emit(out, kOpFAdd, {v2T, u, coordUse, pc.sOff[k]});
          Emit(out, kOpFMul, {v2T, t, u, szf});
          Emit(out, kOpConvertFToS, {pc.v2i, ti, t});
          Emit(out, kOpExtInst, {pc.v2i, tc, pc.glsl, 45, ti, pc.v2i0, szm1});   // SClamp
          Emit(out, kOpImageFetch, {pc.v4f, d4, im, tc, 2, pc.i0});
          Emit(out, kOpCompositeExtract, {fT, d, d4, 0});
          Emit(out, kOpFOrdLessThan, {pc.boolT, isB, pcss->flip ? drefUse : d, pcss->flip ? d : drefUse});
          Emit(out, kOpSelect, {fT, wd, isB, d, pc.f0});
          Emit(out, kOpSelect, {fT, wcn, isB, pc.f1, pc.f0});
          Emit(out, kOpFAdd, {fT, ns, sum, wd}); Emit(out, kOpFAdd, {fT, nc, cnt, wcn});
          sum = ns; cnt = nc;
        }
        const uint32_t cm = next++, avg = next++, diff = next++, has = next++, d2 = next++, r0 = next++;
        rId = next++;
        Emit(out, kOpExtInst, {fT, cm, pc.glsl, 40, cnt, pc.f1});          // FMax
        Emit(out, kOpFDiv, {fT, avg, sum, cm});
        Emit(out, kOpFSub, {fT, diff, drefUse, avg});
        Emit(out, kOpFOrdGreaterThan, {pc.boolT, has, cnt, pc.fhalf});
        Emit(out, kOpSelect, {fT, d2, has, diff, pc.f0});
        Emit(out, kOpFMul, {fT, r0, d2, pc.light});
        Emit(out, kOpExtInst, {fT, rId, pc.glsl, 43, r0, pc.rmin, pc.rmax});   // FClamp
      }
      for (int k = 0; k < taps; k++) {
        const uint32_t c = next++, s = next++;
        uint32_t offId = ids[k];
        if (rId) { offId = next++; Emit(out, kOpVectorTimesScalar, {v2T, offId, ids[k], rId}); }
        Emit(out, kOpFAdd, {v2T, c, coordUse, offId});
        Emit(out, kOpImageSampleDrefExplicitLod, {fT, s, img, c, drefUse, 2, lod});
        if (!acc) acc = s;
        else { const uint32_t a = next++; Emit(out, kOpFAdd, {fT, a, acc, s}); acc = a; }
      }
      if (taps == 1) { Emit(out, kOpFMul, {fT, res, acc, invN[fT]}); }   // 1/1: keeps the original result id as the definition
      else Emit(out, kOpFMul, {fT, res, acc, invN[fT]});
      ui++;
    } else out.insert(out.end(), in.begin() + i, in.begin() + i + wc);
    i += wc;
  }
  out[3] = next;
  return true;
}
}  // namespace nvn
