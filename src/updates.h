#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cwchar>
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

#include "minisign.h"
#include "sha256.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace takeoff {

inline constexpr wchar_t kAppVersion[] = L"2.0.4";
inline constexpr wchar_t kRepoUrl[] = L"https://github.com/sdkasper/lean-launcher";
inline constexpr wchar_t kDefaultReleasesUrl[] = L"https://github.com/sdkasper/lean-launcher/releases";
inline constexpr wchar_t kDefaultApiHost[] = L"api.github.com";
inline constexpr wchar_t kDefaultApiPath[] = L"/repos/sdkasper/lean-launcher/releases/latest";
// US-040: the minisign public key (key ID 104A455E07B4BFA1) that signs every
// release's LeanLauncher.exe.sha256. A compile-time constant with no runtime
// override (NFR-009).
inline constexpr char kUpdatePublicKey[] = "RWShv7QHXkVKEAzaW1aa8XxLrSVCGpRt8eRHE3EqhiB0p5aA5jNQ6i5Q";

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

// What the About tab's update row (US-029) shows. Driven by both the
// automatic startup check and the manual one - they share one worker.
// Unverified (US-040): a newer release exists but could not be proven to be
// the official one, so nothing was staged. It reads like Available.
enum class UpdateCheckState { Idle, Checking, Downloading, UpToDate, Ready, Available, Failed, Unverified };

// Posted from the update worker to the UI thread (lParam, owned by the
// receiver). `path` is only set for a downloaded update.
struct UpdateCheckResult {
    std::wstring tag;
    std::wstring htmlUrl;
    std::wstring path;
    // Lowercase SHA-256 of the bytes received when `path` was downloaded.
    std::string sha256;
};

inline std::wstring DisplayTag(std::wstring_view tag) {
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V')) return std::wstring(tag);
    return L"v" + std::wstring(tag);
}

inline std::wstring UpdateRowText(UpdateCheckState state, std::wstring_view tag) {
    switch (state) {
    case UpdateCheckState::Checking: return L"Checking…";
    case UpdateCheckState::Downloading:
        return tag.empty() ? std::wstring(L"Downloading update…") : L"Downloading " + DisplayTag(tag) + L"…";
    case UpdateCheckState::UpToDate: return L"Up to date";
    case UpdateCheckState::Ready: return DisplayTag(tag) + L" ready - Restart to update";
    case UpdateCheckState::Available:
    case UpdateCheckState::Unverified: return DisplayTag(tag) + L" available - open release page";
    case UpdateCheckState::Failed: return L"Check failed - try again";
    case UpdateCheckState::Idle: break;
    }
    return L"Check now";
}

// The description under the About tab's update row: the unverified warning
// replaces the "last checked" text while it applies.
inline std::wstring UpdateRowDescription(UpdateCheckState state, std::wstring_view lastCheckText) {
    if (state == UpdateCheckState::Unverified) return L"Update couldn't be verified";
    return std::wstring(lastCheckText);
}

inline bool IsUpdateRowClickable(UpdateCheckState state) {
    return state != UpdateCheckState::Checking && state != UpdateCheckState::Downloading;
}

// The row state a finished check leads to. wParam-style verdict: 0 up to
// date, 1 newer release but download failed, 2 downloaded and verified,
// 3 check failed, 4 newer release that could not be verified. A failed check
// (e.g. a network blip on the 24-hour re-check) keeps an update that is
// already downloaded or available rather than hiding it behind "Check failed".
inline UpdateCheckState NextUpdateState(uintptr_t verdict, bool hadDownloaded, bool hadAvailable) {
    switch (verdict) {
    case 2: return UpdateCheckState::Ready;
    case 1: return UpdateCheckState::Available;
    case 4: return UpdateCheckState::Unverified;
    case 0: return UpdateCheckState::UpToDate;
    default:
        if (hadDownloaded) return UpdateCheckState::Ready;
        if (hadAvailable) return UpdateCheckState::Available;
        return UpdateCheckState::Failed;
    }
}

