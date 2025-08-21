cmake_minimum_required(VERSION 3.25)
project(minimalimageviewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── MSYS2 UCRT64 early integration (needed before find_package(glog)) ───────
if (MINGW)
    set(MSYS2_UCRT64_PREFIX "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 root")

    # Make MSYS2-configured packages easy to find
    list(APPEND CMAKE_PREFIX_PATH
            "${MSYS2_UCRT64_PREFIX}"
            "${MSYS2_UCRT64_PREFIX}/lib/cmake"
    )
    list(APPEND CMAKE_MODULE_PATH "${MSYS2_UCRT64_PREFIX}/share/cmake")
    list(APPEND CMAKE_INCLUDE_PATH "${MSYS2_UCRT64_PREFIX}/include")
    list(APPEND CMAKE_LIBRARY_PATH "${MSYS2_UCRT64_PREFIX}/lib")
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "Prefer config-mode packages" FORCE)

    # Help glog’s FindUnwind by pre-seeding Unwind paths if libunwind is present.
    find_path(Unwind_INCLUDE_DIR
        NAMES libunwind.h
        PATHS "${MSYS2_UCRT64_PREFIX}/include" NO_DEFAULT_PATH)
    find_library(Unwind_LIBRARY
        NAMES unwind
        PATHS "${MSYS2_UCRT64_PREFIX}/lib" NO_DEFAULT_PATH)
    if (Unwind_INCLUDE_DIR AND Unwind_LIBRARY)
        set(Unwind_FOUND TRUE CACHE BOOL "" FORCE)
        set(Unwind_INCLUDE_DIR "${Unwind_INCLUDE_DIR}" CACHE PATH "" FORCE)
        set(Unwind_LIBRARY "${Unwind_LIBRARY}" CACHE FILEPATH "" FORCE)
    else()
        # If libunwind isn’t installed, don’t let glog fail hard on it.
        set(CMAKE_DISABLE_FIND_PACKAGE_Unwind ON CACHE BOOL "" FORCE)
    endif()
endif()

# ── Logging deps (MSYS2) ────────────────────────────────────────────────────
# spdlog provides header-only + interface target. glog provides glog::glog.
find_package(spdlog CONFIG REQUIRED)
find_package(glog CONFIG REQUIRED)

# Optional crash-capture deps (MSYS2)
# Breakpad (google-breakpad) and libunwind (LLVM libunwind) may not ship CMake configs in MSYS2,
# so we locate them manually using the MSYS2 UCRT64 prefix paths below.
set(HAVE_BREAKPAD OFF)
set(HAVE_LIBUNWIND OFF)

# ── MSYS2 UCRT64 integration ────────────────────────────────────────────────
# Use this profile with CLion's MinGW toolchain pointing at C:\msys64\ucrt64\bin

if(MINGW)
    set(MSYS2_UCRT64_PREFIX "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 root")

    # Make MSYS2-configured packages easy to find
    list(APPEND CMAKE_PREFIX_PATH
            "${MSYS2_UCRT64_PREFIX}"
            "${MSYS2_UCRT64_PREFIX}/lib/cmake"
    )
    list(APPEND CMAKE_INCLUDE_PATH "${MSYS2_UCRT64_PREFIX}/include")
    list(APPEND CMAKE_LIBRARY_PATH "${MSYS2_UCRT64_PREFIX}/lib")

    # Try to find Breakpad client library and headers
    find_path(BREAKPAD_INCLUDE_DIR
            NAMES client/windows/handler/exception_handler.h
            PATHS "${MSYS2_UCRT64_PREFIX}/include" "${MSYS2_UCRT64_PREFIX}/include/breakpad"
            NO_DEFAULT_PATH)
    # Common library names for breakpad client on MSYS2
    find_library(BREAKPAD_CLIENT_LIB
            NAMES breakpad_client breakpad
            PATHS "${MSYS2_UCRT64_PREFIX}/lib" NO_DEFAULT_PATH)

    if (BREAKPAD_INCLUDE_DIR AND BREAKPAD_CLIENT_LIB)
        message(STATUS "Breakpad found: ${BREAKPAD_CLIENT_LIB}")
        set(HAVE_BREAKPAD ON)
    else()
        message(STATUS "Breakpad not found in MSYS2 UCRT64; proceeding without minidumps")
    endif()

    # Avoid libunwind on MinGW: GCC's unwinder (libgcc_eh) is already linked with -static-libgcc
    # and conflicts with libunwind symbols (_Unwind_Resume, etc.).
    set(HAVE_LIBUNWIND OFF)
    message(STATUS "Skipping libunwind on MinGW/MSYS2 to avoid conflicts with libgcc unwinder")

    # Prefer CONFIG-mode packages (how MSYS2 installs them)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "Prefer config-mode packages" FORCE)

    # Direct config hints
    set(OpenImageIO_DIR "${MSYS2_UCRT64_PREFIX}/lib/cmake/OpenImageIO" CACHE PATH "OpenImageIO config dir")
    set(OpenColorIO_DIR "${MSYS2_UCRT64_PREFIX}/lib/cmake/OpenColorIO" CACHE PATH "OpenColorIO config dir")

    # Ensure MSYS2 tools are visible to any configure-time checks
    if(EXISTS "${MSYS2_UCRT64_PREFIX}/bin")
        set(ENV{PATH} "${MSYS2_UCRT64_PREFIX}/bin;$ENV{PATH}")
    endif()
endif()

