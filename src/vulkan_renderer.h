#pragma once

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#endif

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

#ifdef _WIN32
    bool Initialize(HWND hwnd);
#endif
    void Shutdown();

    // Render to the current swapchain. Will recreate swapchain if size changed.
    void Render(uint32_t width, uint32_t height, float zoom, float offsetX, float offsetY);

#ifdef _WIN32
    // Resize swapchain on window size change
    void Resize(uint32_t width, uint32_t height);
    // Upload BGRA8 pixels from an HBITMAP into an internal VkImage (recreates per size)
    void UpdateImageFromHBITMAP(HBITMAP hBitmap);
#endif

private:
#ifdef _WIN32
    bool initInstance();
    bool pickPhysicalDevice();
    bool createDeviceAndQueues();
    bool createSurface(HWND hwnd);
    bool createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    bool createCommandPool();
    bool createSyncObjects();
    void recreateSwapchain(uint32_t width, uint32_t height);

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);

    bool createTexture(uint32_t width, uint32_t height);
    void destroyTexture();

    // Vulkan core
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = UINT32_MAX;
    uint32_t presentQueueFamily_ = UINT32_MAX;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR swapchainColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    // Commands and sync
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence inFlightFence_ = VK_NULL_HANDLE;

    // Texture for the image
    VkImage textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkImageLayout textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t textureWidth_ = 0;
    uint32_t textureHeight_ = 0;
#endif
};
