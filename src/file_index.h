#pragma once

#include "search.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <shlobj.h>

namespace takeoff {

namespace fs = std::filesystem;

constexpr UINT kFilesReadyMessage = WM_APP + 8;
constexpr uint32_t kCacheMagic = 0x4C4C4649; // "LLFI"
// Bumped to 2 (US-019): the cache now carries the exclusions file's mtime so a
// cache written before an exclusions edit can be rejected instead of silently
// keeping now-excluded content searchable forever.
constexpr uint32_t kCacheFormatVersion = 2;

template <typename T>
void WriteRaw(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}
template <typename T>
bool ReadRaw(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}
inline void WriteWString(std::ofstream& out, const std::wstring& s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    WriteRaw(out, len);
    if (len) out.write(reinterpret_cast<const char*>(s.data()), len * sizeof(wchar_t));
}
inline bool ReadWString(std::ifstream& in, std::wstring& s) {
    uint32_t len = 0;
    if (!ReadRaw(in, len)) return false;
    s.resize(len);
    if (len) in.read(reinterpret_cast<char*>(s.data()), len * sizeof(wchar_t));
    return static_cast<bool>(in) || len == 0;
}

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

// Namespace-scope (not a function-local static inside IsUserRelevantFile)
// so it's constructed at static-init time, before main() runs - and
// therefore, by C++'s reverse-of-construction destruction order, torn
// down *after* FileIndex::Instance()'s function-local-static singleton.
// FileIndex::~FileIndex() joins its worker thread before returning, so
// that ordering guarantees the worker thread can never be mid-call into
// IsUserRelevantFile (e.g. from IncrementalRescan) against an
// already-destroyed set during process exit. Getting this backwards
// (as a function-local static, first constructed only when the worker
// thread's initial scan first calls IsUserRelevantFile - after
// FileIndex::s_instance already exists) previously meant this set was
// destroyed *before* ~FileIndex()'s Stop()/join() ran, letting a
// still-running worker thread dereference a freed unordered_set on
// std::exit() - reproduced reliably by a Debug build under
// core_tests.cpp's FileIndex scoped-scan test.
inline const std::unordered_set<std::wstring_view> kAllowedExtensions = {
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

// Additive-only, user-supplied exclusions layered on top of the hardcoded
// skip-list (ShouldSkipDirectory) and extension allowlist (IsUserRelevantFile)
// - see UserExclusions (US-019). Never used to re-include anything the
// hardcoded rules already exclude; every call site runs the hardcoded check
// first and unconditionally.
struct UserExclusions {
    std::vector<std::wstring> excludedFolders;   // NormalizeForCompare'd: lowercase, '\' separators, no trailing '\'
    std::unordered_set<std::wstring> excludedExtensions; // lowercase, includes leading '.'
};

class FileIndex {
public:
    static FileIndex& Instance() {
        static FileIndex s_instance;
        return s_instance;
    }

    ~FileIndex() { Stop(); }

    enum class Phase { Idle, FirstWalk, Loaded, IncrementalRescan };

    Phase GetPhase() const { return phase_.load(); }

    static std::wstring DefaultCachePath() {
        wchar_t localAppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
            std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return (dir / L"file_index.cache").wstring();
        }
        return L"";
    }

    static std::wstring DefaultExclusionsPath() {
        wchar_t localAppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
            std::filesystem::path dir = std::filesystem::path(localAppData) / L"LeanLauncher";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return (dir / L"file_search_excludes.txt").wstring();
        }
        return L"";
    }

    // Lowercase, '/' -> '\', trailing '\' trimmed - the same *kind* of
    // normalization ShouldSkipDirectory's Windows-directory check performs
    // inline. That inline block is deliberately left as-is rather than
    // refactored to call this; this exists because both exclusions-file
    // loading and folder-exclusion matching need the normalization too.
    static std::wstring NormalizeForCompare(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        std::replace(s.begin(), s.end(), L'/', L'\\');
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    }

    // True if normalizedCandidate is normalizedBase itself or anywhere under it.
    // Both arguments must already be NormalizeForCompare'd.
    static bool IsPathUnderNormalizedFolder(const std::wstring& normalizedCandidate,
                                             const std::wstring& normalizedBase) {
        if (normalizedBase.empty()) return false;
        if (normalizedCandidate == normalizedBase) return true;
        return normalizedCandidate.size() > normalizedBase.size() &&
            normalizedCandidate.compare(0, normalizedBase.size(), normalizedBase) == 0 &&
            normalizedCandidate[normalizedBase.size()] == L'\\';
    }

