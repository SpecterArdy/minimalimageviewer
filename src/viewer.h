#pragma once

#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <memory>
#include <atomic>

#include "resource.h"

// OpenImageIO and OpenColorIO
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

class VulkanRenderer;

struct ImageData {
    std::vector<uint8_t> pixels;        // Unified pixel data (RGBA8 for LDR, interpreted as RGBA16F for HDR)
    uint32_t width = 0;
    uint32_t height = 0;
    bool isHdr = false;
    uint32_t channels = 4; // Always RGBA

    // OpenColorIO color space information
    std::string sourceColorSpace = "sRGB";        // Original color space from file
    std::string workingColorSpace = "Linear Rec.709 (sRGB)"; // Working space for processing
    OCIO::ConstProcessorRcPtr colorTransform;

    // Image metadata
    float exposure = 0.0f;        // Exposure compensation
    float gamma = 2.2f;          // Gamma correction
    bool isTiled = false;        // Whether image uses tiled loading
    bool isSparse = false;       // Whether image uses sparse memory
    uint32_t tileSize = 512;     // Tile size for large images

    bool isValid() const { 
        return width > 0 && height > 0 && !pixels.empty(); 
    }

    void clear() { 
        pixels.clear();
        width = 0; 
        height = 0; 
        isHdr = false; 
        sourceColorSpace = "sRGB";
        workingColorSpace = "Linear Rec.709 (sRGB)";
        colorTransform.reset();
        exposure = 0.0f;
        gamma = 2.2f;
        isTiled = false;
        isSparse = false;
        tileSize = 512;
    }
};

struct AppContext {
    HINSTANCE hInst = nullptr;
    HWND hWnd = nullptr;
    ImageData imageData;

    std::vector<std::wstring> imageFiles;
    int currentImageIndex = -1;
    
    float zoomFactor = 1.0f;
    int rotationAngle = 0;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    bool isFullScreen = false;
    LONG savedStyle = 0;
    RECT savedRect{};

    // Vulkan renderer (initialized after window creation)
    std::unique_ptr<VulkanRenderer> renderer;

    // OpenColorIO context for color management
    OCIO::ConstConfigRcPtr ocioConfig;
    OCIO::ConstProcessorRcPtr currentDisplayTransform;
    std::string displayDevice = "sRGB";
    bool ocioEnabled = false;

    bool showFilePath = false;
    std::wstring currentFilePathOverride;
    bool isHoveringClose = false;

    // FPS counter
    bool showFps = true;
    unsigned long long fpsLastTimeMS = 0;
    int fpsFrameCount = 0;
    float fps = 0.0f;

    // Renderer maintenance
    bool rendererNeedsReset = false;

    // Synchronization: reader-writer lock for safe renderer access/reset
    SRWLOCK renderLock = SRWLOCK_INIT;

    // Tracks whether a render is currently issuing Vulkan commands
    std::atomic<bool> renderInProgress{false};

    // Declarations only; definitions are out-of-line where VulkanRenderer is complete
    AppContext();
    ~AppContext();

    AppContext(const AppContext& other);
    AppContext& operator=(const AppContext& other);

    AppContext(AppContext&&) noexcept;
    AppContext& operator=(AppContext&&) noexcept;
};

//
// Function Prototypes
//

// main.cpp
void CenterImage(bool resetZoom);

// ui_handlers.cpp  
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// Forward declaration for functions that might be missing
void FitImageToWindow();
void ZoomImage(float factor);
void RotateImage(bool clockwise);

// image_io.cpp
void LoadImageFromFile(const wchar_t* filePath);
void GetImagesInDirectory(const wchar_t* filePath);
void SaveImage();
void SaveImageAs();
void DeleteCurrentImage();
void HandleDropFiles(HDROP hDrop);
void HandlePaste();
void HandleCopy();
void OpenFileLocationAction();

// image_drawing.cpp
void DrawImage(HDC hdc, const RECT& clientRect, const AppContext& ctx);
void FitImageToWindow();
void ZoomImage(float factor);
void RotateImage(bool clockwise);
bool IsPointInImage(POINT pt, const RECT& clientRect);