// The release page the "available" row opens. `htmlUrl` comes from the
// GitHub API response, so it is untrusted: ShellExecute would happily run a
// file:, UNC, or custom-protocol URL. Only a plain https URL under this
// repo is used; anything else opens the default releases page.
inline std::wstring ReleasePageUrlOrDefault(std::wstring_view htmlUrl) {
    static constexpr std::wstring_view kPrefix = L"https://github.com/sdkasper/lean-launcher/";
    const bool safe = htmlUrl.size() > kPrefix.size() && htmlUrl.substr(0, kPrefix.size()) == kPrefix &&
        htmlUrl.find(L"..") == std::wstring_view::npos &&
        std::all_of(htmlUrl.begin(), htmlUrl.end(), [](wchar_t ch) {
            return ch > L' ' && ch < 0x7F && ch != L'"' && ch != L'\\' && ch != L'<' && ch != L'>';
        });
    return safe ? std::wstring(htmlUrl) : std::wstring(kDefaultReleasesUrl);
}

// "Last checked: 23 Sep 2026, 17:40" in local time, or "never" for 0.
inline std::wstring FormatLastUpdateCheck(uint64_t seconds) {
    if (seconds == 0) return L"Last checked: never";
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return L"Last checked: never";
    wchar_t buf[64]{};
    if (std::wcsftime(buf, 64, L"%d %b %Y, %H:%M", &local) == 0) return L"Last checked: never";
    return std::wstring(L"Last checked: ") + buf;
}

inline bool ShouldCheckForUpdates(uint64_t lastCheckSeconds, uint64_t currentSeconds, bool enabled) {
    if (!enabled) return false;
    constexpr uint64_t k24HoursInSeconds = 24 * 60 * 60; // 86400
    if (currentSeconds < lastCheckSeconds) return true; // Clock shifted backwards
    return (currentSeconds - lastCheckSeconds) >= k24HoursInSeconds;
}

// US-040: only an asset whose file name is exactly `assetName` is ever
// returned, and only from this repo's release downloads over https. There is no
// fallback to another .exe and no URL is made up from the tag: a release
// without the asset simply has nothing to install.
inline std::wstring ExtractAssetDownloadUrl(std::string_view json, std::wstring_view /*tag*/,
                                           std::wstring_view assetName = L"LeanLauncher.exe") {
    static constexpr std::wstring_view kDownloadPrefix =
        L"https://github.com/sdkasper/lean-launcher/releases/download/";
    const std::string_view urlKey = "\"browser_download_url\"";
    size_t pos = 0;

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

        // A plain ASCII URL only: anything with an escape or a control
        // character is not a download link this launcher would have made.
        if (!std::all_of(rawUrl.begin(), rawUrl.end(), [](char ch) {
                const unsigned char c = static_cast<unsigned char>(ch);
                return c > 0x20 && c < 0x7F && ch != '\\';
            })) {
            continue;
        }
        std::wstring url(rawUrl.begin(), rawUrl.end());
        if (url.size() <= kDownloadPrefix.size() || url.compare(0, kDownloadPrefix.size(), kDownloadPrefix) != 0 ||
            url.find(L"..") != std::wstring::npos || url.find_first_of(L"?#") != std::wstring::npos) {
            continue;
        }

        // Match the asset filename (final path segment) exactly - a
        // substring match would let "LeanLauncher.exe.sha256" win over the
        // real executable if it is listed first.
        const size_t lastSlash = url.find_last_of(L'/');
        if (std::wstring_view(url).substr(lastSlash + 1) == assetName) return url;
    }

    return {};
}

// The three files of a release that an update needs (US-040). Any of them may
// be empty when the release does not carry that asset.
struct ReleaseAssets {
    std::wstring exe;
    std::wstring sha256;
    std::wstring minisig;
};

inline ReleaseAssets ExtractReleaseAssets(std::string_view json, std::wstring_view tag) {
    return {ExtractAssetDownloadUrl(json, tag, L"LeanLauncher.exe"),
            ExtractAssetDownloadUrl(json, tag, L"LeanLauncher.exe.sha256"),
            ExtractAssetDownloadUrl(json, tag, L"LeanLauncher.exe.minisig")};
}

