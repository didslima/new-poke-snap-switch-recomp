// Guest static TLS image (project code, ours).
// The imported ELFs have no PT_TLS, so the runtime hands each guest thread a zeroed TLS block (tp in its middle). main's
// thread-locals are not all zero at start, though: its initialised TLS data (.tdata) is the 8 bytes at main+0xccb900, which the
// NSO to ELF import left inside a 16-byte ".data" section at 0xccb8f8 (8 bytes padding, then the image; nothing references it).
// AArch64 TLS variant 1: the first module's block starts right after the 16-byte TCB, at tp+0x10.
//   tp+0x10 (int32, image ffffffff = -1): the thread's slot in the physics world-bounds list (main+0xcfb1d8). AddBounds
//            (main+0x2a2970) appends a slot when it is negative. Zeroed, every thread "owns" slot 0 of an empty list, the list
//            stays empty, the broadphase grid (main+0x628850) spans a 6 cm box at the origin and no trigger collider ever pairs:
//            the camera scan (Sonar) never reaches a Pokemon, so no "?" and no name (the scan tutorial loops).
//   tp+0x14 (uint32, image 0x00732395): a hash the game overwrites before it reads it.
// The rest of main's TLS (up to tp+0x670) is .tbss and stays zero.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace hle {

constexpr uint64_t kMainTlsImageVaddr = 0xccb900;   // main module offset of the .tdata image (2.0.1, v262144)
constexpr size_t kMainTlsImageSize = 8;
constexpr size_t kTlsBlockOffset = 0x10;            // first module's TLS block, after the 16-byte TCB

