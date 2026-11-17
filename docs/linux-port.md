# Cross-platform build and the Linux port

Status: **stage 2 of the Linux port (neutral OS interfaces and contracts for stages 3 and 4).** The game does not build or run on Linux yet. This page records what exists, how to extend it, what was tested and what is still missing. Statements are facts that were checked unless a line says otherwise.

## Goal

Keep one source tree for the Windows game (built by `host/build_snap.py`, unchanged) and add a native Linux backend in layers. This stage does not add any Linux code. It makes the split between shared, Windows and Linux code explicit and checked, so later stages can move a file from "Windows" to "shared" (or add a Linux sibling) one reviewable step at a time.

## Source classes

Every C/C++ file under `host/` and `runtime/` (159 files) is listed once in `cmake/source_manifest.txt`:

| Class | Meaning | Files now |
|---|---|---|
| `shared` | Portable as written: no Windows header, Win32 API or MSVC-only construct, directly or through the repo headers it includes, **and** it compiles with clang on Linux | 26 |
| `windows` | Windows-only today. The comment on the line says what the scanner found first. When a Linux implementation lands, the file moves to `shared` (if its branches are complete) or gets a `linux` sibling | 128 |
| `linux` | Linux-only. None yet | 0 |
| `offline` | Not compiled into the game by any target: spikes, unit-test sources, reference code, a file that another file `#include`s | 5 |

