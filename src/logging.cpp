#include "logging.h"

#include <string>
#include <atomic>
#include <cstdarg>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <glog/logging.h>

#ifdef HAVE_BREAKPAD
// Breakpad for Windows
#include <client/windows/handler/exception_handler.h>
#endif

#ifdef HAVE_LIBUNWIND
#include <libunwind.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace {
    struct LogState {
        std::atomic<bool> initialized{false};
        std::wstring dir;        // logs directory
        std::wstring spdFile;    // spdlog rotating file path
        std::wstring dumpDir;    // Breakpad dump directory
        std::string glogPrefix;  // utf-8 prefix for glog destinations
#ifdef HAVE_BREAKPAD
        std::unique_ptr<google_breakpad::ExceptionHandler> eh;
#endif
    };
    LogState& S() { static LogState s; return s; }

    static std::string w2u(const std::wstring& w) {
#ifdef _WIN32
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
#else
        return {};
#endif
    }

#ifdef _WIN32
    static bool CreateTree(const std::wstring& path) {
        // SHCreateDirectoryExW returns ERROR_SUCCESS (0) or ERROR_ALREADY_EXISTS if present
        int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) return true;
        return false;
    }

    static std::wstring buildLogsDir(const wchar_t* appName) {
        const std::wstring app = (appName && *appName) ? appName : L"MinimalImageViewer";
        std::wstring candidates[3];

        // Candidate 1: LocalAppData
        {
            PWSTR appData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
                candidates[0] = std::wstring(appData) + L"\\" + app + L"\\logs";
                CoTaskMemFree(appData);
            }
        }
        // Candidate 2: %TEMP%
        {
            wchar_t tempPath[MAX_PATH]{};
            DWORD n = GetTempPathW(MAX_PATH, tempPath);
            if (n > 0) {
                candidates[1] = std::wstring(tempPath) + app + L"\\logs";
            }
        }
        // Candidate 3: local .\logs
        candidates[2] = L".\\logs";

        for (const auto& cand : candidates) {
            if (cand.empty()) continue;
            if (CreateTree(cand)) {
                OutputDebugStringW((L"[LogInit] Using logs dir: " + cand + L"\n").c_str());
                return cand;
            } else {
                OutputDebugStringW((L"[LogInit] Failed to create dir: " + cand + L"\n").c_str());
            }
        }
        // Last resort
        OutputDebugStringW(L"[LogInit] Falling back to current directory.\n");
        return L".";
    }
#endif

    // printf-style format to std::string for spdlog
    static std::string vformat(const char* fmt, va_list ap) {
        if (!fmt) return {};
        char buf[1024];
        va_list ap2; va_copy(ap2, ap);
        int needed = vsnprintf(buf, sizeof(buf), fmt, ap2);
        va_end(ap2);
        if (needed >= 0 && needed < (int)sizeof(buf)) return std::string(buf, (size_t)needed);
        int size = (needed > 0 ? needed + 1 : 4096);
        std::string big(size, '\0');
        vsnprintf(big.data(), big.size(), fmt, ap);
        big.resize(strlen(big.c_str()));
        return big;
    }

#ifdef HAVE_LIBUNWIND
    static void LogBacktrace() {
        unw_context_t ctx;
        unw_cursor_t cur;
        if (unw_getcontext(&ctx) != 0) return;
        if (unw_init_local(&cur, &ctx) != 0) return;
        spdlog::error("Backtrace (libunwind):");
        int frame = 0;
        while (unw_step(&cur) > 0 && frame < 128) {
            unw_word_t ip = 0, off = 0;
            char name[256] = {0};
            unw_get_reg(&cur, UNW_REG_IP, &ip);
            if (unw_get_proc_name(&cur, name, sizeof(name), &off) == 0) {
                spdlog::error("  #{:02d} 0x{:016x} {}+0x{:x}", frame, (unsigned long long)ip, name, (unsigned)off);
            } else {
                spdlog::error("  #{:02d} 0x{:016x}", frame, (unsigned long long)ip);
            }
            ++frame;
        }
    }
#else
    static void LogBacktrace() {}
#endif
}

