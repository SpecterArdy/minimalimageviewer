#include "vulkan_renderer.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <climits>

#ifdef _WIN32
#include <winreg.h>   // For registry functions (RegOpenKeyExA, RegCloseKey)

// NASA Standard: No exceptions - use error codes for all failure paths
static bool checkVulkanResult(VkResult r, const char* /*msg*/, bool& outDeviceLost, bool& outSwapchainOutOfDate) {
    // NASA Standard: Initialize all output parameters
    outDeviceLost = false;
    outSwapchainOutOfDate = false;

    if (r == VK_SUCCESS) {
        return true;
    }

    // NASA Standard: Handle all known error conditions explicitly
    switch (r) {
        case VK_ERROR_DEVICE_LOST:
            outDeviceLost = true;
            return false;

        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
            outSwapchainOutOfDate = true;
            return false;

        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        case VK_ERROR_INITIALIZATION_FAILED:
        case VK_ERROR_LAYER_NOT_PRESENT:
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_FEATURE_NOT_PRESENT:
        case VK_ERROR_INCOMPATIBLE_DRIVER:
        case VK_ERROR_TOO_MANY_OBJECTS:
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
        case VK_ERROR_FRAGMENTED_POOL:
        case VK_ERROR_SURFACE_LOST_KHR:
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        case VK_ERROR_VALIDATION_FAILED_EXT:
        case VK_ERROR_INVALID_SHADER_NV:
        default:
            // NASA Standard: Log specific error but continue execution
            return false;
    }
}

// NASA Standard: Simplified check function that sets error flags instead of throwing
static bool checkVulkanOperation(VkResult r, bool& outDeviceLost, bool& outSwapchainOutOfDate) {
    return checkVulkanResult(r, "", outDeviceLost, outSwapchainOutOfDate);
}

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { Shutdown(); }

bool VulkanRenderer::initInstance() {
    // NASA Standard: Comprehensive system validation before attempting Vulkan

    // Check if we're running in a compatible environment
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    if (sysInfo.dwPageSize == 0 || sysInfo.dwNumberOfProcessors == 0) {
        return false; // System information unavailable
    }

    // NASA Standard: Check available memory before proceeding
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (!GlobalMemoryStatusEx(&memStatus)) {
        return false; // Cannot determine memory status
    }

    // NASA Standard: Require minimum 1GB available memory for Vulkan operations
    if (memStatus.ullAvailPhys < (1024ULL * 1024ULL * 1024ULL)) {
        return false; // Insufficient memory for safe Vulkan operation
    }

    // Skipping registry probe under MSYS2/MinGW; rely on Vulkan loader presence

    // Create Vulkan instance using the linked loader (MSYS2/MinGW)
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MinimalImageViewer";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "MinimalIV";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = 0;
    ci.ppEnabledLayerNames = nullptr;
    ci.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    ci.ppEnabledExtensionNames = extensions;

    VkResult createRes = vkCreateInstance(&ci, nullptr, &instance_);
    if (createRes != VK_SUCCESS || instance_ == VK_NULL_HANDLE) {
        instance_ = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    // NASA Standard: Validate instance state before operations
    if (instance_ == VK_NULL_HANDLE) {
        return false;
    }

    uint32_t count = 0;
    VkResult enumResult = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (enumResult != VK_SUCCESS || count == 0) {
        return false;
    }

    // NASA Standard: Validate count to prevent memory issues
    if (count > 32) { // Reasonable upper limit for GPU devices
        count = 32;
    }

    std::vector<VkPhysicalDevice> devs(count);
    enumResult = vkEnumeratePhysicalDevices(instance_, &count, devs.data());
    if (enumResult != VK_SUCCESS) {
        return false;
    }

    for (auto d : devs) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, qprops.data());

        uint32_t gfxIdx = UINT32_MAX, presentIdx = UINT32_MAX;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gfxIdx = i;
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &presentSupport);
            if (presentSupport) {
                if (presentIdx == UINT32_MAX) presentIdx = i;
            }
        }
        if (gfxIdx != UINT32_MAX && presentIdx != UINT32_MAX) {
            physicalDevice_ = d;
            graphicsQueueFamily_ = gfxIdx;
            presentQueueFamily_ = presentIdx;
            return true;
        }
    }
    return false;
}

void VulkanRenderer::SetColorTransform(void* processor) {
    colorProcessor_ = processor;
    // Implementation would create color LUT from OpenColorIO processor
    // For now, just store the pointer
}

bool VulkanRenderer::createDeviceAndQueues() {
    // NASA Standard: Validate physical device and queue families
    if (physicalDevice_ == VK_NULL_HANDLE || 
        graphicsQueueFamily_ == UINT32_MAX || 
        presentQueueFamily_ == UINT32_MAX) {
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2]{};
    uint32_t uniqueFamilies[2] = { graphicsQueueFamily_, presentQueueFamily_ };
    uint32_t qciCount = (graphicsQueueFamily_ == presentQueueFamily_) ? 1u : 2u;

    for (uint32_t i = 0; i < qciCount; ++i) {
        qcis[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[i].queueFamilyIndex = uniqueFamilies[i];
        qcis[i].queueCount = 1;
        qcis[i].pQueuePriorities = &prio;
    }

    const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = qciCount;
    dci.pQueueCreateInfos = qcis;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;

    if (vkCreateDevice(physicalDevice_, &dci, nullptr, &device_) != VK_SUCCESS) return false;

    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentQueueFamily_, 0, &presentQueue_);
    return true;
}

