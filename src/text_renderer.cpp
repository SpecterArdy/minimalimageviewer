#include "text_renderer.h"
#include "logging.h"
#include <algorithm>
#include <filesystem>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

TextRenderer::TextRenderer() 
    : font_(nullptr), currentFontSize_(18), ttfInitialized_(false),
      isVariableFont_(false), fontFamily_("Unknown") {
    // Set default variable font settings
    fontVariations_["wght"] = 400.0f;  // Weight
    fontVariations_["wdth"] = 100.0f;  // Width
    fontVariations_["slnt"] = 0.0f;    // Slant/Italic
}

TextRenderer::~TextRenderer() {
    Shutdown();
}

bool TextRenderer::Initialize() {
    if (ttfInitialized_) {
        return true; // Already initialized
    }

    if (TTF_Init() != 0) {
        Logger::Error("Failed to initialize SDL_ttf: {}", SDL_GetError());
        return false;
    }

    ttfInitialized_ = true;
    
    // Disable application font for now to ensure stability
    // if (LoadApplicationFont(18)) {
    //     Logger::Info("TextRenderer initialized with application variable font: {}", fontFamily_);
    //     return true;
    // }
    
    // Fall back to system font if application font is not available
    if (LoadSystemFont(18)) {
        Logger::Info("TextRenderer initialized with system font (application font not found)");
        return true;
    }
    
    Logger::Warn("Could not load any font, text rendering may not work");
    return false;
}

void TextRenderer::Shutdown() {
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    
    if (ttfInitialized_) {
        TTF_Quit();
        ttfInitialized_ = false;
    }
}

bool TextRenderer::LoadFont(const std::string& fontPath, int fontSize) {
    if (!ttfInitialized_) {
        Logger::Error("TTF not initialized");
        return false;
    }

    // Close existing font if any
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }

    font_ = TTF_OpenFont(fontPath.c_str(), fontSize);
    if (!font_) {
        Logger::Error("Failed to load font '{}': {}", fontPath, SDL_GetError());
        return false;
    }

    currentFontSize_ = fontSize;
    Logger::Info("Loaded font: {} (size: {})", fontPath, fontSize);
    return true;
}

TTF_Font* TextRenderer::LoadSystemFontInternal(int fontSize) {
    std::vector<std::string> fontPaths;

#ifdef _WIN32
    // Windows font paths
    fontPaths = {
        "C:/Windows/Fonts/segoeui.ttf",     // Segoe UI (Windows 7+)
        "C:/Windows/Fonts/arial.ttf",      // Arial
        "C:/Windows/Fonts/calibri.ttf",    // Calibri  
        "C:/Windows/Fonts/tahoma.ttf",     // Tahoma
        "C:/Windows/Fonts/verdana.ttf",    // Verdana
        "C:/Windows/Fonts/cour.ttf"        // Courier New (fallback)
    };
#elif defined(__linux__)
    // Linux font paths
    fontPaths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/System/Library/Fonts/Arial.ttf"
    };
#elif defined(__APPLE__)
    // macOS font paths
    fontPaths = {
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Geneva.ttf"
    };
#endif

    // Try each font path
    for (const auto& path : fontPaths) {
        if (std::filesystem::exists(path)) {
            TTF_Font* font = TTF_OpenFont(path.c_str(), fontSize);
            if (font) {
                Logger::Info("Loaded system font: {} (size: {})", path, fontSize);
                return font;
            }
        }
    }

    Logger::Warn("No system fonts found in standard locations");
    return nullptr;
}

bool TextRenderer::LoadSystemFont(int fontSize) {
    if (!ttfInitialized_) {
        Logger::Error("TTF not initialized");
        return false;
    }

    // Close existing font if any
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }

    font_ = LoadSystemFontInternal(fontSize);
    if (font_) {
        currentFontSize_ = fontSize;
        return true;
    }

    return false;
}

bool TextRenderer::GetTextSize(const std::string& text, TTF_Font* font, int* w, int* h) {
    if (!font) return false;
    const SDL_Color dummy{255, 255, 255, 255};
    SDL_Surface* s = TTF_RenderText_Blended(font, text.c_str(), text.size(), dummy);
    if (!s) return false;
    if (w) *w = s->w;
    if (h) *h = s->h;
    SDL_DestroySurface(s);
    return true;
}

SDL_Surface* TextRenderer::RenderTextLine(const std::string& text, SDL_Color color, TTF_Font* font) {
    if (!font) return nullptr;
    // SDL3_ttf: TTF_RenderText_Blended(font, const char* text, size_t length, SDL_Color fg)
    return TTF_RenderText_Blended(font, text.c_str(), text.size(), color);
}

