cmake_minimum_required(VERSION 3.25)
project(minimalimageviewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Helpful for CLion inspections and tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)

# Windows GUI executable and project sources
add_executable(minimalimageviewer WIN32
    src/main.cpp
    src/ui_handlers.cpp
    src/image_drawing.cpp
    src/image_io.cpp
    src/vulkan_renderer.cpp
    src/viewer.cpp
    src/vulkan_renderer.h

    # Headers (for IDEs)
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
    # MinGW: use Unicode entry and statically link GCC/STD runtimes and winpthread (keep system/Vulkan dynamic)
    if (MINGW)
        target_link_options(minimalimageviewer PRIVATE -municode -static-libgcc -static-libstdc++)
        # Link winpthread statically to avoid additional runtime DLLs
        target_link_options(minimalimageviewer PRIVATE -Wl,-Bstatic)
        target_link_libraries(minimalimageviewer PRIVATE winpthread)
        # Return to dynamic for system and Vulkan libraries
        target_link_options(minimalimageviewer PRIVATE -Wl,-Bdynamic)
    endif()

    target_link_libraries(minimalimageviewer PRIVATE
        user32
        gdi32
        comdlg32
        shlwapi
        windowscodecs
        ole32
        shell32
        propsys
        oleaut32
    )
endif()

# Warnings (optional but helpful)
if (MSVC)
    target_compile_options(minimalimageviewer PRIVATE /W4 /permissive-)
else()
    # Suppress noisy aggregate init warnings without behavioral change
    target_compile_options(minimalimageviewer PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# MSVC: statically link the runtime to avoid MSVC CRT DLL dependencies
if (MSVC)
    set_property(TARGET minimalimageviewer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
