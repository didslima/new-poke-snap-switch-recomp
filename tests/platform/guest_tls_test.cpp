// SPDX-License-Identifier: MIT
// Unit tests of the guest TLS module (runtime/hle/guest_tls.h): GuestTlsImage and guest_tls_init_block. Nothing here touches the loader,
// the OS layer, an allocator of guest memory or a thread: every block is a buffer the test owns, between canary bytes that must never change.
//
// Layout facts used (docs/linux-port.md, "Stage 4a"): block 0x10000 bytes, tp = block + 0x8000, image at tp + 0x10. What is NOT assumed:
// where main's .tbss ends, the largest TLS alignment, what the half of the block below tp holds. So the tests check that every byte of
// the block outside the image is zero, and that nothing outside the block moves.
//
// SNAP_MAIN_ELF=<path to the local main.elf> (optional, never committed; read only) also checks the constants against the game's own bytes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "guest_tls.h"

using namespace hle;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// the pinned constants: a change of one of them is a change of the contract and must be made on purpose, with docs/linux-port.md
static_assert(kGuestTlsBlockSize == 0x10000, "block size");
static_assert(kGuestTlsTpOffset == 0x8000, "tp offset");
static_assert(kTlsBlockOffset == 0x10, "image offset from tp");
static_assert(kMainTlsImageVaddr == 0xccb900, "main image offset (2.0.1)");
static_assert(kMainTlsImageSize == 8, "main image size");

// main's image for build 2.0.1: slot word -1, then the hash word 0x00732395
static const uint8_t kImage[8] = {0xff, 0xff, 0xff, 0xff, 0x95, 0x23, 0x73, 0x00};

// A block between two guard zones. The zones and the block start with different patterns, so a write outside the block, and a block left
// untouched, are both visible.
struct Arena {
  static constexpr size_t kGuard = 256;
  alignas(64) uint8_t mem[kGuard + kGuestTlsBlockSize + kGuard];
  explicit Arena(uint8_t fill = 0x5a) {
    memset(mem, 0xA5, kGuard);
    memset(mem + kGuard, fill, kGuestTlsBlockSize);
    memset(mem + kGuard + kGuestTlsBlockSize, 0xA5, kGuard);
  }
  uint8_t* block() { return mem + kGuard; }
  bool guards_intact() const {
    for (size_t i = 0; i < kGuard; i++)
      if (mem[i] != 0xA5 || mem[kGuard + kGuestTlsBlockSize + i] != 0xA5) return false;
    return true;
  }
  bool block_is(uint8_t fill) const {
    for (size_t i = 0; i < kGuestTlsBlockSize; i++)
      if (mem[kGuard + i] != fill) return false;
    return true;
  }
};

static GuestTlsImage known_image() {
  GuestTlsImage img;
  CHECK(GuestTlsImage::capture_bytes(kImage, sizeof kImage, &img) == GuestTlsStatus::Ok);
  return img;
}
static bool image_is_known(const GuestTlsImage& img) { return img.size() == sizeof kImage && memcmp(img.data(), kImage, sizeof kImage) == 0; }

// every byte of the block outside [tp + 0x10, tp + 0x10 + n) is zero
static bool rest_is_zero(const uint8_t* block, size_t n) {
  const size_t b = kGuestTlsTpOffset + kTlsBlockOffset, e = b + n;
  for (size_t i = 0; i < kGuestTlsBlockSize; i++)
    if ((i < b || i >= e) && block[i] != 0) return false;
  return true;
}