// Update files may only come from GitHub over https: no other scheme, no
// port, no user info, and a host that is github.com or one of its
// githubusercontent.com asset hosts (where release downloads redirect to).
inline bool IsAllowedUpdateUrl(std::wstring_view url) {
    static constexpr std::wstring_view kScheme = L"https://";
    if (url.size() <= kScheme.size() || url.compare(0, kScheme.size(), kScheme) != 0) return false;
    const std::wstring_view rest = url.substr(kScheme.size());
    const size_t end = rest.find_first_of(L"/?#");
    std::wstring host(rest.substr(0, end));
    if (host.empty() || host.find_first_of(L"@:\\ ") != std::wstring::npos) return false;
    for (wchar_t& ch : host) {
        if (ch >= 0x7F || ch <= L' ') return false;
        ch = static_cast<wchar_t>(towlower(ch));
    }
    static constexpr std::wstring_view kAssetHostSuffix = L".githubusercontent.com";
    return host == L"github.com" ||
        (host.size() > kAssetHostSuffix.size() &&
         host.compare(host.size() - kAssetHostSuffix.size(), kAssetHostSuffix.size(), kAssetHostSuffix) == 0);
}

inline bool ValidateExecutableFile(const std::wstring& filePath) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(filePath, ec);
    if (ec || size < 65536) { // Real Lean Launcher executable is ~400KB+
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

// The same sanity check as ValidateExecutableFile, on bytes already in memory:
// the elevated update helper validates the exact buffer it hashed and installs.
inline bool ValidateExecutableBuffer(const uint8_t* data, size_t size) {
    if (!data || size < 65536) return false;
    IMAGE_DOS_HEADER dosHeader{};
    std::memcpy(&dosHeader, data, sizeof(dosHeader));
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0) return false;
    const size_t ntOffset = static_cast<size_t>(dosHeader.e_lfanew);
    if (ntOffset > size || size - ntOffset < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) return false;
    DWORD ntSignature = 0;
    std::memcpy(&ntSignature, data + ntOffset, sizeof(ntSignature));
    if (ntSignature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, data + ntOffset + sizeof(ntSignature), sizeof(fileHeader));
    return fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 || fileHeader.Machine == IMAGE_FILE_MACHINE_I386;
}

inline std::wstring GetUpdateStagingPath(std::wstring_view tag) {
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
        std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher" / L"updates";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::wstring filename = L"LeanLauncher_";
        for (wchar_t ch : tag) {
            if (iswalnum(ch) || ch == L'.' || ch == L'-') filename.push_back(ch);
            else filename.push_back(L'_');
        }
        filename += L".exe";
        return (dir / filename).wstring();
    }
    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) > 0) {
        std::filesystem::path dir = std::filesystem::path(tempPath) / L"LeanLauncher";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::wstring filename = L"LeanLauncher_update.exe";
        return (dir / filename).wstring();
    }
    return {};
}

// `outSha256`, when given, receives the lowercase SHA-256 of exactly the bytes
// received from the network (only meaningful when the call returns true).
inline bool DownloadUpdateFile(std::wstring_view initialUrl, const std::wstring& destPath,
                               std::string* outSha256 = nullptr) {
    if (initialUrl.empty() || destPath.empty()) return false;
    if (outSha256) outSha256->clear();
    Sha256 hasher;

    std::wstring currentUrl(initialUrl);
    int redirectsRemaining = 5;

    std::wstring tempPath = destPath + L".tmp";
    DeleteFileW(tempPath.c_str());

    HANDLE outFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"LeanLauncher/0.1",
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
                    hasher.Update(buffer.data(), bytesRead);
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
                if (outSha256) *outSha256 = hasher.FinalHex();
                return true;
            }
            Sleep(100);
        }
    }

    DeleteFileW(tempPath.c_str());
    return false;
}

// ---- US-040: signed update verification -------------------------------------

inline constexpr size_t kMaxVerificationFileBytes = 1024;
inline constexpr wchar_t kSha256FileSuffix[] = L".sha256";
inline constexpr wchar_t kMinisigFileSuffix[] = L".minisig";

// The two verification files are staged next to the update exe, under its name.
inline std::wstring VerificationFilePath(const std::wstring& exePath, const wchar_t* suffix) {
    return exePath + suffix;
}

