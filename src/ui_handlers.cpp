#include "viewer.h"
#include <SDL3/SDL.h>
#include <cmath>
#include "logging.h"

#ifdef _WIN32
#include <commdlg.h>
#endif

extern AppContext g_ctx;

//
// SDL3 UI Action Helpers
//

static SDL_Rect GetCloseButtonRect() {
    int w, h;
    SDL_GetWindowSize(g_ctx.window, &w, &h);
    return { w - 30, 0, 30, 20 };
}

static void OpenFileAction() {
#ifdef HAVE_DATADOG
    auto openSpan = Logger::CreateSpan("ui.open_file");
#endif
    
#ifdef _WIN32
    // On Windows, use the native file dialog
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    
    // Get the native window handle from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(g_ctx.window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"All Image Files\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tiff;*.tif;*.ico;*.webp;*.heic;*.heif;*.avif;*.cr2;*.cr3;*.nef;*.dng;*.arw;*.orf;*.rw2\0All Files\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
    
    if (GetOpenFileNameW(&ofn)) {
#ifdef HAVE_DATADOG
        openSpan.set_tag("file_selected", "true");
        std::string utf8Path;
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, szFile, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Size > 0) {
            std::vector<char> utf8Buf(utf8Size);
            WideCharToMultiByte(CP_UTF8, 0, szFile, -1, utf8Buf.data(), utf8Size, nullptr, nullptr);
            utf8Path = std::string(utf8Buf.data());
            openSpan.set_tag("file_path", utf8Path);
        }
#endif
        LoadImageFromFile(szFile);
        GetImagesInDirectory(szFile);
    } else {
#ifdef HAVE_DATADOG
        openSpan.set_tag("file_selected", "false");
#endif
    }
#else
    // For other platforms, could use SDL3 file dialogs when they become available
    // For now, show a simple message
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Open File", 
                            "File dialog not implemented on this platform yet. Use drag and drop instead.", 
                            g_ctx.window);
#endif
}

static void ToggleFullScreen() {
#ifdef HAVE_DATADOG
    auto fullscreenSpan = Logger::CreateSpan("ui.toggle_fullscreen");
    fullscreenSpan.set_tag("entering_fullscreen", !g_ctx.isFullScreen ? "true" : "false");
#endif
    
    if (!g_ctx.isFullScreen) {
        // Save current window state
        SDL_GetWindowPosition(g_ctx.window, &g_ctx.savedWindowRect.x, &g_ctx.savedWindowRect.y);
        SDL_GetWindowSize(g_ctx.window, &g_ctx.savedWindowRect.w, &g_ctx.savedWindowRect.h);
        g_ctx.savedMaximized = (SDL_GetWindowFlags(g_ctx.window) & SDL_WINDOW_MAXIMIZED) != 0;
        
        // Enter fullscreen
        SDL_SetWindowFullscreen(g_ctx.window, true);
        g_ctx.isFullScreen = true;
    } else {
        // Exit fullscreen
        SDL_SetWindowFullscreen(g_ctx.window, false);
        
        // Restore previous window state
        SDL_SetWindowPosition(g_ctx.window, g_ctx.savedWindowRect.x, g_ctx.savedWindowRect.y);
        SDL_SetWindowSize(g_ctx.window, g_ctx.savedWindowRect.w, g_ctx.savedWindowRect.h);
        
        if (g_ctx.savedMaximized) {
            SDL_MaximizeWindow(g_ctx.window);
        }
        
        g_ctx.isFullScreen = false;
    }
    FitImageToWindow();
}

//
// SDL3 Event Handlers
//

void HandleKeyboardEvent(const SDL_KeyboardEvent& event) {
#ifdef HAVE_DATADOG
    auto keySpan = Logger::CreateSpan("ui.keydown");
    keySpan.set_tag("key_code", std::to_string(static_cast<int>(event.key)));
#endif
    
    bool ctrlPressed = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    bool shiftPressed = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    
#ifdef HAVE_DATADOG
    keySpan.set_tag("ctrl_pressed", ctrlPressed ? "true" : "false");
    keySpan.set_tag("shift_pressed", shiftPressed ? "true" : "false");
#endif

    switch (event.key) {
    case SDLK_RIGHT:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "next_image");
#endif
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex + 1) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
        
    case SDLK_LEFT:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "previous_image");
