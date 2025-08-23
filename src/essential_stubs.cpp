#include "viewer.h"
#include "logging.h"

extern AppContext g_ctx;

// Minimal stub implementations to prevent crashes during splash screen testing

void LoadImageFromFile(const wchar_t* filePath) {
    (void)filePath; // Unused parameter
    Logger::Info("LoadImageFromFile(wchar_t*) stub called");
}

void LoadImageFromFile(const char* filePath) {
    (void)filePath; // Unused parameter
    Logger::Info("LoadImageFromFile(char*) stub called");
}

void GetImagesInDirectory(const wchar_t* filePath) {
    (void)filePath; // Unused parameter
    Logger::Info("GetImagesInDirectory(wchar_t*) stub called");
}

void GetImagesInDirectory(const char* filePath) {
    (void)filePath; // Unused parameter
    Logger::Info("GetImagesInDirectory(char*) stub called");
}

void HandleKeyboardEvent(const SDL_KeyboardEvent& event) {
    (void)event; // Unused parameter
    Logger::Info("HandleKeyboardEvent stub called");
}

void HandleMouseEvent(const SDL_MouseButtonEvent& event) {
    (void)event; // Unused parameter
    Logger::Info("HandleMouseEvent stub called");
}

void HandleMouseMotion(const SDL_MouseMotionEvent& event) {
    (void)event; // Unused parameter
    Logger::Info("HandleMouseMotion stub called");
}

void HandleMouseWheel(const SDL_MouseWheelEvent& event) {
    (void)event; // Unused parameter
    Logger::Info("HandleMouseWheel stub called");
}
