#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <memory>
#include <string>
#include <vector>
#include <map>

/**
 * TextRenderer - Utility class for rendering text overlays
 * Uses SDL3_ttf to render text onto surfaces that can be composited
 * with Vulkan-rendered content or used in software fallback mode.
 * 
 * Supports variable fonts with configurable axes (weight, width, slant, etc.)
 */
class TextRenderer {
public:
    struct TextLine {
        std::string text;
        int x, y;
        SDL_Color color;
        int fontSize;
        float fontWeight;  // Variable font weight (400 = normal, 700 = bold)
        float fontWidth;   // Variable font width (100 = normal, 75 = condensed, 125 = expanded)
        
        TextLine(const std::string& t, int px, int py, 
                SDL_Color c = {255, 255, 255, 255}, int size = 18,
                float weight = 400.0f, float width = 100.0f)
            : text(t), x(px), y(py), color(c), fontSize(size), 
              fontWeight(weight), fontWidth(width) {}
    };

    TextRenderer();
    ~TextRenderer();

    // Initialize the text renderer with the application's variable font
    bool Initialize();
    void Shutdown();

    // Load a specific font from file (supports variable fonts)
    bool LoadFont(const std::string& fontPath, int fontSize);
    
    // Load system default font (tries common system locations)
    bool LoadSystemFont(int fontSize = 18);
    
    // Set variable font parameters (weight, width, slant, etc.)
    void SetFontVariation(const std::string& axis, float value);
    void SetFontWeight(float weight);  // 400 = normal, 700 = bold
    void SetFontWidth(float width);    // 100 = normal, 75 = condensed
    
    // Get current font variations
    std::map<std::string, float> GetFontVariations() const { return fontVariations_; }

    // Create a surface with rendered text lines
    SDL_Surface* CreateTextSurface(const std::vector<TextLine>& lines, 
                                   int surfaceWidth, int surfaceHeight,
                                   SDL_Color backgroundColor = {0, 0, 0, 0});

    // Create instructional UI surface for "no image loaded" state
    SDL_Surface* CreateInstructionalSurface(int width, int height, bool openColorIOAvailable = false);

    // Create splash screen surface
    SDL_Surface* CreateSplashScreenSurface(int width, int height, const std::string& statusText = "Loading...");

    // Helper to convert SDL_Surface to RGBA pixel data
    std::vector<uint8_t> SurfaceToRGBA(SDL_Surface* surface);

    // Check if text renderer is ready to use
    bool IsReady() const { return font_ != nullptr; }
    
    // Check if current font is a variable font
    bool IsVariableFont() const { return isVariableFont_; }
    
    // Get font family name
    std::string GetFontFamily() const { return fontFamily_; }

private:
    TTF_Font* font_;
    int currentFontSize_;
    bool ttfInitialized_;
    bool isVariableFont_;
    std::string fontFamily_;
    std::string currentFontPath_;
    std::map<std::string, float> fontVariations_;  // Current variable font settings

    // Helper methods
    bool GetTextSize(const std::string& text, TTF_Font* font, int* width, int* height);
    SDL_Surface* RenderTextLine(const std::string& text, SDL_Color color, TTF_Font* font);
    void BlitTextToSurface(SDL_Surface* textSurface, SDL_Surface* targetSurface, int x, int y);
    TTF_Font* LoadSystemFontInternal(int fontSize);
    
    // Variable font helpers
    bool LoadApplicationFont(int fontSize);
    TTF_Font* CreateFontWithVariations(const std::string& fontPath, int fontSize, 
                                       float weight = 400.0f, float width = 100.0f);
    void ApplyFontVariations(TTF_Font* font);
    std::string GetApplicationFontPath();
};
