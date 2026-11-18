// SPDX-License-Identifier: MIT
// Explicit Linux no-op stubs for Windows-only host/mods/* hooks and tracers (stages 5A & 5D-B2).
//
// Why these stubs are safe:
//   - All 24 rt_gm_* / rt_ui_opt_* / rt_mod_entry hooks below are MAIN_INLINE_HOOKS sites called via
//     rt_inline_hook(Cpu*, ra, fn) before the original guest instruction runs. When they return without
//     modifying Cpu state, the lifted guest code executes its original unmodded Switch instructions.
//   - None of them advertises mod support or registers host overrides via loader::add_override().
//   - rt_drp_decrypt / rt_drp_decrypt_check are NOT stubbed here: they are provided by shared host/mods/drpfast.cpp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "cpu.h"
#include "loader.h"

// The real ultrawide factor lives in runtime/nvn/nvn_wide.cpp. Targets that link this stub without the NVN renderer (hooks, audio runtime, ...)
// get 1.0 = no widening. The definition is weak on purpose: where the renderer is linked its strong definition wins, so this never overrides it.
extern "C" __attribute__((weak)) float nvnHostGetUltrawide(void) { return 1.0f; }

namespace {

struct WideSite { uint64_t off; int reg; };
WideSite g_wide_sites[8] = {{0x296400, 9}};   // ride and lab camera (builder 0x295170)
int g_wide_nsites = 1;

float wide_k() { static const float k = nvnHostGetUltrawide(); return k; }

void wide_camera(Cpu *c, uint64_t off) {
    int reg = -1;
    for (int i = 0; i < g_wide_nsites; i++) if (g_wide_sites[i].off == off) { reg = g_wide_sites[i].reg; break; }
    if (reg < 0) return;
    uint32_t ab = (uint32_t)c->v[reg];
    float a;
    std::memcpy(&a, &ab, 4);
    if (std::fabs(a - 16.0f / 9.0f) >= 0.01f) return;
    a *= wide_k();
    std::memcpy(&ab, &a, 4);
    c->v[reg] = (__uint128_t)ab;
}

void h_tanf(Cpu *c) {
    uint32_t b = (uint32_t)c->v[0];
    float x;
    std::memcpy(&x, &b, 4);
    if (wide_k() > 1.0f) wide_camera(c, c->x[30] - GUEST_MAIN_BASE);
    float r = tanf(x);
    uint32_t rb;
    std::memcpy(&rb, &r, 4);
    c->v[0] = (__uint128_t)rb;
}

}  // namespace

namespace loader {

void snap_wide_eval_copy(uint64_t src) {
    const float k = wide_k();
    if (k <= 1.0f) return;
    int fixed = 0;
    for (uint64_t o = 0; o + 64 <= 0x480; o += 4) {
        float *m = (float *)(src + o);   // column-major; a perspective projection has m[11] = -1 and zeros off its terms
        uint32_t w11;
        std::memcpy(&w11, m + 11, 4);
        if (w11 != 0xbf800000u) continue;
        if (m[1] != 0 || m[2] != 0 || m[3] != 0 || m[4] != 0 || m[6] != 0 || m[7] != 0 || m[12] != 0 || m[13] != 0 || m[15] != 0) continue;
        if (!(m[0] > 0 && m[5] > 0) || std::fabs(m[5] / m[0] / (16.0 / 9.0 * k) - 1.0) > 0.01) continue;
        m[0] *= k;
        fixed++;
    }
    static bool logged;
    if (fixed && !logged) { logged = true; std::fprintf(stderr, "ultrawide: photo Evaluation uses the 16:9 projection (x scale times %.4f)\n", k); }
}
void snap_photoexport_install() {}
void snap_shiny_install() {}
void snap_neofilm_install() {}
void snap_skips_install() {}
void snap_lodanim_install() {
    const char *uw = std::getenv("SNAP_ULTRAWIDE");
    const bool wide = !(uw && (strcasecmp(uw, "off") == 0 || std::strcmp(uw, "0") == 0));
    if (const char *ws = std::getenv("SNAP_WIDE_SITES")) {
        for (const char *p = ws; *p && g_wide_nsites < 8;) {
            char *e;
            const uint64_t off = std::strtoull(p, &e, 0);
            if (*e != ':') break;
            const int reg = (int)std::strtol(e + 1, &e, 10);
            if (off && reg >= 0 && reg < 32) g_wide_sites[g_wide_nsites++] = {off, reg};
            if (*e != ',') break;
            p = e + 1;
        }
        trace("ultrawide: %d camera sites", g_wide_nsites);
    }
    if (wide) add_override("tanf", h_tanf);
}

}  // namespace loader

extern "C" {

// Called by host/loader.cpp die() on fatal load errors (Windows uses host/mods/crashlog.cpp).
void snap_crashlog_fatal(const char *kind, const char *detail) {
    std::fprintf(stderr, "[linux] fatal %s: %s\n", kind ? kind : "error", detail ? detail : "");
}

// Function-entry observers called by host/loader.cpp fnprof_hit() (disabled on Linux until mods are ported).
void snap_fn_observe(uint64_t, uint64_t, uint64_t) {}
void snap_skips_observe(uint64_t, uint64_t, uint64_t) {}
void snap_neo_observe(uint64_t, uint64_t, uint64_t) {}
void snap_highfps_observe(uint64_t, uint64_t, uint64_t) {}

// MAIN_INLINE_HOOKS no-ops: each leaves Cpu untouched so original lifted guest instructions run unchanged.
void rt_mod_entry(Cpu *) {}
void rt_gm_arena7_size(Cpu *) {}       // keeps original 56 MiB arena 7 size (host/boot.cpp overrides to 112 MiB)
void rt_gm_ao_simple(Cpu *) {}
void rt_gm_ao_hbao(Cpu *) {}
void rt_gm_pass(Cpu *) {}
void rt_gm_lod_ratio(Cpu *) {}
void rt_gm_lod_ratio_sub(Cpu *) {}
void rt_gm_anim_rate(Cpu *) {}
void rt_gm_font_atlas_info(Cpu *) {}
void rt_gm_font_atlas_dims(Cpu *) {}
void rt_gm_font_glyph1(Cpu *) {}
void rt_gm_font_glyph2(Cpu *) {}
void rt_gm_font_fix(Cpu *) {}
void rt_gm_neo_speed(Cpu *) {}
void rt_gm_film_c(Cpu *) {}
void rt_gm_skip_wait(Cpu *) {}
void rt_gm_route_ui_event(Cpu *) {}
void rt_gm_route_ui_setstate(Cpu *) {}
// Branch hooks (recomp/snap_gen.py UI_PACE_BRANCH): x[16] != 0 falls through to normal execution.
void rt_gm_ui_enter(Cpu *c) { if (c) c->x[16] = 1; }
void rt_gm_ui_event(Cpu *c) { if (c) c->x[16] = 1; }
void rt_gm_ui_pad(Cpu *) {}
void rt_gm_ui_pointer(Cpu *) {}
void rt_ui_opt_event(Cpu *) {}
void rt_ui_opt_state(Cpu *) {}

}  // extern "C"
