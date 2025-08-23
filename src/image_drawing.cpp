#include "viewer.h"
#include "vulkan_renderer.h"
#include "logging.h"

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
    constexpr float kMaxZoom = 6.0f; // Reduced from 8.0f to avoid numerical issues

    // Conservative maximum viewport dimension to keep scaled image within safe GPU limits
    // Many GPUs support >= 16384; use 8192 for extra safety.
    constexpr float kSafeMaxViewportDim = 8192.0f;

    // Keep the app state comfortably below the theoretical cap; render uses even more headroom.
    constexpr float kStateHeadroom  = 0.90f;
    constexpr float kRenderHeadroom = 0.85f;

    // Compute a dynamic zoom cap so that scaled image dimensions stay well within safe bounds.
    inline float ComputeDynamicZoomCap(uint32_t imageW, uint32_t imageH, bool rotated) {
        // Protect against zero or extremely large dimensions
        if (imageW == 0 || imageH == 0 || imageW > 65536 || imageH > 65536) {
            return 1.0f; // Safe default
        }
        
        float w = static_cast<float>(imageW);
        float h = static_cast<float>(imageH);
        if (rotated) std::swap(w, h);
        
        // Prevent division by zero or extremely small values
        if (w < 1.0f) w = 1.0f;
        if (h < 1.0f) h = 1.0f;
        
        float capByW = kSafeMaxViewportDim / w;
        float capByH = kSafeMaxViewportDim / h;
        
        // Apply additional buffer for numerical stability
        capByW *= 0.95f;
        capByH *= 0.95f;
        
        float cap = std::min(capByW, capByH);
        
        // Bound by global limits with additional safety margin
        cap = std::min(cap, kMaxZoom);
        cap = std::max(cap, kMinZoom);
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
#ifdef HAVE_DATADOG
    auto drawSpan = Logger::CreateSpan("image.draw");
    drawSpan.set_tag("client_width", std::to_string(clientRect.right - clientRect.left));
    drawSpan.set_tag("client_height", std::to_string(clientRect.bottom - clientRect.top));
    drawSpan.set_tag("zoom_factor", std::to_string(g_ctx.zoomFactor));
    drawSpan.set_tag("rotation_angle", std::to_string(g_ctx.rotationAngle));
    drawSpan.set_tag("offset_x", std::to_string(g_ctx.offsetX));
    drawSpan.set_tag("offset_y", std::to_string(g_ctx.offsetY));
#endif
    
    SrwSharedGuard guard(&g_ctx.renderLock);
    RenderInProgressGuard rip(g_ctx.renderInProgress);

    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
#ifdef HAVE_DATADOG
        drawSpan.set_tag("skipped_reason", "invalid_client_dimensions");
#endif
        return;
    }
    
    // Critical safety check: ensure zoom factor is always valid before any operations
    if (g_ctx.zoomFactor <= 0.0f || !std::isfinite(g_ctx.zoomFactor)) {
        g_ctx.zoomFactor = 1.0f; // Reset to safe default
        Logger::Warn("Invalid zoom factor detected and reset to 1.0f");
    }

    // When no image is loaded, draw a startup overlay with instructions and OCIO status.
    if (!g_ctx.imageData.isValid()) {
#ifdef HAVE_DATADOG
        drawSpan.set_tag("no_image", "true");
        drawSpan.set_tag("ocio_enabled", g_ctx.ocioEnabled ? "true" : "false");
#endif
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
#ifdef HAVE_DATADOG
        auto renderSpan = Logger::CreateChildSpan(drawSpan, "vulkan.render");
        renderSpan.set_tag("image_width", std::to_string(ctx.imageData.width));
        renderSpan.set_tag("image_height", std::to_string(ctx.imageData.height));
        renderSpan.set_tag("is_hdr", ctx.imageData.isHdr ? "true" : "false");
#endif
        
        // Log current view parameters at the start of a draw
        Logger::Info("Draw: client=%dx%d zoom=%.3f offset=(%.2f,%.2f) rot=%d",
                     clientWidth, clientHeight, g_ctx.zoomFactor, g_ctx.offsetX, g_ctx.offsetY, g_ctx.rotationAngle);
        // If the device is already lost, do not issue any Vulkan commands this frame.
        if (g_ctx.renderer->IsDeviceLost()) {
            Logger::Warn("Render skipped: device lost flagged — scheduling reset");
#ifdef HAVE_DATADOG
            renderSpan.set_tag("skipped_reason", "device_lost");
            drawSpan.set_tag("skipped_reason", "device_lost");
#endif
            g_ctx.rendererNeedsReset = true;
            return;
        }

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
        
        // NASA Standard: Validate and bound offset values to prevent renderer overflow
        constexpr float kMaxSafeOffset = 1000000.0f;
        if (!std::isfinite(g_ctx.offsetX) || !std::isfinite(g_ctx.offsetY) ||
            std::abs(g_ctx.offsetX) > kMaxSafeOffset || std::abs(g_ctx.offsetY) > kMaxSafeOffset) {
            // Offsets are extreme or invalid - reset to center to prevent crash
            Logger::Warn("Extreme offset values detected (%.2f, %.2f) - resetting to center", 
                        g_ctx.offsetX, g_ctx.offsetY);
            g_ctx.offsetX = 0.0f;
            g_ctx.offsetY = 0.0f;
        }

        // Render with extra headroom to avoid edge-trigger flicker
        const float renderCap = dynCap * kRenderHeadroom;
        float safeZoom = g_ctx.zoomFactor;
        if (safeZoom > renderCap) safeZoom = renderCap;
        if (safeZoom < kMinZoom)  safeZoom = kMinZoom;

        // Log critical state before potentially dangerous Vulkan renderer call
        Logger::LogCriticalState(safeZoom, ctx.offsetX, ctx.offsetY, "before_vulkan_render");
        
        g_ctx.renderer->Render(static_cast<uint32_t>(clientWidth), static_cast<uint32_t>(clientHeight),
                               safeZoom, ctx.offsetX, ctx.offsetY, ctx.rotationAngle);

        // Check for non-throwing error states and defer reset to main loop
        if (g_ctx.renderer->IsDeviceLost() || g_ctx.renderer->IsSwapchainOutOfDate()) {
            Logger::Warn("Renderer signaled reset: deviceLost=%d swapchainOutOfDate=%d",
                         g_ctx.renderer->IsDeviceLost() ? 1 : 0,
                         g_ctx.renderer->IsSwapchainOutOfDate() ? 1 : 0);
#ifdef HAVE_DATADOG
            renderSpan.set_tag("needs_reset", "true");
            renderSpan.set_tag("device_lost", g_ctx.renderer->IsDeviceLost() ? "true" : "false");
            renderSpan.set_tag("swapchain_out_of_date", g_ctx.renderer->IsSwapchainOutOfDate() ? "true" : "false");
#endif
            g_ctx.rendererNeedsReset = true;
            s_lastImageWidth = 0;
            s_lastImageHeight = 0;
            s_lastZoom = -1.0f;
        } else {
#ifdef HAVE_DATADOG
            renderSpan.set_tag("needs_reset", "false");
#endif
        }
    }
}

