// SPDX-License-Identifier: MIT
// Explicit Linux no-op stubs for Windows-only host/mods/* hooks and tracers (stages 5A & 5D-B2).
//
// Why these stubs are safe:
//   - All 24 rt_gm_* / rt_ui_opt_* / rt_mod_entry hooks below are MAIN_INLINE_HOOKS sites called via
//     rt_inline_hook(Cpu*, ra, fn) before the original guest instruction runs. When they return without
//     modifying Cpu state, the lifted guest code executes its original unmodded Switch instructions.
//   - None of them advertises mod support or registers host overrides via loader::add_override().
//   - rt_drp_decrypt / rt_drp_decrypt_check are NOT stubbed here: they are provided by shared host/mods/drpfast.cpp.
#include <cstdint>
#include <cstdio>

struct Cpu;

namespace loader {

void snap_wide_eval_copy(uint64_t) {}
void snap_photoexport_install() {}
void snap_shiny_install() {}
void snap_neofilm_install() {}
void snap_skips_install() {}
void snap_lodanim_install() {}

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
void rt_gm_ui_enter(Cpu *) {}
void rt_gm_ui_event(Cpu *) {}
void rt_gm_ui_pad(Cpu *) {}
void rt_gm_ui_pointer(Cpu *) {}
void rt_ui_opt_event(Cpu *) {}
void rt_ui_opt_state(Cpu *) {}

}  // extern "C"
