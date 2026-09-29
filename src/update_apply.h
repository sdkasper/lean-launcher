#pragma once

// The elevated update helper. When the launcher cannot replace its own exe (for
// example it is installed under Program Files and runs unelevated), it re-runs
// itself elevated with:  --apply-update "<update exe>" <sha256> <launcher pid>
// This mode re-checks the update file against the hash the launcher pinned when
// it downloaded the file, swaps the exe with bounded retries, and starts the
// launcher again as the normal user. It never creates a window or the launcher's
// single-instance mutex. Every input that is not exactly what the launcher
// sends is refused with a message rather than interpreted.

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "sha256.h"
#include "updates.h"

namespace takeoff {

inline constexpr wchar_t kApplyUpdateSwitch[] = L"--apply-update";

struct ApplyUpdateRequest {
    std::wstring updatePath;
    std::string sha256; // lowercase hex
    DWORD pid = 0;
};

inline bool IsApplyUpdateSwitch(std::wstring_view arg) {
    return _wcsicmp(std::wstring(arg).c_str(), kApplyUpdateSwitch) == 0;
}

namespace detail {

inline std::wstring LowerW(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

// An absolute drive-letter path (never UNC or device syntax), no control or
// wildcard characters, no ".." component, and a file name that looks like a
// Lean Launcher release: LeanLauncher*.exe.
inline bool IsAcceptableUpdatePath(const std::wstring& path) {
    if (path.size() < 8 || path.size() > 1024) return false;
    const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    if (!drive) return false;
    for (size_t k = 0; k < path.size(); ++k) {
        const wchar_t ch = path[k];
        if (ch == L':' && k >= 2) return false; // alternate data streams
        if (ch < L' ' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || ch == L'*' || ch == L'?') {
            return false;
        }
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = path.size();
        if (path.compare(start, end - start, L"..") == 0) return false;
        start = end + 1;
    }
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = LowerW(std::wstring_view(path).substr(slash + 1));
    static constexpr std::wstring_view kPrefix = L"leanlauncher";
    static constexpr std::wstring_view kSuffix = L".exe";
    return name.size() >= kPrefix.size() + kSuffix.size() &&
        name.compare(0, kPrefix.size(), kPrefix) == 0 &&
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

} // namespace detail

// `args` is argv without the exe name. Exactly: --apply-update, path, hash, pid.
inline std::optional<ApplyUpdateRequest> ParseApplyUpdateArgs(const std::vector<std::wstring>& args) {
    if (args.size() != 4 || !IsApplyUpdateSwitch(args[0])) return std::nullopt;
    if (!detail::IsAcceptableUpdatePath(args[1])) return std::nullopt;

    const std::wstring& hash = args[2];
    if (hash.size() != 64) return std::nullopt;
    std::string narrowHash;
    for (wchar_t ch : hash) {
        if (ch > 0x7F) return std::nullopt;
        narrowHash.push_back(static_cast<char>(ch));
    }
    if (!IsSha256Hex(narrowHash)) return std::nullopt;
    for (char& ch : narrowHash) {
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch + 32);
    }

    const std::wstring& pidText = args[3];
    if (pidText.empty() || pidText.size() > 10) return std::nullopt;
    unsigned long long pid = 0;
    for (wchar_t ch : pidText) {
        if (ch < L'0' || ch > L'9') return std::nullopt;
        pid = pid * 10 + static_cast<unsigned long long>(ch - L'0');
    }
    if (pid == 0 || pid > 0xFFFFFFFFull) return std::nullopt;

    ApplyUpdateRequest request;
    request.updatePath = args[1];
    request.sha256 = std::move(narrowHash);
    request.pid = static_cast<DWORD>(pid);
    return request;
}

// ---- Win32-only part (not unit tested; see the manual test plan) ----------

namespace detail {

inline constexpr uint64_t kMaxUpdateBytes = 64ull * 1024 * 1024;

inline void ShowApplyError(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"Lean Launcher update",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FileHandle() { Close(); }
    void Close() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
};

// Wait for the launcher that asked for the update. Already gone is fine; still
// running after the timeout is a failure (it is never killed).
inline bool WaitForLauncherExit(DWORD pid, std::wstring& error) {
    if (pid == GetCurrentProcessId()) {
        error = L"Update failed: invalid request.";
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return true; // no such process
        error = L"Update failed: could not check whether Lean Launcher has closed.";
        return false;
    }
    const DWORD waited = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    if (waited != WAIT_OBJECT_0) {
        error = L"Update failed: Lean Launcher did not close in time. Close it and try Restart to Update again.";
        return false;
    }
    return true;
}

// Open the update file denying writers and deletes, and read it whole.
inline bool ReadUpdateFile(const std::wstring& path, FileHandle& file, std::vector<uint8_t>& bytes,
                           std::wstring& error) {
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) {
        error = L"Update failed: the downloaded file could not be opened.";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        error = L"Update failed: invalid request.";
        return false;
    }
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size == 0 || size > kMaxUpdateBytes) {
        error = L"Update failed: the downloaded file has an unexpected size.";
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        if (!ReadFile(file.handle, bytes.data() + done, want, &got, nullptr) || got == 0) {
            error = L"Update failed: the downloaded file could not be read.";
            return false;
        }
        done += got;
    }
    return true;
}

inline bool WriteWholeFileNew(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        ok = WriteFile(out, bytes.data() + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    if (ok) ok = FlushFileBuffers(out) != FALSE;
    CloseHandle(out);
    return ok;
}

// Rename the running exe aside, write the verified bytes in its place, and
// roll back on any failure.
inline bool SwapExe(const std::wstring& target, const std::vector<uint8_t>& bytes, std::wstring& error) {
    const std::wstring backup = target + L".old";
    bool renamed = false;
    for (int attempt = 0; attempt < 5 && !renamed; ++attempt) {
        DeleteFileW(backup.c_str());
        renamed = MoveFileExW(target.c_str(), backup.c_str(), 0) != FALSE;
        if (!renamed) Sleep(200);
    }
    if (!renamed) {
        error = L"Update failed: the current Lean Launcher file could not be renamed.";
        return false;
    }
    if (!WriteWholeFileNew(target, bytes)) {
        DeleteFileW(target.c_str());
        if (!MoveFileExW(backup.c_str(), target.c_str(), 0)) {
            error = L"Update failed and the previous version could not be restored. It is saved as:\n" + backup;
        } else {
            error = L"Update failed: the new file could not be written. The previous version was restored.";
        }
        return false;
    }
    return true;
}

inline bool RelaunchAsNormalUser(const std::wstring& target) {
    // explorer.exe starts the exe with the desktop user's normal token, so the
    // launcher does not keep running elevated.
    const std::wstring quoted = L"\"" + target + L"\"";
    HINSTANCE inst = ShellExecuteW(nullptr, L"open", L"explorer.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(inst) > 32;
}

} // namespace detail

// Returns 0 success, 1 failure, 2 bad arguments. `args` is argv without the exe
// name. Whatever happens, the installed launcher (the new one, or the rolled
// back old one) is started again as the normal user.
inline int RunApplyUpdateMode(const std::vector<std::wstring>& args) {
    std::vector<wchar_t> exeBuffer(32768);
    const DWORD exeLength = GetModuleFileNameW(nullptr, exeBuffer.data(), static_cast<DWORD>(exeBuffer.size()));
    const std::wstring target = (exeLength != 0 && exeLength < exeBuffer.size())
        ? std::wstring(exeBuffer.data(), exeLength) : std::wstring();

    // Shows the single failure message, then restarts the launcher.
    auto fail = [&](const std::wstring& message, int code) {
        detail::ShowApplyError(message);
        if (!target.empty()) {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            detail::RelaunchAsNormalUser(target);
        }
        return code;
    };

    if (target.empty()) {
        detail::ShowApplyError(L"Update failed: could not find the Lean Launcher file.");
        return 1;
    }
    const auto request = ParseApplyUpdateArgs(args);
    if (!request || _wcsicmp(target.c_str(), request->updatePath.c_str()) == 0) {
        return fail(L"Update failed: invalid request.", 2);
    }

    std::wstring error;
    detail::FileHandle updateFile; // held (writers denied) until the swap is done
    std::vector<uint8_t> bytes;
    if (!detail::WaitForLauncherExit(request->pid, error) ||
        !detail::ReadUpdateFile(request->updatePath, updateFile, bytes, error)) {
        return fail(error, 1);
    }
    const std::string actualHash = Sha256Hex(bytes.data(), bytes.size());
    if (!Sha256HexMatches(request->sha256, actualHash)) {
        return fail(L"Update failed: the downloaded file does not match its checksum.", 1);
    }
    if (!ValidateExecutableBuffer(bytes.data(), bytes.size())) {
        return fail(L"Update failed: the downloaded file is not a valid Lean Launcher program.", 1);
    }
    // Confirm the exact bytes about to be installed with the published release.
    // Fail closed: any doubt means nothing is installed.
    std::string releaseJson;
    if (!FetchLatestReleaseJson(releaseJson)) {
        return fail(L"Update failed: could not confirm the update with GitHub. Check your internet connection "
                    L"and try Restart to Update again.", 1);
    }
    if (!ReleaseJsonHasAssetDigest(releaseJson, actualHash)) {
        return fail(L"Update failed: the update does not match the published release.", 1);
    }
    if (!detail::SwapExe(target, bytes, error)) {
        return fail(error, 1);
    }
    updateFile.Close();

    // A stale .old goes away now, or at the next reboot (both best effort). The
    // consumed update file is left for the normal-user launcher to clean up.
    const std::wstring backup = target + L".old";
    if (!DeleteFileW(backup.c_str())) {
        MoveFileExW(backup.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!detail::RelaunchAsNormalUser(target)) {
        detail::ShowApplyError(L"Lean Launcher was updated but could not be restarted. Start it from the Start menu.");
        return 1;
    }
    return 0;
}

} // namespace takeoff
