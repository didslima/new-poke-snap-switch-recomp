# Helper of check_selection.cmake: loads a manifest that is expected to be rejected.
cmake_minimum_required(VERSION 3.20)
set(PROJECT_SOURCE_DIR "${SNAP_TEST_ROOT}")
include("${SNAP_TEST_ROOT}/cmake/sources.cmake")
snap_load_source_manifest("${SNAP_BAD_MANIFEST}" ROOT "${SNAP_TEST_ROOT}/tests/platform/fixture")
