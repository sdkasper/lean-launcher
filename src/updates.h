#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace takeoff {

inline constexpr wchar_t kAppVersion[] = L"1.0.3";
inline constexpr wchar_t kDefaultReleasesUrl[] = L"https://github.com/akiraeng/takeoff-launcher/releases";
inline constexpr wchar_t kDefaultApiHost[] = L"api.github.com";
inline constexpr wchar_t kDefaultApiPath[] = L"/repos/akiraeng/takeoff-launcher/releases/latest";

inline std::wstring ExtractTagName(std::string_view json) {
    const std::string_view key = "\"tag_name\"";
    size_t pos = json.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    if (pos >= json.size() || json[pos] != '"') return {};
    pos++; // skip opening quote
    size_t end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    std::string tag(json.substr(pos, end - pos));
    return std::wstring(tag.begin(), tag.end());
}

inline std::vector<int> ParseVersion(std::wstring_view v) {
    while (!v.empty() && (v.front() == L'v' || v.front() == L'V' || v.front() == L' ')) {
        v.remove_prefix(1);
    }
    std::vector<int> parts;
    int current = 0;
    bool hasNum = false;
    for (wchar_t ch : v) {
        if (ch >= L'0' && ch <= L'9') {
            current = current * 10 + (ch - L'0');
            hasNum = true;
        } else if (ch == L'.' || ch == L'-') {
            if (hasNum) {
                parts.push_back(current);
                current = 0;
                hasNum = false;
            }
            if (ch == L'-') break; // Ignore pre-release suffixes (e.g. -beta, -rc1)
        }
    }
    if (hasNum) {
        parts.push_back(current);
    }
    return parts;
}

inline bool IsNewerVersion(std::wstring_view remote, std::wstring_view current) {
    auto r = ParseVersion(remote);
    auto c = ParseVersion(current);
    if (r.empty()) return false;
    const size_t n = (std::max)(r.size(), c.size());
    for (size_t i = 0; i < n; ++i) {
        const int rv = i < r.size() ? r[i] : 0;
        const int cv = i < c.size() ? c[i] : 0;
        if (rv > cv) return true;
        if (rv < cv) return false;
    }
    return false;
}

inline bool ShouldCheckForUpdates(uint64_t lastCheckSeconds, uint64_t currentSeconds, bool enabled) {
    if (!enabled) return false;
    constexpr uint64_t k24HoursInSeconds = 24 * 60 * 60; // 86400
    if (currentSeconds < lastCheckSeconds) return true; // Clock shifted backwards
    return (currentSeconds - lastCheckSeconds) >= k24HoursInSeconds;
}

inline std::wstring ExtractAssetDownloadUrl(std::string_view json, std::wstring_view tag,
                                           std::wstring_view preferredName = L"Takeoff.exe") {
    const std::string_view urlKey = "\"browser_download_url\"";
    size_t pos = 0;
    std::wstring fallbackExeUrl;

    auto toLowerW = [](std::wstring_view s) {
        std::wstring out;
        out.reserve(s.size());
        for (wchar_t c : s) out.push_back(towlower(c));
        return out;
    };

    const std::wstring targetLower = toLowerW(preferredName);

    while ((pos = json.find(urlKey, pos)) != std::string_view::npos) {
        pos += urlKey.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
            pos++;
        }
        if (pos >= json.size() || json[pos] != '"') continue;
        pos++; // skip opening quote
        size_t end = json.find('"', pos);
        if (end == std::string_view::npos) break;
        std::string rawUrl = std::string(json.substr(pos, end - pos));
        pos = end + 1;

        std::wstring url(rawUrl.begin(), rawUrl.end());
        std::wstring urlLower = toLowerW(url);

        // Match exact or contains preferred name (e.g. Takeoff.exe)
        if (urlLower.find(targetLower) != std::wstring::npos) {
            return url;
        }
        // If it's an .exe file, save as secondary fallback
        if (fallbackExeUrl.empty() && urlLower.size() >= 4 &&
            urlLower.compare(urlLower.size() - 4, 4, L".exe") == 0) {
            fallbackExeUrl = url;
        }
    }

    if (!fallbackExeUrl.empty()) {
        return fallbackExeUrl;
    }

    if (!tag.empty()) {
        std::wstring fallback = L"https://github.com/akiraeng/takeoff-launcher/releases/download/";
        fallback += tag;
        fallback += L"/";
        fallback += preferredName;
        return fallback;
    }

    return {};
}

