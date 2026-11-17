// SPDX-License-Identifier: MIT
// See guest_tls_hooks.h.
#include "guest_tls_hooks.h"

#include "loader.h"
#include "os/guest_tls_block.h"

namespace snap_host {
namespace {
void tp_init_hook(Cpu *c, Thunk *) { osk::guest_tls_hook_init(c); }          // x0 = ThreadType; the thread's TLR is c->tpidrro
void tp_destroy_hook(Cpu *c, Thunk *) { osk::guest_tls_hook_destroy(c); }
}  // namespace

void install_guest_tls_hooks(const OsConfig &oc) {
    if (oc.tp_init_var && !*(uint64_t *)oc.tp_init_var) *(uint64_t *)oc.tp_init_var = loader::add_stub("tp_init", tp_init_hook)->addr;
    if (oc.tp_destroy_var && !*(uint64_t *)oc.tp_destroy_var) *(uint64_t *)oc.tp_destroy_var = loader::add_stub("tp_destroy", tp_destroy_hook)->addr;
}

}  // namespace snap_host
