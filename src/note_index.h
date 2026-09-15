#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "search.h"

namespace leanlauncher {
namespace obsidian {

namespace fs = std::filesystem;

constexpr UINT kNotesReadyMessage = WM_APP + 9;

struct NoteItem {
    std::wstring title;
    std::wstring normTitle;
    std::wstring relativeRef;    // forward-slash, no extension - vault-relative
    std::wstring folderDisplay;  // forward-slash parent folder path; empty at vault root
};

struct NoteSearchResult {
    std::wstring title;
    std::wstring relativeRef;
    std::wstring folderDisplay;
    int score = 0;
};

struct NoteIndexChunk {
    std::vector<NoteItem> items;
};

struct NoteIndexSnapshot {
    std::vector<std::shared_ptr<const NoteIndexChunk>> chunks;
    size_t totalCount = 0;
};

// Recognizes "note <text>" (case-insensitive prefix, at least one
// non-whitespace character required after it) - same trimming rules as
// daily_note.h's TryParseTaskPrefix.
inline bool TryParseNoteJumpPrefix(const std::wstring& input, std::wstring& outQuery) {
    constexpr wchar_t kPrefix[] = L"note ";
    constexpr size_t kPrefixLen = 5;
    if (input.size() <= kPrefixLen) return false;
    if (_wcsnicmp(input.c_str(), kPrefix, kPrefixLen) != 0) return false;

    std::wstring rest = input.substr(kPrefixLen);
    const size_t start = rest.find_first_not_of(L' ');
    if (start == std::wstring::npos) return false;

    outQuery = rest.substr(start);
    return !outQuery.empty();
}

// Builds a NoteItem from a vault root and an absolute note path. Purely
// lexical (fs::relative/parent_path/stem don't touch disk), so this is
// unit-testable without a real filesystem.
inline NoteItem BuildNoteItem(const fs::path& vaultRoot, const fs::path& notePath) {
    NoteItem item;
    item.title = notePath.stem().wstring();
    item.normTitle = takeoff::Normalize(item.title);

    std::error_code ec;
    fs::path rel = fs::relative(notePath, vaultRoot, ec);
    if (ec || rel.empty()) rel = notePath.filename();

    std::wstring relativeRef = (rel.parent_path() / rel.stem()).wstring();
    std::replace(relativeRef.begin(), relativeRef.end(), L'\\', L'/');
    item.relativeRef = relativeRef;

    std::wstring folderDisplay = rel.parent_path().wstring();
    std::replace(folderDisplay.begin(), folderDisplay.end(), L'\\', L'/');
    item.folderDisplay = folderDisplay;

    return item;
}

class NoteIndex {
public:
    static NoteIndex& Instance() {
        static NoteIndex s_instance;
        return s_instance;
    }

    ~NoteIndex() { Stop(); }

    void Start(const std::wstring& vaultPath, HWND notifyHwnd = nullptr) {
        if (running_.exchange(true)) return;
        vaultPath_ = vaultPath;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.reset();
            ready_ = false;
        }
    }

    // Stops the current watcher (if any) - Stop() itself resets
    // snapshot_/ready_, so a direct Start()-after-Stop() caller never
    // observes a stale "ready" index from a prior vault - and starts fresh
    // against newVaultPath. Called from Settings' vault picker when the
    // configured vault changes. newVaultPath empty just stops - the
    // caller (launcher.h) already shows "Set up your vault in Settings"
    // when obsidianVaultPath_ is empty.
    void Restart(const std::wstring& newVaultPath, HWND notifyHwnd = nullptr) {
        Stop();
        if (!newVaultPath.empty()) {
            Start(newVaultPath, notifyHwnd);
        }
    }

    bool IsReady() const { return ready_.load(); }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_ ? snapshot_->totalCount : 0;
    }

    std::vector<NoteSearchResult> Search(std::wstring_view query, size_t maxResults = 30) const {
        if (query.empty()) return {};

        std::shared_ptr<const NoteIndexSnapshot> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
        }
        if (!snapshot || snapshot->chunks.empty()) return {};

