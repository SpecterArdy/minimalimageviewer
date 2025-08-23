#include "viewer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iostream>
#include <vector>
#include <string>
#include "ocio_shim.h"
#include "vulkan_renderer.h"
#include "logging.h"
#include "text_renderer.h"

namespace OCIO = OCIO_NAMESPACE;

AppContext g_ctx;

// Convert wide string to UTF-8
std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

// Convert UTF-8 to wide string
std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

void CenterImage(bool resetZoom) {
#ifdef HAVE_DATADOG
    auto span = Logger::CreateSpan("center_image");
    span.set_tag("reset_zoom", resetZoom ? "true" : "false");
#endif
    
    if (resetZoom) {
        g_ctx.zoomFactor = 1.0f;
    }
    g_ctx.rotationAngle = 0;
    g_ctx.offsetX = 0.0f;
    g_ctx.offsetY = 0.0f;
    FitImageToWindow();
}

void HandleSDLEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data) {
                Logger::Info("Dropped file: %s", event.drop.data);
                LoadImageFromFile(event.drop.data);
                GetImagesInDirectory(event.drop.data);
            }
            break;
            
        case SDL_EVENT_KEY_DOWN:
            HandleKeyboardEvent(event.key);
            break;
            
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            HandleMouseEvent(event.button);
            break;
            
        case SDL_EVENT_MOUSE_MOTION:
            HandleMouseMotion(event.motion);
            break;
            
        case SDL_EVENT_MOUSE_WHEEL:
            HandleMouseWheel(event.wheel);
            break;
            
        case SDL_EVENT_WINDOW_RESIZED:
            FitImageToWindow();
            break;
    }
}