void FitImageToWindow() {
#ifdef HAVE_DATADOG
    auto fitSpan = Logger::CreateSpan("image.fit_to_window");
#endif
    
    if (!g_ctx.imageData.isValid()) {
#ifdef HAVE_DATADOG
        fitSpan.set_tag("success", "false");
        fitSpan.set_tag("error", "no_image");
#endif
        return;
    }

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
    
#ifdef HAVE_DATADOG
    fitSpan.set_tag("success", "true");
    fitSpan.set_tag("calculated_zoom", std::to_string(g_ctx.zoomFactor));
    fitSpan.set_tag("image_width", std::to_string(imageWidth));
    fitSpan.set_tag("image_height", std::to_string(imageHeight));
    fitSpan.set_tag("client_width", std::to_string(clientWidth));
    fitSpan.set_tag("client_height", std::to_string(clientHeight));
#endif
    
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void ZoomImage(float factor) {
#ifdef HAVE_DATADOG
    auto zoomSpan = Logger::CreateSpan("image.zoom");
    zoomSpan.set_tag("zoom_factor", std::to_string(factor));
    zoomSpan.set_tag("current_zoom", std::to_string(g_ctx.zoomFactor));
#endif
    
    if (!g_ctx.imageData.isValid()) {
#ifdef HAVE_DATADOG
        zoomSpan.set_tag("success", "false");
        zoomSpan.set_tag("error", "no_image");
#endif
        return;
    }
    
    // Validate input factor for numerical stability
    if (factor <= 0.0f || !std::isfinite(factor)) {
        Logger::Warn("Invalid zoom factor: %.3f - ignoring", factor);
#ifdef HAVE_DATADOG
        zoomSpan.set_tag("success", "false");
        zoomSpan.set_tag("error", "invalid_factor");
#endif
        return;
    }
    
    // Limit extreme zoom factors to prevent numerical issues
    factor = std::clamp(factor, 0.1f, 2.0f);
    
    Logger::Info("Zoom request: factor=%.3f currentZoom=%.3f", factor, g_ctx.zoomFactor);

    const bool rotated = (g_ctx.rotationAngle == 90 || g_ctx.rotationAngle == 270);
    const float dynCap = ComputeDynamicZoomCap(g_ctx.imageData.width, g_ctx.imageData.height, rotated);

    // Keep user-visible state comfortably below the theoretical cap.
    const float stateCap = dynCap * kStateHeadroom;

    float z = g_ctx.zoomFactor;
    
    // First apply safe bounds to current zoom to recover from any potentially unsafe state
    z = std::clamp(z, kMinZoom, stateCap);

    if (factor > 1.0f) {
        // Zooming in: apply factor and clamp to the state cap
        // Check if multiplication would exceed safe bounds
        if (z > (stateCap / factor)) {
            z = stateCap; // Avoid overflow by capping directly
        } else {
            z *= factor;
        }
    } else {
        // Zooming out: start from current state or cap if beyond
        if (z > stateCap) z = stateCap;
        z *= factor;
    }
    
    // Final safety clamp to ensure we're within valid bounds
    z = std::clamp(z, kMinZoom, stateCap);

    g_ctx.zoomFactor = z;
    
#ifdef HAVE_DATADOG
    zoomSpan.set_tag("success", "true");
    zoomSpan.set_tag("new_zoom", std::to_string(g_ctx.zoomFactor));
    zoomSpan.set_tag("zoom_direction", factor > 1.0f ? "in" : "out");
#endif
    
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void RotateImage(bool clockwise) {
#ifdef HAVE_DATADOG
    auto rotateSpan = Logger::CreateSpan("image.rotate");
    rotateSpan.set_tag("clockwise", clockwise ? "true" : "false");
    rotateSpan.set_tag("current_angle", std::to_string(g_ctx.rotationAngle));
#endif
    
    if (!g_ctx.imageData.isValid()) {
#ifdef HAVE_DATADOG
        rotateSpan.set_tag("success", "false");
        rotateSpan.set_tag("error", "no_image");
#endif
        return;
    }
    
    g_ctx.rotationAngle += clockwise ? 90 : -90;
    g_ctx.rotationAngle = (g_ctx.rotationAngle % 360 + 360) % 360;
    
#ifdef HAVE_DATADOG
    rotateSpan.set_tag("success", "true");
    rotateSpan.set_tag("new_angle", std::to_string(g_ctx.rotationAngle));
#endif
    
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

bool IsPointInImage(POINT pt, const RECT& clientRect) {
    if (!g_ctx.imageData.isValid()) return false;

    // Safety check for zoom factor to prevent division by zero
    if (g_ctx.zoomFactor <= 0.0f || !std::isfinite(g_ctx.zoomFactor)) {
        return false;
    }

    RECT cr;
    GetClientRect(g_ctx.hWnd, &cr);

    float windowCenterX = (cr.right - cr.left) / 2.0f;
    float windowCenterY = (cr.bottom - cr.top) / 2.0f;

    float translatedX = pt.x - (windowCenterX + g_ctx.offsetX);
    float translatedY = pt.y - (windowCenterY + g_ctx.offsetY);

    // Safe division with additional bounds checking
    float scaledX = translatedX / g_ctx.zoomFactor;
    float scaledY = translatedY / g_ctx.zoomFactor;
    
    // Validate scaled coordinates are finite
    if (!std::isfinite(scaledX) || !std::isfinite(scaledY)) {
        return false;
    }

    double rad = -g_ctx.rotationAngle * 3.1415926535 / 180.0;
    float cosTheta = static_cast<float>(cos(rad));
    float sinTheta = static_cast<float>(sin(rad));

    float unrotatedX = scaledX * cosTheta - scaledY * sinTheta;
    float unrotatedY = scaledX * sinTheta + scaledY * cosTheta;
    
    // Validate unrotated coordinates are finite
    if (!std::isfinite(unrotatedX) || !std::isfinite(unrotatedY)) {
        return false;
    }

    float localX = unrotatedX + g_ctx.imageData.width / 2.0f;
    float localY = unrotatedY + g_ctx.imageData.height / 2.0f;
    
    // Final validation of local coordinates
    if (!std::isfinite(localX) || !std::isfinite(localY)) {
        return false;
    }

    return localX >= 0 && localX < static_cast<float>(g_ctx.imageData.width) && 
           localY >= 0 && localY < static_cast<float>(g_ctx.imageData.height);
}