void TextRenderer::BlitTextToSurface(SDL_Surface* textSurface, SDL_Surface* targetSurface, int x, int y) {
    if (!textSurface || !targetSurface) return;

    SDL_Rect destRect = {x, y, 0, 0}; // w/h ignored by SDL_BlitSurface
    SDL_BlitSurface(textSurface, nullptr, targetSurface, &destRect);
}

SDL_Surface* TextRenderer::CreateTextSurface(const std::vector<TextLine>& lines, 
                                            int surfaceWidth, int surfaceHeight,
                                            SDL_Color backgroundColor) {
    if (!IsReady()) {
        Logger::Error("TextRenderer not ready - no font loaded");
        return nullptr;
    }

    if (!font_) {
        Logger::Error("No font loaded for text rendering");
        return nullptr;
    }

    // Create target surface with RGBA format
    SDL_Surface* surface = SDL_CreateSurface(surfaceWidth, surfaceHeight, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        Logger::Error("Failed to create text surface: {}", SDL_GetError());
        return nullptr;
    }

    // Fill with background color using SDL3 API
    Uint32 color = SDL_MapSurfaceRGBA(surface, backgroundColor.r, backgroundColor.g, backgroundColor.b, backgroundColor.a);
    SDL_FillSurfaceRect(surface, nullptr, color);

    // Render each text line
    for (const auto& line : lines) {
        TTF_Font* lineFont = font_;
        bool needToCleanupFont = false;
        
        // If this line needs a different font size or weight, create a temporary font
        if (line.fontSize != currentFontSize_ || 
            (isVariableFont_ && (line.fontWeight != fontVariations_["wght"] || line.fontWidth != fontVariations_["wdth"]))) {
            
            if (isVariableFont_ && !currentFontPath_.empty()) {
                // For variable fonts, try to create with specific variations
                lineFont = CreateFontWithVariations(currentFontPath_, line.fontSize, line.fontWeight, line.fontWidth);
                needToCleanupFont = (lineFont != nullptr && lineFont != font_);
            }
            
            if (!lineFont || lineFont == font_) {
                // Fallback to different size font loading
                if (line.fontSize != currentFontSize_) {
                    if (isVariableFont_ && !currentFontPath_.empty()) {
                        lineFont = TTF_OpenFont(currentFontPath_.c_str(), line.fontSize);
                    } else {
                        lineFont = LoadSystemFontInternal(line.fontSize);
                    }
                    needToCleanupFont = (lineFont != nullptr && lineFont != font_);
                }
            }
            
            if (!lineFont) {
                lineFont = font_; // Final fallback to default font
                needToCleanupFont = false;
            }
        }
        
        // Apply font style if needed (for pseudo-bold/italic)
        if (lineFont && line.fontWeight > 600.0f) {
            TTF_SetFontStyle(lineFont, TTF_GetFontStyle(lineFont) | TTF_STYLE_BOLD);
        }

        // Guard against null font
        if (!lineFont) {
            Logger::Warn("No font available for line: '{}'", line.text);
            continue;
        }

        SDL_Surface* textSurface = RenderTextLine(line.text, line.color, lineFont);
        if (textSurface) {
            BlitTextToSurface(textSurface, surface, line.x, line.y);
            SDL_DestroySurface(textSurface);
        }

        // Clean up temporary font
        if (needToCleanupFont) {
            TTF_CloseFont(lineFont);
        }
    }

    return surface;
}

