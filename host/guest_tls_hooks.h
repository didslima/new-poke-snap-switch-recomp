// SPDX-License-Identifier: MIT
// Binds the musl thread-pointer hooks for the Linux runner (stage 4b). sdk's thread trampoline (sdk+0x348e80) calls *nnmuslThreadPointerInitializer
// (x0 = ThreadType) before a guest thread's entry and *nnmuslThreadPointerDestroyer after its TLS destructors (runtime/os/os_kernel.cpp
// host_entry reads them through OsConfig::tp_init_var / tp_destroy_var). Nothing in the lifted modules installs those pointers, so the host does:
// the Windows runner does it in host/boot.cpp (tp_init_hook / tp_destroy_hook, unchanged); this is the same for Linux, with the bodies in
// runtime/os/guest_tls_block.cpp.
#pragma once
#include "os/os.h"

namespace snap_host {

// Order the runner must keep (host/boot.cpp does the same for Windows; docs/linux-port.md, "Stage 4c", has the reasons):
//   1. loader::load(): the hook variables are guest memory inside the loaded modules, and the loader's stubs need its thunk table;
//   2. os_init(&oc) with oc.tp_init_var / oc.tp_destroy_var = loader::lookup("nnmuslThreadPointerInitializer" / "...Destroyer");
//   3. install_guest_tls_hooks(oc);
//   4. loader::new_thread() for the main thread: it makes the first TLS block and captures main's image;
//   5. only then os_adopt_main_thread, the initializers and any guest thread. A thread started before step 3 would run without a thread pointer
//      (tpidr_el0 = 0); before step 4 it would get a block of zeros (the hook says so on stderr) and the camera scan would not work.
// Each hook variable that is still empty gets the guest address of a loader stub that runs osk::guest_tls_hook_init / osk::guest_tls_hook_destroy.
// A variable that is not empty (the game or a mod set its own hook) is left alone, so calling this again binds nothing new. Returns how many of the
// two hooks are bound afterwards (2 = both, by this call or earlier; less means a variable is missing or null: a warning is printed for each).
int install_guest_tls_hooks(const OsConfig &oc);

}  // namespace snap_host
