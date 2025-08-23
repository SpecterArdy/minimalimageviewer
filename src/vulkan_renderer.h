#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#else
#include <vulkan/vulkan.h>
#endif

#include <vector>
#include <cstdint>

// Structure for sparse image tile information
struct TileInfo {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool loaded = false;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
};

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    // Optional progress callback: percent in [0,100], and a stage description
    using ProgressCallback = void(*)(int percent, const wchar_t* stage);

    bool Initialize(HWND hwnd);
    // Progress-enabled initialization (reports fine-grained steps)
    bool InitializeWithProgress(HWND hwnd, ProgressCallback cb);

    void Shutdown();
    void Resize(uint32_t width, uint32_t height);
    void Render(uint32_t width, uint32_t height, float zoom, float offsetX, float offsetY, int rotationAngle);

    void UpdateImageFromData(const void* pixelData, uint32_t width, uint32_t height, bool isHdr);
    void UpdateImageFromHBITMAP(HBITMAP hBitmap);
    void UpdateImageFromLDRData(const void* pixelData, uint32_t width, uint32_t height, bool generateMipmaps = false);
    void UpdateImageFromHDRData(const uint16_t* pixelData, uint32_t width, uint32_t height, bool generateMipmaps = false);
    void UpdateImageTiled(const void* pixelData, uint32_t fullWidth, uint32_t fullHeight, 
                         uint32_t tileX, uint32_t tileY, uint32_t tileWidth, uint32_t tileHeight, bool isHdr);

    void SetColorTransform(void* processor);

    // Error state accessors
    bool IsDeviceLost() const { return deviceLost_; }
    bool IsSwapchainOutOfDate() const { return swapchainOutOfDate_; }
    // Clear transient error flags after successful recovery (e.g., swapchain recreation)
    void ClearErrorFlags() { deviceLost_ = false; swapchainOutOfDate_ = false; }

private:
    // Vulkan objects
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    uint32_t graphicsQueueFamily_ = UINT32_MAX;
    uint32_t presentQueueFamily_ = UINT32_MAX;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkCommandBuffer> commandBuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    
    // Per-frame synchronization objects
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;
    
    // Legacy synchronization objects (for cleanup compatibility)
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence inFlightFence_ = VK_NULL_HANDLE;

    // Texture data
    VkImage textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkFormat textureFormat_ = VK_FORMAT_UNDEFINED;
    VkImageLayout textureLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t textureWidth_ = 0;
    uint32_t textureHeight_ = 0;
    bool textureIsHdr_ = false;
    bool textureIsSparse_ = false;

    // Sparse image support
    bool sparseImageSupport_ = false;
    uint32_t tileSize_ = 256;
    VkDeviceSize sparseImageMemoryRequirements_ = 0;
    std::vector<TileInfo> imageTiles_;

    // Error tracking
    bool deviceLost_ = false;
    bool swapchainOutOfDate_ = false;
    bool vulkanAvailable_ = false;
    
    // Enhanced device lost diagnostics
    void LogDeviceLostDiagnostics(const char* context) const;
    void LogVulkanObjectState() const;

    // Library handle for Windows
    HMODULE vulkanLibrary_ = nullptr;

    // Software fallback
    HWND fallbackHwnd_ = nullptr;
    uint32_t fallbackWidth_ = 800;
    uint32_t fallbackHeight_ = 600;
    std::vector<uint8_t> fallbackBuffer_;

    void* colorProcessor_ = nullptr;

    // Helper functions
    bool initInstance();
    bool pickPhysicalDevice();
    bool createDeviceAndQueues();
    bool createSurface(HWND hwnd);
    bool createCommandPool();
    bool createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    bool createSyncObjects();
    void recreateSwapchain(uint32_t width, uint32_t height);

    bool createTexture(uint32_t width, uint32_t height, bool isHdr);
    void destroyTexture();
    bool createStagingBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory);
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

    // Sparse image functions
    bool InitializeSparseImage(uint32_t width, uint32_t height, bool isHdr);
    void LoadImageTile(uint32_t tileX, uint32_t tileY, const void* tileData, bool isHdr);

    // Software fallback functions
    bool initializeSoftwareFallback(HWND hwnd);
    void renderSoftwareFallback(uint32_t width, uint32_t height);

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
};
