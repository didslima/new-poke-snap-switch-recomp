// SPDX-License-Identifier: MIT
// The visual settings whose defaults can differ from the original game, in ONE table (project code, ours).
//
// Each row has two values: the original game's ("original") and this port's extras ("enhanced"). kDefaultEnhanced picks which column is
// the default when nothing is set. The owner chose the original look on 2026-11-13, then (2026-11-15) the Enhanced look for new installs:
// a player with no saved choice gets the enhanced column, a saved choice (any row's own key, or the Look row) always wins. The F1 and in-game
// "Look: Enhanced / Original" row stores settings key gfx_look, which settings.cpp exports as SNAP_VISUALS, and clears the rows' own keys.
// SNAP_VISUALS=original|enhanced overrides the choice for one run (all rows at once). (It was SNAP_PROFILE until 2026-11-13, which
// clashed with the save-profile name read by hle_config.cpp; SNAP_PROFILE now means only the save profile.) A value set on its own (env var, or a saved F1 menu
// choice, which settings.cpp exports as env) always wins over the profile, so every extra still works through its own switch.
//
// Readers use snapdef::get("SNAP_...") instead of getenv: it returns the env value when set (non-empty), else the profile's value.
// A code path that must know whether the user chose a value explicitly (SNAP_AA with an upscaler) keeps using getenv.
// SNAP_LOD_BIAS is not a row: "auto" already follows the render scale and is a no-op at 1x, so original and enhanced behave the same.
#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace snapdef {
constexpr bool kDefaultEnhanced = true;   // the one flip: true = ssaa2+smaa, CAS 0.3, 16x aniso, font supersampling, PNG export, HD photos by default (owner 2026-11-15)

struct Def {
  const char* key;        // env var name (the settings layer exports the F1 menu choice under this name)
  const char* original;   // the original game's behavior
  const char* enhanced;   // this port's extra
  const char* original_text;   // plain-language names for the F1 menu labels
  const char* enhanced_text;
};
inline const Def* table(int* n) {
  static const Def t[] = {
      {"SNAP_AA", "original", "ssaa2+smaa", "the game's own FXAA", "2x supersampling + SMAA"},
      {"SNAP_SHARPEN", "0.00", "0.30", "off", "0.30"},                                    // CAS sharpening strength
      {"SNAP_ANISO", "0", "16", "off, the game's own filtering", "16x"},                  // anisotropic filtering (0 = leave the game's)
      {"SNAP_UI_FONT_SCALE", "off", "auto", "off, as the game ships", "automatic: 2x at 1440p and above"},
      {"SNAP_PHOTO_EXPORT", "0", "1", "off", "on"},                                       // a PNG file per kept photo
      {"SNAP_HD_PHOTOS", "0", "1", "off", "on"},                                          // 2160x1440 photo copies (runtime/nvn/nvn_hdphoto.cpp)
      // HOOK for the lighting worker (Lighting v2): when "Enhanced lighting" passes its checks, add its row here and read it through
      // snapdef::get("SNAP_LIGHTING") in runtime/hle/lighting_preset.h (it reads getenv today):
      //   {"SNAP_LIGHTING", "original", "enhanced", "the game's own lighting", "enhanced lighting"},
      // and add "gfx_lighting" to look_keys() below so the Look row flips and clears it with the others.
  };
  *n = (int)(sizeof t / sizeof t[0]);
  return t;
}
// The saved Look (settings key gfx_look, set by hle/settings.cpp on load and live from the Look row): -1 = nothing saved, 0 Original, 1 Enhanced.
inline std::atomic<int>& saved_look() { static std::atomic<int> v{-1}; return v; }
// Order: SNAP_VISUALS set by hand, then the saved Look, then kDefaultEnhanced.
// The settings keys that follow the rows above, one per row with a settings key (the SNAP_LIGHTING hook adds "gfx_lighting"). The Look row (F1, in-game
// Quality tab, launcher) erases all of them when it flips, so the whole bundle follows the new Look; a key left alone keeps the player's own choice.
inline const char* const* look_keys(int* n) {
  static const char* const k[] = {"gfx_aa", "gfx_sharpen", "gfx_aniso", "gfx_ui_font", "photo_export", "hd_photos"};
  *n = (int)(sizeof k / sizeof k[0]);
  return k;
}
// The Enhanced look's supersampling scale (the AA step's "ssaa2" when SNAP_AA is not set by hand), relative to the OUTPUT with a pixel budget:
// render height = min(2 x window height, max(window height, budgetH)), as a scale over the game's 1080 lines, clamped to 1 .. maxScale.
// 1080p -> 2.0 (2x per side), 1440p -> 2.0 (1.5x), 2160p -> 2.0 (1.0x, SMAA and sharpening only) with the default budget of 2160 lines.
// budgetH <= 0 means no budget (the full 2x). Used by runtime/nvn/nvn_aa.cpp; pure so tests/hle can check it.
inline float ssaa_auto_scale(int windowH, float budgetH, float maxScale) {
  const float H = (float)(windowH > 1 ? windowH : 1);
  const float b = budgetH > 0.f ? budgetH : 2.0f * H;
  float r = 2.0f * H; if (b < r) r = b; if (r < H) r = H;
  float v = r / 1080.0f;
  if (v < 1.0f) v = 1.0f;
  if (v > maxScale) v = maxScale;
  return v;
}
inline bool enhanced() {
  const char* p = getenv("SNAP_VISUALS");
  if (p && *p) {
    auto ieq = [](const char* a, const char* b) {
      for (; *a && *b; ++a, ++b) { char ca = *a, cb = *b; if (ca >= 'A' && ca <= 'Z') ca += 32; if (cb >= 'A' && cb <= 'Z') cb += 32; if (ca != cb) return false; }
      return *a == *b;
    };
    if (ieq(p, "enhanced")) return true;
    if (ieq(p, "original")) return false;
  }
  const int l = saved_look().load(std::memory_order_relaxed);
  return l >= 0 ? l == 1 : kDefaultEnhanced;
}
inline const Def* find(const char* key) {
  int n; const Def* t = table(&n);
  for (int i = 0; i < n; i++) if (!strcmp(t[i].key, key)) return &t[i];
  return nullptr;
}
// The profile's value for a key, ignoring the environment (what the F1 menu shows as the default). nullptr for an unknown key.
inline const char* def(const char* key) { const Def* d = find(key); return d ? (enhanced() ? d->enhanced : d->original) : nullptr; }
inline const char* def_text(const char* key) { const Def* d = find(key); return d ? (enhanced() ? d->enhanced_text : d->original_text) : ""; }
// The effective value: the env var when set and non-empty, else the profile's value.
inline const char* get(const char* key) {
  const char* e = getenv(key);
  return (e && *e) ? e : def(key);
}
}  // namespace snapdef
