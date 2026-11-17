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
endif()

# Pure checks, no compiler needed: they run anywhere CMake and Python run.
add_test(NAME source_classification
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/recomp/test_source_classification.py)
add_test(NAME os_neutral_interfaces
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/platform/test_os_neutral.py)
add_test(NAME source_selection
  COMMAND ${CMAKE_COMMAND} -DSNAP_TEST_ROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/tests/check_selection.cmake)
