#pragma once

#include <cstdarg>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Logger {

// Initialize logging. Creates a per-run log file in LocalAppData\MinimalImageViewer\logs.
// Safe to call multiple times; subsequent calls are ignored.
bool Init(const wchar_t* appName = L"MinimalImageViewer");

// Install crash handlers (SEH, signals, terminate). Call after Init().
void InstallCrashHandlers();

// Flush and close the log file.
void Shutdown();

// Logging helpers (printf-style, UTF-8). Thread-safe; each call flushes.
void Info(const char* fmt, ...) noexcept;
void Warn(const char* fmt, ...) noexcept;
void Error(const char* fmt, ...) noexcept;

// Wide variants for convenience.
void InfoW(const wchar_t* fmt, ...) noexcept;
void WarnW(const wchar_t* fmt, ...) noexcept;
void ErrorW(const wchar_t* fmt, ...) noexcept;

} // namespace Logger