// ---- A: the .tdata image is copied exactly, to the right place, and the captured image stays as it was
static void test_copy() {
  const GuestTlsImage img = known_image();
  CHECK(!img.empty() && img.size() == 8);
  Arena a0(0x00), a1(0xff), a2(0x5a), a3(0x11);
  Arena* arenas[] = {&a0, &a1, &a2, &a3};
  for (Arena* a : arenas) {
    uint8_t* tp = nullptr;
    CHECK(guest_tls_init_block(a->block(), kGuestTlsBlockSize, img, &tp) == GuestTlsStatus::Ok);
    CHECK(tp == a->block() + 0x8000);
    CHECK(memcmp(tp + 0x10, kImage, 8) == 0);
    int32_t slot;
    uint32_t hash;
    memcpy(&slot, tp + 0x10, 4);
    memcpy(&hash, tp + 0x14, 4);
    CHECK(slot == -1 && hash == 0x00732395u);   // the physics bounds slot the camera scan needs
    CHECK(memcmp(tp, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);   // the 16-byte TCB stays zero
    CHECK(a->guards_intact());
    // damaging the block does not reach the image
    memset(a->block(), 0x77, kGuestTlsBlockSize);
    CHECK(image_is_known(img));
  }
  // the new function and the old one agree on the image bytes
  Arena legacy(0x00);
  guest_tls_init(legacy.block() + 0x8000, kImage, sizeof kImage);
  Arena fresh(0xcc);
  CHECK(guest_tls_init_block(fresh.block(), kGuestTlsBlockSize, img) == GuestTlsStatus::Ok);
  CHECK(memcmp(legacy.block(), fresh.block(), kGuestTlsBlockSize) == 0);
  // init is repeatable on the same block
  CHECK(guest_tls_init_block(fresh.block(), kGuestTlsBlockSize, img) == GuestTlsStatus::Ok);
  CHECK(memcmp(legacy.block(), fresh.block(), kGuestTlsBlockSize) == 0);
}

// the image of build 2.0.1 ends in a zero byte, so a copy one byte short would look right: also copy images whose every byte is non-zero
static void test_copy_sizes() {
  const size_t max_fit = kGuestTlsBlockSize - kGuestTlsTpOffset - kTlsBlockOffset;   // bytes from tp + 0x10 to the end of the block
  for (size_t n : {(size_t)1, (size_t)2, (size_t)7, (size_t)8, (size_t)9, (size_t)16, (size_t)100, (size_t)4096, max_fit}) {
    std::vector<uint8_t> src(n);
    for (size_t i = 0; i < n; i++) src[i] = (uint8_t)(i * 13 + 1) | 0x80;   // never zero, rarely equal to a neighbour
    GuestTlsImage img;
    CHECK(GuestTlsImage::capture_bytes(src.data(), n, &img) == GuestTlsStatus::Ok);
    CHECK(img.size() == n && memcmp(img.data(), src.data(), n) == 0);
    Arena a(0xC3);
    uint8_t* tp = nullptr;
    CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, img, &tp) == GuestTlsStatus::Ok);
    CHECK(memcmp(tp + 0x10, src.data(), n) == 0);          // every byte, the last one included
    CHECK(rest_is_zero(a.block(), n));                      // and not one byte more
    CHECK(a.guards_intact());
  }
  // one byte more than fits is refused, and the block stays as it was
  std::vector<uint8_t> toolong(max_fit + 1, 0x42);
  GuestTlsImage img;
  CHECK(GuestTlsImage::capture_bytes(toolong.data(), toolong.size(), &img) == GuestTlsStatus::Ok);
  Arena a(0x31);
  CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, img) == GuestTlsStatus::BlockTooSmall);
  CHECK(a.block_is(0x31) && a.guards_intact());
}

// ---- B: the rest of the block is zero, whatever it held, and nothing outside the block changes
static void test_tbss_zero() {
  const GuestTlsImage img = known_image();
  const uint8_t fills[] = {0x00, 0x01, 0x5a, 0x80, 0xff};
  for (uint8_t f : fills) {
    Arena a(f);
    CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, img) == GuestTlsStatus::Ok);
    CHECK(rest_is_zero(a.block(), img.size()));
    CHECK(a.guards_intact());
  }
  // a block with a different non-zero value in every byte (a mask that fills only part of it would be seen)
  Arena ramp;
  for (size_t i = 0; i < kGuestTlsBlockSize; i++) ramp.block()[i] = (uint8_t)(i * 7 + 1) | 1;
  CHECK(guest_tls_init_block(ramp.block(), kGuestTlsBlockSize, img) == GuestTlsStatus::Ok);
  CHECK(rest_is_zero(ramp.block(), img.size()));
  CHECK(ramp.guards_intact());
  // the first and the last byte of the block and the bytes next to the image are zero
  CHECK(ramp.block()[0] == 0 && ramp.block()[kGuestTlsBlockSize - 1] == 0);
  CHECK(ramp.block()[0x8010 + 8] == 0 && ramp.block()[0x8010 - 1] == 0);
  // a smaller block than the default (the checked minimum) is zeroed to its own size only
  alignas(64) uint8_t buf[0x80 + 64];
  memset(buf, 0xA5, sizeof buf);
  CHECK(guest_tls_init_block(buf + 16, 0x80, img, nullptr, 0x20) == GuestTlsStatus::Ok);   // block of 0x80 at buf+16, tp at +0x20
  for (size_t i = 0; i < 16; i++) CHECK(buf[i] == 0xA5);                                   // before the block
  for (size_t i = 16 + 0x80; i < sizeof buf; i++) CHECK(buf[i] == 0xA5);                    // after it
  CHECK(memcmp(buf + 16 + 0x20 + 0x10, kImage, 8) == 0);
  for (size_t i = 0; i < 0x80; i++)
    if (i < 0x30 || i >= 0x38) CHECK(buf[16 + i] == 0);
}

