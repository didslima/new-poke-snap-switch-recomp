// Host context switch for guest fibers (docs/save-states.md section 2.4). Replaces the Win32 fibers (CreateFiberEx / SwitchToFiber): a
// fiber is a VirtualAlloc'd stack plus a saved stack pointer, so a save state can copy a suspended fiber's stack and a restore can put a
// thread back on the stack it was on. Same idea as Boost.Context fcontext on Windows x64: the switch saves the callee-saved registers,
// xmm6-15, MXCSR and the x87 control word on the old stack, stores rsp, and swaps the three TIB stack fields (StackBase, StackLimit,
// DeallocationStack) so the kernel's guard-page stack growth and __chkstk keep working on the fiber stack.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "os_internal.h"

__asm__(
    ".text\n"
    ".globl snap_ctx_switch\n"
    "snap_ctx_switch:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %rdi\n  pushq %rsi\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $176, %rsp\n"
    "  movups %xmm6, 0(%rsp)\n  movups %xmm7, 16(%rsp)\n  movups %xmm8, 32(%rsp)\n  movups %xmm9, 48(%rsp)\n  movups %xmm10, 64(%rsp)\n"
    "  movups %xmm11, 80(%rsp)\n  movups %xmm12, 96(%rsp)\n  movups %xmm13, 112(%rsp)\n  movups %xmm14, 128(%rsp)\n  movups %xmm15, 144(%rsp)\n"
    "  stmxcsr 160(%rsp)\n  fnstcw 164(%rsp)\n"
    "  movq %gs:8, %rax\n  movq %rax, 8(%rcx)\n"
    "  movq %gs:16, %rax\n  movq %rax, 16(%rcx)\n"
    "  movq %gs:0x1478, %rax\n  movq %rax, 24(%rcx)\n"
    "  movq %rsp, 0(%rcx)\n"
    "  movq 0(%rdx), %rsp\n"
    "  movq 8(%rdx), %rax\n  movq %rax, %gs:8\n"
    "  movq 16(%rdx), %rax\n  movq %rax, %gs:16\n"
    "  movq 24(%rdx), %rax\n  movq %rax, %gs:0x1478\n"
    "  ldmxcsr 160(%rsp)\n  fldcw 164(%rsp)\n"
    "  movups 0(%rsp), %xmm6\n  movups 16(%rsp), %xmm7\n  movups 32(%rsp), %xmm8\n  movups 48(%rsp), %xmm9\n  movups 64(%rsp), %xmm10\n"
    "  movups 80(%rsp), %xmm11\n  movups 96(%rsp), %xmm12\n  movups 112(%rsp), %xmm13\n  movups 128(%rsp), %xmm14\n  movups 144(%rsp), %xmm15\n"
    "  addq $176, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rsi\n  popq %rdi\n  popq %rbx\n  popq %rbp\n"
    "  ret\n"
    ".globl snap_ctx_tramp\n"
    "snap_ctx_tramp:\n"
    "  movq %r12, %rcx\n"
    "  subq $32, %rsp\n"
    "  callq *%r13\n"
    "  int3\n");

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
    const size_t commit = 256 << 10, guard = 4096;
    uint8_t *base = (uint8_t *)VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE);
    if (!base) return nullptr;
    uint8_t *top = base + reserve;
    if (!VirtualAlloc(top - commit, commit, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(top - commit - guard, guard, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    uint8_t *end = top - 0x20;   // 16-aligned: the trampoline starts here
    uint8_t *sp = end - 248;
    memset(sp, 0, 248);
    *(uint32_t *)(sp + 160) = 0x1F80;   // MXCSR default
    *(uint16_t *)(sp + 164) = 0x037F;   // x87 control word default
    *(void **)(sp + 192) = (void *)entry;   // r13
    *(void **)(sp + 200) = arg;             // r12
    *(void **)(sp + 240) = (void *)&snap_ctx_tramp;
    HostCtx *c = new HostCtx;
    c->sp = sp;
    c->base = top;
    c->limit = top - commit;
    c->dealloc = base;
    c->alloc = base;
    c->reserve = reserve;
    c->serial = g_serial++;
    return c;
}

void ctx_free(HostCtx *c) {
    if (!c) return;
    if (c->alloc) VirtualFree(c->alloc, 0, MEM_RELEASE);
    delete c;
}

}  // namespace osk