// The release tag as the plain ASCII text the signature covers.
inline bool NarrowReleaseTag(std::wstring_view tag, std::string& out) {
    out.clear();
    if (tag.empty() || tag.size() > 64) return false;
    for (wchar_t ch : tag) {
        if (ch <= L' ' || ch >= 0x7F) return false;
        out.push_back(static_cast<char>(ch));
    }
    return true;
}

// Decides whether a downloaded update may be installed by this running
// version: the key, signature, signed tag and exe hash must all check out, and
// the signed release must be newer than `currentVersion` (so an older signed
// release cannot be replayed as an "update"). The tag is taken from the signed
// text, which VerifyUpdate then checks the signature against.
inline UpdateVerifyResult VerifyUpdateForInstall(const MinisignPublicKey& key, std::string_view signedText,
                                                 std::string_view signatureFile, std::string_view exeSha256Hex,
                                                 std::wstring_view currentVersion) {
    minisign_detail::SignedText parsed;
    if (!minisign_detail::ParseSignedText(signedText, parsed)) return UpdateVerifyResult::Malformed;
    const UpdateVerifyResult result = VerifyUpdate(key, signedText, signatureFile, exeSha256Hex, parsed.tag);
    if (result != UpdateVerifyResult::Ok) return result;
    const std::wstring tag(parsed.tag.begin(), parsed.tag.end());
    return IsNewerVersion(tag, currentVersion) ? UpdateVerifyResult::Ok : UpdateVerifyResult::WrongTag;
}

// A regular file of at most kMaxVerificationFileBytes (no directory, no reparse point).
inline bool ReadSmallFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow > 0 && info.nFileSizeLow <= kMaxVerificationFileBytes;
    if (ok) {
        out.resize(info.nFileSizeLow);
        DWORD got = 0;
        ok = ReadFile(file, out.data(), info.nFileSizeLow, &got, nullptr) && got == info.nFileSizeLow;
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

inline bool WriteSmallFile(const std::wstring& path, std::string_view text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) &&
        wrote == text.size();
    CloseHandle(file);
    return ok;
}

// SHA-256 of a whole file (at most `maxBytes`), as lowercase hex.
inline bool HashFileSha256(const std::wstring& path, std::string& hex, uint64_t maxBytes = 64ull * 1024 * 1024) {
    hex.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    ok = ok && size > 0 && size <= maxBytes;
    if (ok) {
        Sha256 hasher;
        std::vector<uint8_t> buffer(1u << 16);
        uint64_t done = 0;
        while (ok && done < size) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(buffer.size(), size - done));
            ok = ReadFile(file, buffer.data(), want, &got, nullptr) && got > 0;
            if (ok) {
                hasher.Update(buffer.data(), got);
                done += got;
            }
        }
        if (ok) hex = hasher.FinalHex();
    }
    CloseHandle(file);
    return ok;
}

// Re-reads the staged verification files and checks them against `exeSha256Hex`
// (the hash of the exe's actual bytes) with `key`.
inline UpdateVerifyResult VerifyStagedUpdateFiles(const MinisignPublicKey& key, const std::wstring& exePath,
                                                  std::string_view exeSha256Hex, std::wstring_view currentVersion) {
    std::string signedText, signatureFile;
    if (!ReadSmallFile(VerificationFilePath(exePath, kSha256FileSuffix), signedText) ||
        !ReadSmallFile(VerificationFilePath(exePath, kMinisigFileSuffix), signatureFile)) {
        return UpdateVerifyResult::Malformed;
    }
    return VerifyUpdateForInstall(key, signedText, signatureFile, exeSha256Hex, currentVersion);
}

// The check run right before an update is installed, with the release key built
// into this launcher.
inline UpdateVerifyResult VerifyStagedUpdate(const std::wstring& exePath, std::string_view exeSha256Hex) {
    MinisignPublicKey key;
    if (!ParseMinisignPublicKey(kUpdatePublicKey, key)) return UpdateVerifyResult::Malformed;
    return VerifyStagedUpdateFiles(key, exePath, exeSha256Hex, kAppVersion);
}

namespace detail {
struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET h = nullptr) : handle(h) {}
    ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};
} // namespace detail

