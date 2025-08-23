#include "viewer.h"
#include "vulkan_renderer.h"

// Default constructor/destructor with SDL3 initialization
AppContext::AppContext() {
    // Don't create mutex in constructor - SDL may not be initialized yet
    // Will be created later in main() after SDL_Init()
    renderLock = nullptr;
    renderInProgress.store(false, std::memory_order_relaxed);
}

AppContext::~AppContext() {
    if (renderLock) {
        SDL_DestroyMutex(renderLock);
    }
}

// Copy: copy everything except the renderer (leave null in the copy)
AppContext::AppContext(const AppContext& other)
    : window(other.window),
      imageData(other.imageData),
      imageFiles(other.imageFiles),
      currentImageIndex(other.currentImageIndex),
      zoomFactor(other.zoomFactor),
      rotationAngle(other.rotationAngle),
      offsetX(other.offsetX),
      offsetY(other.offsetY),
      isFullScreen(other.isFullScreen),
      savedWindowRect(other.savedWindowRect),
      savedMaximized(other.savedMaximized),
      renderer(nullptr),
      ocioConfig(other.ocioConfig),
      currentDisplayTransform(other.currentDisplayTransform),
      displayDevice(other.displayDevice),
      ocioEnabled(other.ocioEnabled),
      showFilePath(other.showFilePath),
      currentFilePathOverride(other.currentFilePathOverride),
      isHoveringClose(other.isHoveringClose),
      showFps(other.showFps),
      fpsLastTimeMS(other.fpsLastTimeMS),
      fpsFrameCount(other.fpsFrameCount),
      fps(other.fps),
      rendererNeedsReset(other.rendererNeedsReset) 
{
    // Create new mutex for this instance
    renderLock = SDL_CreateMutex();
    renderInProgress.store(false, std::memory_order_relaxed);
}

AppContext& AppContext::operator=(const AppContext& other) {
    if (this != &other) {
        window = other.window;
        imageData = other.imageData;
        imageFiles = other.imageFiles;
        currentImageIndex = other.currentImageIndex;
        zoomFactor = other.zoomFactor;
        rotationAngle = other.rotationAngle;
        offsetX = other.offsetX;
        offsetY = other.offsetY;
        isFullScreen = other.isFullScreen;
        savedWindowRect = other.savedWindowRect;
        savedMaximized = other.savedMaximized;
        // renderer is not copied; ensure null
        renderer.reset();
        ocioConfig = other.ocioConfig;
        currentDisplayTransform = other.currentDisplayTransform;
        displayDevice = other.displayDevice;
        ocioEnabled = other.ocioEnabled;
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

// Custom moves: std::atomic and SDL_Mutex are not movable; reinitialize them safely.
AppContext::AppContext(AppContext&& other) noexcept
    : window(other.window),
      imageData(std::move(other.imageData)),
      imageFiles(std::move(other.imageFiles)),
      currentImageIndex(other.currentImageIndex),
      zoomFactor(other.zoomFactor),
      rotationAngle(other.rotationAngle),
      offsetX(other.offsetX),
      offsetY(other.offsetY),
      isFullScreen(other.isFullScreen),
      savedWindowRect(other.savedWindowRect),
      savedMaximized(other.savedMaximized),
      renderer(std::move(other.renderer)),
      ocioConfig(other.ocioConfig),
      currentDisplayTransform(other.currentDisplayTransform),
      displayDevice(std::move(other.displayDevice)),
      ocioEnabled(other.ocioEnabled),
      showFilePath(other.showFilePath),
      currentFilePathOverride(std::move(other.currentFilePathOverride)),
      isHoveringClose(other.isHoveringClose),
      showFps(other.showFps),
      fpsLastTimeMS(other.fpsLastTimeMS),
      fpsFrameCount(other.fpsFrameCount),
      fps(other.fps),
      rendererNeedsReset(other.rendererNeedsReset)
{
    renderLock = SDL_CreateMutex();
    renderInProgress.store(false, std::memory_order_relaxed);

    // Leave source in benign state
    other.window = nullptr;
    other.rendererNeedsReset = false;
}

AppContext& AppContext::operator=(AppContext&& other) noexcept {
    if (this != &other) {
        // Clean up existing mutex
        if (renderLock) {
            SDL_DestroyMutex(renderLock);
        }
        
        window = other.window;
        imageData = std::move(other.imageData);
        imageFiles = std::move(other.imageFiles);
        currentImageIndex = other.currentImageIndex;
        zoomFactor = other.zoomFactor;
        rotationAngle = other.rotationAngle;
        offsetX = other.offsetX;
        offsetY = other.offsetY;
        isFullScreen = other.isFullScreen;
        savedWindowRect = other.savedWindowRect;
        savedMaximized = other.savedMaximized;
        renderer = std::move(other.renderer);
        ocioConfig = other.ocioConfig;
        currentDisplayTransform = other.currentDisplayTransform;
        displayDevice = std::move(other.displayDevice);
        ocioEnabled = other.ocioEnabled;
        showFilePath = other.showFilePath;
        currentFilePathOverride = std::move(other.currentFilePathOverride);
        isHoveringClose = other.isHoveringClose;
        showFps = other.showFps;
        fpsLastTimeMS = other.fpsLastTimeMS;
        fpsFrameCount = other.fpsFrameCount;
        fps = other.fps;
        rendererNeedsReset = other.rendererNeedsReset;

        // Reinitialize our sync primitives/flags
        renderLock = SDL_CreateMutex();
        renderInProgress.store(false, std::memory_order_relaxed);

        other.window = nullptr;
        other.rendererNeedsReset = false;
    }
    return *this;
}
