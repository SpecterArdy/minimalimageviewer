#include "logging.h"

#include <string>
#include <atomic>
#include <cstdarg>
#include <thread>
#include <vector>
#include <algorithm>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <glog/logging.h>

#ifdef HAVE_BREAKPAD
// Breakpad for Windows
#include <client/windows/handler/exception_handler.h>
#endif

#ifdef HAVE_DATADOG
// Datadog tracing
#include <datadog/span_config.h>
#include <datadog/tracer.h>
#include <datadog/tracer_config.h>
namespace dd = datadog::tracing;
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <wininet.h>
#include <psapi.h>
#include <crtdbg.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace {
    struct LogState {
        std::atomic<bool> initialized{false};
        std::wstring dir;        // logs directory
        std::wstring spdFile;    // spdlog rotating file path
        std::wstring dumpDir;    // crash-dumps directory
        std::string glogPrefix;  // utf-8 prefix for glog destinations
#ifdef HAVE_BREAKPAD
        std::unique_ptr<google_breakpad::ExceptionHandler> eh;
#endif
#ifdef HAVE_DATADOG
        std::unique_ptr<dd::Tracer> tracer;
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

#ifdef _WIN32
    static void EnsureDbgHelp() {
        static std::atomic<bool> inited{false};
        if (!inited.load(std::memory_order_acquire)) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
                inited.store(true, std::memory_order_release);
            }
        }
    }

    static void LogBacktrace() {
        EnsureDbgHelp();
        void* stack[64]{};
        USHORT frames = RtlCaptureStackBackTrace(0, 64, stack, nullptr);
        spdlog::error("Backtrace (DbgHelp): {} frames", (int)frames);
        HANDLE proc = GetCurrentProcess();

        for (USHORT i = 0; i < frames; ++i) {
            DWORD64 addr = (DWORD64)stack[i];
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(proc, addr, &disp, sym)) {
                spdlog::error("  #{:02d} 0x{:016x} {}+0x{:x}", (int)i, (unsigned long long)addr, sym->Name, (unsigned)disp);
            } else {
                spdlog::error("  #{:02d} 0x{:016x}", (int)i, (unsigned long long)addr);
            }
        }
    }

    static std::wstring BuildDumpPath() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[256];
        swprintf(name, 256, L"\\dump_%04u%02u%02u_%02u%02u%02u_%u.dmp",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
        return S().dumpDir + name;
    }

    // Forward declarations
    static void ReportCrashToDatadog(const std::string& dumpPath);
    static bool WriteMinidumpWin(EXCEPTION_POINTERS* ep);
    static void LogSystemInformation();
    
    static bool WriteMinidumpWin(EXCEPTION_POINTERS* ep) {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        std::wstring path = BuildDumpPath();
        hFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            spdlog::error("MiniDumpWriteDump: failed to create {}", w2u(path));
            return false;
        }
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo);
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(hFile);
        spdlog::error("MiniDumpWriteDump: {} -> {}", ok ? "OK" : "FAILED", w2u(path));
        
        // Report crash to Datadog if dump was successful
        if (ok == TRUE) {
            ReportCrashToDatadog(w2u(path));
        }
        
        return ok == TRUE;
    }
    
    // Report crash event to Datadog
    static void ReportCrashToDatadog(const std::string& dumpPath) {
        try {
            // Datadog logs intake endpoint
            const char* datadogUrl = "http-intake.logs.datadoghq.com";
            const char* apiKey = "your-datadog-api-key"; // Replace with your actual Datadog API key
            
            // Create a background thread to handle upload (don't block crash handler)
            std::thread uploadThread([=]() {
                HINTERNET hInternet = nullptr;
                HINTERNET hConnect = nullptr;
                HINTERNET hRequest = nullptr;
                
                try {
                    spdlog::info("Reporting crash event to Datadog: {}", dumpPath);
                    
                    // Initialize WinINet
                    hInternet = InternetOpenA("MinimalImageViewer/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
                    if (!hInternet) {
                        spdlog::error("Failed to initialize WinINet for Datadog crash reporting");
                        return;
                    }
                    
                    // Connect to Datadog
                    hConnect = InternetConnectA(hInternet, datadogUrl, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
                    if (!hConnect) {
                        spdlog::error("Failed to connect to Datadog server");
                        return;
                    }
                    
                    // Prepare URL path for logs API
                    std::string urlPath = "/v1/input/" + std::string(apiKey);
                    
                    // Open HTTPS request
                    const char* acceptTypes[] = {"application/json", nullptr};
                    hRequest = HttpOpenRequestA(hConnect, "POST", urlPath.c_str(), nullptr, nullptr, acceptTypes, 
                                              INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
                    if (!hRequest) {
                        spdlog::error("Failed to create HTTP request for Datadog crash reporting");
                        return;
                    }
                    
                    // Get process info
                    DWORD processId = GetCurrentProcessId();
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    
                    // Prepare JSON payload for crash event
                    std::string jsonPayload;
                    jsonPayload += "{\"message\":\"Application crash detected\",";
                    jsonPayload += "\"level\":\"error\",";
                    jsonPayload += "\"service\":\"minimal-image-viewer\",";
                    jsonPayload += "\"source\":\"crash-handler\",";
                    jsonPayload += "\"tags\":\"env:development,version:1.0\",";
                    jsonPayload += "\"attributes\":{";
                    jsonPayload += "\"crash.dump_path\":\"" + dumpPath + "\",";
                    jsonPayload += "\"crash.process_id\":" + std::to_string(processId) + ",";
                    jsonPayload += "\"crash.timestamp\":\"" + std::to_string(st.wYear) + "-" + 
                                   std::to_string(st.wMonth) + "-" + std::to_string(st.wDay) + "T" +
                                   std::to_string(st.wHour) + ":" + std::to_string(st.wMinute) + ":" + 
                                   std::to_string(st.wSecond) + "Z\",";
                    jsonPayload += "\"crash.application\":\"MinimalImageViewer\"";
                    jsonPayload += "}}";
                    
                    // Prepare headers
                    std::string headers = "Content-Type: application/json\r\n";
                    headers += "DD-API-KEY: " + std::string(apiKey) + "\r\n";
                    
                    // Send request
                    if (!HttpSendRequestA(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()),
                                         const_cast<char*>(jsonPayload.c_str()), static_cast<DWORD>(jsonPayload.length()))) {
                        spdlog::error("Failed to send crash report to Datadog");
                        return;
                    }
                    
                    // Check response
                    DWORD statusCode = 0;
                    DWORD statusSize = sizeof(statusCode);
                    if (HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, nullptr)) {
                        if (statusCode == 200 || statusCode == 202) {
                            spdlog::info("Successfully reported crash to Datadog (HTTP {})", statusCode);
                        } else {
                            spdlog::warn("Crash report completed with HTTP status: {}", statusCode);
                            
                            // Try to get response body for error details
                            char responseBuffer[1024] = {0};
                            DWORD bytesRead = 0;
                            if (InternetReadFile(hRequest, responseBuffer, sizeof(responseBuffer) - 1, &bytesRead) && bytesRead > 0) {
                                responseBuffer[bytesRead] = '\0';
                                spdlog::error("Datadog response: {}", responseBuffer);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::error("Exception during Datadog crash reporting: {}", e.what());
                } catch (...) {
                    spdlog::error("Unknown exception during Datadog crash reporting");
                }
                
                // Cleanup
                if (hRequest) InternetCloseHandle(hRequest);
                if (hConnect) InternetCloseHandle(hConnect);
                if (hInternet) InternetCloseHandle(hInternet);
            });
            
            // Detach thread to let it run in background
            uploadThread.detach();
        } catch (...) {
            // Never throw from crash handler
        }
    }
    // Comprehensive system information logging for crash analysis
    static void LogSystemInformation() {
#ifdef _WIN32
        // Log OS version information
        OSVERSIONINFOEXW osvi = {};
        osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
        
        // Use RtlGetVersion instead of deprecated GetVersionEx for accuracy
        typedef NTSTATUS (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            RtlGetVersionPtr pRtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
            if (pRtlGetVersion) {
                pRtlGetVersion((PRTL_OSVERSIONINFOW)&osvi);
                spdlog::info("OS Version: {}.{}.{} Build {}, Service Pack {}.{}",
                           osvi.dwMajorVersion, osvi.dwMinorVersion, 
                           osvi.dwBuildNumber, osvi.dwBuildNumber,
                           osvi.wServicePackMajor, osvi.wServicePackMinor);
            }
        }
        
        // Log system memory information
        MEMORYSTATUSEX memStatus = {};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus)) {
            spdlog::info("System Memory: {:.2f} GB total, {:.2f} GB available ({}% used)",
                       memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
                       memStatus.ullAvailPhys / (1024.0 * 1024.0 * 1024.0),
                       memStatus.dwMemoryLoad);
            spdlog::info("Virtual Memory: {:.2f} GB total, {:.2f} GB available",
                       memStatus.ullTotalVirtual / (1024.0 * 1024.0 * 1024.0),
                       memStatus.ullAvailVirtual / (1024.0 * 1024.0 * 1024.0));
            spdlog::info("Page File: {:.2f} GB total, {:.2f} GB available",
                       memStatus.ullTotalPageFile / (1024.0 * 1024.0 * 1024.0),
                       memStatus.ullAvailPageFile / (1024.0 * 1024.0 * 1024.0));
        }
        
        // Log CPU information
        SYSTEM_INFO sysInfo = {};
        GetSystemInfo(&sysInfo);
        spdlog::info("CPU: {} processors, Architecture: {}, Page Size: {} KB",
                   sysInfo.dwNumberOfProcessors, sysInfo.wProcessorArchitecture,
                   sysInfo.dwPageSize / 1024);
        
        // Try to get more detailed CPU info from registry
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 
                         0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t cpuName[256] = {};
            DWORD nameSize = sizeof(cpuName);
            if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, nullptr, 
                               (LPBYTE)cpuName, &nameSize) == ERROR_SUCCESS) {
                spdlog::info("CPU Name: {}", w2u(cpuName));
            }
            
            DWORD cpuMHz = 0;
            DWORD mhzSize = sizeof(cpuMHz);
            if (RegQueryValueExW(hKey, L"~MHz", nullptr, nullptr, 
                               (LPBYTE)&cpuMHz, &mhzSize) == ERROR_SUCCESS) {
                spdlog::info("CPU Speed: {} MHz", cpuMHz);
            }
            
            RegCloseKey(hKey);
        }
        
        // Log process information
        DWORD processId = GetCurrentProcessId();
        HANDLE hProcess = GetCurrentProcess();
        spdlog::info("Process ID: {}", processId);
        
        // Log process memory usage
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        if (GetProcessMemoryInfo(hProcess, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            spdlog::info("Process Memory: Working Set {:.2f} MB, Private {:.2f} MB, Peak Working Set {:.2f} MB",
                       pmc.WorkingSetSize / (1024.0 * 1024.0),
                       pmc.PrivateUsage / (1024.0 * 1024.0),
                       pmc.PeakWorkingSetSize / (1024.0 * 1024.0));
        }
        
        // Log GPU information (basic enumeration)
        spdlog::info("=== GPU Information ===");
        DISPLAY_DEVICEW dispDevice = {};
        dispDevice.cb = sizeof(dispDevice);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dispDevice, 0); ++i) {
            spdlog::info("GPU #{}: {} ({})", i, 
                       w2u(dispDevice.DeviceString),
                       (dispDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "Primary" : "Secondary");
            
            if (dispDevice.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                // Try to get display mode information
                DEVMODEW devMode = {};
                devMode.dmSize = sizeof(devMode);
                if (EnumDisplaySettingsW(dispDevice.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
                    spdlog::info("  Resolution: {}x{} @ {}Hz, Color Depth: {} bits",
                               devMode.dmPelsWidth, devMode.dmPelsHeight,
                               devMode.dmDisplayFrequency, devMode.dmBitsPerPel);
                }
            }
        }
        
        // Log Vulkan information if available
        spdlog::info("=== Vulkan Diagnostics ===");
        
        // Try to load Vulkan DLL and get basic info
        HMODULE vulkanDLL = LoadLibraryA("vulkan-1.dll");
        if (vulkanDLL) {
            spdlog::info("Vulkan DLL: Found vulkan-1.dll");
            
            // Try to get vkEnumerateInstanceVersion
            typedef uint32_t (__stdcall* PFN_vkEnumerateInstanceVersion)(uint32_t* pApiVersion);
            PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = 
                (PFN_vkEnumerateInstanceVersion)GetProcAddress(vulkanDLL, "vkEnumerateInstanceVersion");
            
            if (vkEnumerateInstanceVersion) {
                uint32_t apiVersion = 0;
                if (vkEnumerateInstanceVersion(&apiVersion) == 0) { // VK_SUCCESS
                    uint32_t major = (apiVersion >> 22) & 0x3FF;
                    uint32_t minor = (apiVersion >> 12) & 0x3FF;
                    uint32_t patch = apiVersion & 0xFFF;
                    spdlog::info("Vulkan API Version: {}.{}.{}", major, minor, patch);
                }
            }
            
            FreeLibrary(vulkanDLL);
        } else {
            DWORD error = GetLastError();
            spdlog::warn("Vulkan DLL: vulkan-1.dll not found (error {})", error);
        }
        
        // Log environment variables that might affect graphics/debugging
        const char* envVars[] = {
            "VK_INSTANCE_LAYERS",
            "VK_LOADER_DEBUG",
            "VK_LAYER_PATH",
            "VK_ICD_FILENAMES",
            "DXVK_LOG_LEVEL",
            "DXVK_DEBUG",
            "MESA_DEBUG",
            "LIBGL_DEBUG",
            "PATH"
        };
        
        spdlog::info("=== Environment Variables ===");
        for (const char* envVar : envVars) {
            char buffer[2048] = {};
            DWORD result = GetEnvironmentVariableA(envVar, buffer, sizeof(buffer));
            if (result > 0 && result < sizeof(buffer)) {
                spdlog::info("{}: {}", envVar, buffer);
            } else if (result > sizeof(buffer)) {
                spdlog::info("{}: [too long, {} chars]", envVar, result);
            }
            // If result == 0, the variable doesn't exist (don't log)
        }
        
        // Log debug heap information if available
#ifdef _DEBUG
        spdlog::info("=== Debug Build Information ===");
        spdlog::info("Debug build detected - heap checking enabled");
        
        // Check debug heap flags
        int heapFlags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        spdlog::info("CRT Debug Heap Flags: 0x{:X}", heapFlags);
        if (heapFlags & _CRTDBG_ALLOC_MEM_DF) spdlog::info("  - Memory allocation tracking enabled");
        if (heapFlags & _CRTDBG_DELAY_FREE_MEM_DF) spdlog::info("  - Delay free memory enabled");
        if (heapFlags & _CRTDBG_CHECK_ALWAYS_DF) spdlog::info("  - Check heap on every alloc/free");
        if (heapFlags & _CRTDBG_LEAK_CHECK_DF) spdlog::info("  - Leak checking at exit enabled");
#endif
        
        // Log loaded modules (DLLs)
        spdlog::info("=== Key Loaded Modules ===");
        const wchar_t* keyModules[] = {
            L"vulkan-1.dll",
            L"d3d11.dll",
            L"dxgi.dll",
            L"opengl32.dll",
            L"kernel32.dll",
            L"ntdll.dll",
            L"user32.dll",
            L"gdi32.dll"
        };
        
        for (const wchar_t* moduleName : keyModules) {
            HMODULE hMod = GetModuleHandleW(moduleName);
            if (hMod) {
                wchar_t modulePath[MAX_PATH] = {};
                if (GetModuleFileNameW(hMod, modulePath, MAX_PATH)) {
                    // Get file version info if possible
                    DWORD versionInfoSize = GetFileVersionInfoSizeW(modulePath, nullptr);
                    if (versionInfoSize > 0) {
                        std::vector<BYTE> versionData(versionInfoSize);
                        if (GetFileVersionInfoW(modulePath, 0, versionInfoSize, versionData.data())) {
                            VS_FIXEDFILEINFO* fileInfo = nullptr;
                            UINT fileInfoSize = 0;
                            if (VerQueryValueW(versionData.data(), L"\\", (LPVOID*)&fileInfo, &fileInfoSize)) {
                                spdlog::info("{}: {}.{}.{}.{} ({})", 
                                           w2u(moduleName),
                                           HIWORD(fileInfo->dwFileVersionMS),
                                           LOWORD(fileInfo->dwFileVersionMS),
                                           HIWORD(fileInfo->dwFileVersionLS),
                                           LOWORD(fileInfo->dwFileVersionLS),
                                           w2u(modulePath));
                            } else {
                                spdlog::info("{}: [version unknown] ({})", w2u(moduleName), w2u(modulePath));
                            }
                        } else {
                            spdlog::info("{}: [version read failed] ({})", w2u(moduleName), w2u(modulePath));
                        }
                    } else {
                        spdlog::info("{}: [no version info] ({})", w2u(moduleName), w2u(modulePath));
                    }
                } else {
                    spdlog::info("{}: [path unknown] (loaded at 0x{:016X})", w2u(moduleName), (uintptr_t)hMod);
                }
            }
            // If module not loaded, don't log anything
        }
        
#else
        // Non-Windows platform - log what we can
        spdlog::info("=== System Information (Non-Windows) ===");
        spdlog::info("Platform: Non-Windows (limited system info available)");
#endif
        
        spdlog::info("=== End System Information ===");
    }
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
    
    // Log comprehensive system information for crash analysis
    LogSystemInformation();

#ifdef HAVE_DATADOG
    // Initialize Datadog tracer
    try {
        dd::TracerConfig config;
        config.service = "minimal-image-viewer";
        config.environment = "development";
        config.version = "1.0";
        
        const auto validated_config = dd::finalize_config(config);
        if (!validated_config) {
            spdlog::warn("Failed to initialize Datadog tracer");
        } else {
            S().tracer = std::make_unique<dd::Tracer>(*validated_config);
            spdlog::info("Datadog tracer initialized successfully");
        }
    } catch (const std::exception& e) {
        spdlog::warn("Exception initializing Datadog tracer: {}", e.what());
    } catch (...) {
        spdlog::warn("Unknown exception initializing Datadog tracer");
    }
#endif

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

#ifdef _WIN32
    // Our own unhandled SEH filter: log, stack, and write a dump
    auto sehHandler = [](EXCEPTION_POINTERS* ep) -> LONG {
        spdlog::error("Unhandled SEH: code=0x{:08x} at {}", 
                      ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0u,
                      (void*)(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr));
        LogBacktrace();
#ifdef HAVE_BREAKPAD
        if (S().eh) {
            if (S().eh->WriteMinidumpForException(ep)) {
                spdlog::error("Breakpad WriteMinidumpForException: OK");
            } else {
                spdlog::error("Breakpad WriteMinidumpForException: FAILED, trying MiniDumpWriteDump");
                WriteMinidumpWin(ep);
            }
        } else
#endif
        {
            WriteMinidumpWin(ep);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    };
    SetUnhandledExceptionFilter(sehHandler);
#endif

    // glog already installed a failure signal handler; add std::terminate hook
    std::set_terminate([] {
        spdlog::error("std::terminate called");
        LogBacktrace();
        DumpNow("std::terminate");
        google::LogMessage(__FILE__, __LINE__, google::GLOG_FATAL).stream()
            << "std::terminate called";
        std::abort();
    });
    spdlog::info("Crash handlers installed");
}

void LogStackTrace() noexcept {
    LogBacktrace();
}

void DumpNow(const char* reason) noexcept {
    if (reason) spdlog::error("DumpNow: {}", reason);
#ifdef HAVE_BREAKPAD
    if (S().eh) {
        if (S().eh->WriteMinidump()) {
            spdlog::error("Breakpad WriteMinidump: OK");
            return;
        }
    }
#endif
#ifdef _WIN32
    WriteMinidumpWin(nullptr);
#endif
}

void LogCriticalState(float zoomFactor, float offsetX, float offsetY, const char* context) noexcept {
    const char* ctx = context ? context : "unknown";
    spdlog::error("CRITICAL_STATE [{}]: zoom={:.6f} offset=({:.6f},{:.6f}) finite_zoom={} finite_offsets=({},{})",
                  ctx, zoomFactor, offsetX, offsetY,
                  std::isfinite(zoomFactor) ? "yes" : "no",
                  std::isfinite(offsetX) ? "yes" : "no",
                  std::isfinite(offsetY) ? "yes" : "no");
    
    // Check for potentially dangerous states that could cause crashes
    bool shouldDump = false;
    
    if (!std::isfinite(zoomFactor) || !std::isfinite(offsetX) || !std::isfinite(offsetY)) {
        spdlog::error("DANGER: Non-finite values detected - this may cause crashes");
        shouldDump = true;
    }
    
    if (zoomFactor < 1e-6f || zoomFactor > 1e6f) {
        spdlog::error("DANGER: Extreme zoom value - this may cause numerical overflow");
        shouldDump = true;
    }
    
    if (std::abs(offsetX) > 1e6f || std::abs(offsetY) > 1e6f) {
        spdlog::error("DANGER: Extreme offset values - this may cause integer overflow in renderer");
        shouldDump = true;
    }
    
    if (shouldDump) {
        LogBacktrace();
        DumpNow("dangerous_state_detected");
    }
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

#ifdef HAVE_DATADOG
// Datadog tracing helpers
dd::Span CreateSpan(const char* name) noexcept {
    // For now, we'll create a minimal working span or fail gracefully
    if (S().tracer) {
        try {
            auto span = S().tracer->create_span();
            if (name) {
                span.set_name(name);
            }
            return span;
        } catch (...) {
            // If span creation fails, disable the tracer to avoid future failures
            S().tracer.reset();
            spdlog::error("Datadog span creation failed, disabling tracer");
        }
    }
    
    // If no tracer or creation failed, we need to return a valid span.
    // Since dd::Span{} doesn't work, we'll create a "no-op" tracer temporarily
    // This is a workaround until we can properly handle the API
    try {
        // Create a minimal config for emergency span
        dd::TracerConfig config;
        config.service = "minimal-image-viewer-fallback";
        auto validated_config = dd::finalize_config(config);
        if (validated_config) {
            dd::Tracer fallback_tracer(*validated_config);
            auto span = fallback_tracer.create_span();
            if (name) span.set_name(name);
            return span;
        }
    } catch (...) {
        // Even fallback failed
    }
    
    // If everything fails, we have no choice but to abort
    spdlog::error("Fatal: Cannot create any Datadog span");
    std::abort();
}

dd::Span CreateChildSpan(const dd::Span& parent, const char* name) noexcept {
    try {
        auto span = parent.create_child();
        if (name) {
            span.set_name(name);
        }
        return span;
    } catch (...) {
        // If child creation fails, try creating from main tracer
        if (S().tracer) {
            try {
                auto fallback = S().tracer->create_span();
                if (name) fallback.set_name(name);
                return fallback;
            } catch (...) {
                // Disable tracer on repeated failures
                S().tracer.reset();
            }
        }
    }
    
    // Final fallback - create minimal span
    try {
        dd::TracerConfig config;
        config.service = "minimal-image-viewer-child-fallback";
        auto validated_config = dd::finalize_config(config);
        if (validated_config) {
            dd::Tracer fallback_tracer(*validated_config);
            auto span = fallback_tracer.create_span();
            if (name) span.set_name(name);
            return span;
        }
    } catch (...) {
        // Even fallback failed
    }
    
    spdlog::error("Fatal: Cannot create any child span");
    std::abort();
}
#endif

} // namespace Logger
