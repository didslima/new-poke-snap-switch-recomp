# Cross-platform build scaffolding (docs/linux-port.md).
#
# The Windows game is built by host/build_snap.py; nothing here replaces it. This project exists so that the sources that are already
# portable can be compiled on Linux, and so that the shared / Windows / Linux split of the sources is explicit and checked.
# The game itself does not build on Linux yet: the stages after this one add the OS layer, the runner and the Vulkan backend.
cmake_minimum_required(VERSION 3.20)
project(snap_recomp LANGUAGES C CXX)

# The runtime uses clang builtins (__builtin_bitreverse64, ...) and clang's C-in-C++ spellings; the Windows build is clang too.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "clang is required (configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++); found ${CMAKE_C_COMPILER_ID}/${CMAKE_CXX_COMPILER_ID}")
endif()

include(CTest)
include(cmake/sources.cmake)
snap_load_source_manifest("${PROJECT_SOURCE_DIR}/cmake/source_manifest.txt")
snap_select_sources(linux SNAP_LINUX_SOURCES)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(UNIX AND NOT APPLE)
  find_package(Vulkan REQUIRED)   # headers only: runtime/nvn includes <vulkan/vulkan.h>

  # Compiles every source the manifest calls shared or linux with the settings of host/build_snap.py (-march=x86-64-v3, no
  # strict aliasing, warnings off as there). A shared file that stops compiling on Linux fails this target.
  add_library(snap_portable_objects OBJECT ${SNAP_LINUX_SOURCES})
  target_include_directories(snap_portable_objects PRIVATE
    runtime host runtime/nvn runtime/ui runtime/ui/third_party/imgui runtime/hle runtime/fx/ocean_fft)
  target_compile_features(snap_portable_objects PRIVATE cxx_std_20)
  target_compile_options(snap_portable_objects PRIVATE -w -march=x86-64-v3 -fno-strict-aliasing)
  target_link_libraries(snap_portable_objects PRIVATE Vulkan::Headers)
  set_target_properties(snap_portable_objects PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
  set_source_files_properties(runtime/fma.c PROPERTIES COMPILE_OPTIONS "-mfma")

  add_test(NAME portable_sources_compile
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target snap_portable_objects)

  add_executable(snap_json_lite_test tests/platform/json_lite_test.cpp)
  target_include_directories(snap_json_lite_test PRIVATE runtime/hle)
  target_compile_features(snap_json_lite_test PRIVATE cxx_std_20)
  add_test(NAME json_lite COMMAND snap_json_lite_test)

  # The neutral OS interface with the POSIX headers included first (the R_OK macro collision of stage 2) and the HostCtx layout contract.
  add_executable(snap_os_posix_header_test tests/platform/os_posix_header_test.cpp)
  target_include_directories(snap_os_posix_header_test PRIVATE runtime)
  target_compile_features(snap_os_posix_header_test PRIVATE cxx_std_20)
  add_test(NAME os_posix_header COMMAND snap_os_posix_header_test)

  # Stage 3, part A: the System V context switch, tested alone (no scheduler, no guest, no GPU). The source is named here, not globbed.
  find_package(Threads REQUIRED)
  add_executable(snap_ctx_sysv_test tests/platform/ctx_sysv_test.cpp runtime/os/linux/os_ctx_sysv.cpp)
  target_include_directories(snap_ctx_sysv_test PRIVATE runtime runtime/os)
  target_compile_features(snap_ctx_sysv_test PRIVATE cxx_std_20)
  target_compile_options(snap_ctx_sysv_test PRIVATE -O2 -g -fno-omit-frame-pointer -fno-strict-aliasing)
  target_link_libraries(snap_ctx_sysv_test PRIVATE Threads::Threads)
  add_test(NAME ctx_sysv COMMAND snap_ctx_sysv_test)
  add_test(NAME ctx_sysv_repeat COMMAND snap_ctx_sysv_test --repeat 30)
  set_tests_properties(ctx_sysv ctx_sysv_repeat PROPERTIES TIMEOUT 300)

  # Stage 3, part B: the OS layer on the POSIX host primitives and the System V context switch. The OS-layer sources are the manifest's
  # shared and linux files under runtime/os (nothing is globbed), minus the two that no OS test needs (the ABI table and the cheat heap).
  set(SNAP_OS_LAYER_SOURCES ${SNAP_LINUX_SOURCES})
  list(FILTER SNAP_OS_LAYER_SOURCES INCLUDE REGEX "^runtime/os/")
  list(FILTER SNAP_OS_LAYER_SOURCES EXCLUDE REGEX "/(abi|cheat_heap)\\.cpp$")
  function(snap_os_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE runtime runtime/os)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -O2 -g -fno-strict-aliasing -march=x86-64-v3)
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endfunction()

  # the POSIX host primitives alone (threads, clocks, virtual memory, profiling): no scheduler, no guest
  snap_os_test(snap_os_host_test tests/platform/os_host_test.cpp runtime/os/linux/os_host_posix.cpp)
  add_test(NAME os_host COMMAND snap_os_host_test)
  add_test(NAME os_host_repeat COMMAND snap_os_host_test --repeat 20)

  # save states are off: the real OS layer with runtime/os/linux/os_state_off.cpp
  snap_os_test(snap_os_state_off_test tests/platform/os_state_off_test.cpp ${SNAP_OS_LAYER_SOURCES})
  add_test(NAME os_state_off COMMAND snap_os_state_off_test)

  # the scheduler, threads, fibers, sync objects and the VAMM/heap, as tests/os/os_test.cpp has always tested them, on the Linux sources
  snap_os_test(snap_os_test tests/os/os_test.cpp ${SNAP_OS_LAYER_SOURCES})
  add_test(NAME os_scheduler COMMAND snap_os_test)
  add_test(NAME os_scheduler_vamm_fault COMMAND ${CMAKE_COMMAND} -E env SNAP_FAULT_VAMM=0:7 OS_TEST_ONLY=vamm $<TARGET_FILE:snap_os_test>)
  add_test(NAME os_scheduler_sync_full COMMAND ${CMAKE_COMMAND} -E env SNAP_SYNC_FULL=1 $<TARGET_FILE:snap_os_test>)
  set_tests_properties(os_host_repeat os_scheduler os_scheduler_vamm_fault os_scheduler_sync_full os_state_off PROPERTIES TIMEOUT 600)

  # Stage 4a: the shared guest TLS module (runtime/hle/guest_tls.h, header only) on buffers the test owns: no loader, no OS layer, no threads.
  # Built twice: plain (C++20) and, when the compiler can, with AddressSanitizer + UBSan, which turns a write outside a block into a failure.
  add_executable(snap_guest_tls_test tests/platform/guest_tls_test.cpp)
  target_include_directories(snap_guest_tls_test PRIVATE runtime/hle)
  target_compile_features(snap_guest_tls_test PRIVATE cxx_std_20)
  target_compile_options(snap_guest_tls_test PRIVATE -Wall -Wextra -O2)
  add_test(NAME guest_tls COMMAND snap_guest_tls_test)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
  check_cxx_source_compiles("int main() { return 0; }" SNAP_HAVE_ASAN_UBSAN)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(SNAP_HAVE_ASAN_UBSAN)
    add_executable(snap_guest_tls_test_san tests/platform/guest_tls_test.cpp)
    target_include_directories(snap_guest_tls_test_san PRIVATE runtime/hle)
    target_compile_features(snap_guest_tls_test_san PRIVATE cxx_std_20)
    target_compile_options(snap_guest_tls_test_san PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(snap_guest_tls_test_san PRIVATE -fsanitize=address,undefined)
    add_test(NAME guest_tls_sanitized COMMAND snap_guest_tls_test_san)
  endif()

  # Stage 4b: guest TLS blocks (runtime/os/guest_tls_block.cpp), in three layers. None needs the game, a GPU or a window.
  #   guest_tls_block   the module alone: lazy one-time capture (also racing), block memory and rollback through the memory seam, ownership
  #   guest_tls_os      the hooks inside the real OS layer: guest threads, fibers, the order of hook, tpidr and entry, release at thread exit
  #   guest_tls_loader  host/loader.cpp as compiled on Linux, with a stand-in main module: new_thread, init_guest_tls, the hook binding through
  #                     the loader's thunk dispatch
  snap_os_test(snap_guest_tls_block_test tests/platform/guest_tls_block_test.cpp runtime/os/guest_tls_block.cpp runtime/os/linux/os_host_posix.cpp)
  add_test(NAME guest_tls_block COMMAND snap_guest_tls_block_test)
  snap_os_test(snap_guest_tls_os_test tests/platform/guest_tls_os_test.cpp ${SNAP_OS_LAYER_SOURCES})
  add_test(NAME guest_tls_os COMMAND snap_guest_tls_os_test)
  snap_os_test(snap_guest_tls_loader_test tests/platform/guest_tls_loader_test.cpp tests/platform/loader_test_stubs.cpp host/loader.cpp host/elf_check.cpp host/guest_tls_hooks.cpp
               runtime/os/guest_tls_block.cpp runtime/os/linux/os_host_posix.cpp)
  target_include_directories(snap_guest_tls_loader_test PRIVATE host)
  target_compile_options(snap_guest_tls_loader_test PRIVATE -w)
  add_test(NAME guest_tls_loader COMMAND snap_guest_tls_loader_test)
  set_tests_properties(guest_tls_block guest_tls_os guest_tls_loader PROPERTIES TIMEOUT 300)

  snap_os_test(snap_guest_tls_lifecycle_test tests/platform/guest_tls_lifecycle_test.cpp ${SNAP_OS_LAYER_SOURCES})
  add_test(NAME guest_tls_lifecycle COMMAND snap_guest_tls_lifecycle_test)
  set_tests_properties(guest_tls_lifecycle PROPERTIES TIMEOUT 300)

  # Stage 4c: host and guest TLS across fibers that change host threads, with the real loader dispatch and the real OS layer. Built three ways:
  # optimised, unoptimised, and with stack protectors in every function (the canary at %fs:0x28 must survive a fiber changing threads).
  set(SNAP_TLS_FIBER_SOURCES tests/platform/loader_test_stubs.cpp host/loader.cpp host/elf_check.cpp host/guest_tls_hooks.cpp ${SNAP_OS_LAYER_SOURCES})
  foreach(variant plain O0 ssp)
    snap_os_test(snap_tls_host_fibers_${variant} tests/platform/tls_host_fibers_test.cpp ${SNAP_TLS_FIBER_SOURCES})
    target_include_directories(snap_tls_host_fibers_${variant} PRIVATE host)
    target_compile_options(snap_tls_host_fibers_${variant} PRIVATE -w)
    if(variant STREQUAL "O0")
      target_compile_options(snap_tls_host_fibers_${variant} PRIVATE -O0)
    elseif(variant STREQUAL "ssp")
      target_compile_options(snap_tls_host_fibers_${variant} PRIVATE -fstack-protector-all)
    endif()
    add_test(NAME tls_host_fibers_${variant} COMMAND snap_tls_host_fibers_${variant})
    set_tests_properties(tls_host_fibers_${variant} PROPERTIES TIMEOUT 300)
  endforeach()
  if(SNAP_HAVE_ASAN_UBSAN)   # a double release, a write outside a block or a leak in the registry is a failure here
    add_executable(snap_guest_tls_block_test_san tests/platform/guest_tls_block_test.cpp runtime/os/guest_tls_block.cpp runtime/os/linux/os_host_posix.cpp)
    target_include_directories(snap_guest_tls_block_test_san PRIVATE runtime runtime/os)
    target_compile_features(snap_guest_tls_block_test_san PRIVATE cxx_std_20)
    target_compile_options(snap_guest_tls_block_test_san PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(snap_guest_tls_block_test_san PRIVATE -fsanitize=address,undefined)
    target_link_libraries(snap_guest_tls_block_test_san PRIVATE Threads::Threads)
    add_test(NAME guest_tls_block_sanitized COMMAND snap_guest_tls_block_test_san)
    set_tests_properties(guest_tls_block_sanitized PROPERTIES TIMEOUT 300)
  endif()
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=thread")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
  check_cxx_source_compiles("int main() { return 0; }" SNAP_HAVE_TSAN)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(SNAP_HAVE_TSAN)   # the capture and the registry are shared between threads: a data race is a failure here
    add_executable(snap_guest_tls_block_test_tsan tests/platform/guest_tls_block_test.cpp runtime/os/guest_tls_block.cpp runtime/os/linux/os_host_posix.cpp)
    target_include_directories(snap_guest_tls_block_test_tsan PRIVATE runtime runtime/os)
    target_compile_features(snap_guest_tls_block_test_tsan PRIVATE cxx_std_20)
    target_compile_options(snap_guest_tls_block_test_tsan PRIVATE -O1 -g -fsanitize=thread)
    target_link_options(snap_guest_tls_block_test_tsan PRIVATE -fsanitize=thread)
    target_link_libraries(snap_guest_tls_block_test_tsan PRIVATE Threads::Threads)
    add_test(NAME guest_tls_block_tsan COMMAND snap_guest_tls_block_test_tsan)
    set_tests_properties(guest_tls_block_tsan PROPERTIES TIMEOUT 300)
  endif()

  # Stage 5a: os_current_cpu() across fibers/host threads, and portable host/hooks.cpp on Linux.
  snap_os_test(snap_os_current_cpu_test tests/platform/os_current_cpu_test.cpp ${SNAP_TLS_FIBER_SOURCES})
  target_include_directories(snap_os_current_cpu_test PRIVATE host)
  target_compile_options(snap_os_current_cpu_test PRIVATE -w)
  add_test(NAME os_current_cpu COMMAND snap_os_current_cpu_test)

  snap_os_test(snap_hooks_linux_test tests/platform/hooks_linux_test.cpp tests/platform/loader_test_stubs.cpp
               host/hooks.cpp host/linux/hooks_mods_stub.cpp host/loader.cpp host/elf_check.cpp
               ${SNAP_OS_LAYER_SOURCES})
  target_include_directories(snap_hooks_linux_test PRIVATE host)
  target_compile_options(snap_hooks_linux_test PRIVATE -w)
  add_test(NAME hooks_linux COMMAND ${CMAKE_COMMAND} -E env SNAP_MEMLOG=1 $<TARGET_FILE:snap_hooks_linux_test>)

  # Stage 5b/5c/5d-b1: Linux runner bootstrap target, signal handling, preflight, and isolated tests.
  snap_os_test(snap_boot_linux host/linux/boot_linux.cpp host/linux/runner_linux.cpp host/linux/crash_linux.cpp
               host/linux/preflight_linux.cpp host/elf_check.cpp runtime/os/linux/os_host_posix.cpp)
  target_include_directories(snap_boot_linux PRIVATE host)

  snap_os_test(snap_boot_linux_test tests/platform/boot_linux_test.cpp host/linux/runner_linux.cpp host/linux/crash_linux.cpp
               host/linux/preflight_linux.cpp host/elf_check.cpp runtime/os/linux/os_host_posix.cpp)
  target_include_directories(snap_boot_linux_test PRIVATE host)
  add_test(NAME boot_linux COMMAND snap_boot_linux_test)
  add_test(NAME boot_linux_help COMMAND snap_boot_linux --help)

  add_test(NAME preflight_linux
    COMMAND ${CMAKE_COMMAND} -E env SNAP_BOOT_LINUX_BIN=$<TARGET_FILE:snap_boot_linux>
            ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/platform/preflight_linux_test.py)

  snap_os_test(snap_crash_linux_test tests/platform/crash_linux_test.cpp host/linux/crash_linux.cpp)
  target_include_directories(snap_crash_linux_test PRIVATE host)
  add_test(NAME crash_linux COMMAND snap_crash_linux_test)

  # Stage 5d-a: Linux AOT static libraries (libsnap_main.a / libsnap_sdk.a).
  # Only defined when generated AOT C sources are present (SNAP_AOT_GEN_DIR); never makes the repo build fail when absent.
  set(SNAP_AOT_GEN_DIR "${PROJECT_SOURCE_DIR}/generated/v262144" CACHE PATH "Directory containing generated main/ and sdk/ AOT C sources")
  set(SNAP_AOT_BUILD_DIR "${CMAKE_BINARY_DIR}/aot" CACHE PATH "Output directory for Linux AOT objects and archives")
  if(EXISTS "${SNAP_AOT_GEN_DIR}/main/manifest.json" AND EXISTS "${SNAP_AOT_GEN_DIR}/sdk/manifest.json")
    set(SNAP_AOT_MAIN_LIB "${SNAP_AOT_BUILD_DIR}/main/lib/libsnap_main.a")
    set(SNAP_AOT_SDK_LIB "${SNAP_AOT_BUILD_DIR}/sdk/lib/libsnap_sdk.a")
    add_custom_target(snap_aot_main
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/recomp/snap_build_linux.py
              --gen ${SNAP_AOT_GEN_DIR}/main --out ${SNAP_AOT_BUILD_DIR}/main --lib snap_main --cc ${CMAKE_C_COMPILER}
      COMMENT "Building Linux AOT archive libsnap_main.a")
    add_custom_target(snap_aot_sdk
      COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/recomp/snap_build_linux.py
              --gen ${SNAP_AOT_GEN_DIR}/sdk --out ${SNAP_AOT_BUILD_DIR}/sdk --lib snap_sdk --cc ${CMAKE_C_COMPILER}
      COMMENT "Building Linux AOT archive libsnap_sdk.a")
  endif()
  add_test(NAME aot_build_linux
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/platform/aot_build_linux_test.py)
endif()

# Pure checks, no compiler needed: they run anywhere CMake and Python run.
add_test(NAME source_classification
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/recomp/test_source_classification.py)
add_test(NAME os_neutral_interfaces
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/platform/test_os_neutral.py)
add_test(NAME source_selection
  COMMAND ${CMAKE_COMMAND} -DSNAP_TEST_ROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/tests/check_selection.cmake)
