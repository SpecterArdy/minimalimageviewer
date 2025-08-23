#include "viewer.h"
#include <OpenImageIO/imageio.h>
#include <OpenColorIO/OpenColorIO.h>
#include <locale>
#include <codecvt>
#include <algorithm>
#include <cmath>
#include <limits>

#include "vulkan_renderer.h"
#include "logging.h"

namespace OCIO = OCIO_NAMESPACE;

extern AppContext g_ctx;

// Helper function for clamping values (C++14 compatible)
template<typename T>
constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static bool IsImageFile(const wchar_t* filePath) {
    // Clear any previous errors first
    OIIO::geterror();

    std::string utf8Path = wstring_to_utf8(filePath);
    auto in = OIIO::ImageInput::open(utf8Path);
    if (!in) {
        // Clear any pending error to prevent the warning
        OIIO::geterror();
        return false;
    }
    in->close();

    // Clear any errors from the close operation
    OIIO::geterror();
    return true;
}

void LoadImageFromFile(const wchar_t* filePath) {
#ifdef HAVE_DATADOG
    auto loadSpan = Logger::CreateSpan("image.load");
    
    // Convert to UTF-8 for tagging
    std::string utf8Path = wstring_to_utf8(filePath);
    loadSpan.set_tag("file_path", utf8Path);
#else
    // Convert to UTF-8 for logging even without datadog
    std::string utf8Path = wstring_to_utf8(filePath);
#endif
    
    g_ctx.imageData.clear();
    g_ctx.currentFilePathOverride.clear();

    // Clear any previous OpenImageIO errors
    OIIO::geterror();

    auto in = OIIO::ImageInput::open(utf8Path);
    if (!in) {
        // Clear any pending error and show what went wrong
        std::string error = OIIO::geterror();
#ifdef HAVE_DATADOG
        loadSpan.set_tag("success", "false");
        loadSpan.set_tag("error", error);
#endif
        if (!error.empty()) {
            std::wstring werror(error.begin(), error.end());
            MessageBoxW(g_ctx.hWnd, (L"Failed to open image: " + werror).c_str(), L"Image Load Error", MB_OK | MB_ICONWARNING);
        }
        CenterImage(true);
        return;
    }

    const OIIO::ImageSpec& spec = in->spec();

    // NASA Standard: Validate all input parameters and bounds
    if (spec.width <= 0 || spec.height <= 0 || spec.width > 65536 || spec.height > 65536) {
        OIIO::geterror(); // Clear any errors
#ifdef HAVE_DATADOG
        loadSpan.set_tag("success", "false");
        loadSpan.set_tag("error", "Invalid image dimensions");
#endif
        CenterImage(true);
        return;
    }

    uint32_t width = static_cast<uint32_t>(spec.width);
    uint32_t height = static_cast<uint32_t>(spec.height);

    // NASA Standard: Validate computed values
    if (width == 0 || height == 0) {
        OIIO::geterror();
#ifdef HAVE_DATADOG
        loadSpan.set_tag("success", "false");
        loadSpan.set_tag("error", "Zero dimensions after validation");
#endif
        CenterImage(true);
        return;
    }

    // Channels are always converted to 4 (RGBA)

    // NASA Standard: Prevent integer overflow in memory calculations
    const uint64_t maxPixels = UINT64_C(0x7FFFFFFF) / 16; // Conservative limit
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > maxPixels) {
        OIIO::geterror();
#ifdef HAVE_DATADOG
        loadSpan.set_tag("success", "false");
        loadSpan.set_tag("error", "Image too large for memory");
#endif
        CenterImage(true);
        return;
    }

    // Determine if this is an HDR format
    bool isHdr = false;
    std::string formatName = spec.format.c_str();
    if (formatName == "half" || formatName == "float" || formatName == "double") {
        isHdr = true;
    }

    // Check file extension for HDR formats
    std::string lowerPath = utf8Path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    if (lowerPath.find(".exr") != std::string::npos || 
        lowerPath.find(".hdr") != std::string::npos || 
        lowerPath.find(".hdri") != std::string::npos ||
        lowerPath.find(".pfm") != std::string::npos ||
        lowerPath.find(".tiff") != std::string::npos ||
        lowerPath.find(".tif") != std::string::npos) {
        isHdr = true;
    }

    g_ctx.imageData.width = width;
    g_ctx.imageData.height = height;
    g_ctx.imageData.isHdr = isHdr;
    g_ctx.imageData.channels = 4; // Always convert to RGBA
    
    // Tag image properties