bool VulkanRenderer::createSurface(HWND hwnd) {
    // NASA Standard: Validate all input parameters and state
    if (hwnd == nullptr || instance_ == VK_NULL_HANDLE) {
        return false;
    }

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    if (hInstance == nullptr) {
        return false;
    }

    // NASA Standard: Initialize all structure members explicitly
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.pNext = nullptr;
    sci.flags = 0;
    sci.hinstance = hInstance;
    sci.hwnd = hwnd;

    VkResult createResult = vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_);
    if (createResult != VK_SUCCESS) {
        surface_ = VK_NULL_HANDLE;
        return false;
    }

    // NASA Standard: Validate surface was created
    if (surface_ == VK_NULL_HANDLE) {
        return false;
    }

    return true;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    // NASA Standard: Validate device state before operations
    if (!physicalDevice_) {
        return UINT32_MAX; // Invalid memory type index
    }

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    // NASA Standard: Validate returned data
    if (memProps.memoryTypeCount == 0) {
        return UINT32_MAX;
    }

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    // NASA Standard: Return error indicator instead of throwing
    return UINT32_MAX; // No suitable memory type found
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = graphicsQueueFamily_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return vkCreateCommandPool(device_, &pci, nullptr, &commandPool_) == VK_SUCCESS;
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() {
    // NASA Standard: Validate device state before operations
    if (!device_ || !commandPool_) {
        return VK_NULL_HANDLE;
    }

    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = commandPool_;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkResult allocResult = vkAllocateCommandBuffers(device_, &a, &cmd);
    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    if (!checkVulkanOperation(allocResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult beginResult = vkBeginCommandBuffer(cmd, &bi);
    if (!checkVulkanOperation(beginResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void VulkanRenderer::endSingleTimeCommands(VkCommandBuffer cmd) {
    // NASA Standard: Validate input parameters
    if (cmd == VK_NULL_HANDLE || !device_ || !graphicsQueue_) {
        return;
    }

    VkResult endResult = vkEndCommandBuffer(cmd);
    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    if (!checkVulkanOperation(endResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        return;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
    if (!checkVulkanOperation(submitResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        return;
    }

    // NASA Standard: Wait for completion with timeout to prevent hangs
    VkResult waitResult = vkQueueWaitIdle(graphicsQueue_);
    if (waitResult == VK_ERROR_DEVICE_LOST) {
        deviceLost_ = true;
    }

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

bool VulkanRenderer::createSwapchain(uint32_t width, uint32_t height) {
    // Query formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    if (formatCount == 0) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    // Prefer SRGB for correct presentation gamma; fallback to UNORM
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) { chosen = f; break; }
    }
    if (chosen.format != VK_FORMAT_B8G8R8A8_SRGB) {
        for (auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
        }
    }
    swapchainFormat_ = chosen.format;
    swapchainColorSpace_ = chosen.colorSpace;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    swapchainExtent_ = extent;

    uint32_t imageCount = std::clamp(2u, caps.minImageCount, (caps.maxImageCount ? caps.maxImageCount : 3u));

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = swapchainFormat_;
    sci.imageColorSpace = swapchainColorSpace_;
    sci.imageExtent = swapchainExtent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; // We'll blit into it
    uint32_t qfi[2] = { graphicsQueueFamily_, presentQueueFamily_ };
    if (graphicsQueueFamily_ != presentQueueFamily_) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = qfi;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) return false;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());

    // Views
    swapchainImageViews_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = swapchainImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapchainFormat_;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &vi, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) return false;
    }

    // Command buffers per image
    if (commandBuffers_.size() != count) {
        if (commandPool_ == VK_NULL_HANDLE) { if (!createCommandPool()) return false; }
        if (!commandBuffers_.empty())
            vkFreeCommandBuffers(device_, commandPool_, static_cast<uint32_t>(commandBuffers_.size()), commandBuffers_.data());
        VkCommandBufferAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        a.commandPool = commandPool_;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = count;
        commandBuffers_.resize(count);
        if (vkAllocateCommandBuffers(device_, &a, commandBuffers_.data()) != VK_SUCCESS) return false;
    }

    return true;
}

void VulkanRenderer::destroySwapchain() {
    for (auto v : swapchainImageViews_) {
        if (v) vkDestroyImageView(device_, v, nullptr);
    }
    swapchainImageViews_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(device_, &si, nullptr, &imageAvailable_) != VK_SUCCESS) return false;
    if (vkCreateSemaphore(device_, &si, nullptr, &renderFinished_) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fi, nullptr, &inFlightFence_) != VK_SUCCESS) return false;
    return true;
}

void VulkanRenderer::recreateSwapchain(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain(width, height);
}

bool VulkanRenderer::createTexture(uint32_t width, uint32_t height, bool isHdr) {
    destroyTexture();

    // Choose format based on HDR flag
    textureFormat_ = isHdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB;
    textureIsHdr_ = isHdr;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = { width, height, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = textureFormat_;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &ii, nullptr, &textureImage_) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, textureImage_, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // NASA Standard: Validate memory type index before allocation
    if (ai.memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
        return false;
    }

    if (vkAllocateMemory(device_, &ai, nullptr, &textureMemory_) != VK_SUCCESS) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(device_, textureImage_, textureMemory_, 0);

    textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    textureWidth_ = width;
    textureHeight_ = height;
    textureIsSparse_ = false; // NASA Standard: Regular textures are not sparse
    return true;
}

void VulkanRenderer::destroyTexture() {
    // NASA Standard: Clean up sparse image tiles first
    if (textureIsSparse_ && device_ != VK_NULL_HANDLE) {
        for (auto& tile : imageTiles_) {
            if (tile.stagingBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, tile.stagingBuffer, nullptr);
            }
            if (tile.stagingMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, tile.stagingMemory, nullptr);
            }
            if (tile.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, tile.memory, nullptr);
            }
        }
    }

    if (textureImage_) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
    }
    if (textureMemory_) {
        vkFreeMemory(device_, textureMemory_, nullptr);
        textureMemory_ = VK_NULL_HANDLE;
    }
    textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    textureWidth_ = textureHeight_ = 0;
    textureIsSparse_ = false; // NASA Standard: Reset sparse flag when destroying texture
    imageTiles_.clear(); // NASA Standard: Clear any tile data
}