// Copy a module's TLS image into a fresh thread's block (tp = the guest thread pointer of a zeroed TLS area).
inline void guest_tls_init(uint8_t *tp, const uint8_t *image, size_t n) {
    if (tp && image && n) memcpy(tp + kTlsBlockOffset, image, n);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Checked API (platform independent: no OS call, no allocation of guest memory, no global state).
//
// guest_tls_init() above copies and trusts its arguments, and relies on the caller's block being zero already (VirtualAlloc gives that).
// The API below separates the three things that are easy to mix up and checks every size, so that a wrong call fails with a status instead
// of writing outside the block:
//
//   GuestTlsImage   what main's .tdata holds: bytes copied once from the loaded module, immutable afterwards. Changing the source
//                   later, or the blocks made from the image, never changes it.
//   GuestTlsImage::capture   reads the bytes out of a loaded module (its base, its span and the image's module offset).
//   guest_tls_init_block   makes one thread's block from the image: the whole block is zeroed, then the image is copied to tp + 0x10.
//
// What is confirmed and what is not (docs/linux-port.md, "Stage 4a"):
//   confirmed   block 0x10000 bytes, tp = block + 0x8000 (host/loader.cpp new_thread, host/boot.cpp tp_init_hook), image at tp + 0x10, the
//               imported ELFs have no PT_TLS, and the 8 bytes at main + 0xccb900 for build 2.0.1 (checked in a local main.elf)
//   not known   how far main's .tbss reaches (the "tp + 0x670" above is a comment, not a measurement), the largest alignment its TLS asks
//               for, and whether the half of the block below tp is used. So nothing here depends on those numbers: every byte of the block
//               except the image is zero, which is what the old zeroed allocation gave and is a superset of any .tbss.
// The block is allocated and released by the caller (stage 4b will use the OS layer's reserve + commit); this API does not touch memory
// it was not handed.

constexpr size_t kGuestTlsBlockSize = 0x10000;      // bytes of one thread's block (host/loader.cpp:412, host/boot.cpp:672)
constexpr size_t kGuestTlsTpOffset = 0x8000;        // tp = block + this
// The least alignment this API accepts for a block's address. A conservative floor of ours (the TCB is 16 bytes and the image sits at
// tp + 0x10), not the proven largest alignment main's TLS needs: real blocks are 64 KiB aligned.
constexpr size_t kGuestTlsMinBlockAlign = 16;

enum class GuestTlsStatus {
  Ok = 0,
  NullPointer,      // a null block, source or output
  EmptyImage,       // the image has no bytes (nothing captured, or a size of 0)
  OutOfRange,       // the image does not lie inside the module it is read from, or the arithmetic overflows
  BlockTooSmall,    // the block cannot hold tp + 0x10 + the image
  BadTpOffset,      // tp offset outside the block, or smaller than the 0x10 bytes of the TCB
  Misaligned,       // the block's address is not a multiple of kGuestTlsMinBlockAlign
  Overlap,          // the block overlaps the memory of the image (it would be destroyed while being copied)
};

inline const char* guest_tls_status_name(GuestTlsStatus s) {
  switch (s) {
    case GuestTlsStatus::Ok: return "ok";
    case GuestTlsStatus::NullPointer: return "null pointer";
    case GuestTlsStatus::EmptyImage: return "empty image";
    case GuestTlsStatus::OutOfRange: return "image outside the module";
    case GuestTlsStatus::BlockTooSmall: return "block too small";
    case GuestTlsStatus::BadTpOffset: return "bad thread pointer offset";
    case GuestTlsStatus::Misaligned: return "block misaligned";
    case GuestTlsStatus::Overlap: return "block overlaps the image";
  }
  return "?";
}

class GuestTlsImage {
 public:
  GuestTlsImage() = default;   // empty: not captured

  // Copies `size` bytes out of a loaded module. `module_base` is where the module is mapped, `module_span` how many bytes of it are
  // readable, `module_offset` where the image starts (main + 0xccb900 for build 2.0.1). On any status but Ok *out is left unchanged.
  static GuestTlsStatus capture(const uint8_t* module_base, size_t module_span, uint64_t module_offset, size_t size, GuestTlsImage* out) {
    if (!module_base || !out) return GuestTlsStatus::NullPointer;
    if (size == 0) return GuestTlsStatus::EmptyImage;
    if (module_offset > module_span || size > module_span - (size_t)module_offset || module_offset > (uint64_t)SIZE_MAX)
      return GuestTlsStatus::OutOfRange;
    return capture_bytes(module_base + (size_t)module_offset, size, out);
  }

  // The same from bytes already in hand (tests, or a caller that read the image itself).
  static GuestTlsStatus capture_bytes(const uint8_t* bytes, size_t size, GuestTlsImage* out) {
    if (!bytes || !out) return GuestTlsStatus::NullPointer;
    if (size == 0) return GuestTlsStatus::EmptyImage;
    GuestTlsImage img;
    img.bytes_.assign(bytes, bytes + size);   // a private copy: the source can change or go away
    *out = std::move(img);
    return GuestTlsStatus::Ok;
  }

  bool empty() const { return bytes_.empty(); }
  size_t size() const { return bytes_.size(); }
  const uint8_t* data() const { return bytes_.data(); }   // read-only on purpose: nothing can edit a captured image

 private:
  std::vector<uint8_t> bytes_;
};

// Makes one thread's TLS block: every byte of [block, block + block_size) becomes zero, then the image is copied to tp + 0x10 where
// tp = block + tp_offset. Nothing outside the block is read or written; on any status but Ok the block is not touched.
// Needs block_size >= tp_offset + 0x10 + image.size() and an address that is a multiple of kGuestTlsMinBlockAlign.
// The defaults are the block of host/loader.cpp (0x10000 bytes, tp at +0x8000). *tp_out (optional) gets tp.
inline GuestTlsStatus guest_tls_init_block(uint8_t* block, size_t block_size, const GuestTlsImage& image, uint8_t** tp_out = nullptr,
                                           size_t tp_offset = kGuestTlsTpOffset) {
  if (!block) return GuestTlsStatus::NullPointer;
  if (image.empty()) return GuestTlsStatus::EmptyImage;
  if (tp_offset < kTlsBlockOffset || tp_offset >= block_size) return GuestTlsStatus::BadTpOffset;
  const size_t room = block_size - tp_offset;   // bytes from tp to the end of the block
  if (room < kTlsBlockOffset || image.size() > room - kTlsBlockOffset) return GuestTlsStatus::BlockTooSmall;
  if ((uintptr_t)block % kGuestTlsMinBlockAlign != 0) return GuestTlsStatus::Misaligned;
  const uintptr_t b0 = (uintptr_t)block, b1 = b0 + block_size, i0 = (uintptr_t)image.data(), i1 = i0 + image.size();
  if (b0 < i1 && i0 < b1) return GuestTlsStatus::Overlap;
  memset(block, 0, block_size);
  memcpy(block + tp_offset + kTlsBlockOffset, image.data(), image.size());
  if (tp_out) *tp_out = block + tp_offset;
  return GuestTlsStatus::Ok;
}

}  // namespace hle