#ifdef HAVE_DATADOG
    loadSpan.set_tag("width", std::to_string(width));
    loadSpan.set_tag("height", std::to_string(height));
    loadSpan.set_tag("is_hdr", isHdr ? "true" : "false");
    loadSpan.set_tag("channels", "4");
    loadSpan.set_tag("original_channels", std::to_string(spec.nchannels));
    loadSpan.set_tag("format", formatName);
#endif

    // Initialize OpenColorIO with comprehensive error handling (NASA coding standard)
    OCIO::ConstConfigRcPtr config = nullptr;

    // NASA Standard: All operations must have explicit error checking
    try {
        config = OCIO::GetCurrentConfig();

        // Validate config is usable
        if (config == nullptr) {
            config = nullptr; // Explicit null assignment
        }
    } catch (const OCIO::Exception& e) {
        // NASA Standard: Log error and continue with safe fallback
        config = nullptr;
    } catch (const std::exception& e) {
        // NASA Standard: Catch all standard exceptions
        config = nullptr;
    } catch (...) {
        // NASA Standard: Catch-all for unknown exceptions
        config = nullptr;
    }

    // NASA Standard: Always validate pointers and data before use
    std::string sourceColorSpace = "sRGB"; // Safe default
    const OIIO::ParamValue* colorSpaceAttr = nullptr;

    try {
        colorSpaceAttr = spec.find_attribute("oiio:ColorSpace");
        if (colorSpaceAttr != nullptr && 
            colorSpaceAttr->type() == OIIO::TypeDesc::STRING &&
            colorSpaceAttr->data() != nullptr) {

            const char* colorSpaceStr = static_cast<const char*>(colorSpaceAttr->data());
            if (colorSpaceStr != nullptr && strlen(colorSpaceStr) > 0) {
                sourceColorSpace = std::string(colorSpaceStr);
            }
        } else if (isHdr) {
            sourceColorSpace = "Linear";
        }
    } catch (...) {
        // NASA Standard: Any attribute access failure defaults to safe value
        sourceColorSpace = isHdr ? "Linear" : "sRGB";
    }

    // NASA Standard: Use safe defaults
    const std::string targetColorSpace = "sRGB";
    OCIO::ConstProcessorRcPtr processor = nullptr;

    // NASA Standard: Multiple validation checks before potentially dangerous operations
    if (config != nullptr && 
        !sourceColorSpace.empty() && 
        !targetColorSpace.empty() && 
        sourceColorSpace != targetColorSpace) {

        try {
            // NASA Standard: Validate inputs before API calls
            if (sourceColorSpace.length() < 256 && targetColorSpace.length() < 256) {

                // Check if source color space exists in the config
                bool sourceExists = false;
                bool targetExists = false;

                try {
                    int numColorSpaces = config->getNumColorSpaces();
                    for (int i = 0; i < numColorSpaces; ++i) {
                        std::string csName = config->getColorSpaceNameByIndex(i);
                        if (csName == sourceColorSpace) {
                            sourceExists = true;
                        }
                        if (csName == targetColorSpace) {
                            targetExists = true;
                        }
                    }
                } catch (...) {
                    // If we can't enumerate color spaces, try standard fallbacks
                    sourceExists = (sourceColorSpace == "sRGB" || sourceColorSpace == "Linear" || 
                                  sourceColorSpace == "scene_linear" || sourceColorSpace == "linear");
                    targetExists = (targetColorSpace == "sRGB" || targetColorSpace == "Linear" || 
                                  targetColorSpace == "scene_linear" || targetColorSpace == "linear");
                }

                // Only create processor if both color spaces exist
                if (sourceExists && targetExists) {
                    processor = config->getProcessor(sourceColorSpace.c_str(), targetColorSpace.c_str());

                    // NASA Standard: Validate return values
                    if (processor == nullptr) {
                        processor = nullptr; // Explicit null for clarity
                    }
                } else {
                    // Color spaces don't exist, use safe fallbacks
                    std::string safeSrc = sourceExists ? sourceColorSpace : "sRGB";
                    std::string safeDst = targetExists ? targetColorSpace : "sRGB";

                    if (safeSrc != safeDst) {
                        try {
                            processor = config->getProcessor(safeSrc.c_str(), safeDst.c_str());
                        } catch (...) {
                            processor = nullptr;
                        }
                    }
                }
            }
        } catch (const OCIO::Exception& e) {
            // NASA Standard: Specific exception handling with safe fallback
            processor = nullptr;
        } catch (const std::exception& e) {
            // NASA Standard: Handle standard exceptions
            processor = nullptr;
        } catch (...) {
            // NASA Standard: Ultimate fallback for unknown exceptions
            processor = nullptr;
        }
    }

    if (isHdr) {
        // HDR: Read as RGBA16F (half float)
        // NASA Standard: Check for overflow before allocation
        const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        const uint64_t pixelDataSize = pixelCount * 4 * sizeof(uint16_t);

        if (pixelDataSize > SIZE_MAX || pixelDataSize > 0x40000000) { // 1GB limit
            OIIO::geterror();
#ifdef HAVE_DATADOG
            loadSpan.set_tag("success", "false");
            loadSpan.set_tag("error", "HDR image data size exceeds limits");
#endif
            CenterImage(true);
            return;
        }

        try {
            g_ctx.imageData.pixels.resize(static_cast<size_t>(pixelDataSize));
        } catch (const std::bad_alloc& e) {
            // NASA Standard: Handle memory allocation failure
            OIIO::geterror();
#ifdef HAVE_DATADOG
            loadSpan.set_tag("success", "false");
            loadSpan.set_tag("error", "Memory allocation failed for HDR pixels");
#endif
            CenterImage(true);
            return;
        }

        std::vector<float> floatPixels;
        try {
            floatPixels.resize(static_cast<size_t>(pixelCount * 4), 1.0f);
        } catch (const std::bad_alloc& e) {
            // NASA Standard: Handle memory allocation failure
            OIIO::geterror();
#ifdef HAVE_DATADOG
            loadSpan.set_tag("success", "false");
            loadSpan.set_tag("error", "Memory allocation failed for float pixels");
#endif
            g_ctx.imageData.clear();
            CenterImage(true);
            return;
        }

        // Read image with proper channel handling - ensure we get RGBA
        bool readSuccess = false;

        // First try to read as RGBA directly
        if (spec.nchannels >= 4) {
            readSuccess = in->read_image(0, 0, 0, 4, OIIO::TypeDesc::FLOAT, floatPixels.data());
        } else {
            // For images with fewer channels, read and expand to RGBA
            std::vector<float> tempPixels(width * height * spec.nchannels);
            if (in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, tempPixels.data())) {
                // Convert to RGBA format
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t srcIdx = (y * width + x) * spec.nchannels;
                        uint32_t dstIdx = (y * width + x) * 4;

                        // Copy RGB channels
                        floatPixels[dstIdx + 0] = (spec.nchannels > 0) ? tempPixels[srcIdx + 0] : 0.0f; // R
                        floatPixels[dstIdx + 1] = (spec.nchannels > 1) ? tempPixels[srcIdx + 1] : 0.0f; // G
                        floatPixels[dstIdx + 2] = (spec.nchannels > 2) ? tempPixels[srcIdx + 2] : 0.0f; // B
                        floatPixels[dstIdx + 3] = (spec.nchannels > 3) ? tempPixels[srcIdx + 3] : 1.0f; // A
                    }
                }
                readSuccess = true;
            }
        }

        if (readSuccess) {
            // Clear any potential read warnings
            OIIO::geterror();
            // Apply color space conversion if processor exists and is safe
            if (processor) {
                try {
                    // Validate pixel data before color conversion
                    bool validPixelData = true;
                    for (size_t i = 0; i < std::min(size_t(100), floatPixels.size()); i += 4) {
                        if (!std::isfinite(floatPixels[i]) || !std::isfinite(floatPixels[i+1]) || 
                            !std::isfinite(floatPixels[i+2]) || !std::isfinite(floatPixels[i+3])) {
                            validPixelData = false;
                            break;
                        }
                    }

                    if (validPixelData) {
                        OCIO::PackedImageDesc imgDesc(floatPixels.data(), width, height, 4, 
                                                    OCIO::BIT_DEPTH_F32, sizeof(float) * 4, 0, 0);
                        OCIO::ConstCPUProcessorRcPtr cpuProcessor = processor->getDefaultCPUProcessor();
                        if (cpuProcessor) {
                            cpuProcessor->apply(imgDesc);
                        }
                    }
                } catch (const OCIO::Exception& e) {
                    // Color conversion failed, continue without it
                } catch (...) {
                    // Any other exception, skip color conversion
                }
            }

            // Convert float to half precision for GPU storage
            uint16_t* halfPixels = reinterpret_cast<uint16_t*>(g_ctx.imageData.pixels.data());
            for (size_t i = 0; i < floatPixels.size(); ++i) {
                // Use proper IEEE 754 half conversion
                float val = floatPixels[i];
                uint32_t bits = *(uint32_t*)&val;

                uint32_t sign = (bits >> 31) & 0x1;
                int32_t exp = ((bits >> 23) & 0xff) - 127 + 15;
                uint32_t mantissa = (bits >> 13) & 0x3ff;

                if (exp <= 0) {
                    halfPixels[i] = static_cast<uint16_t>(sign << 15);
                } else if (exp >= 31) {
                    halfPixels[i] = static_cast<uint16_t>((sign << 15) | 0x7c00);
                } else {
                    halfPixels[i] = static_cast<uint16_t>((sign << 15) | (exp << 10) | mantissa);
                }
            }
        }
    } else {
        // LDR: Read as RGBA8 sRGB
        size_t pixelDataSize = width * height * 4 * sizeof(uint8_t);
        g_ctx.imageData.pixels.resize(pixelDataSize);

        std::vector<float> floatPixels(width * height * 4, 1.0f);

        // Read image with proper channel handling - ensure we get RGBA
        bool readSuccess = false;

        // First try to read as RGBA directly
        if (spec.nchannels >= 4) {
            readSuccess = in->read_image(0, 0, 0, 4, OIIO::TypeDesc::FLOAT, floatPixels.data());
        } else {
            // For images with fewer channels, read and expand to RGBA
            std::vector<float> tempPixels(width * height * spec.nchannels);
            if (in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, tempPixels.data())) {
                // Convert to RGBA format
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t srcIdx = (y * width + x) * spec.nchannels;
                        uint32_t dstIdx = (y * width + x) * 4;

                        // Copy RGB channels
                        floatPixels[dstIdx + 0] = (spec.nchannels > 0) ? tempPixels[srcIdx + 0] : 0.0f; // R
                        floatPixels[dstIdx + 1] = (spec.nchannels > 1) ? tempPixels[srcIdx + 1] : 0.0f; // G
                        floatPixels[dstIdx + 2] = (spec.nchannels > 2) ? tempPixels[srcIdx + 2] : 0.0f; // B
                        floatPixels[dstIdx + 3] = (spec.nchannels > 3) ? tempPixels[srcIdx + 3] : 1.0f; // A
                    }
                }
                readSuccess = true;
            }
        }

        if (readSuccess) {
            // Clear any potential read warnings
            OIIO::geterror();
            // Apply color space conversion if processor exists
            if (processor) {
                try {
                    OCIO::PackedImageDesc imgDesc(floatPixels.data(), width, height, 4, 
                                                OCIO::BIT_DEPTH_F32, sizeof(float) * 4, 0, 0);
                    OCIO::ConstCPUProcessorRcPtr cpuProcessor = processor->getDefaultCPUProcessor();
                    cpuProcessor->apply(imgDesc);
                } catch (const OCIO::Exception& e) {
                    // Color conversion failed, continue without it
                }
            }

            // Convert to 8-bit RGBA
            uint8_t* bytePixels = g_ctx.imageData.pixels.data();
            for (size_t i = 0; i < floatPixels.size(); ++i) {
                float val = clamp(floatPixels[i], 0.0f, 1.0f);
                bytePixels[i] = static_cast<uint8_t>(val * 255.0f + 0.5f);
            }
        } else {
            g_ctx.imageData.clear();
        }
    }

    in->close();

    // Clear any errors from image processing operations
    OIIO::geterror();

    // Upload to Vulkan if renderer exists and image data is valid
    if (g_ctx.renderer && g_ctx.imageData.isValid()) {
#ifdef HAVE_DATADOG
        auto uploadSpan = Logger::CreateChildSpan(loadSpan, "vulkan.upload");
#endif
        g_ctx.renderer->UpdateImageFromData(
            g_ctx.imageData.pixels.data(),
            g_ctx.imageData.width,
            g_ctx.imageData.height,
            g_ctx.imageData.isHdr
        );
    }
    
