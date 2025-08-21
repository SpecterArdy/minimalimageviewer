# cmake/BundleDeps.cmake
# Usage: cmake -DEXECUTABLE=<path_to_exe> -DSEARCH_DIRS="<dir1>;<dir2>" -P BundleDeps.cmake

if (NOT EXECUTABLE)
    message(FATAL_ERROR "BundleDeps.cmake: EXECUTABLE not set")
endif()

set(SEARCH_DIRS "${SEARCH_DIRS}")
file(TO_CMAKE_PATH "${EXECUTABLE}" EXE_PATH)
get_filename_component(BUNDLE_DIR "${EXE_PATH}" DIRECTORY)

# Load BundleUtilities for fixup_bundle (works on Windows with MinGW if objdump is available)
include(BundleUtilities)

# Copy dependent DLLs from provided search dirs next to the EXE.
# The empty second argument means: compute dependencies from the EXE itself.
fixup_bundle("${EXE_PATH}" "" "${SEARCH_DIRS}")

# Keep the console quiet
message(STATUS "Bundled dependent DLLs into: ${BUNDLE_DIR}")
