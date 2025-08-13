#include "viewer.h"
#include "vulkan_renderer.h"

// Default constructor/destructor can be defaulted now that VulkanRenderer is complete here.
AppContext::AppContext() = default;
AppContext::~AppContext() = default;

// Copy: copy everything except the renderer (leave null in the copy)
AppContext::AppContext(const AppContext& other)
    : hInst(other.hInst),
      hWnd(other.hWnd),
      hBitmap(other.hBitmap),
      wicFactory(other.wicFactory),
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
      showFilePath(other.showFilePath),
      currentFilePathOverride(other.currentFilePathOverride),
      isHoveringClose(other.isHoveringClose) {}

AppContext& AppContext::operator=(const AppContext& other) {
    if (this != &other) {
        hInst = other.hInst;
        hWnd = other.hWnd;
        hBitmap = other.hBitmap;
        wicFactory = other.wicFactory;
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
        showFilePath = other.showFilePath;
        currentFilePathOverride = other.currentFilePathOverride;
        isHoveringClose = other.isHoveringClose;
    }
    return *this;
}

// Moves can be defaulted now that the type is complete here.
AppContext::AppContext(AppContext&&) noexcept = default;
AppContext& AppContext::operator=(AppContext&&) noexcept = default;