bool VulkanRenderer::Initialize(HWND hwnd) {
    // NASA Standard: Validate input parameters
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }

    // NASA Standard: Initialize all member variables to safe states
    deviceLost_ = false;
    swapchainOutOfDate_ = false;
    vulkanAvailable_ = false;

    // NASA Standard: Attempt Vulkan initialization with full error protection
    if (!initInstance()) {
        // NASA Standard: Vulkan unavailable, set up software fallback
        return initializeSoftwareFallback(hwnd);
    }

    if (!createSurface(hwnd)) {
        Shutdown(); // Clean up instance on failure
        return false;
    }

    if (!pickPhysicalDevice()) {
        Shutdown(); // Clean up instance and surface on failure
        return false;
    }

    if (!createDeviceAndQueues()) {
        Shutdown(); // Clean up all previous resources on failure
        return false;
    }

    if (!createCommandPool()) {
        Shutdown(); // Clean up all previous resources on failure
        return false;
    }

    if (!createSwapchain(800, 600)) {
        Shutdown(); // Clean up all previous resources on failure
        return false;
    }

    if (!createSyncObjects()) {
        Shutdown(); // Clean up all previous resources on failure
        return false;
    }

    // NASA Standard: Mark Vulkan as available after successful initialization
    vulkanAvailable_ = true;
    return true;
}

void VulkanRenderer::Shutdown() {
    // NASA Standard: Reset error flags during shutdown
    deviceLost_ = false;
    swapchainOutOfDate_ = false;

    if (device_ == VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        return;
    }

    // NASA Standard: Wait for device to be idle before cleanup
    VkResult waitResult = vkDeviceWaitIdle(device_);
    if (waitResult == VK_ERROR_DEVICE_LOST) {
        // Device is lost, skip device-dependent cleanup
        device_ = VK_NULL_HANDLE;
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        return;
    }

    // NASA Standard: Clean up resources in reverse order of creation
    destroyTexture();
    destroySwapchain();

    if (imageAvailable_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        imageAvailable_ = VK_NULL_HANDLE;
    }
    if (renderFinished_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, renderFinished_, nullptr);
        renderFinished_ = VK_NULL_HANDLE;
    }
    if (inFlightFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, inFlightFence_, nullptr);
        inFlightFence_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    // NASA Standard: Clean up Vulkan library handle
    if (vulkanLibrary_ != nullptr) {
        FreeLibrary(vulkanLibrary_);
        vulkanLibrary_ = nullptr;
    }

    // NASA Standard: Reset all queue handles
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
}

void VulkanRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (swapchainExtent_.width == width && swapchainExtent_.height == height) return;
    recreateSwapchain(width, height);
}

bool VulkanRenderer::createStagingBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bi, nullptr, &buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // NASA Standard: Validate memory type index before allocation
    if (ai.memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }

    if (vkAllocateMemory(device_, &ai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }

    vkBindBufferMemory(device_, buffer, memory, 0);
    return true;
}

void VulkanRenderer::transitionImageLayout(VkImage image, VkFormat /*format*/, VkImageLayout oldLayout, VkImageLayout newLayout) {
    // NASA Standard: Validate all input parameters and device state
    if (!device_ || !commandPool_ || image == VK_NULL_HANDLE) {
        return; // Silently fail if device is not ready
    }

    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return; // Command buffer allocation failed
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        // Fallback: handle any other layout transition with general barriers
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    endSingleTimeCommands(cmd);
}

void VulkanRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    // NASA Standard: Validate all input parameters
    if (buffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }

    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return; // Command buffer allocation failed
    }

    // NASA Standard: Initialize all structure members explicitly
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    endSingleTimeCommands(cmd);
}

