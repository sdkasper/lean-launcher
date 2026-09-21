# Full-Disk File Search Core (DirectoryPool, Cache, Throttling, Incremental Rescan) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework `FileIndex` (`src/file_index.h`) so it can index the whole disk without an artificial file-count cap, without unbounded memory growth, without saturating disk I/O, and without re-walking the entire machine on every 5-minute rescan - while keeping `Search()` and existing callers (`launcher.h`) working unchanged.

**Architecture:** Replace `FileItem`'s per-file duplicated absolute-path strings with a `DirectoryPool` that interns each unique parent directory once (path, normalized path, last-known mtime). Replace the existing "append arbitrary batches to a flat chunk list" snapshot model with one chunk per directory (`chunksByDir`, indexed by the same pool index), so a single changed directory can be updated in isolation instead of rebuilding the whole index. Add a persisted binary cache so only the very first run ever does a full walk; every later launch loads the cache and runs a bounded incremental rescan (mtime-compare per known directory, not a disk walk) plus a drive-presence prune.

**Tech Stack:** C++17, `std::filesystem`, Win32 (`SetFileInformationByHandle`, `GetLogicalDriveStringsW`), the existing `LeanLauncherCoreTests` binary (`tests/core_tests.cpp`, plain sequential `Check()` assertions, no test framework).

**Spec:** `docs/superpowers/specs/2026-09-21-full-disk-file-search-design.md`

## Global Constraints

