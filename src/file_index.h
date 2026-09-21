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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <shlobj.h>

namespace takeoff {

namespace fs = std::filesystem;

constexpr UINT kFilesReadyMessage = WM_APP + 8;

struct DirectoryEntry {
    std::wstring path;
    std::wstring normPath;
    fs::file_time_type lastKnownMtime{};
};

class DirectoryPool {
public:
    uint32_t Intern(const std::wstring& path, const std::wstring& normPath) {
        auto it = index_.find(normPath);
        if (it != index_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(entries_.size());
        entries_.push_back({path, normPath, fs::file_time_type{}});
        index_.emplace(normPath, idx);
        return idx;
    }

    const DirectoryEntry& Get(uint32_t index) const { return entries_[index]; }
    size_t Size() const { return entries_.size(); }

    void SetMtime(uint32_t index, fs::file_time_type mtime) { entries_[index].lastKnownMtime = mtime; }
    fs::file_time_type GetMtime(uint32_t index) const { return entries_[index].lastKnownMtime; }

    // Test/serialization-only: iterate every interned directory.
    const std::vector<DirectoryEntry>& Entries() const { return entries_; }

private:
    std::vector<DirectoryEntry> entries_;
    std::unordered_map<std::wstring, uint32_t> index_;
};

struct FileItem {
    std::wstring name;
    std::wstring normName;
    uint32_t parentDirIndex = 0;
    bool isDirectory = false;
};

struct FileSearchResult {
    std::wstring name;
    std::wstring path;
    bool isDirectory = false;
    int score = 0;
};

// One entry per DirectoryPool index: that directory's direct file/folder
// children. A directory with no indexed children yet has a null entry.
struct IndexSnapshot {
    std::vector<std::shared_ptr<const std::vector<FileItem>>> chunksByDir;
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

    enum class Phase { Idle, FirstWalk, Loaded, IncrementalRescan };

    Phase GetPhase() const { return phase_.load(); }

    // scanRootOverride is test-only: when non-empty, BuildIndex() scans just
    // that one directory tree instead of the whole machine (known user
    // folders, %USERPROFILE%, all fixed/removable drives). Production
    // callers must leave it empty.
    void Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"") {
        if (running_.exchange(true)) return;
        notifyHwnd_ = notifyHwnd;
        scanRootOverride_ = scanRootOverride;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
    }

    bool IsReady() const {
        return ready_.load();
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_ ? snapshot_->totalCount : 0;
    }

    // Test-only: interns a path into this instance's own DirectoryPool so
    // a caller can obtain a poolIndex valid for SetDirectoryChunk /
    // PruneDirectory without running a real scan. Production callers get
    // their poolIndex from ScanPath/BuildIndex instead.
    uint32_t InternDirectoryForTest(const std::wstring& path, const std::wstring& normPath) {
        return InternLocked(pool_, path, normPath);
    }

    // Test-only: clears all indexed data so Count()/Search() start from a
    // known-empty state, independent of any earlier BuildIndex() run in
    // this same process (FileIndex::Instance() is a process-wide singleton).
    void ResetForTest() {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_ = DirectoryPool{};
        snapshot_.reset();
        ready_ = false;
    }

    void SetDirectoryChunk(uint32_t poolIndex, std::vector<FileItem>&& children) {
        std::vector<std::pair<uint32_t, std::vector<FileItem>>> batch;
        batch.emplace_back(poolIndex, std::move(children));
        SetDirectoryChunks(std::move(batch));
    }