#endif
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex - 1 + g_ctx.imageFiles.size()) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
        
    case SDLK_UP:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "rotate_clockwise");
#endif
        RotateImage(true);
        break;
        
    case SDLK_DOWN:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "rotate_counterclockwise");
#endif
        RotateImage(false);
        break;
        
    case SDLK_DELETE:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "delete_image");
#endif
        DeleteCurrentImage();
        break;
        
    case SDLK_F11:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "toggle_fullscreen");
#endif
        ToggleFullScreen();
        break;
        
    case SDLK_ESCAPE:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "quit");
#endif
        // Send quit event
        SDL_Event quit_event;
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
        break;
        
    case SDLK_O:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "open_file");
#endif
            OpenFileAction();
        }
        break;
        
    case SDLK_S:
        if (ctrlPressed && shiftPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "save_as");
#endif
            SaveImageAs();
        } else if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "save");
#endif
            SaveImage();
        }
        break;
        
    case SDLK_C:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "copy");
#endif
            HandleCopy();
        }
        break;
        
    case SDLK_V:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "paste");
#endif
            HandlePaste();
        }
        break;
        
    case SDLK_0:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "center_image");
#endif
            CenterImage(true);
        }
        break;
        
    case SDLK_PLUS:
    case SDLK_EQUALS:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "zoom_in");
#endif
            ZoomImage(1.25f);
        }
        break;
        
    case SDLK_MINUS:
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "zoom_out");
#endif
            ZoomImage(0.8f);
        }
        break;
    }
}

void HandleMouseEvent(const SDL_MouseButtonEvent& event) {
    if (event.button == SDL_BUTTON_LEFT && event.down) {
        // Check if clicking on close button (if we want to keep that functionality)
        SDL_Rect closeRect = GetCloseButtonRect();
        if (event.x >= closeRect.x && event.x < closeRect.x + closeRect.w &&
            event.y >= closeRect.y && event.y < closeRect.y + closeRect.h) {
            // Send quit event
            SDL_Event quit_event;
            quit_event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit_event);
            return;
        }
        
        // Check if clicking on image for dragging
        if (g_ctx.imageData.isValid() && IsPointInImage(event.x, event.y)) {
            // Start image dragging - this would need to be implemented
            // For now, just do nothing
        }
    }
    
    if (event.button == SDL_BUTTON_RIGHT && event.down) {
        ShowContextMenu(event.x, event.y);
    }
    
    if (event.button == SDL_BUTTON_LEFT && event.down && event.clicks == 2) {
        // Double-click to fit image
        FitImageToWindow();
    }
}

void HandleMouseMotion(const SDL_MouseMotionEvent& event) {
    // Check if hovering over close button
    SDL_Rect closeRect = GetCloseButtonRect();
    bool isHoveringNow = (event.x >= closeRect.x && event.x < closeRect.x + closeRect.w &&
                          event.y >= closeRect.y && event.y < closeRect.y + closeRect.h);
    
    if (isHoveringNow != g_ctx.isHoveringClose) {
        g_ctx.isHoveringClose = isHoveringNow;
        // In SDL3, we would trigger a redraw here
    }
    
    // Handle image dragging if mouse is down
    static bool isDragging = false;
    static int dragStartX = 0, dragStartY = 0;
    
    if (event.state & SDL_BUTTON_LMASK) {
        if (!isDragging && g_ctx.imageData.isValid() && IsPointInImage(event.x, event.y)) {
            isDragging = true;
            dragStartX = event.x;
            dragStartY = event.y;
        }
        
        if (isDragging && g_ctx.zoomFactor > 0.0f && std::isfinite(g_ctx.zoomFactor)) {
            float deltaX = static_cast<float>(event.x - dragStartX);
            float deltaY = static_cast<float>(event.y - dragStartY);
            
            float safeDivisor = std::max(g_ctx.zoomFactor, 0.01f);
            float offsetDeltaX = deltaX / safeDivisor;
            float offsetDeltaY = deltaY / safeDivisor;
            
            if (std::isfinite(offsetDeltaX) && std::isfinite(offsetDeltaY)) {
                constexpr float kMaxOffsetDelta = 10000.0f;
                offsetDeltaX = std::clamp(offsetDeltaX, -kMaxOffsetDelta, kMaxOffsetDelta);
                offsetDeltaY = std::clamp(offsetDeltaY, -kMaxOffsetDelta, kMaxOffsetDelta);
                
                float newOffsetX = g_ctx.offsetX + offsetDeltaX;
                float newOffsetY = g_ctx.offsetY + offsetDeltaY;
                
                constexpr float kMaxAbsoluteOffset = 1000000.0f;
                if (std::isfinite(newOffsetX) && std::isfinite(newOffsetY) &&
                    std::abs(newOffsetX) < kMaxAbsoluteOffset && 
                    std::abs(newOffsetY) < kMaxAbsoluteOffset) {
                    
                    g_ctx.offsetX = newOffsetX;
                    g_ctx.offsetY = newOffsetY;
                    dragStartX = event.x;
                    dragStartY = event.y;
                    
                    // Log extreme values for debugging
                    if (std::abs(newOffsetX) > 100000.0f || std::abs(newOffsetY) > 100000.0f) {
                        Logger::LogCriticalState(g_ctx.zoomFactor, g_ctx.offsetX, g_ctx.offsetY, "mouse_drag_extreme_offset");
                    }
                } else {
                    Logger::LogCriticalState(g_ctx.zoomFactor, newOffsetX, newOffsetY, "mouse_drag_prevented_crash");
                    isDragging = false;
                }
            } else {
                isDragging = false;
            }
        }
    } else {
        isDragging = false;
    }
}

