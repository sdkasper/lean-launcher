#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace leanlauncher {
namespace obsidian {

namespace fs = std::filesystem;

struct DailyNoteConfig {
    std::wstring folder;
    std::wstring format = L"YYYY-MM-DD";
    bool found = false;
};

// Recognizes "<prefix> <text>" (case-insensitive on the prefix, at least one
// non-whitespace character required after it). Leading spaces immediately
// after the prefix are trimmed; the rest of the input is used verbatim as
// the result text (not further parsed). Generic replacement for what used
// to be three hand-written, per-action copies of this same function - the
// prefix is now a runtime Settings value, not a compile-time constant.
inline bool TryParsePrefix(const std::wstring& input, const std::wstring& prefix, std::wstring& outText) {
    if (prefix.empty()) return false;
    const std::wstring fullPrefix = prefix + L" ";
    if (input.size() <= fullPrefix.size()) return false;
    if (_wcsnicmp(input.c_str(), fullPrefix.c_str(), fullPrefix.size()) != 0) return false;

    std::wstring rest = input.substr(fullPrefix.size());
    const size_t start = rest.find_first_not_of(L' ');
    if (start == std::wstring::npos) return false;

    outText = rest.substr(start);
    return !outText.empty();
}

// Migration-safe default for the master Obsidian toggle: an install that
// already has a vault configured keeps working with no action needed; a
// fresh install starts opted out until the user turns it on.
inline bool DefaultObsidianEnabled(const std::wstring& vaultPath) {
    return !vaultPath.empty();
}

// Returns a user-facing error message if the three configured action
// prefixes aren't all non-empty and mutually distinct (case-insensitive),
// or nullptr if they're valid. Used to reject an in-progress Settings edit
// before it's saved - two prefixes colliding would make one action
// permanently unreachable, and an empty prefix would match every input.
inline const wchar_t* FindPrefixConflict(
    const std::wstring& vaultSearchPrefix, const std::wstring& taskPrefix, const std::wstring& noteAddPrefix) {
    if (vaultSearchPrefix.empty() || taskPrefix.empty() || noteAddPrefix.empty()) {
        return L"Prefix cannot be empty.";
    }
    auto ciEqual = [](const std::wstring& a, const std::wstring& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };
    if (ciEqual(vaultSearchPrefix, taskPrefix) || ciEqual(vaultSearchPrefix, noteAddPrefix) ||
        ciEqual(taskPrefix, noteAddPrefix)) {
        return L"Prefixes must be unique.";
    }
    return nullptr;
}

// Reads a whole file as raw UTF-8 bytes. Returns an empty string if the file
// does not exist or cannot be read (never throws).
inline std::string ReadFileUtf8(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Parses one JSON string value starting at `pos` (which must point just past
// the field's key, e.g. right after `"folder"`). Advances `pos` past the
// closing quote. Handles `\"` and `\\` escapes; anything else (unicode
// escapes) is passed through raw, which is fine for the plain paths and
// folder names Obsidian's own config files contain.
inline bool ParseJsonStringAt(std::string_view json, size_t& pos, std::wstring& out) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
           json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string raw;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw.push_back(json[pos + 1]);
            pos += 2;
            continue;
        }
        raw.push_back(json[pos]);
        ++pos;
    }
    if (pos < json.size()) ++pos;  // consume closing quote
    if (raw.empty()) {
        out.clear();
        return true;
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0) return false;
    out.assign(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), out.data(), wlen);
    return true;
}

// Finds the first `"key":"value"` occurrence anywhere in `json` and returns
// its decoded value. Not a real JSON parser (no nesting awareness) - matches
// this codebase's existing ad-hoc parsing style in updates.h.
inline bool ExtractStringField(std::string_view json, std::string_view key, std::wstring& out) {
    const std::string keyPattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(keyPattern);
    if (pos == std::string_view::npos) return false;
    pos += keyPattern.size();
    return ParseJsonStringAt(json, pos, out);
}

// Finds every `"path":"..."` value in obsidian.json's `vaults` object.
inline std::vector<std::wstring> FindVaultPathsInJson(std::string_view json) {
    std::vector<std::wstring> paths;
    const std::string key = "\"path\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string_view::npos) {
        pos += key.size();
        std::wstring value;
        if (ParseJsonStringAt(json, pos, value) && !value.empty()) {
            paths.push_back(std::move(value));
        }
    }
    return paths;
}