SDL_Surface* TextRenderer::CreateInstructionalSurface(int width, int height, bool openColorIOAvailable) {
    if (!font_) {
        Logger::Error("No font loaded for instructional surface");
        return nullptr;
    }

    std::vector<TextLine> lines;
    
    // Calculate center positions
    int centerX = width / 2;
    int centerY = height / 2;
    
    // Title - use current font size with centered positioning
    SDL_Color titleColor = {200, 200, 255, 255}; // Light blue
    std::string title = "Minimal Image Viewer";
    int titleW = 0, titleH = 0;
    if (GetTextSize(title, font_, &titleW, &titleH)) {
        lines.emplace_back(title, centerX - titleW/2, centerY - 80, titleColor, currentFontSize_, 400.0f, 100.0f);
    } else {
        // Fallback estimation if text sizing fails
        titleW = static_cast<int>(title.length() * 10);
        lines.emplace_back(title, centerX - titleW/2, centerY - 80, titleColor, currentFontSize_, 400.0f, 100.0f);
    }
    
    // Main instruction
    SDL_Color instructColor = {255, 255, 255, 255}; // White
    std::string instruction = "Drag & drop an image here, or press Ctrl+O to open a file.";
    int instrW = 0, instrH = 0;
    if (GetTextSize(instruction, font_, &instrW, &instrH)) {
        lines.emplace_back(instruction, centerX - instrW/2, centerY - 30, instructColor, currentFontSize_, 400.0f, 100.0f);
    } else {
        // Fallback estimation if text sizing fails
        instrW = static_cast<int>(instruction.length() * 8);
        lines.emplace_back(instruction, centerX - instrW/2, centerY - 30, instructColor, currentFontSize_, 400.0f, 100.0f);
    }
    
    // OpenColorIO status
    SDL_Color statusColor = openColorIOAvailable ? 
        SDL_Color{200, 255, 200, 255} : SDL_Color{255, 200, 200, 255}; // Green if available, red if not
    
    std::string ocioStatus = openColorIOAvailable ? 
        "OpenColorIO: Available (color management enabled)" :
        "OpenColorIO: Not available (basic color display)";
    
    int statusW = 0, statusH = 0;
    if (GetTextSize(ocioStatus, font_, &statusW, &statusH)) {
        lines.emplace_back(ocioStatus, centerX - statusW/2, centerY, statusColor, currentFontSize_, 400.0f, 100.0f);
    } else {
        // Fallback estimation if text sizing fails
        statusW = static_cast<int>(ocioStatus.length() * 7);
        lines.emplace_back(ocioStatus, centerX - statusW/2, centerY, statusColor, currentFontSize_, 400.0f, 100.0f);
    }
    
    // Shortcuts
    SDL_Color shortcutColor = {220, 220, 220, 255}; // Light gray
    std::string shortcuts = "Shortcuts: Ctrl+Wheel/+/- to zoom, Ctrl+0 to fit, Right-click for menu.";
    int shortcutsW = 0, shortcutsH = 0;
    if (GetTextSize(shortcuts, font_, &shortcutsW, &shortcutsH)) {
        lines.emplace_back(shortcuts, centerX - shortcutsW/2, centerY + 30, shortcutColor, currentFontSize_, 400.0f, 100.0f);
    } else {
        // Fallback estimation if text sizing fails
        shortcutsW = static_cast<int>(shortcuts.length() * 7);
        lines.emplace_back(shortcuts, centerX - shortcutsW/2, centerY + 30, shortcutColor, currentFontSize_, 400.0f, 100.0f);
    }

    // Create surface with dark background
    SDL_Color backgroundColor = {25, 25, 35, 255}; // Dark blue-gray
    return CreateTextSurface(lines, width, height, backgroundColor);
}

SDL_Surface* TextRenderer::CreateSplashScreenSurface(int width, int height, const std::string& statusText) {
    std::vector<TextLine> lines;
    
    // Calculate center positions  
    int centerX = width / 2;
    int centerY = height / 2;
    
    // Application name - Bold and slightly expanded
    SDL_Color titleColor = {255, 255, 255, 255}; // White
    lines.emplace_back("Minimal Image Viewer", centerX - 120, centerY - 30, titleColor, 22, 600.0f, 105.0f); // Semi-bold, slightly expanded
    
    // Status text - Light weight, condensed
    SDL_Color statusColor = {200, 200, 200, 255}; // Light gray
    int statusTextWidth = static_cast<int>(statusText.length() * 4); // Rough estimate
    lines.emplace_back(statusText, centerX - statusTextWidth, centerY + 10, statusColor, 14, 350.0f, 95.0f); // Light weight, slightly condensed

    // Create surface with dark background
    SDL_Color backgroundColor = {40, 40, 50, 255}; // Dark gray
    return CreateTextSurface(lines, width, height, backgroundColor);
}

std::vector<uint8_t> TextRenderer::SurfaceToRGBA(SDL_Surface* surface) {
    if (!surface) {
        return {};
    }

    SDL_Surface* rgba = surface;
    const bool needsConv = (surface->format != SDL_PIXELFORMAT_RGBA32);
    if (needsConv) {
        rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        if (!rgba) {
            Logger::Error("Convert to RGBA32 failed: {}", SDL_GetError());
            return {};
        }
    }

    const int w = rgba->w;
    const int h = rgba->h;
    const int bpp = 4;
    const int pitch = rgba->pitch;

    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bpp);

    if (SDL_MUSTLOCK(rgba)) SDL_LockSurface(rgba);
    uint8_t* src = static_cast<uint8_t*>(rgba->pixels);
    uint8_t* dst = out.data();

    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * w * bpp,
                    src + static_cast<size_t>(y) * pitch,
                    static_cast<size_t>(w) * bpp);
    }
    if (SDL_MUSTLOCK(rgba)) SDL_UnlockSurface(rgba);

    if (needsConv) SDL_DestroySurface(rgba);
    return out;
}