int main(int argc, char* argv[]) {
    // Initialize logging and crash handlers as early as possible
    Logger::Init(L"MinimalImageViewer");
    Logger::InstallCrashHandlers();
    Logger::Info("SDL3 Application starting");

#ifdef HAVE_DATADOG
    auto appSpan = Logger::CreateSpan("application.startup");
    appSpan.set_tag("sdl_version", "3");
#endif

    // Create splash screen window first
    SDL_Window* splashWindow = nullptr;
    SDL_Renderer* splashRenderer = nullptr;
    TextRenderer* tr = nullptr;  // Global scope for entire splash sequence
    
    // Helper lambda to update splash screen with text and progress
    auto updateSplash = [&](const std::string& status, int progressWidth) {
        if (splashRenderer && tr) {
            SDL_Surface* textSurface = tr->CreateSplashScreenSurface(400, 300, status);
            if (textSurface) {
                SDL_Texture* textTexture = SDL_CreateTextureFromSurface(splashRenderer, textSurface);
                if (textTexture) {
                    SDL_SetRenderDrawBlendMode(splashRenderer, SDL_BLENDMODE_BLEND);
                    SDL_RenderClear(splashRenderer);
                    SDL_RenderTexture(splashRenderer, textTexture, nullptr, nullptr);
                    
                    // Draw progress bar
                    SDL_SetRenderDrawColor(splashRenderer, 0, 150, 200, 255);
                    SDL_FRect progress = {50, 220, static_cast<float>(progressWidth), 20};
                    SDL_RenderFillRect(splashRenderer, &progress);
                    
                    SDL_RenderPresent(splashRenderer);
                    SDL_DestroyTexture(textTexture);
                }
                SDL_DestroySurface(textSurface);
            }
        }
    };
    
    try {
        // Initialize SDL3
        std::cout << "[INIT] Initializing SDL3..." << std::endl;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            Logger::Error("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }
        std::cout << "[INIT] SDL3 initialized successfully" << std::endl;
        
        // Create splash screen
        splashWindow = SDL_CreateWindow("Minimal Image Viewer - Initializing...", 
                                       400, 300, SDL_WINDOW_BORDERLESS);
        if (splashWindow) {
            SDL_SetWindowPosition(splashWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            splashRenderer = SDL_CreateRenderer(splashWindow, nullptr);
            
            if (splashRenderer) {
                // Initialize TextRenderer for splash screen - single instance for all splash updates
                tr = new TextRenderer();
                if (tr->Initialize()) {
                    // Create splash screen surface with text
                    SDL_Surface* textSurface = tr->CreateSplashScreenSurface(400, 300, "Initializing...");
                    if (textSurface) {
                        SDL_Texture* textTexture = SDL_CreateTextureFromSurface(splashRenderer, textSurface);
                        if (textTexture) {
                            SDL_SetRenderDrawBlendMode(splashRenderer, SDL_BLENDMODE_BLEND);
                            SDL_RenderClear(splashRenderer);
                            SDL_RenderTexture(splashRenderer, textTexture, nullptr, nullptr);
                            
                            // Draw initial progress bar (0%)
                            SDL_SetRenderDrawColor(splashRenderer, 0, 150, 200, 255);
                            SDL_FRect progress = {50, 220, 0, 20};
                            SDL_RenderFillRect(splashRenderer, &progress);
                            
                            SDL_RenderPresent(splashRenderer);
                            SDL_DestroyTexture(textTexture);
                        }
                        SDL_DestroySurface(textSurface);
                    }
                } else {
                    Logger::Warn("TextRenderer init failed; showing splash without text");
                    // Fallback to simple rectangles
                    SDL_SetRenderDrawColor(splashRenderer, 30, 30, 30, 255);
                    SDL_RenderClear(splashRenderer);
                    SDL_SetRenderDrawColor(splashRenderer, 70, 70, 70, 255);
                    SDL_FRect titleRect = {0, 0, 400, 80};
                    SDL_RenderFillRect(splashRenderer, &titleRect);
                    SDL_RenderPresent(splashRenderer);
                    delete tr;
                    tr = nullptr;
                }
            }
        }

        // Update splash: COM initialization (25%)
        updateSplash("Initializing COM...", 75);
        
        // Initialize COM for Windows file operations
#ifdef _WIN32
        std::cout << "[INIT] Initializing COM for Windows file operations..." << std::endl;
        if (FAILED(CoInitialize(nullptr))) {
            Logger::Error("Failed to initialize COM");
            if (splashRenderer) SDL_DestroyRenderer(splashRenderer);
            if (splashWindow) SDL_DestroyWindow(splashWindow);
            SDL_Quit();
            return 1;
        }
        std::cout << "[INIT] COM initialized successfully" << std::endl;
#endif

        // Update splash: OpenColorIO initialization (50%)
        updateSplash("Initializing OpenColorIO...", 150);
        
        // Initialize OpenColorIO
        std::cout << "[INIT] Initializing OpenColorIO..." << std::endl;
        try {
            g_ctx.ocioConfig = OCIO::GetCurrentConfig();
            if (g_ctx.ocioConfig) {
                bool hasBasicSpaces = false;
                try {
                    int numSpaces = g_ctx.ocioConfig->getNumColorSpaces();
                    hasBasicSpaces = (numSpaces > 0);
                } catch (...) {
                    hasBasicSpaces = false;
                }
                if (!hasBasicSpaces) {
                    g_ctx.ocioConfig = OCIO::Config::CreateRaw();
                }
            }
        } catch (const OCIO::Exception& e) {
            try {
                g_ctx.ocioConfig = OCIO::Config::CreateRaw();
            } catch (...) {
                g_ctx.ocioConfig = nullptr;
            }
        } catch (...) {
            try {
                g_ctx.ocioConfig = OCIO::Config::CreateRaw();
            } catch (...) {
                g_ctx.ocioConfig = nullptr;
            }
        }

        // Check OCIO environment
        const char* ocioEnv = SDL_getenv("OCIO");
        g_ctx.ocioEnabled = (ocioEnv && *ocioEnv && g_ctx.ocioConfig);
        g_ctx.displayDevice = "sRGB";

        if (!g_ctx.ocioEnabled) {
            Logger::Info("OpenColorIO: disabled (no $OCIO or no config)");
            std::cout << "[INIT] OpenColorIO: disabled (no $OCIO environment variable or config)" << std::endl;
        } else {
            Logger::Info("OpenColorIO: enabled");
            std::cout << "[INIT] OpenColorIO: enabled with color management" << std::endl;
        }

        // Update splash: Creating main window (75%)
        updateSplash("Creating main window...", 225);
        
        // Create SDL window
        std::cout << "[INIT] Creating main application window..." << std::endl;
        g_ctx.window = SDL_CreateWindow("Minimal Image Viewer", 1280, 720, 
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        if (!g_ctx.window) {
            Logger::Error("SDL_CreateWindow failed: %s", SDL_GetError());
            if (splashRenderer) SDL_DestroyRenderer(splashRenderer);
            if (splashWindow) SDL_DestroyWindow(splashWindow);
            SDL_Quit();
            return 1;
        }
        std::cout << "[INIT] Main window created successfully" << std::endl;

        // Initialize synchronization
        g_ctx.renderLock = SDL_CreateMutex();
        if (!g_ctx.renderLock) {
            Logger::Error("Failed to create render mutex");
            SDL_DestroyWindow(g_ctx.window);
            SDL_Quit();
            return 1;
        }

        // Update splash: Vulkan initialization (93%)
        updateSplash("Initializing Vulkan...", 280);
        
        // Initialize Vulkan renderer
        std::cout << "[INIT] Initializing Vulkan renderer..." << std::endl;
        Logger::Info("Initializing Vulkan renderer...");
        g_ctx.renderer = std::make_unique<VulkanRenderer>();
        if (!g_ctx.renderer->Initialize(g_ctx.window)) {
            Logger::Error("Failed to initialize Vulkan renderer");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", 
                                   "Failed to initialize Vulkan renderer.", g_ctx.window);
            if (splashRenderer) SDL_DestroyRenderer(splashRenderer);
            if (splashWindow) SDL_DestroyWindow(splashWindow);
            SDL_DestroyMutex(g_ctx.renderLock);
            SDL_DestroyWindow(g_ctx.window);
            SDL_Quit();
            return 1;
        }

        Logger::Info("Vulkan renderer initialized successfully");
        std::cout << "[INIT] Vulkan renderer initialized successfully" << std::endl;

        // Finalize splash: Complete initialization (100%)
        if (splashRenderer && tr) {
            SDL_Surface* textSurface = tr->CreateSplashScreenSurface(400, 300, "Ready!");
            if (textSurface) {
                SDL_Texture* textTexture = SDL_CreateTextureFromSurface(splashRenderer, textSurface);
                if (textTexture) {
                    SDL_SetRenderDrawBlendMode(splashRenderer, SDL_BLENDMODE_BLEND);
                    SDL_RenderClear(splashRenderer);
                    SDL_RenderTexture(splashRenderer, textTexture, nullptr, nullptr);
                    
                    // Draw complete progress bar (100%) - green for complete
                    SDL_SetRenderDrawColor(splashRenderer, 0, 200, 100, 255);
                    SDL_FRect progress = {50, 220, 300, 20};
                    SDL_RenderFillRect(splashRenderer, &progress);
                    
                    SDL_RenderPresent(splashRenderer);
                    SDL_DestroyTexture(textTexture);
                }
                SDL_DestroySurface(textSurface);
            }
        }
        
        // Brief pause to show completion
        SDL_Delay(500);
        
        // Clean up TextRenderer before destroying splash window
        if (tr) {
            Logger::Info("Cleaning up splash TextRenderer...");
            tr->Shutdown();  // Explicit shutdown before delete
            delete tr;
            tr = nullptr;
            Logger::Info("Splash TextRenderer cleaned up successfully");
        }
        
        // Clean up splash screen
        if (splashRenderer) {
            SDL_DestroyRenderer(splashRenderer);
            splashRenderer = nullptr;
        }
        if (splashWindow) {
            SDL_DestroyWindow(splashWindow);
            splashWindow = nullptr;
        }
        
        std::cout << "[INIT] Initialization complete - starting main application" << std::endl;
        
        // Process command line arguments
        if (argc > 1) {
            std::cout << "[INIT] Loading image from command line: " << argv[1] << std::endl;
            LoadImageFromFile(argv[1]);
            GetImagesInDirectory(argv[1]);
        }

        // Initialize FPS timer
        g_ctx.fpsLastTimeMS = SDL_GetTicks();

        // Main event loop
        bool running = true;
        std::vector<std::string> pendingDrops;

        SDL_Event event;
        while (running) {
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;

                    case SDL_EVENT_DROP_BEGIN:
                        pendingDrops.clear();
                        break;

                    case SDL_EVENT_DROP_FILE:
                        if (event.drop.data) {
                            pendingDrops.emplace_back(event.drop.data);
                            Logger::Info("Dropped file: %s at (%.1f, %.1f)", 
                                       event.drop.data, event.drop.x, event.drop.y);
                        }
                        break;

                    case SDL_EVENT_DROP_COMPLETE:
                        for (const auto& path : pendingDrops) {
                            Logger::Info("Processing dropped file: %s", path.c_str());
                            LoadImageFromFile(path.c_str());
                            GetImagesInDirectory(path.c_str());
                            break; // Only load the first file for now
                        }
                        pendingDrops.clear();
                        break;

                    default:
                        HandleSDLEvent(event);
                        break;
                }
            }

            // FPS accounting
            ++g_ctx.fpsFrameCount;
            uint64_t now = SDL_GetTicks();
            uint64_t elapsed = now - g_ctx.fpsLastTimeMS;
            if (elapsed >= 1000) {
                g_ctx.fps = static_cast<float>(g_ctx.fpsFrameCount) * 1000.0f / static_cast<float>(elapsed);
                g_ctx.fpsFrameCount = 0;
                g_ctx.fpsLastTimeMS = now;

                if (g_ctx.showFps) {
                    std::string title = "Minimal Image Viewer - " + std::to_string(g_ctx.fps) + " FPS";
                    SDL_SetWindowTitle(g_ctx.window, title.c_str());
                }
            }

            // Handle renderer reset
            if (g_ctx.rendererNeedsReset) {
                while (g_ctx.renderInProgress.load(std::memory_order_acquire)) {
                    SDL_Delay(0);
                }

                SDL_LockMutex(g_ctx.renderLock);

                const bool deviceLost = (g_ctx.renderer && g_ctx.renderer->IsDeviceLost());
                if (g_ctx.renderer && deviceLost) {
                    Logger::Warn("Reset: device lost detected — performing full renderer rebuild");
                    g_ctx.renderer->Shutdown();
                    g_ctx.renderer.reset();
                    g_ctx.renderer = std::make_unique<VulkanRenderer>();
                    if (!g_ctx.renderer->Initialize(g_ctx.window)) {
                        Logger::Error("Reset: VulkanRenderer re-initialization FAILED after device lost");
                        g_ctx.renderer.reset();
                    } else {
                        Logger::Info("Reset: VulkanRenderer re-initialized after device lost");
                    }
                } else if (g_ctx.renderer) {
                    int w, h;
                    SDL_GetWindowSize(g_ctx.window, &w, &h);
                    Logger::Warn("Reset: swapchain recreation (w={}, h={})", w, h);
                    g_ctx.renderer->Resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    g_ctx.renderer->ClearErrorFlags();
                    Logger::Info("Reset: swapchain recreated");
                }

                g_ctx.rendererNeedsReset = false;
                SDL_UnlockMutex(g_ctx.renderLock);
            }

            // Render frame
            if (g_ctx.renderer) {
                DrawImage(); // This will be implemented in image_drawing.cpp
            }

            SDL_Delay(1);
        }

    } catch (const std::exception& e) {
        Logger::Error("Unhandled std::exception: %s", e.what());
        Logger::LogStackTrace();
        Logger::DumpNow("Unhandled std::exception");
    } catch (...) {
        Logger::Error("Unhandled unknown exception");
        Logger::LogStackTrace();
        Logger::DumpNow("Unhandled unknown exception");
    }

    // Cleanup - VERY IMPORTANT: Clean up in reverse order of initialization
    Logger::Info("Shutting down application");
    
    // 1. Clean up any remaining TextRenderer instances first (before SDL shutdown)
    if (tr) {
        Logger::Info("Emergency cleanup of splash TextRenderer...");
        tr->Shutdown();
        delete tr;
        tr = nullptr;
    }
    
    // 2. Shutdown Vulkan renderer
    if (g_ctx.renderer) {
        Logger::Info("Shutting down Vulkan renderer...");
        g_ctx.renderer->Shutdown();
        g_ctx.renderer.reset();
        Logger::Info("Vulkan renderer shut down");
    }
    
    // 3. Destroy mutex
    if (g_ctx.renderLock) {
        Logger::Info("Destroying render mutex...");
        SDL_DestroyMutex(g_ctx.renderLock);
        g_ctx.renderLock = nullptr;
        Logger::Info("Render mutex destroyed");
    }
    
    // 4. Destroy main window
    if (g_ctx.window) {
        Logger::Info("Destroying main window...");
        SDL_DestroyWindow(g_ctx.window);
        g_ctx.window = nullptr;
        Logger::Info("Main window destroyed");
    }
    
    // 5. Uninitialize COM before SDL
#ifdef _WIN32
    Logger::Info("Uninitializing COM...");
    CoUninitialize();
    Logger::Info("COM uninitialized");
#endif
    
    // 6. Quit SDL last
    Logger::Info("Shutting down SDL...");
    SDL_Quit();
    Logger::Info("SDL shut down successfully");
    
    // 7. Logger shutdown last
    Logger::Shutdown();
    
    return 0;
}