// Parses a daily-notes.json-shaped object: {"folder": "...", "format": "..."}.
// `folder` may legitimately be absent (daily notes live at the vault root);
// `format` defaults to "YYYY-MM-DD" when absent. Returns found=false only
// when neither field is present (i.e. this isn't a daily-notes config at all).
inline DailyNoteConfig ParseDailyNoteConfigJson(std::string_view json) {
    DailyNoteConfig config;
    std::wstring folder;
    std::wstring format;
    const bool hasFolder = ExtractStringField(json, "folder", folder);
    const bool hasFormat = ExtractStringField(json, "format", format);
    if (!hasFolder && !hasFormat) {
        return config;  // found = false
    }
    config.folder = hasFolder ? folder : L"";
    config.format = (hasFormat && !format.empty()) ? format : L"YYYY-MM-DD";
    config.found = true;
    return config;
}

// Reads %APPDATA%\obsidian\obsidian.json and returns every known vault path
// that still exists on disk. Empty vector if Obsidian has never run, the
// config is unreadable, or no known vault paths still exist.
inline std::vector<std::wstring> FindKnownVaults() {
    PWSTR appDataPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appDataPath)) ||
        !appDataPath) {
        return {};
    }
    const fs::path configPath = fs::path(appDataPath) / L"obsidian" / L"obsidian.json";
    CoTaskMemFree(appDataPath);

    const std::string json = ReadFileUtf8(configPath);
    if (json.empty()) return {};

    std::vector<std::wstring> existing;
    for (auto& path : FindVaultPathsInJson(json)) {
        std::error_code ec;
        if (fs::exists(path, ec)) existing.push_back(std::move(path));
    }
    return existing;
}

// Reads the daily-notes plugin config for a vault, falling back to the
// Periodic Notes plugin's "daily" section if the core plugin's config is
// missing or unparseable. Returns found=false if neither is available -
// callers should then fall back to vault root + "YYYY-MM-DD.md".
inline DailyNoteConfig ReadDailyNoteConfig(const std::wstring& vaultPath) {
    const fs::path base(vaultPath);

    std::string json = ReadFileUtf8(base / L".obsidian" / L"daily-notes.json");
    if (!json.empty()) {
        DailyNoteConfig config = ParseDailyNoteConfigJson(json);
        if (config.found) return config;
    }

    json = ReadFileUtf8(base / L".obsidian" / L"plugins" / L"periodic-notes" / L"data.json");
    if (!json.empty()) {
        const size_t dailyPos = json.find("\"daily\"");
        if (dailyPos != std::string::npos) {
            DailyNoteConfig config = ParseDailyNoteConfigJson(std::string_view(json).substr(dailyPos));
            if (config.found) return config;
        }
    }

    return DailyNoteConfig{};  // found = false
}

// Wraps ReadDailyNoteConfig with an optional manual override for the
// folder and/or format, applied independently. found becomes true whenever
// either override is set, even if the vault's own plugin config couldn't
// be read - a user-supplied override is a complete answer on its own, not
// a fallback that still needs the "could not read config" warning.
inline DailyNoteConfig ResolveDailyNoteConfig(
    const std::wstring& vaultPath, const std::wstring& folderOverride, const std::wstring& formatOverride) {
    DailyNoteConfig config = ReadDailyNoteConfig(vaultPath);
    if (!folderOverride.empty()) {
        config.folder = folderOverride;
        config.found = true;
    }
    if (!formatOverride.empty()) {
        config.format = formatOverride;
        config.found = true;
    }
    return config;
}

// Quotes a single argument per the Windows command-line escaping rules
// CommandLineToArgvW (and CreateProcessW's argument parser) expect - not a
// raw string join, since a note title containing a literal '"' could
// otherwise inject an unintended argument boundary (e.g. splitting a
// vault=/path= pair). Arguments with no space/tab/quote pass through
// unquoted; everything else is wrapped in quotes with backslash-runs
// doubled before a literal quote or before the closing quote.
inline std::wstring QuoteCommandLineArgument(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out(1, L'"');
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

// Locates Obsidian's official CLI: %LOCALAPPDATA%\Obsidian\Obsidian.com
// first (Obsidian's standard non-portable Windows install location - the
// same "known fixed location, no user config" convention this file already
// trusts for %APPDATA%\obsidian\obsidian.json), falling back to a PATH
// search for portable/custom installs. Empty string if not found.
inline std::wstring FindObsidianCliPath() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) &&
        localAppData) {
        const fs::path candidate = fs::path(localAppData) / L"Obsidian" / L"Obsidian.com";
        CoTaskMemFree(localAppData);
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate.wstring();
    }
    wchar_t buf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"Obsidian.com", nullptr, MAX_PATH, buf, nullptr) > 0) {
        return buf;
    }
    return L"";
}