    // Upserts many directories' chunks in a single chunksByDir copy,
    // instead of one copy per directory - SetDirectoryChunk copies the
    // *entire* chunksByDir vector on every call, so calling it once per
    // directory across a walk of D directories is O(D^2). Callers that
    // publish many directories from one walk (ScanPath) should batch
    // them here instead.
    void SetDirectoryChunks(std::vector<std::pair<uint32_t, std::vector<FileItem>>>&& batch) {
        if (batch.empty()) return;
        std::vector<std::pair<uint32_t, std::shared_ptr<const std::vector<FileItem>>>> chunks;
        chunks.reserve(batch.size());
        for (auto& [poolIdx, children] : batch) {
            chunks.emplace_back(poolIdx, std::make_shared<const std::vector<FileItem>>(std::move(children)));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto newSnapshot = std::make_shared<IndexSnapshot>();
        if (snapshot_) {
            newSnapshot->chunksByDir = snapshot_->chunksByDir;
            newSnapshot->totalCount = snapshot_->totalCount;
        }
        uint32_t maxIdx = 0;
        for (auto& [poolIdx, chunk] : chunks) maxIdx = (std::max)(maxIdx, poolIdx);
        if (maxIdx >= newSnapshot->chunksByDir.size()) {
            newSnapshot->chunksByDir.resize(maxIdx + 1);
        }
        for (auto& [poolIdx, chunk] : chunks) {
            const size_t oldSize = newSnapshot->chunksByDir[poolIdx] ? newSnapshot->chunksByDir[poolIdx]->size() : 0;
            newSnapshot->totalCount = newSnapshot->totalCount - oldSize + chunk->size();
            newSnapshot->chunksByDir[poolIdx] = std::move(chunk);
        }
        snapshot_ = std::move(newSnapshot);
        ready_ = true;
    }

    void PruneDirectory(uint32_t poolIndex) {
        SetDirectoryChunk(poolIndex, {});
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
            // Range-for's implicit operator++ on directory_iterator throws
            // fs::filesystem_error on a mid-iteration error (e.g. a
            // removable drive unplugged during the scan) - guard it the
            // same way ScanPath does, so that isn't an uncaught exception
            // and std::terminate().
            try {
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
            } catch (...) {}
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
            if (lower == L"build" || lower == L"cmake" || lower == L"msbuild" ||
                lower == L"release" || lower == L"debug" ||
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

        // Coarse-locked for the whole call (matches Count()'s existing
        // locking style): pool_ isn't independently thread-safe, and
        // search is already sub-millisecond, so lock hold time here is
        // negligible.
        std::lock_guard<std::mutex> lock(mutex_);
        std::shared_ptr<const IndexSnapshot> snapshot = snapshot_;
        if (!snapshot || snapshot->chunksByDir.empty()) return {};

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
            uint32_t parentDirIndex;
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

        for (const auto& chunk : snapshot->chunksByDir) {
            if (!chunk) continue;
            for (const auto& item : *chunk) {
                int s = -1;

                // 1. Primary match: check if query matches the file/folder name directly
                if (item.normName.size() >= (isSingleToken ? qLen : tokens.back().size())) {
                    if (item.normName.find(firstChar) != std::wstring::npos) {
                        s = ScoreFile(item.normName, normQuery, item.isDirectory);
                    }
                }

                // 2. Secondary match: path / parent directory match
                if (s <= 0 && allowPathMatch) {
                    const std::wstring normPath = pool_.Get(item.parentDirIndex).normPath + L"\\" + item.normName;
                    if (isSingleToken) {
                        // Contiguous substring in path (e.g. folder name in path)
                        size_t pos = normPath.find(normQuery);
                        if (pos != std::wstring::npos) {
                            const size_t penalty = (std::min)(normPath.size() / 4, size_t{300});
                            int pathScore = 2600 - static_cast<int>(penalty);
                            if (item.isDirectory) pathScore += 40;
                            s = (std::max)(1000, pathScore);
                        }
                    } else {
                        // Multi-token match: all tokens must appear in normPath
                        bool allFound = true;
                        for (const auto& token : tokens) {
                            if (normPath.find(token) == std::wstring::npos) {
                                allFound = false;
                                break;
                            }
                        }
                        if (allFound) {
                            const bool lastMatchesName = (item.normName.find(tokens.back()) != std::wstring::npos);
                            const size_t penalty = (std::min)(normPath.size() / 4, size_t{300});
                            int tokenScore = 2400 + (lastMatchesName ? 600 : 0) - static_cast<int>(penalty);
                            if (item.isDirectory) tokenScore += 40;
                            s = (std::max)(1000, tokenScore);
                        }
                    }
                }

                if (s > 0) {
                    candidates.push_back({s, &item, item.parentDirIndex});
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
            const auto& c = candidates[i];
            results.push_back({
                c.item->name,
                pool_.Get(c.parentDirIndex).path + L"\\" + c.item->name,
                c.item->isDirectory,
                c.score
            });
        }
        return results;
    }

private:
    FileIndex() = default;

    void AddItem(const fs::path& p, bool isDir, uint32_t parentDirIndex,
                 std::vector<FileItem>& items, std::unordered_set<uint64_t>& seen) {
        std::wstring name = p.filename().wstring();
        if (name.empty()) {
            name = p.wstring();
            if (name.empty()) return;
        }
        if (!isDir) {
            if (name[0] == L'.' || name[0] == L'~') return;
            if (!IsUserRelevantFile(p)) return;
        }
        uint64_t pathHash = Fnv1a64(Normalize(p.wstring()));
        if (!seen.insert(pathHash).second) return;
        std::wstring norm = Normalize(name);
        items.push_back({std::move(name), std::move(norm), parentDirIndex, isDir});
    }

    // scannedDirs tracks every pool index already fully walked by
    // ScanPath in this BuildIndex() call, across every phase - it
    // prevents a later phase (e.g. a %USERPROFILE% or drive walk)
    // redundantly re-descending into a directory an earlier phase (e.g.
    // the verified-project-root phase) already scanned. Without this
    // guard, the redundant walk would find all of that subtree's items
    // already in `seen` (deduped), publish an empty chunk for it, and
    // silently wipe out the earlier phase's real content for that
    // directory - a genuine cross-phase data-loss bug, not just a
    // sibling-clobbering one.
    void ScanPath(const fs::path& root, uint32_t rootPoolIndex, DirectoryPool& pool,
                  std::unordered_set<uint64_t>& seen, std::unordered_set<uint32_t>& scannedDirs,
                  int maxDepth) {
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        if (IsDriveRoot(root) || ShouldSkipDirectory(root)) return;
        if (!scannedDirs.insert(rootPoolIndex).second) return;

        // Set I/O priority hint on the root directory to avoid disrupting the system during full-disk walks.
        HANDLE dirHandle = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dirHandle != INVALID_HANDLE_VALUE) {
            FILE_IO_PRIORITY_HINT_INFO hint{};
            hint.PriorityHint = IoPriorityHintLow;
            SetFileInformationByHandle(dirHandle, FileIoPriorityHintInfo, &hint, sizeof(hint));
            CloseHandle(dirHandle);
        }

        // childrenByDir[poolIndex] accumulates one directory's direct
        // children until that directory's listing is complete, then gets
        // published as a single chunk - this is what makes a later
        // incremental rescan of just that directory O(its own children),
        // not O(the whole index).
        std::unordered_map<uint32_t, std::vector<FileItem>> childrenByDir;
        childrenByDir[rootPoolIndex]; // ensure root has an entry even if empty

        try {
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            std::vector<uint32_t> dirIndexAtDepth{rootPoolIndex};
            size_t dirsVisitedThisCall = 0;

            while (it != end && !ec) {
                if (!running_.load()) break;

                if (++dirsVisitedThisCall % 64 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                const auto& entry = *it;
                const int depth = it.depth(); // 0 == direct child of root
                const uint32_t parentIdx = dirIndexAtDepth[static_cast<size_t>(depth)];

                bool isDir = entry.is_directory(ec);
                if (!ec && isDir) {
                    if (depth >= maxDepth || ShouldSkipDirectory(entry.path())) {
                        it.disable_recursion_pending();
                    } else {
                        AddItem(entry.path(), true, parentIdx, childrenByDir[parentIdx], seen);
                        uint32_t childIdx = InternLocked(pool, entry.path().wstring(), Normalize(entry.path().wstring()));
                        if (!scannedDirs.insert(childIdx).second) {
                            // Already scanned (as a root by an earlier
                            // phase, or as another branch of this same
                            // walk) - don't redundantly re-walk it, which
                            // would publish an empty chunk over its real
                            // content since all its items are already
                            // in `seen`.
                            it.disable_recursion_pending();
                        } else {
                            childrenByDir[childIdx]; // ensure it exists even if it turns out empty
                            if (static_cast<size_t>(depth) + 1 >= dirIndexAtDepth.size()) {
                                dirIndexAtDepth.push_back(childIdx);
                            } else {
                                dirIndexAtDepth[static_cast<size_t>(depth) + 1] = childIdx;
                            }
                        }
                    }
                    it.increment(ec);
                    continue;
                }

                if (!ec && entry.is_regular_file(ec) && IsUserRelevantFile(entry.path())) {
                    AddItem(entry.path(), false, parentIdx, childrenByDir[parentIdx], seen);
                }
                it.increment(ec);
            }
        } catch (...) {}

        // Batch every directory this one ScanPath call touched into a
        // single SetDirectoryChunks call, instead of one SetDirectoryChunk
        // call per directory - SetDirectoryChunk copies the whole
        // chunksByDir vector per call, so one call per directory in a
        // D-directory walk is O(D^2); one batched call per ScanPath
        // invocation is O(D) per call (O(P*D) total across P phase-level
        // ScanPath invocations, not O(D^2)).
        std::vector<std::pair<uint32_t, std::vector<FileItem>>> batch;
        batch.reserve(childrenByDir.size());
        for (auto& [poolIdx, children] : childrenByDir) {
            RefreshMtimeLocked(pool, poolIdx);
            batch.emplace_back(poolIdx, std::move(children));
        }
        SetDirectoryChunks(std::move(batch));
    }

    static fs::file_time_type CurrentMtime(const std::wstring& path) {
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        return ec ? fs::file_time_type{} : t;
    }

    // DirectoryPool has no internal locking of its own (see DirectoryPool
    // above); pool_ is written by the single worker thread and read by
    // Search() from callers on other threads, so every write to it must
    // go through mutex_ - the same lock Search() holds for its whole call.
    uint32_t InternLocked(DirectoryPool& pool, const std::wstring& path, const std::wstring& normPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool.Intern(path, normPath);
    }

    // Reads the path and stores the freshly-computed mtime as two short,
    // separate critical sections, so the blocking last_write_time() stat
    // call itself runs outside mutex_ - Search() holds that same mutex
    // for its entire call, so a stat call held under the lock would
    // stall concurrent searches for as long as the disk I/O takes.
    void RefreshMtimeLocked(DirectoryPool& pool, uint32_t idx) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = pool.Get(idx).path;
        }
        fs::file_time_type mtime = CurrentMtime(path);
        std::lock_guard<std::mutex> lock(mutex_);
        pool.SetMtime(idx, mtime);
    }

    // Registers `root` as a searchable item in its own parent's chunk by
    // merging (read-modify-write) into whatever's already published
    // there, instead of replacing it - so it can never clobber (or need
    // to run before) another phase's legitimate scan of that same parent
    // directory, regardless of which phase happens to touch that parent
    // first or last. A no-op if `root`'s own path is already in `seen`
    // (i.e. some phase's ordinary scan already added it as a real child
    // of its parent) - safe to call unconditionally after every phase.
    void MergeSelfItem(const fs::path& root, std::unordered_set<uint64_t>& seen) {
        if (!root.has_parent_path()) return;
        fs::path parent = root.parent_path();
        if (parent == root) return;
        uint32_t parentIdx = InternLocked(pool_, parent.wstring(), Normalize(parent.wstring()));
        std::vector<FileItem> selfItem;
        AddItem(root, true, parentIdx, selfItem, seen);
        if (selfItem.empty()) return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto newSnapshot = std::make_shared<IndexSnapshot>();
        if (snapshot_) {
            newSnapshot->chunksByDir = snapshot_->chunksByDir;
            newSnapshot->totalCount = snapshot_->totalCount;
        }
        if (parentIdx >= newSnapshot->chunksByDir.size()) {
            newSnapshot->chunksByDir.resize(parentIdx + 1);
        }
        std::vector<FileItem> merged;
        if (newSnapshot->chunksByDir[parentIdx]) {
            merged = *newSnapshot->chunksByDir[parentIdx];
        }
        const size_t oldSize = merged.size();
        for (auto& item : selfItem) merged.push_back(std::move(item));
        newSnapshot->totalCount = newSnapshot->totalCount - oldSize + merged.size();
        newSnapshot->chunksByDir[parentIdx] = std::make_shared<const std::vector<FileItem>>(std::move(merged));
        snapshot_ = std::move(newSnapshot);
        ready_ = true;
    }

    void BuildIndex() {
        phase_ = Phase::FirstWalk;
        std::unordered_set<uint64_t> seen;
        // Tracks every directory already fully walked by ScanPath in this
        // call, across all phases - see ScanPath's comment on scannedDirs.
        std::unordered_set<uint32_t> scannedDirs;

        if (!scanRootOverride_.empty()) {
            // Test-only path: scan just the given directory tree, skipping
            // the whole-machine scan below entirely - keeps the live index
            // deterministic and independent of whatever else happens to be
            // on the real disk running the test.
            std::error_code overrideEc;
            fs::path overrideRoot(scanRootOverride_);
            if (fs::exists(overrideRoot, overrideEc)) {
                uint32_t rootIdx = InternLocked(pool_, overrideRoot.wstring(), Normalize(overrideRoot.wstring()));
                ScanPath(overrideRoot, rootIdx, pool_, seen, scannedDirs, 8);
                MergeSelfItem(overrideRoot, seen);
            }
            phase_ = Phase::Loaded;
            ready_ = true;
            if (notifyHwnd_) {
                PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
            }
            return;
        }

        // 0. Scan verified user project/repo root immediately (if any).
        // Self-registration (making repoDir findable by its own name) is
        // deferred until after phases 1-3 - see the deferred block below.
        std::error_code ec;
        fs::path currentDir = fs::current_path(ec);
        fs::path repoDir;
        if (!ec && !currentDir.empty()) {
            repoDir = FindVerifiedProjectRoot(currentDir);
            if (!repoDir.empty()) {
                uint32_t repoIdx = InternLocked(pool_, repoDir.wstring(), Normalize(repoDir.wstring()));
                ScanPath(repoDir, repoIdx, pool_, seen, scannedDirs, 6);
            }
        }

        // 1. Scan primary user folders (Desktop, Documents, Downloads, Pictures, Music, Videos).
        // Self-registration for each is deferred until after phase 3 -
        // see the deferred block below.
        const KNOWNFOLDERID userFolders[] = {
            FOLDERID_Desktop,
            FOLDERID_Documents,
            FOLDERID_Downloads,
            FOLDERID_Pictures,
            FOLDERID_Music,
            FOLDERID_Videos,
        };

        std::vector<fs::path> knownFolders;
        for (const auto& kfid : userFolders) {
            if (!running_.load()) break;
            PWSTR folderPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(kfid, KF_FLAG_DEFAULT, nullptr, &folderPath)) && folderPath) {
                fs::path folder(folderPath);
                knownFolders.push_back(folder);
                uint32_t folderIdx = InternLocked(pool_, folder.wstring(), Normalize(folder.wstring()));
                ScanPath(folder, folderIdx, pool_, seen, scannedDirs, 8);
                CoTaskMemFree(folderPath);
            }
        }

        // 2. Scan %USERPROFILE% roots (e.g. source code directories, projects, etc.)
        PWSTR profilePath = nullptr;
        if (running_.load() &&
            SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profilePath)) && profilePath) {
            uint32_t profileIdx = InternLocked(pool_, profilePath, Normalize(std::wstring(profilePath)));
            std::vector<FileItem> profileItems;
            profileItems.reserve(256);
            fs::directory_iterator dit(profilePath, fs::directory_options::skip_permission_denied, ec);
            try {
                for (const auto& entry : dit) {
                    if (!running_.load()) break;
                    if (entry.is_directory(ec)) {
                        std::wstring name = entry.path().filename().wstring();
                        if (!ShouldSkipDirectory(entry.path()) &&
                            _wcsicmp(name.c_str(), L"Desktop") != 0 &&
                            _wcsicmp(name.c_str(), L"Documents") != 0 &&
                            _wcsicmp(name.c_str(), L"Downloads") != 0 &&
                            _wcsicmp(name.c_str(), L"Pictures") != 0 &&
                            _wcsicmp(name.c_str(), L"Music") != 0 &&
                            _wcsicmp(name.c_str(), L"Videos") != 0) {
                            AddItem(entry.path(), true, profileIdx, profileItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, 8);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path())) {
                            AddItem(entry.path(), false, profileIdx, profileItems, seen);
                        }
                    }
                }
            } catch (...) {}
            SetDirectoryChunk(profileIdx, std::move(profileItems));
            CoTaskMemFree(profilePath);
        }

        // 3. Scan all fixed and removable drives (e.g. C:\, D:\, X:\)
        wchar_t driveBuffer[512]{};
        if (running_.load() &&
            GetLogicalDriveStringsW(static_cast<DWORD>(std::size(driveBuffer)), driveBuffer)) {
            const wchar_t* drive = driveBuffer;
            while (*drive && running_.load()) {
                const UINT driveType = GetDriveTypeW(drive);
                if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
                    const wchar_t driveLetter = towupper(drive[0]);
                    const bool isDriveC = (driveLetter == L'C');
                    uint32_t driveIdx = InternLocked(pool_, drive, Normalize(std::wstring(drive)));
                    std::vector<FileItem> driveItems;
                    driveItems.reserve(8192);
                    fs::directory_iterator dit(drive, fs::directory_options::skip_permission_denied, ec);
                    try {
                        for (const auto& entry : dit) {
                            if (!running_.load()) break;
                            if (entry.is_directory(ec)) {
                                std::wstring dirName = entry.path().filename().wstring();
                                if (isDriveC && _wcsicmp(dirName.c_str(), L"Users") == 0) {
                                    continue;
                                }
                                if (!ShouldSkipDirectory(entry.path())) {
                                    AddItem(entry.path(), true, driveIdx, driveItems, seen);
                                    uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                                    const int maxDepth = isDriveC ? 4 : 8;
                                    ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, maxDepth);
                                }
                            } else if (entry.is_regular_file(ec)) {
                                if (IsUserRelevantFile(entry.path())) {
                                    AddItem(entry.path(), false, driveIdx, driveItems, seen);
                                }
                            }
                        }
                    } catch (...) {}
                    SetDirectoryChunk(driveIdx, std::move(driveItems));
                }
                drive += wcslen(drive) + 1;
            }
        }

        // Deferred self-registration: runs after every phase so nothing
        // published afterward can clobber it, and merges rather than
        // replaces so it can't erase anything a phase already legitimately
        // published for that parent. Each call is a no-op if the ordinary
        // scan above already added that root as a real child of its
        // parent (repoDir, if reachable from phase 2/3; known folders
        // never are, since phase 2 explicitly excludes their names and
        // phase 3 excludes "Users" on the C: drive).
        if (!repoDir.empty()) MergeSelfItem(repoDir, seen);
        for (auto& folder : knownFolders) MergeSelfItem(folder, seen);

        if (!running_.load()) return;

        phase_ = Phase::Loaded;
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
    std::atomic<Phase> phase_{Phase::Idle};
    mutable std::mutex mutex_;
    std::shared_ptr<const IndexSnapshot> snapshot_;
    DirectoryPool pool_;
    std::thread worker_;
    HANDLE stopEvent_ = nullptr;
    HWND notifyHwnd_ = nullptr;
    std::wstring scanRootOverride_;
};

} // namespace takeoff