namespace Logger {

bool Init(const wchar_t* appName) {
    if (S().initialized.load(std::memory_order_acquire)) return true;

#ifdef _WIN32
    S().dir = buildLogsDir(appName);
    S().spdFile = S().dir + L"\\app.log";
    S().dumpDir = S().dir + L"\\crashdumps";
    SHCreateDirectoryExW(nullptr, S().dumpDir.c_str(), nullptr); // create full tree
    S().glogPrefix = w2u(S().dir + L"\\glog_");
#else
    S().dir = L".";
    S().spdFile = L"./app.log";
    S().dumpDir = L"./crashdumps";
    S().glogPrefix = "./glog_";
#endif

    // Initialize spdlog async rotating logger
    try {
        spdlog::init_thread_pool(8192, 1);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            w2u(S().spdFile).c_str(), 10 * 1024 * 1024, 5, true);
        auto logger = std::make_shared<spdlog::async_logger>(
            "minimalimageviewer", sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    } catch (...) {
        return false;
    }

    // Initialize glog to capture failure stack traces into the same logs dir
    google::InitGoogleLogging("minimalimageviewer");
    google::SetStderrLogging(google::GLOG_ERROR);
    google::SetLogDestination(google::GLOG_INFO,  (S().glogPrefix + "INFO_").c_str());
    google::SetLogDestination(google::GLOG_WARNING,(S().glogPrefix + "WARN_").c_str());
    google::SetLogDestination(google::GLOG_ERROR,  (S().glogPrefix + "ERROR_").c_str());
    google::InstallFailureSignalHandler();

    spdlog::info("Logger initialized. logs dir={}, spdlog file={}, dumps dir={}, glog prefix={}",
                 w2u(S().dir), w2u(S().spdFile), w2u(S().dumpDir), S().glogPrefix);
    S().initialized.store(true, std::memory_order_release);
    return true;
}

void InstallCrashHandlers() {
#ifdef HAVE_BREAKPAD
    // Setup Breakpad to write minidumps into the crashdumps directory.
    auto dumpCallback = [](const wchar_t* dump_path, const wchar_t* minidump_id,
                           void*, EXCEPTION_POINTERS*, MDRawAssertionInfo*, bool succeeded) -> bool {
        std::wstring path(dump_path ? dump_path : L".");
        std::wstring id(minidump_id ? minidump_id : L"");
        spdlog::error("Breakpad minidump: {}\\{}.dmp (ok={})",
                      std::string(path.begin(), path.end()),
                      std::string(id.begin(), id.end()),
                      succeeded ? "true" : "false");
        return succeeded;
    };
    try {
        S().eh = std::make_unique<google_breakpad::ExceptionHandler>(
            S().dumpDir,
            /*filter*/ nullptr,
            /*callback*/ dumpCallback,
            /*context*/ nullptr,
            google_breakpad::ExceptionHandler::HANDLER_ALL);
        spdlog::info("Breakpad installed; dumps -> {}", w2u(S().dumpDir));
    } catch (...) {
        spdlog::warn("Failed to install Breakpad handler");
    }
#endif

    // glog already installed a failure signal handler; add std::terminate hook with libunwind backtrace
    std::set_terminate([] {
        spdlog::error("std::terminate called");
        LogBacktrace();
        google::LogMessage(__FILE__, __LINE__, google::GLOG_FATAL).stream()
            << "std::terminate called";
        std::abort();
    });
    spdlog::info("Crash handlers installed (glog failure handler active)");
}

void Shutdown() {
    if (!S().initialized.load(std::memory_order_acquire)) return;
    spdlog::shutdown();
    google::ShutdownGoogleLogging();
    S().initialized.store(false, std::memory_order_release);
}

void Info(const char* fmt, ...) noexcept {
    va_list ap; va_start(ap, fmt);
    auto msg = vformat(fmt, ap);
    va_end(ap);
    spdlog::info("{}", msg);
}
void Warn(const char* fmt, ...) noexcept {
    va_list ap; va_start(ap, fmt);
    auto msg = vformat(fmt, ap);
    va_end(ap);
    spdlog::warn("{}", msg);
}
void Error(const char* fmt, ...) noexcept {
    va_list ap; va_start(ap, fmt);
    auto msg = vformat(fmt, ap);
    va_end(ap);
    spdlog::error("{}", msg);
}

void InfoW(const wchar_t* fmt, ...) noexcept {
#ifdef _WIN32
    if (!fmt) { spdlog::info(""); return; }
    wchar_t stack[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(stack, 1024, fmt, ap);
    va_end(ap);
    std::wstring wmsg;
    if (n >= 0 && n < 1024) wmsg.assign(stack, stack + n);
    else {
        std::wstring tmp(4096, L'\0');
        va_start(ap, fmt);
        _vsnwprintf(tmp.data(), tmp.size(), fmt, ap);
        va_end(ap);
        wmsg = tmp.c_str();
    }
    spdlog::info("{}", w2u(wmsg));
#else
    (void)fmt;
#endif
}
void WarnW(const wchar_t* fmt, ...) noexcept {
#ifdef _WIN32
    if (!fmt) { spdlog::warn(""); return; }
    wchar_t stack[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(stack, 1024, fmt, ap);
    va_end(ap);
    std::wstring wmsg;
    if (n >= 0 && n < 1024) wmsg.assign(stack, stack + n);
    else {
        std::wstring tmp(4096, L'\0');
        va_start(ap, fmt);
        _vsnwprintf(tmp.data(), tmp.size(), fmt, ap);
        va_end(ap);
        wmsg = tmp.c_str();
    }
    spdlog::warn("{}", w2u(wmsg));
#else
    (void)fmt;
#endif
}
void ErrorW(const wchar_t* fmt, ...) noexcept {
#ifdef _WIN32
    if (!fmt) { spdlog::error(""); return; }
    wchar_t stack[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(stack, 1024, fmt, ap);
    va_end(ap);
    std::wstring wmsg;
    if (n >= 0 && n < 1024) wmsg.assign(stack, stack + n);
    else {
        std::wstring tmp(4096, L'\0');
        va_start(ap, fmt);
        _vsnwprintf(tmp.data(), tmp.size(), fmt, ap);
        va_end(ap);
        wmsg = tmp.c_str();
    }
    spdlog::error("{}", w2u(wmsg));
#else
    (void)fmt;
#endif
}

} // namespace Logger