# ── Dependencies ────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)
# Keep OpenColorIO enabled; we will bundle its DLLs post-build
find_package(OpenColorIO CONFIG REQUIRED)

# ── Target ──────────────────────────────────────────────────────────────────
add_executable(minimalimageviewer WIN32
        src/main.cpp
        src/ui_handlers.cpp
        src/image_drawing.cpp
        src/image_io.cpp
        src/vulkan_renderer.cpp
        src/vulkan_progress.cpp
        src/logging.cpp
        src/viewer.cpp
        src/vulkan_renderer.h
        src/viewer.h
        src/logging.h
        src/resource.h
)

# Link Vulkan
if (TARGET Vulkan::Vulkan)
    target_link_libraries(minimalimageviewer PRIVATE Vulkan::Vulkan)
else()
    target_include_directories(minimalimageviewer PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(minimalimageviewer PRIVATE ${Vulkan_LIBRARIES})
endif()

# Link OpenImageIO (required)
target_link_libraries(minimalimageviewer PRIVATE OpenImageIO::OpenImageIO)

# Link OpenColorIO only if it is available; otherwise, compile with the shim
if (TARGET OpenColorIO::OpenColorIO)
    target_link_libraries(minimalimageviewer PRIVATE OpenColorIO::OpenColorIO)
else()
    target_compile_definitions(minimalimageviewer PRIVATE PORTABLE_NO_OCIO=1)
endif()

# Logging libraries
target_link_libraries(minimalimageviewer PRIVATE spdlog::spdlog glog::glog)

# Apply optional crash-capture integrations resolved from MSYS2 prefix
if (HAVE_BREAKPAD)
    target_include_directories(minimalimageviewer PRIVATE "${BREAKPAD_INCLUDE_DIR}")
    target_link_libraries(minimalimageviewer PRIVATE "${BREAKPAD_CLIENT_LIB}")
    target_compile_definitions(minimalimageviewer PRIVATE HAVE_BREAKPAD=1)
endif()

if (HAVE_LIBUNWIND AND NOT MINGW)
    target_include_directories(minimalimageviewer PRIVATE "${LIBUNWIND_INCLUDE_DIR}")
    target_link_libraries(minimalimageviewer PRIVATE "${LIBUNWIND_LIB}")
    target_compile_definitions(minimalimageviewer PRIVATE HAVE_LIBUNWIND=1)
endif()

# Include prebuilt resource on MSVC if available (adds icon/metadata)
if (MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.res")
    target_sources(minimalimageviewer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.res")
endif()

# Platform-specific settings
if (WIN32)
    target_compile_definitions(minimalimageviewer PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            VK_USE_PLATFORM_WIN32_KHR
    )
    if (MINGW)
        # Use Unicode entry point and statically link GCC and libstdc++ runtimes to avoid shipping those DLLs
        target_link_options(minimalimageviewer PRIVATE
                -municode
                -static-libgcc
                -static-libstdc++
        )
    endif()

    target_link_libraries(minimalimageviewer PRIVATE
            user32
            gdi32
            comdlg32
            shlwapi
            ole32
            shell32
            propsys
            oleaut32
    )

    if (MINGW)
        # FINAL STEP: Forcefully embed a static winpthread to avoid any runtime dependency on libwinpthread-1.dll.
        # Prefer the explicit static archive from MSYS2 UCRT64; try both common names.
        set(WINPTHREAD_STATIC_CANDIDATES
                "${MSYS2_UCRT64_PREFIX}/lib/libwinpthread.a"
                "${MSYS2_UCRT64_PREFIX}/lib/libwinpthreads.a"
        )
        set(WINPTHREAD_STATIC_FOUND "")
        foreach(CAND ${WINPTHREAD_STATIC_CANDIDATES})
            if (EXISTS "${CAND}")
                set(WINPTHREAD_STATIC_FOUND "${CAND}")
                break()
            endif()
        endforeach()

        if (WINPTHREAD_STATIC_FOUND)
            # Use --whole-archive so the linker cannot drop it, guaranteeing no DLL required.
            target_link_libraries(minimalimageviewer PRIVATE
                    -Wl,--whole-archive
                    "${WINPTHREAD_STATIC_FOUND}"
                    -Wl,--no-whole-archive
            )
        else()
            # Fallback to static link flags; still try to force static if the archive is in the search path.
            target_link_libraries(minimalimageviewer PRIVATE
                    -Wl,-Bstatic
                    -lwinpthread
                    -Wl,-Bdynamic
            )
        endif()

        # Post-build: bundle all dependent DLLs (OpenColorIO, OpenImageIO, and transitive deps)
        # into the output directory so the EXE runs standalone.
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
        add_custom_command(TARGET minimalimageviewer POST_BUILD
                COMMAND "${CMAKE_COMMAND}"
                    -DEXECUTABLE="$<TARGET_FILE:minimalimageviewer>"
                    -DSEARCH_DIRS="${MSYS2_UCRT64_PREFIX}/bin;${MSYS2_UCRT64_PREFIX}/lib"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundleDeps.cmake"
                COMMENT "Bundling runtime DLLs (OCIO/OIIO and dependencies) next to the executable"
        )
    endif()
endif()

# Warnings
if (MSVC)
    target_compile_options(minimalimageviewer PRIVATE -Wconversion -Wshadow)
else()
    target_compile_options(minimalimageviewer PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# MSVC runtime (if you ever switch to MSVC toolchain)
if (MSVC)
    set_property(TARGET minimalimageviewer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
