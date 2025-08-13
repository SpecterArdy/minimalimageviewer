#include "viewer.h"
#include "vulkan_renderer.h"

extern AppContext g_ctx;

void DrawImage(HDC /*hdc*/, const RECT& clientRect, const AppContext& ctx) {
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) return;

    if (g_ctx.renderer) {
        // Upload current bitmap to Vulkan texture (no-op if null)
        if (ctx.hBitmap) {
            g_ctx.renderer->UpdateImageFromHBITMAP(ctx.hBitmap);
        }
        // Render with current zoom/offset (rotation to be handled in a future shader-based pipeline)
        g_ctx.renderer->Render(static_cast<uint32_t>(clientWidth), static_cast<uint32_t>(clientHeight),
                               ctx.zoomFactor, ctx.offsetX, ctx.offsetY);
    }
}

void FitImageToWindow() {
    if (!g_ctx.hBitmap) return;

    RECT clientRect;
    GetClientRect(g_ctx.hWnd, &clientRect);
    if (IsRectEmpty(&clientRect)) return;

    BITMAP bm;
    GetObject(g_ctx.hBitmap, sizeof(BITMAP), &bm);
    
    float clientWidth = static_cast<float>(clientRect.right - clientRect.left);
    float clientHeight = static_cast<float>(clientRect.bottom - clientRect.top);
    float imageWidth = static_cast<float>(bm.bmWidth);
    float imageHeight = static_cast<float>(bm.bmHeight);

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
    if (!g_ctx.hBitmap) return;
    g_ctx.zoomFactor *= factor;
    g_ctx.zoomFactor = std::max(0.01f, std::min(100.0f, g_ctx.zoomFactor));
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

void RotateImage(bool clockwise) {
    if (!g_ctx.hBitmap) return;
    g_ctx.rotationAngle += clockwise ? 90 : -90;
    g_ctx.rotationAngle = (g_ctx.rotationAngle % 360 + 360) % 360;
    InvalidateRect(g_ctx.hWnd, nullptr, FALSE);
}

bool IsPointInImage(POINT pt, const RECT& clientRect) {
    if (!g_ctx.hBitmap) return false;

    BITMAP bm;
    GetObject(g_ctx.hBitmap, sizeof(BITMAP), &bm);
    
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

    float localX = unrotatedX + bm.bmWidth / 2.0f;
    float localY = unrotatedY + bm.bmHeight / 2.0f;

    return localX >= 0 && localX < bm.bmWidth && localY >= 0 && localY < bm.bmHeight;
}