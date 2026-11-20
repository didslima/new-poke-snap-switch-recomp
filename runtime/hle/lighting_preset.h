// SPDX-License-Identifier: MIT
// The one parser of SNAP_LIGHTING (project code, ours). Header-only so the host mods (host/mods/gamemods.cpp) and the NVN layer
// (nvn_entry.cpp shadow resolution, nvn_draw.cpp shadow distance) read the same preset and "ultra" means the same thing everywhere.
//
//   SNAP_LIGHTING unset / default / original -> level 0 (the game's own lighting)
//   SNAP_LIGHTING=enhanced (alias high)      -> level 1, "Enhanced lighting": shadow maps follow the render resolution (2048 at 1080p, 4096 at 4K, 8192 at 8K),
//                                               2x shadow distance, soft contact-hardening shadows (PCSS, 16 taps), the screen-space occlusion and clean contact shadows
//                                               of runtime/nvn/nvn_lighting_fx.cpp, SSAO quality high in the photo passes
//   SNAP_LIGHTING=maximum (alias ultra)      -> level 2: one shadow-map size step above Enhanced, 4x shadow distance, SSAO quality ultra, more occlusion samples
// Every explicit switch (SNAP_SHADOW_RES, SNAP_SHADOW_DISTANCE, SNAP_SSAO_QUALITY, ...) still wins over the preset.
#pragma once
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "live.h"   // the Lighting preset row applies with no restart

namespace snaplight {
inline bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}
inline int level() {
  return SNAP_LIVE(int, ([] {
    const char* l = getenv("SNAP_LIGHTING");
    if (!l || !*l) return 0;
    if (ieq(l, "high") || ieq(l, "enhanced")) return 1;
    if (ieq(l, "ultra") || ieq(l, "maximum")) return 2;
    return 0;
  }()));
}
// SNAP_LIGHT_AB=<P> (test aid): the lighting features (shadow distance and fade, bias, filter, SSAO force) are on only in odd blocks of P presents,
// off in even ones: same-view A/B pairs inside one run. Unset = always on.
inline int ab_period() { static const int p = [] { const char* e = getenv("SNAP_LIGHT_AB"); return e ? atoi(e) : 0; }(); return p; }
// Live override for the one-run A/B capture tool (host/mods/abcap.cpp, host\ab_capture.ps1, docs/ab-capture.md): -1 = SNAP_LIGHT_AB decides (the default),
// 0 = every effect that asks ab_on() is off (the game's own picture), 1 = on. Only the tool writes it.
inline std::atomic<int>& ab_force() { static std::atomic<int> v{-1}; return v; }
inline bool ab_on(unsigned long long present) {
  const int f = ab_force().load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
  const int p = ab_period();
  return p <= 0 || ((present / (unsigned long long)p) & 1ull) != 0;
}
inline bool enhanced() { return level() >= 1; }
inline int shadow_res() { const int l = level(); return l == 2 ? 4096 : l == 1 ? 2048 : 0; }          // 0 = the game's 1024 (fixed sizes, superseded by shadow_res_for_scale)
// Shadow-map side that follows the internal render resolution: the smallest power of two at or above the scene height (1080 x render scale), at least 2048, at most 8192;
// Maximum goes one step further. 0 = the game's own 1024 (preset Original).
inline int shadow_res_for_scale(float scale) {
  const int l = level();
  if (l < 1) return 0;
  const double h = 1080.0 * (scale > 0.f ? scale : 1.f);
  int r = 2048;
  while (r < h && r < 8192) r *= 2;
  if (l == 2 && r < 8192) r *= 2;
  return r;
}
inline float shadow_distance() { const int l = level(); return l == 2 ? 4.0f : l == 1 ? 2.0f : 1.0f; }
inline const char* ssao_quality() { const int l = level(); return l == 2 ? "ultra" : l == 1 ? "high" : nullptr; }
}  // namespace snaplight
