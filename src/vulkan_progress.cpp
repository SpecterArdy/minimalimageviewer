#include "vulkan_renderer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Implement a stepwise initialization that mirrors the standard Initialize()
// but reports progress at each milestone. This avoids blocking the UI splash
// and provides accurate, user-visible feedback.
bool VulkanRenderer::InitializeWithProgress(HWND hwnd, ProgressCallback cb) {
    auto report = [&](int pct, const wchar_t* stage) {
        if (cb) cb(pct, stage);
    };

    report(5,  L"Checking system and creating Vulkan instance...");
    if (!initInstance()) {
        report(100, L"Failed to create Vulkan instance");
        return false;
    }

    report(20, L"Creating presentation surface...");
    if (!createSurface(hwnd)) {
        report(100, L"Failed to create surface");
        return false;
    }

    report(35, L"Selecting physical device...");
    if (!pickPhysicalDevice()) {
        report(100, L"No suitable GPU found");
        return false;
    }

    report(55, L"Creating logical device and queues...");
    if (!createDeviceAndQueues()) {
        report(100, L"Failed to create device/queues");
        return false;
    }

    report(65, L"Creating command pool...");
    if (!createCommandPool()) {
        report(100, L"Failed to create command pool");
        return false;
    }

    // Determine initial swapchain extent from the current window
#ifdef _WIN32
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const uint32_t width  = static_cast<uint32_t>(cr.right  > cr.left ? (cr.right  - cr.left) : 1);
    const uint32_t height = static_cast<uint32_t>(cr.bottom > cr.top  ? (cr.bottom - cr.top)  : 1);
#else
    const uint32_t width = 1, height = 1;
#endif

    report(80, L"Creating swapchain...");
    if (!createSwapchain(width, height)) {
        report(100, L"Failed to create swapchain");
        return false;
    }

    report(90, L"Creating synchronization primitives...");
    if (!createSyncObjects()) {
        report(100, L"Failed to create sync objects");
        return false;
    }

    vulkanAvailable_ = true;
    report(100, L"Vulkan ready");
    return true;
}
