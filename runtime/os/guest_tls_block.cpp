// SPDX-License-Identifier: MIT
// See guest_tls_block.h.
#include "guest_tls_block.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>

#include "os_internal.h"

namespace osk {
namespace {

struct Entry {
    uint8_t *base;    // the reservation (tp - 0x8000)
    uint64_t owner;   // the ThreadType the block was made for
};
std::mutex g_mu;
std::map<uint64_t, Entry> g_live;   // tp -> entry, under g_mu

GuestTlsMemOps g_ops = {host::mem_reserve, host::mem_commit, host::mem_release};   // under g_mu when changed; read without it only by the seam's own tests

// the one captured image: published with release once it is complete, never modified afterwards
std::mutex g_cap_mu;
hle::GuestTlsImage g_image;
std::atomic<const hle::GuestTlsImage *> g_published{nullptr};

}  // namespace

const hle::GuestTlsImage *guest_tls_main_image_if_captured() { return g_published.load(std::memory_order_acquire); }

const hle::GuestTlsImage *guest_tls_main_image(const uint8_t *main_base, uint64_t main_span) {
    if (const hle::GuestTlsImage *p = g_published.load(std::memory_order_acquire)) return p;   // already captured: the arguments are not looked at
    std::lock_guard<std::mutex> lk(g_cap_mu);
    if (const hle::GuestTlsImage *p = g_published.load(std::memory_order_acquire)) return p;   // another thread captured while this one waited
    if (!main_base || main_span > (uint64_t)SIZE_MAX) return nullptr;
    if (hle::GuestTlsImage::capture(main_base, (size_t)main_span, hle::kMainTlsImageVaddr, hle::kMainTlsImageSize, &g_image) != hle::GuestTlsStatus::Ok)
        return nullptr;
    g_published.store(&g_image, std::memory_order_release);
    return &g_image;
}

uint64_t guest_tls_block_create(const hle::GuestTlsImage *image, uint64_t owner, host::MemErr *err) {
    if (!owner) return 0;
    GuestTlsMemOps ops;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ops = g_ops;
    }
    host::MemErr e;
    // aligned to the block size (64 KiB): the Windows allocation granularity the block has always had
    uint8_t *base = (uint8_t *)ops.reserve(hle::kGuestTlsBlockSize, hle::kGuestTlsBlockSize, false, &e);
    if (!base) {
        if (err) *err = e;
        return 0;
    }
    // memory that was only reserved cannot be written: commit before the first byte is touched
    if (!ops.commit(base, hle::kGuestTlsBlockSize, &e)) {
        ops.release(base, hle::kGuestTlsBlockSize);   // the reservation must not outlive a failed commit
        if (err) *err = e;
        return 0;
    }
    uint8_t *tp = nullptr;
    if (image) {
        if (hle::guest_tls_init_block(base, hle::kGuestTlsBlockSize, *image, &tp) != hle::GuestTlsStatus::Ok) {
            ops.release(base, hle::kGuestTlsBlockSize);
            if (err) *err = host::MemErr{};
            return 0;
        }
    } else {
        memset(base, 0, hle::kGuestTlsBlockSize);   // no image: zeros, as before the image existed
        tp = base + hle::kGuestTlsTpOffset;
    }
    try {
        std::lock_guard<std::mutex> lk(g_mu);
        g_live[(uint64_t)(uintptr_t)tp] = Entry{base, owner};
    } catch (...) {   // out of memory for the registry: do not leak the block
        ops.release(base, hle::kGuestTlsBlockSize);
        return 0;
    }
    return (uint64_t)(uintptr_t)tp;
}

bool guest_tls_block_destroy(uint64_t tp, uint64_t owner) {
    GuestTlsMemOps ops;
    uint8_t *base;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_live.find(tp);
        if (!tp || !owner || it == g_live.end() || it->second.owner != owner) return false;   // unknown, already released, or someone else's
        base = it->second.base;
        g_live.erase(it);   // gone from the registry before the memory goes: a racing second release finds nothing
        ops = g_ops;
    }
    ops.release(base, hle::kGuestTlsBlockSize);
    return true;
}

size_t guest_tls_blocks_live() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_live.size();
}

uint64_t guest_tls_block_owner(uint64_t tp) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_live.find(tp);
    return it == g_live.end() ? 0 : it->second.owner;
}

void guest_tls_set_mem_ops(const GuestTlsMemOps *ops) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ops = ops ? *ops : GuestTlsMemOps{host::mem_reserve, host::mem_commit, host::mem_release};
}

void guest_tls_hook_init(Cpu *c) {
    const uint64_t tt = c->x[0], tlr = c->tpidrro;
    if (!tlr) {
        fprintf(stderr, "[os] guest TLS: the thread has no Thread Local Region\n");
        return;
    }
    if (gld<uint64_t>(tlr + oslay::TLR_TP)) {   // not ours to replace: a second block would leak the first
        fprintf(stderr, "[os] guest TLS: TLR+%#x already holds a thread pointer (%#llx), left as it is\n", (unsigned)oslay::TLR_TP,
                (unsigned long long)gld<uint64_t>(tlr + oslay::TLR_TP));
        return;
    }
    const hle::GuestTlsImage *image = guest_tls_main_image_if_captured();
    if (!image) fprintf(stderr, "[os] guest TLS: main's image was never captured, the thread gets a block of zeros (the camera scan will not work)\n");
    host::MemErr e;
    const uint64_t tp = guest_tls_block_create(image, tt, &e);
    if (!tp) fatal("cannot make a guest TLS block (error %lu)", e.code);   // the thread cannot run without its thread pointer
    gst<uint64_t>(tlr + oslay::TLR_TP, tp);
}

void guest_tls_hook_destroy(Cpu *c) {
    const uint64_t tt = c->x[0], tlr = c->tpidrro;
    if (!tlr) return;
    const uint64_t tp = gld<uint64_t>(tlr + oslay::TLR_TP);
    if (!tp) return;   // nothing to release (or already released)
    if (guest_tls_block_destroy(tp, tt)) {
        gst<uint64_t>(tlr + oslay::TLR_TP, 0);
        if (c->tpidr == tp) c->tpidr = 0;   // the thread's own Cpu must not keep a pointer into memory that is gone
    } else fprintf(stderr, "[os] guest TLS: TLR+%#x (%#llx) is not a live block of this thread, nothing released\n", (unsigned)oslay::TLR_TP, (unsigned long long)tp);
}

}  // namespace osk