inline bool ValidateExecutableFile(const std::wstring& filePath) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(filePath, ec);
    if (ec || size < 65536) { // Real Takeoff executable is ~400KB+
        return false;
    }
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    IMAGE_DOS_HEADER dosHeader{};
    DWORD read = 0;
    if (!ReadFile(file, &dosHeader, sizeof(dosHeader), &read, nullptr) || read != sizeof(dosHeader)) {
        CloseHandle(file);
        return false;
    }
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0 ||
        static_cast<uintmax_t>(dosHeader.e_lfanew) >= size) {
        CloseHandle(file);
        return false;
    }

    if (SetFilePointer(file, dosHeader.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(file);
        return false;
    }

    DWORD ntSignature = 0;
    if (!ReadFile(file, &ntSignature, sizeof(ntSignature), &read, nullptr) || read != sizeof(ntSignature)) {
        CloseHandle(file);
        return false;
    }
    if (ntSignature != IMAGE_NT_SIGNATURE) {
        CloseHandle(file);
        return false;
    }

    IMAGE_FILE_HEADER fileHeader{};
    if (!ReadFile(file, &fileHeader, sizeof(fileHeader), &read, nullptr) || read != sizeof(fileHeader)) {
        CloseHandle(file);
        return false;
    }

    CloseHandle(file);
    return (fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 || fileHeader.Machine == IMAGE_FILE_MACHINE_I386);
}

inline std::wstring GetUpdateStagingPath(std::wstring_view tag) {
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
        std::filesystem::path dir = std::filesystem::path(localAppData) / L"Takeoff" / L"updates";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::wstring filename = L"Takeoff_";
        for (wchar_t ch : tag) {
            if (iswalnum(ch) || ch == L'.' || ch == L'-') filename.push_back(ch);
            else filename.push_back(L'_');
        }
        filename += L".exe";
        return (dir / filename).wstring();
    }
    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) > 0) {
        std::filesystem::path dir = std::filesystem::path(tempPath) / L"Takeoff";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::wstring filename = L"Takeoff_update.exe";
        return (dir / filename).wstring();
    }
    return {};
}

inline bool DownloadUpdateFile(std::wstring_view initialUrl, const std::wstring& destPath) {
    if (initialUrl.empty() || destPath.empty()) return false;

    std::wstring currentUrl(initialUrl);
    int redirectsRemaining = 5;

    std::wstring tempPath = destPath + L".tmp";
    DeleteFileW(tempPath.c_str());

    HANDLE outFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"Takeoff-Launcher/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        CloseHandle(outFile);
        DeleteFileW(tempPath.c_str());
        return false;
    }

    WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool downloadSuccess = false;

    while (redirectsRemaining-- > 0) {
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostName[256]{};
        wchar_t urlPath[2048]{};
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostName));
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));

        if (!WinHttpCrackUrl(currentUrl.c_str(), static_cast<DWORD>(currentUrl.size()), 0, &urlComp)) {
            break;
        }

        HINTERNET connect = WinHttpConnect(session, hostName, urlComp.nPort, 0);
        if (!connect) {
            break;
        }

        DWORD openFlags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", urlPath, nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              openFlags);
        if (!request) {
            WinHttpCloseHandle(connect);
            break;
        }

        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            break;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
            DWORD locLen = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &locLen, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locLen > 0) {
                std::vector<wchar_t> locBuf(locLen / sizeof(wchar_t) + 1);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                        locBuf.data(), &locLen, WINHTTP_NO_HEADER_INDEX)) {
                    currentUrl = locBuf.data();
                    WinHttpCloseHandle(request);
                    WinHttpCloseHandle(connect);
                    continue;
                }
            }
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            break;
        }

        if (statusCode == 200) {
            DWORD expectedContentLength = 0;
            DWORD clSize = sizeof(expectedContentLength);
            const bool hasContentLength = WinHttpQueryHeaders(request,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &expectedContentLength, &clSize, WINHTTP_NO_HEADER_INDEX);

            DWORD totalDownloaded = 0;
            DWORD bytesAvailable = 0;
            bool readOk = true;
            while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                    DWORD bytesWritten = 0;
                    if (!WriteFile(outFile, buffer.data(), bytesRead, &bytesWritten, nullptr) ||
                        bytesWritten != bytesRead) {
                        readOk = false;
                        break;
                    }
                    totalDownloaded += bytesRead;
                } else {
                    readOk = false;
                    break;
                }
            }
            if (hasContentLength && expectedContentLength > 0 && totalDownloaded != expectedContentLength) {
                readOk = false; // Truncated download protection
            }
            downloadSuccess = readOk;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        break;
    }

    WinHttpCloseHandle(session);
    CloseHandle(outFile);

    if (downloadSuccess && ValidateExecutableFile(tempPath)) {
        // Retry move in case antivirus scanner is momentarily scanning the file
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (MoveFileExW(tempPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                return true;
            }
            Sleep(100);
        }
    }

    DeleteFileW(tempPath.c_str());
    return false;
}