// Downloads a small text file (at most `maxBytes`) into memory. https only, from
// GitHub hosts only (IsAllowedUpdateUrl), following at most 3 redirects by hand
// so every hop is checked.
inline bool DownloadSmallText(std::wstring_view initialUrl, size_t maxBytes, std::string& out) {
    out.clear();
    detail::InternetHandle session(WinHttpOpen(L"LeanLauncher/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) return false;
    WinHttpSetTimeouts(session.handle, 10000, 10000, 10000, 10000);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    std::wstring url(initialUrl);
    for (int hop = 0; hop < 4; ++hop) {
        if (!IsAllowedUpdateUrl(url)) return false;
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t hostName[256]{};
        wchar_t urlPath[2048]{};
        parts.lpszHostName = hostName;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(hostName));
        parts.lpszUrlPath = urlPath;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return false;

        detail::InternetHandle connect(WinHttpConnect(session.handle, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connect.handle) return false;
        detail::InternetHandle request(WinHttpOpenRequest(connect.handle, L"GET", urlPath, nullptr, WINHTTP_NO_REFERER,
                                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request.handle) return false;
        if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.handle, nullptr)) {
            return false;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
            return false;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            DWORD locationBytes = 0;
            WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                                &locationBytes, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locationBytes == 0 || locationBytes > 4096) return false;
            std::vector<wchar_t> location(locationBytes / sizeof(wchar_t) + 1, L'\0');
            if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &locationBytes, WINHTTP_NO_HEADER_INDEX)) {
                return false;
            }
            url = location.data();
            continue;
        }
        if (status != 200) return false;

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
            if (out.size() + available > maxBytes) {
                out.clear();
                return false;
            }
            std::string chunk(available, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(request.handle, chunk.data(), available, &got) || got == 0) {
                out.clear();
                return false;
            }
            out.append(chunk.data(), got);
        }
        if (out.empty()) return false;
        return true;
    }
    return false;
}

enum class UpdateDownloadOutcome {
    Verified,        // downloaded, signed by the release key, ready to install
    DownloadFailed,  // nothing usable arrived (network, missing exe asset)
    Unverified,      // downloaded or offered, but it could not be proven to be the official release
};

// Downloads the update exe and its two verification files and only reports
// Verified when VerifyUpdate passes against the release key. On any other
// outcome nothing is left in the staging folder.
inline UpdateDownloadOutcome DownloadAndVerifyUpdate(const ReleaseAssets& assets, std::wstring_view tag,
                                                     const std::wstring& stagingPath, std::string& sha256Out) {
    sha256Out.clear();
    if (assets.exe.empty() || stagingPath.empty()) return UpdateDownloadOutcome::DownloadFailed;
    const std::wstring sha256Path = VerificationFilePath(stagingPath, kSha256FileSuffix);
    const std::wstring minisigPath = VerificationFilePath(stagingPath, kMinisigFileSuffix);
    const auto cleanup = [&] {
        DeleteFileW(stagingPath.c_str());
        DeleteFileW(sha256Path.c_str());
        DeleteFileW(minisigPath.c_str());
    };
    cleanup();
    // A release that does not carry both verification files cannot be proven
    // official, so its exe is not even downloaded.
    if (assets.sha256.empty() || assets.minisig.empty()) return UpdateDownloadOutcome::Unverified;

    std::string exeSha256;
    if (!DownloadUpdateFile(assets.exe, stagingPath, &exeSha256)) return UpdateDownloadOutcome::DownloadFailed;

    std::string signedText, signatureFile;
    if (!DownloadSmallText(assets.sha256, kMaxVerificationFileBytes, signedText) ||
        !DownloadSmallText(assets.minisig, kMaxVerificationFileBytes, signatureFile)) {
        cleanup();
        return UpdateDownloadOutcome::DownloadFailed;
    }

    MinisignPublicKey key;
    std::string narrowTag;
    if (!ParseMinisignPublicKey(kUpdatePublicKey, key) || !NarrowReleaseTag(tag, narrowTag) ||
        VerifyUpdate(key, signedText, signatureFile, exeSha256, narrowTag) != UpdateVerifyResult::Ok) {
        cleanup();
        return UpdateDownloadOutcome::Unverified;
    }
    if (!WriteSmallFile(sha256Path, signedText) || !WriteSmallFile(minisigPath, signatureFile)) {
        cleanup();
        return UpdateDownloadOutcome::DownloadFailed;
    }
    sha256Out = std::move(exeSha256);
    return UpdateDownloadOutcome::Verified;
}