// ---- C: instances do not share anything
static void test_isolation() {
  const GuestTlsImage img = known_image();
  Arena a, b, c;
  uint8_t *tpa = nullptr, *tpb = nullptr, *tpc = nullptr;
  CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, img, &tpa) == GuestTlsStatus::Ok);
  CHECK(guest_tls_init_block(b.block(), kGuestTlsBlockSize, img, &tpb) == GuestTlsStatus::Ok);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, &tpc) == GuestTlsStatus::Ok);
  CHECK(tpa != tpb && tpb != tpc && tpa != tpc);
  CHECK(memcmp(a.block(), b.block(), kGuestTlsBlockSize) == 0 && memcmp(b.block(), c.block(), kGuestTlsBlockSize) == 0);   // same start
  // each thread changes its own slot word (what AddBounds does) and writes its own pattern in .tbss
  const int32_t slots[3] = {0, 1, 2};
  uint8_t* tps[3] = {tpa, tpb, tpc};
  for (int i = 0; i < 3; i++) {
    memcpy(tps[i] + 0x10, &slots[i], 4);
    memset(tps[i] + 0x100, 0x40 + i, 0x200);
  }
  for (int i = 0; i < 3; i++) {
    int32_t s;
    memcpy(&s, tps[i] + 0x10, 4);
    CHECK(s == slots[i]);
    for (size_t k = 0; k < 0x200; k++) CHECK(tps[i][0x100 + k] == (uint8_t)(0x40 + i));
    CHECK(memcmp(tps[i] + 0x14, kImage + 4, 4) == 0);   // the hash word is the image's in all of them
  }
  CHECK(image_is_known(img));   // the image did not follow the changes
  Arena d;
  uint8_t* tpd = nullptr;
  CHECK(guest_tls_init_block(d.block(), kGuestTlsBlockSize, img, &tpd) == GuestTlsStatus::Ok);
  CHECK(memcmp(tpd + 0x10, kImage, 8) == 0 && rest_is_zero(d.block(), 8));   // a later thread starts from the original, not from a sibling
  CHECK(a.guards_intact() && b.guards_intact() && c.guards_intact() && d.guards_intact());
}

// ---- D: the image is what was read at capture time
static void test_capture_once() {
  uint8_t module[0x40];
  memset(module, 0, sizeof module);
  memcpy(module + 0x20, kImage, 8);
  GuestTlsImage img;
  CHECK(GuestTlsImage::capture(module, sizeof module, 0x20, 8, &img) == GuestTlsStatus::Ok);
  CHECK(image_is_known(img));
  memset(module, 0xEE, sizeof module);   // the game later writes over its own TLS source, or the module goes away
  Arena a;
  uint8_t* tp = nullptr;
  CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, img, &tp) == GuestTlsStatus::Ok);
  CHECK(memcmp(tp + 0x10, kImage, 8) == 0);   // the bytes of capture time, not 0xEE
  CHECK(image_is_known(img));
  // a second capture is a second, independent image
  GuestTlsImage img2;
  CHECK(GuestTlsImage::capture(module, sizeof module, 0x20, 8, &img2) == GuestTlsStatus::Ok);
  CHECK(img2.size() == 8 && img2.data()[0] == 0xEE && image_is_known(img));
  CHECK(img.data() != img2.data());   // no storage is shared
  // copying an image copies the bytes
  GuestTlsImage copy = img;
  CHECK(copy.data() != img.data() && image_is_known(copy));
}