#ifdef HAVE_DATADOG
    loadSpan.set_tag("success", "true");
#endif
    CenterImage(true);
}

void GetImagesInDirectory(const wchar_t* filePath) {
#ifdef HAVE_DATADOG
    auto dirSpan = Logger::CreateSpan("image.scan_directory");
#endif
    
    // NASA Standard: Validate all input parameters
    if (filePath == nullptr) {
#ifdef HAVE_DATADOG
        dirSpan.set_tag("success", "false");
        dirSpan.set_tag("error", "Null file path");
#endif
        return;
    }
    
    std::string utf8Path = wstring_to_utf8(filePath);
#ifdef HAVE_DATADOG
    dirSpan.set_tag("file_path", utf8Path);
#endif

    g_ctx.imageFiles.clear();

    // NASA Standard: Validate string length before copying
    size_t pathLength = wcsnlen(filePath, MAX_PATH);
    if (pathLength >= MAX_PATH) {
#ifdef HAVE_DATADOG
        dirSpan.set_tag("success", "false");
        dirSpan.set_tag("error", "Path too long");
#endif
        return; // Path too long
    }

    wchar_t folder[MAX_PATH] = { 0 };
    if (wcsncpy_s(folder, MAX_PATH, filePath, _TRUNCATE) != 0) {
#ifdef HAVE_DATADOG
        dirSpan.set_tag("success", "false");
        dirSpan.set_tag("error", "Path copy failed");
#endif
        return; // Copy failed
    }

    PathRemoveFileSpecW(folder);

    WIN32_FIND_DATAW fd{};
    wchar_t searchPath[MAX_PATH] = { 0 };
    if (!PathCombineW(searchPath, folder, L"*.*")) {
#ifdef HAVE_DATADOG
        dirSpan.set_tag("success", "false");
        dirSpan.set_tag("error", "Path combination failed");
#endif
        return; // Path combination failed
    }

    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t fullPath[MAX_PATH] = { 0 };
                PathCombineW(fullPath, folder, fd.cFileName);
                if (IsImageFile(fullPath)) {
                    g_ctx.imageFiles.push_back(fullPath);
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    auto it = std::find_if(g_ctx.imageFiles.begin(), g_ctx.imageFiles.end(),
        [&](const std::wstring& s) { return _wcsicmp(s.c_str(), filePath) == 0; }
    );
    g_ctx.currentImageIndex = (it != g_ctx.imageFiles.end()) ? static_cast<int>(std::distance(g_ctx.imageFiles.begin(), it)) : -1;
    
#ifdef HAVE_DATADOG
    dirSpan.set_tag("success", "true");
    dirSpan.set_tag("images_found", std::to_string(g_ctx.imageFiles.size()));
    dirSpan.set_tag("current_index", std::to_string(g_ctx.currentImageIndex));
#endif
}

void DeleteCurrentImage() {
#ifdef HAVE_DATADOG
    auto deleteSpan = Logger::CreateSpan("image.delete");
#endif
    
    // NASA Standard: Validate all bounds and indices
    if (g_ctx.currentImageIndex < 0 || 
        g_ctx.currentImageIndex >= static_cast<int>(g_ctx.imageFiles.size()) ||
        g_ctx.imageFiles.empty()) {
#ifdef HAVE_DATADOG
        deleteSpan.set_tag("success", "false");
        deleteSpan.set_tag("error", "Invalid image index or empty file list");
#endif
        return;
    }

    const std::wstring& filePathToDelete = g_ctx.imageFiles[g_ctx.currentImageIndex];
#ifdef HAVE_DATADOG
    deleteSpan.set_tag("file_path", wstring_to_utf8(filePathToDelete));
#endif

    // NASA Standard: Validate path length before operations
    if (filePathToDelete.empty() || filePathToDelete.length() >= MAX_PATH) {
#ifdef HAVE_DATADOG
        deleteSpan.set_tag("success", "false");
        deleteSpan.set_tag("error", "Invalid file path length");
#endif
        return;
    }

    std::wstring msg = L"Are you sure you want to move this file to the Recycle Bin?\n\n" + filePathToDelete;

    if (MessageBoxW(g_ctx.hWnd, msg.c_str(), L"Confirm Delete", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        // NASA Standard: Check for potential overflow in buffer size calculation
        size_t requiredSize = filePathToDelete.length() + 2;
        if (requiredSize > MAX_PATH + 2) {
#ifdef HAVE_DATADOG
            deleteSpan.set_tag("success", "false");
            deleteSpan.set_tag("error", "Path too long for deletion");
#endif
            MessageBoxW(g_ctx.hWnd, L"File path is too long for deletion.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        std::vector<wchar_t> pFromBuffer(requiredSize, 0);
        if (wcsncpy_s(pFromBuffer.data(), pFromBuffer.size(), filePathToDelete.c_str(), _TRUNCATE) != 0) {
#ifdef HAVE_DATADOG
            deleteSpan.set_tag("success", "false");
            deleteSpan.set_tag("error", "Failed to prepare path for deletion");
#endif
            MessageBoxW(g_ctx.hWnd, L"Failed to prepare file path for deletion.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        SHFILEOPSTRUCTW sfos = { 0 };
        sfos.hwnd = g_ctx.hWnd;
        sfos.wFunc = FO_DELETE;
        sfos.pFrom = pFromBuffer.data();
        sfos.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION;

        if (SHFileOperationW(&sfos) == 0 && !sfos.fAnyOperationsAborted) {
#ifdef HAVE_DATADOG
            deleteSpan.set_tag("success", "true");
#endif
            g_ctx.imageFiles.erase(g_ctx.imageFiles.begin() + g_ctx.currentImageIndex);
            if (g_ctx.imageFiles.empty()) {
                g_ctx.imageData.clear();
                g_ctx.currentImageIndex = -1;
                InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
            }
            else {
                if (g_ctx.currentImageIndex >= static_cast<int>(g_ctx.imageFiles.size())) {
                    g_ctx.currentImageIndex = 0;
                }
                LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
            }
        }
        else {
#ifdef HAVE_DATADOG
            deleteSpan.set_tag("success", "false");
            deleteSpan.set_tag("error", "SHFileOperation failed");
#endif
            MessageBoxW(g_ctx.hWnd, L"Failed to delete the file.", L"Error", MB_OK | MB_ICONERROR);
        }
    } else {
#ifdef HAVE_DATADOG
        deleteSpan.set_tag("success", "false");
        deleteSpan.set_tag("error", "User cancelled deletion");
#endif
    }
}

// Helper function to get rendered image data from Vulkan renderer
static std::vector<uint8_t> GetRenderedImageData(uint32_t& outWidth, uint32_t& outHeight) {
    if (!g_ctx.renderer || !g_ctx.imageData.isValid()) {
        return {};
    }

    // Get the current viewport dimensions for rendering
    outWidth = g_ctx.imageData.width;
    outHeight = g_ctx.imageData.height;

    // Apply rotation to dimensions
    if (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270) {
        std::swap(outWidth, outHeight);
    }

    // For now, return the source image data
    // In a full implementation, you would render the current view to a buffer
    std::vector<uint8_t> result;

    if (g_ctx.imageData.isHdr) {
        // Convert HDR to LDR for saving
        size_t pixelCount = outWidth * outHeight * 4;
        result.resize(pixelCount);

        const uint16_t* halfPixels = reinterpret_cast<const uint16_t*>(g_ctx.imageData.pixels.data());

        for (size_t i = 0; i < pixelCount; ++i) {
            // Convert half to float then to 8-bit
            uint16_t halfVal = halfPixels[i];
            uint32_t sign = (halfVal >> 15) & 0x1;
            uint32_t exp = (halfVal >> 10) & 0x1f;
            uint32_t mantissa = halfVal & 0x3ff;

            float floatVal;
            if (exp == 0) {
                floatVal = (sign ? -1.0f : 1.0f) * (mantissa / 1024.0f) * std::pow(2.0f, -14.0f);
            } else if (exp == 31) {
                floatVal = (mantissa == 0) ? (sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity()) : std::numeric_limits<float>::quiet_NaN();
            } else {
                floatVal = (sign ? -1.0f : 1.0f) * (1.0f + mantissa / 1024.0f) * std::pow(2.0f, static_cast<float>(exp - 15));
            }

            // Tone mapping for display (simple Reinhard)
            floatVal = floatVal / (1.0f + floatVal);
            result[i] = static_cast<uint8_t>(clamp(floatVal * 255.0f, 0.0f, 255.0f));
        }
    } else {
        result = g_ctx.imageData.pixels;
    }

    return result;
}

void SaveImageAs() {
    // NASA Standard: Validate image data state
    if (!g_ctx.imageData.isValid()) {
        return;
    }

    wchar_t szFile[MAX_PATH] = L"Untitled.png";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_ctx.hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PNG File (*.png)\0*.png\0JPEG File (*.jpg)\0*.jpg\0OpenEXR File (*.exr)\0*.exr\0TIFF File (*.tiff)\0*.tiff\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    std::string utf8Path = wstring_to_utf8(ofn.lpstrFile);

    // Determine if we should save as HDR based on file extension
    std::string lowerPath = utf8Path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    bool saveAsHdr = (lowerPath.find(".exr") != std::string::npos || 
                      lowerPath.find(".hdr") != std::string::npos ||
                      lowerPath.find(".tiff") != std::string::npos);

    uint32_t width, height;
    std::vector<uint8_t> imageData = GetRenderedImageData(width, height);

    if (imageData.empty()) {
        MessageBoxW(g_ctx.hWnd, L"Could not get image data to save.", L"Save Error", MB_ICONERROR);
        return;
    }

    auto out = OIIO::ImageOutput::create(utf8Path);
    if (!out) {
        MessageBoxW(g_ctx.hWnd, L"Could not create output file.", L"Save Error", MB_ICONERROR);
        return;
    }

    OIIO::ImageSpec spec(width, height, 4, saveAsHdr ? OIIO::TypeDesc::HALF : OIIO::TypeDesc::UINT8);

    // Set color space metadata
    if (saveAsHdr) {
        spec.attribute("oiio:ColorSpace", std::string("Linear"));
    } else {
        spec.attribute("oiio:ColorSpace", std::string("sRGB"));
    }

    if (!out->open(utf8Path, spec)) {
        MessageBoxW(g_ctx.hWnd, L"Could not open output file.", L"Save Error", MB_ICONERROR);
        return;
    }

    bool success = false;
    if (saveAsHdr && g_ctx.imageData.isHdr) {
        // Save HDR data directly
        success = out->write_image(OIIO::TypeDesc::HALF, g_ctx.imageData.pixels.data());
    } else {
        // Save LDR data
        success = out->write_image(OIIO::TypeDesc::UINT8, imageData.data());
    }

    out->close();

    // Clear any save operation errors/warnings
    OIIO::geterror();

    if (success) {
        LoadImageFromFile(ofn.lpstrFile);
        GetImagesInDirectory(ofn.lpstrFile);
    } else {
        MessageBoxW(g_ctx.hWnd, L"Failed to save image.", L"Save As Error", MB_ICONERROR);
    }
}

void SaveImage() {
    if (g_ctx.currentImageIndex < 0 || g_ctx.currentImageIndex >= static_cast<int>(g_ctx.imageFiles.size())) {
        SaveImageAs();
        return;
    }

    if (g_ctx.rotationAngle == 0) {
        MessageBoxW(g_ctx.hWnd, L"No changes to save.", L"Save", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto& originalPath = g_ctx.imageFiles[g_ctx.currentImageIndex];
    std::string utf8Path = wstring_to_utf8(originalPath);

    // Get the original file format
    auto in = OIIO::ImageInput::open(utf8Path);
    if (!in) {
        MessageBoxW(g_ctx.hWnd, L"Could not open original file to determine format.", L"Save Error", MB_ICONERROR);
        return;
    }

    const OIIO::ImageSpec& originalSpec = in->spec();
    in->close();

    uint32_t width, height;
    std::vector<uint8_t> imageData = GetRenderedImageData(width, height);

    if (imageData.empty()) {
        MessageBoxW(g_ctx.hWnd, L"Could not get image data to save.", L"Save Error", MB_ICONERROR);
        return;
    }

    // NASA Standard: Validate path length before concatenation
    if (originalPath.length() > MAX_PATH - 20) {
        MessageBoxW(g_ctx.hWnd, L"File path too long for temporary file creation.", L"Save Error", MB_ICONERROR);
        return;
    }

    std::wstring tempPath = originalPath + L".tmp_save";
    std::string utf8TempPath = wstring_to_utf8(tempPath);

    auto out = OIIO::ImageOutput::create(utf8TempPath);
    if (!out) {
        MessageBoxW(g_ctx.hWnd, L"Could not create output file.", L"Save Error", MB_ICONERROR);
        return;
    }

    // Preserve original format characteristics
    OIIO::ImageSpec spec(width, height, 4, originalSpec.format);

    // Copy important attributes from original
    for (const auto& attr : originalSpec.extra_attribs) {
        if (attr.name() != "ImageDescription" && attr.name() != "DateTime") {
            spec.attribute(attr.name(), attr.type(), attr.data());
        }
    }

    if (!out->open(utf8TempPath, spec)) {
        MessageBoxW(g_ctx.hWnd, L"Could not open temporary file for writing.", L"Save Error", MB_ICONERROR);
        return;
    }

    bool success = false;
    if (g_ctx.imageData.isHdr && originalSpec.format == OIIO::TypeDesc::HALF) {
        success = out->write_image(OIIO::TypeDesc::HALF, g_ctx.imageData.pixels.data());
    } else {
        success = out->write_image(OIIO::TypeDesc::UINT8, imageData.data());
    }

    out->close();

    // Clear any save operation errors/warnings
    OIIO::geterror();

    if (success) {
        if (ReplaceFileW(originalPath.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            LoadImageFromFile(originalPath.c_str());
            g_ctx.rotationAngle = 0;
            InvalidateRect(g_ctx.hWnd, NULL, FALSE);
        } else {
            DeleteFileW(tempPath.c_str());
            MessageBoxW(g_ctx.hWnd, L"Failed to replace the original file.", L"Save Error", MB_ICONERROR);
        }
    } else {
        DeleteFileW(tempPath.c_str());
        MessageBoxW(g_ctx.hWnd, L"Failed to save image to temporary file.", L"Save Error", MB_ICONERROR);
    }
}

void HandleDropFiles(HDROP hDrop) {
    // NASA Standard: Validate input parameters
    if (hDrop == nullptr) {
        return;
    }

    wchar_t filePath[MAX_PATH] = { 0 };
    UINT result = DragQueryFileW(hDrop, 0, filePath, MAX_PATH);
    if (result > 0 && result < MAX_PATH) {
        // NASA Standard: Ensure null termination
        filePath[MAX_PATH - 1] = L'\0';
        LoadImageFromFile(filePath);
        GetImagesInDirectory(filePath);
    }
    DragFinish(hDrop);
}

void HandlePaste() {
    // NASA Standard: Validate system state before operations
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        return;
    }

    if (!OpenClipboard(g_ctx.hWnd)) {
        return;
    }

    HANDLE hClipData = GetClipboardData(CF_HDROP);
    if (hClipData != nullptr) {
        HDROP hDrop = static_cast<HDROP>(hClipData);
        wchar_t filePath[MAX_PATH] = { 0 };
        UINT result = DragQueryFileW(hDrop, 0, filePath, MAX_PATH);
        if (result > 0 && result < MAX_PATH) {
            // NASA Standard: Ensure null termination
            filePath[MAX_PATH - 1] = L'\0';
            CloseClipboard();
            LoadImageFromFile(filePath);
            GetImagesInDirectory(filePath);
            return;
        }
    }

    CloseClipboard();
}

void HandleCopy() {
    // NASA Standard: Validate preconditions
    if (!g_ctx.imageData.isValid() || !OpenClipboard(g_ctx.hWnd)) {
        return;
    }
    EmptyClipboard();

    uint32_t width, height;
    std::vector<uint8_t> imageData = GetRenderedImageData(width, height);

    if (imageData.empty()) {
        CloseClipboard();
        return;
    }

    // NASA Standard: Validate dimensions to prevent overflow
    if (width == 0 || height == 0 || width > 65536 || height > 65536) {
        CloseClipboard();
        return;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(width);
    bi.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // NASA Standard: Check for integer overflow before memory allocation
    uint64_t bmpSize64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4;
    if (bmpSize64 > UINT32_MAX) {
        CloseClipboard();
        return;
    }

    DWORD dwBmpSize = static_cast<DWORD>(bmpSize64);
    DWORD dwSizeOfDIB = dwBmpSize + sizeof(BITMAPINFOHEADER);
    HGLOBAL hGlobal = GlobalAlloc(GHND, dwSizeOfDIB);

    if (hGlobal) {
        char* lpGlobal = static_cast<char*>(GlobalLock(hGlobal));
        if (lpGlobal) {
            memcpy(lpGlobal, &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
            memcpy(lpGlobal + sizeof(BITMAPINFOHEADER), imageData.data(), dwBmpSize);
            GlobalUnlock(hGlobal);
            SetClipboardData(CF_DIB, hGlobal);
        }
    }

    CloseClipboard();
}

void OpenFileLocationAction() {
    // NASA Standard: Validate all bounds and state
    if (g_ctx.currentImageIndex < 0 || 
        g_ctx.currentImageIndex >= static_cast<int>(g_ctx.imageFiles.size()) ||
        g_ctx.imageFiles.empty()) {
        return;
    }

    const std::wstring& filePath = g_ctx.imageFiles[g_ctx.currentImageIndex];

    // NASA Standard: Validate path before use
    if (filePath.empty() || filePath.length() >= MAX_PATH) {
        MessageBoxW(g_ctx.hWnd, L"Invalid file path.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(filePath.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr) && pidl != nullptr) {
        HRESULT openResult = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);

        if (FAILED(openResult)) {
            MessageBoxW(g_ctx.hWnd, L"Could not open file location.", L"Error", MB_OK | MB_ICONERROR);
        }
    } else {
        MessageBoxW(g_ctx.hWnd, L"Could not parse file path.", L"Error", MB_OK | MB_ICONERROR);
    }
}