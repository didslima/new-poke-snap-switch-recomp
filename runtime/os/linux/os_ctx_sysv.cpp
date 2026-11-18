// SPDX-License-Identifier: MIT
// Host context switch for guest fibers on Linux x86-64 (System V AMD64 ABI). The Win64 version is runtime/os/os_ctx_win64.cpp; both
// implement the contract declared in runtime/os/os_internal.h (HostCtx, ctx_new_thread, ctx_new_fiber, ctx_free, snap_ctx_switch) and
// docs/linux-port.md ("Contract: os_ctx").
//
// A context is a stack plus the stack pointer saved while it is not running. snap_ctx_switch(from, to) pushes the callee-saved state on
// the current stack, stores rsp in from->sp, loads to->sp and pops the state saved there. There is no TIB: nothing here touches the
// thread pointer (%fs), so a context can be resumed on a different host thread and keeps the %fs of the thread it runs on.
//
// Saved frame, lowest address first (72 bytes; `sp` points at its first byte). This is also the initial frame of a new fiber:
//     +0   u32  MXCSR            (new fiber: 0x1F80, all exceptions masked, round to nearest, no FTZ/DAZ)
//     +4   u16  x87 control word (new fiber: 0x037F, extended precision, round to nearest, all exceptions masked)
//     +6   u16  unused (the control area is 16 bytes)
//     +8   u64  unused
//     +16  r15
//     +24  r14
//     +32  r13   (new fiber: the entry function)
//     +40  r12   (new fiber: its argument)
//     +48  rbx
//     +56  rbp   (new fiber: 0, ends frame-pointer walks)
//     +64  return address (new fiber: snap_ctx_tramp)
// SysV callee-saved state is rbx, rbp, r12-r15, rsp, the MXCSR control bits and the x87 control word; nothing else is saved (rdi, rsi and
// xmm0-15 are caller-saved in SysV, unlike on Win64). After `ret` the stack pointer is sp + 72.
//
// Stack alignment of a new fiber. The ABI wants rsp % 16 == 0 immediately before a `call`, so a callee sees rsp % 16 == 8 at its first
// instruction. snap_ctx_tramp is entered by `ret`, not by `call`, with rsp = sp + 72. The frame is placed so that this value is
// 16-aligned (sp % 16 == 8); the trampoline then calls entry(arg) directly. A suspended context has no alignment requirement of its own:
// it resumes at the instruction after its own `call snap_ctx_switch`, whose alignment the compiler established.
//
// Fiber stacks: one anonymous mapping of `reserve` bytes, read/write, committed lazily by the kernel (MAP_NORESERVE), with the lowest page
// inaccessible as a guard. Unlike Win64 there is no guard-page growth: the whole range is usable.
#if !(defined(__linux__) && defined(__x86_64__))
#error "runtime/os/linux/os_ctx_sysv.cpp is the Linux x86-64 System V implementation"
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../os_internal.h"

__asm__(
    ".text\n"
    ".globl snap_ctx_switch\n"
    ".type snap_ctx_switch, @function\n"
    "snap_ctx_switch:\n"            // rdi = HostCtx *from, rsi = HostCtx *to
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $16, %rsp\n"
    "  stmxcsr 0(%rsp)\n"
    "  fnstcw 4(%rsp)\n"
    "  movq %rsp, 0(%rdi)\n"       // from->sp
    "  movq 0(%rsi), %rsp\n"       // to->sp
    "  ldmxcsr 0(%rsp)\n"
    "  fldcw 4(%rsp)\n"
    "  addq $16, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
    "  ret\n"
    ".size snap_ctx_switch, .-snap_ctx_switch\n"
    ".globl snap_ctx_tramp\n"
    ".type snap_ctx_tramp, @function\n"
    "snap_ctx_tramp:\n"             // entered by `ret` with rsp 16-aligned; r13 = entry, r12 = arg
    "  .cfi_startproc\n"
    "  .cfi_undefined rip\n"
    "  movq %r12, %rdi\n"
    "  callq *%r13\n"
    "  ud2\n"                       // entry must not return
    "  .cfi_endproc\n"
    ".size snap_ctx_tramp, .-snap_ctx_tramp\n");

extern "C" void snap_ctx_tramp(void);

namespace osk {

static std::atomic<uint64_t> g_serial{1};

HostCtx *ctx_new_thread() {
    HostCtx *c = new HostCtx;
    memset(c, 0, sizeof *c);
    c->serial = g_serial++;
    return c;   // filled by the first switch away from the thread's own stack
}

HostCtx *ctx_new_fiber(size_t reserve, void (*entry)(void *), void *arg) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (reserve < 4 * page) return nullptr;
    reserve = (reserve + page - 1) & ~(page - 1);
    uint8_t *base = (uint8_t *)mmap(nullptr, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (base == (uint8_t *)MAP_FAILED) return nullptr;
    if (mprotect(base, page, PROT_NONE) != 0) {   // the guard page: the lowest one
        munmap(base, reserve);
        return nullptr;
    }
    uint8_t *top = base + reserve;                // page aligned, hence 16-aligned
    uint8_t *end = top - 64;                      // rsp after the `ret` into the trampoline: 16-aligned, with slack above it
    uint8_t *sp = end - 72;                       // sp % 16 == 8
    memset(sp, 0, 72);
    *(uint32_t *)(sp + 0) = 0x1F80;               // MXCSR default
    *(uint16_t *)(sp + 4) = 0x037F;               // x87 control word default
    *(void **)(sp + 32) = (void *)entry;          // r13
    *(void **)(sp + 40) = arg;                    // r12
    *(void **)(sp + 64) = (void *)&snap_ctx_tramp;
    HostCtx *c = new HostCtx;
    c->sp = sp;
    c->base = top;
    c->limit = base + page;                       // lowest usable address
    c->dealloc = base;
    c->alloc = base;
    c->reserve = reserve;
    c->serial = g_serial++;
    return c;
}

void ctx_free(HostCtx *c) {
    if (!c) return;
    if (c->alloc) munmap(c->alloc, c->reserve);
    delete c;
}

}  // namespace osk
