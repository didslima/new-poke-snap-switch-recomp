// SPDX-License-Identifier: MIT
// Binds the musl thread-pointer hooks for the Linux runner (stage 4b). sdk's thread trampoline (sdk+0x348e80) calls *nnmuslThreadPointerInitializer
// (x0 = ThreadType) before a guest thread's entry and *nnmuslThreadPointerDestroyer after its TLS destructors (runtime/os/os_kernel.cpp
// host_entry reads them through OsConfig::tp_init_var / tp_destroy_var). Nothing in the lifted modules installs those pointers, so the host does:
// the Windows runner does it in host/boot.cpp (tp_init_hook / tp_destroy_hook, unchanged); this is the same for Linux, with the bodies in
// runtime/os/guest_tls_block.cpp.
#pragma once
#include "os/os.h"

namespace snap_host {

// Call after os_init(&oc), the way host/boot.cpp does: each hook variable that is still empty gets the guest address of a loader stub that runs
// osk::guest_tls_hook_init / osk::guest_tls_hook_destroy. A variable that is not empty (the game or a mod set its own hook) is left alone.
void install_guest_tls_hooks(const OsConfig &oc);

}  // namespace snap_host
