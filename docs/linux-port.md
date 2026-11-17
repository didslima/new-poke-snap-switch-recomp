# Cross-platform build and the Linux port

Status: **stage 1 of the Linux port (platform-neutral groundwork).** The game does not build or run on Linux yet. This page records what exists, how to extend it, what was tested and what is still missing. Statements are facts that were checked unless a line says otherwise.

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

## Known limits

Observed:

- The game does not build on Linux. The 128 `windows` files need later stages (see below); there is no Linux runner, no Vulkan/SDL3 backend and no OS-layer port in this branch.
- 3 files pass the scanner but do not compile on Linux: `runtime/os/os_sync.cpp`, `os_overrides.cpp` and `cheat_heap.cpp` include `os_internal.h`, whose `constexpr R_OK` collides with the POSIX `R_OK` macro from `<unistd.h>`. They are classified `windows` with that reason (stage 3).
- No Windows toolchain exists in this environment. The Windows build was **not** run: it is unchanged by construction (`host/build_snap.py` was not edited; the only shared files touched are the two above).
- Existing C++ test programs (`tests/hle`, `tests/os`, `tests/audio`, `tests/nvn`) are built by Windows batch files from `windows` sources and were not run here.
- The `linux_nvn_host*` tests (which lost focus under Wayland in the earlier port) do not exist in this branch yet; they belong to the Vulkan stage and must be kept with their coverage when they arrive.

Hypotheses (not checked):

- The two shared-code edits behave the same on the Windows toolchain.
- Classes will shift as stages land; `windows` is expected to shrink.

## What the next stages depend on

| Stage | Needs from this one |
|---|---|
| 3 OS layer (`os_ctx`, `os_state`) | `runtime/os/*` entries to move from `windows` once ported; the `R_OK` collision |
| 4 TLS | `host/loader.cpp` (`windows`) |
| 5 Linux runner | A `linux` class for the new files |
| 7 Vulkan backend | `runtime/nvn/*` (most are `windows`: `VK_USE_PLATFORM_WIN32_KHR` comes in through `nvn_internal.h`) |
