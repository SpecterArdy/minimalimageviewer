#include "viewer.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cwchar>
#include <iostream>
#include <vector>
#include <string>
#include "ocio_shim.h"
#include "vulkan_renderer.h"
#include "logging.h"

static UINT GetDpiForHWND(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiWinFn = UINT (WINAPI *)(HWND);
        if (auto pGet = reinterpret_cast<GetDpiWinFn>(GetProcAddress(user32, "GetDpiForWindow"))) {
            return pGet(hwnd);
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
    ReleaseDC(hwnd, hdc);
    return dpi ? dpi : 96u;
}

static HFONT CreateMessageFontForDpi(HWND hwnd) {
    UINT dpi = GetDpiForHWND(hwnd);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    // Scale font height to window DPI; lfHeight is negative (character height)
    lf.lfHeight = MulDiv(lf.lfHeight, static_cast<int>(dpi), 96);
    lf.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    return CreateFontIndirectW(&lf);
}

// ───────────────────────────── Splash window helpers ─────────────────────────
static LRESULT CALLBACK SplashWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 220, 220));
            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT old = (HFONT)SelectObject(hdc, font);
            const wchar_t* title = L"Minimal Image Viewer";
            RECT r = rc;
            r.top += 20;
            DrawTextW(hdc, title, -1, &r, DT_CENTER | DT_TOP);
            SelectObject(hdc, old);
            EndPaint(hWnd, &ps);
            return 0;
        }
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

static HWND CreateSplashWindow(HINSTANCE hInstance) {
    const wchar_t* splashClass = L"MinimalImageViewerSplash";
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.hInstance = hInstance;
    wcex.lpfnWndProc = SplashWndProc;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = splashClass;
    RegisterClassExW(&wcex);

    // Centered small window
    int width = 560, height = 180;
    RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = (wa.right + wa.left - width) / 2;
    int y = (wa.bottom + wa.top - height) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, splashClass, L"Starting Minimal Image Viewer",
                                WS_POPUP,
                                x, y, width, height, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hWnd);
    // Keep it topmost without activating
    SetWindowPos(hWnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return hWnd;
}

static void DrawSplashMessage(HWND splash, const wchar_t* line1, const wchar_t* line2) {
    if (!splash) return;
    HDC hdc = GetDC(splash);
    RECT rc; GetClientRect(splash, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(220, 220, 220));
    HFONT font = CreateMessageFontForDpi(splash);
    HFONT old = (HFONT)SelectObject(hdc, font);

    RECT r = rc;
    r.top += 30;
    DrawTextW(hdc, line1, -1, &r, DT_CENTER | DT_TOP);
    r.top += 24;
    DrawTextW(hdc, line2, -1, &r, DT_CENTER | DT_TOP);

    SelectObject(hdc, old);
    DeleteObject(font);
    ReleaseDC(splash, hdc);
}