inline bool QueryLatestReleaseInfo(std::wstring_view host, std::wstring_view path,
                                  std::wstring& outTag, std::wstring& outHtmlUrl,
                                  std::wstring& outAssetUrl, ReleaseAssets* outAssets = nullptr) {
    HINTERNET session = WinHttpOpen(L"LeanLauncher/0.1",
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
                if (outAssets) *outAssets = ExtractReleaseAssets(response, outTag);
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

// ---- Published-digest confirmation (used by the elevated update helper) -----

namespace detail {

// A strict little JSON reader that only answers one question: does some asset
// object of the release (a direct member of an element of the top-level
// "assets" array) carry a "digest" string equal to "sha256:<hash>"? The whole
// document must be valid JSON. Keys are only recognised when written without
// escapes, so a crafted name cannot forge or hide a digest field.
class ReleaseDigestScanner {
public:
    ReleaseDigestScanner(std::string_view json, std::string_view wantedHex) : s_(json), wanted_(wantedHex) {}

    bool Run() {
        if (!IsSha256Hex(wanted_)) return false;
        SkipWs();
        if (!Value(0, Mode::Root)) return false;
        SkipWs();
        return i_ == s_.size() && found_;
    }

private:
    enum class Mode { Other, Root, Assets, Asset };

    void SkipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
    }

    bool Literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    // `plain` is false when the string used any escape sequence.
    bool String(std::string& out, bool& plain) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        plain = true;
        while (i_ < s_.size()) {
            const unsigned char ch = static_cast<unsigned char>(s_[i_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            plain = false;
            if (i_ >= s_.size()) return false;
            const char esc = s_[i_++];
            if (esc == 'u') {
                if (s_.size() - i_ < 4) return false;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[i_++];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                }
                out.push_back('?');
            } else if (std::string_view("\"\\/bfnrt").find(esc) != std::string_view::npos) {
                out.push_back('?');
            } else {
                return false;
            }
        }
        return false;
    }

    bool Number() {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        auto digits = [&] {
            const size_t from = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            return i_ > from;
        };
        if (!digits()) return false;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits()) return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digits()) return false;
        }
        return i_ > start;
    }

    bool Value(int depth, Mode mode) {
        if (depth > 32 || i_ >= s_.size()) return false;
        const char ch = s_[i_];
        if (ch == '{') return Object(depth, mode);
        if (ch == '[') return Array(depth, mode);
        if (ch == '"') {
            std::string ignored;
            bool plain = false;
            return String(ignored, plain);
        }
        if (ch == 't') return Literal("true");
        if (ch == 'f') return Literal("false");
        if (ch == 'n') return Literal("null");
        return Number();
    }

    bool Array(int depth, Mode mode) {
        ++i_; // [
        SkipWs();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            SkipWs();
            if (!Value(depth + 1, mode == Mode::Assets ? Mode::Asset : Mode::Other)) return false;
            SkipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    bool Object(int depth, Mode mode) {
        ++i_; // {
        SkipWs();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        // An asset counts only when this one element has both the exact name and
        // the matching digest (keys may come in either order, each at most once).
        int nameKeys = 0, digestKeys = 0;
        bool nameExact = false, digestMatches = false;
        auto finishAsset = [&] {
            if (mode == Mode::Asset && nameKeys == 1 && digestKeys == 1 && nameExact && digestMatches) found_ = true;
        };
        while (true) {
            SkipWs();
            std::string key;
            bool keyPlain = false;
            if (!String(key, keyPlain)) return false;
            SkipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            SkipWs();
            const bool plainKey = keyPlain;
            const bool stringValue = i_ < s_.size() && s_[i_] == '"';
            if (mode == Mode::Asset && plainKey && (key == "digest" || key == "name")) {
                std::string value;
                bool valuePlain = false;
                if (stringValue) {
                    if (!String(value, valuePlain)) return false;
                } else if (!Value(depth + 1, Mode::Other)) {
                    return false;
                }
                if (key == "name") {
                    ++nameKeys;
                    nameExact = stringValue && valuePlain && value == "LeanLauncher.exe";
                } else {
                    ++digestKeys;
                    static constexpr std::string_view kPrefix = "sha256:";
                    digestMatches = stringValue && valuePlain && value.size() == kPrefix.size() + 64 &&
                        _strnicmp(value.c_str(), kPrefix.data(), kPrefix.size()) == 0 &&
                        Sha256HexMatches(std::string_view(value).substr(kPrefix.size()), wanted_);
                }
            } else {
                Mode child = Mode::Other;
                if (mode == Mode::Root && plainKey && key == "assets") child = Mode::Assets;
                if (!Value(depth + 1, child)) return false;
            }
            SkipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                finishAsset();
                return true;
            }
            return false;
        }
    }

    std::string_view s_;
    std::string_view wanted_;
    size_t i_ = 0;
    bool found_ = false;
};

} // namespace detail

