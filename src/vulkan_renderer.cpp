#include "vulkan_renderer.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>

#ifdef _WIN32

static void check(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) throw std::runtime_error(msg);
}

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { Shutdown(); }

bool VulkanRenderer::initInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;

    return vkCreateInstance(&ci, nullptr, &instance_) == VK_SUCCESS;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());

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

bool VulkanRenderer::createDeviceAndQueues() {
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
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandle(nullptr);
    sci.hwnd = hwnd;
    return vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_) == VK_SUCCESS;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("No suitable memory type");
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = graphicsQueueFamily_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return vkCreateCommandPool(device_, &pci, nullptr, &commandPool_) == VK_SUCCESS;
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = commandPool_;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device_, &a, &cmd), "alloc cmd");

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi), "begin cmd");
    return cmd;
}

void VulkanRenderer::endSingleTimeCommands(VkCommandBuffer cmd) {
    check(vkEndCommandBuffer(cmd), "end cmd");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE), "submit cmd");
    vkQueueWaitIdle(graphicsQueue_);
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
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
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

bool VulkanRenderer::createTexture(uint32_t width, uint32_t height) {
    destroyTexture();

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = { width, height, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
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

    if (vkAllocateMemory(device_, &ai, nullptr, &textureMemory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, textureImage_, textureMemory_, 0);

    textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    textureWidth_ = width;
    textureHeight_ = height;
    return true;
}

void VulkanRenderer::destroyTexture() {
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
}

bool VulkanRenderer::Initialize(HWND hwnd) {
    if (!initInstance()) return false;
    if (!createSurface(hwnd)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createDeviceAndQueues()) return false;
    if (!createCommandPool()) return false;
    if (!createSwapchain(800, 600)) return false;
    if (!createSyncObjects()) return false;
    return true;
}

void VulkanRenderer::Shutdown() {
    if (!device_) {
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(device_);
    destroyTexture();
    destroySwapchain();
    if (imageAvailable_) vkDestroySemaphore(device_, imageAvailable_, nullptr);
    if (renderFinished_) vkDestroySemaphore(device_, renderFinished_, nullptr);
    if (inFlightFence_) vkDestroyFence(device_, inFlightFence_, nullptr);
    if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}

void VulkanRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (swapchainExtent_.width == width && swapchainExtent_.height == height) return;
    recreateSwapchain(width, height);
}

void VulkanRenderer::UpdateImageFromHBITMAP(HBITMAP hBitmap) {
    if (!hBitmap) return;

    BITMAP bm{};
    GetObject(hBitmap, sizeof(BITMAP), &bm);
    uint32_t width = static_cast<uint32_t>(bm.bmWidth);
    uint32_t height = static_cast<uint32_t>(bm.bmHeight);
    if (width == 0 || height == 0) return;

    // Read pixels as BGRA top-down
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(width * height * 4);
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, hBitmap, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    if (textureWidth_ != width || textureHeight_ != height || textureImage_ == VK_NULL_HANDLE) {
        createTexture(width, height);
    }

    // Create staging buffer
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = pixels.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &bi, nullptr, &staging), "create buffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, staging, &req);

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &mai, nullptr, &stagingMem), "alloc staging");
    check(vkBindBufferMemory(device_, staging, stagingMem, 0), "bind staging");

    void* mapped = nullptr;
    check(vkMapMemory(device_, stagingMem, 0, bi.size, 0, &mapped), "map staging");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(device_, stagingMem);

    // Record copy
    VkCommandBuffer cmd = beginSingleTimeCommands();

    // Transition texture to TRANSFER_DST
    VkImageMemoryBarrier b1{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b1.oldLayout = textureLayout_;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = textureImage_;
    b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b1.subresourceRange.levelCount = 1;
    b1.subresourceRange.layerCount = 1;
    b1.srcAccessMask = 0;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    // Copy
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging, textureImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to TRANSFER_SRC for blitting
    VkImageMemoryBarrier b2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = textureImage_;
    b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b2.subresourceRange.levelCount = 1;
    b2.subresourceRange.layerCount = 1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);

    endSingleTimeCommands(cmd);
    textureLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

void VulkanRenderer::Render(uint32_t width, uint32_t height, float zoom, float offsetX, float offsetY) {
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
    check(vkBeginCommandBuffer(cmd, &bi), "begin");

    // Transition swapchain image to TRANSFER_DST
    VkImageMemoryBarrier pre{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

    check(vkEndCommandBuffer(cmd), "end");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_;

    check(vkQueueSubmit(graphicsQueue_, 1, &submit, inFlightFence_), "submit");

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(presentQueue_, &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain(width, height);
    }
}

#endif // _WIN32
