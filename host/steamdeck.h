// SPDX-License-Identifier: GPL-3.0-or-later
// Steam Deck preset (docs/steam-deck.md): a 1280 x 800 handheld profile, controller only. One table shared by the command line
// (`--steam-deck`, host/cli_flags.h) and the launcher's "Device" row (host/launcher_ui.cpp), so both mean the same thing. Header only and
// free of Windows and runtime includes, so tests/hle/cli_flags_test.cpp checks it on its own.
//
//   window   borderless 1280 x 800 (the Deck's panel; the game's 16:9 picture sits inside it with thin bars)
//   picture  the 3D view is drawn at 960 x 540 x 1.5 = 1440 x 810 (render scale 0.75), SMAA medium, 4x filtering, no extra effects:
//            a handheld GPU at 60 fps within a 15 W budget; frame generation and ray tracing stay off
//   HUD      HUD scale 150% and menu text 2x, so the viewfinder and dialogue read at arm's length on a 7-inch screen
//   buttons  Xbox glyphs always (Steam Input shows the Deck as an Xbox pad), never key caps
#pragma once
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace snapdeck {

constexpr int kWidth = 1280, kHeight = 800;

struct Kv { const char* key; const char* val; };

// settings.cfg keys (the same keys `--set` takes). A value "" would erase the key; none is needed here because the launcher's
// "Desktop" choice erases every key in this table.
inline const std::vector<Kv>& cfg() {
  static const std::vector<Kv> k = {
      {"gfx_render_scale", "0.75"},   // 1440 x 810: lighter than the game's 1920 x 1080, still sharper than the 1280 x 800 panel
      {"gfx_aa", "smaa"},
      {"gfx_smaa_quality", "medium"},
      {"gfx_aniso", "4"},
      {"gfx_sharpen", "0.00"},
      {"ssao_quality", "default"},
      {"gfx_bounce", "0"},
      {"hud_scale", "150"},           // larger HUD text
      {"gfx_ui_font", "2"},           // sharper menu letters at the bigger size
      {"gfx_ultrawide", "off"},
      {"button_icons", "xbox"},       // controller only: pad glyphs, no key caps
  };
  return k;
}

// window.json keys of the launcher (mode, width, height).
inline std::string window_mode() { return "borderless"; }

// Environment variables the `--steam-deck` flag sets for one run (the window flags use the same ones).
inline std::vector<std::pair<std::string, std::string>> env() {
  return {{"SNAP_WINDOW_MODE", "borderless"}, {"SNAP_WINDOW_W", std::to_string(kWidth)}, {"SNAP_WINDOW_H", std::to_string(kHeight)}};
}

}  // namespace snapdeck
