#pragma once

#include "search.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <shlobj.h>

namespace takeoff {

namespace fs = std::filesystem;

constexpr UINT kFilesReadyMessage = WM_APP + 8;

struct FileItem {
    std::wstring name;
    std::wstring normName;
    std::wstring path;
    std::wstring normPath;
    bool isDirectory = false;
};

struct FileSearchResult {
    std::wstring name;
    std::wstring path;
    bool isDirectory = false;
    int score = 0;
};

struct IndexChunk {
    std::vector<FileItem> items;
};

struct IndexSnapshot {
    std::vector<std::shared_ptr<const IndexChunk>> chunks;
    size_t totalCount = 0;
};

inline uint64_t Fnv1a64(std::wstring_view sv) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : sv) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

class FileIndex {
public:
    static FileIndex& Instance() {
        static FileIndex s_instance;
        return s_instance;
    }

    ~FileIndex() { Stop(); }

    void Start(HWND notifyHwnd = nullptr) {
        if (running_.exchange(true)) return;
        notifyHwnd_ = notifyHwnd;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        triggerEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        worker_ = std::thread([this]() { WorkerLoop(); });
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (stopEvent_) SetEvent(stopEvent_);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        if (triggerEvent_) {
            CloseHandle(triggerEvent_);
            triggerEvent_ = nullptr;
        }
    }

    void TriggerReindex() {
        if (triggerEvent_) SetEvent(triggerEvent_);
    }

    bool IsReady() const {
        return ready_.load();
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_ ? snapshot_->totalCount : 0;
    }