inline bool ReleaseJsonHasAssetDigest(std::string_view json, std::string_view sha256Hex) {
    return detail::ReleaseDigestScanner(json, sha256Hex).Run();
}

inline bool DeadlineExceeded(uint64_t startMs, uint64_t nowMs, uint64_t limitMs) {
    return nowMs < startMs || nowMs - startMs >= limitMs;
}

// Fetches the LATEST release JSON from the compile-time endpoint only (NFR-009:
// nothing from the registry, the command line, or the launcher's settings picks
// the host, path, or release). Bounded: 10 s per network step, 60 s in total,
// 1 MB body, no redirects.
inline bool FetchLatestReleaseJson(std::string& outJson) {
    outJson.clear();
    constexpr uint64_t kTotalLimitMs = 60000;
    const uint64_t startMs = GetTickCount64();
    const auto late = [&] { return DeadlineExceeded(startMs, GetTickCount64(), kTotalLimitMs); };
    HINTERNET session = WinHttpOpen(L"LeanLauncher/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool success = false;
    HINTERNET connect = WinHttpConnect(session, kDefaultApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kDefaultApiPath, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request) {
        const wchar_t headers[] = L"Accept: application/vnd.github.v3+json\r\n";
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!late() && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            !late() && WinHttpReceiveResponse(request, nullptr) && !late() &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200) {
            constexpr size_t kMaxBody = 1024 * 1024;
            bool complete = true;
            DWORD available = 0;
            while (true) {
                if (late() || !WinHttpQueryDataAvailable(request, &available)) {
                    complete = false;
                    break;
                }
                if (available == 0) break;
                if (outJson.size() + available > kMaxBody) {
                    complete = false;
                    break;
                }
                std::vector<char> buffer(available);
                DWORD got = 0;
                if (late() || !WinHttpReadData(request, buffer.data(), available, &got) || got == 0) {
                    complete = false;
                    break;
                }
                outJson.append(buffer.data(), got);
            }
            success = complete && !late() && !outJson.empty();
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!success) outJson.clear();
    return success;
}

// Outcome of "Restart to Update". Started: the update was swapped in place, or
// the elevated helper was launched (either way the launcher should exit).
enum class ApplyResult { Started, UacDeclined, NeedsFreshCheck, Failed, NotVerified };

enum class RestartReaction { ExitLauncher, StayWithMessage, StayMessageAndOpenReleases };

inline RestartReaction RestartReactionFor(ApplyResult result) {
    switch (result) {
    case ApplyResult::Started: return RestartReaction::ExitLauncher;
    case ApplyResult::UacDeclined:
    case ApplyResult::NeedsFreshCheck: return RestartReaction::StayWithMessage;
    case ApplyResult::Failed:
    case ApplyResult::NotVerified: break;
    }
    return RestartReaction::StayMessageAndOpenReleases;
}

inline const wchar_t* RestartMessageFor(ApplyResult result) {
    switch (result) {
    case ApplyResult::Started: return L"";
    case ApplyResult::UacDeclined:
        return L"The update needs administrator permission. Restart to Update again and choose Yes, "
               L"or download it from the releases page.";
    case ApplyResult::NeedsFreshCheck:
        return L"This update file has not been verified in this session. Use Check for updates in "
               L"Settings > About to download it again, then Restart to Update.";
    case ApplyResult::NotVerified:
        return L"The update could not be verified as the official release, so it was not installed. The releases "
               L"page will open so you can download it.";
    case ApplyResult::Failed: break;
    }
    return L"The update could not be installed. The releases page will open so you can download it.";
}

// `expectedSha256` is the hash of the bytes received when the update was
// downloaded. It is only needed for the elevated fallback; when it is not
// known the fallback is refused rather than trusting whatever file is on disk.
// `owner` is the launcher window, so the UAC prompt comes to the front.
inline ApplyResult ApplyUpdateAndRestart(const std::wstring& updateExePath, const std::string& expectedSha256,
                                         HWND owner) {
    if (!ValidateExecutableFile(updateExePath)) {
        return ApplyResult::Failed;
    }

    // US-040: the staged file sat on disk since the download, so check it again
    // right before it replaces the running exe - the hash pinned at download
    // time (when known) and the signed release hash and signature.
    std::string stagedSha256;
    if (!HashFileSha256(updateExePath, stagedSha256)) return ApplyResult::Failed;
    if (!expectedSha256.empty() && !Sha256HexMatches(expectedSha256, stagedSha256)) return ApplyResult::NotVerified;
    if (VerifyStagedUpdate(updateExePath, stagedSha256) != UpdateVerifyResult::Ok) return ApplyResult::NotVerified;

    wchar_t currentExe[MAX_PATH]{};
    const DWORD exeLength = GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
    if (exeLength == 0 || exeLength >= MAX_PATH) {
        return ApplyResult::Failed;
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
        return reinterpret_cast<INT_PTR>(inst) > 32 ? ApplyResult::Started : ApplyResult::Failed;
    }

    // 2. The install folder is not writable (for example Program Files). Re-run
    // this exe elevated in its apply-update mode: one UAC prompt, and the
    // helper re-checks the file against the hash verified at download time.
    if (!IsSha256Hex(expectedSha256)) {
        return ApplyResult::NeedsFreshCheck;
    }
    const std::wstring hash(expectedSha256.begin(), expectedSha256.end());
    const std::wstring parameters = L"--apply-update \"" + updateExePath + L"\" " + hash + L" " +
        std::to_wstring(GetCurrentProcessId());

    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = currentExeStr.c_str();
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        return ApplyResult::Started;
    }
    return GetLastError() == ERROR_CANCELLED ? ApplyResult::UacDeclined : ApplyResult::Failed;
}