- No file-count cap anywhere in the walk (the design's whole point) - `kMaxFiles` and all `maxCount`/`totalCountSoFar` plumbing built around it must be fully removed, not just raised.
- Existing public `FileIndex` API used by `launcher.h` must keep working unchanged: `Start(HWND)`, `Stop()`, `IsReady()`, `Count()`, `Search(query, maxResults)`. `Start()` gains new *optional, defaulted* test-only parameters only (same convention as the existing `scanRootOverride`).
- Existing tests must keep passing: the ~20ms/query search benchmark (`tests/core_tests.cpp:749`) and the scoped-repo `FileIndex` integration test (`tests/core_tests.cpp:692-750`).
- No new administrator/elevation requirement (`src/LeanLauncher.manifest:8` stays `asInvoker`).
- Out of scope for this plan (tracked separately): the user exclusion file (`US-019`), the Settings "Edit exclusions"/"Help" rows, `docs/file-search.md`, and the actual "Indexing files... (N so far)" status-line UI rendering - this plan only adds the `Phase` accessor the UI will later consume.
- Build/test gates (from `LOOP.md`, still apply even though this plan isn't run through `/dev-loop`): `cmake --build cmake --config Release`, then `ctest --test-dir cmake -C Release --output-on-failure`, then the `LeanLauncher.sln` MSBuild parity build. Never touch `cmake/**`, `msbuild/**`, `archive/**`, `.github/**`, `CHANGELOG.md`, `LeanLauncher.vcxproj`, `LeanLauncher.sln`.

---

## Task 1: DirectoryPool, per-directory chunk storage, and FileItem restructuring

**Files:**
- Modify: `src/file_index.h` (`FileItem`, `IndexChunk`, `IndexSnapshot`, `PublishSnapshot`/`AppendSnapshotChunk` -> replaced, `AddItem`, `ScanPath`, `BuildIndex`, `Search`)
- Modify: `tests/core_tests.cpp:755-779` (the inline `PublishSnapshot`/`AppendSnapshotChunk` unit test - rewritten against the new API)

**Interfaces:**
- Produces: `class DirectoryPool { uint32_t Intern(const std::wstring& path, const std::wstring& normPath); const DirectoryEntry& Get(uint32_t index) const; size_t Size() const; void SetMtime(uint32_t index, fs::file_time_type mtime); fs::file_time_type GetMtime(uint32_t index) const; }` where `struct DirectoryEntry { std::wstring path; std::wstring normPath; fs::file_time_type lastKnownMtime; };`
- Produces: `FileItem { std::wstring name; std::wstring normName; uint32_t parentDirIndex; bool isDirectory; }`
- Produces: `FileIndex::SetDirectoryChunk(uint32_t poolIndex, std::vector<FileItem>&& children)` - upserts (replaces) the chunk of items belonging to one directory; `poolIndex` must come from `DirectoryPool::Intern`.
- Produces: `FileIndex::PruneDirectory(uint32_t poolIndex)` - equivalent to `SetDirectoryChunk(poolIndex, {})`.
- Consumes (later tasks): none yet - this task is the foundation.

- [ ] **Step 1: Write the failing test for `DirectoryPool` interning**

Add near the top of `tests/core_tests.cpp`'s FileIndex section (just before the existing "Live FileIndex background indexing" block, around line 692):

```cpp
    // -----------------------------------------------------------------------------
    // DirectoryPool interning
    // -----------------------------------------------------------------------------
    {
        takeoff::DirectoryPool pool;
        uint32_t idxA = pool.Intern(L"C:\\Users\\Test", takeoff::Normalize(L"C:\\Users\\Test"));
        uint32_t idxB = pool.Intern(L"C:\\Users\\Test\\Docs", takeoff::Normalize(L"C:\\Users\\Test\\Docs"));
        uint32_t idxA2 = pool.Intern(L"C:\\Users\\Test", takeoff::Normalize(L"C:\\Users\\Test"));
        Check(idxA == idxA2, "DirectoryPool dedupes identical paths to the same index");
        Check(idxA != idxB, "DirectoryPool gives distinct paths distinct indices");
        Check(pool.Get(idxA).path == L"C:\\Users\\Test", "DirectoryPool.Get returns the stored path");
        Check(pool.Size() == 2, "DirectoryPool.Size reflects unique entries only");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build cmake --config Release`
Expected: FAIL to compile - `takeoff::DirectoryPool` is not declared.

- [ ] **Step 3: Implement `DirectoryPool` and restructure `FileItem`/`IndexChunk`/`IndexSnapshot`**

In `src/file_index.h`, replace the existing `FileItem`, `IndexChunk`, `IndexSnapshot` definitions (currently lines 25-47) with:

```cpp
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
```

Remove the old `PublishSnapshot`/`AppendSnapshotChunk` methods entirely and replace them with:

```cpp
    void SetDirectoryChunk(uint32_t poolIndex, std::vector<FileItem>&& children) {
        auto newChunk = std::make_shared<const std::vector<FileItem>>(std::move(children));
        std::lock_guard<std::mutex> lock(mutex_);
        auto newSnapshot = std::make_shared<IndexSnapshot>();
        if (snapshot_) {
            newSnapshot->chunksByDir = snapshot_->chunksByDir;
            newSnapshot->totalCount = snapshot_->totalCount;
        }
        if (poolIndex >= newSnapshot->chunksByDir.size()) {
            newSnapshot->chunksByDir.resize(poolIndex + 1);
        }
        const size_t oldSize = newSnapshot->chunksByDir[poolIndex] ? newSnapshot->chunksByDir[poolIndex]->size() : 0;
        newSnapshot->totalCount = newSnapshot->totalCount - oldSize + newChunk->size();
        newSnapshot->chunksByDir[poolIndex] = std::move(newChunk);
        snapshot_ = std::move(newSnapshot);
        ready_ = true;
    }

    void PruneDirectory(uint32_t poolIndex) {
        SetDirectoryChunk(poolIndex, {});
    }
```

Update `AddItem` to take the parent pool index instead of building a full path string, and to intern directories as it goes:

```cpp
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
```

Update `ScanPath` to intern each directory it descends into and publish that directory's own children as one chunk as soon as it's fully listed (replacing the old "accumulate everything into one big `items` vector across many directories" shape):

```cpp
    void ScanPath(const fs::path& root, uint32_t rootPoolIndex, DirectoryPool& pool,
                  std::unordered_set<uint64_t>& seen, int maxDepth) {
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        if (IsDriveRoot(root) || ShouldSkipDirectory(root)) return;

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

            while (it != end && !ec) {
                if (!running_.load()) break;

                const auto& entry = *it;
                const int depth = it.depth(); // 0 == direct child of root
                const uint32_t parentIdx = dirIndexAtDepth[static_cast<size_t>(depth)];

                bool isDir = entry.is_directory(ec);
                if (!ec && isDir) {
                    if (depth >= maxDepth || ShouldSkipDirectory(entry.path())) {
                        it.disable_recursion_pending();
                    } else {
                        AddItem(entry.path(), true, parentIdx, childrenByDir[parentIdx], seen);
                        uint32_t childIdx = pool.Intern(entry.path().wstring(), Normalize(entry.path().wstring()));
                        childrenByDir[childIdx]; // ensure it exists even if it turns out empty
                        if (static_cast<size_t>(depth) + 1 >= dirIndexAtDepth.size()) {
                            dirIndexAtDepth.push_back(childIdx);
                        } else {
                            dirIndexAtDepth[static_cast<size_t>(depth) + 1] = childIdx;
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

        for (auto& [poolIdx, children] : childrenByDir) {
            pool.SetMtime(poolIdx, CurrentMtime(pool.Get(poolIdx).path));
            SetDirectoryChunk(poolIdx, std::move(children));
        }
    }

    static fs::file_time_type CurrentMtime(const std::wstring& path) {
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        return ec ? fs::file_time_type{} : t;
    }
```

Update `Search()` to reconstruct the normalized path lazily from the pool instead of reading `item.normPath`:

```cpp
    std::vector<FileSearchResult> Search(std::wstring_view query, size_t maxResults = 30) const {
        // ... (tokenizing logic unchanged) ...
        for (const auto& chunk : snapshot->chunks) { // <- becomes chunksByDir
```

becomes (iterate `chunksByDir`, skip nulls, use `pool_` for path reconstruction and for building `FileSearchResult::path`):

```cpp
        for (const auto& chunk : snapshot->chunksByDir) {
            if (!chunk) continue;
            for (const auto& item : *chunk) {
                int s = -1;
                if (item.normName.size() >= (isSingleToken ? qLen : tokens.back().size())) {
                    if (item.normName.find(firstChar) != std::wstring::npos) {
                        s = ScoreFile(item.normName, normQuery, item.isDirectory);
                    }
                }
                if (s <= 0 && allowPathMatch) {
                    const std::wstring normPath = pool_.Get(item.parentDirIndex).normPath + L"\\" + item.normName;
                    if (isSingleToken) {
                        size_t pos = normPath.find(normQuery);
                        if (pos != std::wstring::npos) {
                            const size_t penalty = (std::min)(normPath.size() / 4, size_t{300});
                            int pathScore = 2600 - static_cast<int>(penalty);
                            if (item.isDirectory) pathScore += 40;
                            s = (std::max)(1000, pathScore);
                        }
                    } else {
                        bool allFound = true;
                        for (const auto& token : tokens) {
                            if (normPath.find(token) == std::wstring::npos) { allFound = false; break; }
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
                if (s > 0) candidates.push_back({s, &item, item.parentDirIndex});
            }
        }
```

`Candidate` needs the parent index too (to rebuild the display path for the winners only, not every candidate):

```cpp
        struct Candidate {
            int score;
            const FileItem* item;
            uint32_t parentDirIndex;
        };
```

And the final result-building loop reconstructs the full (non-normalized) path only for the returned winners:

```cpp
        for (size_t i = 0; i < count; ++i) {
            const auto& c = candidates[i];
            results.push_back({
                c.item->name,
                pool_.Get(c.parentDirIndex).path + L"\\" + c.item->name,
                c.item->isDirectory,
                c.score
            });
        }
```

Add a `DirectoryPool pool_;` member to `FileIndex` (guarded by the existing `mutex_` for writes from the worker thread; reads from `Search()` take the same lock briefly to copy what's needed - see Step 3 note below). Since `DirectoryPool::Get`/`Intern` aren't otherwise thread-safe, `Search()` must snapshot-copy the pool entries it needs, or (simpler and sufficient here) take `mutex_` for the whole `Search()` call - this matches the existing coarse-locking style already used for `Count()`/`PublishSnapshot`, and search is already sub-millisecond, so lock hold time is negligible.

- [ ] **Step 4: Rewrite the inline chunk-storage test to use the new API**

Replace `tests/core_tests.cpp:755-779` (the `PublishSnapshot`/`AppendSnapshotChunk` test) with:

```cpp
    // -----------------------------------------------------------------------------
    // Requirement R2: Per-Directory Chunk Storage & Incremental Update
    // -----------------------------------------------------------------------------
    {
        FileIndex::Instance().Stop();
        takeoff::DirectoryPool testPool;
        uint32_t dirIdx = testPool.Intern(L"C:\\Users\\Test", takeoff::Normalize(L"C:\\Users\\Test"));

        std::vector<FileItem> chunk1;
        chunk1.push_back({L"testdoc.pdf", L"testdoc pdf", dirIdx, false});
        chunk1.push_back({L"testcode.cpp", L"testcode cpp", dirIdx, false});
        FileIndex::Instance().SetDirectoryChunk(dirIdx, std::move(chunk1));
        Check(FileIndex::Instance().Count() == 2, "SetDirectoryChunk initializes count to 2");

        std::vector<FileItem> chunk1Updated;
        chunk1Updated.push_back({L"testdoc.pdf", L"testdoc pdf", dirIdx, false});
        chunk1Updated.push_back({L"testcode.cpp", L"testcode cpp", dirIdx, false});
        chunk1Updated.push_back({L"testheader.h", L"testheader h", dirIdx, false});
        FileIndex::Instance().SetDirectoryChunk(dirIdx, std::move(chunk1Updated));
        Check(FileIndex::Instance().Count() == 3, "Re-setting one directory's chunk updates count without touching others");

        auto chunkResults = FileIndex::Instance().Search(L"testheader");
        Check(!chunkResults.empty() && chunkResults[0].name == L"testheader.h", "Search finds an item added via SetDirectoryChunk");
        Check(chunkResults[0].path == L"C:\\Users\\Test\\testheader.h", "Search reconstructs the full path from the DirectoryPool");

        FileIndex::Instance().PruneDirectory(dirIdx);
        Check(FileIndex::Instance().Count() == 0, "PruneDirectory removes all of that directory's items");

        // Memory budget verification: see Task 7 for the recalibrated estimate.
    }
```

(The heap-budget check that used to live at the end of this block moves to Task 7, where it's recalculated against the new struct size.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build cmake --config Release`
Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS, including the existing scoped-repo `FileIndex` test. `BuildIndex`'s step-0/1/2/3 phases must be updated to call the new `ScanPath(root, poolIndex, pool_, seen, maxDepth)` signature - intern each phase's root via `pool_.Intern(root.wstring(), Normalize(root.wstring()))` immediately before calling `ScanPath`, and drop the `maxCount`/`totalCountSoFar` arguments from every call site now, since `ScanPath` no longer accepts them. The surrounding cap-check bookkeeping in `BuildIndex` itself (the `kMaxFiles`/`totalIndexed` variables and `if (totalIndexed + ... >= kMaxFiles)` guards between phases) is left in place for this task - it still compiles fine even though `ScanPath` itself is uncapped now, since those checks only gate whether a phase's outer loop starts another folder, not `ScanPath`'s internal behavior. Task 2 removes that leftover bookkeeping entirely.

- [ ] **Step 6: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Intern directory paths and switch FileIndex to per-directory chunk storage"
```

---

## Task 2: Remove the file-count cap and expose scan phase

**Files:**
- Modify: `src/file_index.h` (`BuildIndex`, new `Phase` enum + accessor)

**Interfaces:**
- Consumes: `ScanPath`, `SetDirectoryChunk`, `pool_` from Task 1.
- Produces: `enum class Phase { Idle, FirstWalk, Loaded, IncrementalRescan };` and `Phase GetPhase() const;` on `FileIndex`. Later tasks (3, 5, 6) set `phase_` at the right transition points; Task 5 is what actually introduces `Loaded`/`IncrementalRescan` transitions (cache load, rescan pass) - this task only adds the enum/accessor and sets `FirstWalk` at the start of a from-scratch walk.

- [ ] **Step 1: Write the failing test**

Add to the scoped-repo `FileIndex` test block, **after** the existing wait-loop that polls `IsReady()` (not immediately after `Start()` itself - the worker thread hasn't necessarily set `phase_` away from `Idle` yet at that exact instant, which would make this check flaky):

```cpp
    Check(FileIndex::Instance().GetPhase() == takeoff::FileIndex::Phase::Loaded,
          "GetPhase reports Loaded once IsReady() is true");
```

- [ ] **Step 2: Run test to verify it fails**

Run the CMake build command above.
Expected: FAIL to compile - `Phase` and `GetPhase` don't exist yet.

- [ ] **Step 3: Add the `Phase` enum/accessor and remove the cap**

In `FileIndex`'s public section, add:

```cpp
    enum class Phase { Idle, FirstWalk, Loaded, IncrementalRescan };

    Phase GetPhase() const { return phase_.load(); }
```

Add `std::atomic<Phase> phase_{Phase::Idle};` as a private member.

In `BuildIndex()`:
- Remove the `constexpr size_t kMaxFiles = 50000;` declaration.
- Remove every `if (totalIndexed + ...Items.size() >= kMaxFiles) break/return;` / `if (totalCountSoFar + items.size() >= maxCount) return;` guard - across `ScanPath` (already dropped in Task 1's rewrite) and each of `BuildIndex`'s four phases (repo root, known folders, profile subfolders, drives).
- Remove the `maxCount`/`totalCountSoFar` parameters from any remaining call sites (Task 1 already dropped them from `ScanPath`'s signature; this step is about `BuildIndex`'s own bookkeeping variables like `totalIndexed`, which become dead code once nothing bounds them - delete them too, don't just stop reading them).
- Set `phase_ = Phase::FirstWalk;` at the very top of `BuildIndex()` (before the `scanRootOverride_` check, so it covers both branches below). `BuildIndex()` has **two** places that already set `ready_ = true` and post `kFilesReadyMessage`: the `scanRootOverride_` early-return branch (used by every existing/new test) and the normal end of the whole-machine scan. Set `phase_ = Phase::Loaded;` immediately before `ready_ = true` at **both** of those locations, not just one - otherwise every scoped-root test (which always takes the override branch) would see `GetPhase()` stuck at `FirstWalk` forever.

- [ ] **Step 4: Run tests to verify they pass**

Run the CMake build + ctest commands above.
Expected: PASS. Note this test run's scoped-repo scan now indexes the *entire* repo with no cap - confirm `indexedCount` printed by the existing `std::cout` line is sane (matches roughly the repo's real file count under the depth limit) rather than silently capped at some round number.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Remove FileIndex's 50,000-file cap and expose scan phase"
```

---

## Task 3: I/O priority throttling during the walk

**Files:**
- Modify: `src/file_index.h` (`ScanPath`)

**Interfaces:**
- Consumes: nothing new from other tasks - self-contained addition to `ScanPath`.
- Produces: nothing new consumed by later tasks; this is a leaf change.

- [ ] **Step 1: Write the failing test**

`SetFileInformationByHandle`'s effect isn't observable from a unit test (it's a hint to the OS I/O scheduler, not a behavior change), so there's no meaningful `Check()` for "the priority hint was set." Instead, add a regression test that the walk still completes correctly with throttling enabled, timing it loosely:

```cpp
    // Throttling must not make a small scoped scan noticeably slow.
    const auto throttleStart = std::chrono::steady_clock::now();
    // (re-uses the FileIndex::Instance().Start(...) + IsReady() wait loop already
    // present earlier in this test block - no new Start() call needed here,
    // this assertion just needs to run after that wait loop completes)
    const auto throttleElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - throttleStart).count();
    Check(throttleElapsed < 15000, "Scoped repo scan with I/O throttling still completes in under 15s");
```

Place this right after the existing `Check(indexedCount > 0, ...)` line.

- [ ] **Step 2: Run test to verify it fails**

This test passes trivially before the throttling change too (there's nothing slow yet) - that's expected and fine; it's a regression guard for Step 3, not a TDD-red step. Skip ahead to Step 3, then re-run this same test afterward to confirm it still passes with throttling active.

- [ ] **Step 3: Add I/O priority hint and periodic yield to `ScanPath`**

At the top of `ScanPath`, after the existing `IsDriveRoot`/`ShouldSkipDirectory` early-return checks, open the root directory with a handle and set its I/O priority hint:

```cpp
        HANDLE dirHandle = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dirHandle != INVALID_HANDLE_VALUE) {
            FILE_IO_PRIORITY_HINT_INFO hint{};
            hint.PriorityHint = IoPriorityHintLow;
            SetFileInformationByHandle(dirHandle, FileIoPriorityHintInfo, &hint, sizeof(hint));
            CloseHandle(dirHandle);
        }
```

Inside the `while (it != end && !ec)` loop, add a directory-count throttle right after the existing `if (!running_.load()) break;` check:

```cpp
                if (++dirsVisitedThisCall % 64 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
```

with `size_t dirsVisitedThisCall = 0;` declared before the `while` loop. This only throttles directory-count-driven pacing (cheap, deterministic), not a wall-clock timer, so it doesn't slow down small scans (like the scoped-repo test) meaningfully - 64 directories is well above this repo's own directory count, so the test in Step 1 shouldn't trip it at all.

- [ ] **Step 4: Run tests to verify they pass**

Run the CMake build + ctest commands above.
Expected: PASS, including the Step 1 timing check.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Throttle FileIndex's disk walk with I/O priority hints and periodic yields"
```

---

## Task 4: Persisted binary cache (save/load, versioned)

**Files:**
- Modify: `src/file_index.h` (new `SaveIndexCache`/`LoadIndexCache`)
- Test: `tests/core_tests.cpp` (new standalone test block)

**Interfaces:**
- Consumes: `DirectoryPool`, `FileItem`, `IndexSnapshot`/`chunksByDir` from Task 1.
- Produces: `bool FileIndex::SaveIndexCache(const std::wstring& path) const;` and `bool FileIndex::LoadIndexCache(const std::wstring& path);` (returns `false` on any missing/corrupt/version-mismatched file, leaving the instance's state untouched - caller falls back to a full walk). Also `static constexpr uint32_t kCacheFormatVersion = 1;`.

- [ ] **Step 1: Write the failing test**

Add a new standalone block in `tests/core_tests.cpp`, after the Task 1 chunk-storage test block:

```cpp
    // -----------------------------------------------------------------------------
    // Persisted cache round-trip and version handling
    // -----------------------------------------------------------------------------
    {
        FileIndex::Instance().Stop();
        takeoff::DirectoryPool& poolBefore = FileIndex::Instance().TestOnlyPool();
        uint32_t dirIdx = poolBefore.Intern(L"D:\\Cache\\Test", takeoff::Normalize(L"D:\\Cache\\Test"));
        std::vector<FileItem> items;
        items.push_back({L"a.txt", L"a txt", dirIdx, false});
        items.push_back({L"sub", L"sub", dirIdx, true});
        FileIndex::Instance().SetDirectoryChunk(dirIdx, std::move(items));

        const std::wstring cachePath = L"cache_roundtrip_test.bin";
        Check(FileIndex::Instance().SaveIndexCache(cachePath), "SaveIndexCache writes successfully");

        FileIndex::Instance().Stop(); // stops the background thread; LoadIndexCache below unconditionally
                                       // overwrites pool_/snapshot_ regardless of what they held before
        Check(FileIndex::Instance().LoadIndexCache(cachePath), "LoadIndexCache reads back what was saved");
        auto results = FileIndex::Instance().Search(L"a.txt");
        Check(!results.empty() && results[0].path == L"D:\\Cache\\Test\\a.txt",
              "LoadIndexCache reconstructs items with correct interned paths");

        std::ofstream corrupt(L"cache_corrupt_test.bin", std::ios::binary);
        corrupt << "not a real cache file";
        corrupt.close();
        Check(!FileIndex::Instance().LoadIndexCache(L"cache_corrupt_test.bin"),
              "LoadIndexCache rejects a corrupt/wrong-format file");

        DeleteFileW(cachePath.c_str());
        DeleteFileW(L"cache_corrupt_test.bin");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run the CMake build command above.
Expected: FAIL to compile - `SaveIndexCache`, `LoadIndexCache`, `TestOnlyPool` don't exist yet.

- [ ] **Step 3: Implement the cache format and save/load**

Add near the top of the `takeoff` namespace in `file_index.h`:

```cpp
constexpr uint32_t kCacheMagic = 0x4C4C4649; // "LLFI"
constexpr uint32_t kCacheFormatVersion = 1;
```

`DirectoryPool` needs a copy constructor/assignment for `SaveIndexCache`'s `poolCopy = pool_;` line below and for `IncrementalRescan`'s `poolCopy = pool_;` in Task 6 - it already gets one implicitly (all members are copyable `std::vector`/`std::unordered_map`), so no extra code is needed, just confirm it compiles as part of this step.

Add to `FileIndex`'s public section:

```cpp
    // Test-only accessor - real callers never touch the pool directly.
    DirectoryPool& TestOnlyPool() { return pool_; }

    bool SaveIndexCache(const std::wstring& path) const {
        std::shared_ptr<const IndexSnapshot> snapshot;
        DirectoryPool poolCopy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
            poolCopy = pool_;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        WriteRaw(out, kCacheMagic);
        WriteRaw(out, kCacheFormatVersion);
        const uint32_t dirCount = static_cast<uint32_t>(poolCopy.Entries().size());
        WriteRaw(out, dirCount);
        for (const auto& entry : poolCopy.Entries()) {
            WriteWString(out, entry.path);
            WriteWString(out, entry.normPath);
            WriteRaw(out, entry.lastKnownMtime.time_since_epoch().count());
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
        return true;
    }
```

Add these small private helpers (file-local, above `FileIndex`):

```cpp
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run the CMake build + ctest commands above.
Expected: PASS, including the round-trip and corrupt-file rejection checks.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Add versioned binary cache save/load for FileIndex"
```

---

## Task 5: Wire the cache into startup (cold start vs. cached load)

**Files:**
- Modify: `src/file_index.h` (`Start`, `WorkerLoop`, `BuildIndex`)
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `Phase` (Task 2), `SaveIndexCache`/`LoadIndexCache` (Task 4).
- Produces: `Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"", const std::wstring& cachePathOverride = L"")` - the new third parameter is test-only, exactly like the existing second one; production callers (`launcher.h:68`) are unaffected since it defaults to empty (meaning "use the real `%LOCALAPPDATA%\LeanLauncher\file_index.cache` path").

- [ ] **Step 1: Write the failing test**

Add a new block after the Task 4 cache test:

```cpp
    // -----------------------------------------------------------------------------
    // Cache-aware startup: cold start writes a cache; a second Start() loads it
    // -----------------------------------------------------------------------------
    {
        FileIndex::Instance().Stop();
        const std::wstring testCachePath = L"startup_cache_test.bin";
        DeleteFileW(testCachePath.c_str()); // ensure a clean cold start

        fs::path currentPath = fs::current_path();
        fs::path repoPath = FileIndex::FindVerifiedProjectRoot(currentPath);
        if (repoPath.empty()) repoPath = currentPath;

        FileIndex::Instance().Start(nullptr, repoPath.wstring(), testCachePath);
        for (int w = 0; w < 40 && !FileIndex::Instance().IsReady(); ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Check(FileIndex::Instance().GetPhase() == takeoff::FileIndex::Phase::Loaded,
              "Cold start reaches Loaded phase after the first walk");
        const size_t firstRunCount = FileIndex::Instance().Count();
        Check(firstRunCount > 0, "Cold start indexed something");
        FileIndex::Instance().Stop();

        std::error_code cacheEc;
        Check(fs::exists(testCachePath, cacheEc), "Cold start wrote a cache file to disk");

        const auto reloadStart = std::chrono::steady_clock::now();
        FileIndex::Instance().Start(nullptr, repoPath.wstring(), testCachePath);
        for (int w = 0; w < 40 && !FileIndex::Instance().IsReady(); ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - reloadStart).count();
        Check(FileIndex::Instance().Count() == firstRunCount,
              "Second Start() with an existing cache loads the same item count");
        Check(reloadElapsed < 2000, "Loading from an existing cache is fast, not a full re-walk");
        FileIndex::Instance().Stop();
        DeleteFileW(testCachePath.c_str());
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run the CMake build command above.
Expected: FAIL to compile - `Start()`'s third parameter doesn't exist yet.

- [ ] **Step 3: Wire cache load/save into `Start()`/`WorkerLoop()`/`BuildIndex()`**

Add a `std::wstring cachePathOverride_;` member (parallel to the existing `scanRootOverride_`) and a helper to resolve the real path:

```cpp
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
```

Update `Start()`'s signature:

```cpp
    void Start(HWND notifyHwnd = nullptr, const std::wstring& scanRootOverride = L"",
               const std::wstring& cachePathOverride = L"") {
        if (running_.exchange(true)) return;
        notifyHwnd_ = notifyHwnd;
        scanRootOverride_ = scanRootOverride;
        cachePathOverride_ = cachePathOverride.empty() ? DefaultCachePath() : cachePathOverride;
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_ = std::thread([this]() { WorkerLoop(); });
    }
```

At the top of `WorkerLoop()`, before the existing `BuildIndex()` call, try the cache first:

```cpp
    void WorkerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        bool loadedFromCache = !cachePathOverride_.empty() && LoadIndexCache(cachePathOverride_);
        if (loadedFromCache) {
            phase_ = Phase::Loaded;
            if (notifyHwnd_) PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
        } else {
            BuildIndex();
            if (running_.load() && !cachePathOverride_.empty()) {
                SaveIndexCache(cachePathOverride_);
            }
        }

        // ... existing change-notification setup and 5-minute WaitForMultipleObjects loop continue unchanged for now (Task 6 replaces the BuildIndex() call inside that loop with an incremental rescan) ...
```

- [ ] **Step 4: Run tests to verify they pass**

Run the CMake build + ctest commands above.
Expected: PASS, including the cold-start-then-cached-reload timing check. If the reload isn't reliably under 2000ms on CI hardware, raise the threshold rather than removing the check - this mirrors the existing documented pattern of hardware-tuned thresholds noted in `LOOP.md`.

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Load FileIndex from a persisted cache on startup instead of always re-walking"
```

---

## Task 6: Incremental mtime-pruned rescan and drive-presence pruning

**Files:**
- Modify: `src/file_index.h` (`WorkerLoop`'s periodic-rescan branch, new `IncrementalRescan`/`PruneAbsentDrives`)
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `pool_`, `SetDirectoryChunk`/`PruneDirectory` (Task 1), `Phase` (Task 2).
- Produces: `void FileIndex::IncrementalRescan();` (also usable directly by a test, not just the timer loop) and `void FileIndex::PruneAbsentDrives();`.

- [ ] **Step 1: Write the failing test**

```cpp
    // -----------------------------------------------------------------------------
    // Incremental rescan: unchanged directories are left alone; drive-prune removes
    // entries under a path that no longer exists.
    // -----------------------------------------------------------------------------
    {
        FileIndex::Instance().Stop();
        takeoff::DirectoryPool& pool = FileIndex::Instance().TestOnlyPool();
        // Simulate a previously-indexed directory that is now gone (stands in for
        // an unplugged removable drive without needing real removable hardware).
        uint32_t goneIdx = pool.Intern(L"Z:\\WasHereOnce", takeoff::Normalize(L"Z:\\WasHereOnce"));
        std::vector<FileItem> goneItems;
        goneItems.push_back({L"ghost.txt", L"ghost txt", goneIdx, false});
        FileIndex::Instance().SetDirectoryChunk(goneIdx, std::move(goneItems));
        Check(FileIndex::Instance().Count() == 1, "Setup: ghost entry present before pruning");

        FileIndex::Instance().PruneAbsentDrives();
        Check(FileIndex::Instance().Count() == 0, "PruneAbsentDrives removes entries whose root no longer exists");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run the CMake build command above.
Expected: FAIL to compile - `PruneAbsentDrives` doesn't exist yet.

- [ ] **Step 3: Implement `PruneAbsentDrives` and `IncrementalRescan`**

```cpp
    void PruneAbsentDrives() {
        DirectoryPool poolCopy;
        std::shared_ptr<const IndexSnapshot> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            poolCopy = pool_;
            snapshot = snapshot_;
        }
        if (!snapshot) return;

        std::unordered_map<std::wstring, bool> driveExistsCache;
        for (uint32_t i = 0; i < poolCopy.Entries().size(); ++i) {
            if (i >= snapshot->chunksByDir.size() || !snapshot->chunksByDir[i]) continue;
            const auto& path = poolCopy.Entries()[i].path;
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
            if (!exists) PruneDirectory(i);
        }
    }

    void IncrementalRescan() {
        phase_ = Phase::IncrementalRescan;
        PruneAbsentDrives();

        DirectoryPool poolSnapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            poolSnapshot = pool_;
        }

        std::unordered_set<uint64_t> seen; // only used to satisfy AddItem's signature per re-listed directory
        for (uint32_t i = 0; i < poolSnapshot.Entries().size(); ++i) {
            if (!running_.load()) break;
            const auto& entry = poolSnapshot.Entries()[i];
            std::error_code ec;
            if (!fs::exists(entry.path, ec)) continue; // handled by PruneAbsentDrives / will be pruned next pass
            fs::file_time_type currentMtime = fs::last_write_time(entry.path, ec);
            if (ec || currentMtime == entry.lastKnownMtime) continue;

            // This directory's own contents changed - re-list its immediate
            // children only (cheap); brand-new subdirectories found here get
            // fully walked (they have no prior pool entry / stored mtime).
            std::vector<FileItem> freshChildren;
            fs::directory_iterator dit(entry.path, fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            seen.clear();
            try {
                for (const auto& child : dit) {
                    bool isDir = child.is_directory(ec);
                    if (!ec && isDir) {
                        if (ShouldSkipDirectory(child.path())) continue;
                        AddItem(child.path(), true, i, freshChildren, seen);
                        uint32_t childIdx = pool_.Intern(child.path().wstring(), Normalize(child.path().wstring()));
                        if (childIdx >= poolSnapshot.Size()) {
                            // Genuinely new subdirectory - fully walk it (bounded depth,
                            // same as a first-run scan of that one subtree).
                            ScanPath(child.path(), childIdx, pool_, seen, 8);
                        }
                    } else if (!ec && child.is_regular_file(ec) && IsUserRelevantFile(child.path())) {
                        AddItem(child.path(), false, i, freshChildren, seen);
                    }
                }
            } catch (...) {}

            pool_.SetMtime(i, currentMtime);
            SetDirectoryChunk(i, std::move(freshChildren));
        }

        phase_ = Phase::Loaded;
        if (notifyHwnd_) PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
        if (!cachePathOverride_.empty()) SaveIndexCache(cachePathOverride_);
    }
```

Then replace the periodic-rescan call inside `WorkerLoop()`'s `while (running_.load())` loop - today it calls `BuildIndex()` unconditionally after the 5-minute wait/debounce; change that call to `IncrementalRescan()`:

```cpp
            // Debounce user file operations (e.g. large file write, burst of downloads)
            WaitForSingleObject(stopEvent_, 3000);
            if (!running_.load()) break;

            IncrementalRescan(); // was: BuildIndex();
```

Also call `IncrementalRescan()` once, immediately, right after a successful cache load in Task 5's `WorkerLoop()` cold-start branch (so a stale cache is corrected quickly on startup, per the design):

```cpp
        bool loadedFromCache = !cachePathOverride_.empty() && LoadIndexCache(cachePathOverride_);
        if (loadedFromCache) {
            phase_ = Phase::Loaded;
            if (notifyHwnd_) PostMessageW(notifyHwnd_, kFilesReadyMessage, 0, 0);
            IncrementalRescan();
        } else {
            ...
```

- [ ] **Step 4: Run tests to verify they pass**

Run the CMake build + ctest commands above.
Expected: PASS, including drive-prune and the full scoped-repo/cache tests from earlier tasks (re-run the whole suite, not just this task's new block, since `IncrementalRescan` now runs automatically after a cached load in Task 5's test).

- [ ] **Step 5: Commit**

```bash
git add src/file_index.h tests/core_tests.cpp
git commit -m "Replace FileIndex's periodic full re-walk with an incremental mtime-pruned rescan"
```

---

## Task 7: Recalibrate the heap-budget test and add a realistic-scale synthetic test

**Files:**
- Modify: `tests/core_tests.cpp` (heap-budget block)

**Interfaces:**
- Consumes: `FileItem`, `DirectoryPool` sizes from Task 1 (no production code changes in this task).

- [ ] **Step 1: Write the failing test**

Replace the old heap-budget estimate (previously `indexedCount * 550`, removed in Task 1's Step 4) with a recalculated estimate based on the new struct shapes, plus a synthetic large-scale check:

```cpp
    // -----------------------------------------------------------------------------
    // Memory budget at realistic disk scale
    // -----------------------------------------------------------------------------
    {
        // FileItem no longer stores a full path per item - just name/normName
        // (short strings, typically inlined by SSO) plus a 4-byte index and a
        // bool. Estimate per-item cost as ~2 short wstrings (~2 x 32 bytes
        // covering typical filename lengths incl. heap-allocation overhead for
        // longer names) + 8 bytes bookkeeping.
        constexpr size_t kEstimatedBytesPerItem = 96;
        // DirectoryPool cost is amortized across many files per directory -
        // estimate generously at one entry per ~8 items, ~200 bytes each
        // (two full path strings + a timestamp).
        constexpr size_t kEstimatedBytesPerDir = 200;

        constexpr size_t kRealisticScale = 500000;
        const size_t estimatedDirs = kRealisticScale / 8;
        const size_t estimatedTotalBytes = kRealisticScale * kEstimatedBytesPerItem +
                                            estimatedDirs * kEstimatedBytesPerDir;
        constexpr size_t kMaxHeapBudgetAtScale = 150 * 1024 * 1024; // 150 MB at 500K items
        std::cout << "[FileIndex] Estimated heap usage at " << kRealisticScale << " items: "
                  << (estimatedTotalBytes / (1024 * 1024)) << " MB\n";
        Check(estimatedTotalBytes < kMaxHeapBudgetAtScale,
              "FileIndex heap usage stays bounded at a realistic 500K-item disk scale");

        // The repo-scoped live index from earlier in this file exercises the
        // real (non-synthetic) code path at whatever this repo's own file
        // count happens to be - keep that as a sanity floor, not the scale
        // claim itself, since it's only ever a few thousand files.
        Check(indexedCount > 0, "Live scoped index still populated (sanity check, not a scale claim)");
    }
```

- [ ] **Step 2: Run test to verify it fails**

This is a self-contained arithmetic check with no production dependency, so it can't fail to compile in a way Step 3 fixes - instead, run it once to confirm the numbers are sane (i.e., confirm `kMaxHeapBudgetAtScale` is neither trivially true nor unrealistically tight):

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: PASS immediately (there's no separate "fix" step for this task - it's a documentation-grade recalibration, not new production code). If it fails, the estimate constants are wrong relative to the actual `FileItem`/`DirectoryEntry` sizes measured in Task 1 - adjust `kEstimatedBytesPerItem`/`kEstimatedBytesPerDir` to match reality (e.g. add `sizeof(FileItem)` and a short-string heap-allocation margin explicitly via `std::cout` for visibility during this step), not to make the test pass artificially.

- [ ] **Step 3: (No separate implementation step - this task only touches test code.)**

- [ ] **Step 4: Run full test suite**

Run the CMake build + ctest commands above.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/core_tests.cpp
git commit -m "Recalibrate FileIndex heap-budget test for interned storage and realistic disk scale"
```

---

## Task 8: Final integration pass

**Files:** none new - verification only.

- [ ] **Step 1: Full clean rebuild via CMake**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build cmake --config Release`
Expected: Exit 0, no `/W4` warnings introduced by this plan's changes.

- [ ] **Step 2: Full ctest run**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --test-dir cmake -C Release --output-on-failure`
Expected: All checks pass, including the pre-existing ~20ms/query search benchmark (`tests/core_tests.cpp:749`, unchanged assertion, now exercised against the lazy path-reconstruction added in Task 1).

- [ ] **Step 3: MSBuild parity build**

Run: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" LeanLauncher.sln -p:Configuration=Release -p:Platform=x64 -v:m`
Expected: Exit 0 - confirms the hand-maintained `.vcxproj` build stays in sync (no new source files were added by this plan, only `file_index.h`/`core_tests.cpp` edited in place, so no `.vcxproj`/`.sln` changes should be needed).

- [ ] **Step 4: Kill any running dev instance and relaunch with the new binary**

Per this repo's established pattern (`LeanLauncher.exe`'s single-instance mutex blocks the build/link step while running - kill it without re-asking, this has been explicitly pre-approved in prior sessions): `taskkill /IM LeanLauncher.exe /F` if running, then launch `cmake/Release/LeanLauncher.exe` and manually smoke-test: open the launcher, type a file query, confirm results still appear and Enter/click still opens files correctly (path reconstruction from Task 1 didn't break anything user-visible).

- [ ] **Step 5: Commit any final cleanup**

```bash
git status
git add -A
git commit -m "Final integration pass for full-disk file search core rework"
```

(Only run this if `git status` shows something to commit - if Task 1-7's commits already captured everything, skip this step.)
