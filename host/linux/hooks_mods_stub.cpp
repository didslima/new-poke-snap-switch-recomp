// SPDX-License-Identifier: MIT
// Explicit Linux no-op stubs for the host/mods/* hooks called by host/hooks.cpp (stage 5A).
// Those mods (photoexport, shiny, neofilm, skips, lodanim) remain Windows-only and unavailable on Linux
// until they are ported; nothing here registers any guest override or advertises mod support.
#include <cstdint>

namespace loader {

void snap_wide_eval_copy(uint64_t) {}
void snap_photoexport_install() {}
void snap_shiny_install() {}
void snap_neofilm_install() {}
void snap_skips_install() {}
void snap_lodanim_install() {}

}  // namespace loader