// Assembles the full CreateProcessW command line for
// "obsidian open vault=<name> path=<relative-file-path>", quoting each
// argument as needed. Kept separate from OpenNoteInObsidian so the
// assembly logic is unit-testable without spawning a process.
inline std::wstring BuildObsidianCliCommandLine(
    const std::wstring& cliPath, const std::wstring& vaultName, const std::wstring& relativeFilePath) {
    return QuoteCommandLineArgument(cliPath) + L" vault=" + QuoteCommandLineArgument(vaultName) +
           L" open path=" + QuoteCommandLineArgument(relativeFilePath);
}

// Posted back to the launcher window when a background note-open attempt
// finishes; wParam is 1 on success, 0 on failure. WM_APP + 8 and + 9 are
// taken by kFilesReadyMessage and kNotesReadyMessage, + 1 through + 7 by
// main.cpp's own constants.
constexpr UINT kNoteOpenResultMessage = WM_APP + 10;

// Posted back to the launcher window when a background FindKnownVaults()
// scan finishes; lParam is a heap-allocated std::vector<std::wstring>* that
// the handler must take ownership of and delete. Needed because
// FindKnownVaults() calls fs::exists() on every known vault path - on the
// UI thread, one unreachable network-drive vault would stall OpenSettings()
// (and with it the global hotkey and tray icon) for the OS I/O timeout.
constexpr UINT kKnownVaultsReadyMessage = WM_APP + 11;

// Opens a note in Obsidian via its official CLI - no community plugin
// dependency, no hand-rolled URI scheme. Returns false if the CLI couldn't
// be located or CreateProcessW couldn't spawn it; this is a launch-dispatch
// failure, not a guarantee Obsidian found the note. Blocks for as long as
// the CLI takes to answer (seconds, on a cold Obsidian start), so callers
// must run it off the UI thread.
inline bool OpenNoteInObsidian(const std::wstring& vaultPath, const std::wstring& relativeNoteRef) {
    const std::wstring cliPath = FindObsidianCliPath();
    if (cliPath.empty()) return false;

    const std::wstring vaultName = fs::path(vaultPath).filename().wstring();
    const std::wstring relativeFilePath = relativeNoteRef + L".md";
    const std::wstring commandLine = BuildObsidianCliCommandLine(cliPath, vaultName, relativeFilePath);

    SECURITY_ATTRIBUTES saAttr{};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) return false;
    if (!SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // CreateProcessW may modify its command-line buffer in place - it must
    // be a mutable buffer, not a string literal or a temporary's c_str().
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL spawned = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // The child inherits its own copy of the write end; the parent's copy
    // must be closed immediately so ReadFile below sees EOF once the child
    // exits, rather than blocking forever waiting for a write end that's
    // still (uselessly) open in this process too.
    CloseHandle(hWritePipe);
    if (!spawned) {
        CloseHandle(hReadPipe);
        return false;
    }

    // Drain the pipe to EOF before waiting on the process, not after: a child
    // that wrote more than the pipe buffer holds would block forever on its
    // own write if this waited for exit first. EOF arrives when the child
    // closes its end, so this loop ends when the child does.
    std::string output;
    char buf[512];
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buf, bytesRead);
        if (output.size() > 4096) break;
    }
    CloseHandle(hReadPipe);

    // Bounded wait, not an indefinite block: per Obsidian's own CLI docs,
    // "If Obsidian is not running, the first command you run launches
    // Obsidian" - a cold start can take a few seconds.
    constexpr DWORD kTimeoutMs = 10000;
    bool succeeded = false;
    if (WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_OBJECT_0) {
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        // stderr is merged into the same pipe as stdout, so anything the CLI
        // warns about first would push "Opened:" off position 0 - match it
        // anywhere in the output rather than only at the start.
        succeeded = exitCode == 0 && output.find("Opened:") != std::string::npos;
    } else {
        // Timed out (or wait failed) - don't leave a stuck process running
        // detached with no result.
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return succeeded;
}

// Converts a vault-relative note ref (forward slashes, no extension, as
// produced by NoteIndex) back into an absolute filesystem path, for
// operations that need one directly (e.g. revealing the file in Explorer).
inline std::wstring ResolveNoteAbsolutePath(const std::wstring& vaultPath, const std::wstring& relativeNoteRef) {
    std::wstring relBackslash(relativeNoteRef);
    std::replace(relBackslash.begin(), relBackslash.end(), L'/', L'\\');
    return (fs::path(vaultPath) / (relBackslash + L".md")).wstring();
}

}  // namespace obsidian
}  // namespace leanlauncher