// Draw/update a determinate progress bar on the splash (percent in [0,100])
static void DrawSplashProgress(HWND splash, int percent, const wchar_t* stage) {
    if (!splash) return;

    HDC hdc = GetDC(splash);
    RECT rc; GetClientRect(splash, &rc);

    // Background
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Title + stage
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(220, 220, 220));
    HFONT font = CreateMessageFontForDpi(splash);
    HFONT old = (HFONT)SelectObject(hdc, font);

    RECT r = rc;
    r.top += 20;
    DrawTextW(hdc, L"Minimal Image Viewer", -1, &r, DT_CENTER | DT_TOP);
    r.top += 24;
    DrawTextW(hdc, stage ? stage : L"", -1, &r, DT_CENTER | DT_TOP);

    // Progress bar frame
    const int barWidth = rc.right - rc.left - 80;
    const int barHeight = 18;
    const int barX = rc.left + 40;
    const int barY = rc.bottom - 40;
    RECT frame{ barX, barY, barX + barWidth, barY + barHeight };
    HBRUSH frameBrush = CreateSolidBrush(RGB(80, 80, 80));
    FrameRect(hdc, &frame, frameBrush);
    DeleteObject(frameBrush);

    // Fill
    int fillWidth = (percent < 0 ? 0 : (percent > 100 ? 100 : percent)) * barWidth / 100;
    RECT fill{ barX + 1, barY + 1, barX + 1 + fillWidth, barY + barHeight - 1 };
    HBRUSH fillBrush = CreateSolidBrush(RGB(50, 150, 255));
    FillRect(hdc, &fill, fillBrush);
    DeleteObject(fillBrush);

    SelectObject(hdc, old);
    DeleteObject(font);
    ReleaseDC(splash, hdc);

    // Keep it topmost
    SetWindowPos(splash, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Pump pending messages so the splash repaints immediately
static void PumpSplashMessages(HWND splash) {
    MSG msg{};
    // Pump all messages (not just window-filtered) to ensure painting
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

namespace OCIO = OCIO_NAMESPACE;

AppContext g_ctx;

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
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // Enable per-monitor DPI awareness before any window is created
    EnableDpiAwareness();

    // Initialize logging and crash handlers as early as possible
    Logger::Init(L"MinimalImageViewer");
    Logger::InstallCrashHandlers();
    Logger::Info("Application starting (pid=%lu)", GetCurrentProcessId());
    
#ifdef HAVE_DATADOG
    // Start root application span
    auto appSpan = Logger::CreateSpan("application.startup");
    appSpan.set_tag("pid", std::to_string(GetCurrentProcessId()));
    if (lpCmdLine && *lpCmdLine) {
        // Convert to UTF-8 for tagging
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, lpCmdLine, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Size > 0) {
            std::vector<char> utf8Buf(utf8Size);
            WideCharToMultiByte(CP_UTF8, 0, lpCmdLine, -1, utf8Buf.data(), utf8Size, nullptr, nullptr);
            appSpan.set_tag("command_line", std::string(utf8Buf.data()));
        }
    }
#endif

    try {

    auto singleInstanceSpan = Logger::CreateChildSpan(appSpan, "check_single_instance");
    HWND existingWnd = FindWindowW(L"MinimalImageViewer", nullptr);
    if (existingWnd) {
        singleInstanceSpan.set_tag("existing_instance_found", "true");
        SetForegroundWindow(existingWnd);
        if (IsIconic(existingWnd)) {
            ShowWindow(existingWnd, SW_RESTORE);
        }
        if (lpCmdLine && *lpCmdLine) {
            COPYDATASTRUCT cds{};
            cds.dwData = 1;
            cds.cbData = (static_cast<DWORD>(wcslen(lpCmdLine)) + 1) * sizeof(wchar_t);
            cds.lpData = lpCmdLine;
            SendMessage(existingWnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hInstance), reinterpret_cast<LPARAM>(&cds));
        }
        // spans are finished automatically when they go out of scope
        return 0;
    }
    singleInstanceSpan.set_tag("existing_instance_found", "false");

    g_ctx.hInst = hInstance;

    auto comInitSpan = Logger::CreateChildSpan(appSpan, "com_initialize");
    if (FAILED(CoInitialize(nullptr))) {
        comInitSpan.set_tag("success", "false");
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    comInitSpan.set_tag("success", "true");

    // Initialize OpenColorIO with proper fallback config
    auto ocioInitSpan = Logger::CreateChildSpan(appSpan, "ocio_initialize");
    try {
        // Try to get current config
        g_ctx.ocioConfig = OCIO::GetCurrentConfig();

        // If we got a config, validate it has basic color spaces
        if (g_ctx.ocioConfig) {
            bool hasBasicSpaces = false;
            try {
                int numSpaces = g_ctx.ocioConfig->getNumColorSpaces();
                if (numSpaces > 0) {
                    hasBasicSpaces = true;
                }
            } catch (...) {
                hasBasicSpaces = false;
            }

            // If config is invalid, fall back to raw
            if (!hasBasicSpaces) {
                g_ctx.ocioConfig = OCIO::Config::CreateRaw();
            }
        }
    } catch (const OCIO::Exception& e) {
        // Fall back to raw config on any failure
        try {
            g_ctx.ocioConfig = OCIO::Config::CreateRaw();
        } catch (const OCIO::Exception& e2) {
            g_ctx.ocioConfig = nullptr;
        }
    } catch (...) {
        // Ultimate fallback
        try {
            g_ctx.ocioConfig = OCIO::Config::CreateRaw();
        } catch (...) {
            g_ctx.ocioConfig = nullptr;
        }
    }

    // Determine whether OCIO is enabled by environment
    bool envHasOCIO = false;
    {
        // Check Windows wide-character environment variable
        const wchar_t* ocioEnv = _wgetenv(L"OCIO");
        envHasOCIO = (ocioEnv && *ocioEnv);
    }
    g_ctx.ocioEnabled = envHasOCIO && static_cast<bool>(g_ctx.ocioConfig);
    
    ocioInitSpan.set_tag("enabled", g_ctx.ocioEnabled ? "true" : "false");
    ocioInitSpan.set_tag("env_has_ocio", envHasOCIO ? "true" : "false");
    ocioInitSpan.set_tag("has_config", static_cast<bool>(g_ctx.ocioConfig) ? "true" : "false");

    if (!g_ctx.ocioEnabled) {
        OutputDebugStringA("[OpenColorIO Info]: Color management disabled. (Specify the $OCIO environment variable to enable.)\n");
        Logger::Info("OpenColorIO: disabled (no $OCIO or no config)");
    } else {
        Logger::Info("OpenColorIO: enabled");
    }

    // Initialize display device
    g_ctx.displayDevice = "sRGB";

    // Don't create display transform at startup - do it when needed
    g_ctx.currentDisplayTransform = nullptr;

    // ── Startup splash (boot sequence) ──
    auto splashSpan = Logger::CreateChildSpan(appSpan, "splash_screen");
    HWND splash = CreateSplashWindow(hInstance);
    if (g_ctx.ocioEnabled) {
        DrawSplashMessage(splash, L"Starting Minimal Image Viewer...", L"OpenColorIO: enabled");
    } else {
        DrawSplashMessage(splash, L"Starting Minimal Image Viewer...", L"OpenColorIO: disabled (set $OCIO to enable)");
    }
    PumpSplashMessages(splash);
    // splash span finishes automatically

    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    wcex.lpszClassName = L"MinimalImageViewer";
    RegisterClassExW(&wcex);

    g_ctx.hWnd = CreateWindowW(
        wcex.lpszClassName,
        L"Minimal Image Viewer",
        WS_POPUP, // hidden until Vulkan is ready so the splash stays visible
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_ctx.hWnd) {
        MessageBoxW(nullptr, L"Failed to create window.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetWindowLongPtr(g_ctx.hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_ctx));

    // Initialize Vulkan renderer (accurate progress on splash)
    auto vulkanInitSpan = Logger::CreateChildSpan(appSpan, "vulkan_initialize");
    DrawSplashProgress(splash, 0, L"Preparing to initialize Vulkan...");
    PumpSplashMessages(splash);

    static HWND s_splash = nullptr;
    s_splash = splash;
    auto progressCb = [](int pct, const wchar_t* stage) {
        DrawSplashProgress(s_splash, pct, stage);
        PumpSplashMessages(s_splash);
        Logger::Info("Vulkan init: %d%% - %s", pct, stage ? "stage" : "");
        if (stage) {
            Logger::InfoW(L"Vulkan stage: %s", stage);
        }
    };

    g_ctx.renderer = std::make_unique<VulkanRenderer>();
    if (!g_ctx.renderer->InitializeWithProgress(g_ctx.hWnd, progressCb)) {
        if (splash) DestroyWindow(splash), splash = nullptr;
        vulkanInitSpan.set_tag("success", "false");
        Logger::Error("Failed to initialize Vulkan renderer");
        MessageBoxW(nullptr, L"Failed to initialize Vulkan renderer.", L"Error", MB_OK | MB_ICONERROR);
        Logger::Shutdown();
        return 1;
    }
    vulkanInitSpan.set_tag("success", "true");

    // Close splash and show the main window only after Vulkan is ready
    if (splash) { DestroyWindow(splash); splash = nullptr; }

    DragAcceptFiles(g_ctx.hWnd, TRUE);

    ShowWindow(g_ctx.hWnd, nCmdShow);
    UpdateWindow(g_ctx.hWnd);

    auto cmdLineSpan = Logger::CreateChildSpan(appSpan, "process_command_line");
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        cmdLineSpan.set_tag("has_file_argument", "true");
        LoadImageFromFile(argv[1]);
        GetImagesInDirectory(argv[1]);
    } else {
        cmdLineSpan.set_tag("has_file_argument", "false");
    }
    if (argv) {
        LocalFree(argv);
    }
    // spans finish automatically when they go out of scope

    // Initialize FPS timer baseline
    g_ctx.fpsLastTimeMS = GetTickCount64();

    // Non-blocking loop to drive continuous rendering and FPS updates
    MSG msg{};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                auto shutdownSpan = Logger::CreateSpan("application.shutdown");
                Logger::Info("WM_QUIT received, shutting down");
                Logger::Shutdown();
                CoUninitialize();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Request a repaint to drive rendering
        InvalidateRect(g_ctx.hWnd, nullptr, FALSE);

        // FPS accounting
        ++g_ctx.fpsFrameCount;
        unsigned long long now = GetTickCount64();
        unsigned long long elapsed = now - g_ctx.fpsLastTimeMS;
        if (elapsed >= 1000ULL) {
            g_ctx.fps = static_cast<float>(g_ctx.fpsFrameCount) * 1000.0f / static_cast<float>(elapsed);
            g_ctx.fpsFrameCount = 0;
            g_ctx.fpsLastTimeMS = now;

            if (g_ctx.showFps) {
                wchar_t title[256];
                swprintf(title, 256, L"Minimal Image Viewer - %.1f FPS", g_ctx.fps);
                SetWindowTextW(g_ctx.hWnd, title);
            }
        }

        // Handle deferred renderer reset outside paint/draw for safety
        if (g_ctx.rendererNeedsReset) {
            auto resetSpan = Logger::CreateSpan("renderer.reset");
            // Ensure no frame is currently issuing Vulkan work
            while (g_ctx.renderInProgress.load(std::memory_order_acquire)) {
                Sleep(0); // yield until current render completes
            }

            // Exclusive lock ensures no new rendering uses stale Vulkan handles during recovery
            AcquireSRWLockExclusive(&g_ctx.renderLock);

            const bool deviceLost = (g_ctx.renderer && g_ctx.renderer->IsDeviceLost());
            resetSpan.set_tag("device_lost", deviceLost ? "true" : "false");
            if (g_ctx.renderer && deviceLost) {
                resetSpan.set_tag("reset_type", "full_rebuild");
                Logger::Warn("Reset: device lost detected — performing full renderer rebuild");
                // Full teardown and reinit
                g_ctx.renderer->Shutdown();
                g_ctx.renderer.reset();
                g_ctx.renderer = std::make_unique<VulkanRenderer>();
                if (!g_ctx.renderer->Initialize(g_ctx.hWnd)) {
                    resetSpan.set_tag("success", "false");
                    Logger::Error("Reset: VulkanRenderer re-initialization FAILED after device lost");
                    g_ctx.renderer.reset();
                } else {
                    resetSpan.set_tag("success", "true");
                    Logger::Info("Reset: VulkanRenderer re-initialized after device lost");
                }
            } else if (g_ctx.renderer) {
                // Swapchain-only path (e.g., out-of-date)
                RECT cr{};
                GetClientRect(g_ctx.hWnd, &cr);
                uint32_t w = static_cast<uint32_t>(std::max<LONG>(1, cr.right - cr.left));
                uint32_t h = static_cast<uint32_t>(std::max<LONG>(1, cr.bottom - cr.top));
                resetSpan.set_tag("reset_type", "swapchain_only");
                resetSpan.set_tag("width", std::to_string(w));
                resetSpan.set_tag("height", std::to_string(h));
                Logger::Warn("Reset: swapchain recreation (w={}, h={})", w, h);
                g_ctx.renderer->Resize(w, h);
                g_ctx.renderer->ClearErrorFlags();
                resetSpan.set_tag("success", "true");
                Logger::Info("Reset: swapchain recreated");
            }

            g_ctx.rendererNeedsReset = false;
            ReleaseSRWLockExclusive(&g_ctx.renderLock);
        }

        // Small sleep to avoid busy-waiting
        Sleep(1);
    }
    } catch (const std::exception& e) {
        Logger::Error("Unhandled std::exception: %s", e.what());
        Logger::LogStackTrace();
        Logger::DumpNow("Unhandled std::exception");
        Logger::Shutdown();
        return 1;
    } catch (...) {
        Logger::Error("Unhandled unknown exception");
        Logger::LogStackTrace();
        Logger::DumpNow("Unhandled unknown exception");
        Logger::Shutdown();
        return 1;
    }
}