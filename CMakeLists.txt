cmake_minimum_required(VERSION 3.25)
project(minimalimageviewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
find_package(OpenColorIO CONFIG REQUIRED)

# ── Target ──────────────────────────────────────────────────────────────────
add_executable(minimalimageviewer WIN32
        src/main.cpp
        src/ui_handlers.cpp
        src/image_drawing.cpp
        src/image_io.cpp
        src/vulkan_renderer.cpp
        src/vulkan_progress.cpp
        src/viewer.cpp
        src/vulkan_renderer.h
        src/viewer.h
        src/resource.h
)

# Link Vulkan
if (TARGET Vulkan::Vulkan)
    target_link_libraries(minimalimageviewer PRIVATE Vulkan::Vulkan)
else()
    target_include_directories(minimalimageviewer PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(minimalimageviewer PRIVATE ${Vulkan_LIBRARIES})
endif()

# Link OpenImageIO and OpenColorIO from MSYS2
target_link_libraries(minimalimageviewer
        PRIVATE
        OpenImageIO::OpenImageIO
        OpenColorIO::OpenColorIO
)

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
        # Use Unicode entry point; keep MSYS2-provided DLL runtime (avoid static-libgcc/std++)
        target_link_options(minimalimageviewer PRIVATE -municode)
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