inline bool QueryLatestReleaseInfo(std::wstring_view host, std::wstring_view path,
                                  std::wstring& outTag, std::wstring& outHtmlUrl,
                                  std::wstring& outAssetUrl) {
    HINTERNET session = WinHttpOpen(L"Takeoff-Launcher/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    // 5-second timeouts for connect, send, and receive so we never stall
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    std::wstring hostStr(host);
    HINTERNET connect = WinHttpConnect(session, hostStr.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring pathStr(path);
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathStr.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    // GitHub API requires Accept header and recommends User-Agent (already set in WinHttpOpen)
    const wchar_t headers[] = L"Accept: application/vnd.github.v3+json\r\n";
    BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    bool success = false;
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200) {
            std::string response;
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0) {
                std::vector<char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                    response.append(buffer.data(), bytesRead);
                }
            }
            std::wstring tag = ExtractTagName(response);
            if (!tag.empty()) {
                outTag = std::move(tag);
                // Also optionally extract "html_url" if present
                const std::string_view htmlUrlKey = "\"html_url\"";
                size_t hpos = response.find(htmlUrlKey);
                if (hpos != std::string_view::npos) {
                    hpos += htmlUrlKey.size();
                    while (hpos < response.size() && (response[hpos] == ' ' || response[hpos] == ':' || response[hpos] == '\t')) hpos++;
                    if (hpos < response.size() && response[hpos] == '"') {
                        hpos++;
                        size_t hend = response.find('"', hpos);
                        if (hend != std::string_view::npos) {
                            std::string u = response.substr(hpos, hend - hpos);
                            outHtmlUrl = std::wstring(u.begin(), u.end());
                        }
                    }
                }
                outAssetUrl = ExtractAssetDownloadUrl(response, outTag);
                success = true;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

inline bool QueryLatestReleaseTag(std::wstring_view host, std::wstring_view path,
                                  std::wstring& outTag, std::wstring& outHtmlUrl) {
    std::wstring dummyAssetUrl;
    return QueryLatestReleaseInfo(host, path, outTag, outHtmlUrl, dummyAssetUrl);
}

inline bool ApplyUpdateAndRestart(const std::wstring& updateExePath) {
    if (!ValidateExecutableFile(updateExePath)) {
        return false;
    }

    wchar_t currentExe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, currentExe, MAX_PATH)) {
        return false;
    }

    const std::wstring currentExeStr(currentExe);
    const std::wstring backupExeStr = currentExeStr + L".old";

    // 1. Attempt in-place rename and move with retry loop (for transient AV locks)
    bool swapped = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DeleteFileW(backupExeStr.c_str());
        if (MoveFileExW(currentExeStr.c_str(), backupExeStr.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (MoveFileExW(updateExePath.c_str(), currentExeStr.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                swapped = true;
                break;
            } else {
                // Rollback if destination move failed
                MoveFileExW(backupExeStr.c_str(), currentExeStr.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        Sleep(100);
    }

    if (swapped) {
        HINSTANCE inst = ShellExecuteW(nullptr, L"open", currentExeStr.c_str(), L"--replace", nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(inst) > 32;
    }

    // 2. Fallback: Write a robust batch updater script in %TEMP%
    // Waits for the current PID to exit, retries the move, launches the new executable, and cleans up.
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) > 0) {
        const std::wstring batPath = std::wstring(tempDir) + L"takeoff_updater.bat";
        const DWORD pid = GetCurrentProcessId();

        HANDLE batFile = CreateFileW(batPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (batFile != INVALID_HANDLE_VALUE) {
            char batContent[2048]{};
            sprintf_s(batContent,
                "@echo off\r\n"
                ":wait_pid\r\n"
                "tasklist /fi \"pid eq %lu\" 2>nul | find \"%lu\" >nul\r\n"
                "if not errorlevel 1 (\r\n"
                "    ping 127.0.0.1 -n 2 >nul\r\n"
                "    goto wait_pid\r\n"
                ")\r\n"
                ":move_loop\r\n"
                "move /y \"%ls\" \"%ls\" >nul 2>&1\r\n"
                "if errorlevel 1 (\r\n"
                "    ping 127.0.0.1 -n 2 >nul\r\n"
                "    goto move_loop\r\n"
                ")\r\n"
                "start \"\" \"%ls\" --replace\r\n"
                "del \"%%~f0\"\r\n",
                pid, pid, updateExePath.c_str(), currentExeStr.c_str(), currentExeStr.c_str());

            DWORD written = 0;
            WriteFile(batFile, batContent, static_cast<DWORD>(strlen(batContent)), &written, nullptr);
            CloseHandle(batFile);

            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"open";
            sei.lpFile = batPath.c_str();
            sei.nShow = SW_HIDE;

            if (ShellExecuteExW(&sei)) {
                return true;
            }

            // If access denied (e.g. Program Files), request elevation via UAC
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                sei.lpVerb = L"runas";
                if (ShellExecuteExW(&sei)) {
                    return true;
                }
            }
        }
    }

    return false;
}

inline void CleanupOldUpdates() {
    wchar_t currentExe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH)) {
        const std::wstring oldExe = std::wstring(currentExe) + L".old";
        DeleteFileW(oldExe.c_str());
    }
}

} // namespace takeoff