void HandleMouseWheel(const SDL_MouseWheelEvent& event) {
    float zoomFactor = (event.y > 0) ? 1.1f : 0.9f;
    ZoomImage(zoomFactor);
}

void ShowContextMenu(int x, int y) {
    // For now, implement a simple menu using message boxes or print to console
    // A full implementation would need a proper context menu system
    
    Logger::Info("Context menu requested at (%d, %d)", x, y);
    
    // For demonstration, let's show a simple message with options
    // In a real implementation, you'd want to use a native context menu or implement a custom one
    
#ifdef _WIN32
    // Use Windows context menu
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Open Image\tCtrl+O");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 2, L"Copy\tCtrl+C");
    AppendMenuW(hMenu, MF_STRING, 3, L"Paste\tCtrl+V");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 4, L"Next Image\tRight Arrow");
    AppendMenuW(hMenu, MF_STRING, 5, L"Previous Image\tLeft Arrow");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 6, L"Rotate Clockwise\tUp Arrow");
    AppendMenuW(hMenu, MF_STRING, 7, L"Rotate Counter-Clockwise\tDown Arrow");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 8, L"Zoom In\tCtrl++");
    AppendMenuW(hMenu, MF_STRING, 9, L"Zoom Out\tCtrl+-");
    AppendMenuW(hMenu, MF_STRING, 10, L"Fit to Window\tCtrl+0");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 11, L"Save\tCtrl+S");
    AppendMenuW(hMenu, MF_STRING, 12, L"Save As\tCtrl+Shift+S");
    AppendMenuW(hMenu, MF_STRING, 13, L"Delete Image\tDelete");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 14, L"Full Screen\tF11");
    AppendMenuW(hMenu, MF_STRING, 15, L"Exit\tEsc");

    // Get the native window handle from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(g_ctx.window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    
    // Convert SDL coordinates to screen coordinates
    POINT pt = {x, y};
    ClientToScreen(hwnd, &pt);

    int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    // Handle menu selection
    switch (cmd) {
    case 1: OpenFileAction(); break;
    case 2: HandleCopy(); break;
    case 3: HandlePaste(); break;
    case 4: 
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex + 1) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
    case 5:
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex - 1 + g_ctx.imageFiles.size()) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
    case 6: RotateImage(true); break;
    case 7: RotateImage(false); break;
    case 8: ZoomImage(1.25f); break;
    case 9: ZoomImage(0.8f); break;
    case 10: CenterImage(true); break;
    case 11: SaveImage(); break;
    case 12: SaveImageAs(); break;
    case 13: DeleteCurrentImage(); break;
    case 14: ToggleFullScreen(); break;
    case 15: {
        SDL_Event quit_event;
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
        break;
    }
    }
#else
    // For other platforms, show a simple message box with common options
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Context Menu", 
                            "Right-click context menu - use keyboard shortcuts instead:\n"
                            "Ctrl+O: Open File\n"
                            "Arrow Keys: Navigate/Rotate\n"
                            "Ctrl+0: Fit to Window\n"
                            "F11: Fullscreen\n"
                            "Esc: Exit", 
                            g_ctx.window);
#endif
}