// Files the download step leaves in %LOCALAPPDATA%\LeanLauncher\updates:
// LeanLauncher_v<tag>.exe, its .tmp, and the .sha256 / .minisig verification
// files staged beside it. Nothing else in that folder is touched.
inline bool IsStaleUpdateFileName(std::wstring_view name) {
    std::wstring lower(name);
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    const auto endsWith = [&](std::wstring_view suffix) {
        return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return lower.rfind(L"leanlauncher_v", 0) == 0 &&
        (endsWith(L".exe") || endsWith(L".exe.tmp") || endsWith(L".exe.sha256") || endsWith(L".exe.minisig"));
}

// Best effort and bounded. The elevated helper leaves the consumed update file
// alone (deleting it while elevated would follow a swapped junction), so the
// normal-user launcher removes it here. Only plain files are deleted.
inline void CleanupStaleUpdateFiles() {
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0 || !localAppData[0]) return;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher" / L"updates";
    int inspected = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && inspected < 64;
         it.increment(ec), ++inspected) {
        const std::filesystem::file_status status = it->symlink_status(ec);
        if (ec || status.type() != std::filesystem::file_type::regular) continue;
        if (IsStaleUpdateFileName(it->path().filename().wstring())) {
            DeleteFileW(it->path().c_str());
        }
    }
}

inline void CleanupOldUpdates() {
    wchar_t currentExe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH)) {
        const std::wstring oldExe = std::wstring(currentExe) + L".old";
        DeleteFileW(oldExe.c_str());
    }
    CleanupStaleUpdateFiles();
}

} // namespace takeoff