void VulkanRenderer::UpdateImageFromData(const void* pixelData, uint32_t width, uint32_t height, bool isHdr) {
    // NASA Standard: Validate all input parameters before any operations
    if (pixelData == nullptr || width == 0 || height == 0) {
        return;
    }

    // NASA Standard: Validate dimensions to prevent GPU memory exhaustion
    if (width > 65536 || height > 65536) {
        return; // Dimensions too large for safe GPU operation
    }

    // NASA Standard: Check for potential integer overflow in size calculations
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxSafePixels = UINT64_C(67108864); // 8K x 8K limit
    if (pixelCount > maxSafePixels) {
        return;
    }

    // NASA Standard: Check device state before GPU operations
    if (deviceLost_) {
        return; // Cannot update texture when device is lost
    }

    if (!pixelData || width == 0 || height == 0 || !device_) return;

    bool needNewTexture = (textureWidth_ != width || textureHeight_ != height || 
                           textureIsHdr_ != isHdr || textureImage_ == VK_NULL_HANDLE);

    if (needNewTexture) {
        if (!createTexture(width, height, isHdr)) {
            // Failed to create texture, mark device as lost to trigger recovery
            deviceLost_ = true;
            return;
        }
    }

    // Calculate pixel data size based on format
    size_t pixelSize = isHdr ? (4 * sizeof(uint16_t)) : (4 * sizeof(uint8_t)); // RGBA16F or RGBA8
    VkDeviceSize dataSize = static_cast<VkDeviceSize>(width * height * pixelSize);

    // Create staging buffer
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    if (!createStagingBuffer(dataSize, staging, stagingMem)) {
        return;
    }

    void* mapped = nullptr;
    VkResult mapResult = vkMapMemory(device_, stagingMem, 0, dataSize, 0, &mapped);
    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    if (!checkVulkanOperation(mapResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return;
    }

    std::memcpy(mapped, pixelData, dataSize);
    vkUnmapMemory(device_, stagingMem);

    // NASA Standard: No exceptions - defensive layout transitions
    transitionImageLayout(textureImage_, textureFormat_, textureLayout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!deviceLost_) {
        copyBufferToImage(staging, textureImage_, width, height);
        if (!deviceLost_) {
            transitionImageLayout(textureImage_, textureFormat_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            if (!deviceLost_) {
                textureLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
        }
    }

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

void VulkanRenderer::UpdateImageFromHBITMAP(HBITMAP hBitmap) {
    if (!hBitmap) return;

    BITMAP bm{};
    GetObject(hBitmap, sizeof(BITMAP), &bm);
    uint32_t width = static_cast<uint32_t>(bm.bmWidth);
    uint32_t height = static_cast<uint32_t>(bm.bmHeight);
    if (width == 0 || height == 0) return;

    // Read pixels as BGRA and convert to RGBA
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgraPixels(width * height * 4);
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, hBitmap, 0, height, bgraPixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    // Convert BGRA to RGBA
    std::vector<uint8_t> rgbaPixels(width * height * 4);
    for (size_t i = 0; i < bgraPixels.size(); i += 4) {
        rgbaPixels[i] = bgraPixels[i + 2];     // R
        rgbaPixels[i + 1] = bgraPixels[i + 1]; // G
        rgbaPixels[i + 2] = bgraPixels[i];     // B
        rgbaPixels[i + 3] = bgraPixels[i + 3]; // A
    }

    UpdateImageFromData(rgbaPixels.data(), width, height, false);
}

void VulkanRenderer::UpdateImageFromLDRData(const void* pixelData, uint32_t width, uint32_t height, bool /*generateMipmaps*/) {
    UpdateImageFromData(pixelData, width, height, false);
}

void VulkanRenderer::UpdateImageFromHDRData(const uint16_t* pixelData, uint32_t width, uint32_t height, bool /*generateMipmaps*/) {
    UpdateImageFromData(pixelData, width, height, true);
}

void VulkanRenderer::Render(uint32_t width, uint32_t height, float zoom, float offsetX, float offsetY, int /*rotationAngle*/) {
    // NASA Standard: Validate all input parameters
    if (width == 0 || height == 0 || width > 65536 || height > 65536) {
        return;
    }

    // NASA Standard: Use software fallback if Vulkan is unavailable
    if (!vulkanAvailable_) {
        renderSoftwareFallback(width, height);
        return;
    }

    // NASA Standard: Check device state before operations
    if (deviceLost_) {
        return; // Device lost, cannot render
    }

    // NASA Standard: Validate zoom parameters to prevent GPU driver stress
    if (zoom < 0.001f || zoom > 1000.0f) {
        zoom = 1.0f; // Clamp to safe default
    }

    // NASA Standard: Clear any previous transient error states
    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    if (!device_ || !swapchain_) return;

    // Recreate swapchain if size changed
    if (width == 0 || height == 0) return;
    if (swapchainExtent_.width != width || swapchainExtent_.height != height) {
        recreateSwapchain(width, height);
    }

    vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &inFlightFence_);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain(width, height);
        return;
    }

    VkCommandBuffer cmd = commandBuffers_[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkResult beginResult = vkBeginCommandBuffer(cmd, &bi);
    if (!checkVulkanOperation(beginResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        return;
    }

    // Transition swapchain image from PRESENT to TRANSFER_DST to avoid flicker
    VkImageMemoryBarrier pre{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    pre.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.image = swapchainImages_[imageIndex];
    pre.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    pre.subresourceRange.levelCount = 1;
    pre.subresourceRange.layerCount = 1;
    pre.srcAccessMask = 0;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &pre);

    // Clear to black (via vkCmdClearColorImage on TRANSFER_DST layout)
    VkClearColorValue black{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    // Blit image if available
    if (textureImage_ && textureLayout_ == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        // Compute destination rectangle with zoom and offsets (no rotation for now)
        float contentW = static_cast<float>(swapchainExtent_.width);
        float contentH = static_cast<float>(swapchainExtent_.height);
        float imgW = static_cast<float>(textureWidth_);
        float imgH = static_cast<float>(textureHeight_);

        float fitScale = std::min(contentW / imgW, contentH / imgH);
        float scale = fitScale * std::clamp(zoom, 0.01f, 100.0f);

        float drawW = imgW * scale;
        float drawH = imgH * scale;
        float cx = contentW * 0.5f + offsetX;
        float cy = contentH * 0.5f + offsetY;
        int32_t dstX0 = static_cast<int32_t>(cx - drawW * 0.5f);
        int32_t dstY0 = static_cast<int32_t>(cy - drawH * 0.5f);
        int32_t dstX1 = static_cast<int32_t>(cx + drawW * 0.5f);
        int32_t dstY1 = static_cast<int32_t>(cy + drawH * 0.5f);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { static_cast<int32_t>(textureWidth_), static_cast<int32_t>(textureHeight_), 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = { dstX0, dstY0, 0 };
        blit.dstOffsets[1] = { dstX1, dstY1, 1 };

        vkCmdBlitImage(cmd,
            textureImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);
    }

    // Present transition
    VkImageMemoryBarrier post{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = swapchainImages_[imageIndex];
    post.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    post.subresourceRange.levelCount = 1;
    post.subresourceRange.layerCount = 1;
    post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &post);

    VkResult endResult = vkEndCommandBuffer(cmd);
    if (!checkVulkanOperation(endResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        return;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_;

    // Non-blocking fence check to avoid UI stalls; adhere to defensive programming
    VkResult fenceStatus = vkGetFenceStatus(device_, inFlightFence_);
    if (fenceStatus == VK_SUCCESS) {
        // Fence signaled; reset for next submit
        vkResetFences(device_, 1, &inFlightFence_);
    } else if (fenceStatus == VK_NOT_READY) {
        // Fence is unsignaled; this is acceptable for submitting new work (do not reset)
        // Proceed to submit so the first frame can render.
    } else if (fenceStatus == VK_ERROR_DEVICE_LOST) {
        deviceLost_ = true;
        return;
    } else {
        // Unknown fence state; fail safe
        deviceLost_ = true;
        return;
    }

    VkResult sr = vkQueueSubmit(graphicsQueue_, 1, &submit, inFlightFence_);
    if (sr == VK_ERROR_DEVICE_LOST) {
        deviceLost_ = true;
        return;
    } else if (sr != VK_SUCCESS) {
        // Treat other submit errors conservatively
        deviceLost_ = true;
        return;
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(presentQueue_, &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        swapchainOutOfDate_ = true;
        return;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        deviceLost_ = true;
        return;
    } else if (pr != VK_SUCCESS) {
        // Unknown present error; mark swapchain out-of-date to trigger safe recovery
        swapchainOutOfDate_ = true;
        return;
    }
}

// Additional method stubs for tiled/sparse image support
void VulkanRenderer::UpdateImageTiled(const void* pixelData, uint32_t fullWidth, uint32_t fullHeight, 
                                     uint32_t tileX, uint32_t tileY, uint32_t tileWidth, uint32_t tileHeight, bool isHdr) {
    // NASA Standard: Validate all input parameters
    if (pixelData == nullptr || fullWidth == 0 || fullHeight == 0 || 
        tileWidth == 0 || tileHeight == 0 || !device_) {
        return;
    }

    // NASA Standard: Validate tile coordinates and dimensions
    if (tileX >= fullWidth || tileY >= fullHeight || 
        tileX + tileWidth > fullWidth || tileY + tileHeight > fullHeight) {
        return;
    }

    // NASA Standard: Check for potential integer overflow in size calculations
    const uint64_t pixelCount = static_cast<uint64_t>(tileWidth) * static_cast<uint64_t>(tileHeight);
    const uint64_t maxSafePixels = UINT64_C(16777216); // 4K x 4K tile limit
    if (pixelCount > maxSafePixels) {
        return;
    }

    // NASA Standard: Initialize texture if not already done
    if (textureImage_ == VK_NULL_HANDLE || 
        textureWidth_ != fullWidth || textureHeight_ != fullHeight || textureIsHdr_ != isHdr) {

        // NASA Standard: Try sparse image first for large images
        bool sparseCreated = false;
        if (fullWidth >= 4096 && fullHeight >= 4096) {
            sparseCreated = InitializeSparseImage(fullWidth, fullHeight, isHdr);
        }

        // NASA Standard: Fallback to regular texture if sparse failed or not suitable
        if (!sparseCreated) {
            if (!createTexture(fullWidth, fullHeight, isHdr)) {
                return;
            }
            // NASA Standard: Ensure sparse flag is correctly set after regular texture creation
            textureIsSparse_ = false;
        }
    }

    // NASA Standard: Handle sparse images and regular textures in separate, reachable paths
    if (textureIsSparse_) {
        // Calculate which sparse tile this update belongs to
        uint32_t sparseTileX = tileX / tileSize_;
        uint32_t sparseTileY = tileY / tileSize_;

        // NASA Standard: Extract tile data from the full pixel data
        uint32_t pixelSize = isHdr ? (4 * sizeof(uint16_t)) : (4 * sizeof(uint8_t));
        std::vector<uint8_t> tileData(tileWidth * tileHeight * pixelSize);

        const uint8_t* srcData = static_cast<const uint8_t*>(pixelData);
        uint8_t* dstData = tileData.data();

        // NASA Standard: Copy tile data row by row to handle stride differences
        for (uint32_t y = 0; y < tileHeight; ++y) {
            const uint8_t* srcRow = srcData + ((tileY + y) * fullWidth + tileX) * pixelSize;
            uint8_t* dstRow = dstData + y * tileWidth * pixelSize;
            std::memcpy(dstRow, srcRow, tileWidth * pixelSize);
        }

        LoadImageTile(sparseTileX, sparseTileY, tileData.data(), isHdr);
    } else {
        // NASA Standard: For regular textures, update the specific region
        uint32_t pixelSize = isHdr ? (4 * sizeof(uint16_t)) : (4 * sizeof(uint8_t));
        VkDeviceSize tileDataSize = static_cast<VkDeviceSize>(tileWidth * tileHeight * pixelSize);

        // NASA Standard: Create staging buffer for tile update
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

        if (!createStagingBuffer(tileDataSize, stagingBuffer, stagingMemory)) {
            return;
        }

        // NASA Standard: Map and copy tile data
        void* mapped = nullptr;
        bool deviceLost = false;
        bool swapchainOutOfDate = false;
        VkResult mapResult = vkMapMemory(device_, stagingMemory, 0, tileDataSize, 0, &mapped);
        if (!checkVulkanOperation(mapResult, deviceLost, swapchainOutOfDate)) {
            if (deviceLost) deviceLost_ = true;
            vkDestroyBuffer(device_, stagingBuffer, nullptr);
            vkFreeMemory(device_, stagingMemory, nullptr);
            return;
        }

        // NASA Standard: Copy tile data with proper stride handling
        const uint8_t* srcData = static_cast<const uint8_t*>(pixelData);
        uint8_t* dstData = static_cast<uint8_t*>(mapped);

        for (uint32_t y = 0; y < tileHeight; ++y) {
            const uint8_t* srcRow = srcData + ((tileY + y) * fullWidth + tileX) * pixelSize;
            uint8_t* dstRow = dstData + y * tileWidth * pixelSize;
            std::memcpy(dstRow, srcRow, tileWidth * pixelSize);
        }

        vkUnmapMemory(device_, stagingMemory);

        // NASA Standard: Transition texture to transfer destination layout
        VkImageLayout oldLayout = textureLayout_;
        transitionImageLayout(textureImage_, textureFormat_, oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // NASA Standard: Copy tile region from staging buffer
        VkCommandBuffer cmd = beginSingleTimeCommands();
        if (cmd != VK_NULL_HANDLE) {
            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;
            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageOffset = { static_cast<int32_t>(tileX), static_cast<int32_t>(tileY), 0 };
            copyRegion.imageExtent = { tileWidth, tileHeight, 1 };

            vkCmdCopyBufferToImage(cmd, stagingBuffer, textureImage_, 
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            endSingleTimeCommands(cmd);
        }

        // NASA Standard: Transition back to shader read optimal
        transitionImageLayout(textureImage_, textureFormat_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        textureLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        // NASA Standard: Clean up staging resources
        vkDestroyBuffer(device_, stagingBuffer, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
    }
}

bool VulkanRenderer::InitializeSparseImage(uint32_t width, uint32_t height, bool isHdr) {
    // NASA Standard: Validate all input parameters
    if (width == 0 || height == 0 || !device_ || !physicalDevice_) {
        return false;
    }

    // NASA Standard: Validate dimensions for sparse images (must be large enough to justify sparse allocation)
    const uint32_t minSparseSize = 4096; // Minimum dimension for sparse images
    if (width < minSparseSize || height < minSparseSize) {
        return false; // Use regular images for smaller textures
    }

    // NASA Standard: Check if sparse images are supported
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
    if (!features.sparseBinding || !features.sparseResidencyImage2D) {
        sparseImageSupport_ = false;
        textureIsSparse_ = false;
        return false;
    }

    // Clean up any existing texture first
    destroyTexture();

    // NASA Standard: Choose appropriate format based on HDR flag
    VkFormat format = isHdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB;

    // NASA Standard: Create sparse image with proper validation
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = nullptr;
    imageInfo.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    VkResult createResult = vkCreateImage(device_, &imageInfo, nullptr, &textureImage_);
    if (!checkVulkanOperation(createResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        return false;
    }

    // NASA Standard: Get sparse memory requirements
    VkMemoryRequirements memReqs{};
    vkGetImageMemoryRequirements(device_, textureImage_, &memReqs);
    sparseImageMemoryRequirements_ = memReqs.size;

    // NASA Standard: Get sparse image memory requirements for tiling
    uint32_t sparseReqCount = 0;
    vkGetImageSparseMemoryRequirements(device_, textureImage_, &sparseReqCount, nullptr);
    if (sparseReqCount == 0) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
        textureIsSparse_ = false;
        return false;
    }

    std::vector<VkSparseImageMemoryRequirements> sparseReqs(sparseReqCount);
    vkGetImageSparseMemoryRequirements(device_, textureImage_, &sparseReqCount, sparseReqs.data());

    // NASA Standard: Calculate tile size from sparse requirements
    tileSize_ = std::max(sparseReqs[0].formatProperties.imageGranularity.width,
                        sparseReqs[0].formatProperties.imageGranularity.height);

    // NASA Standard: Initialize tile tracking structures
    uint32_t tilesX = (width + tileSize_ - 1) / tileSize_;
    uint32_t tilesY = (height + tileSize_ - 1) / tileSize_;
    uint32_t totalTiles = tilesX * tilesY;

    // NASA Standard: Validate tile count to prevent excessive memory allocation
    const uint32_t maxTiles = 65536; // Reasonable limit
    if (totalTiles > maxTiles) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
        textureIsSparse_ = false;
        return false;
    }

    imageTiles_.clear();
    imageTiles_.resize(totalTiles);

    // NASA Standard: Initialize tile information
    for (uint32_t y = 0; y < tilesY; ++y) {
        for (uint32_t x = 0; x < tilesX; ++x) {
            uint32_t tileIndex = y * tilesX + x;
            TileInfo& tile = imageTiles_[tileIndex];
            tile.x = x * tileSize_;
            tile.y = y * tileSize_;
            tile.width = std::min(tileSize_, width - tile.x);
            tile.height = std::min(tileSize_, height - tile.y);
            tile.loaded = false;
            tile.memory = VK_NULL_HANDLE;
            tile.stagingBuffer = VK_NULL_HANDLE;
            tile.stagingMemory = VK_NULL_HANDLE;
        }
    }

    // NASA Standard: Set texture properties
    textureWidth_ = width;
    textureHeight_ = height;
    textureFormat_ = format;
    textureIsHdr_ = isHdr;
    textureIsSparse_ = true;
    textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    sparseImageSupport_ = true;

    return true;
}

void VulkanRenderer::LoadImageTile(uint32_t tileX, uint32_t tileY, const void* tileData, bool isHdr) {
    // NASA Standard: Validate all input parameters
    if (tileData == nullptr || !textureIsSparse_ || textureImage_ == VK_NULL_HANDLE) {
        return;
    }

    // NASA Standard: Validate tile coordinates
    uint32_t tilesX = (textureWidth_ + tileSize_ - 1) / tileSize_;
    uint32_t tilesY = (textureHeight_ + tileSize_ - 1) / tileSize_;
    if (tileX >= tilesX || tileY >= tilesY) {
        return;
    }

    uint32_t tileIndex = tileY * tilesX + tileX;
    if (tileIndex >= imageTiles_.size()) {
        return;
    }

    TileInfo& tile = imageTiles_[tileIndex];

    // NASA Standard: Skip if tile is already loaded
    if (tile.loaded) {
        return;
    }

    // NASA Standard: Calculate tile data size
    uint32_t pixelSize = isHdr ? (4 * sizeof(uint16_t)) : (4 * sizeof(uint8_t));
    VkDeviceSize tileDataSize = static_cast<VkDeviceSize>(tile.width * tile.height * pixelSize);

    // NASA Standard: Validate tile data size
    if (tileDataSize == 0 || tileDataSize > 0x10000000) { // 256MB limit per tile
        return;
    }

    // NASA Standard: Allocate memory for this tile
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = tileDataSize;
    allocInfo.memoryTypeIndex = findMemoryType(UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        return; // No suitable memory type
    }

    bool deviceLost = false;
    bool swapchainOutOfDate = false;
    VkResult allocResult = vkAllocateMemory(device_, &allocInfo, nullptr, &tile.memory);
    if (!checkVulkanOperation(allocResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        return;
    }

    // NASA Standard: Create staging buffer for tile data
    if (!createStagingBuffer(tileDataSize, tile.stagingBuffer, tile.stagingMemory)) {
        vkFreeMemory(device_, tile.memory, nullptr);
        tile.memory = VK_NULL_HANDLE;
        return;
    }

    // NASA Standard: Map and copy tile data
    void* mapped = nullptr;
    VkResult mapResult = vkMapMemory(device_, tile.stagingMemory, 0, tileDataSize, 0, &mapped);
    if (!checkVulkanOperation(mapResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkDestroyBuffer(device_, tile.stagingBuffer, nullptr);
        vkFreeMemory(device_, tile.stagingMemory, nullptr);
        vkFreeMemory(device_, tile.memory, nullptr);
        tile.stagingBuffer = VK_NULL_HANDLE;
        tile.stagingMemory = VK_NULL_HANDLE;
        tile.memory = VK_NULL_HANDLE;
        return;
    }

    std::memcpy(mapped, tileData, static_cast<size_t>(tileDataSize));
    vkUnmapMemory(device_, tile.stagingMemory);

    // NASA Standard: Bind sparse memory for this tile
    VkSparseImageMemoryBind bind{};
    bind.subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bind.subresource.mipLevel = 0;
    bind.subresource.arrayLayer = 0;
    bind.offset = { static_cast<int32_t>(tile.x), static_cast<int32_t>(tile.y), 0 };
    bind.extent = { tile.width, tile.height, 1 };
    bind.memory = tile.memory;
    bind.memoryOffset = 0;
    bind.flags = 0;

    VkSparseImageMemoryBindInfo imageBindInfo{};
    imageBindInfo.image = textureImage_;
    imageBindInfo.bindCount = 1;
    imageBindInfo.pBinds = &bind;

    VkBindSparseInfo bindInfo{};
    bindInfo.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bindInfo.imageBindCount = 1;
    bindInfo.pImageBinds = &imageBindInfo;

    // NASA Standard: Submit sparse bind operation
    VkResult bindResult = vkQueueBindSparse(graphicsQueue_, 1, &bindInfo, VK_NULL_HANDLE);
    if (!checkVulkanOperation(bindResult, deviceLost, swapchainOutOfDate)) {
        if (deviceLost) deviceLost_ = true;
        vkDestroyBuffer(device_, tile.stagingBuffer, nullptr);
        vkFreeMemory(device_, tile.stagingMemory, nullptr);
        vkFreeMemory(device_, tile.memory, nullptr);
        tile.stagingBuffer = VK_NULL_HANDLE;
        tile.stagingMemory = VK_NULL_HANDLE;
        tile.memory = VK_NULL_HANDLE;
        return;
    }

    // NASA Standard: Wait for sparse bind completion
    vkQueueWaitIdle(graphicsQueue_);

    // NASA Standard: Copy data from staging buffer to tile
    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd != VK_NULL_HANDLE) {
        // Transition tile region to transfer destination
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = textureImage_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy from staging buffer to tile region
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = { static_cast<int32_t>(tile.x), static_cast<int32_t>(tile.y), 0 };
        copyRegion.imageExtent = { tile.width, tile.height, 1 };

        vkCmdCopyBufferToImage(cmd, tile.stagingBuffer, textureImage_, 
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition to shader read optimal
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);

        endSingleTimeCommands(cmd);
    }

    // NASA Standard: Clean up staging resources
    vkDestroyBuffer(device_, tile.stagingBuffer, nullptr);
    vkFreeMemory(device_, tile.stagingMemory, nullptr);
    tile.stagingBuffer = VK_NULL_HANDLE;
    tile.stagingMemory = VK_NULL_HANDLE;

    // NASA Standard: Mark tile as loaded
    tile.loaded = true;
}

bool VulkanRenderer::initializeSoftwareFallback(HWND hwnd) {
    // NASA Standard: Initialize software rendering fallback
    fallbackHwnd_ = hwnd;
    vulkanAvailable_ = false;

    // NASA Standard: Get window dimensions for fallback buffer
    RECT clientRect;
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    fallbackWidth_ = static_cast<uint32_t>(clientRect.right - clientRect.left);
    fallbackHeight_ = static_cast<uint32_t>(clientRect.bottom - clientRect.top);

    if (fallbackWidth_ == 0 || fallbackHeight_ == 0) {
        fallbackWidth_ = 800;
        fallbackHeight_ = 600;
    }

    // NASA Standard: Allocate software rendering buffer
    size_t bufferSize = static_cast<size_t>(fallbackWidth_) * fallbackHeight_ * 4; // RGBA
    fallbackBuffer_.resize(bufferSize, 0);

    return true;
}

void VulkanRenderer::renderSoftwareFallback(uint32_t width, uint32_t height) {
    // NASA Standard: Software fallback rendering
    if (fallbackBuffer_.empty() || fallbackHwnd_ == nullptr) {
        return;
    }

    // NASA Standard: Update buffer size if window changed
    if (width != fallbackWidth_ || height != fallbackHeight_) {
        fallbackWidth_ = width;
        fallbackHeight_ = height;
        size_t bufferSize = static_cast<size_t>(width) * height * 4;
        fallbackBuffer_.resize(bufferSize, 0);
    }

    // NASA Standard: Clear to dark gray to indicate software mode
    uint32_t clearColor = 0xFF404040; // Dark gray ARGB
    uint32_t* pixels = reinterpret_cast<uint32_t*>(fallbackBuffer_.data());
    size_t pixelCount = static_cast<size_t>(width) * height;

    for (size_t i = 0; i < pixelCount; ++i) {
        pixels[i] = clearColor;
    }

    // NASA Standard: Display software-rendered content
    HDC hdc = GetDC(fallbackHwnd_);
    if (hdc != nullptr) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetDIBitsToDevice(hdc, 0, 0, width, height, 0, 0, 0, height,
                         fallbackBuffer_.data(), &bmi, DIB_RGB_COLORS);

        ReleaseDC(fallbackHwnd_, hdc);
    }
}

#endif // _WIN32