// ---- E: invalid input fails with a status and writes nothing
static bool untouched(Arena& a, uint8_t fill) { return a.guards_intact() && a.block_is(fill); }
static void test_errors() {
  const GuestTlsImage img = known_image();
  GuestTlsImage empty;
  Arena a(0x33);

  // null pointers and empty images
  CHECK(guest_tls_init_block(nullptr, kGuestTlsBlockSize, img) == GuestTlsStatus::NullPointer);
  CHECK(guest_tls_init_block(a.block(), kGuestTlsBlockSize, empty) == GuestTlsStatus::EmptyImage);
  CHECK(untouched(a, 0x33));
  GuestTlsImage out = img;   // holds a known image: a failed capture must leave it alone
  CHECK(GuestTlsImage::capture(nullptr, 16, 0, 8, &out) == GuestTlsStatus::NullPointer);
  CHECK(GuestTlsImage::capture(kImage, 8, 0, 8, nullptr) == GuestTlsStatus::NullPointer);
  CHECK(GuestTlsImage::capture_bytes(nullptr, 8, &out) == GuestTlsStatus::NullPointer);
  CHECK(GuestTlsImage::capture_bytes(kImage, 0, &out) == GuestTlsStatus::EmptyImage);
  CHECK(GuestTlsImage::capture(kImage, 8, 0, 0, &out) == GuestTlsStatus::EmptyImage);
  CHECK(image_is_known(out));   // a failed capture leaves the output as it was

  // capture ranges: inside, at the edge, past the edge, overflowing
  uint8_t mod[64] = {};
  CHECK(GuestTlsImage::capture(mod, 64, 56, 8, &out) == GuestTlsStatus::Ok);
  CHECK(out.size() == 8 && out.data()[0] == 0);   // the zero bytes of mod, captured
  out = img;
  CHECK(GuestTlsImage::capture(mod, 64, 57, 8, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, 64, 1, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, 65, 1, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, 0, 65, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, UINT64_MAX, 8, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, 8, SIZE_MAX, &out) == GuestTlsStatus::OutOfRange);
  CHECK(GuestTlsImage::capture(mod, 64, UINT64_MAX - 3, 8, &out) == GuestTlsStatus::OutOfRange);
  CHECK(image_is_known(out));

  // block size: exactly enough, one byte short
  const size_t need = kGuestTlsTpOffset + kTlsBlockOffset + img.size();   // 0x8018
  Arena b(0x44);
  CHECK(guest_tls_init_block(b.block(), need - 1, img) == GuestTlsStatus::BlockTooSmall);
  CHECK(b.block_is(0x44) && b.guards_intact());
  CHECK(guest_tls_init_block(b.block(), need, img) == GuestTlsStatus::Ok);
  CHECK(b.guards_intact());
  for (size_t i = need; i < kGuestTlsBlockSize; i++) CHECK(b.block()[i] == 0x44);   // only `need` bytes belong to the call
  CHECK(guest_tls_init_block(b.block(), 0, img) == GuestTlsStatus::BadTpOffset);    // tp offset is not inside an empty block
  CHECK(guest_tls_init_block(b.block(), 16, img) == GuestTlsStatus::BadTpOffset);

  // tp offsets: before the TCB, at its edge, at the end of the block, past it
  Arena c(0x55);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, 0) == GuestTlsStatus::BadTpOffset);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, 0xf) == GuestTlsStatus::BadTpOffset);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, kGuestTlsBlockSize) == GuestTlsStatus::BadTpOffset);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, kGuestTlsBlockSize + 1) == GuestTlsStatus::BadTpOffset);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, SIZE_MAX) == GuestTlsStatus::BadTpOffset);
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, kGuestTlsBlockSize - 0x10) == GuestTlsStatus::BlockTooSmall);   // no room for the image
  CHECK(guest_tls_init_block(c.block(), kGuestTlsBlockSize, img, nullptr, kGuestTlsBlockSize - 0x18) == GuestTlsStatus::Ok);   // exactly room
  Arena c2(0x55);
  CHECK(guest_tls_init_block(c2.block(), kGuestTlsBlockSize, img, nullptr, 0x10) == GuestTlsStatus::Ok);   // smallest legal offset
  CHECK(memcmp(c2.block() + 0x20, kImage, 8) == 0 && c2.guards_intact());
  CHECK(c.guards_intact());

  // alignment
  Arena d(0x66);
  for (size_t off : {1u, 2u, 4u, 8u, 15u}) {
    CHECK(guest_tls_init_block(d.block() + off, kGuestTlsBlockSize - 64, img) == GuestTlsStatus::Misaligned);
    CHECK(untouched(d, 0x66));
  }
  CHECK(guest_tls_init_block(d.block() + 16, kGuestTlsBlockSize - 64, img) == GuestTlsStatus::Ok);   // 16 is the floor

  // overlap with the image's own bytes: refused before any write. (The block is built over the image's storage; the call must not touch it.)
  std::vector<uint8_t> big(0x100, 0x99);
  GuestTlsImage bigimg;
  CHECK(GuestTlsImage::capture_bytes(big.data(), big.size(), &bigimg) == GuestTlsStatus::Ok);
  uint8_t* inside = const_cast<uint8_t*>(bigimg.data());
  const size_t tp_off = 0x10;
  CHECK((uintptr_t)inside % 16 == 0);
  CHECK(guest_tls_init_block(inside, tp_off + 0x10 + bigimg.size(), bigimg, nullptr, tp_off) == GuestTlsStatus::Overlap);
  for (size_t i = 0; i < bigimg.size(); i++) CHECK(bigimg.data()[i] == 0x99);
}

