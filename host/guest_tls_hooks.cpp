// SPDX-License-Identifier: MIT
// See guest_tls_hooks.h.
#include "guest_tls_hooks.h"

#include <cstdio>

#include "loader.h"
#include "os/guest_tls_block.h"

namespace snap_host {
namespace {
void tp_init_hook(Cpu *c, Thunk *) { osk::guest_tls_hook_init(c); }          // x0 = ThreadType; the thread's TLR is c->tpidrro
void tp_destroy_hook(Cpu *c, Thunk *) { osk::guest_tls_hook_destroy(c); }
}  // namespace

int install_guest_tls_hooks(const OsConfig &oc) {
    int bound = 0;
    if (!oc.tp_init_var) fprintf(stderr, "[os] guest TLS: no nnmuslThreadPointerInitializer variable: guest threads will have no thread pointer\n");
    else {
        if (!*(uint64_t *)oc.tp_init_var) *(uint64_t *)oc.tp_init_var = loader::add_stub("tp_init", tp_init_hook)->addr;
        bound++;
    }
    if (!oc.tp_destroy_var) fprintf(stderr, "[os] guest TLS: no nnmuslThreadPointerDestroyer variable: guest TLS blocks will not be released at thread exit\n");
    else {
        if (!*(uint64_t *)oc.tp_destroy_var) *(uint64_t *)oc.tp_destroy_var = loader::add_stub("tp_destroy", tp_destroy_hook)->addr;
        bound++;
    }
    return bound;
}

}  // namespace snap_host
