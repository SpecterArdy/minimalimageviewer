#include "viewer.h"
#include <cmath>
#include "logging.h"

extern AppContext g_ctx;

//
// UI Action Helpers
//

static RECT GetCloseButtonRect() {
    RECT clientRect{};
    GetClientRect(g_ctx.hWnd, &clientRect);
    return { clientRect.right - 30, 0, clientRect.right, 20 };
}

static void OpenFileAction() {
#ifdef HAVE_DATADOG
    auto openSpan = Logger::CreateSpan("ui.open_file");
#endif
    
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.hwndOwner = g_ctx.hWnd;
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
}

static void ToggleFullScreen() {
#ifdef HAVE_DATADOG
    auto fullscreenSpan = Logger::CreateSpan("ui.toggle_fullscreen");
    fullscreenSpan.set_tag("entering_fullscreen", !g_ctx.isFullScreen ? "true" : "false");
#endif
    
    if (!g_ctx.isFullScreen) {
        g_ctx.savedStyle = GetWindowLong(g_ctx.hWnd, GWL_STYLE);
        GetWindowRect(g_ctx.hWnd, &g_ctx.savedRect);
        HMONITOR hMonitor = MonitorFromWindow(g_ctx.hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);
        SetWindowLong(g_ctx.hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_ctx.hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_ctx.isFullScreen = true;
    }
    else {
        SetWindowLong(g_ctx.hWnd, GWL_STYLE, g_ctx.savedStyle | WS_VISIBLE);
        SetWindowPos(g_ctx.hWnd, HWND_NOTOPMOST, g_ctx.savedRect.left, g_ctx.savedRect.top,
            g_ctx.savedRect.right - g_ctx.savedRect.left, g_ctx.savedRect.bottom - g_ctx.savedRect.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_ctx.isFullScreen = false;
    }
    FitImageToWindow();
}

//
// Message Handlers
//

static void OnPaint(HWND hWnd) {
#ifdef HAVE_DATADOG
    auto paintSpan = Logger::CreateSpan("ui.paint");
    paintSpan.set_tag("minimized", IsIconic(hWnd) ? "true" : "false");
    paintSpan.set_tag("has_image", g_ctx.imageData.isValid() ? "true" : "false");
#endif
    
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT clientRect{};
    GetClientRect(hWnd, &clientRect);
    
#ifdef HAVE_DATADOG
    paintSpan.set_tag("width", std::to_string(clientRect.right));
    paintSpan.set_tag("height", std::to_string(clientRect.bottom));
#endif

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, memBitmap));

    FillRect(memDC, &clientRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    if (g_ctx.imageData.isValid() && !IsIconic(hWnd)) {
        DrawImage(memDC, clientRect, g_ctx);
    }
    else if (!g_ctx.imageData.isValid()) {
        SetTextColor(memDC, RGB(255, 255, 255));
        SetBkMode(memDC, TRANSPARENT);
        DrawTextW(memDC, L"Right-click for options or drag an image here", -1, &clientRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (g_ctx.showFilePath) {
        std::wstring pathToDisplay;
        if (!g_ctx.currentFilePathOverride.empty()) {
            pathToDisplay = g_ctx.currentFilePathOverride;
        }
        else if (g_ctx.currentImageIndex >= 0 && g_ctx.currentImageIndex < static_cast<int>(g_ctx.imageFiles.size())) {
            pathToDisplay = g_ctx.imageFiles[g_ctx.currentImageIndex];
        }

        if (!pathToDisplay.empty()) {
            SetBkMode(memDC, TRANSPARENT);
            HFONT hPathFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hOldPathFont = static_cast<HFONT>(SelectObject(memDC, hPathFont));

            RECT textRect = clientRect;
            textRect.bottom -= 5;
            textRect.right -= 5;

            RECT shadowRect = textRect;
            OffsetRect(&shadowRect, 1, 1);
            SetTextColor(memDC, RGB(0, 0, 0));
            DrawTextW(memDC, pathToDisplay.c_str(), -1, &shadowRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SetTextColor(memDC, RGB(220, 220, 220));
            DrawTextW(memDC, pathToDisplay.c_str(), -1, &textRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(memDC, hOldPathFont);
            DeleteObject(hPathFont);
        }
    }

    RECT closeRect = GetCloseButtonRect();
    HPEN hPen;
    if (g_ctx.isHoveringClose) {
        hPen = CreatePen(PS_SOLID, 2, RGB(220, 50, 50));
    }
    else {
        hPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    }
    HPEN hOldPen = static_cast<HPEN>(SelectObject(memDC, hPen));

    MoveToEx(memDC, closeRect.left + 9, closeRect.top + 6, nullptr);
    LineTo(memDC, closeRect.right - 9, closeRect.bottom - 6);
    MoveToEx(memDC, closeRect.right - 9, closeRect.top + 6, nullptr);
    LineTo(memDC, closeRect.left + 9, closeRect.bottom - 6);

    SelectObject(memDC, hOldPen);
    DeleteObject(hPen);

    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
        ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
        memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(hWnd, &ps);
}

static void OnKeyDown(WPARAM wParam) {
#ifdef HAVE_DATADOG
    auto keySpan = Logger::CreateSpan("ui.keydown");
    keySpan.set_tag("key_code", std::to_string(static_cast<int>(wParam)));
#endif
    
    bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
#ifdef HAVE_DATADOG
    keySpan.set_tag("ctrl_pressed", ctrlPressed ? "true" : "false");
#endif

    switch (wParam) {
    case VK_RIGHT:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "next_image");
#endif
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex + 1) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
    case VK_LEFT:
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "previous_image");
#endif
        if (!g_ctx.imageFiles.empty()) {
            g_ctx.currentImageIndex = (g_ctx.currentImageIndex - 1 + g_ctx.imageFiles.size()) % g_ctx.imageFiles.size();
            LoadImageFromFile(g_ctx.imageFiles[g_ctx.currentImageIndex].c_str());
        }
        break;
    case VK_UP:    
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "rotate_clockwise");
#endif
        RotateImage(true); 
        break;
    case VK_DOWN:  
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "rotate_counterclockwise");
#endif
        RotateImage(false); 
        break;
    case VK_DELETE: 
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "delete_image");
#endif
        DeleteCurrentImage(); 
        break;
    case VK_F11:   
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "toggle_fullscreen");
#endif
        ToggleFullScreen(); 
        break;
    case VK_ESCAPE: 
