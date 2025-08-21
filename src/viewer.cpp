#include "viewer.h"
#include "vulkan_renderer.h"

// Default constructor/destructor can be defaulted now that VulkanRenderer is complete here.
AppContext::AppContext() = default;
AppContext::~AppContext() = default;

// Copy: copy everything except the renderer (leave null in the copy)
AppContext::AppContext(const AppContext& other)
    : hInst(other.hInst),
      hWnd(other.hWnd),
      imageData(other.imageData),
      imageFiles(other.imageFiles),
      currentImageIndex(other.currentImageIndex),
      zoomFactor(other.zoomFactor),
      rotationAngle(other.rotationAngle),
      offsetX(other.offsetX),
      offsetY(other.offsetY),
      isFullScreen(other.isFullScreen),
      savedStyle(other.savedStyle),
      savedRect(other.savedRect),
      renderer(nullptr),
      ocioConfig(other.ocioConfig),
      currentDisplayTransform(other.currentDisplayTransform),
      displayDevice(other.displayDevice),
      showFilePath(other.showFilePath),
      currentFilePathOverride(other.currentFilePathOverride),
      isHoveringClose(other.isHoveringClose),
      showFps(other.showFps),
      fpsLastTimeMS(other.fpsLastTimeMS),
      fpsFrameCount(other.fpsFrameCount),
      fps(other.fps),
      rendererNeedsReset(other.rendererNeedsReset) 
{
    // Reinitialize synchronization primitives for this new instance
    InitializeSRWLock(&renderLock);
    renderInProgress.store(false, std::memory_order_relaxed);
}

AppContext& AppContext::operator=(const AppContext& other) {
    if (this != &other) {
        hInst = other.hInst;
        hWnd = other.hWnd;
        imageData = other.imageData;
        imageFiles = other.imageFiles;
        currentImageIndex = other.currentImageIndex;
        zoomFactor = other.zoomFactor;
        rotationAngle = other.rotationAngle;
        offsetX = other.offsetX;
        offsetY = other.offsetY;
        isFullScreen = other.isFullScreen;
        savedStyle = other.savedStyle;
        savedRect = other.savedRect;
        // renderer is not copied; ensure null
        renderer.reset();
        ocioConfig = other.ocioConfig;
        currentDisplayTransform = other.currentDisplayTransform;
        displayDevice = other.displayDevice;
        showFilePath = other.showFilePath;
        currentFilePathOverride = other.currentFilePathOverride;
        isHoveringClose = other.isHoveringClose;
        showFps = other.showFps;
        fpsLastTimeMS = other.fpsLastTimeMS;
        fpsFrameCount = other.fpsFrameCount;
        fps = other.fps;
        rendererNeedsReset = other.rendererNeedsReset;
    }
    return *this;
}

// Custom moves: std::atomic and SRWLOCK are not movable; reinitialize them safely.
AppContext::AppContext(AppContext&& other) noexcept
    : hInst(other.hInst),
      hWnd(other.hWnd),
      imageData(std::move(other.imageData)),
      imageFiles(std::move(other.imageFiles)),
      currentImageIndex(other.currentImageIndex),
      zoomFactor(other.zoomFactor),
      rotationAngle(other.rotationAngle),
      offsetX(other.offsetX),
      offsetY(other.offsetY),
      isFullScreen(other.isFullScreen),
      savedStyle(other.savedStyle),
      savedRect(other.savedRect),
      renderer(std::move(other.renderer)),
      ocioConfig(other.ocioConfig),
      currentDisplayTransform(other.currentDisplayTransform),
      displayDevice(std::move(other.displayDevice)),
      showFilePath(other.showFilePath),
      currentFilePathOverride(std::move(other.currentFilePathOverride)),
      isHoveringClose(other.isHoveringClose),
      showFps(other.showFps),
      fpsLastTimeMS(other.fpsLastTimeMS),
      fpsFrameCount(other.fpsFrameCount),
      fps(other.fps),
      rendererNeedsReset(other.rendererNeedsReset)
{
    InitializeSRWLock(&renderLock);
    renderInProgress.store(false, std::memory_order_relaxed);

    // Leave source in benign state
    other.hWnd = nullptr;
    other.rendererNeedsReset = false;
}

AppContext& AppContext::operator=(AppContext&& other) noexcept {
    if (this != &other) {
        hInst = other.hInst;
        hWnd = other.hWnd;
        imageData = std::move(other.imageData);
        imageFiles = std::move(other.imageFiles);
        currentImageIndex = other.currentImageIndex;
        zoomFactor = other.zoomFactor;
        rotationAngle = other.rotationAngle;
        offsetX = other.offsetX;
        offsetY = other.offsetY;
        isFullScreen = other.isFullScreen;
        savedStyle = other.savedStyle;
        savedRect = other.savedRect;
        renderer = std::move(other.renderer);
        ocioConfig = other.ocioConfig;
        currentDisplayTransform = other.currentDisplayTransform;
        displayDevice = std::move(other.displayDevice);
        showFilePath = other.showFilePath;
        currentFilePathOverride = std::move(other.currentFilePathOverride);
        isHoveringClose = other.isHoveringClose;
        showFps = other.showFps;
        fpsLastTimeMS = other.fpsLastTimeMS;
        fpsFrameCount = other.fpsFrameCount;
        fps = other.fps;
        rendererNeedsReset = other.rendererNeedsReset;

        // Reinitialize our sync primitives/flags
        InitializeSRWLock(&renderLock);
        renderInProgress.store(false, std::memory_order_relaxed);

        other.hWnd = nullptr;
        other.rendererNeedsReset = false;
    }
    return *this;
}