        const std::wstring normQuery = takeoff::Normalize(query);
        if (normQuery.empty()) return {};

        struct Candidate {
            int score;
            const NoteItem* item;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        for (const auto& chunk : snapshot->chunks) {
            for (const auto& item : chunk->items) {
                const int s = takeoff::MatchScore(item.normTitle, normQuery);
                if (s > 0) candidates.push_back({s, &item});
            }
        }
        if (candidates.empty()) return {};

        const size_t count = (std::min)(maxResults, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

        std::vector<NoteSearchResult> results;
        results.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            results.push_back({candidates[i].item->title, candidates[i].item->relativeRef,
                candidates[i].item->folderDisplay, candidates[i].score});
        }
        return results;
    }

private:
    NoteIndex() = default;

    void BuildIndex() {
        std::vector<NoteItem> items;
        items.reserve(256);

        std::error_code ec;
        if (!vaultPath_.empty() && fs::exists(vaultPath_, ec)) {
            fs::recursive_directory_iterator it(vaultPath_, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            while (it != end && !ec) {
                if (!running_.load()) return;
                const auto& entry = *it;
                bool isDir = entry.is_directory(ec);
                if (!ec && isDir) {
                    const std::wstring dirName = entry.path().filename().wstring();
                    if (_wcsicmp(dirName.c_str(), L".obsidian") == 0 || _wcsicmp(dirName.c_str(), L".trash") == 0) {
                        it.disable_recursion_pending();
                    }
                    it.increment(ec);
                    continue;
                }
                if (!ec && entry.is_regular_file(ec)) {
                    const std::wstring ext = entry.path().extension().wstring();
                    if (_wcsicmp(ext.c_str(), L".md") == 0) {
                        items.push_back(BuildNoteItem(vaultPath_, entry.path()));
                    }
                }
                it.increment(ec);
            }
        }

        if (!running_.load()) return;

        auto chunk = std::make_shared<const NoteIndexChunk>(NoteIndexChunk{std::move(items)});
        auto newSnapshot = std::make_shared<NoteIndexSnapshot>();
        newSnapshot->totalCount = chunk->items.size();
        newSnapshot->chunks.push_back(std::move(chunk));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = std::move(newSnapshot);
            ready_ = true;
        }

        if (notifyHwnd_) {
            PostMessageW(notifyHwnd_, kNotesReadyMessage, 0, 0);
        }
    }

    void WorkerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        BuildIndex();

        HANDLE hVaultChange = INVALID_HANDLE_VALUE;
        if (!vaultPath_.empty()) {
            hVaultChange = FindFirstChangeNotificationW(vaultPath_.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        }

        std::vector<HANDLE> waitHandles;
        if (stopEvent_) waitHandles.push_back(stopEvent_);
        if (triggerEvent_) waitHandles.push_back(triggerEvent_);
        if (hVaultChange != INVALID_HANDLE_VALUE && hVaultChange != nullptr) waitHandles.push_back(hVaultChange);

        while (running_.load()) {
            DWORD wait = WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                FALSE,
                300000  // 5-minute periodic idle scan, matches FileIndex
            );

            if (!running_.load()) break;
            if (wait == WAIT_OBJECT_0) break;  // stopEvent_

            // Debounce burst filesystem activity (e.g. Obsidian syncing many
            // files at once).
            WaitForSingleObject(stopEvent_, 3000);
            if (!running_.load()) break;

            BuildIndex();

            if (hVaultChange != INVALID_HANDLE_VALUE && hVaultChange != nullptr) {
                FindNextChangeNotification(hVaultChange);
            }
        }

        if (hVaultChange != INVALID_HANDLE_VALUE && hVaultChange != nullptr) {
            FindCloseChangeNotification(hVaultChange);
        }
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    mutable std::mutex mutex_;
    std::shared_ptr<const NoteIndexSnapshot> snapshot_;
    std::thread worker_;
    HANDLE stopEvent_ = nullptr;
    HANDLE triggerEvent_ = nullptr;
    HWND notifyHwnd_ = nullptr;
    std::wstring vaultPath_;
};

}  // namespace obsidian
}  // namespace leanlauncher
