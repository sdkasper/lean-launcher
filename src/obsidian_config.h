#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

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

}  // namespace obsidian
}  // namespace leanlauncher