    void PublishSnapshot(std::vector<FileItem>&& items) {
        if (items.empty()) return;
        auto chunk = std::make_shared<const IndexChunk>(IndexChunk{std::move(items)});
        auto newSnapshot = std::make_shared<IndexSnapshot>();
        newSnapshot->totalCount = chunk->items.size();
        newSnapshot->chunks.push_back(std::move(chunk));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = std::move(newSnapshot);
            ready_ = true;
        }
    }

    void PublishSnapshot(const std::vector<FileItem>& items) {
        std::vector<FileItem> copy = items;
        PublishSnapshot(std::move(copy));
    }

    void AppendSnapshotChunk(std::vector<FileItem>&& items) {
        if (items.empty()) return;
        auto chunk = std::make_shared<const IndexChunk>(IndexChunk{std::move(items)});
        const size_t chunkSize = chunk->items.size();

        std::shared_ptr<IndexSnapshot> newSnapshot = std::make_shared<IndexSnapshot>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_) {
                newSnapshot->chunks = snapshot_->chunks;
                newSnapshot->totalCount = snapshot_->totalCount;
            }
            newSnapshot->totalCount += chunkSize;
            newSnapshot->chunks.push_back(std::move(chunk));
            snapshot_ = std::move(newSnapshot);
            ready_ = true;
        }
    }

    void AppendSnapshotChunk(const std::vector<FileItem>& items) {
        std::vector<FileItem> copy = items;
        AppendSnapshotChunk(std::move(copy));
    }

    static bool IsDriveRoot(const fs::path& p) {
        if (p.empty()) return true;
        fs::path root = p.root_path();
        if (!root.empty() && (p == root || p.parent_path() == p)) {
            return true;
        }
        std::wstring s = p.wstring();
        if (s.size() == 2 && s[1] == L':') return true;
        if (s.size() == 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) return true;
        if (s == L"\\" || s == L"/") return true;
        return false;
    }

    static bool ShouldSkipDirectory(const fs::path& dirPath) {
        if (dirPath.empty()) return false;

        wchar_t winDirBuf[MAX_PATH]{};
        if (GetWindowsDirectoryW(winDirBuf, MAX_PATH) > 0) {
            std::wstring pStr = dirPath.wstring();
            std::wstring wStr = winDirBuf;
            std::transform(pStr.begin(), pStr.end(), pStr.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            std::transform(wStr.begin(), wStr.end(), wStr.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            std::replace(pStr.begin(), pStr.end(), L'/', L'\\');
            std::replace(wStr.begin(), wStr.end(), L'/', L'\\');
            while (!wStr.empty() && wStr.back() == L'\\') wStr.pop_back();
            if (pStr == wStr || (pStr.rfind(wStr, 0) == 0 && pStr.size() > wStr.size() && pStr[wStr.size()] == L'\\')) {
                return true;
            }
        }

        for (const auto& part : dirPath) {
            std::wstring seg = part.wstring();
            if (seg.empty() || seg == L"." || seg == L"..") continue;
            if (seg.size() > 1 && seg[0] == L'.') return true;

            std::wstring lower = seg;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

            while (!lower.empty() && (lower.back() == L'\\' || lower.back() == L'/')) {
                lower.pop_back();
            }

            if (lower == L"node_modules" || lower == L"appdata" || lower == L"packages" ||
                lower == L"package cache" || lower == L"temp" || lower == L"tmp" ||
                lower == L"bin" || lower == L"obj" || lower == L"build" || lower == L"target" ||
                lower == L"dist" || lower == L".git" || lower == L".vs" || lower == L".idea" ||
                lower == L"recovery" || lower == L"$recycle.bin" || lower == L"system volume information" ||
                lower == L"crashdumps" || lower == L"windows" || lower == L"program files" ||
                lower == L"program files (x86)" || lower == L"programdata" || lower == L"perflogs" ||
                lower == L"hostedtoolcache" || lower == L"actions-runner" || lower == L"actions" ||
                lower == L"vcpkg" || lower == L"msys64" || lower == L"msys32" || lower == L"chocolatey" ||
                lower == L"tools" || lower == L"miniconda" || lower == L"miniconda3" ||
                lower == L"anaconda" || lower == L"anaconda3" || lower == L"venv" || lower == L"virtualenvs" ||
                lower == L"winsxs" || lower == L"servicing" || lower == L"assembly" ||
                lower == L"catroot" || lower == L"catroot2" ||
                lower == L"driverstore" || lower == L"drivers" || lower == L"filerepository" || lower == L"hostdriverstore" ||
                lower == L"system32" || lower == L"syswow64" || lower == L"sysnative" ||
                lower == L"windows.old" || lower == L"$windows.~bt" || lower == L"$windows.~ws" || lower == L"$winreagent" ||
                lower == L"windowsapps" || lower == L"softwaredistribution" ||
                lower == L"systemresources" || lower == L"rescache" || lower == L"config.msi" ||
                lower == L"msapps" || lower == L"common files" || lower == L"inf") {
                return true;
            }
        }
        return false;
    }

    static bool HasRepositoryMarkers(const fs::path& dirPath) {
        std::error_code ec;
        if (dirPath.empty() || !fs::is_directory(dirPath, ec)) return false;

        if (fs::exists(dirPath / L".git", ec)) return true;
        if (fs::exists(dirPath / L"CMakeLists.txt", ec)) return true;
        if (fs::exists(dirPath / L"package.json", ec)) return true;
        if (fs::exists(dirPath / L"Cargo.toml", ec)) return true;

        fs::directory_iterator dit(dirPath, fs::directory_options::skip_permission_denied, ec);
        if (!ec) {
            for (const auto& entry : dit) {
                if (entry.is_regular_file(ec)) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                    if (ext == L".sln" || ext == L".vcxproj") {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    static fs::path FindVerifiedProjectRoot(const fs::path& startDir) {
        if (startDir.empty() || IsDriveRoot(startDir)) return {};

        wchar_t winDirBuf[MAX_PATH]{};
        if (GetWindowsDirectoryW(winDirBuf, MAX_PATH) > 0) {
            std::wstring pStr = startDir.wstring();
            std::wstring wStr = winDirBuf;
            std::transform(pStr.begin(), pStr.end(), pStr.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            std::transform(wStr.begin(), wStr.end(), wStr.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            std::replace(pStr.begin(), pStr.end(), L'/', L'\\');
            std::replace(wStr.begin(), wStr.end(), L'/', L'\\');
            while (!wStr.empty() && wStr.back() == L'\\') wStr.pop_back();
            if (pStr == wStr || (pStr.rfind(wStr, 0) == 0 && pStr.size() > wStr.size() && pStr[wStr.size()] == L'\\')) {
                return {};
            }
        }

        for (const auto& part : startDir) {
            std::wstring seg = part.wstring();
            std::wstring lower = seg;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            while (!lower.empty() && (lower.back() == L'\\' || lower.back() == L'/')) lower.pop_back();
            if (lower == L"windows" || lower == L"system32" || lower == L"syswow64" ||
                lower == L"catroot" || lower == L"catroot2" || lower == L"driverstore" ||
                lower == L"winsxs" || lower == L"program files" || lower == L"program files (x86)" ||
                lower == L"programdata" || lower == L"assembly" || lower == L"servicing" ||
                lower == L"windows.old" || lower == L"$windows.~bt") {
                return {};
            }
        }

        fs::path curr = startDir;

        while (curr.has_parent_path()) {
            std::wstring name = curr.filename().wstring();
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (lower == L"build" || lower == L"release" || lower == L"debug" ||
                lower == L"bin" || lower == L"obj" || lower == L"out" ||
                lower == L"target" || lower == L"dist" || lower == L"x64" || lower == L"x86") {
                curr = curr.parent_path();
            } else {
                break;
            }
        }

        for (int depth = 0; depth < 5; ++depth) {
            if (curr.empty() || IsDriveRoot(curr)) break;
            if (HasRepositoryMarkers(curr)) {
                return curr;
            }
            if (!curr.has_parent_path()) break;
            fs::path parent = curr.parent_path();
            if (parent == curr) break;
            curr = parent;
        }

        return {};
    }

    static bool IsUserRelevantFile(const fs::path& filePath) {
        const std::wstring ext = filePath.extension().wstring();
        if (ext.empty()) {
            const std::wstring filename = filePath.filename().wstring();
            std::wstring lowerName;
            lowerName.reserve(filename.size());
            for (wchar_t c : filename) lowerName.push_back(static_cast<wchar_t>(towlower(c)));
            return (lowerName == L"makefile" || lowerName == L"dockerfile" ||
                    lowerName == L"license" || lowerName == L"readme");
        }

        std::wstring lowerExt;
        lowerExt.reserve(ext.size());
        for (wchar_t c : ext) lowerExt.push_back(static_cast<wchar_t>(towlower(c)));

        static const std::unordered_set<std::wstring_view> kAllowedExtensions = {
            // Documents & Office
            L".pdf", L".doc", L".docx", L".docm", L".dot", L".dotx", L".odt", L".rtf", L".wps",
            L".xls", L".xlsx", L".xlsm", L".xlsb", L".xlt", L".xltx", L".ods", L".csv", L".tsv",
            L".ppt", L".pptx", L".pptm", L".pot", L".potx", L".odp",
            L".epub", L".mobi", L".azw", L".azw3", L".djvu",
            L".txt", L".md", L".markdown", L".rst", L".tex",
            // Media - Images
            L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg", L".ico",
            L".tiff", L".tif", L".psd", L".ai", L".raw", L".heic", L".avif",
            // Media - Audio
            L".mp3", L".wav", L".flac", L".m4a", L".aac", L".ogg", L".wma", L".mid", L".midi", L".opus",
            // Media - Video
            L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".flv", L".webm", L".m4v", L".mpg", L".mpeg",
            // Archives
            L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz", L".iso", L".cab", L".tgz",
            // Code & Development
            L".c", L".cpp", L".cxx", L".cc", L".h", L".hpp", L".hxx", L".inl", L".rc",
            L".cs", L".fs", L".vb", L".sln", L".vcxproj", L".csproj", L".fsproj", L".props", L".targets",
            L".java", L".kt", L".kts", L".scala", L".gradle",
            L".html", L".htm", L".css", L".scss", L".sass", L".less",
            L".js", L".mjs", L".cjs", L".ts", L".tsx", L".jsx", L".vue", L".svelte",
            L".py", L".pyw", L".rb", L".php", L".pl", L".pm",
            L".rs", L".go", L".swift", L".dart", L".lua",
            L".json", L".jsonc", L".xml", L".yaml", L".yml", L".toml", L".ini", L".cfg", L".conf",
            L".env", L".sql", L".proto", L".cmake",
            L".sh", L".bash", L".zsh", L".ps1", L".psm1", L".bat", L".cmd",
            L".asm", L".s",
            // Executables & Shortcuts
            L".exe", L".lnk", L".url", L".appref-ms", L".msi"
        };

        return kAllowedExtensions.find(std::wstring_view(lowerExt)) != kAllowedExtensions.end();
    }

    // Ultra-fast in-memory search across filenames and directory paths
    std::vector<FileSearchResult> Search(std::wstring_view query, size_t maxResults = 30) const {
        if (query.empty()) return {};

        std::shared_ptr<const IndexSnapshot> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
        }
        if (!snapshot || snapshot->chunks.empty()) return {};

        const std::wstring normQuery = Normalize(query);
        if (normQuery.empty()) return {};

        // Parse query tokens
        std::vector<std::wstring_view> tokens;
        size_t tStart = 0;
        while (tStart < normQuery.size()) {
            size_t tEnd = normQuery.find(L' ', tStart);
            if (tEnd == std::wstring::npos) tEnd = normQuery.size();
            if (tEnd > tStart) {
                tokens.push_back(std::wstring_view(normQuery.data() + tStart, tEnd - tStart));
            }
            tStart = tEnd + 1;
        }

        struct Candidate {
            int score;
            const FileItem* item;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(128);

        const wchar_t firstChar = normQuery[0];
        const size_t qLen = normQuery.size();
        const bool isSingleToken = (tokens.size() <= 1);
        const bool hasPathSep = (query.find(L'/') != std::wstring_view::npos ||
                                 query.find(L'\\') != std::wstring_view::npos ||
                                 query.find(L':') != std::wstring_view::npos);
        const bool allowPathMatch = hasPathSep || (tokens.size() > 1) || (normQuery.size() >= 3);

        for (const auto& chunk : snapshot->chunks) {
            for (const auto& item : chunk->items) {
                int s = -1;

                // 1. Primary match: check if query matches the file/folder name directly
                if (item.normName.size() >= (isSingleToken ? qLen : tokens.back().size())) {
                    if (item.normName.find(firstChar) != std::wstring::npos) {
                        s = ScoreFile(item.normName, normQuery, item.isDirectory);
                    }
                }

                // 2. Secondary match: path / parent directory match
                if (s <= 0 && allowPathMatch) {
                    if (isSingleToken) {
                        // Contiguous substring in path (e.g. folder name in path)
                        size_t pos = item.normPath.find(normQuery);
                        if (pos != std::wstring::npos) {
                            const size_t penalty = (std::min)(item.normPath.size() / 4, size_t{300});
                            int pathScore = 2600 - static_cast<int>(penalty);
                            if (item.isDirectory) pathScore += 40;
                            s = (std::max)(1000, pathScore);
                        }
                    } else {
                        // Multi-token match: all tokens must appear in normPath
                        bool allFound = true;
                        for (const auto& token : tokens) {
                            if (item.normPath.find(token) == std::wstring::npos) {
                                allFound = false;
                                break;
                            }
                        }
                        if (allFound) {
                            const bool lastMatchesName = (item.normName.find(tokens.back()) != std::wstring::npos);
                            const size_t penalty = (std::min)(item.normPath.size() / 4, size_t{300});
                            int tokenScore = 2400 + (lastMatchesName ? 600 : 0) - static_cast<int>(penalty);
                            if (item.isDirectory) tokenScore += 40;
                            s = (std::max)(1000, tokenScore);
                        }
                    }
                }

                if (s > 0) {
                    candidates.push_back({s, &item});
                }
            }
        }

        if (candidates.empty()) return {};

        const size_t count = (std::min)(maxResults, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.score > b.score;
            });

        std::vector<FileSearchResult> results;
        results.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            results.push_back({
                candidates[i].item->name,
                candidates[i].item->path,
                candidates[i].item->isDirectory,
                candidates[i].score
            });
        }
        return results;
    }

private:
    FileIndex() = default;

    void AddItem(const fs::path& p, bool isDir, std::vector<FileItem>& items, std::unordered_set<uint64_t>& seen) {
        std::wstring name = p.filename().wstring();
        if (name.empty()) {
            name = p.wstring();
            if (name.empty()) return;
        }
        if (!isDir) {
            if (name[0] == L'.' || name[0] == L'~') return;
            if (!IsUserRelevantFile(p)) return;
        }
        std::wstring fullPath = p.wstring();
        std::wstring normPath = Normalize(fullPath);
        uint64_t pathHash = Fnv1a64(normPath);
        if (!seen.insert(pathHash).second) {
            return;
        }
        std::wstring norm = Normalize(name);
        items.push_back({std::move(name), std::move(norm), std::move(fullPath), std::move(normPath), isDir});
    }

    void ScanPath(const fs::path& root, std::vector<FileItem>& items, std::unordered_set<uint64_t>& seen, int maxDepth, size_t maxCount, size_t totalCountSoFar = 0) {
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        if (IsDriveRoot(root) || ShouldSkipDirectory(root)) return;

        try {
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;

            while (it != end && !ec) {
                if (!running_.load()) return;
                if (totalCountSoFar + items.size() >= maxCount) return;

                const auto& entry = *it;
                bool isDir = entry.is_directory(ec);
                if (!ec && isDir) {
                    if (it.depth() >= maxDepth || ShouldSkipDirectory(entry.path())) {
                        it.disable_recursion_pending();
                    } else {
                        AddItem(entry.path(), true, items, seen);
                    }
                    it.increment(ec);
                    continue;
                }

                if (!ec && entry.is_regular_file(ec)) {
                    if (IsUserRelevantFile(entry.path())) {
                        AddItem(entry.path(), false, items, seen);
                    }
                }
                it.increment(ec);
            }
        } catch (...) {}
    }

    void BuildIndex() {
        constexpr size_t kMaxFiles = 50000;
        std::unordered_set<uint64_t> seen;
        seen.reserve(kMaxFiles);

        bool isFirstChunk = true;
        auto publishOrAppend = [this, &isFirstChunk](std::vector<FileItem>&& chunkItems) {
            if (chunkItems.empty()) return;
            if (isFirstChunk) {
                PublishSnapshot(std::move(chunkItems));
                isFirstChunk = false;
            } else {
                AppendSnapshotChunk(std::move(chunkItems));
            }
        };

        size_t totalIndexed = 0;

        // 0. Scan verified user project/repo root immediately (if any)
        std::error_code ec;
        fs::path currentDir = fs::current_path(ec);
        if (!ec && !currentDir.empty()) {
            fs::path repoDir = FindVerifiedProjectRoot(currentDir);
            if (!repoDir.empty()) {
                std::vector<FileItem> step0Items;
                step0Items.reserve(2048);
                AddItem(repoDir, true, step0Items, seen);
                ScanPath(repoDir, step0Items, seen, 6, kMaxFiles, totalIndexed);
                totalIndexed += step0Items.size();
                publishOrAppend(std::move(step0Items));
            }
        }

        // 1. Scan primary user folders (Desktop, Documents, Downloads, Pictures, Music, Videos)
        const KNOWNFOLDERID userFolders[] = {
            FOLDERID_Desktop,
            FOLDERID_Documents,
            FOLDERID_Downloads,
            FOLDERID_Pictures,
            FOLDERID_Music,
            FOLDERID_Videos,
        };

        std::vector<FileItem> step1Items;
        step1Items.reserve(8192);
        for (const auto& kfid : userFolders) {
            if (!running_.load() || totalIndexed + step1Items.size() >= kMaxFiles) break;
            PWSTR folderPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(kfid, KF_FLAG_DEFAULT, nullptr, &folderPath)) && folderPath) {
                AddItem(folderPath, true, step1Items, seen);
                ScanPath(folderPath, step1Items, seen, 8, kMaxFiles, totalIndexed);
                CoTaskMemFree(folderPath);
            }
        }
        totalIndexed += step1Items.size();
        publishOrAppend(std::move(step1Items));

        // 2. Scan %USERPROFILE% roots (e.g. source code directories, projects, etc.)
        PWSTR profilePath = nullptr;
        if (running_.load() && totalIndexed < kMaxFiles &&
            SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profilePath)) && profilePath) {
            std::vector<FileItem> step2Items;
            step2Items.reserve(8192);
            fs::directory_iterator dit(profilePath, fs::directory_options::skip_permission_denied, ec);
            for (const auto& entry : dit) {
                if (!running_.load() || totalIndexed + step2Items.size() >= kMaxFiles) break;
                if (entry.is_directory(ec)) {
                    std::wstring name = entry.path().filename().wstring();
                    if (!ShouldSkipDirectory(entry.path()) &&
                        _wcsicmp(name.c_str(), L"Desktop") != 0 &&
                        _wcsicmp(name.c_str(), L"Documents") != 0 &&
                        _wcsicmp(name.c_str(), L"Downloads") != 0 &&
                        _wcsicmp(name.c_str(), L"Pictures") != 0 &&
                        _wcsicmp(name.c_str(), L"Music") != 0 &&
                        _wcsicmp(name.c_str(), L"Videos") != 0) {
                        AddItem(entry.path(), true, step2Items, seen);
                        ScanPath(entry.path(), step2Items, seen, 8, kMaxFiles, totalIndexed);
                    }
                } else if (entry.is_regular_file(ec)) {
                    if (IsUserRelevantFile(entry.path())) {
                        AddItem(entry.path(), false, step2Items, seen);
                    }
                }
            }
            CoTaskMemFree(profilePath);
            totalIndexed += step2Items.size();
            publishOrAppend(std::move(step2Items));
        }

        // 3. Scan all fixed and removable drives (e.g. C:\, D:\, X:\)
        wchar_t driveBuffer[512]{};
        if (running_.load() && totalIndexed < kMaxFiles &&
            GetLogicalDriveStringsW(static_cast<DWORD>(std::size(driveBuffer)), driveBuffer)) {
            const wchar_t* drive = driveBuffer;
            while (*drive && running_.load() && totalIndexed < kMaxFiles) {
                const UINT driveType = GetDriveTypeW(drive);
                if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
                    std::vector<FileItem> driveItems;
                    driveItems.reserve(8192);
                    const wchar_t driveLetter = towupper(drive[0]);
                    const bool isDriveC = (driveLetter == L'C');
                    fs::directory_iterator dit(drive, fs::directory_options::skip_permission_denied, ec);
                    for (const auto& entry : dit) {
                        if (!running_.load() || totalIndexed + driveItems.size() >= kMaxFiles) break;
                        if (entry.is_directory(ec)) {
                            std::wstring dirName = entry.path().filename().wstring();
                            if (isDriveC && _wcsicmp(dirName.c_str(), L"Users") == 0) {
                                continue;
                            }
                            if (!ShouldSkipDirectory(entry.path())) {
                                AddItem(entry.path(), true, driveItems, seen);
                                const int maxDepth = isDriveC ? 4 : 8;
                                ScanPath(entry.path(), driveItems, seen, maxDepth, kMaxFiles, totalIndexed);
                            }
                        } else if (entry.is_regular_file(ec)) {
                            if (IsUserRelevantFile(entry.path())) {
                                AddItem(entry.path(), false, driveItems, seen);
                            }
                        }
                    }
                    totalIndexed += driveItems.size();
                    publishOrAppend(std::move(driveItems));
                }
                drive += wcslen(drive) + 1;
            }
        }

        if (!running_.load()) return;

        ready_ = true;

        if (notifyHwnd_) {
            PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
        }
    }

    void WorkerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        // Initial background index
        BuildIndex();

        // Setup change monitors for active user directories
        PWSTR desktopPath = nullptr, docPath = nullptr, downPath = nullptr;
        SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktopPath);
        SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docPath);
        SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downPath);

        HANDLE hDesktop = desktopPath ? FindFirstChangeNotificationW(desktopPath, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME) : INVALID_HANDLE_VALUE;
        HANDLE hDocs = docPath ? FindFirstChangeNotificationW(docPath, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME) : INVALID_HANDLE_VALUE;
        HANDLE hDownloads = downPath ? FindFirstChangeNotificationW(downPath, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME) : INVALID_HANDLE_VALUE;

        // Release path strings immediately after initializing handles
        if (desktopPath) { CoTaskMemFree(desktopPath); desktopPath = nullptr; }
        if (docPath) { CoTaskMemFree(docPath); docPath = nullptr; }
        if (downPath) { CoTaskMemFree(downPath); downPath = nullptr; }

        std::vector<HANDLE> waitHandles;
        if (stopEvent_) waitHandles.push_back(stopEvent_);
        if (triggerEvent_) waitHandles.push_back(triggerEvent_);
        if (hDesktop != INVALID_HANDLE_VALUE && hDesktop != nullptr) waitHandles.push_back(hDesktop);
        if (hDocs != INVALID_HANDLE_VALUE && hDocs != nullptr) waitHandles.push_back(hDocs);
        if (hDownloads != INVALID_HANDLE_VALUE && hDownloads != nullptr) waitHandles.push_back(hDownloads);

        while (running_.load()) {
            DWORD wait = WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                FALSE,
                300000 // 5-minute periodic idle scan
            );

            if (!running_.load()) break;

            if (wait == WAIT_OBJECT_0) {
                // stopEvent_
                break;
            }

            // Debounce user file operations (e.g. large file write, burst of downloads)
            WaitForSingleObject(stopEvent_, 3000);
            if (!running_.load()) break;

            BuildIndex();

            // Refresh change notification handles
            if (hDesktop != INVALID_HANDLE_VALUE && hDesktop != nullptr) FindNextChangeNotification(hDesktop);
            if (hDocs != INVALID_HANDLE_VALUE && hDocs != nullptr) FindNextChangeNotification(hDocs);
            if (hDownloads != INVALID_HANDLE_VALUE && hDownloads != nullptr) FindNextChangeNotification(hDownloads);
        }

        if (hDesktop != INVALID_HANDLE_VALUE && hDesktop != nullptr) FindCloseChangeNotification(hDesktop);
        if (hDocs != INVALID_HANDLE_VALUE && hDocs != nullptr) FindCloseChangeNotification(hDocs);
        if (hDownloads != INVALID_HANDLE_VALUE && hDownloads != nullptr) FindCloseChangeNotification(hDownloads);
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    mutable std::mutex mutex_;
    std::shared_ptr<const IndexSnapshot> snapshot_;
    std::thread worker_;
    HANDLE stopEvent_ = nullptr;
    HANDLE triggerEvent_ = nullptr;
    HWND notifyHwnd_ = nullptr;
};

} // namespace takeoff
