# cmake/BundleDeps.cmake
# Enhanced dependency bundling for MSYS2 UCRT64 applications
# Usage: cmake -DEXECUTABLE=<path_to_exe> -DSEARCH_DIRS="<dir1>;<dir2>" -P BundleDeps.cmake

if (NOT EXECUTABLE)
    message(FATAL_ERROR "BundleDeps.cmake: EXECUTABLE not set")
endif()

set(SEARCH_DIRS "${SEARCH_DIRS}")
file(TO_CMAKE_PATH "${EXECUTABLE}" EXE_PATH)
get_filename_component(BUNDLE_DIR "${EXE_PATH}" DIRECTORY)

# Convert search dirs to list
string(REPLACE ";" ";" SEARCH_DIRS_LIST "${SEARCH_DIRS}")

# Essential DLLs that must be bundled for standalone operation
set(REQUIRED_DLLS
    # SDL3 core libraries
    "SDL3.dll"
    "SDL3_ttf.dll"
    "SDL3_image.dll"
    
    # OpenImageIO and OpenColorIO
    "libOpenImageIO-3.0.dll"
    "libOpenImageIO_Util-3.0.dll"
    "libOpenColorIO_2_4.dll"
    
    # Common C++ runtime and system libraries
    "libstdc++-6.dll"
    "libgcc_s_seh-1.dll"
    "libwinpthread-1.dll"
    
    # Image format libraries
    "libjpeg-8.dll"
    "libpng16-16.dll"
    "libtiff-6.dll"
    "libwebp-7.dll"
    "libwebpmux-3.dll"
    "libwebpdemux-2.dll"
    "libheif-1.dll"
    "libde265-0.dll"
    "libx265.dll"
    "libaom.dll"
    "libdav1d-7.dll"
    "libzstd-1.dll"
    "liblzma-5.dll"
    "zlib1.dll"
    "libbz2-1.dll"
    
    # Font and text rendering
    "libfreetype-6.dll"
    "libharfbuzz-0.dll"
    "libglib-2.0-0.dll"
    "libintl-8.dll"
    "libiconv-2.dll"
    "libpcre2-8-0.dll"
    "libgraphite2.dll"
    
    # Color management and YAML
    "libyaml-cpp.dll"
    "libexpat-1.dll"
    
    # Math and utility libraries
    "libboost_filesystem-mt.dll"
    "libboost_system-mt.dll"
    "libboost_thread-mt.dll"
    "libfmt.dll"
    "libspdlog.dll"
    
    # OpenEXR libraries
    "libOpenEXR-3_2.dll"
    "libImath-3_1.dll"
    "libIlmThread-3_2.dll"
    "libIex-3_2.dll"
    "libOpenEXRCore-3_2.dll"
    "libOpenEXRUtil-3_2.dll"
)

# Function to copy DLL if it exists
function(copy_dll_if_exists dll_name search_dirs target_dir)
    set(dll_found FALSE)
    foreach(search_dir ${search_dirs})
        set(dll_path "${search_dir}/${dll_name}")
        if(EXISTS "${dll_path}")
            message(STATUS "Copying ${dll_name} from ${search_dir}")
            file(COPY "${dll_path}" DESTINATION "${target_dir}")
            set(dll_found TRUE)
            break()
        endif()
    endforeach()
    
    if(NOT dll_found)
        message(STATUS "Warning: ${dll_name} not found in search directories")
    endif()
endfunction()

# Copy all required DLLs
message(STATUS "Bundling DLLs to: ${BUNDLE_DIR}")
foreach(dll_name ${REQUIRED_DLLS})
    copy_dll_if_exists("${dll_name}" "${SEARCH_DIRS_LIST}" "${BUNDLE_DIR}")
endforeach()

# Use BundleUtilities as fallback to catch any missed dependencies
include(BundleUtilities)

# Run fixup_bundle to catch any dependencies we might have missed
# Note: this may fail silently if objdump isn't available, but our manual copy above covers most cases
if(EXISTS "${EXE_PATH}")
    message(STATUS "Running fixup_bundle to catch any remaining dependencies...")
    fixup_bundle("${EXE_PATH}" "" "${SEARCH_DIRS}")
else()
    message(WARNING "Executable not found: ${EXE_PATH}")
endif()

message(STATUS "Dependency bundling completed for: ${BUNDLE_DIR}")
