#include "viewer.h"
#include "vulkan_renderer.h"

extern AppContext g_ctx;

void DrawImage(HDC /*hdc*/, const RECT& clientRect, const AppContext& ctx) {
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
        return;
    }

    if (g_ctx.renderer && !g_ctx.rendererNeedsReset && g_ctx.imageData.isValid()) {
        // Re-upload texture when image data changes OR zoom changes significantly.
        static uint32_t s_lastImageWidth = 0;
        static uint32_t s_lastImageHeight = 0;
        static float s_lastZoom = -1.0f;
        static bool s_lastIsHdr = false;

        const bool imageChanged = (ctx.imageData.width != s_lastImageWidth || 
                                  ctx.imageData.height != s_lastImageHeight ||
                                  ctx.imageData.isHdr != s_lastIsHdr);
        const float zoomDelta = (s_lastZoom < 0.0f) ? 1.0f : std::fabs(ctx.zoomFactor - s_lastZoom);
        const bool zoomChangedSignificantly = (zoomDelta >= 0.05f);

        if (imageChanged || zoomChangedSignificantly) {
            // Use the unified pixels vector
            const void* pixelData = ctx.imageData.pixels.data();

            g_ctx.renderer->UpdateImageFromData(
                pixelData, ctx.imageData.width, ctx.imageData.height, ctx.imageData.isHdr);

            s_lastImageWidth = ctx.imageData.width;
            s_lastImageHeight = ctx.imageData.height;
            s_lastIsHdr = ctx.imageData.isHdr;
            s_lastZoom = ctx.zoomFactor;
        }

        // Render with current zoom/offset/rotation
        g_ctx.renderer->Render(static_cast<uint32_t>(clientWidth), static_cast<uint32_t>(clientHeight),
                               ctx.zoomFactor, ctx.offsetX, ctx.offsetY, ctx.rotationAngle);

        // Check for non-throwing error states and defer reset to main loop
        if (g_ctx.renderer->IsDeviceLost() || g_ctx.renderer->IsSwapchainOutOfDate()) {
            g_ctx.rendererNeedsReset = true;
            // Force reupload next time after reset
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
    g_ctx.offsetX = 0.0f;
    g_ctx.offsetY = 0.0f;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void ZoomImage(float factor) {
    if (!g_ctx.imageData.isValid()) return;
    g_ctx.zoomFactor *= factor;
    g_ctx.zoomFactor = std::max(0.01f, std::min(100.0f, g_ctx.zoomFactor));
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