#ifdef HAVE_DATADOG
        keySpan.set_tag("action", "quit");
#endif
        PostQuitMessage(0); 
        break;
    case 'O':      
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "open_file");
#endif
            OpenFileAction();
        }
        break;
    case 'S':      
        if (ctrlPressed && (GetKeyState(VK_SHIFT) & 0x8000)) {
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
    case 'C':      
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "copy");
#endif
            HandleCopy();
        }
        break;
    case 'V':      
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "paste");
#endif
            HandlePaste();
        }
        break;
    case '0':      
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "center_image");
#endif
            CenterImage(true);
        }
        break;
    case VK_OEM_PLUS:  
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "zoom_in");
#endif
            ZoomImage(1.25f);
        }
        break;
    case VK_OEM_MINUS: 
        if (ctrlPressed) {
#ifdef HAVE_DATADOG
            keySpan.set_tag("action", "zoom_out");
#endif
            ZoomImage(0.8f);
        }
        break;
    }
}


static void OnContextMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open Image\tCtrl+O");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"Copy\tCtrl+C");
    AppendMenuW(hMenu, MF_STRING, IDM_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_NEXT_IMG, L"Next Image\tRight Arrow");
    AppendMenuW(hMenu, MF_STRING, IDM_PREV_IMG, L"Previous Image\tLeft Arrow");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_ROTATE_CW, L"Rotate Clockwise\tUp Arrow");
    AppendMenuW(hMenu, MF_STRING, IDM_ROTATE_CCW, L"Rotate Counter-Clockwise\tDown Arrow");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_IN, L"Zoom In\tCtrl++");
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_OUT, L"Zoom Out\tCtrl+-");
    AppendMenuW(hMenu, MF_STRING, IDM_FIT_TO_WINDOW, L"Fit to Window\tCtrl+0");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_SAVE, L"Save\tCtrl+S");
    AppendMenuW(hMenu, MF_STRING, IDM_SAVE_AS, L"Save As\tCtrl+Shift+S");

    UINT locationFlags = (g_ctx.currentImageIndex != -1) ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(hMenu, locationFlags, IDM_OPEN_LOCATION, L"Open File Location");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (g_ctx.showFilePath ? MF_CHECKED : MF_UNCHECKED), IDM_SHOW_FILE_PATH, L"Show File Path");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_FULLSCREEN, L"Full Screen\tF11");
    AppendMenuW(hMenu, MF_STRING, IDM_DELETE_IMG, L"Delete Image\tDelete");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit\tEsc");

    int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_OPEN:          OpenFileAction(); break;
    case IDM_COPY:          HandleCopy(); break;
    case IDM_PASTE:         HandlePaste(); break;
    case IDM_NEXT_IMG:      OnKeyDown(VK_RIGHT); break;
    case IDM_PREV_IMG:      OnKeyDown(VK_LEFT); break;
    case IDM_ZOOM_IN:       ZoomImage(1.25f); break;
    case IDM_ZOOM_OUT:      ZoomImage(0.8f); break;
    case IDM_FIT_TO_WINDOW: FitImageToWindow(); break;
    case IDM_FULLSCREEN:    ToggleFullScreen(); break;
    case IDM_DELETE_IMG:    DeleteCurrentImage(); break;
    case IDM_EXIT:          PostQuitMessage(0); break;
    case IDM_ROTATE_CW:     RotateImage(true); break;
    case IDM_ROTATE_CCW:    RotateImage(false); break;
    case IDM_SAVE:          SaveImage(); break;
    case IDM_SAVE_AS:       SaveImageAs(); break;
    case IDM_OPEN_LOCATION: OpenFileLocationAction(); break;
    case IDM_SHOW_FILE_PATH:
        g_ctx.showFilePath = !g_ctx.showFilePath;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool isDraggingImage = false;
    static POINT dragStart{};

    switch (message) {
    case WM_PAINT:
        OnPaint(hWnd);
        break;
    case WM_KEYDOWN:
        OnKeyDown(wParam);
        break;
    case WM_MOUSEWHEEL:
        ZoomImage(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.1f : 0.9f);
        break;
    case WM_LBUTTONDBLCLK:
        FitImageToWindow();
        break;
    case WM_RBUTTONUP: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        ClientToScreen(hWnd, &pt);
        OnContextMenu(hWnd, pt);
        break;
    }
    case WM_DROPFILES:
        Logger::Info("WM_DROPFILES message received in WndProc");
        HandleDropFiles(reinterpret_cast<HDROP>(wParam));
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT closeRect = GetCloseButtonRect();
        if (PtInRect(&closeRect, pt)) {
            PostQuitMessage(0);
            return 0;
        }
        if (g_ctx.imageData.isValid() && IsPointInImage(pt, {})) {
            isDraggingImage = true;
            dragStart = pt;
            SetCapture(hWnd);
        }
        else if (!g_ctx.isFullScreen) {
            ReleaseCapture();
            SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;
    }
    case WM_LBUTTONUP:
        if (isDraggingImage) {
            isDraggingImage = false;
            ReleaseCapture();
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT closeRect = GetCloseButtonRect();
        bool isHoveringNow = PtInRect(&closeRect, pt);
        if (isHoveringNow != g_ctx.isHoveringClose) {
            g_ctx.isHoveringClose = isHoveringNow;
            InvalidateRect(hWnd, &closeRect, FALSE);
            SendMessage(hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hWnd), MAKELPARAM(HTCLIENT, 0));
        }
        if (isDraggingImage) {
            // Safety check to prevent division by zero or invalid zoom factor
            if (g_ctx.zoomFactor > 0.0f && std::isfinite(g_ctx.zoomFactor)) {
                // NASA Standard: Calculate offset changes with bounds checking
                float deltaX = static_cast<float>(pt.x - dragStart.x);
                float deltaY = static_cast<float>(pt.y - dragStart.y);
                
                // NASA Standard: Prevent division by very small zoom factors
                float safeDivisor = std::max(g_ctx.zoomFactor, 0.01f);
                
                float offsetDeltaX = deltaX / safeDivisor;
                float offsetDeltaY = deltaY / safeDivisor;
                
                // NASA Standard: Validate offset deltas are finite
                if (std::isfinite(offsetDeltaX) && std::isfinite(offsetDeltaY)) {
                    // NASA Standard: Bound offset changes to prevent extreme values
                    constexpr float kMaxOffsetDelta = 10000.0f;
                    offsetDeltaX = std::clamp(offsetDeltaX, -kMaxOffsetDelta, kMaxOffsetDelta);
                    offsetDeltaY = std::clamp(offsetDeltaY, -kMaxOffsetDelta, kMaxOffsetDelta);
                    
                    float newOffsetX = g_ctx.offsetX + offsetDeltaX;
                    float newOffsetY = g_ctx.offsetY + offsetDeltaY;
                    
                    // NASA Standard: Bound absolute offset values to prevent integer overflow in renderer
                    constexpr float kMaxAbsoluteOffset = 1000000.0f;
                    if (std::isfinite(newOffsetX) && std::isfinite(newOffsetY) &&
                        std::abs(newOffsetX) < kMaxAbsoluteOffset && 
                        std::abs(newOffsetY) < kMaxAbsoluteOffset) {
                        
                        g_ctx.offsetX = newOffsetX;
                        g_ctx.offsetY = newOffsetY;
                        dragStart = pt;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        
                        // Log critical state for crash analysis if values are getting extreme
                        if (std::abs(newOffsetX) > 100000.0f || std::abs(newOffsetY) > 100000.0f) {
                            Logger::LogCriticalState(g_ctx.zoomFactor, g_ctx.offsetX, g_ctx.offsetY, "mouse_drag_extreme_offset");
                        }
                    } else {
                        // Offset would be too large - stop dragging to prevent crash
                        Logger::LogCriticalState(g_ctx.zoomFactor, newOffsetX, newOffsetY, "mouse_drag_prevented_crash");
                        isDraggingImage = false;
                        ReleaseCapture();
                    }
                } else {
                    // Non-finite offset deltas - stop dragging
                    isDraggingImage = false;
                    ReleaseCapture();
                }
            } else {
                // Reset zoom factor to safe value and stop dragging
                g_ctx.zoomFactor = 1.0f;
                isDraggingImage = false;
                ReleaseCapture();
            }
        }
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT && g_ctx.isHoveringClose) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    case WM_COPYDATA: {
        PCOPYDATASTRUCT pcds = reinterpret_cast<PCOPYDATASTRUCT>(lParam);
        if (pcds && pcds->dwData == 1) {
            LoadImageFromFile(static_cast<wchar_t*>(pcds->lpData));
            GetImagesInDirectory(static_cast<wchar_t*>(pcds->lpData));
        }
        return TRUE;
    }
    case WM_SIZE:
        FitImageToWindow();
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}