// Variable font support methods
void TextRenderer::SetFontVariation(const std::string& axis, float value) {
    fontVariations_[axis] = value;
    
    // If we have a font loaded and it's variable, apply the changes
    if (font_ && isVariableFont_) {
        ApplyFontVariations(font_);
    }
}

void TextRenderer::SetFontWeight(float weight) {
    SetFontVariation("wght", weight);
}

void TextRenderer::SetFontWidth(float width) {
    SetFontVariation("wdth", width);
}

bool TextRenderer::LoadApplicationFont(int fontSize) {
    std::string fontPath = GetApplicationFontPath();
    if (fontPath.empty()) {
        Logger::Info("Application font not found");
        return false;
    }
    
    if (!std::filesystem::exists(fontPath)) {
        Logger::Info("Application font file does not exist: {}", fontPath);
        return false;
    }
    
    // Try to load the font with default variable settings
    font_ = CreateFontWithVariations(fontPath, fontSize, 
                                   fontVariations_["wght"], 
                                   fontVariations_["wdth"]);
    
    if (!font_) {
        Logger::Error("Failed to load application font: {}", fontPath);
        return false;
    }
    
    currentFontSize_ = fontSize;
    currentFontPath_ = fontPath;
    isVariableFont_ = true;
    
    // Get actual font family name using SDL3_ttf API
    const char* familyName = TTF_GetFontFamilyName(font_);
    if (familyName) {
        fontFamily_ = familyName;
    } else {
        fontFamily_ = "TT Interphases Pro Variable"; // Fallback
    }
    
    // Get actual font weight using SDL3_ttf API
    int actualWeight = TTF_GetFontWeight(font_);
    Logger::Info("Loaded variable font: {} - Family: '{}', Actual weight: {}, Requested weight: {}, Width: {}", 
                fontPath, fontFamily_, actualWeight, fontVariations_["wght"], fontVariations_["wdth"]);
    
    // Check if the font is scalable (variable fonts should be)
    if (TTF_FontIsScalable(font_)) {
        Logger::Info("Font is scalable (variable font detected)");
    } else {
        Logger::Info("Font is not scalable (bitmap font)");
        isVariableFont_ = false; // Not actually variable
    }
    
    return true;
}

TTF_Font* TextRenderer::CreateFontWithVariations(const std::string& fontPath, int fontSize, 
                                                 float weight, float width) {
    // Suppress unused parameter warnings until variable font support is implemented
    (void)weight;
    (void)width;
    
    TTF_Font* font = TTF_OpenFont(fontPath.c_str(), fontSize);
    if (!font) {
        Logger::Error("Failed to open font file: {} - {}", fontPath, SDL_GetError());
        return nullptr;
    }
    
    // Note: SDL3_ttf variable font support may be limited
    // For now, we'll load the font and store the variation settings for future use
    // Full variable font support might require direct FreeType integration
    
    return font;
}

void TextRenderer::ApplyFontVariations(TTF_Font* font) {
    if (!font || !isVariableFont_) {
        return;
    }
    
    // Note: SDL3_ttf may have limited variable font support
    // This is where we would apply variable font settings if supported
    // For full variable font support, we might need to use FreeType directly
    
    Logger::Info("Variable font settings would be applied here (weight: {}, width: {})",
                fontVariations_["wght"], fontVariations_["wdth"]);
}

std::string TextRenderer::GetApplicationFontPath() {
    // Try relative paths from the executable directory
    std::vector<std::string> possiblePaths = {
        "../fonts/TT Interphases Pro Variable (Regular).ttf",  // From src/ to fonts/
        "fonts/TT Interphases Pro Variable (Regular).ttf",     // From exe directory
        "./TT Interphases Pro Variable (Regular).ttf",        // Same directory
        "../TT Interphases Pro Variable (Regular).ttf"         // Parent directory
    };
    
    for (const auto& path : possiblePaths) {
        if (std::filesystem::exists(path)) {
            std::string absolutePath = std::filesystem::absolute(path).string();
            Logger::Info("Found application font at: {}", absolutePath);
            return absolutePath;
        }
    }
    
    return ""; // Not found
}
