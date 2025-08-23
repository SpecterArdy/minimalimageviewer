#pragma once

#include <cmath>

#ifdef HAVE_DATADOG
#include <datadog/span.h>
#include <datadog/tracer.h>
#include <memory>
namespace dd = datadog::tracing;
#endif
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Logger {

// Initialize logging. Creates a per-run log file in LocalAppData\MinimalImageViewer\logs.
// Safe to call multiple times; subsequent calls are ignored.
bool Init(const wchar_t* appName = L"MinimalImageViewer");

// Install crash handlers (SEH + glog failure + terminate). Call after Init().
void InstallCrashHandlers();

// Explicitly log a symbolized stack trace (best-effort on Windows).
void LogStackTrace() noexcept;

// Force writing a minidump immediately (best-effort). 'reason' goes into the log.
void DumpNow(const char* reason = nullptr) noexcept;

// Log critical application state for crash analysis
void LogCriticalState(float zoomFactor, float offsetX, float offsetY, const char* context = nullptr) noexcept;

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

#ifdef HAVE_DATADOG
// Datadog tracing helpers
dd::Span CreateSpan(const char* name) noexcept;
dd::Span CreateChildSpan(const dd::Span& parent, const char* name) noexcept;
#endif

} // namespace Logger