How a file is judged (`scripts/source_classes.py`): it reads the file and every repo header it includes, ignoring comments, and looks for Windows-only headers (`<windows.h>`, `<d3d11.h>`, ...), Win32 and MSVC names (`VirtualAlloc`, `HANDLE`, `__declspec`, `_MSC_VER`, `#pragma comment`, ...) and the `_WIN32` / `VK_USE_PLATFORM_WIN32_KHR` switches. A file with a `_WIN32` branch is `windows` even when the branch is guarded, because the guard means the other side does not exist yet. Vendored code under `third_party/` is not scanned (its `_WIN32` branches are the library's own portability code); its Windows backends (`imgui_impl_win32.cpp`, `imgui_impl_dx11.cpp`) are classified by name. One header is a reviewed platform switch with a working other side: `runtime/simd.h` (`SIMD_SELECTANY`); the list is `_REVIEWED_SWITCHES` in the script. Add an entry only after reading both sides.

The scanner is a filter, not a proof. A `shared` file must also compile on Linux, and the build below does that.

### Adding or moving a file

1. Add the file, then run `python3 -I scripts/source_classes.py`. It lists unclassified files with a suggested class, and complains about stale entries.
2. Put the line `<class> <path>` in `cmake/source_manifest.txt`, with `# why` for anything that is not `shared`.
3. For `shared`, build it: configure as below and `cmake --build build/cmake-stage1`. The scanner may suggest `shared` for a file that still does not compile on Linux (for example `runtime/os/os_sync.cpp`, see below). The compile decides.
4. Files that `host/build_snap.py` compiles for Windows must stay `shared` or `windows`. The test enforces it.

## CMake

`CMakeLists.txt` + `cmake/sources.cmake`. There is no glob of sources anywhere: the manifest is the only list.

- `snap_load_source_manifest()` reads the manifest, rejects unknown classes, duplicates and entries whose file is missing, and fails the configure step when a `.cpp`/`.c` under `host/` or `runtime/` is not in it. (A glob is used there only to detect unlisted files, never to select sources.)
- `snap_select_sources(windows|linux out)` returns `shared` + that platform's own list. Linux never receives a `windows` or `offline` file.
- On Linux the project builds `snap_portable_objects`: every `shared` and `linux` source, with the settings of `host/build_snap.py` (`-march=x86-64-v3 -fno-strict-aliasing -w`, C++20). Warnings are off like in the Windows build; turning them on is future work.
- clang is required (the runtime uses clang builtins). Configure inside the `snap-recomp-linux` distrobox:

```
cmake -S . -B build/cmake-stage1 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/cmake-stage1 -j
ctest --test-dir build/cmake-stage1 --output-on-failure
```

The Windows list is exposed (`SNAP_SOURCES_WINDOWS`) for tooling; the Windows game is still built only by `host/build_snap.py`.

## Tests (stage 1)

| ctest name | What it checks | Needs |
|---|---|---|
| `source_classification` | `tests/recomp/test_source_classification.py`: every tracked source is classified, no stale entries, no `shared` file with Windows-only content, everything `host/build_snap.py` compiles is `shared` or `windows`; plus the rules and the scanner on synthetic data | Python, git |
| `source_selection` | `cmake/tests/check_selection.cmake`: lists and per-platform selection on a fixture, the Linux selection never holds a `windows`/`offline` file, the loader rejects unknown classes, duplicates, missing files and unclassified files | CMake |
| `portable_sources_compile` | Builds `snap_portable_objects` (the 26 shared sources) | clang, Vulkan headers |
| `json_lite` | `tests/platform/json_lite_test.cpp`: `hle::Json` parse/query/build/copy/move (also built by hand with `-std=c++17` and `-std=c++20`) | clang |

Results of the run on this branch are in the commit message and the stage report.

## Changes to shared code

Two small, behaviour-neutral edits, both needed to make real shared files compile with libstdc++/clang on Linux:

- `runtime/hle/json_lite.h`: `hle::Json` keeps its members in `std::vector<Member>` with a named `Member {first, second}` instead of `std::vector<std::pair<std::string, Json>>`, because libstdc++ rejects a `vector<pair<..., Json>>` member while `Json` is incomplete. The `.first`/`.second` names, the API and the insertion order are unchanged; the file is built as C++17 by `host/` sources and as C++20 by `runtime/` sources, and the test covers both standards. Impact on Windows: none expected (MSVC's library accepted the old shape and accepts this one) but **not verified on Windows**.
- `runtime/simd.h`: the two `__declspec(selectany)` globals use a `SIMD_SELECTANY` macro, which is exactly `__declspec(selectany)` when `_WIN32` is defined and `__attribute__((weak))` elsewhere. Windows still gets the same text it had.

Line endings: both files are LF, and the edits keep that. New files are LF.

## Stage 2: neutral OS interfaces

Stage 2 adds no Linux implementation. It removes one compile error from the shared OS interface, pins what the interface promises with tests, and writes down what stages 3 and 4 have to provide. Sections marked **Contract** describe code that does not exist yet.

### The OS and platform layers today (facts, read from the tree at `555a53e`)

There is no `runtime/platform/` directory and no platform abstraction in the upstream code: the runtime calls Win32 directly. Stage 2 did not introduce one. Where a later stage needs a seam it should follow the shape already used in the upstream tree (a shared header with the contract, one implementation file per platform, the shared `.cpp` calling only the contract).

| Part | Responsibility | Windows dependencies | Shared interface | Status for Linux |
|---|---|---|---|---|
| `runtime/os/os.h`, `os_layout.h`, `perf.h` | Public C interface of the nn::os layer (threads, events, SVCs, override table); guest object layouts | none | consumed by the loader, HLE, tests | portable (scanner clean) |
| `runtime/os/os_internal.h` | Scheduler types (`Thread`, `Fiber`, `HostCtx`), result codes, keyed wait queues | none (comments mention the TIB and `VirtualAlloc`) | internal to `runtime/os` | portable; **`R_OK` fixed in stage 2** |
| `os_sync.cpp`, `os_overrides.cpp`, `cheat_heap.cpp`, `abi.cpp` | Guest mutex/condvar/event/semaphore emulation on the scheduler lock, override table lookup, heap base for cheats | none | `osk::` | **shared**, compile on Linux |
| `os_kernel.cpp` | Host thread start (`_beginthreadex`, `CREATE_SUSPENDED`, reserved stack), `SetThreadPriority`, `SetThreadDescription`, `SwitchToThread`, `QueryPerformanceCounter`, `timeBeginPeriod`, `Sleep`, TLR/Cpu allocation (`_aligned_malloc`) | yes, about 20 API names | `os_init`, `os_adopt_main_thread`, `call_guest` | `windows`, stage 3 |
| `os_memory.cpp` | Guest virtual memory: `VirtualAlloc` reserve/commit/decommit/release, `VirtualQuery`, `GetWriteWatch` flag, `VirtualAlloc2` aligned reserve, `GetSystemInfo` | yes (about 19 names) | `mem_get_info`, svc memory calls | `windows`, stage 3 |
| `os_thread.cpp` | Threads, TLS slots, fibers (`do_switch`, `fiber_proc`), time; calls `ctx_*` and `snap_ctx_switch` | `SwitchToThread`, `Sleep`, `ExitProcess`, `GetCurrentProcess`, `GetLastError` | `HostCtx` contract below | `windows`, stage 3 |
| `os_ctx.cpp` | Host context switch (Win64 assembly, TIB, `VirtualAlloc` stacks) | yes | `ctx_new_thread`, `ctx_new_fiber`, `ctx_free`, `snap_ctx_switch` | `windows`; Linux sibling in stage 3 |
| `os_state.cpp` | Save states (drain, copy, restore) | `dbghelp`, `VirtualQuery`, `VirtualAlloc` | `state_*` hooks called by the kernel | `windows`; not part of the Linux first version (see below) |
| `perf.cpp`, `hostprof.cpp`, `sampler.cpp` | Profilers | `OpenThread`, `GetThreadContext`, toolhelp, `psapi` | `perf.h` | `windows`, later |
| `runtime/hle/*` | Services, input, settings, save files | `CreateFileW`, known folders, DLL loading; `guest_tls.h`, `access.cpp`, `btn_icons.cpp`, `freecam.cpp`, `zip_read.cpp`, `services_net.cpp` are already shared | `hle_config.h`, `settings.h` | 5 shared, 14 `windows` (stage 5 and later) |
| `runtime/audio/*` | Audio engine and the WASAPI backend | `audio_wasapi.cpp` (COM), `audio_out.h` exports with `__declspec` | `audio_out.h` | `windows` |
| `runtime/ui/*` | ImGui overlay, options, pickers | `HWND`, shell, `imgui_impl_win32`; 6 files are shared | `ui_present.h` | mostly `windows` |
| `host/*` | Loader, boot, hooks, mods | everything under `windows` | `loader.h` (`call_guest`, `init_guest_tls`) | stages 4 and 5 |

What Linux needs from this layer, in the order of the plan: an OS-layer implementation of the five Win32 services above (stage 3), the TLS contract (stage 4) and a runner (stage 5). What stays out of the first Linux version: save states (`os_state.cpp`; the kernel calls `state_gate_wait`, `state_gate_acq`, `state_kick_cores`, `state_time_get` and `state_init`, so a Linux build needs either the real port or an explicit decision about those five calls in stage 3; **a success-returning stub is not acceptable**), profilers and the HLE/UI parts that depend on the shell.

### Change: `osk::R_OK` is now `osk::R_SUCCESS`

Cause (checked by compiling): `runtime/os/os_internal.h` declared `constexpr uint32_t R_OK = 0, ...` in `namespace osk`. `<unistd.h>` defines `R_OK` as the macro `4` (the `access()` flag); here it arrives through `os_internal.h`'s own first include: `<atomic>` -> `bits/atomic_base.h` -> `bits/atomic_wait.h` -> `<unistd.h>` (libstdc++ 15, clang 21 `-H`), and `-E -dD` shows `#define R_OK 4` before the declaration (preprocessed line 50458 against 165778). The declaration became `constexpr uint32_t 4 = 0` and `clang` stopped with `expected unqualified-id` in `os_sync.cpp`, `os_overrides.cpp` and `cheat_heap.cpp`.

What `R_OK` was: the Horizon `ResultSuccess` value (0) that OS functions return in `x0` (`c->x[0] = R_OK`) and that the wait helpers return when woken (`cv_wait` returns `R_OK` or `R_TIMEOUT`). It is an internal constant, not a status of the host.

Decision: rename the identifier, keep the value. Alternatives rejected: `#undef R_OK` (silently changes `access()` users that come after the header), a `#define` of our own (would break the POSIX headers), an `osk::` qualifier (does not help: the macro is expanded before name lookup). Nothing else collides on Linux: `os_posix_header` includes the POSIX headers first and compiles with all the other names (`R_TIMEOUT`, `R_INVALID_*`, the `Wait` and `KeyKind` enumerators, `NS_INFINITE`). Not checked: the Win32 headers (no Windows toolchain here); `R_SUCCESS` is not a Win32 name that is known to this author, and `R_OK` is not defined by the MSVC runtime headers, which is why the old name compiled there.

Scope of the edit: 35 lines in `os_internal.h`, `os_kernel.cpp`, `os_memory.cpp`, `os_sync.cpp`; no other file in the repository uses the name (searched `*.cpp *.h *.c *.py *.md *.txt *.bat *.ps1`). `os_sync.cpp` keeps its CRLF line endings. Windows: the rename is a pure token change inside `osk`; on Windows the identifier did not collide before and does not now.

Covered by tests: `os_posix_header` (compiles `os_internal.h` after the POSIX headers and pins `R_SUCCESS == 0` and every other result value), `os_neutral_interfaces` (no identifier in `runtime/os` spelled like `R_OK/W_OK/X_OK/F_OK`; the interface headers scan clean), `portable_sources_compile` (the three files now build).

### Contract: `os_ctx` (host context switch), for stage 3

Where it is used (checked): `os_thread.cpp` `do_switch` calls `snap_ctx_switch(from->host, to->host)`; `ensure_own` creates the context of a thread's own stack with `ctx_new_thread()`, `do_switch` creates a fiber's with `ctx_new_fiber(g_cfg.fiber_host_stack, fiber_proc, to)`; `ctx_free` runs at `InitializeFiber` of a re-used fiber, `FinalizeFiber` and thread exit; `os_state.cpp` reads `HostCtx::serial` and `sp`.

Interface that must stay as it is (also asserted by `os_posix_header`): `struct HostCtx { void *sp; void *base; void *limit; void *dealloc; void *alloc; size_t reserve; uint64_t serial; }` with `sp` at offset 0, `HostCtx *ctx_new_thread()`, `HostCtx *ctx_new_fiber(size_t reserve, void (*entry)(void *), void *arg)`, `void ctx_free(HostCtx *)`, `extern "C" void snap_ctx_switch(HostCtx *from, HostCtx *to)`. The Win64 switch reads `base`, `limit`, `dealloc` as the TIB stack fields at +8/+16/+24; a SysV implementation has no TIB, so those three fields are free for its own use (for example the mapping base and size). Changing the layout is only justified if stage 3 shows it is needed; nothing requires it today.

Win64 implementation, for reference (`os_ctx.cpp`): pushes `rbp rbx rdi rsi r12-r15`, reserves 176 bytes for `xmm6-xmm15`, `MXCSR` (+160) and the x87 control word (+164), swaps `gs:8`, `gs:16`, `gs:0x1478`, stores `rsp` in `from->sp`, loads `to->sp` and restores in reverse order. A new fiber gets a 248-byte frame: `MXCSR = 0x1F80`, x87 CW `0x037F`, `r13 = entry`, `r12 = arg`, return address `snap_ctx_tramp`, which moves `r12` into the first argument register and calls `r13`; `int3` follows (entry must not return). The stack is `VirtualAlloc`'d: `reserve` bytes reserved, 256 KiB committed at the top, one `PAGE_GUARD` page below it so Windows grows the stack on demand.

What the SysV implementation has to preserve (derived from the System V AMD64 ABI and from the callers; the assembly itself is not written):

- Callee-saved registers: `rbx`, `rbp`, `r12-r15`, and `rsp`. `rdi` and `rsi` are caller-saved in SysV (they are callee-saved on Win64) and `xmm6-xmm15` are caller-saved (callee-saved on Win64), so they need not be saved; do not copy the Win64 frame size.
- Control state: the `MXCSR` control bits and the x87 control word are callee-saved in SysV. Each context keeps its own, because the runtime mirrors the guest FPCR into `MXCSR` (`rt_fpcr_sync`: FTZ/DAZ, round to nearest) and the comment in `cpu.h` says fibers keep their own `MXCSR`. Initial values for a new fiber: `0x1F80` and `0x037F`.
- Arguments: first argument in `rdi` (Win64: `rcx`), second in `rsi` (`rdx`); `snap_ctx_tramp` must pass `arg` in `rdi`.
- Stack alignment: `rsp` is 16-byte aligned immediately before the `call` that enters `entry`, so that `entry` sees `rsp % 16 == 8` at its first instruction; the existing frame is built so that the trampoline starts on a 16-aligned address.
- Resume on another host thread is a requirement (`do_switch` says "resumed, possibly on another host thread"). A context must not hold anything bound to the thread it first ran on. The thread pointer `fs` is per host thread and must **not** be swapped. Consequence for the C++ code around the switch: a `thread_local` address computed before `snap_ctx_switch` may be reused by the compiler after it; the code already reloads it (`self_fresh()` is `noinline` with a compiler barrier for this reason). Any new `thread_local` access that spans a switch has the same hazard.
- Stack memory: `ctx_new_fiber` must provide a usable, 16-aligned stack of `reserve` bytes (default 64 MiB per fiber, 256 MiB per guest thread in `OsConfig`), committed lazily, with an inaccessible guard page below it. Hypothesis, not checked: `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` plus `mprotect(PROT_NONE)` on the lowest page matches `VirtualAlloc` reserve/commit/guard closely enough; overcommit settings of the host decide whether 64 MiB x many fibers is acceptable.
- Not covered by `snap_ctx_switch`: the guest registers are in `Cpu`, not on the host stack, so nothing of the guest's state is saved by this function.
- Hypothesis, not checked: Linux user-space shadow stacks (CET) are opt-in and not enabled for ordinary binaries, so a hand-written switch is not expected to hit them; this needs a check on the target kernel.
- Save states (`os_state.cpp`) depend on `serial` and on the saved `sp` being readable. They are not part of the first Linux version.

Tests required in stage 3 (not written yet): many fibers switched across several threads with checks of the callee-saved registers, `MXCSR`, the x87 control word and stack alignment; a fiber that is resumed on a different thread than the one it started on; `ctx_free` of a context that never ran.

### Contract: TLS and `.tdata`, for stage 4

Two different things are called TLS here.

1. **Host TLS**: C++ `thread_local` objects: `t_self` in `os_kernel.cpp` (the scheduler's `Thread`), `loader::g_tls_cpu` in `host/loader.cpp` (the `Cpu` of the last host call, used by crash reports). No change is needed for them on Linux except the fiber hazard above.
2. **Guest TLS**: the AArch64 thread pointer the game's code uses (`tpidr_el0`). The imported ELFs have no `PT_TLS` segment (comment in `host/loader.cpp`, `host/boot.cpp`), so the host builds the block.

Guest TLS layout per guest thread (checked in `host/loader.cpp::new_thread`, `host/boot.cpp::tp_init_hook`, `runtime/os/os_layout.h`, `runtime/os/os_kernel.cpp::host_entry`):

- A zeroed, read/write block of 0x10000 bytes; the thread pointer `tp` is the block address plus 0x8000.
- A 0x200-byte Thread Local Region (`TLR_SIZE`, aligned to its size) that `Cpu::tpidrro` points at: `TLR+0x1f0` (`TLR_TP`) holds `tp`, `TLR+0x1f8` (`TLR_THREAD`) holds the guest `ThreadType*`.
- `c->tpidr = tp`.
- `.tdata`: the game's `main` has an 8-byte initialised TLS image at `main + 0xccb900` (`hle::kMainTlsImageVaddr`, `kMainTlsImageSize`, valid for build 2.0.1, v262144); it belongs at `tp + 0x10` (`kTlsBlockOffset`: first module's block after the 16-byte TCB). `.tbss` is zero. `hle::guest_tls_init(tp, image, n)` in `runtime/hle/guest_tls.h` does the copy and only needs `<cstring>`. Why it matters: without the image the physics bounds slot is wrong and the camera scan never finds a Pokemon (comment in `guest_tls.h`).
- `loader::init_guest_tls(tp)` (`host/loader.cpp`) reads the image once, from the loaded `main` bytes, before any guest code runs (the guest later modifies its TLS), and traces a warning when the slot word is not `-1` (a build other than 2.0.1).

Where the block is created (two sites, both call `init_guest_tls`):

1. `host/loader.cpp::new_thread`: the adopted main thread. Allocates stack, TLS block, TLR, `ThreadType` with `VirtualAlloc`, then `init_guest_tls(c->tpidr)`.
2. Every thread created by the game: `host/boot.cpp::tp_init_hook` is installed as the guest function behind sdk's `nnmuslThreadPointerInitializer`. `os_kernel.cpp::host_entry` calls it through `OsConfig::tp_init_var` with `x0 = ThreadType`; the hook allocates the block, calls `init_guest_tls`, writes `TLR[0x1f0]`; `host_entry` then sets `c->tpidr = TLR[0x1f0]`. `tp_destroy_hook` frees `tp - 0x8000` when the thread ends (`tp_destroy_var`, after the TLS destructors).

What stage 4 has to provide, without changing these semantics:

- A zero-filled, read/write 0x10000-byte allocation per guest thread and its release at thread exit, from the OS layer's memory primitive (no `VirtualAlloc`/`VirtualFree`).
- `init_guest_tls` reachable from the Linux runner and from the thread-start hook, after `g_main.base` and `g_main.span` are known and before the first guest instruction of every thread.
- The order inside `host_entry` unchanged: `tp_init` hook, then `c->tpidr = TLR[0x1f0]`, then the entry.
- The build check (`-1` slot word) kept.
- Not to be changed: the offsets above, the 2.0.1 constants, `TLR` layout.

Test already existing upstream and to be reused in stage 4: the TLS tests in `tests/hle/hle_tests.cpp` (`guest_tls_init`). They are Windows-batch tests and were not run here.

### Memory and scheduler contracts noted for stage 3 (not implemented)

- `os_memory.cpp` needs a platform memory primitive with: reserve (no access), commit (read/write), decommit (back to no access), release, query of the region state, an aligned reserve (the Windows path uses `VirtualAlloc2` when present, otherwise reserve-release-reserve with up to 16 tries), and the write-watch flag that can only be set at reserve time. The write watch has a consumer outside the OS layer: `runtime/nvn/nvn_vk.cpp::SyncPoolRange` uses `GetWriteWatch` to copy only the pages the game wrote (`SNAP_SYNC_FULL=1` is the existing full-copy fallback). A Linux replacement for write watch (soft-dirty bits, `userfaultfd` write-protect or full copies) is a stage 3 decision, not made here. Guest addresses are host addresses, so reservations must land at the addresses the guest was told.
- `os_kernel.cpp` needs: a host thread with a reserved stack size (`STACK_SIZE_PARAM_IS_A_RESERVATION`, default 256 MiB), created suspended and resumed after the bookkeeping, a thread priority hint, a thread name, a yield, a monotonic counter and its frequency, a sleep, and a timer-resolution request. None of these needs a change in the scheduler design; the scheduler lock and wait queues (`os_sync.cpp`) are plain C++ and are already shared.
- The scheduler's behaviour is not changed by stage 2. Any difference introduced by Linux timers or priorities has to come with tests in stage 3.

### Tests of stage 2

| ctest name | What it checks | Needs |
|---|---|---|
| `os_posix_header` | `os_internal.h` compiled after the POSIX headers (macro collision regression); the Horizon result values; the `HostCtx` offsets | clang, no GPU |
| `os_neutral_interfaces` | `tests/platform/test_os_neutral.py`: the five interface headers scan clean of Windows content; no identifier in `runtime/os` spelled like a POSIX access macro; `R_SUCCESS` declared once with value 0 | Python |
| `source_classification`, `source_selection`, `json_lite`, `portable_sources_compile` | Stage 1 tests; the manifest now lists `cheat_heap.cpp`, `os_overrides.cpp` and `os_sync.cpp` as `shared` (29 shared files compiled) | as in stage 1 |

Not run, pending CI or a maintainer with a Windows toolchain: the Windows build (`host/build_snap.py`), `tests/os/os_test.cpp` and the other batch-built suites. The rename touches `os_kernel.cpp` and `os_memory.cpp`, which only the Windows build compiles, so their compile on Windows is **unverified** (the change is a token rename; both files fail to compile on Linux only because of `<windows.h>`, which was checked).

### Left for the next stages

- Stage 3: `os_ctx_sysv` implementation and tests, the Linux memory/thread/timer primitives for `os_memory.cpp`, `os_kernel.cpp`, `os_thread.cpp`, the decision for the `state_*` calls, and moving those files to `shared` or `linux` in the manifest.
- Stage 4: the TLS block allocation and the `.tdata` copy on Linux, as specified above.
- Stage 5: the Linux runner.
- Not designed yet: write-watch replacement, shadow-stack behaviour of the target kernel, overcommit limits for fiber stacks.

## Known limits

Observed:

- The game does not build on Linux. The 128 `windows` files need later stages (see below); there is no Linux runner, no Vulkan/SDL3 backend and no OS-layer port in this branch.
- (Stage 1 limit, fixed in stage 2) `runtime/os/os_sync.cpp`, `os_overrides.cpp` and `cheat_heap.cpp` did not compile on Linux because `osk::R_OK` collided with the POSIX `R_OK` macro. See "Stage 2" below; the three files are `shared` now.
- No Windows toolchain exists in this environment. The Windows build was **not** run: it is unchanged by construction (`host/build_snap.py` was not edited; the only shared files touched are the two above).
- Existing C++ test programs (`tests/hle`, `tests/os`, `tests/audio`, `tests/nvn`) are built by Windows batch files from `windows` sources and were not run here.
- The `linux_nvn_host*` tests (which lost focus under Wayland in the earlier port) do not exist in this branch yet; they belong to the Vulkan stage and must be kept with their coverage when they arrive.

Hypotheses (not checked):

- The two shared-code edits behave the same on the Windows toolchain.
- Classes will shift as stages land; `windows` is expected to shrink.

## What the next stages depend on

| Stage | Needs from this one |
|---|---|
| 3 OS layer (`os_ctx`, `os_state`) | `runtime/os/*` entries to move from `windows` once ported; the contracts in "Stage 2" (`os_ctx`, memory, scheduler threads) |
| 4 TLS | `host/loader.cpp` (`windows`); the TLS contract in "Stage 2" |
| 5 Linux runner | A `linux` class for the new files |
| 7 Vulkan backend | `runtime/nvn/*` (most are `windows`: `VK_USE_PLATFORM_WIN32_KHR` comes in through `nvn_internal.h`) |
