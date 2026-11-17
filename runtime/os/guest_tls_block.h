// SPDX-License-Identifier: MIT
// Guest TLS blocks: where they come from, who owns them, and the bodies of the two musl thread-pointer hooks (stage 4b; docs/linux-port.md).
//
// The checked initialisation is runtime/hle/guest_tls.h (stage 4a). This file adds what that header leaves to the caller:
//   * the one lazy capture of main's .tdata image (guest_tls_main_image),
//   * the memory of a block: osk::host reserve + commit, released with the whole reservation, with rollback when a step fails,
//   * ownership: every live block is registered with its tp and the ThreadType that owns it, so that a release names the right block
//     once and only for its owner,
//   * guest_tls_hook_init / guest_tls_hook_destroy: what sdk's nnmuslThreadPointerInitializer / Destroyer slots call. The Windows runner
//     keeps its own copies in host/boot.cpp (unchanged); the Linux runner binds these (host/guest_tls_hooks.cpp).
// Nothing here touches the host's own thread-local storage (%fs on Linux): a guest tp is a pointer into guest memory kept in the Cpu.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../hle/guest_tls.h"
#include "os.h"
#include "os_host.h"

namespace osk {

// The image of main's .tdata, captured once (lazily, thread safe) from the loaded module: the first call that can read it does the capture,
// every later call returns the same object whatever its arguments are. Until a capture succeeds the answer is null and the next call tries
// again (main not loaded yet). The object is immutable and lives for the process.
const hle::GuestTlsImage *guest_tls_main_image(const uint8_t *main_base, uint64_t main_span);
// The same image if it has been captured, null if not; never captures.
const hle::GuestTlsImage *guest_tls_main_image_if_captured();

// Makes one block for `owner` (the guest ThreadType, or any non-zero key for a thread the loader makes): reserves 0x10000 bytes at a 64 KiB
// aligned address, commits them, zeroes the block and copies `image` to tp + 0x10 (hle::guest_tls_init_block). A null image gives a block
// of zeros (what a thread got before the image existed). Returns tp, or 0 with *err filled when a step failed; on failure nothing stays reserved.
uint64_t guest_tls_block_create(const hle::GuestTlsImage *image, uint64_t owner, host::MemErr *err = nullptr);
// Releases the block whose thread pointer is `tp`. Only a live block, and only for the owner it was made for: anything else (an unknown tp,
// a block already released, another thread's block, tp 0) returns false and releases nothing.
bool guest_tls_block_destroy(uint64_t tp, uint64_t owner);
size_t guest_tls_blocks_live();
// Owner of the live block with this tp, 0 if there is none.
uint64_t guest_tls_block_owner(uint64_t tp);

// nnmuslThreadPointerInitializer: x0 = the ThreadType, c->tpidrro = the thread's Thread Local Region (TLR). Makes the thread's block and stores
// its tp in TLR+0x1f0 (oslay::TLR_TP, an offset of the TLR, not of tp). The caller (os_kernel.cpp host_entry) reads TLR+0x1f0 into tpidr_el0
// afterwards. A TLR that already holds a thread pointer is left alone.
void guest_tls_hook_init(Cpu *c);
// nnmuslThreadPointerDestroyer, after the TLS destructors: x0 = the ThreadType. Releases the block named by TLR+0x1f0 if it is this thread's,
// then clears the TLR word so that a second call finds nothing.
void guest_tls_hook_destroy(Cpu *c);

// Test seam: the three memory operations a block uses. nullptr restores osk::host. Not for production code.
struct GuestTlsMemOps {
    void *(*reserve)(size_t size, size_t align, bool write_watch, host::MemErr *err);
    bool (*commit)(void *addr, size_t size, host::MemErr *err);
    void (*release)(void *addr, size_t size);
};
void guest_tls_set_mem_ops(const GuestTlsMemOps *ops);

}  // namespace osk
