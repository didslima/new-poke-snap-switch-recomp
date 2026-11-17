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
endif()

# Pure checks, no compiler needed: they run anywhere CMake and Python run.
add_test(NAME source_classification
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/recomp/test_source_classification.py)
add_test(NAME os_neutral_interfaces
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/platform/test_os_neutral.py)
add_test(NAME source_selection
  COMMAND ${CMAKE_COMMAND} -DSNAP_TEST_ROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/tests/check_selection.cmake)
