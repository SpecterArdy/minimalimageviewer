#include "viewer.h"
#include "vulkan_renderer.h"

extern AppContext g_ctx;

namespace {

// ── DPI helpers for overlay text ─────────────────────────────────────────────
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
    lf.lfHeight = MulDiv(lf.lfHeight, static_cast<int>(dpi), 96);
    lf.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    return CreateFontIndirectW(&lf);
}
    // Centralized zoom bounds
    constexpr float kMinZoom = 0.1f;
    constexpr float kMaxZoom = 8.0f;

    // Conservative maximum viewport dimension to keep scaled image within safe GPU limits
    // Many GPUs support >= 16384; use 8192 for extra safety.
    constexpr float kSafeMaxViewportDim = 8192.0f;

    // Keep the app state comfortably below the theoretical cap; render uses even more headroom.
    constexpr float kStateHeadroom  = 0.90f;
    constexpr float kRenderHeadroom = 0.85f;

    // Compute a dynamic zoom cap so that scaled image dimensions stay well within safe bounds.
    inline float ComputeDynamicZoomCap(uint32_t imageW, uint32_t imageH, bool rotated) {
        float w = static_cast<float>(imageW);
        float h = static_cast<float>(imageH);
        if (rotated) std::swap(w, h);

        float capByW = (w > 0.0f) ? (kSafeMaxViewportDim / w) : kMaxZoom;
        float capByH = (h > 0.0f) ? (kSafeMaxViewportDim / h) : kMaxZoom;
        float cap = std::min(capByW, capByH);

        // Bound by global limits
        if (cap > kMaxZoom) cap = kMaxZoom;
        if (cap < kMinZoom) cap = kMinZoom;
        return cap;
    }

    // RAII guard for SRWLOCK shared (reader) access
    struct SrwSharedGuard {
        SRWLOCK* lock;
        explicit SrwSharedGuard(SRWLOCK* l) : lock(l) { AcquireSRWLockShared(lock); }
        ~SrwSharedGuard() { ReleaseSRWLockShared(lock); }
        SrwSharedGuard(const SrwSharedGuard&) = delete;
        SrwSharedGuard& operator=(const SrwSharedGuard&) = delete;
    };

    // RAII guard to mark rendering as active
    struct RenderInProgressGuard {
        std::atomic<bool>& flag;
        explicit RenderInProgressGuard(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
        ~RenderInProgressGuard() { flag.store(false, std::memory_order_release); }
        RenderInProgressGuard(const RenderInProgressGuard&) = delete;
        RenderInProgressGuard& operator=(const RenderInProgressGuard&) = delete;
    };
}

void DrawImage(HDC hdc, const RECT& clientRect, const AppContext& ctx) {
    SrwSharedGuard guard(&g_ctx.renderLock);
    RenderInProgressGuard rip(g_ctx.renderInProgress);

    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
        return;
    }

    // When no image is loaded, draw a startup overlay with instructions and OCIO status.
    if (!g_ctx.imageData.isValid()) {
        if (hdc) {
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &clientRect, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(200, 200, 200));

            const wchar_t* title = L"Minimal Image Viewer";
            const wchar_t* info1 = L"Drag & drop an image here, or press Ctrl+O to open a file.";
            const wchar_t* info2 = g_ctx.ocioEnabled
                ? L"[OpenColorIO Info]: Color management enabled."
                : L"[OpenColorIO Info]: Color management disabled. (Set $OCIO to enable.)";
            const wchar_t* help  = L"Shortcuts: Ctrl+Wheel/+/– to zoom, Ctrl+0 to fit, Right-click for menu.";

            HFONT font = CreateMessageFontForDpi(g_ctx.hWnd);
            HFONT old = (HFONT)SelectObject(hdc, font);

            RECT r = clientRect;
            r.top += 40;
            DrawTextW(hdc, title, -1, &r, DT_CENTER | DT_TOP);
            r.top += 30;
            DrawTextW(hdc, info1, -1, &r, DT_CENTER | DT_TOP);
            r.top += 20;
            DrawTextW(hdc, info2, -1, &r, DT_CENTER | DT_TOP);
            r.top += 20;
            DrawTextW(hdc, help,  -1, &r, DT_CENTER | DT_TOP);

            SelectObject(hdc, old);
            DeleteObject(font);
        }
        return;
    }

    if (g_ctx.renderer && !g_ctx.rendererNeedsReset) {
        // Re-upload texture ONLY when image data changes. Zoom now affects rendering only.
        static uint32_t s_lastImageWidth = 0;
        static uint32_t s_lastImageHeight = 0;
        static float s_lastZoom = -1.0f;
        static bool s_lastIsHdr = false;

        const bool imageChanged = (ctx.imageData.width != s_lastImageWidth || 
                                  ctx.imageData.height != s_lastImageHeight ||
                                  ctx.imageData.isHdr != s_lastIsHdr);
        const float zoomDelta = (s_lastZoom < 0.0f) ? 1.0f : std::fabs(ctx.zoomFactor - s_lastZoom);
        const bool zoomChangedSignificantly = (zoomDelta >= 0.05f);

        if (imageChanged) {
            const void* pixelData = ctx.imageData.pixels.data();

            g_ctx.renderer->UpdateImageFromData(
                pixelData, ctx.imageData.width, ctx.imageData.height, ctx.imageData.isHdr);

            s_lastImageWidth = ctx.imageData.width;
            s_lastImageHeight = ctx.imageData.height;
            s_lastIsHdr = ctx.imageData.isHdr;
            s_lastZoom = ctx.zoomFactor;
        }

        // Compute dynamic cap for current orientation
        const bool rotated = (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270);
        const float dynCap = ComputeDynamicZoomCap(ctx.imageData.width, ctx.imageData.height, rotated);

        // Enforce a conservative state cap every frame so zoom-out always responds after hitting limits
        const float stateCap = dynCap * kStateHeadroom;
        if (g_ctx.zoomFactor > stateCap) g_ctx.zoomFactor = stateCap;
        if (g_ctx.zoomFactor < kMinZoom) g_ctx.zoomFactor = kMinZoom;

        // Render with extra headroom to avoid edge-trigger flicker
        const float renderCap = dynCap * kRenderHeadroom;
        float safeZoom = g_ctx.zoomFactor;
        if (safeZoom > renderCap) safeZoom = renderCap;
        if (safeZoom < kMinZoom)  safeZoom = kMinZoom;

        g_ctx.renderer->Render(static_cast<uint32_t>(clientWidth), static_cast<uint32_t>(clientHeight),
                               safeZoom, ctx.offsetX, ctx.offsetY, ctx.rotationAngle);

        // Check for non-throwing error states and defer reset to main loop
        if (g_ctx.renderer->IsDeviceLost() || g_ctx.renderer->IsSwapchainOutOfDate()) {
            g_ctx.rendererNeedsReset = true;
            s_lastImageWidth = 0;
            s_lastImageHeight = 0;
            s_lastZoom = -1.0f;
        }
    }
}