    // file_index.h is deliberately self-contained (no include of
    // obsidian_config.h, which has its own copy of this exact UTF-8<->UTF-16
    // idiom) - duplicated locally rather than introducing a cross-header
    // dependency for two small functions.
    static std::wstring Utf8BytesToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) return L"";
        std::wstring out(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), wlen);
        return out;
    }

    static std::string WideToUtf8Bytes(const std::wstring& wide) {
        if (wide.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    // Parses one line per folder or extension exclusion. A line is a folder
    // exclusion if it starts with a drive letter ("D:"), "\\" (UNC), or "/";
    // an extension exclusion if it starts with "." and contains no path
    // separator or whitespace after that. Blank lines, "#" comments, and
    // anything else (relative paths, bare words) are silently ignored - this
    // file is additive-only, so there is no "include" syntax to parse at all.
    static UserExclusions LoadUserExclusions(const std::wstring& path) {
        UserExclusions result;
        if (path.empty()) return result;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return result;
        std::ifstream file(path, std::ios::binary);
        if (!file) return result;
        std::ostringstream ss;
        ss << file.rdbuf();
        std::wstring content = Utf8BytesToWide(ss.str());
        // A UTF-8 BOM (EF BB BF) decodes to a leading U+FEFF, which would make
        // line 1 match no entry shape and be dropped silently - several Windows
        // editors write one by default when saving this file.
        if (!content.empty() && content[0] == static_cast<wchar_t>(0xFEFF)) content.erase(0, 1);

        size_t pos = 0;
        while (pos <= content.size()) {
            size_t nl = content.find(L'\n', pos);
            std::wstring line = (nl == std::wstring::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
            pos = (nl == std::wstring::npos) ? content.size() + 1 : nl + 1;

            size_t start = line.find_first_not_of(L" \t\r");
            if (start == std::wstring::npos) continue;
            size_t end = line.find_last_not_of(L" \t\r");
            std::wstring trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == L'#') continue;

            const bool looksLikeFolder =
                (trimmed.size() >= 2 && iswalpha(trimmed[0]) && trimmed[1] == L':') ||
                trimmed.rfind(L"\\\\", 0) == 0 ||
                trimmed[0] == L'/';
            if (looksLikeFolder) {
                result.excludedFolders.push_back(NormalizeForCompare(trimmed));
                continue;
            }

            if (trimmed[0] == L'.' && trimmed.size() > 1 && trimmed[1] != L'.' &&
                trimmed.find_first_of(L"\\/ \t") == std::wstring::npos) {
                std::wstring ext = trimmed;
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                result.excludedExtensions.insert(ext);
            }
        }
        return result;
    }

    static constexpr const wchar_t* kExclusionsFileHeader =
        L"# Lean Launcher file search exclusions\r\n"
        L"# One entry per line, on top of the app's built-in system/build-folder\r\n"
        L"# exclusions. This file is additive only - it cannot un-exclude anything\r\n"
        L"# the app already skips (e.g. system32).\r\n"
        L"#\r\n"
        L"# Folder exclusion: a full path starting with a drive letter, \\\\ (UNC), or /\r\n"
        L"#   D:\\Personal Archive\r\n"
        L"#   \\\\NAS\\Backups\r\n"
        L"#\r\n"
        L"# Extension exclusion: a dot followed by the extension, nothing else\r\n"
        L"#   .iso\r\n"
        L"#\r\n"
        L"# Blank lines, lines starting with #, and anything else are ignored.\r\n"
        L"# Changes take effect on the next scan pass - no restart needed.\r\n"
        L"\r\n";

    // Creates path (and its parent directory) with the header template above
    // if it doesn't already exist. Never touches an existing file - called
    // every time the user clicks "Edit exclusions..." in Settings, so a
    // second/third click must be a pure no-op against their saved edits.
    static bool EnsureExclusionsFileWithHeader(const std::wstring& path) {
        if (path.empty()) return false;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return true;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string utf8Header = WideToUtf8Bytes(kExclusionsFileHeader);
        out << utf8Header;
        return true;
    }

    // scanRootOverride is test-only: when non-empty, BuildIndex() scans just
    // that one directory tree instead of the whole machine (known user
    // folders, %USERPROFILE%, all fixed/removable drives). Production
    // callers must leave it empty.
    // cachePathOverride is test-only, exactly like scanRootOverride: when
    // empty, the real %LOCALAPPDATA%\LeanLauncher\file_index.cache path is
    // used instead - unless scanRootOverride is set, in which case no cache
    // path is resolved at all (see below).
    void Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"",
               const std::wstring& cachePathOverride = L"", const std::wstring& exclusionsPathOverride = L"") {
        if (running_.exchange(true)) return;
        // A prior cycle (e.g. a direct LoadIndexCache() call, or this same
        // singleton's previous Start()/Stop() pair) may have left ready_
        // set to true; reset it so IsReady() reflects this cycle's worker
        // progress, not leftover state from before this Start() call.
        ready_ = false;
        notifyHwnd_ = notifyHwnd;
        scanRootOverride_ = scanRootOverride;
        // A scoped (test) scan must never read or write the production
        // cache: reading it would silently replace the scoped index with
        // the machine-wide one, and writing it would replace the user's
        // real index with a single directory tree - permanently, since a
        // successful cache load means BuildIndex() never runs again. So
        // when a scan root is overridden and no explicit cache path is
        // given, this cycle simply has no cache (every use site already
        // guards on the path being empty).
        cachePath_ = !cachePathOverride.empty() ? cachePathOverride
                   : scanRootOverride.empty()   ? DefaultCachePath()
                                                : std::wstring{};
        // Same reasoning as cachePath_ above, applied to the exclusions file:
        // a scoped test scan must never depend on whatever the real user has
        // configured in %LOCALAPPDATA%\LeanLauncher\file_search_excludes.txt.
        exclusionsPath_ = !exclusionsPathOverride.empty() ? exclusionsPathOverride
                         : scanRootOverride.empty()       ? DefaultExclusionsPath()
                                                           : std::wstring{};
        // Exclusions-change tracking is per-cycle: exclusionsPath_ may differ
        // from the previous cycle's (tests reuse this singleton), and a
        // different file's mtime would otherwise read as an edit and force a
        // needless full re-walk on this cycle's first rescan pass.
        exclusionsMtimeKnown_ = false;
        exclusionsChangedSinceLastPass_ = false;
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
        // Without this, a Loaded left over from a previous Start()/Stop()
        // block can satisfy a test's "wait until the phase settles on
        // Loaded" poll before the next worker has done any work at all.
        phase_ = Phase::Idle;
    }

    // Test-only accessor - real callers never touch the pool directly.
    DirectoryPool& TestOnlyPool() { return pool_; }

    // Test-only: the exclusions-file path resolved for the current Start()
    // cycle. Empty means "this cycle has no user exclusions" - see Start().
    const std::wstring& TestOnlyExclusionsPath() const { return exclusionsPath_; }

    // Test-only: runs exactly one IncrementalRescan() pass synchronously on the
    // calling thread, with running_ held true so the pass isn't treated as
    // interrupted. The worker must already be stopped (Stop() joins it), so
    // this can never race the worker's own calls. Exists because the
    // in-process "exclusions file changed since the last pass" path otherwise
    // only ever fires from the worker's five-minute timer.
    void RunRescanPassForTest() {
        const bool wasRunning = running_.exchange(true);
        IncrementalRescan();
        running_.store(wasRunning);
    }

    // Serializes the pool one entry at a time under brief, separate locks
    // (the same pattern RefreshMtimeLocked / PruneAbsentDrives /
    // IncrementalRescan use), and does the actual file I/O with no lock
    // held at all. SaveIndexCache runs every ~5 minutes from the live
    // worker thread while Search() can block on this same mutex_
    // concurrently, so neither a deep copy of every path/normPath wstring
    // nor a multi-megabyte blocking write may happen under the lock.
    // Pool indices below poolSize stay valid for the whole call: entries
    // are only ever appended (Intern), never removed or reordered, and the
    // only wholesale replacements (LoadIndexCache, ResetForTest) cannot run
    // concurrently with this - the worker thread is the sole caller of both.
    bool SaveIndexCache(const std::wstring& path) const {
        std::shared_ptr<const IndexSnapshot> snapshot;
        size_t poolSize = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
            poolSize = pool_.Size();
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        WriteRaw(out, kCacheMagic);
        WriteRaw(out, kCacheFormatVersion);
        // Staleness field for the user exclusions (US-019). An exclusions edit
        // changes no *directory's* mtime, so IncrementalRescan's per-directory
        // mtime diff can never notice one - and a successful cache load means
        // BuildIndex() never runs again for the life of the process. Recording
        // the exclusions file's mtime here lets LoadIndexCache reject a cache
        // written under different exclusions and fall through to a fresh walk.
        // Serialized exactly like DirectoryEntry::lastKnownMtime below.
        WriteRaw(out, CurrentExclusionsMtime().time_since_epoch().count());
        const uint32_t dirCount = static_cast<uint32_t>(poolSize);
        WriteRaw(out, dirCount);
        for (uint32_t i = 0; i < dirCount; ++i) {
            std::wstring entryPath, entryNormPath;
            fs::file_time_type mtime;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const DirectoryEntry& entry = pool_.Get(i);
                entryPath = entry.path;
                entryNormPath = entry.normPath;
                mtime = entry.lastKnownMtime;
            }
            WriteWString(out, entryPath);
            WriteWString(out, entryNormPath);
            WriteRaw(out, mtime.time_since_epoch().count());
        }

        if (!snapshot) { WriteRaw(out, uint32_t{0}); return static_cast<bool>(out); }
        const uint32_t chunkCount = static_cast<uint32_t>(snapshot->chunksByDir.size());
        WriteRaw(out, chunkCount);
        for (uint32_t i = 0; i < chunkCount; ++i) {
            const auto& chunk = snapshot->chunksByDir[i];
            const uint32_t itemCount = chunk ? static_cast<uint32_t>(chunk->size()) : 0;
            WriteRaw(out, itemCount);
            if (!chunk) continue;
            for (const auto& item : *chunk) {
                WriteWString(out, item.name);
                WriteWString(out, item.normName);
                WriteRaw(out, item.parentDirIndex);
                WriteRaw(out, item.isDirectory);
            }
        }
        return static_cast<bool>(out);
    }

    bool LoadIndexCache(const std::wstring& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        uint32_t magic = 0, version = 0;
        if (!ReadRaw(in, magic) || magic != kCacheMagic) return false;
        if (!ReadRaw(in, version) || version != kCacheFormatVersion) return false;

        // Reject a cache saved under a different exclusions file than the one
        // in effect now (see SaveIndexCache). Both sides go through
        // CurrentExclusionsMtime(), so "no exclusions file" can only ever mean
        // the one thing ReloadUserExclusions() also means by it. A rejected
        // cache falls through to a fresh BuildIndex(), exactly like a
        // missing/corrupt one.
        fs::file_time_type::rep exclusionsRep{};
        if (!ReadRaw(in, exclusionsRep)) return false;
        if (fs::file_time_type(fs::file_time_type::duration(exclusionsRep)) != CurrentExclusionsMtime()) {
            return false;
        }

        // A corrupt file can carry a garbled length field (e.g. a bit-flip
        // producing len ~= 0xFFFFFFFF) for dirCount/chunkCount/itemCount or
        // any wstring's length prefix - those feed directly into
        // allocation-driving calls below (vector(n), resize(n), reserve(n))
        // and can throw length_error/bad_alloc. Catch that here so this
        // function's documented contract (false on any corrupt file) holds
        // instead of letting the exception escape and crash the app.
        try {
            DirectoryPool newPool;
            uint32_t dirCount = 0;
            if (!ReadRaw(in, dirCount)) return false;
            std::vector<DirectoryEntry> entries(dirCount);
            for (uint32_t i = 0; i < dirCount; ++i) {
                if (!ReadWString(in, entries[i].path)) return false;
                if (!ReadWString(in, entries[i].normPath)) return false;
                fs::file_time_type::rep rep{};
                if (!ReadRaw(in, rep)) return false;
                entries[i].lastKnownMtime = fs::file_time_type(fs::file_time_type::duration(rep));
            }
            for (auto& e : entries) newPool.Intern(e.path, e.normPath); // rebuild index map in original order
            for (uint32_t i = 0; i < dirCount; ++i) newPool.SetMtime(i, entries[i].lastKnownMtime);

            auto newSnapshot = std::make_shared<IndexSnapshot>();
            uint32_t chunkCount = 0;
            if (!ReadRaw(in, chunkCount)) return false;
            // Every chunk index is a pool index, so a file claiming more
            // chunks than directories is corrupt. Rejecting it here also
            // keeps chunksByDir's size within the pool's range.
            if (chunkCount > dirCount) return false;
            newSnapshot->chunksByDir.resize(chunkCount);
            for (uint32_t i = 0; i < chunkCount; ++i) {
                uint32_t itemCount = 0;
                if (!ReadRaw(in, itemCount)) return false;
                if (itemCount == 0) continue;
                auto items = std::make_shared<std::vector<FileItem>>();
                items->reserve(itemCount);
                for (uint32_t j = 0; j < itemCount; ++j) {
                    FileItem item;
                    if (!ReadWString(in, item.name)) return false;
                    if (!ReadWString(in, item.normName)) return false;
                    if (!ReadRaw(in, item.parentDirIndex)) return false;
                    // Search() indexes the pool with this value directly
                    // (pool_.Get(item.parentDirIndex)), which is an
                    // unchecked vector read - a garbled index would be an
                    // out-of-bounds access on the UI thread's hot path, and
                    // nothing else in this function would catch it.
                    if (item.parentDirIndex >= dirCount) return false;
                    if (!ReadRaw(in, item.isDirectory)) return false;
                    items->push_back(std::move(item));
                }
                newSnapshot->totalCount += items->size();
                newSnapshot->chunksByDir[i] = std::move(items);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            pool_ = std::move(newPool);
            snapshot_ = std::move(newSnapshot);
            ready_ = true;
            phase_ = Phase::Loaded;
            return true;
        } catch (const std::exception&) {
            return false;
        }
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

    // Drops every indexed entry whose root drive no longer exists (e.g. an
    // unplugged removable drive) - each distinct drive letter is stat'd at
    // most once per call via driveExistsCache, not once per directory.
    // Reads pool_ one entry at a time under brief, separate locks (same
    // pattern as RefreshMtimeLocked) instead of one DirectoryPool deep-copy
    // held under mutex_ for the whole call - at full-disk scale that
    // upfront copy alone would stall a concurrent Search() caller for as
    // long as the copy of every path/normPath wstring takes.
    void PruneAbsentDrives() {
        std::shared_ptr<const IndexSnapshot> snapshot;
        size_t poolSize;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
            poolSize = pool_.Size();
        }
        if (!snapshot) return;

        std::unordered_map<std::wstring, bool> driveExistsCache;
        std::vector<std::pair<uint32_t, std::vector<FileItem>>> batch;
        size_t entriesVisited = 0;
        for (uint32_t i = 0; i < poolSize; ++i) {
            // Same periodic yield ScanPath uses on the first walk - this
            // pass runs on the recurring path, so it owes the rest of the
            // system the same courtesy. It is dropped once the worker is
            // shutting down, so Stop()'s join() never waits on sleeps; the
            // pass itself is cheap enough to just finish.
            if (++entriesVisited % 64 == 0 && running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Nothing to prune if this directory has no published content -
            // including one already emptied by a previous pass, which would
            // otherwise be re-pruned (and re-published) every five minutes
            // for as long as the drive stays unplugged.
            if (i >= snapshot->chunksByDir.size() || !snapshot->chunksByDir[i] ||
                snapshot->chunksByDir[i]->empty()) {
                continue;
            }
            std::wstring path;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                path = pool_.Get(i).path;
            }
            if (path.size() < 2 || path[1] != L':') continue;
            const std::wstring driveRoot = path.substr(0, 2) + L"\\";
            auto it = driveExistsCache.find(driveRoot);
            bool exists;
            if (it != driveExistsCache.end()) {
                exists = it->second;
            } else {
                std::error_code ec;
                exists = fs::exists(driveRoot, ec);
                driveExistsCache[driveRoot] = exists;
            }
            if (!exists) {
                // Reset the stored mtime alongside emptying the chunk:
                // an unplugged drive's directories keep the mtimes they
                // had, so without this the next pass after a replug would
                // compare equal, skip every entry, and leave the emptied
                // chunks empty forever. A default (epoch) mtime makes that
                // pass see them as changed and re-list them instead.
                SetMtimeLocked(pool_, i, fs::file_time_type{});
                batch.emplace_back(i, std::vector<FileItem>{});
            }
        }
        // One publish for the whole pass - PruneDirectory per entry would
        // copy the entire chunksByDir vector per absent directory (O(D^2)).
        SetDirectoryChunks(std::move(batch));
    }

    // Periodic maintenance pass: prunes anything under a now-absent drive,
    // then re-lists (cheaply - immediate children only) every remaining
    // directory whose mtime has changed since it was last scanned. A
    // brand-new subdirectory discovered during a re-list gets a full
    // ScanPath walk, same as a first-run scan of that one subtree. Called
    // from WorkerLoop's periodic timer and once right after a successful
    // cache load; also callable directly by a test.
    void IncrementalRescan() {
        phase_ = Phase::IncrementalRescan;
        ReloadUserExclusions();
        // The exclusions file changed since the last pass in this process, so
        // this pass cannot be an incremental one. Editing that file changes no
        // directory's mtime, so the per-directory diff below would re-list
        // nothing; and even where a parent does get re-listed, a
        // newly-excluded directory's *own* chunk still holds its own files and
        // is never revisited (its mtime is unchanged), so its contents would
        // stay searchable indefinitely.
        //
        // A plain BuildIndex() is not enough either: it only ever republishes
        // the chunks of directories it walks, and it deliberately does not walk
        // an excluded one - so that directory's stale chunk would survive the
        // re-walk. Dropping the index first makes this pass identical to the
        // cold-start path (empty index + full walk), which is the same result a
        // restart now produces via the cache's exclusions-mtime check. One full
        // rescan per exclusions edit is the cost the spec already tolerates.
        if (exclusionsChangedSinceLastPass_) {
            ClearIndex();
            BuildIndex(); // re-runs ReloadUserExclusions() at its top; idempotent
            if (running_.load() && !cachePath_.empty()) SaveIndexCache(cachePath_);
            return;
        }
        // Discover roots that appeared since the first walk. Once a cache
        // exists BuildIndex() never runs again, so this is the only thing
        // that can ever notice a newly attached drive (or a known folder
        // that did not resolve before) - without it, such a root has no
        // pool entry, the loop below only visits existing entries, and the
        // drive stays invisible to search for the life of the cache.
        EnsureRootsInterned();
        PruneAbsentDrives();

        // Bound the loop below by the pool's size *at this instant* rather
        // than deep-copying the whole DirectoryPool under mutex_ (same
        // reasoning as PruneAbsentDrives' comment) - any entry a nested
        // ScanPath call below adds mid-pass just won't be revisited until
        // the next pass, which is fine.
        size_t poolSizeAtStart;
        std::shared_ptr<const IndexSnapshot> snapshotAtStart;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            poolSizeAtStart = pool_.Size();
            snapshotAtStart = snapshot_;
        }

        // Fresh for this rescan pass only - never shared with BuildIndex's
        // own seen/scannedDirs, and never persisted across passes (see
        // ScanPath's comment on scannedDirs for why a stale/shared set is
        // dangerous: it would make a ScanPath call below think a
        // directory was already fully walked and skip it, silently
        // emptying that directory's chunk).
        std::unordered_set<uint64_t> seen;
        std::unordered_set<uint32_t> scannedDirs;

        // Accumulated and published in one SetDirectoryChunks call at the
        // end of the pass: SetDirectoryChunk copies the whole chunksByDir
        // vector (and bumps every element's atomic refcount) per call, so
        // one call per changed directory is O(D^2).
        std::vector<std::pair<uint32_t, std::vector<FileItem>>> batch;
        size_t entriesVisited = 0;

        for (uint32_t i = 0; i < poolSizeAtStart; ++i) {
            if (!running_.load()) break;
            // Same periodic yield ScanPath uses on the first walk. This
            // loop stats every interned directory and runs every 5 minutes
            // *and* on every watched-folder change, so it is the feature's
            // dominant steady-state I/O load - it needs the throttle more
            // than the one-shot first walk does.
            if (++entriesVisited % 64 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::wstring path;
            fs::file_time_type lastKnownMtime;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                path = pool_.Get(i).path;
                lastKnownMtime = pool_.Get(i).lastKnownMtime;
            }
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                // The directory itself is gone (deleted, or under a
                // deleted tree). Nothing else prunes this - PruneAbsentDrives
                // only prunes by drive-letter absence - so without this its
                // files would keep surfacing in search results forever, with
                // reconstructed paths that no longer exist. Reset the mtime
                // (outside the hasContent check below, so it always happens)
                // so that if this exact path is later restored by an
                // mtime-preserving tool (backup/archive restore), the
                // restored mtime won't equal the stale recorded one and the
                // directory gets re-listed instead of staying permanently
                // empty - the same self-healing guarantee PruneAbsentDrives
                // gives unplugged/replugged drives.
                SetMtimeLocked(pool_, i, fs::file_time_type{});
                // Skipped once the chunk is already empty, so a long-gone
                // directory does not get re-published as empty on every pass.
                const bool hasContent = snapshotAtStart && i < snapshotAtStart->chunksByDir.size() &&
                                        snapshotAtStart->chunksByDir[i] &&
                                        !snapshotAtStart->chunksByDir[i]->empty();
                if (hasContent) batch.emplace_back(i, std::vector<FileItem>{});
                continue;
            }
            fs::file_time_type currentMtime = fs::last_write_time(path, ec);
            if (ec || currentMtime == lastKnownMtime) continue;

            // This directory's own contents changed - re-list its immediate
            // children only (cheap); brand-new subdirectories found here get
            // fully walked (they have no prior pool entry / stored mtime).
            std::vector<FileItem> freshChildren;
            fs::directory_iterator dit(path, fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            seen.clear();
            // Mirrors BuildIndex's own drive-root exclusion: C:\Users is
            // deliberately never indexed as a whole (BuildIndex reaches
            // each user's own profile directly via known-folder paths
            // instead) - without repeating that exclusion here, a rare
            // real change to C:\'s own mtime would re-list C:\, discover
            // "Users" as a brand-new child (it was never interned), and
            // fully walk every profile on the machine.
            const bool isDriveRootEntry = IsDriveRoot(path);
            const bool isDriveC = isDriveRootEntry && !path.empty() && towupper(path[0]) == L'C';
            try {
                for (const auto& child : dit) {
                    bool isDir = child.is_directory(ec);
                    if (!ec && isDir) {
                        if (ShouldSkipDirectory(child.path(), &userExclusions_)) continue;
                        if (isDriveC && _wcsicmp(child.path().filename().wstring().c_str(), L"Users") == 0) continue;
                        AddItem(child.path(), true, i, freshChildren, seen);
                        uint32_t childIdx = InternLocked(pool_, child.path().wstring(), Normalize(child.path().wstring()));
                        if (childIdx >= poolSizeAtStart) {
                            // Genuinely new subdirectory - fully walk it (bounded depth,
                            // same as a first-run scan of that one subtree, including
                            // BuildIndex's shallower budget for the system drive).
                            ScanPath(child.path(), childIdx, pool_, seen, scannedDirs, isDriveC ? 4 : 8);
                        }
                    } else if (!ec && child.is_regular_file(ec) && IsUserRelevantFile(child.path(), &userExclusions_)) {
                        AddItem(child.path(), false, i, freshChildren, seen);
                    }
                }
            } catch (...) {}

            SetMtimeLocked(pool_, i, currentMtime);
            batch.emplace_back(i, std::move(freshChildren));
        }

        SetDirectoryChunks(std::move(batch));

        // An interrupted pass has not finished its work, so it must neither
        // claim Loaded nor write a cache - the same shape BuildIndex() uses,
        // and the same reason: Stop() joins this thread from WM_DESTROY, so
        // a full-disk-scale cache write here would block application exit.
        if (!running_.load()) return;

        phase_ = Phase::Loaded;
        if (notifyHwnd_) PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
        if (!cachePath_.empty()) SaveIndexCache(cachePath_);
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

    static bool ShouldSkipDirectory(const fs::path& dirPath, const UserExclusions* userExclusions = nullptr) {
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
        if (userExclusions && !userExclusions->excludedFolders.empty()) {
            const std::wstring normalizedCandidate = NormalizeForCompare(dirPath.wstring());
            for (const auto& excluded : userExclusions->excludedFolders) {
                if (IsPathUnderNormalizedFolder(normalizedCandidate, excluded)) return true;
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

    static bool IsUserRelevantFile(const fs::path& filePath, const UserExclusions* userExclusions = nullptr) {
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

        if (kAllowedExtensions.find(std::wstring_view(lowerExt)) == kAllowedExtensions.end()) return false;
        if (userExclusions && userExclusions->excludedExtensions.count(lowerExt)) return false;
        return true;
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

        // The path match below is evaluated against the parent directory's
        // normPath and the item's normName separately, rather than building
        // (and heap-allocating) the concatenated path per item. That is
        // exactly equivalent: Normalize() never emits a backslash, so no
        // query or token can contain one, so no match can straddle the
        // separator between the two halves - every match lies wholly within
        // one of them.
        //
        // The saved allocation is the smaller half of the win. The parent is
        // the *long* half of the path and is identical for every item in a
        // chunk, so scanning it once per directory instead of once per item
        // is what actually makes this loop affordable: with the file-count
        // cap gone the loop is unbounded, and it runs under mutex_ on the UI
        // thread's call path.
        uint32_t cachedParentIdx = (std::numeric_limits<uint32_t>::max)();
        size_t cachedParentLen = 0;
        bool parentHasQuery = false;
        std::vector<char> parentHasToken(tokens.size(), 0);

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
                    if (item.parentDirIndex != cachedParentIdx) {
                        const std::wstring& dirNorm = pool_.Get(item.parentDirIndex).normPath;
                        cachedParentLen = dirNorm.size();
                        parentHasQuery = dirNorm.find(normQuery) != std::wstring::npos;
                        for (size_t t = 0; t < tokens.size(); ++t) {
                            parentHasToken[t] = dirNorm.find(tokens[t]) != std::wstring::npos ? 1 : 0;
                        }
                        cachedParentIdx = item.parentDirIndex;
                    }
                    // Length of the path this would have reconstructed:
                    // parent + separator + name.
                    const size_t pathLen = cachedParentLen + 1 + item.normName.size();
                    if (isSingleToken) {
                        // Contiguous substring in path (e.g. folder name in path)
                        if (parentHasQuery || item.normName.find(normQuery) != std::wstring::npos) {
                            const size_t penalty = (std::min)(pathLen / 4, size_t{300});
                            int pathScore = 2600 - static_cast<int>(penalty);
                            if (item.isDirectory) pathScore += 40;
                            s = (std::max)(1000, pathScore);
                        }
                    } else {
                        // Multi-token match: all tokens must appear in the path
                        bool allFound = true;
                        for (size_t t = 0; t < tokens.size(); ++t) {
                            if (!parentHasToken[t] && item.normName.find(tokens[t]) == std::wstring::npos) {
                                allFound = false;
                                break;
                            }
                        }
                        if (allFound) {
                            const bool lastMatchesName = (item.normName.find(tokens.back()) != std::wstring::npos);
                            const size_t penalty = (std::min)(pathLen / 4, size_t{300});
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
            // Drive roots are interned with a trailing separator (GetLogicalDriveStringsW
            // returns e.g. L"D:\\"), so unconditionally appending another one produced a
            // doubled backslash for any file directly under a drive root (e.g.
            // "D:\\\\file.txt"), which explorer.exe's /select argument doesn't reliably
            // resolve.
            const std::wstring& parentPath = pool_.Get(c.parentDirIndex).path;
            std::wstring fullPath = parentPath;
            if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
            fullPath += c.item->name;
            results.push_back({
                c.item->name,
                std::move(fullPath),
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
            if (!IsUserRelevantFile(p, &userExclusions_)) return;
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
        if (IsDriveRoot(root) || ShouldSkipDirectory(root, &userExclusions_)) return;
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
                    if (depth >= maxDepth || ShouldSkipDirectory(entry.path(), &userExclusions_)) {
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

                if (!ec && entry.is_regular_file(ec) && IsUserRelevantFile(entry.path(), &userExclusions_)) {
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

    // Companion to RefreshMtimeLocked, for callers (IncrementalRescan) that
    // already have a freshly stat'd mtime in hand and don't need this call
    // to make its own last_write_time() stat under the lock.
    void SetMtimeLocked(DirectoryPool& pool, uint32_t idx, fs::file_time_type mtime) {
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
        // Without this, excluding a folder correctly hides everything under it
        // but leaves the folder itself findable as a stray, empty-looking hit -
        // AC #2 covers "that folder (and everything under it)".
        if (ShouldSkipDirectory(root, &userExclusions_)) return;
        if (!root.has_parent_path()) return;
        fs::path parent = root.parent_path();
        if (parent == root) return;
        uint32_t parentIdx = InternLocked(pool_, parent.wstring(), Normalize(parent.wstring()));
        // Unlike a directory ScanPath actually walks, this parent is only
        // ever touched here (to file one self-item) - without this, its
        // lastKnownMtime stays default/epoch forever, so the very first
        // IncrementalRescan pass would always see it as "changed" and
        // re-list it in full, even when nothing on disk changed.
        RefreshMtimeLocked(pool_, parentIdx);
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

    struct ScanRoots {
        std::vector<fs::path> knownFolders;
        std::vector<fs::path> drives;
    };

    // Enumerates the machine's top-level scan roots - the six known user
    // folders and every fixed/removable drive root - and interns each one
    // into pool_, returning them for BuildIndex()'s first walk.
    // IncrementalRescan() calls it purely for the interning side effect: a
    // root with no pool entry yet gets one with DirectoryEntry's default
    // (epoch) mtime, so the very next mtime comparison in the same rescan
    // pass sees it as changed and re-lists it, merging a newly attached
    // drive back in like any other changed directory.
    ScanRoots EnsureRootsInterned() {
        ScanRoots roots;
        // A scoped (test) scan stays inside its override root - it must
        // never enumerate, intern, or later walk the real machine.
        if (!scanRootOverride_.empty()) return roots;

        const KNOWNFOLDERID userFolders[] = {
            FOLDERID_Desktop,
            FOLDERID_Documents,
            FOLDERID_Downloads,
            FOLDERID_Pictures,
            FOLDERID_Music,
            FOLDERID_Videos,
        };
        for (const auto& kfid : userFolders) {
            if (!running_.load()) return roots;
            PWSTR folderPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(kfid, KF_FLAG_DEFAULT, nullptr, &folderPath)) && folderPath) {
                fs::path folder(folderPath);
                CoTaskMemFree(folderPath);
                InternLocked(pool_, folder.wstring(), Normalize(folder.wstring()));
                roots.knownFolders.push_back(std::move(folder));
            }
        }

        wchar_t driveBuffer[512]{};
        if (running_.load() &&
            GetLogicalDriveStringsW(static_cast<DWORD>(std::size(driveBuffer)), driveBuffer)) {
            const wchar_t* drive = driveBuffer;
            while (*drive && running_.load()) {
                const UINT driveType = GetDriveTypeW(drive);
                if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
                    InternLocked(pool_, drive, Normalize(std::wstring(drive)));
                    roots.drives.emplace_back(drive);
                }
                drive += wcslen(drive) + 1;
            }
        }
        return roots;
    }

    // Re-reads the exclusions file fresh - called once at the top of every
    // BuildIndex()/IncrementalRescan() pass (never on every
    // ShouldSkipDirectory/IsUserRelevantFile call, and never via a
    // filesystem watcher - see US-019's "re-read once per scan pass" AC).
    void ReloadUserExclusions() {
        userExclusions_ = exclusionsPath_.empty() ? UserExclusions{} : LoadUserExclusions(exclusionsPath_);
        const fs::file_time_type mtime = CurrentExclusionsMtime();
        // "Changed" strictly means "changed since an earlier pass in this same
        // Start() cycle". The first pass of a cycle has nothing to compare
        // against and must report no change - otherwise every startup would
        // force a redundant second full walk on top of the one it is already
        // doing, and cross-restart edits are already covered by the cache's own
        // exclusions-mtime check in LoadIndexCache().
        exclusionsChangedSinceLastPass_ = exclusionsMtimeKnown_ && mtime != lastExclusionsMtime_;
        lastExclusionsMtime_ = mtime;
        exclusionsMtimeKnown_ = true;
    }

    // The exclusions file's current mtime, or a default (epoch) value when
    // there is no exclusions file for this cycle. Deliberately routed through
    // the same two conditions ReloadUserExclusions()/LoadUserExclusions() treat
    // as "no exclusions" - an empty exclusionsPath_, or a path that doesn't
    // resolve - so the cache's staleness check can never disagree with the
    // runtime one about whether exclusions exist. CurrentMtime() already
    // returns {} for a path that cannot be stat'd, including a missing one.
    fs::file_time_type CurrentExclusionsMtime() const {
        return exclusionsPath_.empty() ? fs::file_time_type{} : CurrentMtime(exclusionsPath_);
    }

    // Returns the index to the state a cold start begins in. Only used when
    // the exclusions file changes mid-process - see IncrementalRescan() for
    // why a re-walk alone cannot undo a newly added exclusion. ready_/phase_
    // are left alone: BuildIndex() republishes content as it walks, so the
    // gap is transient and callers keep seeing a (shrinking, then growing)
    // index rather than an "indexing..." flicker.
    void ClearIndex() {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_ = DirectoryPool{};
        snapshot_.reset();
    }

    void BuildIndex() {
        phase_ = Phase::FirstWalk;
        ReloadUserExclusions();
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
        const ScanRoots roots = EnsureRootsInterned();
        for (const auto& folder : roots.knownFolders) {
            if (!running_.load()) break;
            uint32_t folderIdx = InternLocked(pool_, folder.wstring(), Normalize(folder.wstring()));
            ScanPath(folder, folderIdx, pool_, seen, scannedDirs, 8);
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
                        if (!ShouldSkipDirectory(entry.path(), &userExclusions_) &&
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
                        if (IsUserRelevantFile(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), false, profileIdx, profileItems, seen);
                        }
                    }
                }
            } catch (...) {}
            SetDirectoryChunk(profileIdx, std::move(profileItems));
            // Same reasoning as MergeSelfItem's comment: this directory is
            // listed by hand here (not via ScanPath, which refreshes mtime
            // itself), so without this its mtime would stay default/epoch
            // and IncrementalRescan would always treat it as "changed".
            RefreshMtimeLocked(pool_, profileIdx);
            CoTaskMemFree(profilePath);
        }

        // 3. Scan all fixed and removable drives (e.g. C:\, D:\, X:\)
        for (const auto& driveRoot : roots.drives) {
            if (!running_.load()) break;
            const std::wstring drive = driveRoot.wstring();
            if (drive.empty()) continue;
            const bool isDriveC = (towupper(drive[0]) == L'C');
            uint32_t driveIdx = InternLocked(pool_, drive, Normalize(drive));
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
                        if (!ShouldSkipDirectory(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), true, driveIdx, driveItems, seen);
                            uint32_t childIdx = InternLocked(pool_, entry.path().wstring(), Normalize(entry.path().wstring()));
                            const int maxDepth = isDriveC ? 4 : 8;
                            ScanPath(entry.path(), childIdx, pool_, seen, scannedDirs, maxDepth);
                        }
                    } else if (entry.is_regular_file(ec)) {
                        if (IsUserRelevantFile(entry.path(), &userExclusions_)) {
                            AddItem(entry.path(), false, driveIdx, driveItems, seen);
                        }
                    }
                }
            } catch (...) {}
            SetDirectoryChunk(driveIdx, std::move(driveItems));
            // Same reasoning as MergeSelfItem's/profileIdx's comment
            // above: listed by hand here, not via ScanPath, so its
            // mtime needs an explicit refresh or IncrementalRescan
            // would always treat every drive root as "changed".
            RefreshMtimeLocked(pool_, driveIdx);
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
        for (const auto& folder : roots.knownFolders) MergeSelfItem(folder, seen);

        if (!running_.load()) return;

        phase_ = Phase::Loaded;
        ready_ = true;

        if (notifyHwnd_) {
            PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
        }
    }

    void WorkerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        // Initial background index: try loading a persisted cache first so a
        // warm start doesn't pay for a full disk walk; fall back to a real
        // walk (and persist its result) if there's no usable cache.
        bool loadedFromCache = !cachePath_.empty() && LoadIndexCache(cachePath_);
        if (loadedFromCache) {
            phase_ = Phase::Loaded;
            if (notifyHwnd_) PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
            IncrementalRescan(); // correct a stale cache quickly on startup
        } else {
            BuildIndex();
            if (running_.load() && !cachePath_.empty()) {
                SaveIndexCache(cachePath_);
            }
        }

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

            IncrementalRescan(); // was: BuildIndex();

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
    // The effective cache path for this Start() cycle - empty means "this
    // cycle has no cache" (a scoped scan without an explicit cache path).
    std::wstring cachePath_;
    // The effective exclusions-file path for this Start() cycle - empty means
    // "this cycle has no user exclusions" (a scoped scan without an explicit
    // exclusions path; see ReloadUserExclusions()). Same safety rule as
    // cachePath_ above - a scoped test scan must never read the real user's
    // exclusions file.
    std::wstring exclusionsPath_;
    // Reloaded fresh at the top of every BuildIndex()/IncrementalRescan()
    // pass by ReloadUserExclusions() - never mutated anywhere else, and only
    // ever read by the worker thread, so it needs no mutex_ protection (same
    // reasoning as scanRootOverride_/cachePath_).
    UserExclusions userExclusions_;
    // The exclusions file's mtime as observed by the most recent
    // ReloadUserExclusions() call, and whether any call has been made yet this
    // Start() cycle. Same threading reasoning as userExclusions_ above (worker
    // thread only), reset per cycle by Start().
    fs::file_time_type lastExclusionsMtime_{};
    bool exclusionsMtimeKnown_ = false;
    bool exclusionsChangedSinceLastPass_ = false;
};

} // namespace takeoff
