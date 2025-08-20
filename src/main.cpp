#include "viewer.h"
#include <winbase.h>
#include <cwchar>
#include <OpenColorIO/OpenColorIO.h>
#include "vulkan_renderer.h"

namespace OCIO = OCIO_NAMESPACE;

AppContext g_ctx;

void CenterImage(bool resetZoom) {
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
    HWND existingWnd = FindWindowW(L"MinimalImageViewer", nullptr);
    if (existingWnd) {
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
        return 0;
    }

    g_ctx.hInst = hInstance;

    if (FAILED(CoInitialize(nullptr))) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize OpenColorIO with proper fallback config
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

    // Initialize display device
    g_ctx.displayDevice = "sRGB";

    // Don't create display transform at startup - do it when needed
    g_ctx.currentDisplayTransform = nullptr;

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
        WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_ctx.hWnd) {
        MessageBoxW(nullptr, L"Failed to create window.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetWindowLongPtr(g_ctx.hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_ctx));

    // Initialize Vulkan renderer
    g_ctx.renderer = std::make_unique<VulkanRenderer>();
    if (!g_ctx.renderer->Initialize(g_ctx.hWnd)) {
        MessageBoxW(nullptr, L"Failed to initialize Vulkan renderer.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(g_ctx.hWnd, TRUE);

    ShowWindow(g_ctx.hWnd, nCmdShow);
    UpdateWindow(g_ctx.hWnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        LoadImageFromFile(argv[1]);
        GetImagesInDirectory(argv[1]);
    }
    if (argv) {
        LocalFree(argv);
    }

    // Initialize FPS timer baseline
    g_ctx.fpsLastTimeMS = GetTickCount64();

    // Non-blocking loop to drive continuous rendering and FPS updates
    MSG msg{};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
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
            if (g_ctx.renderer) {
                g_ctx.renderer->Shutdown();
                g_ctx.renderer.reset();
            }
            g_ctx.renderer = std::make_unique<VulkanRenderer>();
            if (!g_ctx.renderer->Initialize(g_ctx.hWnd)) {
                g_ctx.renderer.reset();
            }
            g_ctx.rendererNeedsReset = false;
        }

        // Small sleep to avoid busy-waiting
        Sleep(1);
    }
}