void FitImageToWindow() {
    if (!g_ctx.imageData.isValid()) return;

    RECT clientRect;
    GetClientRect(g_ctx.hWnd, &clientRect);
    if (IsRectEmpty(&clientRect)) return;

    float clientWidth = static_cast<float>(clientRect.right - clientRect.left);
    float clientHeight = static_cast<float>(clientRect.bottom - clientRect.top);
    float imageWidth = static_cast<float>(g_ctx.imageData.width);
    float imageHeight = static_cast<float>(g_ctx.imageData.height);

    if (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270) {
        std::swap(imageWidth, imageHeight);
    }

    if (imageWidth <= 0 || imageHeight <= 0) return;

    g_ctx.zoomFactor = std::min(clientWidth / imageWidth, clientHeight / imageHeight);

    // Enforce dynamic cap derived from image dimensions plus global bounds
    const bool rotated = (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270);
    const float dynCap = ComputeDynamicZoomCap(g_ctx.imageData.width, g_ctx.imageData.height, rotated);
    if (g_ctx.zoomFactor > dynCap) g_ctx.zoomFactor = dynCap;
    if (g_ctx.zoomFactor < kMinZoom) g_ctx.zoomFactor = kMinZoom;

    g_ctx.offsetX = 0.0f;
    g_ctx.offsetY = 0.0f;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void ZoomImage(float factor) {
    if (!g_ctx.imageData.isValid()) return;

    const bool rotated = (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270);
    const float dynCap = ComputeDynamicZoomCap(g_ctx.imageData.width, g_ctx.imageData.height, rotated);

    // Keep user-visible state comfortably below the theoretical cap.
    const float stateCap = dynCap * kStateHeadroom;

    float z = g_ctx.zoomFactor;

    if (factor > 1.0f) {
        // Zooming in: apply factor and clamp to the state cap
        z *= factor;
        if (z > stateCap) z = stateCap;
    } else {
        // Zooming out: if state overshot, start from the state cap so it responds immediately
        if (z > stateCap) z = stateCap;
        z *= factor;
        if (z < kMinZoom) z = kMinZoom;
    }

    g_ctx.zoomFactor = z;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void RotateImage(bool clockwise) {
    if (!g_ctx.imageData.isValid()) return;
    g_ctx.rotationAngle += clockwise ? 90 : -90;
    g_ctx.rotationAngle = (g_ctx.rotationAngle % 360 + 360) % 360;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

bool IsPointInImage(POINT pt, const RECT& clientRect) {
    if (!g_ctx.imageData.isValid()) return false;

    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);

    float windowCenterX = (cr.right - cr.left) / 2.0f;
    float windowCenterY = (cr.bottom - cr.top) / 2.0f;

    float translatedX = pt.x - (windowCenterX + g_ctx.offsetX);
    float translatedY = pt.y - (windowCenterY + g_ctx.offsetY);

    float scaledX = translatedX / g_ctx.zoomFactor;
    float scaledY = translatedY / g_ctx.zoomFactor;

    double rad = -g_ctx.rotationAngle * 3.1415926535 / 180.0;
    float cosTheta = static_cast<float>(cos(rad));
    float sinTheta = static_cast<float>(sin(rad));

    float unrotatedX = scaledX * cosTheta - scaledY * sinTheta;
    float unrotatedY = scaledX * sinTheta + scaledY * cosTheta;

    float localX = unrotatedX + g_ctx.imageData.width / 2.0f;
    float localY = unrotatedY + g_ctx.imageData.height / 2.0f;

    return localX >= 0 && localX < static_cast<float>(g_ctx.imageData.width) && 
           localY >= 0 && localY < static_cast<float>(g_ctx.imageData.height);
}