// ---- G: the values themselves, against the game's bytes, when a local main.elf is given (read only)
struct Elf64Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf64Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
static void test_real_main_elf() {
  const char* path = getenv("SNAP_MAIN_ELF");
  if (!path || !*path) { std::printf("  main.elf: skipped (set SNAP_MAIN_ELF to check the constants against the game's bytes)\n"); return; }
  FILE* f = fopen(path, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  Elf64Ehdr eh;
  CHECK(fread(&eh, sizeof eh, 1, f) == 1 && memcmp(eh.ident, "\177ELF", 4) == 0 && eh.phentsize == sizeof(Elf64Phdr));
  bool has_tls = false, found = false;
  uint8_t got[kMainTlsImageSize] = {};
  for (unsigned i = 0; i < eh.phnum; i++) {
    Elf64Phdr ph;
    fseek(f, (long)(eh.phoff + i * sizeof ph), SEEK_SET);
    if (fread(&ph, sizeof ph, 1, f) != 1) break;
    if (ph.type == 7) has_tls = true;   // PT_TLS
    if (ph.type == 1 && kMainTlsImageVaddr >= ph.vaddr && kMainTlsImageVaddr + kMainTlsImageSize <= ph.vaddr + ph.filesz) {
      fseek(f, (long)(ph.offset + (kMainTlsImageVaddr - ph.vaddr)), SEEK_SET);
      found = fread(got, 1, sizeof got, f) == sizeof got;
    }
  }
  fclose(f);
  CHECK(!has_tls);   // the premise of the whole module: no PT_TLS segment
  CHECK(found && memcmp(got, kImage, sizeof got) == 0);
  GuestTlsImage img;
  CHECK(GuestTlsImage::capture_bytes(got, sizeof got, &img) == GuestTlsStatus::Ok && image_is_known(img));
  std::printf("  main.elf: no PT_TLS, image at +%#llx matches\n", (unsigned long long)kMainTlsImageVaddr);
}

int main() {
  test_copy();
  test_copy_sizes();
  test_tbss_zero();
  test_isolation();
  test_capture_once();
  test_errors();
  test_real_main_elf();
  for (GuestTlsStatus s : {GuestTlsStatus::Ok, GuestTlsStatus::NullPointer, GuestTlsStatus::EmptyImage, GuestTlsStatus::OutOfRange,
                           GuestTlsStatus::BlockTooSmall, GuestTlsStatus::BadTpOffset, GuestTlsStatus::Misaligned, GuestTlsStatus::Overlap})
    CHECK(std::strcmp(guest_tls_status_name(s), "?") != 0);
  if (g_fail) { std::fprintf(stderr, "guest_tls_test: %d of %d checks failed\n", g_fail, g_checks); return 1; }
  std::printf("guest_tls_test: ok (%d checks)\n", g_checks);
  return 0;
}
