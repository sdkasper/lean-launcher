#pragma once

// Included by main.cpp inside its private namespace.
class LauncherWindow {
public:
    bool Create(HINSTANCE instance) {
        LoadSettings();
        EnsureStartMenuShortcut();
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
            return false;
        }
        if (!CreateFormat(19.0f, DWRITE_FONT_WEIGHT_NORMAL, searchFormat_) ||
            !CreateFormat(14.0f, DWRITE_FONT_WEIGHT_MEDIUM, resultFormat_) ||
            !CreateFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, hintFormat_) ||
            !CreateFormat(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, calcResultFormat_) ||
            !CreateFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL, previewMonoFormat_, L"Consolas")) return false;

        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
        windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        // Not layered: DWM supplies the backdrop; Direct2D supplies premultiplied content.
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kWindowClass, L"Lean Launcher", WS_POPUP | WS_THICKFRAME,
            CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyBackdrop();
        ResizeAndPosition();
        // Create the hardware render target while still hidden so the first
        // visible frame does not stall on device setup.
        EnsureTarget();
        RegisterShortcut();
        UpdateTrayIcon();
        takeoff::CleanupOldUpdates();
        try {
            iconThread_ = std::thread([this] { IconWorkerMain(); });
        } catch (const std::system_error&) {
            // Without the worker the launcher still runs with letter placeholders.
        }
        indexStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        indexTriggerEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if constexpr (!kUiTest) {
            SHChangeNotifyEntry notifyEntry{};
            notifyEntry.pidl = nullptr;
            notifyEntry.fRecursive = TRUE;
            shellNotifyId_ = SHChangeNotifyRegister(
                hwnd_,
                SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                SHCNE_ALLEVENTS,
                kShellNotifyMessage,
                1,
                &notifyEntry
            );
        }
        try {
            indexWorkerThread_ = std::thread([this] { IndexWorkerMain(); });
        } catch (const std::system_error&) {}
        if constexpr (!kUiTest) {
            // NFR-018: File search off means no index in memory at all.
            if (settings_.enableFileSearch) FileIndex::Instance().Start(hwnd_);
            if (settings_.obsidianEnabled && !obsidianVaultPath_.empty()) {
                leanlauncher::obsidian::NoteIndex::Instance().Start(obsidianVaultPath_, hwnd_);
            }
        }
        ApplySnippetsState();
        SetTimer(hwnd_, kHotkeyTimer, 2000, nullptr);
        RestorePomodoro();
        // Not forced: let the 24h throttle in CheckForUpdatesAsync decide
        // whether a launch actually warrants a network call.
        CheckForUpdatesAsync();
        return true;
    }

    HWND Handle() const { return hwnd_; }

private:
    static constexpr float kWidth = 750.0f;
    static constexpr float kSearchHeight = 64.0f;
    static constexpr float kSectionHeight = 32.0f;
    static constexpr float kRowHeight = 42.0f;
    static constexpr float kCalcRowHeight = 58.0f;
    static constexpr float kFooterHeight = 42.0f;
    static constexpr float kSettingsHeaderHeight = 46.0f;
    static constexpr float kSettingsRowHeight = 47.0f;
    static constexpr float kVaultDropdownItemHeight = 36.0f;
    static constexpr int kVisibleRows = 8;
    static constexpr float kTextLeft = 48.0f;
    static constexpr wchar_t kSettingsRegistryPath[] = L"Software\\LeanLauncher";
    static constexpr wchar_t kStartupRegistryPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr wchar_t kStartupValueName[] = L"LeanLauncher";

    static constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    static constexpr DWORD kDwmwaWindowCornerPreference = 33;
    static constexpr DWORD kDwmwaBorderColor = 34;
    static constexpr DWORD kDwmwaSystemBackdropType = 38;
    static constexpr DWORD kDwmwcpRound = 2;
    static constexpr DWORD kDwmColorNone = 0xFFFFFFFE;
    static constexpr DWORD kDwmBackdropTransient = 3;
    static constexpr DWORD kDwmBackdropNone = 1;

    enum class Page { Launcher, Settings };

    struct IconEntry {
        ComPtr<IWICBitmapSource> source;
        ComPtr<ID2D1Bitmap> bitmap;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        LauncherWindow* self = reinterpret_cast<LauncherWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<LauncherWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(message, wParam, lParam)
                    : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd_, WM_NCACTIVATE, wParam, -1);
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_SIZE:
            if (target_ && LOWORD(lParam) && HIWORD(lParam)) {
                if (FAILED(target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))))) {
                    DiscardTarget();
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_HOTKEY:
            if (wParam == kHotkeyId) {
                if (IsWindowVisible(hwnd_)) Hide(); else Show();
            }
            return 0;
        case kShowLauncherMessage:
            Show();
            return 0;
        case kExitLauncherMessage:
            DestroyWindow(hwnd_);
            return 0;
        case kTrayMessage:
            HandleTrayMessage(LOWORD(lParam));
            return 0;
        case kShellNotifyMessage: {
            HANDLE lock = SHChangeNotification_Lock(
                reinterpret_cast<HANDLE>(wParam),
                static_cast<DWORD>(lParam),
                nullptr, nullptr);
            if (lock) {
                SHChangeNotification_Unlock(lock);
            }
            TriggerAppReindex();
            return 0;
        }
        case kAppsReadyMessage: {
            std::unique_ptr<std::vector<AppEntry>> incoming(
                reinterpret_cast<std::vector<AppEntry>*>(lParam));
            std::vector<std::wstring> activeRecentPaths;
            for (size_t i : recent_) {
                if (i < apps_.size()) {
                    activeRecentPaths.push_back(apps_[i].path);
                }
            }
            apps_ = std::move(*incoming);
            indexReady_ = true;
            if (!activeRecentPaths.empty()) {
                recentPaths_ = std::move(activeRecentPaths);
            }
            recent_.clear();
            for (const auto& rPath : recentPaths_) {
                for (size_t i = 0; i < apps_.size(); ++i) {
                    if (apps_[i].path == rPath) {
                        if (std::find(recent_.begin(), recent_.end(), i) == recent_.end()) {
                            recent_.push_back(i);
                        }
                        break;
                    }
                }
            }
            baseAppsCount_ = apps_.size();
            RefreshPins();
            // Set here (UI thread, via the PostMessageW handoff) rather than
            // on the index worker thread that posted this message - writing
            // it there raced Show()'s read of the same field with no
            // synchronization.
            lastIndexTime_ = std::chrono::steady_clock::now();
            UpdateResults();
            return 0;
        }
        case kFilesReadyMessage: {
            if (page_ == Page::Launcher && !input_.text.empty() && settings_.enableFileSearch) {
                UpdateResults();
            }
            return 0;
        }
        case kNotesReadyMessage: {
            std::wstring noteQuery;
            if (page_ == Page::Launcher && !input_.text.empty() &&
                settings_.obsidianEnabled && settings_.vaultSearchEnabled &&
                leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.vaultSearchPrefix, noteQuery)) {
                UpdateResults();
            }
            return 0;
        }
        case kNoteOpenResultMessage: {
            // Success needs no UI: the launcher already hid itself and
            // Obsidian now has the foreground.
            if (wParam == 0) {
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
                SetFocus(hwnd_);
                status_ = L"Could not open this note in Obsidian.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case kKnownVaultsReadyMessage: {
            std::unique_ptr<std::vector<std::wstring>> result(
                reinterpret_cast<std::vector<std::wstring>*>(lParam));
            knownVaults_ = std::move(*result);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case kIconReadyMessage: {
            std::unique_ptr<IconResult> result(reinterpret_cast<IconResult*>(lParam));
            // Drop stale answers (e.g. a pre-DPI-change size) and duplicates.
            const UINT expected = static_cast<UINT>((std::max)(1, ToPixel(26)));
            if (result->size == expected && iconPending_.erase(result->path) != 0) {
                IconEntry entry;
                entry.source = std::move(result->source);
                iconCache_[result->path] = std::move(entry);
                if (IsWindowVisible(hwnd_) && page_ == Page::Launcher && !results_.empty()) {
                    const RECT rect{0, ToPixel(ResultsTop()), ToPixel(width_), ToPixel(FooterTop())};
                    InvalidateRect(hwnd_, &rect, FALSE);
                }
            }
            return 0;
        }
        case kUpdateCheckCompletedMessage: {
            // wParam: 0 up to date, 1 newer release but download failed,
            // 2 downloaded and validated, 3 check failed (no usable answer).
            std::unique_ptr<takeoff::UpdateCheckResult> result(reinterpret_cast<takeoff::UpdateCheckResult*>(lParam));
            const takeoff::UpdateCheckState next =
                takeoff::NextUpdateState(wParam, updateDownloaded_, updateAvailable_);
            // A failed check that keeps an earlier update carries no tag or
            // URL of its own - keep the earlier ones too.
            const bool keptEarlierUpdate = wParam == 3 && next != takeoff::UpdateCheckState::Failed;
            if (result && !keptEarlierUpdate) {
                updateTag_ = result->tag;
                updateReleaseUrl_ = result->htmlUrl;
            }
            if (wParam == 2 && result && !result->path.empty()) {
                downloadedUpdatePath_ = result->path;
                downloadedUpdateSha256_ = result->sha256;
            } else if (wParam == 0 || wParam == 1) {
                // Up to date, or the re-download failed after the old staged file was
                // deleted: never leave a path or hash pointing at a file that is gone.
                downloadedUpdatePath_.clear();
                downloadedUpdateSha256_.clear();
            }
            updateDownloaded_ = next == takeoff::UpdateCheckState::Ready;
            updateAvailable_ = updateDownloaded_ || next == takeoff::UpdateCheckState::Available;
            updateState_ = next;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case kPathListingMessage: {
            // US-043: a listing that arrives after the query moved on is dropped.
            std::unique_ptr<PathListing> listing(reinterpret_cast<PathListing*>(lParam));
            --pathThreadsRunning_;
            if (listing && listing->generation == pathGeneration_ && settings_.enablePathCompletion) {
                KillTimer(hwnd_, kPathTimeoutTimer);
                pathListedFolder_ = std::move(listing->folder);
                pathEntries_ = std::move(listing->entries);
                pathError_ = listing->error;
                pathTruncated_ = listing->truncated;
                pathListingReady_ = true;
                pathTimedOut_ = false;
                if (IsWindowVisible(hwnd_)) UpdateResults();
            }
            return 0;
        }
        case kPreviewReadyMessage: {
            // US-045: only the latest request's result is shown; anything
            // older (the selection moved on, the launcher hid, the panel
            // closed or the toggle went off) is freed right here.
            std::unique_ptr<PreviewContent> content(reinterpret_cast<PreviewContent*>(lParam));
            if (previewThreadsRunning_ > 0) --previewThreadsRunning_;
            if (content && previewGate_.IsCurrent(content->generation) && PreviewVisible()) {
                ShowPreviewContent(std::move(content));
            }
            return 0;
        }
        case kRecycleBinInfoMessage:
            // US-041: count and size for the Empty Recycle Bin row (x64: both fit).
            recycleBinQueryPending_ = false;
            recycleBinItems_ = static_cast<long long>(wParam);
            recycleBinBytes_ = static_cast<long long>(lParam);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case kUpdateProgressMessage: {
            std::unique_ptr<takeoff::UpdateCheckResult> result(reinterpret_cast<takeoff::UpdateCheckResult*>(lParam));
            if (result) updateTag_ = result->tag;
            updateState_ = takeoff::UpdateCheckState::Downloading;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_POWERBROADCAST:
            // US-049: re-check the timer right after waking; never wake the PC for it.
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) CheckPomodoro(true);
            return TRUE;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd_) && !modalDialogOpen_) Hide();
            return 0;
        case WM_SETFOCUS:
            // A hidden system caret exposes the insertion point to IME/accessibility.
            // The visible caret is rendered by Direct2D, including on an empty query.
            CreateCaret(hwnd_, nullptr, 1, ScaleForDpi(24, dpi_));
            ResetCaret();
            return 0;
        case WM_KILLFOCUS:
            KillTimer(hwnd_, kCaretTimer);
            DestroyCaret();
            return 0;
        case WM_TIMER:
            if (wParam == kRenderRetryTimer) {
                KillTimer(hwnd_, kRenderRetryTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wParam == kPomodoroTickTimer || wParam == kPomodoroEndTimer) {
                if (wParam == kPomodoroEndTimer) KillTimer(hwnd_, kPomodoroEndTimer);
                CheckPomodoro(false);
            } else if (wParam == kPathTimeoutTimer) {
                KillTimer(hwnd_, kPathTimeoutTimer);
                if (!pathListingReady_) {
                    pathTimedOut_ = true;
                    UpdateResults();
                }
            } else if (wParam == kPreviewDebounceTimer) {
                KillTimer(hwnd_, kPreviewDebounceTimer);
                RequestPreview();
            } else if (wParam == kCommandConfirmTimer) {
                KillTimer(hwnd_, kCommandConfirmTimer);
                commandGate_.Cancel();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wParam == kTrimTimer) {
                KillTimer(hwnd_, kTrimTimer);
                if (!IsWindowVisible(hwnd_)) {
                    // Give idle pages back while the launcher sits hidden.
                    SetProcessWorkingSetSize(GetCurrentProcess(),
                        static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                }
            } else if (wParam == kHotkeyTimer && !hotkeyRegistered_) {
                RegisterShortcut();
            } else if (wParam == kCaretTimer && GetFocus() == hwnd_ &&
                    !actionsOpen_ && page_ == Page::Launcher) {
                caretVisible_ = !caretVisible_;
                InvalidateSearch();
            }
            return 0;
        case WM_CHAR:
            if (ShouldShowHotkeyWarning()) return 0;
            if (page_ == Page::Settings && editingRow_ >= 0 && wParam >= L' ' && wParam != 0x7F) {
                const wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch >= 0xD800 && ch <= 0xDBFF) {
                    pendingSurrogate_ = ch;
                    return 0;
                }
                if (ch >= 0xDC00 && ch <= 0xDFFF) {
                    if (!pendingSurrogate_) return 0;
                    const wchar_t pair[]{pendingSurrogate_, ch};
                    settingsEdit_.Insert(std::wstring_view(pair, 2));
                } else {
                    settingsEdit_.Insert(std::wstring_view(&ch, 1));
                }
                pendingSurrogate_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (page_ == Page::Launcher && !actionsOpen_ && wParam >= L' ' && wParam != 0x7F &&
                (!(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))) {
                const wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch >= 0xD800 && ch <= 0xDBFF) {
                    pendingSurrogate_ = ch;
                    return 0;
                }
                if (ch >= 0xDC00 && ch <= 0xDFFF) {
                    if (!pendingSurrogate_) return 0;
                    const wchar_t pair[]{pendingSurrogate_, ch};
                    input_.Insert(std::wstring_view(pair, 2));
                } else {
                    input_.Insert(std::wstring_view(&ch, 1));
                }
                pendingSurrogate_ = 0;
                OnQueryChanged();
            }
            return 0;
        case WM_KEYDOWN:
            return HandleKeyDown(wParam, lParam);
        case WM_SYSKEYDOWN:
            if (page_ == Page::Settings && recordingRow_ >= 0) {
                return HandleKeyDown(wParam, lParam);
            }
            if ((wParam >= '1' && wParam <= '8') ||
                    (page_ == Page::Settings && wParam == VK_LEFT)) {
                return HandleKeyDown(wParam, lParam);
            }
            break;
        case WM_SYSCHAR:
            if (page_ == Page::Settings && recordingRow_ >= 0) return 0;
            if (wParam >= '1' && wParam <= '8') return 0;
            break;
        case WM_IME_SETCONTEXT:
            return DefWindowProcW(hwnd_, message, wParam, lParam & ~ISC_SHOWUICOMPOSITIONWINDOW);
        case WM_IME_STARTCOMPOSITION:
            composing_ = true;
            PositionIme();
            return 0;
        case WM_IME_COMPOSITION:
            HandleComposition(lParam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            composing_ = false;
            composition_.clear();
            InvalidateSearch();
            return 0;
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(input_.text.size());
        case WM_GETTEXT:
            if (wParam && lParam) {
                const size_t count = (std::min)(input_.text.size(), static_cast<size_t>(wParam - 1));
                std::copy_n(input_.text.c_str(), count, reinterpret_cast<wchar_t*>(lParam));
                reinterpret_cast<wchar_t*>(lParam)[count] = 0;
                return static_cast<LRESULT>(count);
            }
            return 0;
        case WM_LBUTTONDOWN:
            HandleClick(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_MBUTTONDOWN:
            HandleMiddleClick(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_RBUTTONDOWN:
            HandleRightClick(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_RBUTTONUP:
            return 0;
        case WM_LBUTTONDBLCLK:
            if (page_ == Page::Launcher && ToDip(GET_Y_LPARAM(lParam)) < kSearchHeight) {
                input_.SelectAll();
                ResetCaret();
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_) { dragging_ = false; ReleaseCapture(); }
            if (settingsDraggingScroll_) { settingsDraggingScroll_ = false; ReleaseCapture(); }
            return 0;
        case WM_CAPTURECHANGED:
            dragging_ = false;
            settingsDraggingScroll_ = false;
            return 0;
        case WM_MOUSEMOVE:
            HandleMouseMove(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            mouseKnown_ = false;
            hoverLockRow_ = -1;
            adminActionHovered_ = false;
            return 0;
        case WM_MOUSEWHEEL: {
            POINT wheelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // screen coordinates
            ScreenToClient(hwnd_, &wheelPoint);
            if (page_ == Page::Launcher && !actionsOpen_ && PointInPreview(ToDip(wheelPoint.x), ToDip(wheelPoint.y))) {
                // US-045: the wheel over the panel scrolls it, never the results.
                wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
                const int steps = wheelDelta_ / WHEEL_DELTA;
                wheelDelta_ %= WHEEL_DELTA;
                ScrollPreview(-static_cast<float>(steps) * 48.0f);
            } else if (page_ == Page::Launcher && !actionsOpen_ && !results_.empty()) {
                wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
                const int steps = wheelDelta_ / WHEEL_DELTA;
                wheelDelta_ %= WHEEL_DELTA;
                MoveSelection(-steps * 3, false);
            } else if (page_ == Page::Settings && vaultDropdownOpen_) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                ScrollVaultDropdown(-delta / WHEEL_DELTA);
            } else if (page_ == Page::Settings) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                ScrollSettings(-static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * 36.0f);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd_, &point);
                const float x = ToDip(point.x), y = ToDip(point.y);
                if (ShouldShowHotkeyWarning()) {
                    const bool hand = PointInHotkeyWarningSettings(x, y) || PointInHotkeyWarningDismiss(x, y);
                    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                const bool text = page_ == Page::Launcher && !actionsOpen_ &&
                    y < kSearchHeight && x >= kTextLeft && x < width_ - 86;
                const bool button = page_ == Page::Settings || (!PointInPreview(x, y) &&
                    (ResultAtPoint(x, y) >= 0 || y >= FooterTop() || x > width_ - 84));
                SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : button ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            if (target_) target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
            iconCache_.clear();
            iconPending_.clear();
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd_, HWND_TOPMOST, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOACTIVATE);
            ResizeAndPosition();
            PrepareVisibleIcons();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DWMCOMPOSITIONCHANGED:
            ApplyBackdrop();
            ResetCaret();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DISPLAYCHANGE:
            ResizeAndPosition();
            return 0;
        case WM_DESTROY: {
            expander_.Stop();
            if (shellNotifyId_) {
                SHChangeNotifyDeregister(shellNotifyId_);
                shellNotifyId_ = 0;
            }
            if (indexStopEvent_) {
                SetEvent(indexStopEvent_);
            }
            if (indexWorkerThread_.joinable()) {
                indexWorkerThread_.join();
            }
            if (indexStopEvent_) {
                CloseHandle(indexStopEvent_);
                indexStopEvent_ = nullptr;
            }
            if (indexTriggerEvent_) {
                CloseHandle(indexTriggerEvent_);
                indexTriggerEvent_ = nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(iconMutex_);
                iconStop_ = true;
            }
            iconCv_.notify_one();
            if (iconThread_.joinable()) iconThread_.join();
            if (updateThread_.joinable()) {
                // The update thread can be mid network-request/download (up
                // to ~45s). An unconditional join() here would block the
                // whole shutdown for that long and trigger a Windows "not
                // responding" prompt. Wait briefly, then detach rather than
                // block - PostQuitMessage follows shortly below, so the
                // imminent process exit reaps the thread instead.
                if (WaitForSingleObject(updateThread_.native_handle(), 250) == WAIT_OBJECT_0) {
                    updateThread_.join();
                } else {
                    updateThread_.detach();
                }
            }
            if constexpr (!kUiTest) {
                FileIndex::Instance().Stop();
                leanlauncher::obsidian::NoteIndex::Instance().Stop();
            }
            KillTimer(hwnd_, kCaretTimer);
            KillTimer(hwnd_, kHotkeyTimer);
            KillTimer(hwnd_, kRenderRetryTimer);
            KillTimer(hwnd_, kTrimTimer);
            ClosePreview(true);  // a load still in flight posts to a dead window and frees its own result
            if (hotkeyRegistered_) UnregisterHotKey(hwnd_, kHotkeyId);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        case WM_CLOSE:
            if constexpr (kUiTest) DestroyWindow(hwnd_);
            else Hide();
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    float ToDip(int value) const { return static_cast<float>(value) * 96.0f / dpi_; }
    int ToPixel(float value) const { return static_cast<int>(std::lround(value * dpi_ / 96.0f)); }
    float FooterTop() const { return height_ - kFooterHeight; }
    float ResultsTop() const { return kSearchHeight + kSectionHeight; }
    float RowHeight(int resultIndex) const {
        if (resultIndex >= 0 && resultIndex < static_cast<int>(results_.size())) {
            const size_t appIdx = results_[resultIndex];
            if (appIdx < apps_.size() && apps_[appIdx].category == takeoff::AppCategory::Calculator) {
                return kCalcRowHeight;
            }
        }
        return kRowHeight;
    }

    void RegisterShortcut() {
        if (hotkeyRegistered_) {
            UnregisterHotKey(hwnd_, kHotkeyId);
            hotkeyRegistered_ = false;
        }
        if constexpr (kUiTest) {
            hotkeyRegistered_ = (hwnd_ != nullptr);
        } else {
            if (!settings_.launcherHotkey.disabled && settings_.launcherHotkey.key) {
                hotkeyRegistered_ = RegisterHotKey(hwnd_, kHotkeyId,
                    static_cast<UINT>(settings_.launcherHotkey.modifiers) | MOD_NOREPEAT,
                    settings_.launcherHotkey.key) != FALSE;
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
        DWORD value = fallback;
        DWORD size = sizeof(value);
        if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
            return fallback;
        }
        return value;
    }

    void LoadSettings() {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, KEY_READ, &key) ==
                    ERROR_SUCCESS) {
                const DWORD version = ReadDword(key, L"SettingsVersion", 0);
                if (version >= 2) {
                    settings_.launcherHotkey.modifiers =
                        static_cast<uint16_t>(ReadDword(key, L"LauncherMod", quicklaunch::kModAlt));
                    settings_.launcherHotkey.key =
                        static_cast<uint16_t>(ReadDword(key, L"LauncherKey", quicklaunch::kVkSpace));
                    settings_.launcherHotkey.disabled = ReadDword(key, L"LauncherOff", 0) != 0;
                    settings_.actionsHotkey.modifiers =
                        static_cast<uint16_t>(ReadDword(key, L"ActionsMod", quicklaunch::kModControl));
                    settings_.actionsHotkey.key =
                        static_cast<uint16_t>(ReadDword(key, L"ActionsKey", 'K'));
                    settings_.actionsHotkey.disabled = ReadDword(key, L"ActionsOff", 0) != 0;
                    settings_.administratorHotkey.modifiers =
                        static_cast<uint16_t>(ReadDword(key, L"AdminMod", quicklaunch::kModControl));
                    settings_.administratorHotkey.disabled = ReadDword(key, L"AdminOff", 0) != 0;
                    settings_.quickLaunchHotkey.modifiers =
                        static_cast<uint16_t>(ReadDword(key, L"QuickMod", quicklaunch::kModAlt));
                    settings_.quickLaunchHotkey.disabled = ReadDword(key, L"QuickOff", 0) != 0;
                } else {
                    settings_.launcherHotkey = quicklaunch::MigrateLauncherHotkey(
                        static_cast<int>(ReadDword(key, L"LauncherHotkey", 0)));
                    settings_.actionsHotkey = quicklaunch::MigrateActionsHotkey(
                        static_cast<int>(ReadDword(key, L"ActionsHotkey", 0)));
                    settings_.administratorHotkey = quicklaunch::MigrateAdminHotkey(
                        static_cast<int>(ReadDword(key, L"AdministratorHotkey", 0)));
                    settings_.quickLaunchHotkey = quicklaunch::MigrateQuickLaunchHotkey(
                        static_cast<int>(ReadDword(key, L"QuickLaunchHotkey", 0)));
                }
                settings_.showTrayIcon = ReadDword(key, L"ShowTrayIcon", 1) != 0;
                // Fallback must match settings_.checkForUpdates's coded default (false,
                // per NFR-003: no release pipeline yet, opt-in only). A fallback of 1
                // here would silently re-enable background update checks the moment
                // the HKCU\Software\LeanLauncher key exists for any other reason, even
                // if CheckForUpdates itself was never written.
                settings_.checkForUpdates = ReadDword(key, L"CheckForUpdates", 0) != 0;
                settings_.enableFileSearch = ReadDword(key, L"FileSearchEnabled", 1) != 0;
                settings_.enableWebSearch = ReadDword(key, L"WebSearchEnabled", 1) != 0;
                settings_.enableSystemCommands = ReadDword(key, L"SystemCommandsEnabled", 1) != 0;
                settings_.enableTypedUrls = ReadDword(key, L"TypedUrlsEnabled", 1) != 0;
                settings_.enablePathCompletion = ReadDword(key, L"PathCompletionEnabled", 1) != 0;
                settings_.enableUnitConverter = ReadDword(key, L"UnitConverterEnabled", 1) != 0;
                settings_.enableTimeZones = ReadDword(key, L"TimeZonesEnabled", 1) != 0;
                settings_.enablePomodoro = ReadDword(key, L"PomodoroEnabled", 1) != 0;
                settings_.enableSnippets = ReadDword(key, L"SnippetsEnabled", 0) != 0;
                settings_.pomodoroLog = ReadDword(key, L"PomodoroLog", 1) != 0;
                // US-045: preview panel. No old-format migration needed - this
                // setting didn't exist before SettingsVersion 2, so it's read
                // unconditionally here rather than inside the version-gated
                // hotkey migration block above.
                settings_.enablePreview = ReadDword(key, L"PreviewEnabled", 1) != 0;
                settings_.previewOpen = ReadDword(key, L"PreviewOpen", 0) != 0;
                settings_.previewHotkey.modifiers =
                    static_cast<uint16_t>(ReadDword(key, L"PreviewMod", quicklaunch::kModControl));
                settings_.previewHotkey.key = static_cast<uint16_t>(ReadDword(key, L"PreviewKey", 'P'));
                settings_.previewHotkey.disabled = ReadDword(key, L"PreviewOff", 0) != 0;
                settings_.runAtStartup = ReadDword(key, L"RunAtStartup", 0) != 0;
                settings_.vaultSearchEnabled = ReadDword(key, L"VaultSearchEnabled", 1) != 0;
                settings_.taskAddEnabled = ReadDword(key, L"TaskAddEnabled", 1) != 0;
                settings_.noteAddEnabled = ReadDword(key, L"NoteAddEnabled", 1) != 0;
                settings_.logEnabled = ReadDword(key, L"LogEnabled", 1) != 0;
                settings_.quickOpenTarget =
                    std::clamp(static_cast<int>(ReadDword(key, L"QuickOpenTarget", 0)), 0, kQuickOpenTargetCount - 1);
                const DWORD low = ReadDword(key, L"LastUpdateCheckLow", 0);
                const DWORD high = ReadDword(key, L"LastUpdateCheckHigh", 0);
                lastUpdateCheck_ = (static_cast<uint64_t>(high) << 32) | low;

                // Update endpoints are compile-time constants only (kDefaultReleasesUrl /
                // kDefaultApiHost / kDefaultApiPath) - no registry override. This used to
                // read UpdateReleasesUrl/UpdateApiHost/UpdateApiPath from HKCU with no
                // validation and no Settings UI to set them, letting any unprivileged local
                // process redirect the updater to an attacker-controlled host.
                wchar_t buf[512]{};
                DWORD bufSize = sizeof(buf);
                if (RegGetValueW(key, nullptr, L"VaultPath", RRF_RT_REG_SZ, nullptr, buf, &bufSize) == ERROR_SUCCESS && buf[0]) {
                    obsidianVaultPath_ = buf;
                }
                settings_.obsidianEnabled =
                    ReadDword(key, L"ObsidianEnabled",
                        leanlauncher::obsidian::DefaultObsidianEnabled(obsidianVaultPath_) ? 1 : 0) != 0;

                auto readStringSetting = [&](const wchar_t* valueName, std::wstring& target) {
                    wchar_t strBuf[512]{};
                    DWORD strBufSize = sizeof(strBuf);
                    // No "&& strBuf[0]" guard here, unlike VaultPath above:
                    // an empty string is itself a meaningful saved value for
                    // these fields (an empty override means auto-detect; an
                    // empty label is a valid cosmetic choice), so presence
                    // is determined solely by the registry read succeeding.
                    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, nullptr, strBuf, &strBufSize) ==
                        ERROR_SUCCESS) {
                        target = strBuf;
                    }
                };
                readStringSetting(L"WebSearchUrlTemplate", settings_.webSearchUrlTemplate);
                readStringSetting(L"WebSearchEngineName", settings_.webSearchEngineName);
                readStringSetting(L"WebSearchPrefix", settings_.webSearchPrefix);
                readStringSetting(L"WebSearchPillLabel", settings_.webSearchPillLabel);
                readStringSetting(L"FileSearchPrefix", settings_.fileSearchPrefix);
                readStringSetting(L"AppSearchPrefix", settings_.appSearchPrefix);
                readStringSetting(L"SystemCommandsPrefix", settings_.systemCommandsPrefix);
                readStringSetting(L"PomodoroPrefix", settings_.pomodoroPrefix);
                readStringSetting(L"PomodoroFocusMinutes", settings_.pomodoroFocusMinutes);
                readStringSetting(L"PomodoroBreakMinutes", settings_.pomodoroBreakMinutes);
                readStringSetting(L"SnippetsPrefix", settings_.snippetsPrefix);
                readStringSetting(L"SnippetsPath", settings_.snippetsPath);
                readStringSetting(L"VaultSearchPrefix", settings_.vaultSearchPrefix);
                readStringSetting(L"VaultSearchPillLabel", settings_.vaultSearchPillLabel);
                readStringSetting(L"TaskPrefix", settings_.taskPrefix);
                readStringSetting(L"TaskPillLabel", settings_.taskPillLabel);
                readStringSetting(L"TaskPreviewPrefix", settings_.taskPreviewPrefix);
                readStringSetting(L"NoteAddPrefix", settings_.noteAddPrefix);
                readStringSetting(L"NoteAddPillLabel", settings_.noteAddPillLabel);
                readStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
                // Pre-1.6.0 default, saved verbatim by every SaveSettings() -
                // no longer accurate once a capture can target a non-daily
                // note (US-025). Only the untouched default migrates; a
                // user-customized preview is left alone.
                if (settings_.noteAddPreviewPrefix == L"Add to today's note: ") {
                    settings_.noteAddPreviewPrefix = quicklaunch::Settings{}.noteAddPreviewPrefix;
                }
                readStringSetting(L"LogPrefix", settings_.logPrefix);
                readStringSetting(L"LogPillLabel", settings_.logPillLabel);
                readStringSetting(L"LogPreviewPrefix", settings_.logPreviewPrefix);
                readStringSetting(L"LogHeading", settings_.logHeading);
                readStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
                readStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
                readStringSetting(L"TaskTargetNote", settings_.taskTargetNote);
                readStringSetting(L"NoteAddTargetNote", settings_.noteAddTargetNote);
                readStringSetting(L"LogTargetNote", settings_.logTargetNote);
                // Settings UI only ever saves normalized refs, but the
                // registry is hand-editable - anything that doesn't
                // normalize to a note inside the vault is dropped (treated
                // as unset) rather than trusted as a write target.
                for (std::wstring* target : {&settings_.taskTargetNote, &settings_.noteAddTargetNote,
                         &settings_.logTargetNote}) {
                    std::wstring normalized;
                    *target = leanlauncher::obsidian::NormalizeTargetNoteRef(*target, normalized)
                        ? normalized : std::wstring();
                }

                RegCloseKey(key);
            }
            if (!obsidianVaultPath_.empty()) {
                dailyNoteConfig_ = leanlauncher::obsidian::ResolveDailyNoteConfig(
                    obsidianVaultPath_, settings_.dailyNoteFolderOverride, settings_.dailyNoteFormatOverride);
            }
            // Read-only (v1.6.1): the toggle shows whether the Run key holds
            // this exe's own startup command. Launch never writes the key -
            // silently (re)registering itself on every start is what got
            // unsigned builds quarantined as Behavior:Win32/Persistence.A!ml.
            // A missing or foreign entry just shows as off; the user turns
            // it on in Settings, which is the only place SetRunAtStartup runs.
            bool startupRegistered = false;
            HKEY startup = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRegistryPath, 0, KEY_QUERY_VALUE, &startup) == ERROR_SUCCESS) {
                wchar_t existingCmd[MAX_PATH * 2]{};
                DWORD size = sizeof(existingCmd);
                wchar_t currentExe[MAX_PATH]{};
                startupRegistered =
                    RegGetValueW(startup, nullptr, kStartupValueName, RRF_RT_REG_SZ, nullptr, existingCmd, &size) ==
                        ERROR_SUCCESS &&
                    GetModuleFileNameW(nullptr, currentExe, MAX_PATH) &&
                    quicklaunch::IsStartupCommandFor(existingCmd, currentExe);
                RegCloseKey(startup);
            }
            settings_.runAtStartup = startupRegistered;
            LoadRecent();
            LoadPins();
        }
    }

    // Pinned results (US-024), persisted like Recent: HKCU\...\Pinned\Pin0..Pin4,
    // newest first, each value "app|<path>" or "file|<path>" (see EncodePin).
    void SavePins() {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, (std::wstring(kSettingsRegistryPath) + L"\\Pinned").c_str(),
                    0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
                for (size_t i = 0; i < leanlauncher::pins::kMaxPins; ++i) {
                    RegDeleteValueW(key, (L"Pin" + std::to_wstring(i)).c_str());
                }
                for (size_t i = 0; i < pins_.size(); ++i) {
                    const std::wstring value = leanlauncher::pins::EncodePin(pins_[i]);
                    RegSetValueExW(key, (L"Pin" + std::to_wstring(i)).c_str(), 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
                }
                RegCloseKey(key);
            }
        }
    }

    void LoadPins() {
        pins_.clear();
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kSettingsRegistryPath) + L"\\Pinned").c_str(),
                    0, KEY_READ, &key) == ERROR_SUCCESS) {
                for (size_t i = 0; i < leanlauncher::pins::kMaxPins; ++i) {
                    wchar_t buffer[MAX_PATH * 2]{};
                    DWORD size = sizeof(buffer);
                    leanlauncher::pins::Pin pin;
                    if (RegGetValueW(key, nullptr, (L"Pin" + std::to_wstring(i)).c_str(), RRF_RT_REG_SZ, nullptr,
                            buffer, &size) == ERROR_SUCCESS &&
                        leanlauncher::pins::DecodePin(buffer, pin) &&
                        leanlauncher::pins::FindPin(pins_, pin.path) < 0) {
                        pins_.push_back(std::move(pin));
                    }
                }
                RegCloseKey(key);
            }
        }
        RefreshPins();
    }

    // Rebuilds the per-pin lookup data PinRank() and result assembly use -
    // on load, after every app rescan, and on pin/unpin; never per keystroke
    // (NFR-015). A pinned app missing from the current scan (uninstalled)
    // resolves to npos and is silently left out.
    void RefreshPins() {
        pinnedAppIndex_.assign(pins_.size(), static_cast<size_t>(-1));
        pinnedNames_.clear();
        for (size_t p = 0; p < pins_.size(); ++p) {
            if (pins_[p].isApp) {
                for (size_t i = 0; i < baseAppsCount_ && i < apps_.size(); ++i) {
                    if (leanlauncher::pins::SamePath(apps_[i].path, pins_[p].path)) {
                        pinnedAppIndex_[p] = i;
                        break;
                    }
                }
            }
            pinnedNames_.push_back(Normalize(fs::path(pins_[p].path).filename().wstring()));
        }
    }

    // Pin order of apps_[index] (0 = newest pin), or -1 if it isn't pinned.
    // At most kMaxPins comparisons - cheap enough for every result row.
    int PinRank(size_t index) const {
        if (index >= apps_.size()) return -1;
        if (index < baseAppsCount_) {
            for (size_t p = 0; p < pinnedAppIndex_.size(); ++p) {
                if (pinnedAppIndex_[p] == index) return static_cast<int>(p);
            }
            return -1;
        }
        const AppEntry& entry = apps_[index];
        if (entry.category != takeoff::AppCategory::File && entry.category != takeoff::AppCategory::Folder) return -1;
        for (size_t p = 0; p < pins_.size(); ++p) {
            if (!pins_[p].isApp && leanlauncher::pins::SamePath(entry.path, pins_[p].path)) return static_cast<int>(p);
        }
        return -1;
    }

    static bool IsPinnable(const AppEntry& app) {
        return !app.inert && (app.category == takeoff::AppCategory::Application ||
            app.category == takeoff::AppCategory::System || app.category == takeoff::AppCategory::File ||
            app.category == takeoff::AppCategory::Folder);
    }

    // Result row for a pinned file/folder built straight from its saved
    // path, or false if it no longer exists (stale pins are dropped
    // silently, never shown as a broken row).
    bool BuildPinnedFileEntry(size_t pinIndex, AppEntry& entry) const {
        std::error_code ec;
        const fs::path path(pins_[pinIndex].path);
        if (!fs::exists(path, ec)) return false;
        entry.name = path.filename().wstring();
        entry.path = pins_[pinIndex].path;
        entry.normalizedName = pinnedNames_[pinIndex];
        entry.category = fs::is_directory(path, ec) ? takeoff::AppCategory::Folder : takeoff::AppCategory::File;
        return true;
    }

    void TogglePinSelected() {
        if (!HasResult()) return;
        const AppEntry& app = apps_[results_[selected_]];
        if (!IsPinnable(app)) return;
        const std::wstring path = app.path;
        const bool isApp = (app.category == takeoff::AppCategory::Application ||
                            app.category == takeoff::AppCategory::System);
        const auto result = leanlauncher::pins::TogglePin(pins_, path, isApp);
        if (result == leanlauncher::pins::PinResult::AtCap) {
            status_ = L"Unpin one first - up to 5 pins";
        } else {
            SavePins();
            RefreshPins();
            status_ = (result == leanlauncher::pins::PinResult::Pinned) ? L"Pinned" : L"Unpinned";
            UpdateResults();
            // Keep the toggled item selected wherever it moved to.
            for (size_t i = 0; i < results_.size(); ++i) {
                if (leanlauncher::pins::SamePath(apps_[results_[i]].path, path)) {
                    selected_ = static_cast<int>(i);
                    EnsureVisible();
                    break;
                }
            }
            SchedulePreview();
        }
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SaveRecent() {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, (std::wstring(kSettingsRegistryPath) + L"\\Recent").c_str(),
                    0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
                for (int i = 0; i < 8; ++i) {
                    const std::wstring valName = L"App" + std::to_wstring(i);
                    RegDeleteValueW(key, valName.c_str());
                }
                for (size_t i = 0; i < recent_.size() && i < 8; ++i) {
                    if (recent_[i] < apps_.size()) {
                        const std::wstring valName = L"App" + std::to_wstring(i);
                        const std::wstring& path = apps_[recent_[i]].path;
                        RegSetValueExW(key, valName.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(path.c_str()),
                            static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
                    }
                }
                RegCloseKey(key);
            }
        }
    }

    void LoadRecent() {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(kSettingsRegistryPath) + L"\\Recent").c_str(),
                    0, KEY_READ, &key) == ERROR_SUCCESS) {
                recentPaths_.clear();
                for (int i = 0; i < 8; ++i) {
                    const std::wstring valName = L"App" + std::to_wstring(i);
                    wchar_t buffer[MAX_PATH * 2]{};
                    DWORD size = sizeof(buffer);
                    if (RegGetValueW(key, nullptr, valName.c_str(), RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
                        recentPaths_.push_back(buffer);
                    }
                }
                RegCloseKey(key);
            }
        }
    }

    void SaveSettings() {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
                settingsStatus_ = L"Could not save this setting.";
                return;
            }
            struct Entry { const wchar_t* name; DWORD value; };
            const Entry entries[] = {
                {L"SettingsVersion", 2},
                {L"LauncherMod", settings_.launcherHotkey.modifiers},
                {L"LauncherKey", settings_.launcherHotkey.key},
                {L"LauncherOff", settings_.launcherHotkey.disabled ? 1u : 0u},
                {L"ActionsMod", settings_.actionsHotkey.modifiers},
                {L"ActionsKey", settings_.actionsHotkey.key},
                {L"ActionsOff", settings_.actionsHotkey.disabled ? 1u : 0u},
                {L"AdminMod", settings_.administratorHotkey.modifiers},
                {L"AdminOff", settings_.administratorHotkey.disabled ? 1u : 0u},
                {L"QuickMod", settings_.quickLaunchHotkey.modifiers},
                {L"QuickOff", settings_.quickLaunchHotkey.disabled ? 1u : 0u},
                {L"ShowTrayIcon", settings_.showTrayIcon ? 1u : 0u},
                {L"CheckForUpdates", settings_.checkForUpdates ? 1u : 0u},
                {L"FileSearchEnabled", settings_.enableFileSearch ? 1u : 0u},
                {L"WebSearchEnabled", settings_.enableWebSearch ? 1u : 0u},
                {L"SystemCommandsEnabled", settings_.enableSystemCommands ? 1u : 0u},
                {L"TypedUrlsEnabled", settings_.enableTypedUrls ? 1u : 0u},
                {L"PathCompletionEnabled", settings_.enablePathCompletion ? 1u : 0u},
                {L"UnitConverterEnabled", settings_.enableUnitConverter ? 1u : 0u},
                {L"TimeZonesEnabled", settings_.enableTimeZones ? 1u : 0u},
                {L"PomodoroEnabled", settings_.enablePomodoro ? 1u : 0u},
                {L"SnippetsEnabled", settings_.enableSnippets ? 1u : 0u},
                {L"PomodoroLog", settings_.pomodoroLog ? 1u : 0u},
                {L"PreviewEnabled", settings_.enablePreview ? 1u : 0u},
                {L"PreviewOpen", settings_.previewOpen ? 1u : 0u},
                {L"PreviewMod", settings_.previewHotkey.modifiers},
                {L"PreviewKey", settings_.previewHotkey.key},
                {L"PreviewOff", settings_.previewHotkey.disabled ? 1u : 0u},
                {L"RunAtStartup", settings_.runAtStartup ? 1u : 0u},
                {L"ObsidianEnabled", settings_.obsidianEnabled ? 1u : 0u},
                {L"VaultSearchEnabled", settings_.vaultSearchEnabled ? 1u : 0u},
                {L"TaskAddEnabled", settings_.taskAddEnabled ? 1u : 0u},
                {L"NoteAddEnabled", settings_.noteAddEnabled ? 1u : 0u},
                {L"LogEnabled", settings_.logEnabled ? 1u : 0u},
                {L"QuickOpenTarget", static_cast<DWORD>(settings_.quickOpenTarget)},
            };
            bool saved = true;
            for (const auto& entry : entries) {
                saved = RegSetValueExW(key, entry.name, 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&entry.value), sizeof(entry.value)) ==
                    ERROR_SUCCESS && saved;
            }
            auto writeStringSetting = [&](const wchar_t* valueName, const std::wstring& value) {
                saved = RegSetValueExW(key, valueName, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(value.c_str()),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS && saved;
            };
            writeStringSetting(L"WebSearchUrlTemplate", settings_.webSearchUrlTemplate);
            writeStringSetting(L"WebSearchEngineName", settings_.webSearchEngineName);
            writeStringSetting(L"WebSearchPrefix", settings_.webSearchPrefix);
            writeStringSetting(L"WebSearchPillLabel", settings_.webSearchPillLabel);
            writeStringSetting(L"FileSearchPrefix", settings_.fileSearchPrefix);
            writeStringSetting(L"AppSearchPrefix", settings_.appSearchPrefix);
            writeStringSetting(L"SystemCommandsPrefix", settings_.systemCommandsPrefix);
            writeStringSetting(L"PomodoroPrefix", settings_.pomodoroPrefix);
            writeStringSetting(L"PomodoroFocusMinutes", settings_.pomodoroFocusMinutes);
            writeStringSetting(L"PomodoroBreakMinutes", settings_.pomodoroBreakMinutes);
            writeStringSetting(L"SnippetsPrefix", settings_.snippetsPrefix);
            writeStringSetting(L"SnippetsPath", settings_.snippetsPath);
            writeStringSetting(L"VaultPath", obsidianVaultPath_);
            writeStringSetting(L"VaultSearchPrefix", settings_.vaultSearchPrefix);
            writeStringSetting(L"VaultSearchPillLabel", settings_.vaultSearchPillLabel);
            writeStringSetting(L"TaskPrefix", settings_.taskPrefix);
            writeStringSetting(L"TaskPillLabel", settings_.taskPillLabel);
            writeStringSetting(L"TaskPreviewPrefix", settings_.taskPreviewPrefix);
            writeStringSetting(L"NoteAddPrefix", settings_.noteAddPrefix);
            writeStringSetting(L"NoteAddPillLabel", settings_.noteAddPillLabel);
            writeStringSetting(L"NoteAddPreviewPrefix", settings_.noteAddPreviewPrefix);
            writeStringSetting(L"LogPrefix", settings_.logPrefix);
            writeStringSetting(L"LogPillLabel", settings_.logPillLabel);
            writeStringSetting(L"LogPreviewPrefix", settings_.logPreviewPrefix);
            writeStringSetting(L"LogHeading", settings_.logHeading);
            writeStringSetting(L"DailyNoteFolderOverride", settings_.dailyNoteFolderOverride);
            writeStringSetting(L"DailyNoteFormatOverride", settings_.dailyNoteFormatOverride);
            writeStringSetting(L"TaskTargetNote", settings_.taskTargetNote);
            writeStringSetting(L"NoteAddTargetNote", settings_.noteAddTargetNote);
            writeStringSetting(L"LogTargetNote", settings_.logTargetNote);
            RegCloseKey(key);
            if (!saved) settingsStatus_ = L"Could not save this setting.";
        }
    }

    void SaveLastUpdateCheck(uint64_t timestamp) {
        if constexpr (!kUiTest) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
                const DWORD low = static_cast<DWORD>(timestamp & 0xFFFFFFFF);
                const DWORD high = static_cast<DWORD>(timestamp >> 32);
                RegSetValueExW(key, L"LastUpdateCheckLow", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&low), sizeof(low));
                RegSetValueExW(key, L"LastUpdateCheckHigh", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&high), sizeof(high));
                RegCloseKey(key);
            }
        }
    }

    // `manual` is the About tab's "Check for updates" row (US-029): it runs
    // even with automatic checks turned off, and skips the 24h throttle.
    void CheckForUpdatesAsync(bool force = false, bool manual = false) {
        if constexpr (kUiTest) return;
        if (!manual && !settings_.checkForUpdates) return;
        if (manual) force = true;
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        // Throttle to at most once per 24h unless explicitly forced (startup
        // used to pass force=true unconditionally, so this check was never
        // actually consulted and the update endpoint got hit on every launch).
        if (!force && !takeoff::ShouldCheckForUpdates(lastUpdateCheck_, now, settings_.checkForUpdates)) {
            return;
        }
        if (updateInProgress_->exchange(true)) {
            // Already checking or downloading, do not block UI. The one
            // exception: the worker has already posted its result (the row
            // no longer reads Checking/Downloading) and is only unwinding, so
            // a manual click would be silently dropped - wait for it instead.
            const bool workerFinishing = updateState_ != takeoff::UpdateCheckState::Checking &&
                updateState_ != takeoff::UpdateCheckState::Downloading;
            if (!manual || !workerFinishing || !updateThread_.joinable()) return;
            updateThread_.join();
            *updateInProgress_ = true; // the worker's guard cleared it on exit
        }
        if (updateThread_.joinable()) {
            updateThread_.join();
        }
        lastUpdateCheck_ = now;
        SaveLastUpdateCheck(now);
        updateState_ = takeoff::UpdateCheckState::Checking;
        InvalidateRect(hwnd_, nullptr, FALSE);
        const HWND hwnd = hwnd_;
        const std::wstring host = apiHost_;
        const std::wstring path = apiPath_;
        try {
            // The flag is held by shared_ptr and captured by value: shutdown may
            // detach this thread, so the guard can outlive the LauncherWindow.
            updateThread_ = std::thread([hwnd, host, path, flag = updateInProgress_] {
                struct Guard {
                    std::shared_ptr<std::atomic<bool>> flag;
                    ~Guard() { *flag = false; }
                } guard{flag};

                std::wstring tag;
                std::wstring htmlUrl;
                std::wstring assetUrl;
                // The handler takes ownership of the posted result, so only
                // release it once the post is known to have succeeded - it
                // fails if shutdown got there first.
                auto post = [hwnd, &tag, &htmlUrl](UINT message, WPARAM verdict, std::wstring stagedPath = {},
                                                   std::string sha256 = {}) {
                    auto p = std::make_unique<takeoff::UpdateCheckResult>(
                        takeoff::UpdateCheckResult{tag, htmlUrl, std::move(stagedPath), std::move(sha256)});
                    if (PostMessageW(hwnd, message, verdict, reinterpret_cast<LPARAM>(p.get()))) {
                        p.release();
                    }
                };
                if (!takeoff::QueryLatestReleaseInfo(host, path, tag, htmlUrl, assetUrl)) {
                    post(kUpdateCheckCompletedMessage, 3);
                    return;
                }
                if (takeoff::IsNewerVersion(tag, takeoff::kAppVersion)) {
                    const std::wstring stagingPath = takeoff::GetUpdateStagingPath(tag);
                    // A file left in the staging folder by an earlier session has no
                    // hash pinned by this session (the folder is user-writable), so
                    // it is downloaded again rather than trusted for an elevated install.
                    if (!assetUrl.empty() && !stagingPath.empty()) {
                        DeleteFileW(stagingPath.c_str());
                        post(kUpdateProgressMessage, 0);
                        std::string sha256;
                        if (takeoff::DownloadUpdateFile(assetUrl, stagingPath, &sha256)) {
                            post(kUpdateCheckCompletedMessage, 2, stagingPath, std::move(sha256));
                            return;
                        }
                    }
                    post(kUpdateCheckCompletedMessage, 1);
                    return;
                }
                post(kUpdateCheckCompletedMessage, 0);
            });
        } catch (const std::system_error&) {
            *updateInProgress_ = false;
            // Don't leave the row stuck on "Checking…" (unclickable).
            updateState_ = takeoff::NextUpdateState(3, updateDownloaded_, updateAvailable_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    bool SetRunAtStartup(bool enabled) {
        if constexpr (kUiTest) {
            return true;
        } else {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kStartupRegistryPath, 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
                return false;
            }
            LONG result = ERROR_SUCCESS;
            if (enabled) {
                wchar_t executable[MAX_PATH]{};
                if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
                    RegCloseKey(key);
                    return false;
                }
                const std::wstring command = quicklaunch::StartupCommandFor(executable);
                result = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            } else {
                result = RegDeleteValueW(key, kStartupValueName);
                if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
            }
            RegCloseKey(key);
            return result == ERROR_SUCCESS;
        }
    }

    bool CreateStartMenuShortcut() {
        if constexpr (kUiTest) return true;
        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return false;

        PWSTR programsPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programsPath))) {
            return false;
        }

        std::wstring shortcutPath = std::wstring(programsPath) + L"\\Lean Launcher.lnk";
        CoTaskMemFree(programsPath);

        ComPtr<IShellLinkW> shellLink;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink)))) {
            return false;
        }

        shellLink->SetPath(executable);
        fs::path exeFs(executable);
        shellLink->SetWorkingDirectory(exeFs.parent_path().c_str());
        shellLink->SetDescription(L"Lean Launcher - Quick Obsidian Task Capture");

        ComPtr<IPersistFile> persistFile;
        if (FAILED(shellLink.As(&persistFile))) {
            return false;
        }

        if (SUCCEEDED(persistFile->Save(shortcutPath.c_str(), TRUE))) {
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, shortcutPath.c_str(), nullptr);
            return true;
        }
        return false;
    }

    void EnsureStartMenuShortcut() {
        if constexpr (kUiTest) return;
        HKEY key = nullptr;
        DWORD added = 0;
        DWORD size = sizeof(added);
        bool shouldAdd = false;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS) {
            if (RegGetValueW(key, nullptr, L"AddedToStartMenu", RRF_RT_REG_DWORD, nullptr, &added, &size) != ERROR_SUCCESS) {
                shouldAdd = true;
            }
        } else {
            shouldAdd = true;
        }

        if (shouldAdd) {
            CreateStartMenuShortcut();
            if (!key) {
                RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
            }
            if (key) {
                added = 1;
                RegSetValueExW(key, L"AddedToStartMenu", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&added), sizeof(added));
            }
        }
        if (key) RegCloseKey(key);
    }

    void UpdateTrayIcon() {
        if constexpr (!kUiTest) {
            if (!settings_.showTrayIcon && !balloonTempIcon_) {
                RemoveTrayIcon();
                return;
            }
            AddTrayIcon();
        }
    }

    void AddTrayIcon() {
        if constexpr (!kUiTest) {
            NOTIFYICONDATAW data{sizeof(data)};
            data.hWnd = hwnd_;
            data.uID = 1;
            // NIF_SHOWTIP is required for the hover tooltip to actually render
            // once NIM_SETVERSION below opts into NOTIFYICON_VERSION_4 - NIF_TIP
            // alone stores szTip but the shell won't display it under v4 behavior.
            data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            data.uCallbackMessage = kTrayMessage;
            // Load the crisp small variant from the .ico; the shell copies it.
            HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
            data.hIcon = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
            const std::wstring tip = pomodoro_
                ? leanlauncher::pomodoro::TooltipText(pomodoro_->kind,
                    leanlauncher::pomodoro::RemainingSeconds(pomodoro_->endTicks, NowTicks()), pomodoro_->label)
                : std::wstring(L"Lean Launcher");
            wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
            if (Shell_NotifyIconW(trayIconAdded_ ? NIM_MODIFY : NIM_ADD, &data)) {
                trayIconAdded_ = true;
                data.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &data);
            }
            if (icon) DestroyIcon(icon);
        }
    }

    void RemoveTrayIcon() {
        if (!trayIconAdded_) return;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = hwnd_;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayIconAdded_ = false;
    }

    void RestartToUpdate() {
        std::wstring targetPath = downloadedUpdatePath_;
        if (targetPath.empty() || !takeoff::ValidateExecutableFile(targetPath)) {
            wchar_t localAppData[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0 && localAppData[0]) {
                std::error_code ec;
                std::filesystem::path updateDir = std::filesystem::path(localAppData) / L"LeanLauncher" / L"updates";
                for (const auto& entry : std::filesystem::directory_iterator(updateDir, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == L".exe") {
                        if (takeoff::ValidateExecutableFile(entry.path().wstring())) {
                            targetPath = entry.path().wstring();
                            break;
                        }
                    }
                }
            }
        }
        // The pinned hash only belongs to the file it was computed for.
        const std::string expectedSha256 = targetPath == downloadedUpdatePath_ ? downloadedUpdateSha256_ : std::string();
        const takeoff::ApplyResult result = targetPath.empty()
            ? takeoff::ApplyResult::Failed
            : takeoff::ApplyUpdateAndRestart(targetPath, expectedSha256, hwnd_);
        switch (takeoff::RestartReactionFor(result)) {
        case takeoff::RestartReaction::ExitLauncher:
            DestroyWindow(hwnd_);
            return;
        case takeoff::RestartReaction::StayWithMessage:
            MessageBoxW(hwnd_, takeoff::RestartMessageFor(result), L"Lean Launcher update",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return;
        case takeoff::RestartReaction::StayMessageAndOpenReleases:
            MessageBoxW(hwnd_, takeoff::RestartMessageFor(result), L"Lean Launcher update",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            ShellExecuteW(nullptr, L"open", releasesUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }

    void HandleTrayMessage(UINT message) {
        if (message == NIN_BALLOONUSERCLICK || message == NIN_BALLOONTIMEOUT || message == NIN_BALLOONHIDE) {
            if (message == NIN_BALLOONUSERCLICK && pomodoroBreakOffer_ && settings_.enablePomodoro && !pomodoro_) {
                StartPomodoro(leanlauncher::pomodoro::Kind::Break, PomodoroMinutes(settings_.pomodoroBreakMinutes, 5), L"");
            }
            pomodoroBreakOffer_ = false;
            if (balloonTempIcon_) {
                balloonTempIcon_ = false;
                if (!settings_.showTrayIcon) RemoveTrayIcon();
            }
            return;
        }
        if (message == WM_LBUTTONUP || message == NIN_SELECT || message == NIN_KEYSELECT) {
            Show();
            return;
        }
        if (message != WM_RBUTTONUP && message != WM_CONTEXTMENU) return;
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        if (updateDownloaded_) {
            AppendMenuW(menu, MF_STRING, 5, L"Restart to Update");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, 1, L"Open Lean Launcher");
        AppendMenuW(menu, MF_STRING, 2, L"Settings");
        AppendMenuW(menu, MF_STRING, 4, L"Reload Programs");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Exit");
        SetForegroundWindow(hwnd_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
            TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (command == 1) Show();
        else if (command == 2) { Show(); OpenSettings(); }
        else if (command == 3) DestroyWindow(hwnd_);
        else if (command == 4) TriggerAppReindex();
        else if (command == 5) RestartToUpdate();
    }

    void ApplyBackdrop() {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        const bool contrastOn = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        DWORD transparency = 1, size = sizeof(transparency);
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
        BOOL compositionEnabled = FALSE;
        DwmIsCompositionEnabled(&compositionEnabled);
        const bool allowBlur = compositionEnabled && transparency && !contrastOn &&
            !GetSystemMetrics(SM_REMOTESESSION);

        // Re-applying identical DWM attributes and window regions on every
        // WM_SETTINGCHANGE broadcast makes DWM recompute the frame, which shows
        // as random white outline flashes. Only touch DWM when state changed.
        if (backdropApplied_ && contrastOn == highContrast_ && allowBlur == allowBlur_) {
            return;
        }
        if (contrastOn != highContrast_) ReleasePreviewLayout();  // its effect brushes use contrast colours
        highContrast_ = contrastOn;
        allowBlur_ = allowBlur;
        backdropApplied_ = true;

        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
        const DWORD corner = kDwmwcpRound;
        nativeCorners_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &corner, sizeof(corner)));
        const DWORD border = kDwmColorNone;
        DwmSetWindowAttribute(hwnd_, kDwmwaBorderColor, &border, sizeof(border));

        // Prefer documented Windows 11 transient acrylic. Never stack it with accent blur.
        const DWORD backdrop = allowBlur ? kDwmBackdropTransient : kDwmBackdropNone;
        acrylic_ = allowBlur && SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop)));
        if (!allowBlur) DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop));

        struct AccentPolicy { int state; DWORD flags; DWORD color; DWORD animation; };
        struct AttributeData { int attribute; void* data; SIZE_T size; };
        using SetComposition = BOOL(WINAPI*)(HWND, AttributeData*);
        const auto setComposition = reinterpret_cast<SetComposition>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
        if (!acrylic_ && setComposition) {
            // Windows 10 has no documented desktop-acrylic API. Use its compositor
            // accent policy only as a fallback, with an opaque fallback on failure.
            AccentPolicy policy{allowBlur ? 4 : 0, 0, 0xB8242424, 0};
            AttributeData data{19, &policy, sizeof(policy)};
            const BOOL applied = setComposition(hwnd_, &data);
            if (policy.state == 4) acrylic_ = applied != FALSE;
        }
        const MARGINS margins = acrylic_ ? MARGINS{-1, -1, -1, -1} : MARGINS{1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hwnd_, &margins);
        UpdateRegion();
    }

    void UpdateRegion() {
        // Never ask SetWindowRgn to redraw: an immediate frame redraw races the
        // Direct2D present and flashes the raw window outline. Callers repaint.
        if (nativeCorners_) {
            SetWindowRgn(hwnd_, nullptr, FALSE);
        } else {
            RECT client{};
            GetClientRect(hwnd_, &client);
            HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                ToPixel(20), ToPixel(20));
            if (!SetWindowRgn(hwnd_, region, FALSE)) DeleteObject(region);
        }
    }

    void ResizeAndPosition() {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
        const float workHeight = ToDip(info.rcWork.bottom - info.rcWork.top);
        const float workLeft = ToDip(info.rcWork.left);
        const float workWidth = ToDip(info.rcWork.right - info.rcWork.left);
        width_ = (std::min)(kWidth, workWidth - 32.0f);
        visibleRows_ = std::clamp(static_cast<int>(
            (workHeight - 64 - kSearchHeight - kSectionHeight - kFooterHeight - 8) / kRowHeight),
            1, kVisibleRows);
        height_ = ResultsTop() + visibleRows_ * kRowHeight + 8 + kFooterHeight;
        // US-045: width_ stays the launcher area; the side panel adds
        // panelWidth_ to its right. Too narrow a work area shows the panel as
        // an overlay over the results instead, at the launcher's own width.
        const float centeredLeft = workLeft + (workWidth - width_) / 2;
        float windowWidth = width_, windowLeft = centeredLeft;
        panelWidth_ = 0;
        previewOverlay_ = false;
        if (PreviewVisible()) {
            const auto place = leanlauncher::preview::PanelGeometry(workLeft, workWidth, width_, centeredLeft);
            previewOverlay_ = place.overlay;
            panelWidth_ = place.overlay ? 0.0f : leanlauncher::preview::kPanelWidth;
            windowWidth = place.windowWidthDip;
            windowLeft = place.windowLeftDip;
        }
        const int width = ToPixel(windowWidth), height = ToPixel(height_);
        // Closed (or overlay): keep the pixel-exact centring used before the
        // panel existed, so the launcher doesn't shift by a rounding pixel.
        const int x = panelWidth_ > 0 ? ToPixel(windowLeft)
            : info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
        const int idealY = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top) * 20 / 100;
        const int y = (std::max)(static_cast<int>(info.rcWork.top) + 8,
            (std::min)(idealY, static_cast<int>(info.rcWork.bottom) - height - 8));
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_NOCOPYBITS);
        EnsureVisible();
        UpdateRegion();
    }

    void Show() {
        KillTimer(hwnd_, kTrimTimer);
        page_ = Page::Launcher;
        hotkeyWarningDismissed_ = false;
        input_.Clear();
        pendingSurrogate_ = 0;
        composition_.clear();
        textScroll_ = 0;
        selected_ = firstVisible_ = 0;
        actionsOpen_ = false;
        status_.clear();
        mouseKnown_ = false;
        hoverLockRow_ = -1;
        webSearchCardHovered_ = false;
        if (!IsWindowVisible(hwnd_)) {
            // Fresh open: whatever has focus now is the target (none if it is
            // the launcher, the taskbar or the desktop). Already open: keep it.
            HWND foreground = GetForegroundWindow();
            if (foreground) {
                wchar_t cls[64]{};
                GetClassNameW(foreground, cls, static_cast<int>(std::size(cls)));
                if (foreground == hwnd_ || foreground == GetShellWindow() || foreground == GetDesktopWindow() ||
                    wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                    wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
                    foreground = nullptr;
                }
            }
            snippetTarget_ = foreground;
        }
        ReloadSnippetsIfChanged();
        UpdateResults();
        ResizeAndPosition();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        ResetCaret();
        if constexpr (!kUiTest) {
            const auto now = std::chrono::steady_clock::now();
            if (lastIndexTime_.time_since_epoch().count() > 0 &&
                now - lastIndexTime_ > std::chrono::minutes(5)) {
                TriggerAppReindex();
            }
        }
    }

    enum class SettingsCategory : uint8_t { General, Search, Tools, Snippets, Obsidian, About };

    // Settings rows 0-3: keyboard shortcuts. 4-6: system (4-5 General; 6, the
    // startup update check, lives in About's UPDATES card). 7-14: search (7
    // File search, 8 Web search, 9 Search engine picker - US-016; 10-12 add
    // the File/Web/App search prefix fields - US-017; 13-14 add Edit
    // exclusions.../Help - US-019). 15-39: Obsidian (only row 15 is active
    // when settings_.obsidianEnabled is false - see
    // IsRowInCategory/ObsidianRowCount). Each of the five *Summary rows is
    // always shown when Obsidian is enabled; its detail rows only appear
    // while obsidianExpandedSection_ names that section - see
    // ObsidianVisibleRows(). 40-43 are later Obsidian additions (capture
    // target notes - US-025; quick-open target - US-026), numbered after 39
    // so no existing row ID shifts - like every Obsidian row, their screen
    // position comes from ObsidianVisibleRows(), not the number. 44-45 are the
    // About tab's rows, check now then GitHub, in screen order (they
    // sit in no card list - see IsRowInCategory). 46 is the Reset button, handled as a sentinel row rather than a real
    // settings row.
    // Rows 9-14 are not part of the Obsidian block below - they live in the
    // Search category alongside rows 7-8 - so every Obsidian row constant
    // shifts relative to the row it would otherwise have under a plain
    // contiguous 0.. numbering.
    static constexpr int kRowWebSearchEngine = 9;
    static constexpr int kRowFileSearchPrefix = 10;
    static constexpr int kRowWebSearchPrefix = 11;
    static constexpr int kRowAppSearchPrefix = 12;
    static constexpr int kRowFileSearchEditExclusions = 13;
    static constexpr int kRowFileSearchHelp = 14;
    static constexpr int kRowObsidianEnabled = 15;
    static constexpr int kRowVaultPicker = 16;
    static constexpr int kRowVaultSearchSummary = 17;
    static constexpr int kRowVaultSearchEnabled = 18;
    static constexpr int kRowVaultSearchPrefix = 19;
    static constexpr int kRowVaultSearchPillLabel = 20;
    static constexpr int kRowTaskSummary = 21;
    static constexpr int kRowTaskAddEnabled = 22;
    static constexpr int kRowTaskPrefix = 23;
    static constexpr int kRowTaskPillLabel = 24;
    static constexpr int kRowTaskPreviewPrefix = 25;
    static constexpr int kRowNoteAddSummary = 26;
    static constexpr int kRowNoteAddEnabled = 27;
    static constexpr int kRowNoteAddPrefix = 28;
    static constexpr int kRowNoteAddPillLabel = 29;
    static constexpr int kRowNoteAddPreviewPrefix = 30;
    static constexpr int kRowLogSummary = 31;
    static constexpr int kRowLogEnabled = 32;
    static constexpr int kRowLogPrefix = 33;
    static constexpr int kRowLogPillLabel = 34;
    static constexpr int kRowLogPreviewPrefix = 35;
    static constexpr int kRowLogHeading = 36;
    static constexpr int kRowOverridesSummary = 37;
    static constexpr int kRowDailyNoteFolderOverride = 38;
    static constexpr int kRowDailyNoteFormatOverride = 39;
    static constexpr int kRowTaskTargetNote = 40;
    static constexpr int kRowNoteAddTargetNote = 41;
    static constexpr int kRowLogTargetNote = 42;
    static constexpr int kRowQuickOpenTarget = 43;
    static constexpr int kRowAboutCheckUpdates = 44;
    static constexpr int kRowAboutGithubLink = 45;
    static constexpr int kRowResetToDefaults = 46;
    // 47+: feature toggles (NFR-018), numbered after the Reset sentinel so no
    // existing row ID shifts; their position comes from the tab card lists below.
    static constexpr int kRowSystemCommandsEnabled = 47;
    static constexpr int kRowSystemCommandsPrefix = 48;
    static constexpr int kRowTypedUrlsEnabled = 49;
    static constexpr int kRowPathCompletionEnabled = 50;
    static constexpr int kRowAboutExportSettings = 51;  // US-044, General tab BACKUP card
    static constexpr int kRowAboutImportSettings = 52;
    static constexpr int kRowUnitConverterEnabled = 53;
    static constexpr int kRowTimeZonesEnabled = 54;
    static constexpr int kRowPomodoroEnabled = 55;  // US-049
    static constexpr int kRowPomodoroPrefix = 56;
    static constexpr int kRowPomodoroFocusMinutes = 57;
    static constexpr int kRowPomodoroBreakMinutes = 58;
    static constexpr int kRowPomodoroLog = 59;
    static constexpr int kRowPreviewHotkey = 60;   // US-045
    static constexpr int kRowPreviewEnabled = 61;
    static constexpr int kRowSnippetsEnabled = 62;  // US-050
    static constexpr int kRowSnippetsPrefix = 63;
    static constexpr int kRowSnippetsFile = 64;
    static constexpr int kRowSnippetsOpen = 65;
    static constexpr int kRowSnippetsImport = 66;
    static constexpr int kSettingsMaxRow = 66;
    static constexpr int kRowCheckForUpdatesOnStart = 6;  // shown in About's UPDATES card

    // Every tab except Obsidian (an accordion) and About (fixed layout) is an
    // ordered list of cards. A row's position comes from its place in these
    // lists, not its number, so a feature toggle (NFR-018) is added by
    // appending one entry - see settings_layout.h for the geometry.
    struct SettingsCard {
        const wchar_t* header;
        const int* rows;
        int count;
    };
    struct SettingsCardList {
        const SettingsCard* cards;
        std::size_t count;
    };
    static constexpr int kShortcutRows[] = {0, 1, 2, 3};
    static constexpr int kStartupRows[] = {4, 5};
    static constexpr int kBackupRows[] = {kRowAboutExportSettings, kRowAboutImportSettings};
    static constexpr int kSearchSourceRows[] = {7, 8, kRowWebSearchEngine};
    static constexpr int kSearchPrefixRows[] = {kRowFileSearchPrefix, kRowWebSearchPrefix, kRowAppSearchPrefix};
    static constexpr int kSearchExclusionRows[] = {kRowFileSearchEditExclusions, kRowFileSearchHelp};
    static constexpr int kSearchResultsRows[] = {kRowPreviewEnabled, kRowPreviewHotkey};
    static constexpr int kQuickAnswerRows[] = {
        kRowUnitConverterEnabled, kRowTimeZonesEnabled, kRowTypedUrlsEnabled, kRowPathCompletionEnabled};
    static constexpr int kSystemCommandRows[] = {kRowSystemCommandsEnabled, kRowSystemCommandsPrefix};
    static constexpr int kPomodoroRows[] = {
        kRowPomodoroEnabled, kRowPomodoroPrefix, kRowPomodoroFocusMinutes, kRowPomodoroBreakMinutes, kRowPomodoroLog};
    static constexpr int kSnippetRows[] = {
        kRowSnippetsEnabled, kRowSnippetsPrefix, kRowSnippetsFile, kRowSnippetsOpen, kRowSnippetsImport};

    static constexpr SettingsCard kGeneralCards[] = {
        {L"KEYBOARD SHORTCUTS", kShortcutRows, static_cast<int>(std::size(kShortcutRows))},
        {L"STARTUP", kStartupRows, static_cast<int>(std::size(kStartupRows))},
        {L"BACKUP", kBackupRows, static_cast<int>(std::size(kBackupRows))},
    };
    static constexpr SettingsCard kSearchCards[] = {
        {L"SOURCES", kSearchSourceRows, static_cast<int>(std::size(kSearchSourceRows))},
        {L"PREFIXES", kSearchPrefixRows, static_cast<int>(std::size(kSearchPrefixRows))},
        {L"FILE EXCLUSIONS", kSearchExclusionRows, static_cast<int>(std::size(kSearchExclusionRows))},
        {L"RESULTS", kSearchResultsRows, static_cast<int>(std::size(kSearchResultsRows))},
    };
    static constexpr SettingsCard kToolsCards[] = {
        {L"QUICK ANSWERS", kQuickAnswerRows, static_cast<int>(std::size(kQuickAnswerRows))},
        {L"SYSTEM COMMANDS", kSystemCommandRows, static_cast<int>(std::size(kSystemCommandRows))},
        {L"POMODORO", kPomodoroRows, static_cast<int>(std::size(kPomodoroRows))},
    };
    static constexpr SettingsCard kSnippetCards[] = {
        {L"TEXT EXPANDER", kSnippetRows, static_cast<int>(std::size(kSnippetRows))},
    };

    // Empty for Obsidian and About, which lay themselves out.
    static constexpr SettingsCardList CardsFor(SettingsCategory cat) {
        switch (cat) {
            case SettingsCategory::General: return {kGeneralCards, std::size(kGeneralCards)};
            case SettingsCategory::Search: return {kSearchCards, std::size(kSearchCards)};
            case SettingsCategory::Tools: return {kToolsCards, std::size(kToolsCards)};
            case SettingsCategory::Snippets: return {kSnippetCards, std::size(kSnippetCards)};
            default: return {nullptr, 0};
        }
    }

    // Tab strip, in screen order.
    static constexpr SettingsCategory kSettingsCategories[] = {
        SettingsCategory::General, SettingsCategory::Search, SettingsCategory::Tools,
        SettingsCategory::Snippets, SettingsCategory::Obsidian, SettingsCategory::About};
    static constexpr const wchar_t* kSettingsCategoryLabels[] = {
        L"General", L"Search", L"Tools", L"Snippets", L"Obsidian", L"About"};
    static constexpr float kSettingsTabWidths[] = {66.0f, 62.0f, 54.0f, 72.0f, 84.0f, 58.0f};

    // Which of the five Obsidian action blocks is currently expanded, or
    // -1 if all are collapsed. A single int gives accordion behavior for
    // free: setting it to a new section implicitly collapses whichever one
    // was open before.
    static constexpr int kSectionVaultSearch = 0;
    static constexpr int kSectionTask = 1;
    static constexpr int kSectionNoteAdd = 2;
    static constexpr int kSectionLog = 3;
    static constexpr int kSectionOverrides = 4;

    // The ordered list of Obsidian row IDs currently on screen. Every other
    // Obsidian-category geometry/hit-testing function derives its answer
    // from this single source of truth instead of doing arithmetic on the
    // row number directly - a row's screen position is its rank in this
    // list, not a fixed formula. Each action's summary row is always
    // present when Obsidian is enabled; its detail rows only appear while
    // that action is the expanded section.
    //
    // The list depends on nothing but obsidianEnabled and the expanded
    // section, yet hit-testing and painting ask for it dozens of times per
    // mouse move, so it is memoized on those two inputs. The returned
    // reference stays valid until one of them changes - callers must not
    // hold on to it across a settings mutation.
    const std::vector<int>& ObsidianVisibleRows() const {
        if (!obsidianRowsCache_.empty() &&
            obsidianRowsCacheEnabled_ == settings_.obsidianEnabled &&
            obsidianRowsCacheSection_ == obsidianExpandedSection_) {
            return obsidianRowsCache_;
        }
        obsidianRowsCacheEnabled_ = settings_.obsidianEnabled;
        obsidianRowsCacheSection_ = obsidianExpandedSection_;

        std::vector<int>& rows = obsidianRowsCache_;
        rows.clear();
        rows.push_back(kRowObsidianEnabled);
        if (!settings_.obsidianEnabled) return rows;
        rows.push_back(kRowVaultPicker);

        rows.push_back(kRowVaultSearchSummary);
        if (obsidianExpandedSection_ == kSectionVaultSearch) {
            rows.push_back(kRowVaultSearchEnabled);
            rows.push_back(kRowVaultSearchPrefix);
            rows.push_back(kRowVaultSearchPillLabel);
            rows.push_back(kRowQuickOpenTarget);
        }

        rows.push_back(kRowTaskSummary);
        if (obsidianExpandedSection_ == kSectionTask) {
            rows.push_back(kRowTaskAddEnabled);
            rows.push_back(kRowTaskPrefix);
            rows.push_back(kRowTaskPillLabel);
            rows.push_back(kRowTaskPreviewPrefix);
            rows.push_back(kRowTaskTargetNote);
        }

        rows.push_back(kRowNoteAddSummary);
        if (obsidianExpandedSection_ == kSectionNoteAdd) {
            rows.push_back(kRowNoteAddEnabled);
            rows.push_back(kRowNoteAddPrefix);
            rows.push_back(kRowNoteAddPillLabel);
            rows.push_back(kRowNoteAddPreviewPrefix);
            rows.push_back(kRowNoteAddTargetNote);
        }

        rows.push_back(kRowLogSummary);
        if (obsidianExpandedSection_ == kSectionLog) {
            rows.push_back(kRowLogEnabled);
            rows.push_back(kRowLogPrefix);
            rows.push_back(kRowLogPillLabel);
            rows.push_back(kRowLogPreviewPrefix);
            rows.push_back(kRowLogHeading);
            rows.push_back(kRowLogTargetNote);
        }

        rows.push_back(kRowOverridesSummary);
        if (obsidianExpandedSection_ == kSectionOverrides) {
            rows.push_back(kRowDailyNoteFolderOverride);
            rows.push_back(kRowDailyNoteFormatOverride);
        }
        return rows;
    }

    int ObsidianRowCount() const { return static_cast<int>(ObsidianVisibleRows().size()); }

    // -1 if row isn't currently visible in the Obsidian section.
    int ObsidianRowRank(int row) const {
        const auto& rows = ObsidianVisibleRows();
        const auto it = std::find(rows.begin(), rows.end(), row);
        return it == rows.end() ? -1 : static_cast<int>(std::distance(rows.begin(), it));
    }

    bool IsRowInCategory(int row, SettingsCategory cat) const {
        if (cat == SettingsCategory::Obsidian) return ObsidianRowRank(row) >= 0;
        if (cat == SettingsCategory::About) {
            return row == kRowCheckForUpdatesOnStart || row == kRowAboutCheckUpdates ||
                row == kRowAboutGithubLink;
        }
        const auto list = CardsFor(cat);
        return leanlauncher::settings_layout::FindRow(list.cards, list.count, row).Found();
    }

    // Every row of a tab in screen order (Reset excluded).
    std::vector<int> CategoryRowsInOrder(SettingsCategory cat) const {
        std::vector<int> rows;
        if (cat == SettingsCategory::Obsidian) return ObsidianVisibleRows();
        if (cat == SettingsCategory::About) {
            return {kRowCheckForUpdatesOnStart, kRowAboutCheckUpdates, kRowAboutGithubLink};
        }
        const auto list = CardsFor(cat);
        for (std::size_t c = 0; c < list.count; ++c) {
            rows.insert(rows.end(), list.cards[c].rows, list.cards[c].rows + list.cards[c].count);
        }
        return rows;
    }

    int FirstRowInCategory(SettingsCategory cat) const {
        const auto rows = CategoryRowsInOrder(cat);
        return rows.empty() ? 0 : rows.front();
    }

    int LastRowInCategory(SettingsCategory cat) const {
        const auto rows = CategoryRowsInOrder(cat);
        return rows.empty() ? 0 : rows.back();
    }

    int NextSettingsRow(int current, int delta) const {
        std::vector<int> activeRows = CategoryRowsInOrder(settingsCategory_);
        activeRows.push_back(kRowResetToDefaults);
        auto it = std::find(activeRows.begin(), activeRows.end(), current);
        if (it == activeRows.end()) {
            return activeRows.empty() ? 0 : activeRows.front();
        }
        int idx = static_cast<int>(std::distance(activeRows.begin(), it));
        const int count = static_cast<int>(activeRows.size());
        idx = (idx + delta + count) % count;
        return activeRows[idx];
    }

    D2D1_RECT_F CategoryTabRect(SettingsCategory cat) const {
        constexpr float y = 11.0f;
        constexpr float h = 24.0f;
        float x = 160.0f;
        for (std::size_t i = 0; i < std::size(kSettingsCategories); ++i) {
            if (kSettingsCategories[i] == cat) return D2D1::RectF(x, y, x + kSettingsTabWidths[i], y + h);
            x += kSettingsTabWidths[i] + 6.0f;
        }
        return D2D1::RectF(0, y, 0, y + h);
    }

    D2D1_RECT_F ResetButtonRect() const {
        return D2D1::RectF(width_ - 136.0f, 10.0f, width_ - 20.0f, 36.0f);
    }

    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::Obsidian) {
            // 36 header offset + N rows + 16 bottom padding.
            return 36.0f + ObsidianRowCount() * kSettingsRowHeight + 16.0f;
        } else if (settingsCategory_ == SettingsCategory::About) {
            // INDEX card is last; 2 rows + 16 bottom padding.
            return AboutIndexCardTop() + 2 * kSettingsRowHeight + 16.0f;
        }
        const auto list = CardsFor(settingsCategory_);
        return leanlauncher::settings_layout::ContentBottom(list.cards, list.count);
    }

    // About tab layout: version/author lines, "UPDATES" header@66 and its
    // two-row card@86 (the startup-check toggle, then Check now - US-029), then
    // "LINKS" and "INDEX", each header 18px below the previous card
    // and its card 20px below the header - the same header/card spacing the
    // card-based tabs use.
    static constexpr float AboutUpdatesCardTop() { return 86.0f; }
    static constexpr float AboutLinksHeaderTop() { return AboutUpdatesCardTop() + 2 * kSettingsRowHeight + 18.0f; }
    static constexpr float AboutLinksCardTop() { return AboutLinksHeaderTop() + 20.0f; }
    static constexpr float AboutIndexHeaderTop() { return AboutLinksCardTop() + kSettingsRowHeight + 18.0f; }
    static constexpr float AboutIndexCardTop() { return AboutIndexHeaderTop() + 20.0f; }

    // "482113" -> "482,113" for the About tab's index counts.
    static std::wstring FormatCount(size_t value) {
        std::wstring digits = std::to_wstring(value);
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
            digits.insert(static_cast<size_t>(i), 1, L',');
        }
        return digits;
    }

    float SettingsContentHeight() const {
        return SettingsContentBottom();
    }

    float SettingsViewportHeight() const {
        return FooterTop() - kSettingsHeaderHeight;
    }

    float SettingsMaxScroll() const {
        return (std::max)(0.0f, SettingsContentBottom() - SettingsViewportHeight());
    }

    float SettingsRowTop(int row) const {
        if (settingsCategory_ == SettingsCategory::Obsidian) {
            return 36.0f + ObsidianRowRank(row) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::About) {
            if (row == kRowCheckForUpdatesOnStart) return AboutUpdatesCardTop();
            if (row == kRowAboutCheckUpdates) return AboutUpdatesCardTop() + kSettingsRowHeight;
            return AboutLinksCardTop();
        }
        const auto list = CardsFor(settingsCategory_);
        return leanlauncher::settings_layout::RowTop(list.cards, list.count, row);
    }

    int SettingsRowAtPoint(float x, float y) const {
        if (x < 16.0f || x > width_ - 16.0f) return -1;
        if (y < kSettingsHeaderHeight || y >= FooterTop()) return -1;
        const float contentY = (y - kSettingsHeaderHeight) + settingsScroll_;
        for (int r = 0; r <= kSettingsMaxRow; ++r) {
            if (!IsRowInCategory(r, settingsCategory_)) continue;
            const float rTop = SettingsRowTop(r);
            if (contentY >= rTop && contentY < rTop + kSettingsRowHeight) {
                return r;
            }
        }
        return -1;
    }

    void ScrollSettings(float delta) {
        const float maxScroll = SettingsMaxScroll();
        const float newScroll = std::clamp(settingsScroll_ + delta, 0.0f, maxScroll);
        if (newScroll != settingsScroll_) {
            settingsScroll_ = newScroll;
            if (vaultDropdownOpen_) CloseVaultDropdown();
            if (webSearchDropdownOpen_) CloseWebSearchDropdown();
            if (quickOpenDropdownOpen_) CloseQuickOpenDropdown();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void EnsureSettingsVisible(int row) {
        if (row == kRowResetToDefaults) {
            settingsScroll_ = 0.0f;
            return;
        }
        if (row < 0 || row > kSettingsMaxRow || !IsRowInCategory(row, settingsCategory_)) return;
        const float rTop = SettingsRowTop(row);
        const float rBottom = rTop + kSettingsRowHeight;
        const float maxScroll = SettingsMaxScroll();
        // Landing on the first row of a card scrolls its header into view too.
        float sectionHeaderTop = rTop;
        if (settingsCategory_ == SettingsCategory::Obsidian || settingsCategory_ == SettingsCategory::About) {
            if (row == kRowObsidianEnabled || row == kRowCheckForUpdatesOnStart) sectionHeaderTop = 16.0f;
        } else {
            const auto list = CardsFor(settingsCategory_);
            const auto slot = leanlauncher::settings_layout::FindRow(list.cards, list.count, row);
            if (slot.Found() && slot.rank == 0) {
                sectionHeaderTop = leanlauncher::settings_layout::CardHeaderTop(
                    list.cards, list.count, static_cast<std::size_t>(slot.card));
            }
        }
        const float visibleTop = sectionHeaderTop;
        const float visibleBottom = rBottom + 8.0f;

        if (visibleTop - settingsScroll_ < 2.0f) {
            settingsScroll_ = (std::max)(0.0f, visibleTop - 2.0f);
        } else if (visibleBottom - settingsScroll_ > SettingsViewportHeight() - 2.0f) {
            settingsScroll_ = (std::min)(maxScroll, visibleBottom - (SettingsViewportHeight() - 2.0f));
        }
    }

    void OpenSettings(SettingsCategory targetCategory = SettingsCategory::General) {
        page_ = Page::Settings;
        actionsOpen_ = false;
        dragging_ = false;
        settingsCategory_ = targetCategory;
        settingsSelected_ = FirstRowInCategory(targetCategory);
        settingsScroll_ = 0.0f;
        settingsDraggingScroll_ = false;
        settingsStatus_.clear();
        recordingRow_ = -1;
        editingRow_ = -1;
        vaultDropdownOpen_ = false;
        vaultDropdownHighlight_ = -1;
        webSearchDropdownOpen_ = false;
        webSearchDropdownHighlight_ = -1;
        quickOpenDropdownOpen_ = false;
        quickOpenDropdownHighlight_ = -1;
        obsidianExpandedSection_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        KillTimer(hwnd_, kCaretTimer);
        // PreviewVisible() is false on this page, so an open side panel
        // shrinks the window back to the launcher's own width.
        if (panelWidth_ > 0 || previewOverlay_) ResizeAndPosition();
        ClosePreview(false);  // nothing to show on this page; reloaded on the way back
        // FindKnownVaults() calls fs::exists() per known vault; run it off
        // the UI thread so a disconnected network-drive vault can't stall
        // Settings opening. knownVaults_ keeps its previous value (fine -
        // it rarely changes) until the scan posts back.
        const HWND hwnd = hwnd_;
        try {
            std::thread([hwnd] {
                auto result = std::make_unique<std::vector<std::wstring>>(
                    leanlauncher::obsidian::FindKnownVaults());
                // Ownership transfers to the message handler only if the post
                // succeeds - a shutdown in flight makes it fail, and the
                // payload would otherwise leak.
                if (PostMessageW(hwnd, leanlauncher::obsidian::kKnownVaultsReadyMessage,
                        0, reinterpret_cast<LPARAM>(result.get()))) {
                    result.release();
                }
            }).detach();
        } catch (const std::system_error&) {
            // Thread creation failed - keep whatever knownVaults_ already has.
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CloseSettings() {
        page_ = Page::Launcher;
        recordingRow_ = -1;
        editingRow_ = -1;
        vaultDropdownOpen_ = false;
        vaultDropdownHighlight_ = -1;
        webSearchDropdownOpen_ = false;
        webSearchDropdownHighlight_ = -1;
        quickOpenDropdownOpen_ = false;
        quickOpenDropdownHighlight_ = -1;
        obsidianExpandedSection_ = -1;
        settingsScroll_ = 0.0f;
        settingsDraggingScroll_ = false;
        if (PreviewVisible()) ResizeAndPosition();  // re-widen for a remembered open panel
        SchedulePreview();
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Hide() {
        if (composing_) {
            if (HIMC context = ImmGetContext(hwnd_)) {
                ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(hwnd_, context);
            }
        }
        snippetTarget_ = nullptr;
        composing_ = false;
        pendingSurrogate_ = 0;
        composition_.clear();
        actionsOpen_ = false;
        adminActionHovered_ = false;
        dragging_ = false;
        CancelCommandConfirm();
        recycleBinItems_ = -1;  // re-query next time the row shows
        ReleasePathCompletion();
        ClosePreview(false);  // NFR-018: content goes; the remembered open state stays
        if (GetCapture() == hwnd_) ReleaseCapture();
        KillTimer(hwnd_, kCaretTimer);
        ShowWindow(hwnd_, SW_HIDE);
        // After a while hidden, release idle pages so the resident process
        // stays cheap. Quick toggles never wait: showing kills this timer.
        SetTimer(hwnd_, kTrimTimer, 10000, nullptr);
    }

    void InvalidateSearch() {
        RECT rect{0, 0, ToPixel(width_), ToPixel(kSearchHeight)};
        InvalidateRect(hwnd_, &rect, FALSE);
    }

    void ResetCaret() {
        caretVisible_ = true;
        KillTimer(hwnd_, kCaretTimer);
        const UINT blink = GetCaretBlinkTime();
        if (IsWindowVisible(hwnd_) && GetFocus() == hwnd_ && blink != INFINITE && blink != 0) {
            SetTimer(hwnd_, kCaretTimer, blink, nullptr);
        }
        InvalidateSearch();
    }

    void OnQueryChanged() {
        selected_ = firstVisible_ = 0;
        status_.clear();
        actionsOpen_ = false;
        CancelCommandConfirm();
        ResetCaret();
        UpdateResults();
    }

    // Every way the result list is rebuilt ends here, so the preview (US-045)
    // follows the selection whichever branch BuildResults() returns from.
    void UpdateResults() {
        BuildResults();
        SchedulePreview();
    }

    void BuildResults() {
        results_.clear();
        hoverLockRow_ = -1;
        int topAppScore = -1;
        if (apps_.size() > baseAppsCount_) {
            apps_.resize(baseAppsCount_);
        }
        // US-017: "f "/"p " narrow this pass to just files or just apps by
        // stripping the prefix and gating which of the two ranking sources
        // below run. Only attempted when the underlying feature is on (File
        // search for "f"; "p" has no toggle) - otherwise the raw text is
        // scored normally below, same as Task/Note/Log when their own
        // toggle is off.
        // US-041: "s " lists every system command, "s re" narrows them;
        // nothing else is shown while the prefix is typed.
        std::wstring commandFilter;
        if (settings_.enableSystemCommands && leanlauncher::syscmd::TryParseCommandPrefix(
                input_.text, settings_.systemCommandsPrefix, commandFilter)) {
            ShowCommandResults(Normalize(commandFilter));
            return;
        }
        // US-050: ", " lists snippets, ", sig" narrows them.
        std::wstring snippetFilter;
        if (settings_.enableSnippets && snippetIndex_ && !settings_.snippetsPrefix.empty() &&
            leanlauncher::syscmd::TryParseCommandPrefix(input_.text, settings_.snippetsPrefix, snippetFilter)) {
            ShowSnippetResults(Normalize(snippetFilter));
            return;
        }
        // US-049: "pomo ..." shows only timer rows.
        if (settings_.enablePomodoro && ShowPomodoroResults()) return;
        // US-043: a typed path shows only completions for that folder.
        if (settings_.enablePathCompletion && leanlauncher::typed::LooksLikePath(TrimmedQuery())) {
            ShowPathResults();
            return;
        }
        std::wstring scopedText = input_.text;
        bool fileSearchOnly = false;
        bool appSearchOnly = false;
        std::wstring scopedRemainder;
        if (settings_.enableFileSearch &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.fileSearchPrefix, scopedRemainder)) {
            fileSearchOnly = true;
            scopedText = scopedRemainder;
        } else if (leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.appSearchPrefix, scopedRemainder)) {
            appSearchOnly = true;
            scopedText = scopedRemainder;
        }
        const std::wstring query = Normalize(scopedText);
        if (input_.text.empty()) {
            // Pins first (US-024) - pinned files included, a deliberate
            // exception to this otherwise apps-only view - then the
            // recency view, skipping anything already shown as a pin.
            const size_t appCount = apps_.size();
            for (size_t p = 0; p < pins_.size(); ++p) {
                if (pins_[p].isApp) {
                    if (pinnedAppIndex_[p] < appCount) results_.push_back(pinnedAppIndex_[p]);
                    continue;
                }
                AppEntry entry;
                if (BuildPinnedFileEntry(p, entry)) {
                    results_.push_back(apps_.size());
                    apps_.push_back(std::move(entry));
                }
            }
            for (size_t index : recent_) {
                if (index < appCount && PinRank(index) < 0) results_.push_back(index);
            }
            for (size_t i = 0; i < appCount; ++i) {
                // US-041: commands only appear when typed, never in the empty view.
                if (apps_[i].category == takeoff::AppCategory::Command) continue;
                if (std::find(recent_.begin(), recent_.end(), i) == recent_.end() && PinRank(i) < 0) {
                    results_.push_back(i);
                }
            }
        } else if (!query.empty()) {
            std::vector<RankedResult> ranked;
            ranked.reserve(apps_.size());
            if (!fileSearchOnly) {
                for (size_t i = 0; i < apps_.size(); ++i) {
                    if (apps_[i].category == takeoff::AppCategory::Command) {
                        // US-041: exact name or alias outranks settings pages.
                        if (!settings_.enableSystemCommands) continue;
                        const int commandScore = leanlauncher::syscmd::ScoreCommand(
                            apps_[i].normalizedName, apps_[i].aliases, query);
                        if (commandScore >= 0) ranked.push_back({i, commandScore});
                        continue;
                    }
                    int recencyRank = -1;
                    auto it = std::find(recent_.begin(), recent_.end(), i);
                    if (it != recent_.end()) {
                        recencyRank = static_cast<int>(std::distance(recent_.begin(), it));
                    }
                    const int score = takeoff::ScoreApp(apps_[i].normalizedName, apps_[i].aliases, query, recencyRank);
                    if (score >= 0) ranked.push_back({i, score});
                }
            }
            if (!appSearchOnly && settings_.enableFileSearch) {
                const auto fileResults = takeoff::FileIndex::Instance().Search(query, 30);
                for (const auto& item : fileResults) {
                    AppEntry entry;
                    entry.name = item.name;
                    entry.path = item.path;
                    entry.normalizedName = Normalize(entry.name);
                    entry.category = item.isDirectory ? takeoff::AppCategory::Folder : takeoff::AppCategory::File;
                    const size_t newIdx = apps_.size();
                    apps_.push_back(std::move(entry));
                    ranked.push_back({newIdx, item.score});
                }
            }
            if (!appSearchOnly) {
                // A pinned file matching the query must not vanish just
                // because FileIndex's top 30 didn't include it (US-024).
                static const std::vector<std::wstring> kNoAliases;
                for (size_t p = 0; p < pins_.size(); ++p) {
                    if (pins_[p].isApp) continue;
                    const int score = takeoff::ScoreApp(pinnedNames_[p], kNoAliases, query, -1);
                    if (score < 0) continue;
                    bool present = false;
                    for (size_t k = baseAppsCount_; k < apps_.size() && !present; ++k) {
                        present = leanlauncher::pins::SamePath(apps_[k].path, pins_[p].path);
                    }
                    AppEntry entry;
                    if (present || !BuildPinnedFileEntry(p, entry)) continue;
                    const size_t newIdx = apps_.size();
                    apps_.push_back(std::move(entry));
                    ranked.push_back({newIdx, score});
                }
            }
            std::sort(ranked.begin(), ranked.end(), [this](const RankedResult& a, const RankedResult& b) {
                if (a.score != b.score) return a.score > b.score;
                const auto& appA = apps_[a.appIndex];
                const auto& appB = apps_[b.appIndex];
                const bool authA = (appA.category == takeoff::AppCategory::System ||
                                    appA.path.rfind(L"shell:AppsFolder", 0) == 0 ||
                                    (appA.path.size() >= 4 && _wcsicmp(appA.path.c_str() + appA.path.size() - 4, L".lnk") == 0));
                const bool authB = (appB.category == takeoff::AppCategory::System ||
                                    appB.path.rfind(L"shell:AppsFolder", 0) == 0 ||
                                    (appB.path.size() >= 4 && _wcsicmp(appB.path.c_str() + appB.path.size() - 4, L".lnk") == 0));
                if (authA != authB) return authA > authB;
                return appA.name < appB.name;
            });
            for (const auto& item : ranked) results_.push_back(item.appIndex);
            // Matching pins rise to the top in pin order; non-matching pins
            // simply aren't in results_. Command rows (calculator, capture,
            // web, vault search) are inserted above this afterwards.
            leanlauncher::pins::MovePinnedToFront(results_, [this](size_t index) { return PinRank(index); });
            topAppScore = ranked.empty() ? -1 : ranked.front().score;
        }
        if (!input_.text.empty()) {
            auto calc = takeoff::EvaluateExpression(input_.text);
            if (calc.has_value()) {
                AppEntry entry;
                entry.name = calc->formattedResult;
                entry.path = calc->rawResult;
                entry.normalizedName = Normalize(entry.name);
                entry.category = takeoff::AppCategory::Calculator;
                entry.parameters = calc->expression;
                entry.iconPath = L"calc.exe";
                const size_t calcIdx = apps_.size();
                apps_.push_back(std::move(entry));
                results_.insert(results_.begin(), calcIdx);
            } else if (!fileSearchOnly && !appSearchOnly) {
                if (settings_.enableUnitConverter) AddConversionRows(topAppScore);
                if (settings_.enableTimeZones) AddTimeZoneRow();
            }
        }
        std::wstring taskText;
        if (settings_.obsidianEnabled && settings_.taskAddEnabled &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.taskPrefix, taskText) &&
            !leanlauncher::obsidian::SplitCaptureLines(taskText).empty()) {
            AppEntry entry;
            entry.category = AppCategory::TaskAdd;
            entry.parameters = taskText;
            if (obsidianVaultPath_.empty()) {
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
            } else {
                entry.path = CaptureTargetPath(settings_.taskTargetNote);
                entry.name = settings_.taskPreviewPrefix + leanlauncher::obsidian::CapturePreviewText(taskText) + CaptureTargetSuffix(settings_.taskTargetNote);
                entry.iconPath = L"notepad.exe";
            }
            entry.normalizedName = Normalize(entry.name);
            const size_t taskIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Don't force the TaskAdd row above a strong app match already
            // leading results_ (e.g. "task manager" shouldn't bury the real
            // Task Manager app under an "add task" row - see review finding).
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), taskIdx);
        }
        std::wstring noteText;
        if (settings_.obsidianEnabled && settings_.noteAddEnabled &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.noteAddPrefix, noteText) &&
            !leanlauncher::obsidian::SplitCaptureLines(noteText).empty()) {
            AppEntry entry;
            entry.category = AppCategory::NoteAdd;
            entry.parameters = noteText;
            if (obsidianVaultPath_.empty()) {
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
            } else {
                entry.path = CaptureTargetPath(settings_.noteAddTargetNote);
                entry.name = settings_.noteAddPreviewPrefix + leanlauncher::obsidian::CapturePreviewText(noteText) + CaptureTargetSuffix(settings_.noteAddTargetNote);
                entry.iconPath = L"notepad.exe";
            }
            entry.normalizedName = Normalize(entry.name);
            const size_t noteAddIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), noteAddIdx);
        }
        std::wstring logText;
        if (settings_.obsidianEnabled && settings_.logEnabled &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.logPrefix, logText) &&
            !leanlauncher::obsidian::SplitCaptureLines(logText).empty()) {
            AppEntry entry;
            entry.category = AppCategory::LogAdd;
            entry.parameters = logText;
            if (obsidianVaultPath_.empty()) {
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
            } else {
                entry.path = CaptureTargetPath(settings_.logTargetNote);
                entry.name = settings_.logPreviewPrefix + leanlauncher::obsidian::CapturePreviewText(logText) + CaptureTargetSuffix(settings_.logTargetNote);
                entry.iconPath = L"notepad.exe";
            }
            entry.normalizedName = Normalize(entry.name);
            const size_t logIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), logIdx);
        }
        std::wstring webSearchText;
        if (settings_.enableWebSearch &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.webSearchPrefix, webSearchText)) {
            AppEntry entry;
            entry.category = AppCategory::WebSearch;
            entry.parameters = webSearchText;
            entry.name = L"Search " + settings_.webSearchEngineName + L" for “" + webSearchText + L"”";
            entry.iconPath = L"msedge.exe";
            entry.normalizedName = Normalize(entry.name);
            const size_t webSearchIdx = apps_.size();
            apps_.push_back(std::move(entry));
            // Same strong-app-match guard as TaskAdd above.
            constexpr int kStrongMatchThreshold = 9000;
            const size_t insertPos = (topAppScore >= kStrongMatchThreshold) ? 1 : 0;
            results_.insert(results_.begin() + std::min(insertPos, results_.size()), webSearchIdx);
        }
        std::wstring noteQuery;
        if (settings_.obsidianEnabled && settings_.vaultSearchEnabled &&
            leanlauncher::obsidian::TryParsePrefix(input_.text, settings_.vaultSearchPrefix, noteQuery)) {
            if (obsidianVaultPath_.empty()) {
                AppEntry entry;
                entry.category = AppCategory::NoteJump;
                entry.name = L"Set up your vault in Settings";
                entry.iconPath = L"notepad.exe";
                entry.normalizedName = Normalize(entry.name);
                const size_t idx = apps_.size();
                apps_.push_back(std::move(entry));
                results_.insert(results_.begin(), idx);
            } else if (noteQuery == L".") {
                // Quick open (US-026): exactly "<prefix> ." opens the note
                // chosen in Settings instead of searching. Never creates a
                // missing note - that case gets an inert row instead.
                const std::wstring target = leanlauncher::obsidian::SelectQuickOpenTarget(
                    settings_.quickOpenTarget, settings_.taskTargetNote, settings_.noteAddTargetNote,
                    settings_.logTargetNote);
                std::wstring noteRef = target;
                if (noteRef.empty()) {
                    int year = 0, month = 0, day = 0;
                    leanlauncher::obsidian::GetTodayYmd(year, month, day);
                    noteRef = leanlauncher::obsidian::TodayNoteRef(dailyNoteConfig_, year, month, day);
                }
                AppEntry entry;
                entry.category = AppCategory::NoteJump;
                entry.iconPath = L"notepad.exe";
                // One stat of a single known path, and only for this exact
                // query - not the per-keystroke vault walk NoteIndex exists
                // to avoid. The index itself can't answer this reliably: a
                // daily note created seconds ago may not be indexed yet.
                std::error_code existsEc;
                if (fs::exists(leanlauncher::obsidian::ResolveNoteAbsolutePath(obsidianVaultPath_, noteRef), existsEc)) {
                    entry.name = target.empty() ? std::wstring(L"Open today's daily note") : L"Open " + target;
                    entry.path = noteRef;
                    entry.parameters = fs::path(noteRef).filename().wstring();
                } else {
                    entry.name = target.empty() ? std::wstring(L"Today's daily note doesn't exist yet")
                                                : target + L" doesn't exist yet";
                    entry.inert = true;
                }
                entry.normalizedName = Normalize(entry.name);
                const size_t idx = apps_.size();
                apps_.push_back(std::move(entry));
                results_.insert(results_.begin(), idx);
            } else {
                const auto noteResults = leanlauncher::obsidian::NoteIndex::Instance().Search(noteQuery, 30);
                std::vector<size_t> noteIndices;
                noteIndices.reserve(noteResults.size());
                for (const auto& note : noteResults) {
                    AppEntry entry;
                    entry.category = AppCategory::NoteJump;
                    entry.name = note.folderDisplay.empty()
                        ? note.title
                        : note.title + L" - " + note.folderDisplay;
                    entry.path = note.relativeRef;
                    entry.parameters = note.title;
                    entry.iconPath = L"notepad.exe";
                    entry.normalizedName = Normalize(entry.name);
                    const size_t idx = apps_.size();
                    apps_.push_back(std::move(entry));
                    noteIndices.push_back(idx);
                }
                results_.insert(results_.begin(), noteIndices.begin(), noteIndices.end());
            }
        }
        if (settings_.enableTypedUrls && !fileSearchOnly && !appSearchOnly) AddTypedUrlRow();
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Command rows for the system commands prefix: table order when there's
    // no filter, best match first otherwise.
    void ShowCommandResults(const std::wstring& filter) {
        std::vector<RankedResult> ranked;
        for (size_t i = 0; i < apps_.size(); ++i) {
            if (apps_[i].category != takeoff::AppCategory::Command) continue;
            const int score = filter.empty() ? 0
                : leanlauncher::syscmd::ScoreCommand(apps_[i].normalizedName, apps_[i].aliases, filter);
            if (score >= 0) ranked.push_back({i, score});
        }
        if (filter.empty()) {
            const auto tableIndex = [this](size_t appIndex) {
                const auto command = leanlauncher::syscmd::CommandFromPath(apps_[appIndex].path);
                return command ? static_cast<int>(*command) : 0;
            };
            std::sort(ranked.begin(), ranked.end(), [&](const RankedResult& a, const RankedResult& b) {
                return tableIndex(a.appIndex) < tableIndex(b.appIndex);
            });
        } else {
            std::stable_sort(ranked.begin(), ranked.end(),
                [](const RankedResult& a, const RankedResult& b) { return a.score > b.score; });
        }
        for (const auto& r : ranked) results_.push_back(r.appIndex);
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // US-042: "Open URL" row for typed http(s):// / www. (first) or a bare
    // domain (below the top match, so a file like readme.md stays first).
    // US-047: conversion rows in the calculator's row style. A bare
    // "<number> <unit>" (several rows) doesn't push past a strong app match.
    void AddConversionRows(int topAppScore) {
        wchar_t decimal[8] = L".";
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, static_cast<int>(std::size(decimal)));
        const auto rows = leanlauncher::convert::EvaluateConversion(input_.text, decimal[0] ? decimal[0] : L'.');
        if (rows.empty() || (rows.size() > 1 && topAppScore >= 9000)) return;
        size_t insertAt = 0;
        for (const auto& row : rows) {
            AppEntry entry;
            entry.name = row.text;
            entry.path = row.valueText;       // Enter copies the value
            entry.parameters = row.source;    // shown as "5 km ="; "Copy calculation" gives "5 km = 3.107 mi"
            entry.note = row.note;
            entry.normalizedName = Normalize(entry.name);
            entry.category = takeoff::AppCategory::Calculator;
            entry.iconPath = L"calc.exe";
            results_.insert(results_.begin() + static_cast<std::ptrdiff_t>(insertAt++), apps_.size());
            apps_.push_back(std::move(entry));
        }
    }

    // US-048: one calculator-style row; Windows' zone list loads on the first
    // time query only.
    void AddTimeZoneRow() {
        SYSTEMTIME nowUtc{};
        GetSystemTime(&nowUtc);
        const auto row = leanlauncher::timezones::EvaluateTimeQuery(input_.text, nowUtc, timeZones_);
        if (!row) return;
        AppEntry entry;
        entry.name = row->text;
        entry.path = row->valueText;
        entry.parameters = row->source;
        entry.note = row->note;
        entry.normalizedName = Normalize(entry.name);
        entry.category = takeoff::AppCategory::Calculator;
        entry.iconPath = L"calc.exe";
        results_.insert(results_.begin(), apps_.size());
        apps_.push_back(std::move(entry));
    }

    void AddTypedUrlRow() {
        const std::wstring text = TrimmedQuery();
        const auto kind = leanlauncher::typed::ClassifyUrl(text);
        if (kind == leanlauncher::typed::UrlKind::None) return;
        AppEntry entry;
        entry.category = AppCategory::Url;
        entry.name = text;
        entry.normalizedName = Normalize(text);
        entry.path = leanlauncher::typed::NormalizeUrl(text);
        const size_t idx = apps_.size();
        apps_.push_back(std::move(entry));
        const size_t pos = leanlauncher::typed::UrlRowPosition(kind, results_.size());
        results_.insert(results_.begin() + static_cast<std::ptrdiff_t>((std::min)(pos, results_.size())), idx);
    }

    std::wstring TrimmedQuery() const {
        const size_t start = input_.text.find_first_not_of(L" \t");
        if (start == std::wstring::npos) return {};
        const size_t end = input_.text.find_last_not_of(L" \t");
        return input_.text.substr(start, end - start + 1);
    }

    // ---- US-044 settings export/import ---------------------------------------
    static std::wstring LocalAppDataFolder() {
        PWSTR path = nullptr;
        std::wstring folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
            folder = std::wstring(path) + L"\\LeanLauncher";
        }
        if (path) CoTaskMemFree(path);
        return folder;
    }

    static std::vector<std::wstring> ReadExclusionLines(const std::wstring& path) {
        std::vector<std::wstring> lines;
        std::ifstream file(path, std::ios::binary);
        if (!file) return lines;
        std::ostringstream ss;
        ss << file.rdbuf();
        std::wstring content = leanlauncher::obsidian::Utf8ToWide(ss.str());
        if (!content.empty() && content[0] == static_cast<wchar_t>(0xFEFF)) content.erase(0, 1);
        size_t pos = 0;
        while (pos <= content.size()) {
            const size_t nl = content.find(L'\n', pos);
            std::wstring line = content.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
            pos = nl == std::wstring::npos ? content.size() + 1 : nl + 1;
            const size_t start = line.find_first_not_of(L" \t\r");
            if (start == std::wstring::npos) continue;
            line = line.substr(start, line.find_last_not_of(L" \t\r") - start + 1);
            if (line[0] != L'#') lines.push_back(line);
        }
        return lines;
    }

    leanlauncher::settings_io::PortableExtras CurrentExtras() const {
        leanlauncher::settings_io::PortableExtras extras;
        extras.vaultPath = obsidianVaultPath_;
        for (const auto& pin : pins_) extras.pins.push_back(leanlauncher::pins::EncodePin(pin));
        extras.exclusions = ReadExclusionLines(takeoff::FileIndex::DefaultExclusionsPath());
        return extras;
    }

    static bool WriteUtf8File(const std::wstring& path, const std::string& content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << content;
        return static_cast<bool>(out);
    }

    // File dialogs are modal on this window: keep it from hiding when the
    // dialog takes focus, and keep the global hotkey from firing meanwhile.
    struct ModalDialogScope {
        LauncherWindow& self;
        explicit ModalDialogScope(LauncherWindow& owner) : self(owner) {
            self.modalDialogOpen_ = true;
            if (self.hotkeyRegistered_) {
                UnregisterHotKey(self.hwnd_, kHotkeyId);
                self.hotkeyRegistered_ = false;
            }
        }
        ~ModalDialogScope() {
            self.modalDialogOpen_ = false;
            self.RegisterShortcut();
            SetForegroundWindow(self.hwnd_);
        }
    };

    void ExportSettings() {
        ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            settingsStatus_ = L"Couldn't open the save dialog.";
            return;
        }
        const COMDLG_FILTERSPEC types[] = {{L"Lean Launcher settings (*.json)", L"*.json"}};
        dialog->SetFileTypes(1, types);
        dialog->SetDefaultExtension(L"json");
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t name[64];
        swprintf(name, std::size(name), L"LeanLauncher-settings-%04u-%02u-%02u.json", now.wYear, now.wMonth, now.wDay);
        dialog->SetFileName(name);
        PWSTR documents = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(documents, nullptr, IID_PPV_ARGS(&folder)))) {
                dialog->SetFolder(folder.Get());
            }
        }
        if (documents) CoTaskMemFree(documents);
        enum : DWORD { kVaultBox = 1, kPinsBox = 2, kExclusionsBox = 3 };
        ComPtr<IFileDialogCustomize> customize;
        if (SUCCEEDED(dialog.As(&customize))) {
            customize->AddCheckButton(kVaultBox, L"Include vault path", FALSE);
            customize->AddCheckButton(kPinsBox, L"Include pins", FALSE);
            customize->AddCheckButton(kExclusionsBox, L"Include file-search exclusions", FALSE);
        }
        HRESULT shown = E_FAIL;
        {
            ModalDialogScope scope(*this);
            shown = dialog->Show(hwnd_);
        }
        if (FAILED(shown)) return;  // cancelled
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            settingsStatus_ = L"Couldn't save the settings file.";
            return;
        }
        const std::wstring target(path);
        CoTaskMemFree(path);
        leanlauncher::settings_io::ExportOptions options;
        if (customize) {
            BOOL checked = FALSE;
            options.includeVaultPath = SUCCEEDED(customize->GetCheckButtonState(kVaultBox, &checked)) && checked;
            options.includePins = SUCCEEDED(customize->GetCheckButtonState(kPinsBox, &checked)) && checked;
            options.includeExclusions = SUCCEEDED(customize->GetCheckButtonState(kExclusionsBox, &checked)) && checked;
        }
        const std::string json = leanlauncher::settings_io::ExportJson(settings_, CurrentExtras(), options,
            takeoff::kAppVersion);
        settingsStatus_ = WriteUtf8File(target, json) ? L"Settings exported." : L"Couldn't save the settings file.";
    }

    void ImportSettings() {
        namespace io = leanlauncher::settings_io;
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            settingsStatus_ = L"Couldn't open the file dialog.";
            return;
        }
        const COMDLG_FILTERSPEC types[] = {{L"Lean Launcher settings (*.json)", L"*.json"}};
        dialog->SetFileTypes(1, types);
        HRESULT shown = E_FAIL;
        {
            ModalDialogScope scope(*this);
            shown = dialog->Show(hwnd_);
        }
        if (FAILED(shown)) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        const std::wstring source(path);
        CoTaskMemFree(path);

        std::error_code ec;
        const auto size = std::filesystem::file_size(source, ec);
        if (ec || size > io::kMaxImportBytes) {
            settingsStatus_ = ec ? L"Couldn't read that file." : L"That file is too large to be a settings file.";
            return;
        }
        std::ifstream file(source, std::ios::binary);
        std::ostringstream ss;
        ss << file.rdbuf();
        io::ImportResult result = io::ParseImport(ss.str(), settings_);
        if (!result.ok) {
            settingsStatus_ = result.error;
            return;
        }

        // Machine-specific extras only apply where they fit this PC.
        std::optional<std::wstring> vault;
        if (result.vaultPath && std::filesystem::is_directory(*result.vaultPath + L"\\.obsidian", ec)) {
            if (_wcsicmp(result.vaultPath->c_str(), obsidianVaultPath_.c_str()) != 0) vault = result.vaultPath;
        } else if (result.vaultPath) {
            result.skipped.push_back(L"Vault path: " + *result.vaultPath + L" isn't an Obsidian vault on this PC");
        }
        std::vector<leanlauncher::pins::Pin> pins;
        for (const auto& encoded : result.pins) {
            leanlauncher::pins::Pin pin;
            if (!leanlauncher::pins::DecodePin(encoded, pin)) continue;
            if (!std::filesystem::exists(pin.path, ec)) {
                result.skipped.push_back(L"Pin: " + pin.path + L" doesn't exist on this PC");
                continue;
            }
            if (leanlauncher::pins::FindPin(pins, pin.path) < 0 && pins.size() < leanlauncher::pins::kMaxPins) {
                pins.push_back(std::move(pin));
            }
        }
        const std::wstring exclusionsPath = takeoff::FileIndex::DefaultExclusionsPath();
        const std::vector<std::wstring> currentExclusions = ReadExclusionLines(exclusionsPath);
        std::vector<std::wstring> newExclusions;
        for (const auto& line : io::MergeExclusions(currentExclusions, result.exclusions)) {
            if (std::find(currentExclusions.begin(), currentExclusions.end(), line) == currentExclusions.end()) {
                newExclusions.push_back(line);
            }
        }

        const std::wstring backupFolder = LocalAppDataFolder();
        const std::wstring backupPath = backupFolder + L"\\settings-before-import.json";
        std::wstring summary = std::to_wstring(result.changed) +
            (result.changed == 1 ? L" setting will change" : L" settings will change");
        if (vault) summary += L", the vault becomes " + *vault;
        if (result.hasPins) summary += L", " + std::to_wstring(pins.size()) + L" pin(s) will replace your pins";
        if (!newExclusions.empty()) summary += L", " + std::to_wstring(newExclusions.size()) + L" exclusion(s) will be added";
        summary += L".";
        if (result.newerSchema) summary += L"\n\nThis file comes from a newer Lean Launcher. Settings it knows are imported.";
        if (!result.skipped.empty()) {
            summary += L"\n\n" + std::to_wstring(result.skipped.size()) + L" skipped:";
            for (size_t i = 0; i < result.skipped.size() && i < 12; ++i) summary += L"\n- " + result.skipped[i];
            if (result.skipped.size() > 12) summary += L"\n- ...";
        }
        summary += L"\n\nYour current settings are saved first to\n" + backupPath + L"\nImport that file to undo.";
        int answer = IDCANCEL;
        {
            ModalDialogScope scope(*this);
            answer = MessageBoxW(hwnd_, summary.c_str(), L"Import settings", MB_OKCANCEL | MB_ICONQUESTION);
        }
        if (answer != IDOK) return;

        std::filesystem::create_directories(backupFolder, ec);
        const std::string backup = io::ExportJson(settings_, CurrentExtras(), {true, true, true}, takeoff::kAppVersion);
        if (backupFolder.empty() || !WriteUtf8File(backupPath, backup)) {
            settingsStatus_ = L"Couldn't save a backup of your current settings, so nothing was imported.";
            return;
        }

        const quicklaunch::Settings before = settings_;
        const std::wstring vaultBefore = obsidianVaultPath_;
        settings_ = result.settings;
        if (vault) obsidianVaultPath_ = *vault;
        if (result.hasPins) {
            pins_ = std::move(pins);
            SavePins();
            RefreshPins();
        }
        if (!newExclusions.empty() && takeoff::FileIndex::EnsureExclusionsFileWithHeader(exclusionsPath)) {
            std::ofstream out(exclusionsPath, std::ios::binary | std::ios::app);
            for (const auto& line : newExclusions) out << "\r\n" << leanlauncher::obsidian::WideToUtf8(line);
        }
        SaveSettings();
        ApplyRuntimeSettings(before, vaultBefore);
        settingsStatus_ = L"Imported " + std::to_wstring(result.changed) + L" setting(s)" +
            (result.skipped.empty() ? L"." : L", skipped " + std::to_wstring(result.skipped.size()) + L".");
    }

    // After settings change wholesale (import, reset): bring every running
    // part in line - hotkeys, tray icon, file index (NFR-018), note index,
    // daily-note config, and the per-feature caches.
    void ApplyRuntimeSettings(const quicklaunch::Settings& before, const std::wstring& vaultBefore) {
        RegisterShortcut();
        UpdateTrayIcon();
        dailyNoteConfig_ = leanlauncher::obsidian::ResolveDailyNoteConfig(
            obsidianVaultPath_, settings_.dailyNoteFolderOverride, settings_.dailyNoteFormatOverride);
        if constexpr (!kUiTest) {
            if (settings_.enableFileSearch != before.enableFileSearch) {
                if (settings_.enableFileSearch) FileIndex::Instance().Start(hwnd_);
                else FileIndex::Instance().StopAndRelease();
            }
            auto& notes = leanlauncher::obsidian::NoteIndex::Instance();
            const bool wantNotes = settings_.obsidianEnabled && !obsidianVaultPath_.empty();
            if (!wantNotes) notes.Stop();
            else if (!before.obsidianEnabled || vaultBefore.empty()) notes.Start(obsidianVaultPath_, hwnd_);
            else if (_wcsicmp(vaultBefore.c_str(), obsidianVaultPath_.c_str()) != 0) notes.Restart(obsidianVaultPath_, hwnd_);
        }
        if (!settings_.enablePathCompletion) ReleasePathCompletion();
        if (!settings_.enableSystemCommands) CancelCommandConfirm();
        if (!settings_.enableTimeZones) timeZones_.Clear();
        if (!settings_.enablePomodoro) EndPomodoro(leanlauncher::pomodoro::EndReason::Stopped);
        if (!settings_.enablePreview) ClosePreview(true);
        if (settings_.enableSnippets != before.enableSnippets || settings_.snippetsPath != before.snippetsPath) {
            ApplySnippetsState();
        }
        UpdateResults();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ---- US-049 Pomodoro ------------------------------------------------------
    static unsigned long long NowTicks() {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    int PomodoroMinutes(const std::wstring& text, int fallback) const {
        return leanlauncher::settings_io::detail::IsMinutes(text) ? std::stoi(text) : fallback;
    }

    void AddPomodoroRow(std::wstring name, std::wstring path, std::wstring label, bool inert = false) {
        AppEntry entry;
        entry.category = inert ? AppCategory::Info : AppCategory::Pomodoro;
        entry.name = std::move(name);
        entry.path = std::move(path);
        entry.parameters = std::move(label);
        entry.inert = inert;
        results_.push_back(apps_.size());
        apps_.push_back(std::move(entry));
    }

    // Rows for "pomo ..." - nothing else is shown while the prefix is typed.
    bool ShowPomodoroResults() {
        namespace pm = leanlauncher::pomodoro;
        const int focus = PomodoroMinutes(settings_.pomodoroFocusMinutes, 25);
        const int rest = PomodoroMinutes(settings_.pomodoroBreakMinutes, 5);
        const pm::Command command = pm::ParseCommand(input_.text, settings_.pomodoroPrefix, focus, rest);
        using K = pm::Command::Kind;
        if (command.kind == K::None) return false;
        const bool running = pomodoro_.has_value();
        const long long left = running ? pm::RemainingSeconds(pomodoro_->endTicks, NowTicks()) : 0;
        const std::wstring runningText = running
            ? std::wstring(pm::kTomato) + L" " + pm::FormatClock(left) + L" left" +
                (pomodoro_->kind == pm::Kind::Break ? L" - break" : (pomodoro_->label.empty() ? L"" : L" - " + pomodoro_->label))
            : std::wstring();
        const auto startRow = [&](bool isBreak, int minutes, const std::wstring& label) {
            std::wstring name = isBreak ? L"Start " + std::to_wstring(minutes) + L"-min break"
                                        : L"Start " + std::to_wstring(minutes) + L"-min focus" + (label.empty() ? L"" : L": " + label);
            const std::wstring path = std::wstring(isBreak ? L"pomo:break:" : L"pomo:focus:") + std::to_wstring(minutes);
            if (running && pomodoroReplacePending_ && pomodoroReplacePath_ == path + L"|" + label &&
                GetTickCount64() - pomodoroReplaceAt_ <= 5000) {
                name = L"Press Enter again to replace the running timer";
            } else if (running) {
                name = L"Replace running timer: " + name;
            }
            AddPomodoroRow(std::move(name), path, label);
        };
        switch (command.kind) {
        case K::Menu:
            if (running) AddPomodoroRow(runningText, L"pomo:stop", L"");
            else startRow(false, focus, L"");
            break;
        case K::StartFocus: startRow(false, command.minutes, command.label); break;
        case K::StartBreak: startRow(true, command.minutes, L""); break;
        case K::Stop:
            if (running) AddPomodoroRow(L"Stop timer (" + pm::FormatClock(left) + L" left)", L"pomo:stop", L"");
            else AddPomodoroRow(L"No timer is running", L"", L"", true);
            break;
        case K::Invalid:
            AddPomodoroRow(L"Use 1-180 minutes, like " + settings_.pomodoroPrefix + L" 25 write intro", L"", L"", true);
            break;
        case K::None: break;
        }
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    // ---- US-050 snippets ------------------------------------------------------
    std::wstring SnippetsFilePath() const {
        const std::wstring fallback = L"%APPDATA%\\LeanLauncher\\snippets.yml";
        const auto expand = [](const std::wstring& raw) {
            wchar_t expanded[MAX_PATH * 2]{};
            const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
            return (n > 0 && n <= std::size(expanded)) ? std::wstring(expanded) : raw;
        };
        if (!settings_.snippetsPath.empty()) {
            std::wstring path = expand(settings_.snippetsPath);
            // Never use an expanded path that fails the same validation as the Settings field.
            if (!leanlauncher::snippets::SnippetsPathProblem(path)) return path;
        }
        return expand(fallback);
    }

    // A commented starter file, so the feature is discoverable on first enable.
    static void WriteDefaultSnippetsFile(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        static const char kStarter[] =
               "# Lean Launcher snippets. Type a trigger in any app and it is replaced by the text.\n"
               "# Same shape as Espanso: matches, trigger, replace, label. Variables are not supported yet.\n"
               "# Edit this file in any editor; it reloads the next time the launcher opens.\n"
               "matches:\n"
               "  - trigger: \":sig\"\n"
               "    label: \"Email signature\"\n"
               "    replace: \"Best regards,\\nYour Name\"\n"
               "  - trigger: \":addr\"\n"
               "    label: \"Address\"\n"
               "    replace: \"Street 1, 12345 City\"\n";
        // CREATE_NEW: an existing file is never truncated, whatever the caller checked before.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, kStarter, static_cast<DWORD>(sizeof(kStarter) - 1), &written, nullptr);
        CloseHandle(file);
    }

    // Returns true when the index was replaced. A transient read failure keeps
    // the previous index and leaves the write time alone so the next Show retries.
    bool ReloadSnippets() {
        namespace sn = leanlauncher::snippets;
        const std::filesystem::path path = SnippetsFilePath();
        std::error_code ec;
        const sn::FileState state = sn::ClassifyPath(path);
        if (state == sn::FileState::Error) {  // status failed (for example a network drive hiccup): touch nothing
            snippetWarnings_ = {L"Couldn't reach the snippets file"};
            if (!snippetIndex_) snippetIndex_ = std::make_shared<const sn::SnippetIndex>(std::vector<sn::Snippet>{});
            return false;
        }
        if (state == sn::FileState::Missing) {
            WriteDefaultSnippetsFile(path);
            if (sn::ClassifyPath(path) != sn::FileState::Present) {
                snippetWarnings_ = {L"Couldn't create the snippets file"};
                if (!snippetIndex_) snippetIndex_ = std::make_shared<const sn::SnippetIndex>(std::vector<sn::Snippet>{});
                return false;
            }
        }
        // The time is read before the content, so an edit landing in between is loaded next time.
        const auto stamp = std::filesystem::last_write_time(path, ec);
        const bool stampOk = !ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            snippetWarnings_ = {L"Couldn't read the snippets file"};
            return false;
        }
        std::string bytes;
        if (size > sn::kMaxFileBytes) {
            snippetWarnings_ = {L"The snippets file is larger than 1 MB, so it was not loaded"};
            snippetIndex_ = std::make_shared<const sn::SnippetIndex>(std::vector<sn::Snippet>{});
            if (stampOk) snippetsWriteTime_ = stamp;
            return true;
        }
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                snippetWarnings_ = {L"Couldn't read the snippets file"};
                return false;
            }
            bytes.resize(sn::kMaxFileBytes + 1);
            in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (in.bad()) {
                snippetWarnings_ = {L"Couldn't read the snippets file"};
                return false;
            }
            bytes.resize(static_cast<size_t>(in.gcount()));
        }
        if (stampOk) snippetsWriteTime_ = stamp;
        sn::ParseResult parsed = sn::ParseSnippets(bytes);
        snippetWarnings_ = std::move(parsed.warnings);
        snippetIndex_ = std::make_shared<const sn::SnippetIndex>(std::move(parsed.snippets));
        if (expander_.Running()) expander_.SetIndex(snippetIndex_);
        return true;
    }

    // Brings the hook in line with the setting: on = (re)load the file and start
    // the keyboard listener when there is at least one snippet; off = stop it
    // and free everything (NFR-018).
    void ApplySnippetsState() {
        if (!settings_.enableSnippets) {
            expander_.Stop();
            snippetIndex_.reset();
            snippetWarnings_.clear();
            return;
        }
        const bool replaced = ReloadSnippets();
        if constexpr (!kUiTest) {  // the UI test build never installs a global hook
            if (snippetIndex_ && !snippetIndex_->Empty()) {
                if (replaced || !expander_.Running()) {
                    if (!expander_.Start(hwnd_, snippetIndex_)) {
                        // The toggle reads off, and the saved state matches.
                        settings_.enableSnippets = false;
                        SaveSettings();
                        snippetIndex_.reset();
                        snippetWarnings_.clear();
                        settingsStatus_ = L"Couldn't start the keyboard listener, so snippets were turned off.";
                    }
                }
            } else if (replaced) {
                expander_.Stop();
            }
        }
    }

    // Adds the text matches of the user's Espanso folder to the snippets file.
    // Order: read everything and merge in memory, check the limits, ask, back up
    // (never overwriting an earlier backup), then replace the file via a temp
    // file so a failed write cannot leave a truncated snippets file.
    void ImportEspansoSnippets() {
        namespace sn = leanlauncher::snippets;
        constexpr size_t kMaxImportFiles = 200;
        wchar_t folder[MAX_PATH * 2]{};
        if (!ExpandEnvironmentStringsW(L"%APPDATA%\\espanso\\match", folder, static_cast<DWORD>(std::size(folder)))) {
            settingsStatus_ = L"Couldn't find the Espanso folder.";
            return;
        }
        std::vector<sn::Snippet> incoming;
        size_t skipped = 0, files = 0, examined = 0, incomingBytes = 0;
        bool limitReached = false;
        std::error_code ec;
        const sn::FileState folderState = sn::ClassifyPath(folder);
        if (folderState == sn::FileState::Missing) {
            settingsStatus_ = L"No Espanso match files found in %APPDATA%\\espanso\\match.";
            return;
        }
        if (folderState == sn::FileState::Error) {
            settingsStatus_ = L"Couldn't read the whole Espanso folder; nothing changed.";
            return;
        }
        std::filesystem::directory_iterator it(folder, ec);
        for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::wstring ext = it->path().extension().wstring();
            if (_wcsicmp(ext.c_str(), L".yml") != 0 && _wcsicmp(ext.c_str(), L".yaml") != 0) continue;
            if (examined >= kMaxImportFiles || limitReached) { limitReached = true; ++skipped; continue; }
            ++examined;
            const auto bytesOnDisk = it->file_size(fec);
            if (fec || bytesOnDisk > sn::kMaxFileBytes) { ++skipped; continue; }
            if (incomingBytes + bytesOnDisk > sn::kMaxFileBytes) { limitReached = true; ++skipped; continue; }
            std::string bytes(static_cast<size_t>(bytesOnDisk), '\0');
            {
                std::ifstream in(it->path(), std::ios::binary);
                if (in) in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!in || in.gcount() != static_cast<std::streamsize>(bytes.size())) { ++skipped; continue; }
            }
            incomingBytes += bytes.size();
            sn::ParseResult parsed = sn::ParseSnippets(bytes);
            skipped += parsed.warnings.size();
            for (auto& snippet : parsed.snippets) {
                if (sn::LooksLikeEspansoVariable(snippet.replace)) ++skipped;
                else incoming.push_back(std::move(snippet));
            }
            ++files;
        }
        if (ec) {
            settingsStatus_ = L"Couldn't read the whole Espanso folder; nothing changed.";
            return;
        }
        if (files == 0) {
            settingsStatus_ = L"No Espanso match files found in %APPDATA%\\espanso\\match.";
            return;
        }

        // Existing snippets win. Parse the current file (not the running index, which is empty when off).
        // Nothing is dropped silently: entries the parser cannot keep are counted and shown in the prompt.
        const std::filesystem::path file = SnippetsFilePath();
        std::vector<sn::Snippet> current;
        size_t unreadable = 0;
        const sn::FileState fileState = sn::ClassifyPath(file);
        if (fileState == sn::FileState::Error) {
            settingsStatus_ = L"Your snippets file is too large or unreadable; nothing changed.";
            return;
        }
        const bool hadFile = (fileState == sn::FileState::Present);
        if (hadFile) {
            const auto size = std::filesystem::file_size(file, ec);
            if (ec || size > sn::kMaxFileBytes) {
                settingsStatus_ = L"Your snippets file is too large or unreadable; nothing changed.";
                return;
            }
            std::ifstream in(file, std::ios::binary);
            std::string bytes(static_cast<size_t>(size), '\0');
            if (in) in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!in || in.bad() || in.gcount() != static_cast<std::streamsize>(bytes.size())) {
                settingsStatus_ = L"Couldn't read your snippets file; nothing changed.";
                return;
            }
            sn::ParseResult parsed = sn::ParseSnippets(bytes);
            unreadable = parsed.warnings.size();
            current = std::move(parsed.snippets);
        }
        const sn::MergeResult merged = sn::MergeSnippets(std::move(current), incoming);
        const std::string yaml = sn::SerializeSnippets(merged.merged);
        if (merged.overflow > 0 || !sn::ImportWithinLimits(yaml.size(), merged.merged.size())) {
            // The size shown is that of the capped file that would be written, not of the full set.
            settingsStatus_ = L"The import would exceed the snippet limits (" +
                              std::to_wstring(merged.merged.size() + merged.overflow) +
                              L" snippets, " + std::to_wstring((yaml.size() + 1023) / 1024) + L" KB); nothing changed.";
            return;
        }

        std::filesystem::path backup;
        if (hadFile) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            for (int attempt = 1; attempt <= 50; ++attempt) {
                const std::filesystem::path candidate = file.parent_path() / sn::BackupName(
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, attempt);
                if (!std::filesystem::exists(candidate, ec)) { backup = candidate; break; }
            }
            if (backup.empty()) {
                settingsStatus_ = L"Could not save a backup; nothing changed.";
                return;
            }
        }
        const std::wstring prompt = sn::EspansoImportPrompt(
            merged.added, merged.duplicates, skipped, unreadable,
            hadFile ? backup.filename().wstring() : std::wstring(), limitReached);
        int answer = IDNO;
        {
            ModalDialogScope scope(*this);
            answer = MessageBoxW(hwnd_, prompt.c_str(), L"Import from Espanso", MB_YESNO | MB_ICONQUESTION);
        }
        if (answer != IDYES) {
            settingsStatus_ = L"Import cancelled.";
            return;
        }

        std::filesystem::create_directories(file.parent_path(), ec);
        if (hadFile) {
            std::error_code copyEc;
            std::filesystem::copy_file(file, backup, std::filesystem::copy_options::none, copyEc);
            if (copyEc) {
                settingsStatus_ = L"Could not save a backup; nothing changed.";
                return;
            }
        }
        const std::filesystem::path temp = file.parent_path() / L"snippets-import.tmp";
        bool written = false;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));
                out.flush();
                out.close();
                written = !out.fail();
            }
        }
        std::error_code renameEc;
        if (written) std::filesystem::rename(temp, file, renameEc);
        if (!written || renameEc) {
            std::error_code removeEc;
            std::filesystem::remove(temp, removeEc);
            settingsStatus_ = L"Couldn't write the snippets file; nothing changed.";
            return;
        }
        ApplySnippetsState();
        // ApplySnippetsState sets its own message when the keyboard listener could not start; keep it.
        if (settings_.enableSnippets || settingsStatus_.empty()) {
            settingsStatus_ = L"Imported " + sn::CountNoun(merged.added, L"snippet", L"snippets") + L" (" +
                              std::to_wstring(merged.duplicates) + L" already existed, " +
                              sn::CountNoun(skipped, L"entry", L"entries") + L" skipped).";
        }
    }

    // Reload when the file changed since it was last read (checked on Show, no watcher).
    void ReloadSnippetsIfChanged() {
        if (!settings_.enableSnippets) return;
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(SnippetsFilePath(), ec);
        if (!ec && stamp != snippetsWriteTime_) ApplySnippetsState();
    }

    // Rows for ", ..." (the snippets prefix): label plus trigger, best match first.
    void ShowSnippetResults(const std::wstring& normalizedFilter) {
        namespace sn = leanlauncher::snippets;
        if (!snippetIndex_) return;
        const auto& all = snippetIndex_->All();
        for (size_t index : sn::SearchSnippets(all, normalizedFilter)) {
            const sn::Snippet& snippet = all[index];
            AppEntry entry;
            entry.category = AppCategory::Snippet;
            entry.name = snippet.label.empty() ? snippet.trigger : snippet.label + L"  " + snippet.trigger;
            entry.path = sn::PathForIndex(index);
            results_.push_back(apps_.size());
            apps_.push_back(std::move(entry));
        }
        if (results_.empty()) {
            AppEntry entry;
            entry.category = AppCategory::Info;
            entry.name = all.empty() ? L"No snippets yet - edit your snippets file in Settings" : L"No matching snippet";
            entry.inert = true;
            results_.push_back(apps_.size());
            apps_.push_back(std::move(entry));
        }
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RunSnippetRow(const AppEntry& app) {
        namespace sn = leanlauncher::snippets;
        size_t index = 0;
        if (!settings_.enableSnippets || !snippetIndex_ || !sn::IndexFromPath(app.path, index) ||
            index >= snippetIndex_->Size()) {
            return;
        }
        std::wstring text = snippetIndex_->All()[index].replace;
        const HWND target = snippetTarget_;
        snippetTarget_ = nullptr;  // one use only, so it can never go stale
        if (target && IsWindow(target)) {
            Hide();
            SetForegroundWindow(target);
            sn::Expander::InsertIntoWindowAsync(hwnd_, target, std::move(text));
        } else {
            // Nowhere to paste: leave the text on the clipboard and the launcher open.
            CopyText(text);
            status_ = L"Copied - paste with Ctrl+V";
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void RunPomodoroRow(const AppEntry& app) {
        namespace pm = leanlauncher::pomodoro;
        if (app.path == L"pomo:stop") {
            EndPomodoro(pm::EndReason::Stopped);
            Hide();
            return;
        }
        const bool isBreak = app.path.rfind(L"pomo:break:", 0) == 0;
        const bool isFocus = app.path.rfind(L"pomo:focus:", 0) == 0;
        if (!isBreak && !isFocus) return;
        const int minutes = std::stoi(app.path.substr(11));
        if (pomodoro_) {
            // Replacing a running timer needs a second Enter within 5 s.
            const std::wstring key = app.path + L"|" + app.parameters;
            if (!(pomodoroReplacePending_ && pomodoroReplacePath_ == key && GetTickCount64() - pomodoroReplaceAt_ <= 5000)) {
                pomodoroReplacePending_ = true;
                pomodoroReplacePath_ = key;
                pomodoroReplaceAt_ = GetTickCount64();
                UpdateResults();
                return;
            }
            EndPomodoro(pm::EndReason::Stopped);
        }
        StartPomodoro(isBreak ? pm::Kind::Break : pm::Kind::Focus, minutes, app.parameters);
        Hide();
    }

    void StartPomodoro(leanlauncher::pomodoro::Kind kind, int minutes, const std::wstring& label) {
        pomodoroReplacePending_ = false;
        pomodoroBreakOffer_ = false;
        pomodoro_ = leanlauncher::pomodoro::State{kind, NowTicks() + static_cast<unsigned long long>(minutes) * 600000000ULL,
            minutes, label};
        SavePomodoroState();
        SchedulePomodoroTimers();
        UpdateTrayIcon();
    }

    // The coarse tick refreshes the tooltip and footer; in the last minute a
    // one-shot fires at the exact end. Computed from the stored end time, so
    // sleep and clock changes can't drift it.
    void SchedulePomodoroTimers() {
        KillTimer(hwnd_, kPomodoroEndTimer);
        if (!pomodoro_) {
            KillTimer(hwnd_, kPomodoroTickTimer);
            return;
        }
        const long long left = leanlauncher::pomodoro::RemainingSeconds(pomodoro_->endTicks, NowTicks());
        if (left <= 0) {
            CheckPomodoro(false);
            return;
        }
        SetTimer(hwnd_, kPomodoroTickTimer, 30000, nullptr);
        if (left <= 60) SetTimer(hwnd_, kPomodoroEndTimer, static_cast<UINT>(left * 1000), nullptr);
    }

    void CheckPomodoro(bool resumedFromSleep) {
        if (!pomodoro_) return;
        const long long left = leanlauncher::pomodoro::RemainingSeconds(pomodoro_->endTicks, NowTicks());
        if (left > 0) {
            SchedulePomodoroTimers();
            UpdateTrayIcon();
            if (IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        EndPomodoro(resumedFromSleep ? leanlauncher::pomodoro::EndReason::EndedWhileAsleep
                                     : leanlauncher::pomodoro::EndReason::Finished);
    }

    void EndPomodoro(leanlauncher::pomodoro::EndReason reason) {
        namespace pm = leanlauncher::pomodoro;
        if (!pomodoro_) return;
        const pm::State ended = *pomodoro_;
        pomodoro_.reset();
        pomodoroReplacePending_ = false;
        KillTimer(hwnd_, kPomodoroTickTimer);
        KillTimer(hwnd_, kPomodoroEndTimer);
        SavePomodoroState();
        UpdateTrayIcon();
        if (reason == pm::EndReason::Stopped) return;
        std::wstring title, text;
        if (ended.kind == pm::Kind::Break) {
            title = L"Break over";
            text = L"Time to get back to it.";
        } else if (reason == pm::EndReason::EndedWhileAsleep) {
            title = std::wstring(pm::kTomato) + L" Focus ended while your PC was asleep";
            text = ended.label.empty() ? L"Not logged." : ended.label + L" - not logged.";
        } else if (reason == pm::EndReason::EndedWhileClosed) {
            title = std::wstring(pm::kTomato) + L" Pomodoro ended while Lean Launcher wasn't running";
            text = L"Not logged.";
        } else {
            title = std::wstring(pm::kTomato) + L" Focus done" + (ended.label.empty() ? L"" : L": " + ended.label);
            text = L"Click to start a " + std::to_wstring(PomodoroMinutes(settings_.pomodoroBreakMinutes, 5)) + L"-min break";
            pomodoroBreakOffer_ = true;
            if (pm::ShouldLog(ended.kind, reason, settings_.pomodoroLog) && settings_.obsidianEnabled &&
                !obsidianVaultPath_.empty()) {
                // Only into a note that already exists: the timer never creates
                // a daily note and never starts Obsidian.
                if (!pm::AppendToExistingNote(CaptureTargetPath(settings_.logTargetNote),
                        pm::LogText(ended.minutes, ended.label), settings_.logHeading)) {
                    text += L"\n(not logged: today's note doesn't exist yet)";
                }
            }
        }
        ShowBalloon(title, text);
    }

    void ShowBalloon(const std::wstring& title, const std::wstring& text) {
        if constexpr (kUiTest) return;
        if (!trayIconAdded_) {
            balloonTempIcon_ = true;  // tray icon is off: show one just for the balloon
            AddTrayIcon();
        }
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        data.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void SavePomodoroState() {
        if constexpr (kUiTest) return;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                nullptr) != ERROR_SUCCESS) return;
        if (pomodoro_) {
            const std::wstring value = leanlauncher::pomodoro::EncodeState(*pomodoro_);
            RegSetValueExW(key, L"PomodoroState", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(key, L"PomodoroState");
        }
        RegCloseKey(key);
    }

    // A timer running when Lean Launcher closed (e.g. Restart to update) carries on.
    void RestorePomodoro() {
        if constexpr (kUiTest) return;
        if (!settings_.enablePomodoro) return;
        wchar_t buffer[512]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRegistryPath, L"PomodoroState", RRF_RT_REG_SZ, nullptr, buffer,
                &size) != ERROR_SUCCESS) return;
        pomodoro_ = leanlauncher::pomodoro::DecodeState(buffer);
        if (!pomodoro_) {
            SavePomodoroState();  // drop a damaged value
            return;
        }
        if (leanlauncher::pomodoro::RemainingSeconds(pomodoro_->endTicks, NowTicks()) <= 0) {
            EndPomodoro(leanlauncher::pomodoro::EndReason::EndedWhileClosed);
            return;
        }
        SchedulePomodoroTimers();
        UpdateTrayIcon();
    }

    // ---- US-043 path completion -------------------------------------------
    struct PathListing {
        unsigned generation = 0;
        std::wstring folder;
        std::vector<leanlauncher::typed::PathEntry> entries;
        DWORD error = 0;
        bool truncated = false;
    };

    // Runs on a short-lived background thread: FindFirstFileEx can't be
    // cancelled, so a stuck network lookup must never hold up the next one.
    static PathListing ListFolder(const std::wstring& folder) {
        PathListing listing;
        listing.folder = folder;
        std::wstring pattern = folder;
        if (pattern.size() >= MAX_PATH - 12) {
            pattern = (pattern.rfind(L"\\\\", 0) == 0) ? L"\\\\?\\UNC\\" + pattern.substr(2)
                                                      : L"\\\\?\\" + pattern;
        }
        pattern += L"*";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            listing.error = GetLastError();
            return listing;
        }
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            if (listing.entries.size() >= leanlauncher::typed::kMaxPathEntries) {
                listing.truncated = true;
                break;
            }
            listing.entries.push_back({data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0});
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return listing;
    }

    static bool IsNetworkFolder(const std::wstring& folder) {
        if (folder.rfind(L"\\\\", 0) == 0) return true;
        if (folder.size() >= 3 && folder[1] == L':') {
            return GetDriveTypeW(folder.substr(0, 3).c_str()) == DRIVE_REMOTE;
        }
        return false;
    }

    // "\\nas" for "\\nas\share\", "Z:" for a mapped drive.
    static std::wstring NetworkHostLabel(const std::wstring& folder) {
        if (folder.rfind(L"\\\\", 0) == 0) {
            const size_t end = folder.find(L'\\', 2);
            return folder.substr(0, end);
        }
        return folder.substr(0, 2);
    }

    std::wstring ExpandTypedPath(const std::wstring& typed) const {
        std::wstring expanded = typed;
        if (typed.find(L'%') != std::wstring::npos) {
            wchar_t buffer[4096];
            const DWORD len = ExpandEnvironmentStringsW(typed.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
            if (len > 0 && len <= std::size(buffer)) expanded.assign(buffer);
        }
        std::wstring profile;
        if (expanded.rfind(L"~", 0) == 0) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &path))) profile = path;
            if (path) CoTaskMemFree(path);
        }
        return leanlauncher::typed::ExpandHome(expanded, profile);
    }

    void RequestPathListing(const std::wstring& folder) {
        if (_wcsicmp(folder.c_str(), pathRequestedFolder_.c_str()) == 0) return;  // already in flight or cached
        if (pathThreadsRunning_ >= 4) return;  // several lookups stuck; the next keystroke retries
        ++pathGeneration_;
        pathRequestedFolder_ = folder;
        pathListingReady_ = false;
        pathTimedOut_ = false;
        pathEntries_.clear();
        if (IsNetworkFolder(folder)) SetTimer(hwnd_, kPathTimeoutTimer, 10000, nullptr);
        const HWND hwnd = hwnd_;
        const unsigned generation = pathGeneration_;
        try {
            std::thread([hwnd, folder, generation] {
                auto listing = std::make_unique<PathListing>(ListFolder(folder));
                listing->generation = generation;
                if (PostMessageW(hwnd, kPathListingMessage, 0, reinterpret_cast<LPARAM>(listing.get()))) {
                    listing.release();
                }
            }).detach();
            ++pathThreadsRunning_;
        } catch (const std::system_error&) {
            pathRequestedFolder_.clear();
        }
    }

    // NFR-018: nothing is kept once the launcher hides or the feature is off.
    void ReleasePathCompletion() {
        KillTimer(hwnd_, kPathTimeoutTimer);
        ++pathGeneration_;  // drop listings still in flight
        pathRequestedFolder_.clear();
        pathListedFolder_.clear();
        std::vector<leanlauncher::typed::PathEntry>().swap(pathEntries_);
        pathListingReady_ = false;
        pathTimedOut_ = false;
        pathError_ = 0;
        pathTruncated_ = false;
    }

    void AddInfoRow(std::wstring text) {
        AppEntry entry;
        entry.category = AppCategory::Info;
        entry.name = std::move(text);
        entry.inert = true;
        results_.push_back(apps_.size());
        apps_.push_back(std::move(entry));
    }

    void ShowPathResults() {
        namespace ti = leanlauncher::typed;
        const ti::PathQuery query = ti::SplitPathQuery(ExpandTypedPath(TrimmedQuery()));
        if (query.kind == ti::PathKind::NeedsShare) {
            AddInfoRow(L"Type a share name, like \\\\server\\share\\");
        } else if (query.kind != ti::PathKind::None) {
            const bool cached = pathListingReady_ && _wcsicmp(query.folder.c_str(), pathListedFolder_.c_str()) == 0;
            if (!cached) RequestPathListing(query.folder);
            if (!cached) {
                if (pathTimedOut_) AddInfoRow(L"Can't reach " + NetworkHostLabel(query.folder));
                else if (IsNetworkFolder(query.folder)) AddInfoRow(L"Looking up " + NetworkHostLabel(query.folder) + L"\u2026");
                else AddInfoRow(L"Reading folder\u2026");
            } else if (pathError_ == ERROR_ACCESS_DENIED) {
                AddInfoRow(L"Access denied");
            } else if (pathError_ != 0 && pathError_ != ERROR_FILE_NOT_FOUND) {
                AddInfoRow(L"Folder not found");
            } else {
                for (const auto& entry : ti::FilterPathEntries(pathEntries_, query.partial)) {
                    AppEntry row;
                    row.category = entry.isDirectory ? AppCategory::Folder : AppCategory::File;
                    row.name = entry.name;
                    row.normalizedName = Normalize(entry.name);
                    row.path = query.folder + entry.name;
                    results_.push_back(apps_.size());
                    apps_.push_back(std::move(row));
                }
                if (results_.empty()) AddInfoRow(L"No matches in this folder");
                if (pathTruncated_) AddInfoRow(L"Showing first 2,000 - keep typing");
            }
        }
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool PathModeActive() const {
        return settings_.enablePathCompletion && page_ == Page::Launcher &&
            leanlauncher::typed::LooksLikePath(TrimmedQuery());
    }

    // Tab in path mode: complete the selected folder or file into the query.
    bool CompleteSelectedPath() {
        if (!PathModeActive() || !HasResult()) return false;
        const AppEntry& app = apps_[results_[selected_]];
        if (app.inert) return true;  // path mode owns Tab even on a status row
        const bool isFolder = app.category == takeoff::AppCategory::Folder;
        input_.text = leanlauncher::typed::CompleteTypedPath(TrimmedQuery(), app.name, isFolder);
        input_.caret = input_.anchor = input_.text.size();
        OnQueryChanged();
        return true;
    }

    // Shift+Enter in path mode: open the typed path itself (its folder part if
    // the full path doesn't exist yet).
    bool OpenTypedPath() {
        if (!PathModeActive()) return false;
        std::wstring target = ExpandTypedPath(TrimmedQuery());
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
            target = leanlauncher::typed::SplitPathQuery(target).folder;
        }
        if (target.empty()) return true;
        Hide();
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    void EnsureVisible() {
        if (selected_ < firstVisible_) firstVisible_ = selected_;
        if (selected_ >= firstVisible_ + visibleRows_) firstVisible_ = selected_ - visibleRows_ + 1;
        firstVisible_ = std::clamp(firstVisible_, 0,
            (std::max)(0, static_cast<int>(results_.size()) - visibleRows_));
    }

    void MoveSelection(int delta, bool wrap) {
        const int count = static_cast<int>(results_.size());
        if (count == 0 || delta == 0) return;
        CancelCommandConfirm();
        LockHoverAtPointer();
        selected_ = wrap ? (selected_ + delta + count) % count
                         : std::clamp(selected_ + delta, 0, count - 1);
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
        SchedulePreview();
    }

    void ResetToDefaults() {
        recordingRow_ = -1;
        editingRow_ = -1;
        vaultDropdownOpen_ = false;
        vaultDropdownHighlight_ = -1;
        webSearchDropdownOpen_ = false;
        webSearchDropdownHighlight_ = -1;
        quickOpenDropdownOpen_ = false;
        quickOpenDropdownHighlight_ = -1;
        obsidianExpandedSection_ = -1;
        settingsStatus_.clear();
        const quicklaunch::Settings before = settings_;
        settings_ = quicklaunch::Settings{};
        obsidianVaultPath_.clear();
        // Same staleness gap as CommitEditingRow: settings_.dailyNote*Override
        // just got reset to empty, but dailyNoteConfig_ is a cache that
        // won't reflect that until something recomputes it.
        dailyNoteConfig_ = leanlauncher::obsidian::ResolveDailyNoteConfig(
            obsidianVaultPath_, settings_.dailyNoteFolderOverride, settings_.dailyNoteFormatOverride);
        if constexpr (!kUiTest) {
            leanlauncher::obsidian::NoteIndex::Instance().Stop();
        }
        SetRunAtStartup(settings_.runAtStartup);
        SaveSettings();
        ApplyRuntimeSettings(before, L"");  // also restarts the file index if it was off (NFR-018)
        settingsStatus_ = L"Settings reset to default.";
        CheckForUpdatesAsync(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Screen-space Y where the vault dropdown list begins - directly below
    // the "Obsidian Vault" row at its current scroll position.
    float VaultDropdownTop() const {
        return SettingsRowTop(kRowVaultPicker) + kSettingsRowHeight + (kSettingsHeaderHeight - settingsScroll_);
    }

    void OpenVaultDropdown() {
        if (knownVaults_.empty()) {
            settingsStatus_ = L"No Obsidian vaults found. Open a vault in Obsidian, then reopen Settings.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editingRow_ >= 0) CancelEditingRow();
        vaultDropdownOpen_ = true;
        const auto it = std::find(knownVaults_.begin(), knownVaults_.end(), obsidianVaultPath_);
        vaultDropdownHighlight_ = (it != knownVaults_.end())
            ? static_cast<int>(std::distance(knownVaults_.begin(), it))
            : 0;
        vaultDropdownScroll_ = 0;
        EnsureVaultDropdownHighlightVisible();
        settingsStatus_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CloseVaultDropdown() {
        vaultDropdownOpen_ = false;
        vaultDropdownHighlight_ = -1;
        vaultDropdownScroll_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // -1 leaves the current vault unchanged (used when the dropdown is
    // dismissed without picking anything, e.g. Esc or click-away).
    void SelectVaultDropdownItem(int index) {
        if (index < 0 || index >= static_cast<int>(knownVaults_.size())) {
            CloseVaultDropdown();
            return;
        }
        obsidianVaultPath_ = knownVaults_[index];
        dailyNoteConfig_ = leanlauncher::obsidian::ResolveDailyNoteConfig(
            obsidianVaultPath_, settings_.dailyNoteFolderOverride, settings_.dailyNoteFormatOverride);
        if constexpr (!kUiTest) {
            leanlauncher::obsidian::NoteIndex::Instance().Restart(obsidianVaultPath_, hwnd_);
        }
        SaveSettings();
        CloseVaultDropdown();
    }

    // How many items the dropdown can show at once before running past the
    // bottom of the settings viewport. Vault counts are unbounded (one per
    // Obsidian vault the user has ever opened), unlike the fixed-size web
    // search preset list, so this list needs to scroll instead of overflow.
    int VaultDropdownMaxVisibleItems() const {
        const float available = FooterTop() - VaultDropdownTop();
        return (std::max)(1, static_cast<int>(available / kVaultDropdownItemHeight));
    }

    // Clamps vaultDropdownScroll_ so vaultDropdownHighlight_ stays within the
    // visible window - called after the highlight moves (open, arrow keys).
    void EnsureVaultDropdownHighlightVisible() {
        const int count = static_cast<int>(knownVaults_.size());
        const int maxVisible = VaultDropdownMaxVisibleItems();
        const int maxScroll = (std::max)(0, count - maxVisible);
        if (vaultDropdownHighlight_ < vaultDropdownScroll_) {
            vaultDropdownScroll_ = vaultDropdownHighlight_;
        } else if (vaultDropdownHighlight_ >= vaultDropdownScroll_ + maxVisible) {
            vaultDropdownScroll_ = vaultDropdownHighlight_ - maxVisible + 1;
        }
        vaultDropdownScroll_ = std::clamp(vaultDropdownScroll_, 0, maxScroll);
    }

    // Scrolls the open dropdown list by deltaItems (positive = down), used by
    // WM_MOUSEWHEEL while the dropdown has focus.
    void ScrollVaultDropdown(int deltaItems) {
        const int count = static_cast<int>(knownVaults_.size());
        const int maxVisible = VaultDropdownMaxVisibleItems();
        const int maxScroll = (std::max)(0, count - maxVisible);
        const int newScroll = std::clamp(vaultDropdownScroll_ + deltaItems, 0, maxScroll);
        if (newScroll != vaultDropdownScroll_) {
            vaultDropdownScroll_ = newScroll;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    // Index into knownVaults_ for a point inside the open dropdown list, or
    // -1 if the point misses the list (including when it's closed).
    int VaultDropdownItemAtPoint(float x, float y) const {
        if (!vaultDropdownOpen_) return -1;
        if (x < 16.0f || x > width_ - 16.0f) return -1;
        if (y < kSettingsHeaderHeight || y >= FooterTop()) return -1;
        const float listTop = VaultDropdownTop();
        const int count = static_cast<int>(knownVaults_.size());
        const int visibleCount = (std::min)(count, VaultDropdownMaxVisibleItems());
        for (int slot = 0; slot < visibleCount; ++slot) {
            const float itemTop = listTop + static_cast<float>(slot) * kVaultDropdownItemHeight;
            if (y >= itemTop && y < itemTop + kVaultDropdownItemHeight) {
                return vaultDropdownScroll_ + slot;
            }
        }
        return -1;
    }

    // Screen-space Y where the web search engine dropdown list begins.
    // Unlike VaultDropdownTop() (which never has more than a handful of
    // real-world entries), this list always has kWebSearchPresetCount + 1
    // fixed entries and the "Search engine" row sits near the bottom of a
    // 3-row category - directly below the row overflows past FooterTop()
    // with no way to scroll to the last items. Clamp so the whole list
    // stays inside [viewportTop, viewportBottom], sliding it up over
    // whatever rows are above (same "drawn on top" philosophy as the vault
    // dropdown) rather than letting it run off the bottom of the window.
    float WebSearchDropdownTop() const {
        const float desired =
            SettingsRowTop(kRowWebSearchEngine) + kSettingsRowHeight + (kSettingsHeaderHeight - settingsScroll_);
        const int itemCount = static_cast<int>(takeoff::kWebSearchPresetCount) + 1;
        const float listHeight = static_cast<float>(itemCount) * kVaultDropdownItemHeight;
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        return std::clamp(desired, viewportTop, (std::max)(viewportTop, viewportBottom - listHeight));
    }

    void OpenWebSearchDropdown() {
        if (editingRow_ >= 0) CancelEditingRow();
        webSearchDropdownOpen_ = true;
        const int presetIndex = takeoff::FindWebSearchPresetIndex(settings_.webSearchUrlTemplate);
        webSearchDropdownHighlight_ = (presetIndex >= 0) ? presetIndex : static_cast<int>(takeoff::kWebSearchPresetCount);
        settingsStatus_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CloseWebSearchDropdown() {
        webSearchDropdownOpen_ = false;
        webSearchDropdownHighlight_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // index in [0, kWebSearchPresetCount) picks a preset immediately; index
    // == kWebSearchPresetCount ("Custom") opens inline text edit on the
    // current template instead of applying anything yet - matching AC3's
    // "Selecting Custom reveals a text field", not an immediate save.
    void SelectWebSearchDropdownItem(int index) {
        const int presetCount = static_cast<int>(takeoff::kWebSearchPresetCount);
        if (index < 0 || index > presetCount) {
            CloseWebSearchDropdown();
            return;
        }
        CloseWebSearchDropdown();
        if (index == presetCount) {
            BeginEditingRow(kRowWebSearchEngine, settings_.webSearchUrlTemplate);
            return;
        }
        settings_.webSearchUrlTemplate = takeoff::kWebSearchPresets[index].urlTemplate;
        settings_.webSearchEngineName = takeoff::kWebSearchPresets[index].name;
        SaveSettings();
    }

    // Index into [0, kWebSearchPresetCount] ("Custom" is the trailing
    // entry) for a point inside the open dropdown list, or -1 if the point
    // misses the list (including when it's closed). Mirrors VaultDropdownItemAtPoint.
    int WebSearchDropdownItemAtPoint(float x, float y) const {
        if (!webSearchDropdownOpen_) return -1;
        if (x < 16.0f || x > width_ - 16.0f) return -1;
        if (y < kSettingsHeaderHeight || y >= FooterTop()) return -1;
        const float listTop = WebSearchDropdownTop();
        const int itemCount = static_cast<int>(takeoff::kWebSearchPresetCount) + 1;
        for (int i = 0; i < itemCount; ++i) {
            const float itemTop = listTop + static_cast<float>(i) * kVaultDropdownItemHeight;
            if (y >= itemTop && y < itemTop + kVaultDropdownItemHeight) {
                return i;
            }
        }
        return -1;
    }

    static constexpr int kQuickOpenTargetCount = 4;

    static const wchar_t* QuickOpenTargetLabel(int choice) {
        switch (choice) {
        case 1: return L"Task target";
        case 2: return L"Append target";
        case 3: return L"Log target";
        default: return L"Daily note";
        }
    }

    // Overlay picker for the Quick open row (US-026) - same fixed-list
    // pattern as the web search engine dropdown, with four entries indexed
    // by settings_.quickOpenTarget.
    float QuickOpenDropdownTop() const {
        const float desired =
            SettingsRowTop(kRowQuickOpenTarget) + kSettingsRowHeight + (kSettingsHeaderHeight - settingsScroll_);
        const float listHeight = static_cast<float>(kQuickOpenTargetCount) * kVaultDropdownItemHeight;
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        return std::clamp(desired, viewportTop, (std::max)(viewportTop, viewportBottom - listHeight));
    }

    void OpenQuickOpenDropdown() {
        if (editingRow_ >= 0) CancelEditingRow();
        quickOpenDropdownOpen_ = true;
        quickOpenDropdownHighlight_ = std::clamp(settings_.quickOpenTarget, 0, kQuickOpenTargetCount - 1);
        settingsStatus_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CloseQuickOpenDropdown() {
        quickOpenDropdownOpen_ = false;
        quickOpenDropdownHighlight_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SelectQuickOpenDropdownItem(int index) {
        CloseQuickOpenDropdown();
        if (index < 0 || index >= kQuickOpenTargetCount) return;
        settings_.quickOpenTarget = index;
        SaveSettings();
    }

    // Mirrors WebSearchDropdownItemAtPoint.
    int QuickOpenDropdownItemAtPoint(float x, float y) const {
        if (!quickOpenDropdownOpen_) return -1;
        if (x < 16.0f || x > width_ - 16.0f) return -1;
        if (y < kSettingsHeaderHeight || y >= FooterTop()) return -1;
        const float listTop = QuickOpenDropdownTop();
        for (int i = 0; i < kQuickOpenTargetCount; ++i) {
            const float itemTop = listTop + static_cast<float>(i) * kVaultDropdownItemHeight;
            if (y >= itemTop && y < itemTop + kVaultDropdownItemHeight) {
                return i;
            }
        }
        return -1;
    }

    // Expands the given section, collapsing whichever other one was open;
    // expanding the already-expanded section collapses it instead.
    void ToggleObsidianSection(int section) {
        obsidianExpandedSection_ = (obsidianExpandedSection_ == section) ? -1 : section;
        // Expanding/collapsing changes SettingsContentBottom() by up to a
        // section's worth of rows - clamp scroll back into range (collapse
        // can leave it past the new, shorter bottom) and, when expanding,
        // scroll the newly-revealed rows into view (they can land below
        // the fold with no other cue that anything changed).
        settingsScroll_ = std::clamp(settingsScroll_, 0.0f, SettingsMaxScroll());
        if (obsidianExpandedSection_ >= 0) {
            EnsureSettingsVisible(LastRowInCategory(settingsCategory_));
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Maps an editable Settings row to the Settings field it edits, or
    // nullptr if the row isn't a free-text row. Centralizes the mapping so
    // ChangeSetting/BeginEditingRow/CommitEditingRow can't drift apart.
    std::wstring* SettingsTextFieldForRow(int row) {
        switch (row) {
        case kRowWebSearchEngine: return &settings_.webSearchUrlTemplate;
        case kRowFileSearchPrefix: return &settings_.fileSearchPrefix;
        case kRowWebSearchPrefix: return &settings_.webSearchPrefix;
        case kRowAppSearchPrefix: return &settings_.appSearchPrefix;
        case kRowSystemCommandsPrefix: return &settings_.systemCommandsPrefix;
        case kRowPomodoroPrefix: return &settings_.pomodoroPrefix;
        case kRowPomodoroFocusMinutes: return &settings_.pomodoroFocusMinutes;
        case kRowPomodoroBreakMinutes: return &settings_.pomodoroBreakMinutes;
        case kRowVaultSearchPrefix: return &settings_.vaultSearchPrefix;
        case kRowVaultSearchPillLabel: return &settings_.vaultSearchPillLabel;
        case kRowTaskPrefix: return &settings_.taskPrefix;
        case kRowTaskPillLabel: return &settings_.taskPillLabel;
        case kRowTaskPreviewPrefix: return &settings_.taskPreviewPrefix;
        case kRowNoteAddPrefix: return &settings_.noteAddPrefix;
        case kRowNoteAddPillLabel: return &settings_.noteAddPillLabel;
        case kRowNoteAddPreviewPrefix: return &settings_.noteAddPreviewPrefix;
        case kRowLogPrefix: return &settings_.logPrefix;
        case kRowLogPillLabel: return &settings_.logPillLabel;
        case kRowLogPreviewPrefix: return &settings_.logPreviewPrefix;
        case kRowLogHeading: return &settings_.logHeading;
        case kRowDailyNoteFolderOverride: return &settings_.dailyNoteFolderOverride;
        case kRowDailyNoteFormatOverride: return &settings_.dailyNoteFormatOverride;
        case kRowTaskTargetNote: return &settings_.taskTargetNote;
        case kRowNoteAddTargetNote: return &settings_.noteAddTargetNote;
        case kRowLogTargetNote: return &settings_.logTargetNote;
        case kRowSnippetsPrefix: return &settings_.snippetsPrefix;
        case kRowSnippetsFile: return &settings_.snippetsPath;
        default: return nullptr;
        }
    }

    void BeginEditingRow(int row, const std::wstring& currentValue) {
        if (editingRow_ == row) return;
        editingRow_ = row;
        settingsEdit_.text = currentValue;
        settingsEdit_.anchor = 0;
        settingsEdit_.caret = currentValue.size();
        settingsStatus_.clear();
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CommitEditingRow() {
        std::wstring* field = SettingsTextFieldForRow(editingRow_);
        if (!field) {
            editingRow_ = -1;
            return;
        }
        // US-044: the same per-field check the import uses (prefix conflicts,
        // minutes, web template, target notes inside the vault - US-025).
        const leanlauncher::settings_io::TextCheck checked = leanlauncher::settings_io::CheckTextSetting(
            settings_, leanlauncher::settings_io::TextSettingMember(settings_, field), settingsEdit_.text);
        if (checked.error) {
            settingsStatus_ = checked.error;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;  // stay in edit mode so the user can fix it
        }
        const bool wasLogHeadingRow = (editingRow_ == kRowLogHeading);
        const bool wasWebSearchEngineRow = (editingRow_ == kRowWebSearchEngine);
        const bool wasSnippetsFileRow = (editingRow_ == kRowSnippetsFile);
        *field = checked.value;
        editingRow_ = -1;
        if (wasWebSearchEngineRow) {
            settings_.webSearchEngineName = takeoff::DeriveSearchEngineName(*field);
        }
        if (wasLogHeadingRow && leanlauncher::obsidian::HeadingLevel(settingsEdit_.text) == 0) {
            settingsStatus_ = L"No matching heading - log entries will be appended to the end of the note.";
        } else {
            settingsStatus_.clear();
        }
        // dailyNoteConfig_ is a cache derived from the override fields, not
        // read from them directly (see ResolveTodayPath's call sites) - it
        // must be refreshed here or a folder/format override just edited
        // has no effect until the app restarts or the vault is reselected.
        dailyNoteConfig_ = leanlauncher::obsidian::ResolveDailyNoteConfig(
            obsidianVaultPath_, settings_.dailyNoteFolderOverride, settings_.dailyNoteFormatOverride);
        SaveSettings();
        if (wasSnippetsFileRow) ApplySnippetsState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CancelEditingRow() {
        editingRow_ = -1;
        settingsStatus_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ChangeSetting(int row) {
        settingsStatus_.clear();
        if (row == kRowResetToDefaults) {
            ResetToDefaults();
            return;
        }
        if (row == kRowObsidianEnabled) {
            settings_.obsidianEnabled = !settings_.obsidianEnabled;
            SaveSettings();
            if constexpr (!kUiTest) {
                if (settings_.obsidianEnabled) {
                    if (!obsidianVaultPath_.empty()) {
                        leanlauncher::obsidian::NoteIndex::Instance().Start(obsidianVaultPath_, hwnd_);
                    }
                } else {
                    leanlauncher::obsidian::NoteIndex::Instance().Stop();
                }
            }
            if (!settings_.obsidianEnabled) {
                editingRow_ = -1;
                obsidianExpandedSection_ = -1;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowVaultPicker) {
            OpenVaultDropdown();
            return;
        }
        if (row == kRowWebSearchEngine) {
            OpenWebSearchDropdown();
            return;
        }
        if (row == kRowQuickOpenTarget) {
            OpenQuickOpenDropdown();
            return;
        }
        if (row == kRowAboutExportSettings) {
            ExportSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowAboutImportSettings) {
            ImportSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowAboutGithubLink) {
            ShellExecuteW(nullptr, L"open", takeoff::kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (row == kRowAboutCheckUpdates) {
            if (!takeoff::IsUpdateRowClickable(updateState_)) return;
            if (updateState_ == takeoff::UpdateCheckState::Ready) {
                RestartToUpdate();
            } else if (updateState_ == takeoff::UpdateCheckState::Available) {
                const std::wstring url = takeoff::ReleasePageUrlOrDefault(updateReleaseUrl_);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                CheckForUpdatesAsync(true, true);
            }
            return;
        }
        if (row == kRowFileSearchEditExclusions) {
            // File I/O + spawning the user's editor is a real side effect
            // that a UI test run must not trigger - mirrors the existing
            // if constexpr (!kUiTest) guard used for Obsidian's own
            // Start()/Stop() side effect a few lines above kRowVaultPicker.
            if constexpr (!kUiTest) {
                const std::wstring exclusionsPath = takeoff::FileIndex::DefaultExclusionsPath();
                if (takeoff::FileIndex::EnsureExclusionsFileWithHeader(exclusionsPath)) {
                    ShellExecuteW(nullptr, L"open", exclusionsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return;
        }
        if (row == kRowFileSearchHelp) {
            const std::wstring helpUrl = std::wstring(takeoff::kRepoUrl) + L"/blob/master/docs/file-search.md";
            ShellExecuteW(nullptr, L"open", helpUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (row == kRowVaultSearchSummary) { ToggleObsidianSection(kSectionVaultSearch); return; }
        if (row == kRowTaskSummary) { ToggleObsidianSection(kSectionTask); return; }
        if (row == kRowNoteAddSummary) { ToggleObsidianSection(kSectionNoteAdd); return; }
        if (row == kRowLogSummary) { ToggleObsidianSection(kSectionLog); return; }
        if (row == kRowOverridesSummary) { ToggleObsidianSection(kSectionOverrides); return; }
        if (row == kRowVaultSearchEnabled) {
            settings_.vaultSearchEnabled = !settings_.vaultSearchEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowTaskAddEnabled) {
            settings_.taskAddEnabled = !settings_.taskAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowNoteAddEnabled) {
            settings_.noteAddEnabled = !settings_.noteAddEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowLogEnabled) {
            settings_.logEnabled = !settings_.logEnabled;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowPomodoroEnabled) {
            settings_.enablePomodoro = !settings_.enablePomodoro;
            SaveSettings();
            if (!settings_.enablePomodoro) EndPomodoro(leanlauncher::pomodoro::EndReason::Stopped);  // NFR-018
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowPomodoroLog) {
            settings_.pomodoroLog = !settings_.pomodoroLog;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowPreviewEnabled) {
            settings_.enablePreview = !settings_.enablePreview;
            SaveSettings();
            // NFR-018: turning the panel off frees its timer, buffers,
            // layouts and bitmaps, and drops any load still in flight.
            if (!settings_.enablePreview) ClosePreview(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowTimeZonesEnabled) {
            settings_.enableTimeZones = !settings_.enableTimeZones;
            SaveSettings();
            if (!settings_.enableTimeZones) timeZones_.Clear();  // NFR-018
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowUnitConverterEnabled) {
            settings_.enableUnitConverter = !settings_.enableUnitConverter;
            SaveSettings();
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowPathCompletionEnabled) {
            settings_.enablePathCompletion = !settings_.enablePathCompletion;
            SaveSettings();
            if (!settings_.enablePathCompletion) ReleasePathCompletion();
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowTypedUrlsEnabled) {
            settings_.enableTypedUrls = !settings_.enableTypedUrls;
            SaveSettings();
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowSystemCommandsEnabled) {
            settings_.enableSystemCommands = !settings_.enableSystemCommands;
            SaveSettings();
            CancelCommandConfirm();
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowSnippetsEnabled) {
            if (!settings_.enableSnippets) {
                // The prefix only joins the conflict check while the feature is on,
                // so an empty or colliding prefix can be saved while it is off.
                quicklaunch::Settings candidate = settings_;
                candidate.enableSnippets = true;
                if (const wchar_t* problem = leanlauncher::settings_io::detail::PrefixConflict(candidate)) {
                    settingsStatus_ = std::wstring(problem) + L" Change the snippets prefix first.";
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            settings_.enableSnippets = !settings_.enableSnippets;
            SaveSettings();
            ApplySnippetsState();  // starts or fully stops the keyboard listener (NFR-018)
            // A failed hook start turns the toggle off again and sets its own message.
            UpdateResults();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (row == kRowSnippetsOpen) {
            if constexpr (!kUiTest) {
                const std::wstring path = SnippetsFilePath();
                namespace sn = leanlauncher::snippets;
                sn::FileState state = sn::ClassifyPath(path);
                if (state == sn::FileState::Missing) {
                    WriteDefaultSnippetsFile(path);
                    state = sn::ClassifyPath(path);
                }
                if (state != sn::FileState::Present) {
                    settingsStatus_ = L"Couldn't reach your snippets file.";
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        if (row == kRowSnippetsImport) {
            if constexpr (!kUiTest) ImportEspansoSnippets();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (std::wstring* field = SettingsTextFieldForRow(row)) {
            BeginEditingRow(row, *field);
            return;
        }
        if (row < 4 || row == kRowPreviewHotkey) {
            // Keyboard shortcut rows: enter recording mode.
            recordingRow_ = row;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        switch (row) {
        case 4: {
            const bool enabled = !settings_.runAtStartup;
            if (SetRunAtStartup(enabled)) {
                settings_.runAtStartup = enabled;
                SaveSettings();
            } else settingsStatus_ = L"Windows would not update the startup setting.";
            break;
        }
        case 5:
            settings_.showTrayIcon = !settings_.showTrayIcon;
            SaveSettings();
            UpdateTrayIcon();
            break;
        case 6:
            settings_.checkForUpdates = !settings_.checkForUpdates;
            SaveSettings();
            if (!settings_.checkForUpdates) {
                // A check that is already running still reports its result
                // (the setting only gates automatic checks) - resetting the
                // row now would show "Check now" while it keeps running.
                if (!*updateInProgress_) {
                    updateAvailable_ = false;
                    updateDownloaded_ = false;
                    updateState_ = takeoff::UpdateCheckState::Idle;
                }
            } else {
                CheckForUpdatesAsync(true);
            }
            break;
        case 7:
            settings_.enableFileSearch = !settings_.enableFileSearch;
            SaveSettings();
            if constexpr (!kUiTest) {
                // NFR-018: off frees the index now, not at the next restart;
                // on reloads it from the cache the stopped cycle left behind.
                if (settings_.enableFileSearch) {
                    FileIndex::Instance().Start(hwnd_);
                } else {
                    FileIndex::Instance().StopAndRelease();
                }
            }
            UpdateResults();
            break;
        case 8:
            settings_.enableWebSearch = !settings_.enableWebSearch;
            SaveSettings();
            break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void HandleRecordingKey(WPARAM key, bool control, bool shift, bool alt) {
        if (key == VK_ESCAPE) {
            recordingRow_ = -1;
            settingsStatus_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
            key == VK_LCONTROL || key == VK_RCONTROL ||
            key == VK_LSHIFT || key == VK_RSHIFT ||
            key == VK_LMENU || key == VK_RMENU) {
            return;
        }
        if (key == VK_BACK && !control && !shift && !alt) {
            if (recordingRow_ >= 2) {
                ApplyRecordedBinding({0, 0, true});
            } else {
                settingsStatus_ = L"This shortcut cannot be disabled.";
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        uint16_t modifiers = 0;
        if (control) modifiers |= quicklaunch::kModControl;
        if (alt) modifiers |= quicklaunch::kModAlt;
        if (shift) modifiers |= quicklaunch::kModShift;
        if (modifiers == 0) {
            settingsStatus_ = L"Hold Ctrl, Alt, or Shift with your key.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        quicklaunch::HotkeyBinding proposed;
        if (recordingRow_ <= 1 || recordingRow_ == kRowPreviewHotkey) {
            proposed = {modifiers, static_cast<uint16_t>(key)};
        } else {
            proposed = {modifiers, 0};
        }
        ApplyRecordedBinding(proposed);
    }

    bool CheckHotkeyTaken(uint16_t modifiers, uint16_t vk) {
        if constexpr (kUiTest) {
            return hwnd_ == nullptr;
        } else {
            if (vk == 0) return false;
            const bool hadRegistered = hotkeyRegistered_;
            if (hadRegistered) {
                UnregisterHotKey(hwnd_, kHotkeyId);
                hotkeyRegistered_ = false;
            }
            constexpr int kTestId = 0xBEEF;
            const UINT winMods = static_cast<UINT>(modifiers) | MOD_NOREPEAT;
            const bool taken = !RegisterHotKey(hwnd_, kTestId, winMods, vk);
            if (!taken) {
                UnregisterHotKey(hwnd_, kTestId);
            }
            if (hadRegistered) {
                RegisterShortcut();
            }
            return taken;
        }
    }

    void ApplyRecordedBinding(const quicklaunch::HotkeyBinding& proposed) {
        settingsStatus_.clear();
        if (proposed.disabled) {
            if (recordingRow_ == 0) settings_.launcherHotkey = proposed;
            else if (recordingRow_ == 1) settings_.actionsHotkey = proposed;
            else if (recordingRow_ == 2) settings_.administratorHotkey = proposed;
            else if (recordingRow_ == 3) settings_.quickLaunchHotkey = proposed;
            else if (recordingRow_ == kRowPreviewHotkey) settings_.previewHotkey = proposed;
            if (recordingRow_ == 0) RegisterShortcut();
            recordingRow_ = -1;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if ((recordingRow_ <= 1 || recordingRow_ == kRowPreviewHotkey) &&
            quicklaunch::IsSystemReserved(proposed.modifiers, proposed.key)) {
            settingsStatus_ = L"That shortcut is reserved by Windows.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (recordingRow_ == 2 && quicklaunch::IsSystemReserved(proposed.modifiers, VK_RETURN)) {
            settingsStatus_ = L"That shortcut is reserved by Windows.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if ((recordingRow_ == 1 || recordingRow_ == kRowPreviewHotkey) && quicklaunch::IsReservedInApp(proposed)) {
            settingsStatus_ = L"That shortcut is reserved for text editing.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // Preview panel is row 4 in HasInternalConflict's row scheme, not
        // kRowPreviewHotkey (60) - that's the on-screen Settings row ID.
        const int conflictRow = (recordingRow_ == kRowPreviewHotkey) ? 4 : recordingRow_;
        if (const wchar_t* conflict = quicklaunch::HasInternalConflict(conflictRow, proposed, settings_)) {
            settingsStatus_ = conflict;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (recordingRow_ == 0) {
            quicklaunch::HotkeyBinding previous = settings_.launcherHotkey;
            settings_.launcherHotkey = proposed;
            RegisterShortcut();
            if (!hotkeyRegistered_) {
                settings_.launcherHotkey = previous;
                RegisterShortcut();
                settingsStatus_ = L"That shortcut is used by another application.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        } else if (recordingRow_ == 1) {
            if (CheckHotkeyTaken(proposed.modifiers, proposed.key)) {
                settingsStatus_ = L"That shortcut is used by another application.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            settings_.actionsHotkey = proposed;
        } else if (recordingRow_ == 2) {
            if (CheckHotkeyTaken(proposed.modifiers, VK_RETURN)) {
                settingsStatus_ = L"That shortcut is used by another application.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            settings_.administratorHotkey = proposed;
        } else if (recordingRow_ == 3) {
            bool anyTaken = false;
            for (uint16_t k = '1'; k <= '8'; ++k) {
                if (CheckHotkeyTaken(proposed.modifiers, k)) {
                    anyTaken = true;
                    break;
                }
            }
            if (anyTaken) {
                settingsStatus_ = L"That shortcut is used by another application.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            settings_.quickLaunchHotkey = proposed;
        } else if (recordingRow_ == kRowPreviewHotkey) {
            if (CheckHotkeyTaken(proposed.modifiers, proposed.key)) {
                settingsStatus_ = L"That shortcut is used by another application.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            settings_.previewHotkey = proposed;
        }

        recordingRow_ = -1;
        SaveSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static bool MatchesBinding(const quicklaunch::HotkeyBinding& binding, WPARAM key,
            bool control, bool shift, bool alt) {
        if (binding.disabled || binding.key == 0) return false;
        uint16_t mods = 0;
        if (control) mods |= quicklaunch::kModControl;
        if (alt) mods |= quicklaunch::kModAlt;
        if (shift) mods |= quicklaunch::kModShift;
        return mods == binding.modifiers && static_cast<uint16_t>(key) == binding.key;
    }

    bool MatchesActionsHotkey(WPARAM key, bool control, bool shift, bool alt) const {
        return MatchesBinding(settings_.actionsHotkey, key, control, shift, alt);
    }

    bool MatchesPreviewHotkey(WPARAM key, bool control, bool shift, bool alt) const {
        return settings_.enablePreview &&
               MatchesBinding(settings_.previewHotkey, key, control, shift, alt);
    }

    // US-045: the panel shows only on the launcher page, with the toggle on
    // and the remembered open state set. A disabled Ctrl+P binding doesn't
    // hide a panel that was left open - the remembered state still applies.
    bool PreviewVisible() const {
        return settings_.enablePreview && settings_.previewOpen && page_ == Page::Launcher;
    }

    // The panel's area: the side strip right of the launcher, or the results
    // area when it's drawn as an overlay on a narrow work area.
    D2D1_RECT_F PreviewRect() const {
        if (previewOverlay_) return D2D1::RectF(8, ResultsTop(), width_ - 8, FooterTop());
        return D2D1::RectF(width_, 0, width_ + panelWidth_, height_);
    }

    void TogglePreview() {
        if (!settings_.enablePreview) return;
        settings_.previewOpen = !settings_.previewOpen;
        SaveSettings();
        ResizeAndPosition();
        // Opening loads the selection straight away (no debounce: it's one
        // keypress, not a stream of selection changes); closing frees it all.
        if (PreviewVisible()) RequestPreview();
        else ClosePreview(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ---- US-045 preview loading (Task 6) --------------------------------------
    // What DrawPreviewPanel draws. Built on a short-lived worker thread (the
    // US-043 RequestPathListing pattern) from plain data only - the worker
    // never touches Direct2D, DirectWrite or window state.
    struct PreviewContent {
        unsigned generation = 0;
        leanlauncher::preview::PreviewKind kind = leanlauncher::preview::PreviewKind::None;
        std::wstring title;
        std::wstring propertyLine;
        std::wstring body;
        std::wstring status;  // non-empty: shown instead of the body
        bool truncated = false;
        bool todayNote = false;  // a capture row's target is today's daily note
        // US-046 images. The worker hands over plain premultiplied BGRA
        // pixels, never a COM object: the UI thread makes the ID2D1Bitmap
        // from them once, then drops them (see PreviewImageBitmap).
        std::wstring imageKey;   // the thumbnail cache key: path|last write|size
        std::wstring caption;    // "name · W×H · size"
        std::vector<BYTE> pixels;
        UINT pixelWidth = 0, pixelHeight = 0;
        int thumbnailSize = 0;   // the requested box, in pixels
        std::vector<std::wstring> cachedKeys;  // the worker skips the thumbnail for these
    };
    // At most 2 thumbnails, most recent last, so going back to the previous
    // image is quick. UI thread only; freed by ClosePreview and DiscardTarget.
    struct PreviewImage {
        std::wstring key, caption;
        ComPtr<ID2D1Bitmap> bitmap;
    };

    // Loading starts 80 ms after the selection stops changing, so holding an
    // arrow key reads nothing until it's let go.
    void SchedulePreview() {
        if (!PreviewVisible()) return;
        SetTimer(hwnd_, kPreviewDebounceTimer, 80, nullptr);
    }

    // The row's own label, as the results list shows it (for rows with
    // nothing to read: commands, URLs, capture rows and so on).
    std::wstring PreviewCategoryLabel(const AppEntry& app) const {
        switch (app.category) {
        case AppCategory::Command: return L"Command";
        case AppCategory::Url: return L"URL";
        case AppCategory::Pomodoro: return L"Timer";
        case AppCategory::Snippet: return L"Snippet";
        case AppCategory::Calculator: return L"Calculator";
        case AppCategory::TaskAdd: return settings_.taskPillLabel;
        case AppCategory::NoteAdd: return settings_.noteAddPillLabel;
        case AppCategory::LogAdd: return settings_.logPillLabel;
        case AppCategory::NoteJump: return settings_.vaultSearchPillLabel;
        case AppCategory::WebSearch: return settings_.webSearchPillLabel;
        default: return L"";
        }
    }

    void RequestPreview() {
        namespace pv = leanlauncher::preview;
        KillTimer(hwnd_, kPreviewDebounceTimer);
        if (!PreviewVisible() || !IsWindowVisible(hwnd_)) return;
        if (selected_ < 0 || selected_ >= static_cast<int>(results_.size()) || results_[selected_] >= apps_.size()) {
            ClosePreview(false);  // nothing selected: drop loads in flight, show an empty panel
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const AppEntry& app = apps_[results_[selected_]];
        // The results list is rebuilt for many reasons (an index update, a
        // pin) without the selection changing; keep what's shown (and its
        // scroll position) or already loading for the same row.
        const bool captureRow = app.category == AppCategory::TaskAdd || app.category == AppCategory::NoteAdd ||
                                app.category == AppCategory::LogAdd;
        // A capture row's name carries the typed text: keying on it would
        // reload the target note (and reset its scroll) on every keystroke.
        std::wstring key = pv::PreviewKey(static_cast<int>(app.category), app.path, app.name, obsidianVaultPath_,
                                          !(captureRow && !app.path.empty()));
        if (key == previewKey_) return;
        previewKey_ = std::move(key);
        auto content = std::make_unique<PreviewContent>();
        content->title = app.category == AppCategory::NoteJump && !app.parameters.empty() ? app.parameters : app.name;
        std::wstring path;
        switch (app.category) {
        case AppCategory::NoteJump:
            content->kind = pv::PreviewKind::Note;
            if (app.path.empty() || obsidianVaultPath_.empty()) {
                content->kind = pv::PreviewKind::None;
                content->body = PreviewCategoryLabel(app);
            } else if (leanlauncher::obsidian::IsUnsafeVaultRelativePath(app.path)) {
                content->status = L"This note is outside the vault";
            } else {
                // ResolveNoteAbsolutePath appends ".md" itself. Obsidian is never started.
                path = leanlauncher::obsidian::ResolveNoteAbsolutePath(obsidianVaultPath_, app.path);
            }
            break;
        case AppCategory::Folder:
            content->kind = pv::PreviewKind::Folder;
            content->propertyLine = app.path;
            path = app.path;
            if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
            break;
        case AppCategory::File:
            content->kind = pv::ClassifyPreview(app.path, false, false, false);
            if (content->kind == pv::PreviewKind::None) {
                content->propertyLine = app.path;  // AC7: the full path, as for a folder
                content->status = L"No preview for this file type";
            } else {
                path = app.path;
            }
            if (content->kind == pv::PreviewKind::Image) {
                // 384 DIP, kept to the 256-512 px the thumbnail cache serves well.
                content->thumbnailSize = std::clamp(ToPixel(384), 256, 512);
                // The worker checks the file's time and size (no disk access
                // here) and skips the thumbnail if one of these still matches.
                for (const auto& image : previewImages_) content->cachedKeys.push_back(image.key);
            }
            break;
        case AppCategory::Application:
        case AppCategory::System:
            content->kind = pv::PreviewKind::App;
            content->propertyLine = app.category == AppCategory::System ? L"System" : L"Application";
            content->body = app.path;
            path = app.path;
            break;
        case AppCategory::TaskAdd:
        case AppCategory::NoteAdd:
        case AppCategory::LogAdd:
            content->kind = pv::PreviewKind::None;
            content->body = PreviewCategoryLabel(app);
            if (app.path.empty()) break;  // no vault yet: the row's label only
            // app.path is already absolute (CaptureTargetPath); still refuse
            // one that isn't inside the vault. The preview only reads: the
            // capture creates a missing daily note, never the preview.
            content->kind = pv::PreviewKind::Note;
            content->body.clear();
            content->title = fs::path(app.path).stem().wstring();
            if (!pv::IsNoteInsideVault(app.path, obsidianVaultPath_)) {
                content->propertyLine = app.path;  // the loaded note's own property line replaces it otherwise
                content->status = L"This note is outside the vault";
            } else {
                content->todayNote = _wcsicmp(app.path.c_str(), CaptureTargetPath(L"").c_str()) == 0;
                path = app.path;
            }
            break;
        case AppCategory::Snippet: {
            // The replacement text is the preview body.
            content->kind = pv::PreviewKind::None;
            content->body = PreviewCategoryLabel(app);
            size_t snippetIndexValue = 0;
            if (snippetIndex_ && leanlauncher::snippets::IndexFromPath(app.path, snippetIndexValue) &&
                snippetIndexValue < snippetIndex_->Size()) {
                content->body = snippetIndex_->All()[snippetIndexValue].replace;
            }
            break;
        }
        default:
            content->kind = pv::PreviewKind::None;
            content->body = PreviewCategoryLabel(app);
            if (app.category == AppCategory::Url) content->propertyLine = app.path;  // the full URL
            break;
        }
        content->generation = previewGate_.Next();  // anything older in flight is now stale
        if (path.empty()) {  // nothing to read: show it now
            ShowPreviewContent(std::move(content));
            return;
        }
        if (previewThreadsRunning_ >= 4) {
            // Several reads are stuck (a slow network share); try again shortly
            // rather than start a fifth. The stale results drop on arrival.
            content->status = L"Loading\u2026";
            ShowPreviewContent(std::move(content));
            previewKey_.clear();  // not loaded yet: the retry must not be skipped
            SetTimer(hwnd_, kPreviewDebounceTimer, 250, nullptr);
            return;
        }
        const HWND hwnd = hwnd_;
        try {
            std::thread([hwnd, path, content = std::move(content)]() mutable {
                // COM for the shell thumbnail and WIC (US-046). Every COM
                // object LoadPreview makes is local to it and gone before
                // CoUninitialize; the content holds plain data only.
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                LoadPreview(*content, path);
                // Ownership passes to the UI thread only if the post succeeds;
                // a window already destroyed makes it fail, and it's freed here.
                if (PostMessageW(hwnd, kPreviewReadyMessage, 0, reinterpret_cast<LPARAM>(content.get()))) {
                    content.release();
                }
                content.reset();
                if (SUCCEEDED(com)) CoUninitialize();
            }).detach();
            ++previewThreadsRunning_;
        } catch (const std::system_error&) {
            // No thread: the content went with the failed callable; retry later.
            previewKey_.clear();
            SetTimer(hwnd_, kPreviewDebounceTimer, 250, nullptr);
        }
    }

    // Runs on the preview worker: bounded reads only (64 KB, one folder
    // listing, the version resource), nothing written, nothing downloaded.
    static void LoadPreview(PreviewContent& content, const std::wstring& path) {
        namespace pv = leanlauncher::preview;
        try {
            switch (content.kind) {
            case pv::PreviewKind::Note:
            case pv::PreviewKind::Text: {
                pv::TextPreview text = pv::BuildTextPreview(pv::ReadPreviewBytes(path), content.kind == pv::PreviewKind::Note,
                                                           content.todayNote);
                content.propertyLine = std::move(text.propertyLine);
                content.body = std::move(text.body);
                content.status = std::move(text.status);
                content.truncated = text.truncated;
                break;
            }
            case pv::PreviewKind::Folder: {
                const PathListing listing = ListFolder(path);
                if (listing.error == ERROR_ACCESS_DENIED) content.status = L"Access denied";
                else if (listing.error != 0 && listing.error != ERROR_FILE_NOT_FOUND) content.status = L"Folder not found";
                else content.body = pv::FolderSummary(listing.entries, 20, listing.truncated);
                break;
            }
            case pv::PreviewKind::App: {
                const std::wstring version = AppFileVersion(path);
                if (!version.empty()) content.body += L"\n\nVersion " + version;
                break;
            }
            case pv::PreviewKind::Image:
                // The only place a thumbnail handler runs: ClassifyPreview's
                // image extensions, never arbitrary file types.
                LoadImagePreview(content, path);
                break;
            case pv::PreviewKind::None:
                break;
            }
        } catch (...) {
            content.body.clear();
            content.status = L"Can't preview this item";
        }
    }

    // US-046, on the preview worker: the Windows thumbnail (cache) for an
    // image, scaled down to the requested box if the shell handed back a
    // bigger one, as premultiplied BGRA pixels. Online-only files are never
    // thumbnailed or read. No thumbnail means a status, never a full decode.
    static void LoadImagePreview(PreviewContent& content, const std::wstring& path) {
        namespace pv = leanlauncher::preview;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            content.status = L"File not found";
            return;
        }
        if (pv::IsCloudPlaceholder(info.dwFileAttributes)) {
            content.status = L"Not downloaded - open to download";
            return;
        }
        const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        content.imageKey = path + L"|" + std::to_wstring((static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime) + L"|" + std::to_wstring(size);
        if (std::find(content.cachedKeys.begin(), content.cachedKeys.end(), content.imageKey) != content.cachedKeys.end()) {
            return;  // unchanged since it was cached: the UI draws its bitmap
        }
        content.status = L"No thumbnail for this image";
        ComPtr<IShellItemImageFactory> shell;
        ComPtr<IWICImagingFactory> wic;
        HBITMAP hbitmap = nullptr;
        const int box = content.thumbnailSize;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&shell))) ||
                FAILED(shell->GetImage({box, box}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbitmap)) || !hbitmap) {
            return;
        }
        ComPtr<IWICBitmap> thumbnail;
        const bool converted = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbitmap, nullptr, WICBitmapUsePremultipliedAlpha, &thumbnail));
        DeleteObject(hbitmap);
        UINT width = 0, height = 0;
        if (!converted || FAILED(thumbnail->GetSize(&width, &height)) || !width || !height) return;
        const pv::FitSize fit = pv::FitImage(static_cast<float>(width), static_cast<float>(height),
            static_cast<float>(box), static_cast<float>(box));
        const UINT fitWidth = (std::max)(1u, static_cast<UINT>(std::lround(fit.width)));
        const UINT fitHeight = (std::max)(1u, static_cast<UINT>(std::lround(fit.height)));
        ComPtr<IWICBitmapSource> source = thumbnail;
        ComPtr<IWICBitmapScaler> scaler;
        if (fitWidth < width || fitHeight < height) {
            if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
                    FAILED(scaler->Initialize(thumbnail.Get(), fitWidth, fitHeight, WICBitmapInterpolationModeFant))) return;
            source = scaler;
        }
        ComPtr<IWICBitmapSource> bgra;
        if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, source.Get(), &bgra))) return;
        std::vector<BYTE> pixels(static_cast<size_t>(fitWidth) * fitHeight * 4);
        if (FAILED(bgra->CopyPixels(nullptr, fitWidth * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return;
        // The image's own dimensions for the caption, parsed from at most the
        // first 64 KB (the bounded reader: never blocks other apps, never
        // recalls). No codec runs, so the extension gate holds.
        const auto dims = pv::ImageDimensions(pv::ReadPreviewBytes(path).bytes);
        content.status.clear();
        content.pixels = std::move(pixels);
        content.pixelWidth = fitWidth;
        content.pixelHeight = fitHeight;
        content.caption = pv::ImageCaption(path, dims ? dims->first : 0, dims ? dims->second : 0, size);
    }

    // "FileVersion" from an .exe's version resource, or empty. Online-only
    // files are skipped: reading the resource would download them.
    static std::wstring AppFileVersion(const std::wstring& path) {
        if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".exe") != 0) return {};
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || leanlauncher::preview::IsCloudPlaceholder(attributes)) return {};
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (size == 0) return {};
        std::vector<BYTE> data(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
        struct Translation { WORD language; WORD codePage; };
        Translation* translations = nullptr;
        UINT length = 0;
        if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) &&
                translations && length >= sizeof(Translation)) {
            wchar_t key[64];
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileVersion", translations[0].language, translations[0].codePage);
            wchar_t* value = nullptr;
            UINT valueLength = 0;
            if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &valueLength) && value && valueLength > 1) {
                return std::wstring(value, wcsnlen(value, valueLength));
            }
        }
        VS_FIXEDFILEINFO* fixed = nullptr;
        if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) && fixed && length >= sizeof(VS_FIXEDFILEINFO)) {
            return std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"." +
                   std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(fixed->dwFileVersionLS));
        }
        return {};
    }

    // UI thread only: the new content replaces the old, and the old layout
    // (Task 7) is rebuilt for it on the next paint.
    void ShowPreviewContent(std::unique_ptr<PreviewContent> content) {
        preview_ = std::move(content);
        previewScroll_ = 0;
        ReleasePreviewLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // NFR-018: drops everything the panel holds, thumbnails included (US-046:
    // they're freed when the launcher hides, too). `free` is true when the
    // panel closed or the toggle went off, false when the launcher hides or
    // there's nothing to show. A worker still reading finishes on its own
    // (reads are bounded) and its result is deleted when it arrives.
    void ClosePreview([[maybe_unused]] bool free) {
        KillTimer(hwnd_, kPreviewDebounceTimer);
        previewGate_.Invalidate();
        previewKey_.clear();
        preview_.reset();
        ReleasePreviewLayout();
        previewScroll_ = 0;
        previewImages_.clear();
    }

    // The shown image's cache entry: found by key (moved to most recent), or
    // made from the worker's pixels on first draw (which then go; the cache
    // keeps 2 at most). Neither, e.g. after the target was lost or the entry
    // was evicted meanwhile: the image loads again.
    const PreviewImage* PreviewImageEntry() {
        for (auto it = previewImages_.begin(); it != previewImages_.end(); ++it) {
            if (it->key != preview_->imageKey) continue;
            std::rotate(it, it + 1, previewImages_.end());
            return &previewImages_.back();
        }
        if (preview_->pixels.empty()) {
            previewKey_.clear();
            SchedulePreview();
            preview_->status = L"Loading…";  // asked once; the reload replaces this content
            return nullptr;
        }
        PreviewImage image{preview_->imageKey, preview_->caption, nullptr};
        const HRESULT made = target_->CreateBitmap(D2D1::SizeU(preview_->pixelWidth, preview_->pixelHeight), preview_->pixels.data(),
            preview_->pixelWidth * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED), static_cast<float>(dpi_), static_cast<float>(dpi_)), &image.bitmap);
        std::vector<BYTE>().swap(preview_->pixels);
        if (FAILED(made)) {
            preview_->status = L"Can't show this image";  // not retried on every paint
            return nullptr;
        }
        if (previewImages_.size() >= 2) previewImages_.erase(previewImages_.begin());
        previewImages_.push_back(std::move(image));
        return &previewImages_.back();
    }

    // The thumbnail at the top of the body, centred, fitted to the card with
    // its aspect ratio kept and never past 1:1 at this DPI, with its caption below.
    void DrawPreviewImage(const D2D1_RECT_F& body) {
        const PreviewImage* entry = PreviewImageEntry();
        if (!entry) {
            if (!preview_->status.empty()) Text(preview_->status, body, hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        ID2D1Bitmap* bitmap = entry->bitmap.Get();
        const D2D1_SIZE_U pixels = bitmap->GetPixelSize();
        const float scale = 96.0f / static_cast<float>(dpi_);
        const auto fit = leanlauncher::preview::FitImage(pixels.width * scale, pixels.height * scale,
            body.right - body.left, body.bottom - body.top - 26);
        if (fit.width <= 0) return;
        const float left = std::round(body.left + (body.right - body.left - fit.width) / 2);
        const D2D1_RECT_F image = D2D1::RectF(left, body.top, left + fit.width, body.top + fit.height);
        target_->DrawBitmap(bitmap, image, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        Text(entry->caption, D2D1::RectF(body.left, image.bottom + 8, body.right, image.bottom + 26), hintFormat_.Get(),
            Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void ReleasePreviewLayout() {
        previewLayout_.Reset();
        previewAccent_.Reset();
        previewMuted_.Reset();
        previewLayoutWidth_ = previewMaxScroll_ = 0;
    }

    // Is the point over the open panel? Its clicks, hover and wheel belong to
    // the panel, never to a result row underneath (overlay) or beside it.
    bool PointInPreview(float x, float y) const {
        if (!PreviewVisible()) return false;
        const auto area = PreviewRect();
        return x >= area.left && x < area.right && y >= area.top && y < area.bottom;
    }

    void ScrollPreview(float delta) {
        if (!previewLayout_) return;
        const float scroll = std::clamp(previewScroll_ + delta, 0.0f, previewMaxScroll_);
        if (scroll == previewScroll_) return;
        previewScroll_ = scroll;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Builds the body layout once per content (and panel width): light
    // markdown for notes, monospace for Text files, only about the first 8 KB,
    // wrapping even inside long lines with no spaces.
    void EnsurePreviewLayout(float width) {
        namespace pv = leanlauncher::preview;
        if (previewLayout_ && previewLayoutWidth_ == width) return;
        ReleasePreviewLayout();
        if (!preview_ || !target_) return;
        const pv::ScannedText laid = pv::BodyForLayout(preview_->body, preview_->kind == pv::PreviewKind::Note, preview_->truncated);
        IDWriteTextFormat* format = preview_->kind == pv::PreviewKind::Text ? previewMonoFormat_.Get() : resultFormat_.Get();
        if (FAILED(writeFactory_->CreateTextLayout(laid.text.c_str(), static_cast<UINT32>(laid.text.size()), format,
                (std::max)(1.0f, width), 100000.0f, &previewLayout_))) return;
        previewLayoutWidth_ = width;
        previewLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
        // resultFormat_ is Medium for result names; body text reads better Normal.
        previewLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(laid.text.size())});
        for (const auto& span : laid.spans) {
            const DWRITE_TEXT_RANGE range{static_cast<UINT32>(span.start), static_cast<UINT32>(span.length)};
            switch (span.style) {
            case pv::SpanStyle::Heading1:
            case pv::SpanStyle::Heading2:
            case pv::SpanStyle::Heading3:
                previewLayout_->SetFontSize(span.style == pv::SpanStyle::Heading1 ? 20.0f
                    : span.style == pv::SpanStyle::Heading2 ? 17.0f : 15.0f, range);
                previewLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                break;
            case pv::SpanStyle::Bold: previewLayout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range); break;
            case pv::SpanStyle::Italic: previewLayout_->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range); break;
            case pv::SpanStyle::Code: previewLayout_->SetFontFamilyName(L"Consolas", range); break;
            case pv::SpanStyle::Link:
            case pv::SpanStyle::Tag:
            case pv::SpanStyle::Muted: {
                auto& brush = span.style == pv::SpanStyle::Muted ? previewMuted_ : previewAccent_;
                if (!brush) target_->CreateSolidColorBrush(span.style == pv::SpanStyle::Muted ? Muted()
                    : highContrast_ ? SystemColor(COLOR_HOTLIGHT) : D2D1::ColorF(0x7C9CFF), &brush);
                if (brush) previewLayout_->SetDrawingEffect(brush.Get(), range);
                break;
            }
            }
        }
    }

    // The panel: its own card fill (the overlay first paints the window
    // background so the results underneath don't show through), a title, the
    // property line, then the body clipped and scrolled, or a centred status.
    void DrawPreviewPanel() {
        if (!PreviewVisible()) return;
        const D2D1_RECT_F area = PreviewRect();
        D2D1_RECT_F card = area;
        if (previewOverlay_) {
            Fill(area, highContrast_ ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0x252527));  // square: no row shows at the corners
        } else {
            Line(area.left + 0.5f, area.top + 8, area.left + 0.5f, area.bottom - 8,
                highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.12f));
            card = D2D1::RectF(area.left + 8, area.top + 8, area.right - 8, area.bottom - 8);
        }
        Fill(card, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(1, 1, 1, 0.035f), 8);
        if (!preview_) return;
        const float left = card.left + 12, right = card.right - 12;
        float top = card.top + 10;
        Text(preview_->title, D2D1::RectF(left, top, right, top + 24), resultFormat_.Get(), Foreground());
        top += 24;
        if (!preview_->propertyLine.empty()) {
            // Long paths wrap at any character (they have no spaces to break on),
            // capped at 3 lines with an ellipsis; the body moves down to fit.
            auto layout = Layout(preview_->propertyLine, hintFormat_.Get(), right - left, 1024);
            if (layout) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                const UINT32 lines = (std::max)(1u, metrics.lineCount);
                const float lineHeight = (std::max)(18.0f, metrics.height / static_cast<float>(lines));
                const float height = lineHeight * static_cast<float>((std::min)(lines, 3u));
                layout->SetMaxHeight(height);
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ellipsis;
                writeFactory_->CreateEllipsisTrimmingSign(hintFormat_.Get(), &ellipsis);
                layout->SetTrimming(&trimming, ellipsis.Get());
                brush_->SetColor(Muted());
                target_->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                top += height;
            }
        }
        const D2D1_RECT_F body = D2D1::RectF(left, top + 8, right, card.bottom - 12);
        if (body.bottom <= body.top) return;
        if (!preview_->status.empty()) {
            Text(preview_->status, body, hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (preview_->kind == leanlauncher::preview::PreviewKind::Image) {
            DrawPreviewImage(body);
            return;
        }
        EnsurePreviewLayout(right - left);
        if (!previewLayout_) return;
        DWRITE_TEXT_METRICS metrics{};
        previewLayout_->GetMetrics(&metrics);
        previewBodyHeight_ = body.bottom - body.top;
        previewMaxScroll_ = (std::max)(0.0f, metrics.height - previewBodyHeight_);
        previewScroll_ = std::clamp(previewScroll_, 0.0f, previewMaxScroll_);
        target_->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
        brush_->SetColor(Foreground());
        target_->DrawTextLayout(D2D1::Point2F(body.left, body.top - previewScroll_), previewLayout_.Get(), brush_.Get());
        target_->PopAxisAlignedClip();
    }

    bool MatchesAdministratorHotkey(bool control, bool shift, bool alt) const {
        if (settings_.administratorHotkey.disabled) return false;
        uint16_t mods = 0;
        if (control) mods |= quicklaunch::kModControl;
        if (alt) mods |= quicklaunch::kModAlt;
        if (shift) mods |= quicklaunch::kModShift;
        return mods == settings_.administratorHotkey.modifiers;
    }

    bool MatchesQuickLaunchHotkey(bool control, bool alt, bool shift) const {
        if (settings_.quickLaunchHotkey.disabled) return false;
        uint16_t mods = 0;
        if (control) mods |= quicklaunch::kModControl;
        if (alt) mods |= quicklaunch::kModAlt;
        if (shift) mods |= quicklaunch::kModShift;
        return mods == settings_.quickLaunchHotkey.modifiers;
    }

    LRESULT HandleKeyDown(WPARAM key, LPARAM lParam) {
        if (ShouldShowHotkeyWarning()) {
            if (key == VK_ESCAPE) {
                hotkeyWarningDismissed_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (key == VK_RETURN || key == VK_SPACE) {
                OpenSettings();
                recordingRow_ = 0;
                return 0;
            }
            return 0;
        }
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (page_ == Page::Settings) {
            if (vaultDropdownOpen_) {
                if (key == VK_UP) {
                    const int count = static_cast<int>(knownVaults_.size());
                    if (count > 0) vaultDropdownHighlight_ = (vaultDropdownHighlight_ - 1 + count) % count;
                    EnsureVaultDropdownHighlightVisible();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_DOWN) {
                    const int count = static_cast<int>(knownVaults_.size());
                    if (count > 0) vaultDropdownHighlight_ = (vaultDropdownHighlight_ + 1) % count;
                    EnsureVaultDropdownHighlightVisible();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_RETURN) { SelectVaultDropdownItem(vaultDropdownHighlight_); return 0; }
                if (key == VK_ESCAPE) { CloseVaultDropdown(); return 0; }
                return 0;
            }
            if (webSearchDropdownOpen_) {
                const int itemCount = static_cast<int>(takeoff::kWebSearchPresetCount) + 1;
                if (key == VK_UP) {
                    webSearchDropdownHighlight_ = (webSearchDropdownHighlight_ - 1 + itemCount) % itemCount;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_DOWN) {
                    webSearchDropdownHighlight_ = (webSearchDropdownHighlight_ + 1) % itemCount;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_RETURN) { SelectWebSearchDropdownItem(webSearchDropdownHighlight_); return 0; }
                if (key == VK_ESCAPE) { CloseWebSearchDropdown(); return 0; }
                return 0;
            }
            if (quickOpenDropdownOpen_) {
                if (key == VK_UP) {
                    quickOpenDropdownHighlight_ =
                        (quickOpenDropdownHighlight_ - 1 + kQuickOpenTargetCount) % kQuickOpenTargetCount;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_DOWN) {
                    quickOpenDropdownHighlight_ = (quickOpenDropdownHighlight_ + 1) % kQuickOpenTargetCount;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_RETURN) { SelectQuickOpenDropdownItem(quickOpenDropdownHighlight_); return 0; }
                if (key == VK_ESCAPE) { CloseQuickOpenDropdown(); return 0; }
                return 0;
            }
            if (editingRow_ >= 0) {
                if (key == VK_RETURN) { CommitEditingRow(); return 0; }
                if (key == VK_ESCAPE) { CancelEditingRow(); return 0; }
                if (key == VK_LEFT) {
                    settingsEdit_.Move(false, shift, control);
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_RIGHT) {
                    settingsEdit_.Move(true, shift, control);
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_HOME) {
                    settingsEdit_.MoveTo(0, shift);
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_END) {
                    settingsEdit_.MoveTo(settingsEdit_.text.size(), shift);
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_BACK) {
                    settingsEdit_.Erase(true, control);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (key == VK_DELETE) {
                    settingsEdit_.Erase(false, control);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }
            if (recordingRow_ >= 0) {
                HandleRecordingKey(key, control, shift, alt);
                return 0;
            }
            if (key == VK_ESCAPE || (alt && key == VK_LEFT)) {
                CloseSettings();
            } else if (key == VK_UP || (key == VK_TAB && shift)) {
                settingsSelected_ = NextSettingsRow(settingsSelected_, -1);
                EnsureSettingsVisible(settingsSelected_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (key == VK_DOWN || key == VK_TAB) {
                settingsSelected_ = NextSettingsRow(settingsSelected_, 1);
                EnsureSettingsVisible(settingsSelected_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (key == VK_HOME) {
                settingsSelected_ = FirstRowInCategory(settingsCategory_);
                EnsureSettingsVisible(settingsSelected_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (key == VK_END) {
                settingsSelected_ = LastRowInCategory(settingsCategory_);
                EnsureSettingsVisible(settingsSelected_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (key == VK_PRIOR) {
                ScrollSettings(-kSettingsRowHeight * 2);
            } else if (key == VK_NEXT) {
                ScrollSettings(kSettingsRowHeight * 2);
            } else if (key == VK_LEFT || key == VK_RIGHT ||
                       key == VK_RETURN || key == VK_SPACE) {
                ChangeSetting(settingsSelected_);
            }
            return 0;
        }
        if (composing_ && key != VK_ESCAPE) {
            return DefWindowProcW(hwnd_, WM_KEYDOWN, key, lParam);
        }
        if (key == VK_ESCAPE) {
            if (actionsOpen_) {
                actionsOpen_ = false;
                actionsPositioned_ = false;
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (composing_) {
                if (HIMC context = ImmGetContext(hwnd_)) {
                    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    ImmReleaseContext(hwnd_, context);
                }
            } else if (!input_.text.empty()) {
                input_.Clear();
                OnQueryChanged();
            } else {
                Hide();
            }
            return 0;
        }
        if (MatchesActionsHotkey(key, control, shift, alt)) { ToggleActions(); return 0; }
        if (MatchesPreviewHotkey(key, control, shift, alt)) {
            // Held down, Ctrl+P toggles once: auto-repeats (lParam bit 30,
            // "key was already down") neither flap the width nor rewrite the registry.
            if ((lParam & 0x40000000) == 0) TogglePreview();
            return 0;
        }
        if (actionsOpen_) {
            if (key == VK_UP || key == VK_DOWN || key == VK_TAB) {
                actionSelected_ = (actionSelected_ + (key == VK_UP || (key == VK_TAB && shift) ? ActionCount() - 1 : 1)) % ActionCount();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (key == VK_RETURN) RunAction(actionSelected_);
            return 0;
        }
        if (control) {
            switch (key) {
            case 'A': input_.SelectAll(); ResetCaret(); return 0;
            case 'C':
                if (input_.HasSelection()) {
                    CopySelection(false);
                } else if (HasResult() && apps_[results_[selected_]].category == takeoff::AppCategory::Calculator) {
                    const bool copied = CopyText(apps_[results_[selected_]].path);
                    status_ = copied ? L"Result copied to clipboard" : L"Clipboard is busy. Try again.";
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            case 'X': CopySelection(true); return 0;
            case 'V': Paste(); return 0;
            case 'L': input_.SelectAll(); ResetCaret(); return 0;
            }
        }
        if (key >= '1' && key <= '8' && MatchesQuickLaunchHotkey(control, alt, shift)) {
            const int index = firstVisible_ + static_cast<int>(key - '1');
            if (index < static_cast<int>(results_.size())) { selected_ = index; LaunchSelected(); }
            return 0;
        }
        switch (key) {
        case VK_RETURN:
            if (results_.empty() && !takeoff::Normalize(input_.text).empty()) {
                if (!settings_.enableWebSearch) {
                    status_ = L"No results. Web search is disabled in settings.";
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                const bool allReady = indexReady_ &&
                    (!settings_.enableFileSearch || takeoff::FileIndex::Instance().IsReady());
                if (allReady) {
                    OpenWebSearch(input_.text);
                } else {
                    status_ = L"Still indexing\u2009—\u2009try again in a moment.";
                    ResetCaret();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            if (shift && !control && !alt && !MatchesAdministratorHotkey(control, shift, alt) && OpenTypedPath()) return 0;
            LaunchSelected(MatchesAdministratorHotkey(control, shift, alt)); return 0;
        case VK_UP: MoveSelection(-1, true); return 0;
        case VK_DOWN: MoveSelection(1, true); return 0;
        case VK_TAB:
            // US-043: in path mode Tab completes the selected entry; elsewhere
            // it keeps moving the selection.
            if (!shift && CompleteSelectedPath()) return 0;
            MoveSelection(shift ? -1 : 1, true);
            return 0;
        case VK_PRIOR:
        case VK_NEXT:
            // US-045: Shift+PgUp/PgDn scroll the preview; plain PgUp/PgDn keep
            // moving through the results.
            if (shift && PreviewVisible()) {
                ScrollPreview((key == VK_NEXT ? 1.0f : -1.0f) * (std::max)(24.0f, previewBodyHeight_ - 24));
                return 0;
            }
            MoveSelection(key == VK_NEXT ? visibleRows_ : -visibleRows_, false);
            return 0;
        case VK_LEFT: input_.Move(false, shift, control); ResetCaret(); return 0;
        case VK_RIGHT: input_.Move(true, shift, control); ResetCaret(); return 0;
        case VK_HOME: input_.MoveTo(0, shift); ResetCaret(); return 0;
        case VK_END: input_.MoveTo(input_.text.size(), shift); ResetCaret(); return 0;
        case VK_BACK: input_.Erase(true, control); OnQueryChanged(); return 0;
        case VK_DELETE: input_.Erase(false, control); OnQueryChanged(); return 0;
        }
        return DefWindowProcW(hwnd_, WM_KEYDOWN, key, lParam);
    }

    bool CopyText(const std::wstring& text) {
        if (!OpenClipboard(hwnd_)) return false;
        bool copied = false;
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
            }
            if (!copied) GlobalFree(memory);
        }
        CloseClipboard();
        return copied;
    }

    void CopySelection(bool cut) {
        if (!input_.HasSelection()) return;
        if (CopyText(input_.text.substr(input_.Start(), input_.End() - input_.Start())) && cut) {
            input_.Erase(true);
            OnQueryChanged();
        }
    }

    void Paste() {
        if (!OpenClipboard(hwnd_)) return;
        std::wstring value;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                const size_t maximum = (std::min)(static_cast<size_t>(GlobalSize(data) / sizeof(wchar_t)),
                    SearchInput::kLimit);
                size_t count = 0;
                while (count < maximum && text[count]) ++count;
                value.assign(text, count);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        value = leanlauncher::obsidian::PastedTextForInput(value);
        if (!value.empty()) { input_.Insert(value); OnQueryChanged(); }
    }

    void HandleComposition(LPARAM flags) {
        HIMC context = ImmGetContext(hwnd_);
        if (!context) return;
        auto read = [context](DWORD kind) {
            const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
            std::wstring text(bytes > 0 ? static_cast<size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
            if (bytes > 0) ImmGetCompositionStringW(context, kind, text.data(), static_cast<DWORD>(bytes));
            return text;
        };
        if (flags & GCS_RESULTSTR) {
            input_.Insert(read(GCS_RESULTSTR));
            composition_.clear();
            OnQueryChanged();
        }
        if (flags & GCS_COMPSTR) composition_ = read(GCS_COMPSTR);
        ImmReleaseContext(hwnd_, context);
        ResetCaret();
    }

    void PositionIme() {
        const int x = ToPixel(caretX_), y = ToPixel(44);
        SetCaretPos(x, ToPixel(20));
        if (HIMC context = ImmGetContext(hwnd_)) {
            COMPOSITIONFORM composition{CFS_POINT, {x, y}, {}};
            ImmSetCompositionWindow(context, &composition);
            CANDIDATEFORM candidate{0, CFS_CANDIDATEPOS, {x, y}, {}};
            ImmSetCandidateWindow(context, &candidate);
            ImmReleaseContext(hwnd_, context);
        }
    }

    // Where a capture action writes (US-025): its target note if one is set,
    // otherwise today's daily note. Target refs are normalized on save/load,
    // so they're trusted here.
    std::wstring CaptureTargetPath(const std::wstring& targetRef) const {
        if (!targetRef.empty()) {
            return leanlauncher::obsidian::ResolveNoteAbsolutePath(obsidianVaultPath_, targetRef);
        }
        int year = 0, month = 0, day = 0;
        leanlauncher::obsidian::GetTodayYmd(year, month, day);
        return leanlauncher::obsidian::ResolveTodayPath(dailyNoteConfig_, obsidianVaultPath_, year, month, day);
    }

    // Preview-row suffix naming a non-default destination, e.g. " \u2192 Inbox/Tasks".
    static std::wstring CaptureTargetSuffix(const std::wstring& targetRef) {
        return targetRef.empty() ? std::wstring() : L" \u2192 " + targetRef;
    }

    bool HasResult() const { return selected_ >= 0 && selected_ < static_cast<int>(results_.size()); }

    void LaunchSelected(bool asAdministrator = false) {
        if (!HasResult()) return;
        const size_t index = results_[selected_];
        const AppEntry& app = apps_[index];
        if (app.inert) return;  // informational row, e.g. a quick-open note that doesn't exist yet
        if (app.category == takeoff::AppCategory::Command) {
            // US-041: every route (Enter, click, Ctrl+Enter, Alt+1-8, Ctrl+K)
            // lands here, so none can skip the second-Enter confirmation.
            namespace sc = leanlauncher::syscmd;
            const auto command = sc::CommandFromPath(app.path);
            if (!command || !settings_.enableSystemCommands) return;
            if (*command == sc::Command::EmptyRecycleBin && recycleBinItems_ == 0) return;
            if (commandGate_.Press(*command, GetTickCount64()) == sc::PressResult::AskAgain) {
                SetTimer(hwnd_, kCommandConfirmTimer, static_cast<UINT>(sc::kConfirmTimeoutMs), nullptr);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            Hide();
            if (!RunSystemCommand(*command)) {
                Show();
                status_ = L"Could not run this command.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::Calculator) {
            CopyText(app.path);
            Hide();
            return;
        }
        if (app.category == takeoff::AppCategory::TaskAdd) {
            if (app.path.empty()) {
                // No vault configured yet - open Settings instead of silently
                // doing nothing (spec: result row reads "Set up your vault in
                // Settings" and must not add anything when activated). Land
                // directly on the Vault category/row so onboarding doesn't
                // strand the user on an unrelated settings tab.
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendTask(app.path, app.parameters);
            if (!ok) {
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
                SetFocus(hwnd_);
                status_ = L"Could not write to your vault. Check Settings.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendNoteText(app.path, app.parameters);
            if (!ok) {
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
                SetFocus(hwnd_);
                status_ = L"Could not write to your vault. Check Settings.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::LogAdd) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            const bool ok = leanlauncher::obsidian::AppendLogEntry(app.path, app.parameters, settings_.logHeading);
            if (!ok) {
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
                SetFocus(hwnd_);
                status_ = L"Could not write to your vault. Check Settings.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::WebSearch) {
            OpenWebSearch(app.parameters);
            return;
        }
        if (app.category == takeoff::AppCategory::Pomodoro) {
            RunPomodoroRow(app);
            return;
        }
        if (app.category == takeoff::AppCategory::Snippet) {
            RunSnippetRow(app);
            return;
        }
        if (app.category == takeoff::AppCategory::Url) {
            // Re-check the scheme: only http(s) is ever handed to the shell.
            if (leanlauncher::typed::ClassifyUrl(app.path) != leanlauncher::typed::UrlKind::Explicit) return;
            Hide();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app.path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL)) <= 32) {
                Show();
                status_ = L"Could not open this URL.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::NoteJump) {
            if (app.path.empty()) {
                OpenSettings(SettingsCategory::Obsidian);
                return;
            }
            Hide();
            // The CLI can take seconds to answer on a cold Obsidian start, so
            // the spawn runs on a detached worker that only calls
            // OpenNoteInObsidian and posts its verdict back. Blocking here
            // would stall the message pump, and with it the global hotkey and
            // the tray icon.
            const HWND hwnd = hwnd_;
            const std::wstring vaultPath = obsidianVaultPath_;
            const std::wstring noteRef = app.path;
            try {
                std::thread([hwnd, vaultPath, noteRef] {
                    const bool opened = leanlauncher::obsidian::OpenNoteInObsidian(vaultPath, noteRef);
                    PostMessageW(hwnd, leanlauncher::obsidian::kNoteOpenResultMessage,
                        opened ? 1 : 0, 0);
                }).detach();
            } catch (const std::system_error&) {
                PostMessageW(hwnd, leanlauncher::obsidian::kNoteOpenResultMessage, 0, 0);
            }
            return;
        }
        const std::wstring& path = app.path;
        Hide();
        const bool isProtocol = path.rfind(L"ms-settings:", 0) == 0 || path.rfind(L"shell:", 0) == 0;
        const bool isCpl = (path.size() >= 4 &&
            (_wcsicmp(path.c_str() + path.size() - 4, L".cpl") == 0));

        const wchar_t* fileToExec = isCpl ? L"control.exe" : path.c_str();
        const std::wstring paramsStr = isCpl
            ? (!app.parameters.empty() ? app.parameters : (L"\"" + path + L"\""))
            : app.parameters;
        const wchar_t* params = paramsStr.empty() ? nullptr : paramsStr.c_str();

        std::wstring workingDir;
        if (!isProtocol) {
            // System executables live under %SystemRoot% (e.g. powershell.exe in
            // System32\WindowsPowerShell\v1.0). Using their folder as working dir
            // drops the user in System32. Start those in %USERPROFILE% instead so
            // shells behave like a normal Start Menu / Terminal launch.
            bool isSystemPath = false;
            wchar_t winDirBuf[MAX_PATH]{};
            if (GetWindowsDirectoryW(winDirBuf, MAX_PATH) > 0) {
                std::wstring winStr(winDirBuf);
                if (path.size() >= winStr.size() &&
                    _wcsnicmp(path.c_str(), winStr.c_str(), winStr.size()) == 0 &&
                    (path.size() == winStr.size() || path[winStr.size()] == L'\\')) {
                    isSystemPath = true;
                }
            }
            if (isSystemPath) {
                workingDir = ExpandEnv(L"%USERPROFILE%");
                if (workingDir.empty()) {
                    PWSTR profile = nullptr;
                    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT,
                            nullptr, &profile)) && profile) {
                        workingDir = profile;
                        CoTaskMemFree(profile);
                    }
                }
            } else {
                std::error_code ec;
                fs::path p(path);
                if (p.has_parent_path()) {
                    workingDir = p.parent_path().wstring();
                }
            }
        }
        const wchar_t* dir = workingDir.empty() ? nullptr : workingDir.c_str();

        INT_PTR result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, (asAdministrator && !isProtocol) ? L"runas" : L"open",
                fileToExec, params, dir, SW_SHOWNORMAL));
        if (result <= 32 && asAdministrator && !isProtocol) {
            result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", fileToExec, params, dir, SW_SHOWNORMAL));
        }

        if (result <= 32) {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            status_ = L"Could not open this item. Try another result.";
            ResetCaret();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            // Only Application/System launches update the recent-apps list -
            // opening a file/folder/other result must not evict a real
            // recently-used app from it (this used to run unconditionally
            // above the category check, making the guard below a no-op).
            if (app.category == takeoff::AppCategory::Application ||
                app.category == takeoff::AppCategory::System) {
                recent_.erase(std::remove(recent_.begin(), recent_.end(), index), recent_.end());
                recent_.insert(recent_.begin(), index);
                if (recent_.size() > 8) recent_.resize(8);
                SaveRecent();
            }
        }
    }

    // Result-row title for a system command: the confirmation prompt while
    // it's pending, the Recycle Bin's contents once known; otherwise the name.
    std::wstring CommandRowText(const AppEntry& app) const {
        namespace sc = leanlauncher::syscmd;
        if (app.category != takeoff::AppCategory::Command) return app.name;
        const auto command = sc::CommandFromPath(app.path);
        if (!command) return app.name;
        if (commandGate_.IsPending(*command, GetTickCount64())) return sc::ConfirmPrompt(*command);
        if (*command == sc::Command::EmptyRecycleBin && recycleBinItems_ >= 0) {
            if (recycleBinItems_ == 0) return sc::FormatRecycleBinSummary(0, 0);
            return app.name + L" (" + sc::FormatRecycleBinSummary(recycleBinItems_, recycleBinBytes_) + L")";
        }
        return app.name;
    }

    // The Empty Recycle Bin row is greyed out (and does nothing) when the bin is empty.
    bool CommandRowDimmed(const AppEntry& app) const {
        return app.category == takeoff::AppCategory::Command && recycleBinItems_ == 0 &&
            app.path == leanlauncher::syscmd::CommandPath(leanlauncher::syscmd::Command::EmptyRecycleBin);
    }

    void CancelCommandConfirm() {
        if (!commandGate_.Pending()) return;
        commandGate_.Cancel();
        KillTimer(hwnd_, kCommandConfirmTimer);
    }

    // US-041. Restart and shut down go through shutdown.exe without /f, so no
    // privilege-raising code lives in this unsigned exe and apps can still
    // ask to save work.
    static bool RunSystemCommand(leanlauncher::syscmd::Command command) {
        using leanlauncher::syscmd::Command;
        switch (command) {
        case Command::Lock:
            return LockWorkStation() != FALSE;
        case Command::Sleep:
            return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
        case Command::Hibernate:
            return SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
        case Command::Restart:
        case Command::ShutDown: {
            wchar_t systemDirectory[MAX_PATH]{};
            if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
            const std::wstring exe = std::wstring(systemDirectory) + L"\\shutdown.exe";
            const std::wstring args = leanlauncher::syscmd::ShutdownArguments(command);
            return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(),
                nullptr, SW_HIDE)) > 32;
        }
        case Command::SignOut:
            return ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED) != FALSE;
        case Command::EmptyRecycleBin:
            return SUCCEEDED(SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION));
        }
        return false;
    }

    // Fetches the Recycle Bin's item count and size off the UI thread the
    // first time its row is visible after the launcher shows (it can take a
    // moment with several drives).
    void RequestRecycleBinInfo() {
        if (recycleBinItems_ >= 0 || recycleBinQueryPending_) return;
        recycleBinQueryPending_ = true;
        const HWND hwnd = hwnd_;
        try {
            std::thread([hwnd] {
                SHQUERYRBINFO info{};
                info.cbSize = sizeof(info);
                if (FAILED(SHQueryRecycleBinW(nullptr, &info))) info.i64NumItems = info.i64Size = 0;
                PostMessageW(hwnd, kRecycleBinInfoMessage, static_cast<WPARAM>(info.i64NumItems),
                    static_cast<LPARAM>(info.i64Size));
            }).detach();
        } catch (const std::system_error&) {
            recycleBinQueryPending_ = false;
        }
    }

    bool OpenWebSearch(std::wstring_view query) {
        if (query.empty()) return false;
        const std::wstring url = takeoff::BuildSearchUrl(settings_.webSearchUrlTemplate, query);
        Hide();
        const INT_PTR result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            status_ = L"Could not open your browser. Try another query.";
            ResetCaret();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return false;
        }
        return true;
    }

    void ToggleActions() {
        if (!HasResult() || apps_[results_[selected_]].inert) return;
        actionsOpen_ = !actionsOpen_;
        actionsPositioned_ = false;
        actionSelected_ = 0;
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OpenActionsAt(float x, float y) {
        if (!HasResult() || apps_[results_[selected_]].inert) return;
        actionsOpen_ = true;
        actionsX_ = x;
        actionsY_ = y;
        actionsPositioned_ = true;
        actionSelected_ = 0;
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RunAction(int action) {
        if (!HasResult()) return;
        actionsOpen_ = false;
        actionsPositioned_ = false;
        const AppEntry& app = apps_[results_[selected_]];
        if (app.category == takeoff::AppCategory::Command || app.category == takeoff::AppCategory::Pomodoro ||
            app.category == takeoff::AppCategory::Snippet) {
            LaunchSelected(false);  // "Run", through the same confirmation
            return;
        }
        if (app.category == takeoff::AppCategory::Calculator) {
            if (action == 0) {
                CopyText(app.path);
                Hide();
                return;
            } else if (action == 1) {
                const std::wstring calc = app.parameters + L" = " + app.name;
                CopyText(calc);
                Hide();
                return;
            } else if (action == 2) {
                ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr, SW_SHOWNORMAL);
                Hide();
                return;
            }
        }
        if (app.category == takeoff::AppCategory::TaskAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Task text copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                if (!app.path.empty()) {
                    ShellExecuteW(nullptr, L"open", app.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                Hide();
                return;
            }
        }
        if (app.category == takeoff::AppCategory::NoteAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Text copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                if (!app.path.empty()) {
                    ShellExecuteW(nullptr, L"open", app.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                Hide();
                return;
            }
        }
        if (app.category == takeoff::AppCategory::LogAdd) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Text copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                if (!app.path.empty()) {
                    ShellExecuteW(nullptr, L"open", app.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                Hide();
                return;
            }
        }
        if (app.category == takeoff::AppCategory::Url) {
            if (action == 0) {
                LaunchSelected(false);
            } else {
                const bool copied = CopyText(app.path);
                status_ = copied ? L"URL copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (app.category == takeoff::AppCategory::WebSearch) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Query text copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                const std::wstring url = takeoff::BuildSearchUrl(settings_.webSearchUrlTemplate, app.parameters);
                const bool copied = CopyText(url);
                status_ = copied ? L"Search URL copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        if (app.category == takeoff::AppCategory::NoteJump) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                const bool copied = CopyText(app.parameters);
                status_ = copied ? L"Note title copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 2) {
                if (!app.path.empty()) {
                    const std::wstring absPath =
                        leanlauncher::obsidian::ResolveNoteAbsolutePath(obsidianVaultPath_, app.path);
                    std::wstring param = L"/select,\"" + absPath + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                }
                Hide();
                return;
            }
        }
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        if (isFileOrFolder) {
            if (action == 0) {
                LaunchSelected(false);
                return;
            } else if (action == 1) {
                std::wstring param = L"/select,\"" + app.path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                Hide();
                return;
            } else if (action == 2) {
                const bool copied = CopyText(app.path);
                status_ = copied ? L"File path copied" : L"Clipboard is busy. Try again.";
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            } else if (action == 3) {
                TogglePinSelected();
                return;
            }
        }
        if (action == 3 && IsPinnable(app)) { TogglePinSelected(); return; }
        if (action == 0) { LaunchSelected(true); return; }
        const bool copied = CopyText(action == 1 ? app.name : app.path);
        status_ = copied ? (action == 1 ? L"App name copied" : L"Launch path copied")
                         : L"Clipboard is busy. Try again.";
        ResetCaret();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 3 actions for most rows; pinnable rows (apps, files, folders) add
    // Pin/Unpin as a 4th (US-024).
    int ActionCount() const {
        if (HasResult() && apps_[results_[selected_]].category == takeoff::AppCategory::Command) return 1;
        if (HasResult() && apps_[results_[selected_]].category == takeoff::AppCategory::Pomodoro) return 1;
        if (HasResult() && apps_[results_[selected_]].category == takeoff::AppCategory::Snippet) return 1;
        if (HasResult() && apps_[results_[selected_]].category == takeoff::AppCategory::Url) return 2;
        return (HasResult() && IsPinnable(apps_[results_[selected_]])) ? 4 : 3;
    }

    D2D1_RECT_F ActionsRect(float x, float y) const {
        constexpr float kActionsWidth = 276.0f;
        const float kActionsHeight = 32.0f + ActionCount() * 36.0f + 12.0f;
        const float minLeft = 8.0f;
        const float maxLeft = (std::max)(minLeft, width_ - kActionsWidth - 8.0f);
        const float minTop = 8.0f;
        const float maxTop = (std::max)(minTop, FooterTop() - kActionsHeight - 8.0f);
        const float left = std::clamp(x, minLeft, maxLeft);
        const float top = std::clamp(y, minTop, maxTop);
        return D2D1::RectF(left, top, left + kActionsWidth, top + kActionsHeight);
    }

    D2D1_RECT_F ActionsRect() const {
        return ActionsRect(actionsPositioned_ ? actionsX_ : width_ - 288.0f,
            actionsPositioned_ ? actionsY_ : FooterTop() - 160.0f);
    }

    int ResultAtPoint(float x, float y) const {
        if (page_ != Page::Launcher || PointInPreview(x, y)) return -1;
        if (x < 8 || x > width_ - 12 || y < ResultsTop() || y >= FooterTop() - 8) return -1;
        float top = ResultsTop();
        for (int i = firstVisible_; i < static_cast<int>(results_.size()); ++i) {
            const float h = RowHeight(i);
            if (top + h > FooterTop()) break;
            if (y >= top && y < top + h) {
                return i;
            }
            top += h;
        }
        return -1;
    }

    // Same hit test as ResultAtPoint, expressed relative to the first visible row.
    int ResultSlotAtPoint(float x, float y) const {
        const int result = ResultAtPoint(x, y);
        return result < 0 ? -1 : result - firstVisible_;
    }

    void HandleClick(float x, float y) {
        SetFocus(hwnd_);
        hoverLockRow_ = -1;
        if (ShouldShowHotkeyWarning()) {
            if (PointInHotkeyWarningSettings(x, y)) {
                OpenSettings();
                recordingRow_ = 0;
            } else if (PointInHotkeyWarningDismiss(x, y) || !PointInHotkeyWarningCard(x, y)) {
                hotkeyWarningDismissed_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (page_ == Page::Settings) {
            // Close-on-click-away for the vault dropdown, checked before any
            // other Settings click handling so every zone (header gaps,
            // footer, scrollbar strip, rows) is covered by one path instead
            // of needing its own close call.
            if (vaultDropdownOpen_) {
                const int clickedItem = VaultDropdownItemAtPoint(x, y);
                if (clickedItem >= 0) {
                    SelectVaultDropdownItem(clickedItem);
                    return;
                }
                const bool clickedTrigger = (SettingsRowAtPoint(x, y) == kRowVaultPicker);
                CloseVaultDropdown();
                if (clickedTrigger) {
                    // Clicking the row that opened the dropdown just closes
                    // it - falling through would immediately reopen it via
                    // ChangeSetting(kRowVaultPicker) further down.
                    settingsSelected_ = kRowVaultPicker;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                // Click landed elsewhere (header, footer, scrollbar, another
                // row, or empty space) - fall through so that click still
                // does its normal thing, same as how an in-progress text
                // edit is cancelled but the click that cancelled it still
                // proceeds.
            }
            // Close-on-click-away for the web search engine dropdown - mirrors
            // the vault dropdown block above.
            if (webSearchDropdownOpen_) {
                const int clickedItem = WebSearchDropdownItemAtPoint(x, y);
                if (clickedItem >= 0) {
                    SelectWebSearchDropdownItem(clickedItem);
                    return;
                }
                const bool clickedTrigger = (SettingsRowAtPoint(x, y) == kRowWebSearchEngine);
                CloseWebSearchDropdown();
                if (clickedTrigger) {
                    settingsSelected_ = kRowWebSearchEngine;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            // Same close-on-click-away behavior for the Quick open picker.
            if (quickOpenDropdownOpen_) {
                const int clickedItem = QuickOpenDropdownItemAtPoint(x, y);
                if (clickedItem >= 0) {
                    SelectQuickOpenDropdownItem(clickedItem);
                    return;
                }
                const bool clickedTrigger = (SettingsRowAtPoint(x, y) == kRowQuickOpenTarget);
                CloseQuickOpenDropdown();
                if (clickedTrigger) {
                    settingsSelected_ = kRowQuickOpenTarget;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            if (y < kSettingsHeaderHeight) {
                if (x < 46.0f) {
                    CloseSettings();
                    return;
                }
                for (auto cat : kSettingsCategories) {
                    const auto r = CategoryTabRect(cat);
                    if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                        if (editingRow_ >= 0) CancelEditingRow();
                        obsidianExpandedSection_ = -1;
                        settingsCategory_ = cat;
                        settingsScroll_ = 0.0f;
                        if (!IsRowInCategory(settingsSelected_, settingsCategory_)) {
                            settingsSelected_ = FirstRowInCategory(settingsCategory_);
                        }
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return;
                    }
                }
                const auto resetRect = ResetButtonRect();
                if (x >= resetRect.left && x <= resetRect.right && y >= resetRect.top && y <= resetRect.bottom) {
                    settingsSelected_ = kRowResetToDefaults;
                    ResetToDefaults();
                    return;
                }
                return;
            }
            if (y >= FooterTop()) {
                return;
            }
            // Scrollbar track/thumb click & drag
            if (x >= width_ - 14.0f) {
                const float maxScroll = SettingsMaxScroll();
                if (maxScroll > 0.0f) {
                    const float trackTop = kSettingsHeaderHeight + 6.0f;
                    const float trackBottom = FooterTop() - 6.0f;
                    if (y >= trackTop && y <= trackBottom) {
                        const float progress = (y - trackTop) / (trackBottom - trackTop);
                        settingsScroll_ = std::clamp(progress * maxScroll, 0.0f, maxScroll);
                        settingsDraggingScroll_ = true;
                        SetCapture(hwnd_);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                return;
            }

            const int row = SettingsRowAtPoint(x, y);
            if (row >= 0) {
                if (recordingRow_ >= 0 && row != recordingRow_) {
                    recordingRow_ = -1;
                    settingsStatus_.clear();
                }
                if (editingRow_ >= 0 && row != editingRow_) {
                    CancelEditingRow();
                }
                settingsSelected_ = row;
                ChangeSetting(row);
            } else if (recordingRow_ >= 0) {
                recordingRow_ = -1;
                settingsStatus_.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (editingRow_ >= 0) {
                CancelEditingRow();
            }
            return;
        }
        if (actionsOpen_) {
            const auto rect = ActionsRect();
            if (x >= rect.left && x <= rect.right && y >= rect.top + 32 && y < rect.bottom - 6) {
                RunAction(std::clamp(static_cast<int>((y - rect.top - 32) / 36), 0, ActionCount() - 1));
            } else if (PointInAdminAction(x, y)) {
                actionsOpen_ = false;
                actionsPositioned_ = false;
                LaunchSelected(true);
            } else {
                actionsOpen_ = false;
                ResetCaret();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (PointInPreview(x, y)) return;  // US-045: the panel isn't a button
        if (y < kSearchHeight) {
            if (x > width_ - 84 && x < width_ - 52 && !input_.text.empty()) {
                input_.Clear();
                OnQueryChanged();
            } else if (x >= width_ - 52) {
                OpenSettings();
            } else {
                PlaceCaret(x, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                dragging_ = true;
                SetCapture(hwnd_);
            }
        } else if (y >= FooterTop()) {
            if (PointInUpdateIndicator(x, y)) {
                if (updateDownloaded_) {
                    RestartToUpdate();
                } else {
                    ShellExecuteW(nullptr, L"open", releasesUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return;
            }
            if (x >= width_ / 2) {
                ToggleActions();
            } else if (HasResult()) {
                const AppEntry& app = apps_[results_[selected_]];
                if (app.category == takeoff::AppCategory::Calculator) {
                    CopyText(app.path);
                    Hide();
                } else if (app.category == takeoff::AppCategory::TaskAdd ||
                           app.category == takeoff::AppCategory::NoteAdd ||
                           app.category == takeoff::AppCategory::LogAdd) {
                    LaunchSelected(false);
                } else if (!settings_.administratorHotkey.disabled) {
                    LaunchSelected(true);
                }
            }
        } else if (const int result = ResultAtPoint(x, y); result >= 0) {
            selected_ = result;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            LaunchSelected(MatchesAdministratorHotkey(control, shift, alt));
        } else if (PointInWebSearchCard(x, y)) {
            OpenWebSearch(input_.text);
        }
    }

    void HandleMiddleClick(float x, float y) {
        SetFocus(hwnd_);
        hoverLockRow_ = -1;
        if (page_ != Page::Launcher) return;
        if (actionsOpen_) {
            actionsOpen_ = false;
            actionsPositioned_ = false;
        }
        if (const int result = ResultAtPoint(x, y); result >= 0) {
            selected_ = result;
            LaunchSelected(true);
        } else if (y >= FooterTop() && x < width_ / 2 && !PointInPreview(x, y)) {
            LaunchSelected(true);
        }
    }

    void HandleRightClick(float x, float y) {
        SetFocus(hwnd_);
        hoverLockRow_ = -1;
        if (actionsOpen_) {
            const auto rect = ActionsRect();
            if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) return;
            actionsOpen_ = false;
            actionsPositioned_ = false;
        }
        if (const int result = ResultAtPoint(x, y); result >= 0) {
            selected_ = result;
            SchedulePreview();
            OpenActionsAt(x, y);
        } else {
            ResetCaret();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void HandleMouseMove(float x, float y) {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            trackingMouse_ = true;
        }
        if (dragging_) { PlaceCaret(x, true); return; }
        if (ShouldShowHotkeyWarning()) {
            if (mouseKnown_) {
                const bool prevHover = PointInHotkeyWarningSettings(mouseX_, mouseY_) || PointInHotkeyWarningDismiss(mouseX_, mouseY_);
                const bool newHover = PointInHotkeyWarningSettings(x, y) || PointInHotkeyWarningDismiss(x, y);
                if (prevHover != newHover) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            mouseX_ = x; mouseY_ = y; mouseKnown_ = true;
            return;
        }
        if (page_ == Page::Settings) {
            if (settingsDraggingScroll_) {
                const float maxScroll = SettingsMaxScroll();
                if (maxScroll > 0.0f) {
                    const float trackTop = kSettingsHeaderHeight + 6.0f;
                    const float trackBottom = FooterTop() - 6.0f;
                    const float progress = std::clamp((y - trackTop) / (trackBottom - trackTop), 0.0f, 1.0f);
                    const float newScroll = progress * maxScroll;
                    if (newScroll != settingsScroll_) {
                        settingsScroll_ = newScroll;
                        if (vaultDropdownOpen_) CloseVaultDropdown();
                        if (webSearchDropdownOpen_) CloseWebSearchDropdown();
                        if (quickOpenDropdownOpen_) CloseQuickOpenDropdown();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                return;
            }
            if (vaultDropdownOpen_) {
                const int hoveredItem = VaultDropdownItemAtPoint(x, y);
                if (hoveredItem >= 0 && hoveredItem != vaultDropdownHighlight_) {
                    vaultDropdownHighlight_ = hoveredItem;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
            if (webSearchDropdownOpen_) {
                const int hoveredItem = WebSearchDropdownItemAtPoint(x, y);
                if (hoveredItem >= 0 && hoveredItem != webSearchDropdownHighlight_) {
                    webSearchDropdownHighlight_ = hoveredItem;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
            if (quickOpenDropdownOpen_) {
                const int hoveredItem = QuickOpenDropdownItemAtPoint(x, y);
                if (hoveredItem >= 0 && hoveredItem != quickOpenDropdownHighlight_) {
                    quickOpenDropdownHighlight_ = hoveredItem;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
            const auto resetRect = ResetButtonRect();
            int row = -1;
            if (y < kSettingsHeaderHeight) {
                if (x >= resetRect.left && x <= resetRect.right && y >= resetRect.top && y <= resetRect.bottom) {
                    row = kRowResetToDefaults;
                }
            } else if (y >= kSettingsHeaderHeight && y < FooterTop() && x < width_ - 14.0f) {
                row = SettingsRowAtPoint(x, y);
            }
            if (row >= 0 && row != settingsSelected_) {
                settingsSelected_ = row;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        // Ignore synthetic moves caused by showing/repainting under a stationary pointer.
        if (mouseKnown_ && std::abs(x - mouseX_) < 1 && std::abs(y - mouseY_) < 1) return;
        mouseX_ = x; mouseY_ = y;
        if (!mouseKnown_) { mouseKnown_ = true; return; }
        if (updateAvailable_) {
            const bool hovered = PointInUpdateIndicator(x, y);
            if (hovered != updateHovered_) {
                updateHovered_ = hovered;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (page_ == Page::Launcher && results_.empty() && settings_.enableWebSearch) {
            const bool hovered = PointInWebSearchCard(x, y);
            if (hovered != webSearchCardHovered_) {
                webSearchCardHovered_ = hovered;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (page_ == Page::Launcher && HasResult()) {
            const bool hovered = PointInAdminAction(x, y);
            if (hovered != adminActionHovered_) {
                adminActionHovered_ = hovered;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (actionsOpen_) {
            const auto rect = ActionsRect();
            if (x >= rect.left && x <= rect.right && y >= rect.top + 32 && y < rect.bottom - 6) {
                actionSelected_ = std::clamp(static_cast<int>((y - rect.top - 32) / 36), 0, ActionCount() - 1);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else if (const int result = ResultAtPoint(x, y); result >= 0 && result != selected_) {
            if (hoverLockRow_ >= 0) {
                // The wheel/keyboard just chose this selection; the pointer
                // only takes over once it travels to a different row.
                if (result - firstVisible_ == hoverLockRow_) return;
                hoverLockRow_ = -1;
            }
            selected_ = result;
            InvalidateRect(hwnd_, nullptr, FALSE);
            SchedulePreview();
        }
    }

    bool CreateFormat(float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& format,
        const wchar_t* family = L"Segoe UI") {
        if (FAILED(writeFactory_->CreateTextFormat(family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return false;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    ComPtr<IDWriteTextLayout> Layout(std::wstring_view text, IDWriteTextFormat* format,
        float width, float height = 64) const {
        ComPtr<IDWriteTextLayout> layout;
        writeFactory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
            (std::max)(1.0f, width), height, &layout);
        return layout;
    }

    void PlaceCaret(float x, bool selecting) {
        auto layout = Layout(input_.text, searchFormat_.Get(), 32768);
        if (layout) {
            BOOL trailing = FALSE, inside = FALSE;
            DWRITE_HIT_TEST_METRICS hit{};
            layout->HitTestPoint(x - kTextLeft + textScroll_, 10, &trailing, &inside, &hit);
            input_.MoveTo(static_cast<size_t>(hit.textPosition) + (trailing ? hit.length : 0), selecting);
        }
        ResetCaret();
    }

    void PrepareVisibleIcons() {
        if (!iconThread_.joinable()) return;
        // Shell icon extraction (including packaged-app icons) can take tens of
        // milliseconds per item, so it must never run on the UI thread. Queue
        // the visible rows; results arrive via kIconReadyMessage and repaint.
        if (iconCache_.size() > 512) EvictIconCache();
        const UINT size = static_cast<UINT>((std::max)(1, ToPixel(26)));
        const int end = (std::min)(firstVisible_ + visibleRows_, static_cast<int>(results_.size()));
        for (int i = firstVisible_; i < end; ++i) {
            const AppEntry& app = apps_[results_[i]];
            if (app.category == takeoff::AppCategory::Url) continue;  // a web address has no file icon
            if (app.category == takeoff::AppCategory::Info) continue;  // status text only
            if (app.category == takeoff::AppCategory::Pomodoro) continue;
            if (app.category == takeoff::AppCategory::Snippet) continue;
            if (app.category == takeoff::AppCategory::Command) {
                if (app.path == leanlauncher::syscmd::CommandPath(leanlauncher::syscmd::Command::EmptyRecycleBin)) {
                    RequestRecycleBinInfo();
                }
                continue;  // no file to take an icon from
            }
            const std::wstring& lookupPath = !app.iconPath.empty() ? app.iconPath : app.path;
            if (iconCache_.find(lookupPath) != iconCache_.end() || iconPending_.count(lookupPath)) continue;
            iconPending_.insert(lookupPath);
            {
                std::lock_guard<std::mutex> lock(iconMutex_);
                iconQueue_.push_back({lookupPath, size, dpi_});
            }
            iconCv_.notify_one();
        }
    }

    void EvictIconCache() {
        // Cached icons are pre-scaled and tiny, so this only guards against
        // pathological growth. Keep what is on screen or already in flight.
        std::unordered_set<std::wstring> keep;
        const int end = (std::min)(firstVisible_ + visibleRows_, static_cast<int>(results_.size()));
        for (int i = firstVisible_; i < end; ++i) {
            const AppEntry& app = apps_[results_[i]];
            keep.insert(!app.iconPath.empty() ? app.iconPath : app.path);
        }
        for (auto it = iconCache_.begin(); it != iconCache_.end();) {
            if (keep.count(it->first) || iconPending_.count(it->first)) ++it;
            else it = iconCache_.erase(it);
        }
    }

    void IconWorkerMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ComPtr<IWICImagingFactory> wic;
        if (SUCCEEDED(com)) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wic));
        }
        for (;;) {
            IconRequest request;
            {
                std::unique_lock<std::mutex> lock(iconMutex_);
                iconCv_.wait(lock, [&] { return iconStop_ || !iconQueue_.empty(); });
                if (iconStop_) break;
                request = std::move(iconQueue_.front());
                iconQueue_.pop_front();
            }
            auto result = std::make_unique<IconResult>();
            result->path = std::move(request.path);
            result->size = request.size;
            if (wic) result->source = LoadIconSource(wic.Get(), result->path, request.size, request.dpi);
            if (PostMessageW(hwnd_, kIconReadyMessage, 0, reinterpret_cast<LPARAM>(result.get()))) {
                result.release();
            } else {
                break; // The window is gone; shutdown is in progress.
            }
        }
        wic.Reset();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void TriggerAppReindex() {
        if (indexTriggerEvent_) {
            SetEvent(indexTriggerEvent_);
        }
    }

    void IndexWorkerMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        HANDLE hUserStartMenu = nullptr;
        HANDLE hCommonStartMenu = nullptr;
        HANDLE hDesktop = nullptr;

        if constexpr (!kUiTest) {
            PWSTR userStartMenuPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_StartMenu, KF_FLAG_DEFAULT, nullptr, &userStartMenuPath))) {
                hUserStartMenu = FindFirstChangeNotificationW(
                    userStartMenuPath, TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
                CoTaskMemFree(userStartMenuPath);
            }
            PWSTR commonStartMenuPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonStartMenu, KF_FLAG_DEFAULT, nullptr, &commonStartMenuPath))) {
                hCommonStartMenu = FindFirstChangeNotificationW(
                    commonStartMenuPath, TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
                CoTaskMemFree(commonStartMenuPath);
            }
            PWSTR desktopPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktopPath))) {
                hDesktop = FindFirstChangeNotificationW(
                    desktopPath, TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
                CoTaskMemFree(desktopPath);
            }
        }

        auto runIndex = [this]() {
            auto apps = std::make_unique<std::vector<AppEntry>>();
            try {
                *apps = BuildAppIndex();
            } catch (const fs::filesystem_error&) {}
            if (WaitForSingleObject(indexStopEvent_, 0) != WAIT_OBJECT_0) {
                if (PostMessageW(hwnd_, kAppsReadyMessage, 0,
                        reinterpret_cast<LPARAM>(apps.get()))) {
                    apps.release();
                }
            }
        };

        // Perform initial index build
        runIndex();

        std::vector<HANDLE> waitHandles;
        if (indexStopEvent_) waitHandles.push_back(indexStopEvent_);
        if (indexTriggerEvent_) waitHandles.push_back(indexTriggerEvent_);
        if (hUserStartMenu && hUserStartMenu != INVALID_HANDLE_VALUE) {
            waitHandles.push_back(hUserStartMenu);
        }
        if (hCommonStartMenu && hCommonStartMenu != INVALID_HANDLE_VALUE) {
            waitHandles.push_back(hCommonStartMenu);
        }
        if (hDesktop && hDesktop != INVALID_HANDLE_VALUE) {
            waitHandles.push_back(hDesktop);
        }

        while (!waitHandles.empty()) {
            const DWORD wait = WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                FALSE,
                INFINITE);

            if (wait == WAIT_OBJECT_0) {
                // indexStopEvent_ was signaled
                break;
            }

            if (wait >= WAIT_OBJECT_0 + 1 && wait < WAIT_OBJECT_0 + waitHandles.size()) {
                const HANDLE signaled = waitHandles[wait - WAIT_OBJECT_0];
                if (signaled == hUserStartMenu) {
                    FindNextChangeNotification(hUserStartMenu);
                } else if (signaled == hCommonStartMenu) {
                    FindNextChangeNotification(hCommonStartMenu);
                } else if (signaled == hDesktop) {
                    FindNextChangeNotification(hDesktop);
                }

                // Debounce quiet period: wait 750ms for filesystem changes to settle
                bool debounce = true;
                while (debounce) {
                    const DWORD debWait = WaitForMultipleObjects(
                        static_cast<DWORD>(waitHandles.size()),
                        waitHandles.data(),
                        FALSE,
                        750);

                    if (debWait == WAIT_OBJECT_0) {
                        debounce = false;
                        break;
                    } else if (debWait == WAIT_TIMEOUT) {
                        debounce = false;
                    } else if (debWait >= WAIT_OBJECT_0 + 1 && debWait < WAIT_OBJECT_0 + waitHandles.size()) {
                        const HANDLE nextSignaled = waitHandles[debWait - WAIT_OBJECT_0];
                        if (nextSignaled == hUserStartMenu) {
                            FindNextChangeNotification(hUserStartMenu);
                        } else if (nextSignaled == hCommonStartMenu) {
                            FindNextChangeNotification(hCommonStartMenu);
                        } else if (nextSignaled == hDesktop) {
                            FindNextChangeNotification(hDesktop);
                        }
                    } else {
                        debounce = false;
                    }
                }

                if (WaitForSingleObject(indexStopEvent_, 0) == WAIT_OBJECT_0) {
                    break;
                }

                runIndex();
            } else {
                break;
            }
        }

        if (hUserStartMenu && hUserStartMenu != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(hUserStartMenu);
        }
        if (hCommonStartMenu && hCommonStartMenu != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(hCommonStartMenu);
        }
        if (hDesktop && hDesktop != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(hDesktop);
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }

    void LockHoverAtPointer() {
        // Remember the screen row the pointer rests on when the wheel or
        // keyboard moves the selection, so a stationary or barely-moving mouse
        // cannot immediately steal that selection back.
        if (mouseKnown_ && page_ == Page::Launcher &&
            mouseY_ >= ResultsTop() && mouseY_ < FooterTop() - 8 &&
            mouseX_ >= 8 && mouseX_ <= width_ - 12) {
            hoverLockRow_ = ResultSlotAtPoint(mouseX_, mouseY_);
        } else {
            hoverLockRow_ = -1;
        }
    }

    bool EnsureTarget() {
        if (target_) return true;
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        if (FAILED(factory_->CreateHwndRenderTarget(properties,
                D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rect.right, rect.bottom)), &target_))) {
            properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
            if (FAILED(factory_->CreateHwndRenderTarget(properties,
                    D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rect.right, rect.bottom)), &target_))) {
                return false;
            }
        }
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_))) {
            DiscardTarget();
            return false;
        }
        return true;
    }

    void DiscardTarget() {
        for (auto& item : iconCache_) item.second.bitmap.Reset();
        ReleasePreviewLayout();  // its drawing-effect brushes belong to this target
        // Thumbnails belong to this target too. Their pixels are gone once
        // drawn, so a shown image loads again on the next paint (US-046).
        previewImages_.clear();
        brush_.Reset();
        target_.Reset();
    }

    D2D1_COLOR_F Foreground() const {
        if (!highContrast_) return D2D1::ColorF(0xF2F2F3);
        return SystemColor(COLOR_WINDOWTEXT);
    }

    static D2D1_COLOR_F SystemColor(int index) {
        const COLORREF color = GetSysColor(index);
        return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
    }

    D2D1_COLOR_F Muted() const {
        return highContrast_ ? Foreground() : D2D1::ColorF(0xA7A7AC);
    }

    void Fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0) {
        brush_->SetColor(color);
        if (radius) target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
        else target_->FillRectangle(rect, brush_.Get());
    }

    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness = 1) {
        brush_->SetColor(color);
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), thickness);
    }

    void Text(std::wstring_view text, D2D1_RECT_F rect, IDWriteTextFormat* format,
        D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        auto layout = Layout(text, format, rect.right - rect.left, rect.bottom - rect.top);
        if (!layout) return;
        layout->SetTextAlignment(alignment);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        writeFactory_->CreateEllipsisTrimmingSign(format, &ellipsis);
        layout->SetTrimming(&trimming, ellipsis.Get());
        brush_->SetColor(color);
        target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Key(std::wstring_view label, float x, float y, float width = 24) {
        const auto rect = D2D1::RectF(x, y, x + width, y + 22);
        Fill(rect, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(1, 1, 1, 0.065f), 4);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.08f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4, 4), brush_.Get(), 0.75f);
        Text(label, rect, hintFormat_.Get(), highContrast_ ? SystemColor(COLOR_BTNTEXT) : Muted(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    static std::vector<std::wstring_view> SplitKeys(std::wstring_view label) {
        std::vector<std::wstring_view> tokens;
        size_t start = 0;
        while (start < label.size()) {
            const size_t plus = label.find(L" + ", start);
            tokens.push_back(label.substr(start,
                plus == std::wstring_view::npos ? std::wstring_view::npos : plus - start));
            if (plus == std::wstring_view::npos) break;
            start = plus + 3;
        }
        return tokens;
    }

    float KeyBadgeWidth(std::wstring_view token) const {
        if (token == L"Enter") return 23; // Drawn as a return glyph.
        float width = 22;
        if (auto layout = Layout(token, hintFormat_.Get(), 4096)) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
                width = (std::max)(22.0f, metrics.widthIncludingTrailingWhitespace + 14);
        }
        return width;
    }

    float KeyPlusWidth() const {
        float width = 16; // "+" glyph plus spacing
        if (auto layout = Layout(L"+", hintFormat_.Get(), 64)) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
                width = metrics.widthIncludingTrailingWhitespace + 8;
        }
        return width;
    }

    float KeyBadgesWidth(std::wstring_view label) const {
        float width = 0;
        bool first = true;
        for (const auto token : SplitKeys(label)) {
            if (!first) width += KeyPlusWidth();
            width += KeyBadgeWidth(token);
            first = false;
        }
        return width;
    }

    float DrawKeyBadges(std::wstring_view label, float x, float centerY) {
        const float y = centerY - 11;
        float drawn = 0;
        bool first = true;
        for (const auto token : SplitKeys(label)) {
            if (!first) {
                const float plus = KeyPlusWidth();
                Text(L"+", D2D1::RectF(x + drawn, y, x + drawn + plus, y + 22),
                    hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
                drawn += plus;
            }
            const float width = KeyBadgeWidth(token);
            Key(token == L"Enter" ? std::wstring_view(L"↵") : token, x + drawn, y, width);
            drawn += width;
            first = false;
        }
        return drawn;
    }

    void MouseKey(float x, float y, bool hovering = false) {
        const auto rect = D2D1::RectF(x, y, x + 22, y + 22);
        Fill(rect, highContrast_ ? SystemColor(COLOR_BTNFACE)
            : hovering ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.065f), 4);
        const auto color = highContrast_ ? SystemColor(COLOR_BTNTEXT)
            : hovering ? Foreground() : Muted();
        const auto mouse = D2D1::RectF(x + 6, y + 2, x + 16, y + 20);
        Fill(D2D1::RectF(x + 7.2f, y + 3.2f, x + 10.5f, y + 8.4f),
            highContrast_ ? SystemColor(COLOR_HIGHLIGHT)
            : hovering ? D2D1::ColorF(0x6EA8FE) : D2D1::ColorF(1, 1, 1, 0.95f), 1.4f);
        brush_->SetColor(color);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(mouse, 5, 5), brush_.Get(), 1.1f);
        Line(x + 6, y + 9, x + 16, y + 9, color, 1.0f);
        Line(x + 11, y + 3, x + 11, y + 9, color, 1.0f);
    }

    void SearchGlyph(float x, float y, float radius = 6) {
        brush_->SetColor(Muted());
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush_.Get(), 1.6f);
        Line(x + radius * 0.72f, y + radius * 0.72f, x + radius + 4, y + radius + 4, Muted(), 1.6f);
    }

    void GearGlyph(float x, float y) {
        const auto color = Muted();
        brush_->SetColor(color);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 7, 7), brush_.Get(), 1.6f);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 2.2f, 2.2f), brush_.Get(), 1.4f);
        constexpr float diagonal = 0.7071067f;
        const D2D1_POINT_2F directions[] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {diagonal, diagonal}, {-diagonal, diagonal},
            {diagonal, -diagonal}, {-diagonal, -diagonal},
        };
        for (const auto& direction : directions) {
            Line(x + direction.x * 7.6f, y + direction.y * 7.6f,
                x + direction.x * 10.0f, y + direction.y * 10.0f, color, 1.8f);
        }
    }

    void DrawSearch() {
        SearchGlyph(26, 30);
        const float right = width_ - 92;
        const auto clip = D2D1::RectF(kTextLeft, 12, right, kSearchHeight - 12);
        auto layout = Layout(input_.text, searchFormat_.Get(), 32768);
        if (layout) {
            float caret = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hit{};
            layout->HitTestTextPosition(static_cast<UINT32>(input_.caret), FALSE, &caret, &y, &hit);
            const float available = right - kTextLeft - 3;
            textScroll_ = (std::max)(0.0f, (std::max)(caret - available, (std::min)(textScroll_, caret)));
            const float origin = kTextLeft - textScroll_;
            caretX_ = std::clamp(origin + caret, kTextLeft, right - 2);
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            const float top = (kSearchHeight - metrics.height) / 2;
            target_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (input_.HasSelection()) {
                UINT32 count = 0;
                layout->HitTestTextRange(static_cast<UINT32>(input_.Start()),
                    static_cast<UINT32>(input_.End() - input_.Start()), origin, top, nullptr, 0, &count);
                std::vector<DWRITE_HIT_TEST_METRICS> selections(count);
                if (count && SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(input_.Start()),
                        static_cast<UINT32>(input_.End() - input_.Start()), origin, top,
                        selections.data(), count, &count))) {
                    for (const auto& selection : selections) {
                        Fill(D2D1::RectF(selection.left, selection.top,
                            selection.left + selection.width, selection.top + selection.height),
                            highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(0.42f, 0.62f, 0.95f, 0.35f), 2);
                    }
                }
            }
            if (input_.text.empty() && composition_.empty()) {
                Text(settings_.obsidianEnabled
                         ? L"Launch apps, search files, capture thoughts\u2026"
                         : L"Launch apps, search files\u2026",
                    D2D1::RectF(kTextLeft + 2, 0, right, kSearchHeight), searchFormat_.Get(), Muted());
            } else {
                brush_->SetColor(Foreground());
                target_->DrawTextLayout(D2D1::Point2F(origin, top), layout.Get(), brush_.Get());
            }
            if (!composition_.empty()) {
                Text(composition_, D2D1::RectF(caretX_, 0, right, kSearchHeight), searchFormat_.Get(), Foreground());
                Line(caretX_, 46, right, 46, Muted());
            }
            if (caretVisible_ && GetFocus() == hwnd_ && !actionsOpen_ && !input_.HasSelection()) {
                Fill(D2D1::RectF(caretX_, 21, caretX_ + 1.5f, 44), Foreground(), 0.5f);
            }
            target_->PopAxisAlignedClip();
            PositionIme();
        }
        if (!input_.text.empty()) {
            Line(width_ - 74, 28, width_ - 66, 36, Muted(), 1.4f);
            Line(width_ - 74, 36, width_ - 66, 28, Muted(), 1.4f);
        }
        GearGlyph(width_ - 28, 32);
        Line(1, kSearchHeight, width_ - 1, kSearchHeight, D2D1::ColorF(1, 1, 1, 0.09f));
    }

    void DrawResults() {
        const std::wstring section = !indexReady_ ? L"Applications" : input_.text.empty()
            ? (recent_.empty() ? L"Applications" : L"Recent & all applications") : L"Results";
        Text(section, D2D1::RectF(16, kSearchHeight, width_ / 2, ResultsTop()),
            hintFormat_.Get(), Muted());
        const std::wstring count = !indexReady_ ? L"Indexing\u2026" : std::to_wstring(results_.size()) +
            (input_.text.empty() ? L" apps" : results_.size() == 1 ? L" match" : L" matches");
        Text(count, D2D1::RectF(width_ / 2, kSearchHeight, width_ - 18, ResultsTop()),
            hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (results_.empty()) {
            const float center = (ResultsTop() + FooterTop()) / 2.0f;
            const bool hasQuery = !input_.text.empty();
            const bool hasSearchableText = !takeoff::Normalize(input_.text).empty();

            if (hasSearchableText && settings_.enableWebSearch) {
                SearchGlyph(width_ / 2.0f - 3.0f, center - 62.0f, 12.0f);
                Text(!indexReady_ ? L"Finding your applications\u2026" : L"No matching applications",
                    D2D1::RectF(32.0f, center - 42.0f, width_ - 32.0f, center - 14.0f), resultFormat_.Get(),
                    Foreground(), DWRITE_TEXT_ALIGNMENT_CENTER);

                const auto cardRect = WebSearchCardRect();
                const bool hovering = mouseKnown_ && PointInWebSearchCard(mouseX_, mouseY_);

                if (highContrast_) {
                    Fill(cardRect, hovering ? SystemColor(COLOR_HIGHLIGHT) : SystemColor(COLOR_BTNFACE), 8.0f);
                    brush_->SetColor(hovering ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground());
                    target_->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), brush_.Get(), 1.0f);
                } else {
                    Fill(cardRect, hovering ? D2D1::ColorF(0x6EA8FE, 0.16f) : D2D1::ColorF(1, 1, 1, 0.055f), 8.0f);
                    brush_->SetColor(hovering ? D2D1::ColorF(0x6EA8FE, 0.55f) : D2D1::ColorF(1, 1, 1, 0.12f));
                    target_->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), brush_.Get(), 1.0f);
                }

                SearchGlyph(cardRect.left + 22.0f, (cardRect.top + cardRect.bottom) / 2.0f - 1.0f, 6.0f);

                const std::wstring searchPrompt =
                    L"Search " + settings_.webSearchEngineName + L" for \u201C" + input_.text + L"\u201D";
                const auto promptRect = D2D1::RectF(cardRect.left + 38.0f, cardRect.top, cardRect.right - 54.0f, cardRect.bottom);
                const auto textColor = highContrast_ && hovering ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground();
                Text(searchPrompt, promptRect, resultFormat_.Get(), textColor);

                Key(L"↵", cardRect.right - 44.0f, (cardRect.top + cardRect.bottom) / 2.0f - 11.0f, 30.0f);

                Text(L"Press Enter or click to search in your browser",
                    D2D1::RectF(32.0f, cardRect.bottom + 12.0f, width_ - 32.0f, cardRect.bottom + 34.0f),
                    hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                SearchGlyph(width_ / 2.0f - 3.0f, center - 48.0f, 12.0f);
                Text(!indexReady_ ? L"Finding your applications\u2026" : !hasQuery
                        ? L"No applications found" : L"No matching applications",
                    D2D1::RectF(32.0f, center - 13.0f, width_ - 32.0f, center + 17.0f), resultFormat_.Get(),
                    Foreground(), DWRITE_TEXT_ALIGNMENT_CENTER);
                const std::wstring hint = !indexReady_
                    ? L"Your Start Menu and installed apps will appear here."
                    : !hasQuery
                        ? L"Apps from your Start Menu appear here."
                        : hasSearchableText && !settings_.enableWebSearch
                            ? L"Web search is disabled in Settings. Press Esc to clear."
                            : L"Try a shorter name, or press Esc to clear your search.";
                Text(hint, D2D1::RectF(32.0f, center + 20.0f, width_ - 32.0f, center + 48.0f), hintFormat_.Get(),
                    Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            return;
        }
        float currentTop = ResultsTop();
        for (int i = firstVisible_; i < static_cast<int>(results_.size()); ++i) {
            const float rowHeight = RowHeight(i);
            if (currentTop + rowHeight > FooterTop() - 4.0f) {
                break;
            }
            const float top = currentTop;
            currentTop += rowHeight;

            const bool selected = i == selected_;
            const auto row = D2D1::RectF(8, top, width_ - 12, top + rowHeight - 2);
            if (selected) {
                Fill(row, highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.10f), 7);
                brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.035f));
                target_->DrawRoundedRectangle(D2D1::RoundedRect(row, 7, 7), brush_.Get(), 1);
            }
            const AppEntry& app = apps_[results_[i]];
            const auto textColor = highContrast_ && selected ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground();

            if (app.category == takeoff::AppCategory::Calculator) {
                const auto iconRect = D2D1::RectF(20, top + (rowHeight - 32) / 2, 52, top + (rowHeight + 32) / 2);
                Fill(iconRect, D2D1::ColorF(0x2563EB), 7);
                Text(L"=", iconRect, searchFormat_.Get(), D2D1::ColorF(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);

                const float textLeft = 64.0f;
                const float rightAnswerWidth = 320.0f;
                const float leftTextRight = (std::max)(textLeft + 100.0f, width_ - rightAnswerWidth - 16.0f);

                Text(app.note.empty() ? std::wstring(L"Calculation") : app.note,
                    D2D1::RectF(textLeft, top + 8, leftTextRight, top + 26),
                    resultFormat_.Get(), textColor);

                std::wstring exprDisplay = app.parameters;
                if (!exprDisplay.empty() && exprDisplay.back() != L'=') {
                    exprDisplay += L" =";
                }
                Text(exprDisplay, D2D1::RectF(textLeft, top + 28, leftTextRight, top + 48),
                    hintFormat_.Get(), highContrast_ && selected ? textColor : Muted());

                const float answerRight = width_ - 24.0f;
                const float answerLeft = (std::max)(leftTextRight + 8.0f, width_ - rightAnswerWidth);
                Text(app.name, D2D1::RectF(answerLeft, top, answerRight, top + rowHeight - 2),
                    calcResultFormat_.Get(), textColor, DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else {
                const std::wstring& lookupPath = !app.iconPath.empty() ? app.iconPath : app.path;
                const auto iconRect = D2D1::RectF(20, top + 7, 46, top + 33);
                auto found = iconCache_.find(lookupPath);
                ID2D1Bitmap* bitmap = nullptr;
                if (found != iconCache_.end()) {
                    IconEntry& entry = found->second;
                    if (!entry.bitmap && entry.source) target_->CreateBitmapFromWicBitmap(entry.source.Get(), &entry.bitmap);
                    bitmap = entry.bitmap.Get();
                }
                if (bitmap) {
                    target_->DrawBitmap(bitmap, iconRect, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else {
                    if (app.category == takeoff::AppCategory::Folder) {
                        Fill(iconRect, D2D1::ColorF(0xD97706), 6);
                        Text(L"F", iconRect, resultFormat_.Get(), D2D1::ColorF(0xFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
                    } else if (app.category == takeoff::AppCategory::File) {
                        Fill(iconRect, D2D1::ColorF(0x4B5563), 6);
                        Text(L"F", iconRect, resultFormat_.Get(), D2D1::ColorF(0xFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
                    } else {
                        Fill(iconRect, D2D1::ColorF(0x555B71), 6);
                        Text(app.name.substr(0, 1), iconRect, resultFormat_.Get(), D2D1::ColorF(0xFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
                    }
                }
                Text(CommandRowText(app), D2D1::RectF(60, top, width_ - 158, top + 40), resultFormat_.Get(),
                    CommandRowDimmed(app) && !(highContrast_ && selected) ? Muted() : textColor);
                const bool pinned = PinRank(results_[i]) >= 0;
                const bool recent = input_.text.empty() &&
                    std::find(recent_.begin(), recent_.end(), results_[i]) != recent_.end();
                const wchar_t* categoryLabel = pinned ? L"Pinned" : recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::Command ? L"Command"
                    : (app.category == takeoff::AppCategory::Url ? L"URL"
                    : (app.category == takeoff::AppCategory::Info ? L""
                    : (app.category == takeoff::AppCategory::Snippet ? L"Snippet"
                    : (app.category == takeoff::AppCategory::Pomodoro ? L"Timer"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File"
                    : (app.category == takeoff::AppCategory::TaskAdd ? settings_.taskPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteAdd ? settings_.noteAddPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::LogAdd ? settings_.logPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteJump ? settings_.vaultSearchPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::WebSearch ? settings_.webSearchPillLabel.c_str() : L"Application")))))))))))));
                Text(categoryLabel,
                    D2D1::RectF(width_ - 154, top, width_ - 28, top + 40), hintFormat_.Get(),
                    highContrast_ && selected ? textColor : Muted(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
        if (results_.size() > static_cast<size_t>(visibleRows_)) {
            const float track = visibleRows_ * kRowHeight - 4;
            const float thumb = (std::max)(24.0f, track * visibleRows_ / static_cast<float>(results_.size()));
            const float offset = (track - thumb) * firstVisible_ /
                static_cast<float>(results_.size() - visibleRows_);
            Fill(D2D1::RectF(width_ - 6, ResultsTop() + offset, width_ - 3, ResultsTop() + offset + thumb),
                D2D1::ColorF(1, 1, 1, 0.22f), 1.5f);
        }
    }

    D2D1_RECT_F WebSearchCardRect() const {
        const float center = (ResultsTop() + FooterTop()) / 2.0f;
        constexpr float cardHeight = 44.0f;
        const float cardWidth = (std::min)(width_ - 64.0f, 460.0f);
        const float left = (width_ - cardWidth) / 2.0f;
        const float top = center + 6.0f;
        return D2D1::RectF(left, top, left + cardWidth, top + cardHeight);
    }

    bool PointInWebSearchCard(float x, float y) const {
        if (page_ != Page::Launcher || !results_.empty() || !settings_.enableWebSearch ||
            takeoff::Normalize(input_.text).empty()) return false;
        const auto rect = WebSearchCardRect();
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    D2D1_RECT_F UpdateIndicatorRect() const {
        const float top = FooterTop();
        const float cx = width_ / 2.0f;
        const float halfWidth = updateDownloaded_ ? 80.0f : 70.0f;
        return D2D1::RectF(cx - halfWidth, top, cx + halfWidth, height_);
    }

    bool PointInUpdateIndicator(float x, float y) const {
        if (!updateAvailable_) return false;
        const auto rect = UpdateIndicatorRect();
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    bool PointInAdminAction(float x, float y) const {
        if (page_ != Page::Launcher || !HasResult()) return false;
        const AppEntry& app = apps_[results_[selected_]];
        if (app.category == takeoff::AppCategory::Calculator ||
            app.category == takeoff::AppCategory::TaskAdd ||
            app.category == takeoff::AppCategory::NoteAdd ||
            app.category == takeoff::AppCategory::NoteJump ||
            app.category == takeoff::AppCategory::LogAdd ||
            app.category == takeoff::AppCategory::WebSearch) return false;
        if (settings_.administratorHotkey.disabled) return false;
        const float top = FooterTop();
        if (y < top || y > height_) return false;
        const std::wstring adminLabel =
            quicklaunch::FormatAdminBinding(settings_.administratorHotkey);
        const float adminWidth = 160.0f + KeyBadgesWidth(adminLabel) + 16.0f + 22.0f + 20.0f;
        return x >= 20.0f && x <= adminWidth;
    }

    void DrawUpdateIndicator() {
        if (!updateAvailable_) return;
        const auto rect = UpdateIndicatorRect();
        const bool hovering = mouseKnown_ && PointInUpdateIndicator(mouseX_, mouseY_);

        if (updateDownloaded_) {
            const float btnHeight = 26.0f;
            const float btnTop = rect.top + (kFooterHeight - btnHeight) / 2.0f;
            const auto btnRect = D2D1::RectF(rect.left, btnTop, rect.right, btnTop + btnHeight);

            const auto bgColor = highContrast_
                ? (hovering ? SystemColor(COLOR_HIGHLIGHT) : SystemColor(COLOR_BTNFACE))
                : (hovering ? D2D1::ColorF(0x3B82F6) : D2D1::ColorF(0x2563EB));
            Fill(btnRect, bgColor, 6.0f);

            brush_->SetColor(highContrast_
                ? Foreground()
                : (hovering ? D2D1::ColorF(1, 1, 1, 0.40f) : D2D1::ColorF(1, 1, 1, 0.20f)));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, 6.0f, 6.0f), brush_.Get(), 1.0f);

            const std::wstring_view text = L"Restart to Update";
            auto layout = Layout(text, hintFormat_.Get(), btnRect.right - btnRect.left, btnRect.bottom - btnRect.top);
            if (layout) {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const auto textColor = highContrast_
                    ? (hovering ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground())
                    : D2D1::ColorF(0xFFFFFF);
                brush_->SetColor(textColor);
                target_->DrawTextLayout(D2D1::Point2F(btnRect.left, btnRect.top), layout.Get(), brush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }

        const std::wstring_view text = L"Update Available";
        auto layout = Layout(text, hintFormat_.Get(), rect.right - rect.left, rect.bottom - rect.top);
        if (!layout) return;
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});

        const auto color = highContrast_ ? Foreground()
            : hovering ? D2D1::ColorF(0x9EC5FE) : D2D1::ColorF(0x6EA8FE);
        brush_->SetColor(color);
        target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    bool ShouldShowHotkeyWarning() const {
        return page_ == Page::Launcher &&
               !hotkeyRegistered_ &&
               !settings_.launcherHotkey.disabled &&
               settings_.launcherHotkey.key != 0 &&
               !hotkeyWarningDismissed_;
    }

    D2D1_RECT_F HotkeyWarningCardRect() const {
        constexpr float cardWidth = 480.0f;
        constexpr float cardHeight = 240.0f;
        const float left = (width_ - cardWidth) / 2.0f;
        const float top = (height_ - cardHeight) / 2.0f;
        return D2D1::RectF(left, top, left + cardWidth, top + cardHeight);
    }

    D2D1_RECT_F HotkeyWarningSettingsButtonRect() const {
        const auto card = HotkeyWarningCardRect();
        constexpr float buttonHeight = 34.0f;
        const float bottom = card.bottom - 22.0f;
        const float top = bottom - buttonHeight;
        return D2D1::RectF(card.left + 142.0f, top, card.right - 24.0f, bottom);
    }

    D2D1_RECT_F HotkeyWarningDismissButtonRect() const {
        const auto card = HotkeyWarningCardRect();
        constexpr float buttonHeight = 34.0f;
        const float bottom = card.bottom - 22.0f;
        const float top = bottom - buttonHeight;
        return D2D1::RectF(card.left + 24.0f, top, card.left + 130.0f, bottom);
    }

    bool PointInHotkeyWarningSettings(float x, float y) const {
        if (!ShouldShowHotkeyWarning()) return false;
        const auto r = HotkeyWarningSettingsButtonRect();
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    bool PointInHotkeyWarningDismiss(float x, float y) const {
        if (!ShouldShowHotkeyWarning()) return false;
        const auto r = HotkeyWarningDismissButtonRect();
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    bool PointInHotkeyWarningCard(float x, float y) const {
        if (!ShouldShowHotkeyWarning()) return false;
        const auto r = HotkeyWarningCardRect();
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    void DrawHotkeyWarningModal() {
        if (!ShouldShowHotkeyWarning()) return;

        // Frosted semi-transparent overlay covering full launcher window
        Fill(D2D1::RectF(0, 0, width_, height_),
            highContrast_ ? D2D1::ColorF(0, 0, 0, 0.85f) : D2D1::ColorF(0x0C0C0E, 0.80f), 8.0f);

        const auto card = HotkeyWarningCardRect();

        // Drop shadow behind modal card
        Fill(D2D1::RectF(card.left - 6, card.top - 2, card.right + 6, card.bottom + 8),
            D2D1::ColorF(0, 0, 0, 0.35f), 14.0f);

        // Modal card surface
        Fill(card, highContrast_ ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0x1F1F23), 10.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.16f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(card, 10.0f, 10.0f), brush_.Get(), 1.0f);

        // Warning badge icon (!)
        const auto pillRect = D2D1::RectF(card.left + 24.0f, card.top + 22.0f, card.left + 46.0f, card.top + 44.0f);
        Fill(pillRect, D2D1::ColorF(0xF59E0B, 0.18f), 11.0f);
        Text(L"!", pillRect, hintFormat_.Get(), D2D1::ColorF(0xFBBF24), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Header title
        Text(L"Hotkey Conflict Detected",
            D2D1::RectF(card.left + 54.0f, card.top + 20.0f, card.right - 24.0f, card.top + 46.0f),
            resultFormat_.Get(), Foreground(), DWRITE_TEXT_ALIGNMENT_LEADING);

        // Description text
        Text(L"Your launcher hotkey is taken by another application. Lean Launcher cannot listen for this shortcut until changed.",
            D2D1::RectF(card.left + 24.0f, card.top + 54.0f, card.right - 24.0f, card.top + 98.0f),
            hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_LEADING);

        // Conflict preview box showing current hotkey badges
        const float boxTop = card.top + 104.0f;
        const auto badgeBox = D2D1::RectF(card.left + 24.0f, boxTop, card.right - 24.0f, boxTop + 40.0f);
        Fill(badgeBox, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(0, 0, 0, 0.25f), 6.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.08f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(badgeBox, 6.0f, 6.0f), brush_.Get(), 0.75f);

        Text(L"Currently assigned:",
            D2D1::RectF(badgeBox.left + 14.0f, badgeBox.top, badgeBox.left + 150.0f, badgeBox.bottom),
            hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_LEADING);

        const std::wstring hotkeyStr = quicklaunch::FormatBinding(settings_.launcherHotkey);
        const float badgesW = KeyBadgesWidth(hotkeyStr);
        DrawKeyBadges(hotkeyStr, badgeBox.right - 14.0f - badgesW, (badgeBox.top + badgeBox.bottom) / 2.0f);

        // Dismiss button
        const auto dismissRect = HotkeyWarningDismissButtonRect();
        const bool dismissHover = mouseKnown_ && PointInHotkeyWarningDismiss(mouseX_, mouseY_);
        Fill(dismissRect, highContrast_
            ? (dismissHover ? SystemColor(COLOR_HIGHLIGHT) : SystemColor(COLOR_BTNFACE))
            : dismissHover ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(1, 1, 1, 0.06f), 6.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.12f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(dismissRect, 6.0f, 6.0f), brush_.Get(), 1.0f);
        Text(L"Dismiss", dismissRect, hintFormat_.Get(),
            highContrast_ && dismissHover ? SystemColor(COLOR_HIGHLIGHTTEXT) : (dismissHover ? Foreground() : Muted()),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        // Change in Settings button (Primary action)
        const auto settingsRect = HotkeyWarningSettingsButtonRect();
        const bool settingsHover = mouseKnown_ && PointInHotkeyWarningSettings(mouseX_, mouseY_);
        Fill(settingsRect, highContrast_
            ? (settingsHover ? SystemColor(COLOR_HIGHLIGHT) : SystemColor(COLOR_BTNFACE))
            : settingsHover ? D2D1::ColorF(0x3B82F6) : D2D1::ColorF(0x2563EB), 6.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.20f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(settingsRect, 6.0f, 6.0f), brush_.Get(), 1.0f);
        Text(L"Change in Settings", settingsRect, hintFormat_.Get(),
            highContrast_ && settingsHover ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0xFFFFFF),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void DrawFooter() {
        const float top = FooterTop();
        const float middle = width_ / 2;
        Fill(D2D1::RectF(1, top, width_ - 1, height_ - 1), D2D1::ColorF(0, 0, 0, 0.10f));
        Line(1, top, width_ - 1, top, D2D1::ColorF(1, 1, 1, 0.09f));
        if (!status_.empty()) {
            Text(status_, D2D1::RectF(12, top, middle - 12, height_), hintFormat_.Get(), Muted(),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (!hotkeyRegistered_) {
            const std::wstring message =
                quicklaunch::FormatBinding(settings_.launcherHotkey) + L" is in use";
            Text(message, D2D1::RectF(12, top, middle - 12, height_), hintFormat_.Get(), Muted(),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (HasResult()) {
            const AppEntry& app = apps_[results_[selected_]];
            if (app.category == takeoff::AppCategory::Calculator) {
                Text(L"Copy result", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::TaskAdd) {
                Text(L"Add task", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::NoteAdd) {
                Text(L"Add to note", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::LogAdd) {
                Text(L"Add log entry", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::NoteJump) {
                Text(L"Open in Obsidian", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::Url) {
                Text(L"Open URL", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::Snippet) {
                Text(L"Insert", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::Command) {
                Text(L"Run command", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else if (app.category == takeoff::AppCategory::WebSearch) {
                Text(L"Search in browser", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted());
                Key(L"\u21B5", 96, top + (kFooterHeight - 22) / 2, 24);
            } else {
                const bool adminHover = mouseKnown_ && PointInAdminAction(mouseX_, mouseY_);
                Text(L"Open as Administrator", D2D1::RectF(24, top, 156, height_),
                    hintFormat_.Get(), (adminHover || actionsOpen_) ? Foreground() : Muted());
                if (settings_.administratorHotkey.disabled) {
                    Text(L"Disabled", D2D1::RectF(160, top, 286, height_), hintFormat_.Get(), Muted(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    const std::wstring adminLabel =
                        quicklaunch::FormatAdminBinding(settings_.administratorHotkey);
                    float x = 160 + DrawKeyBadges(adminLabel, 160, top + kFooterHeight / 2);
                    Text(L"/", D2D1::RectF(x + 2, top, x + 16, height_), hintFormat_.Get(), Muted(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                    MouseKey(x + 20, top + 10, adminHover);
                }
            }
        }
        if (HasResult()) {
            const std::wstring actionsLabel =
                quicklaunch::FormatBinding(settings_.actionsHotkey);
            const float badges = KeyBadgesWidth(actionsLabel);
            DrawKeyBadges(actionsLabel, width_ - 20 - badges, top + kFooterHeight / 2);
            Text(L"Actions", D2D1::RectF(middle, top, width_ - 30 - badges, height_),
                hintFormat_.Get(), actionsOpen_ ? Foreground() : Muted(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (results_.empty() && !takeoff::Normalize(input_.text).empty() && settings_.enableWebSearch) {
            Text(L"Search " + settings_.webSearchEngineName, D2D1::RectF(middle, top, width_ - 58, height_),
                hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            Key(L"↵", width_ - 48, top + (kFooterHeight - 22) / 2, 28);
        }
        if (updateAvailable_) {
            DrawUpdateIndicator();
        } else if (pomodoro_ && page_ == Page::Launcher) {
            const long long left = leanlauncher::pomodoro::RemainingSeconds(pomodoro_->endTicks, NowTicks());
            Text(leanlauncher::pomodoro::FooterLabel(left), D2D1::RectF(width_ / 2 - 60, top, width_ / 2 + 60, height_),
                hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void DrawActions() {
        if (!actionsOpen_ || !HasResult()) return;
        const auto rect = ActionsRect();
        Fill(D2D1::RectF(rect.left - 4, rect.top - 2, rect.right + 4, rect.bottom + 5),
            D2D1::ColorF(0, 0, 0, 0.25f), 10);
        Fill(rect, highContrast_ ? SystemColor(COLOR_WINDOW) : D2D1::ColorF(0x303033), 8);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.16f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 8, 8), brush_.Get());
        const AppEntry& app = apps_[results_[selected_]];
        Text(app.name,
            D2D1::RectF(rect.left + 12, rect.top + 2, rect.right - 12, rect.top + 30),
            hintFormat_.Get(), Muted());
        const bool isCalc = (app.category == takeoff::AppCategory::Calculator);
        const bool isTaskAdd = (app.category == takeoff::AppCategory::TaskAdd);
        const bool isNoteAdd = (app.category == takeoff::AppCategory::NoteAdd);
        const bool isLogAdd = (app.category == takeoff::AppCategory::LogAdd);
        const bool isNoteJump = (app.category == takeoff::AppCategory::NoteJump);
        const bool isWebSearch = (app.category == takeoff::AppCategory::WebSearch);
        const bool isFileOrFolder = (app.category == takeoff::AppCategory::File ||
                                     app.category == takeoff::AppCategory::Folder);
        const wchar_t* pinLabel = PinRank(results_[selected_]) >= 0 ? L"Unpin" : L"Pin";
        const wchar_t* appLabels[] = {L"Open as Administrator", L"Copy app name", L"Copy launch path", pinLabel};
        const wchar_t* fileLabels[] = {L"Open", L"Open containing folder", L"Copy file path", pinLabel};
        const wchar_t* calcLabels[] = {L"Copy result", L"Copy calculation", L"Open Windows Calculator"};
        const wchar_t* taskLabels[] = {L"Add task", L"Copy task text", L"Open today's note"};
        const wchar_t* noteAddLabels[] = {L"Add to note", L"Copy text", L"Open today's note"};
        const wchar_t* logAddLabels[] = {L"Add log entry", L"Copy text", L"Open today's note"};
        const wchar_t* noteLabels[] = {L"Open in Obsidian", L"Copy note title", L"Reveal in Explorer"};
        const wchar_t* webSearchLabels[] = {L"Search in browser", L"Copy query text", L"Copy search URL"};
        const wchar_t* commandLabels[] = {L"Run"};
        const wchar_t* snippetLabels[] = {L"Insert"};
        const wchar_t* urlLabels[] = {L"Open URL", L"Copy URL"};
        const bool isCommand = (app.category == takeoff::AppCategory::Command ||
                                app.category == takeoff::AppCategory::Pomodoro);
        const bool isUrl = (app.category == takeoff::AppCategory::Url);
        const bool isSnippet = (app.category == takeoff::AppCategory::Snippet);
        const wchar_t** labels = isSnippet ? snippetLabels : isCommand ? commandLabels : isUrl ? urlLabels : isCalc ? calcLabels
            : (isTaskAdd ? taskLabels
            : (isNoteAdd ? noteAddLabels
            : (isLogAdd ? logAddLabels
            : (isNoteJump ? noteLabels
            : (isWebSearch ? webSearchLabels
            : (isFileOrFolder ? fileLabels : appLabels))))));
        const int actionCount = ActionCount();
        for (int i = 0; i < actionCount; ++i) {
            const float top = rect.top + 32 + i * 36;
            const auto row = D2D1::RectF(rect.left + 6, top, rect.right - 6, top + 34);
            if (i == actionSelected_) {
                Fill(row, highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.09f), 5);
            }
            Text(labels[i], D2D1::RectF(row.left + 8, row.top, row.right - 38, row.bottom),
                hintFormat_.Get(), highContrast_ && i == actionSelected_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground());
            if (i == actionSelected_) Key(L"\u21B5", row.right - 30, top + 6, 23);
        }
    }

    void DrawToggle(float right, float centerY, bool enabled) {
        const auto track = D2D1::RectF(right - 38, centerY - 10, right, centerY + 10);
        Fill(track, highContrast_
                ? SystemColor(enabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE)
                : enabled ? D2D1::ColorF(0x6EA8FE) : D2D1::ColorF(1, 1, 1, 0.13f),
            10);
        const float knobX = enabled ? right - 10 : right - 28;
        Fill(D2D1::RectF(knobX - 7, centerY - 7, knobX + 7, centerY + 7),
            highContrast_ && enabled ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0xF6F6F7), 7);
    }

    void DrawSettingsRow(int index, float top, std::wstring_view title,
        std::wstring_view description, std::wstring_view value = {}, bool toggle = false,
        bool enabled = false, bool plainValue = false, bool editable = false) {
        const bool selected = (index == settingsSelected_);
        const auto row = D2D1::RectF(18, top + 1, width_ - 18, top + kSettingsRowHeight - 1);
        if (selected) {
            Fill(row, highContrast_ ? SystemColor(COLOR_HIGHLIGHT) :
                D2D1::ColorF(1, 1, 1, 0.08f), 6.0f);
        } else if (index == recordingRow_ || index == editingRow_) {
            Fill(row, D2D1::ColorF(0x3B82F6, 0.12f), 6.0f);
        }
        const auto primary = highContrast_ && selected ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground();
        const auto secondary = highContrast_ && selected ? primary : Muted();

        Text(title, D2D1::RectF(32, top + 4, width_ - 260, top + 26),
            resultFormat_.Get(), primary);
        Text(description, D2D1::RectF(32, top + 24, width_ - 260, top + 44),
            hintFormat_.Get(), secondary);

        if (toggle) {
            DrawToggle(width_ - 36, top + kSettingsRowHeight / 2, enabled);
        } else if (index == recordingRow_) {
            Text(L"Press keys\u2026", D2D1::RectF(width_ - 240, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0x6EA8FE),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (editable && index == editingRow_) {
            // Live edit buffer, not the (not-yet-committed) `value` argument.
            // No true caret hit-testing here, unlike the main search box -
            // a trailing bar is a deliberately simple stand-in, adequate for
            // a short Settings field.
            const std::wstring editText = settingsEdit_.text + L"\u2502";
            Text(editText, D2D1::RectF(width_ - 260, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(0x6EA8FE),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (plainValue || editable) {
            Text(value, D2D1::RectF(width_ - 260, top, width_ - 36, top + kSettingsRowHeight),
                hintFormat_.Get(), secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            const float badgesW = KeyBadgesWidth(value);
            DrawKeyBadges(value, width_ - 36 - badgesW, top + kSettingsRowHeight / 2);
        }
    }

    // One collapsed summary row for an Obsidian action block: title, a
    // compact "current prefix / label" descriptor (or "off" if the action
    // itself is disabled), and a chevron showing expanded/collapsed state.
    void DrawObsidianSectionSummary(int row, float top, int section, std::wstring_view title,
        bool enabled, const std::wstring& prefix, const std::wstring& label) {
        const bool expanded = (obsidianExpandedSection_ == section);
        std::wstring value;
        if (!enabled) value += L"off · ";
        value += prefix + L" / " + label + (expanded ? L"  ▾" : L"  ›");
        DrawSettingsRow(row, top, title, expanded ? L"Tap to collapse" : L"Tap to edit",
            value, false, false, true);
    }

    // Dispatches a single Obsidian-section row ID to its title/description/
    // value. Called once per entry in ObsidianVisibleRows() - this is the
    // only place that maps a row ID to what it displays, so DrawSettings()
    // itself just loops without needing to know what any given row is.
    void DrawObsidianRow(int row, float top) {
        switch (row) {
        case kRowObsidianEnabled:
            DrawSettingsRow(row, top, L"Enable Obsidian integration",
                L"Turn on vault search, task capture, note capture, and log capture from the launcher",
                {}, true, settings_.obsidianEnabled);
            return;
        case kRowVaultPicker: {
            std::wstring vaultValue = L"None found";
            std::wstring vaultDescription = L"No Obsidian vaults found. Install Obsidian and open a vault, then reopen Settings.";
            if (!knownVaults_.empty()) {
                if (obsidianVaultPath_.empty()) {
                    vaultValue = L"Not set";
                    vaultDescription = L"Press Enter to select a detected vault";
                } else {
                    vaultValue = fs::path(obsidianVaultPath_).filename().wstring();
                    vaultDescription = dailyNoteConfig_.found
                        ? L"Tasks are added to today's daily note in this vault"
                        : L"Could not read this vault's daily notes config - using vault root + YYYY-MM-DD.md";
                }
            }
            DrawSettingsRow(row, top, L"Obsidian Vault", vaultDescription, vaultValue, false, false, true);
            return;
        }
        case kRowVaultSearchSummary:
            DrawObsidianSectionSummary(row, top, kSectionVaultSearch, L"Vault search",
                settings_.vaultSearchEnabled, settings_.vaultSearchPrefix, settings_.vaultSearchPillLabel);
            return;
        case kRowVaultSearchEnabled:
            DrawSettingsRow(row, top, L"Vault search enabled",
                L"Fuzzy-search note titles and open the match in Obsidian",
                {}, true, settings_.vaultSearchEnabled);
            return;
        case kRowVaultSearchPrefix:
            DrawSettingsRow(row, top, L"Vault search prefix", L"Type this followed by a space, then a note title",
                settings_.vaultSearchPrefix, false, false, false, true);
            return;
        case kRowVaultSearchPillLabel:
            DrawSettingsRow(row, top, L"Vault search label", L"Result-row tag shown next to a matched note",
                settings_.vaultSearchPillLabel, false, false, false, true);
            return;
        case kRowTaskSummary:
            DrawObsidianSectionSummary(row, top, kSectionTask, L"Add task",
                settings_.taskAddEnabled, settings_.taskPrefix, settings_.taskPillLabel);
            return;
        case kRowTaskAddEnabled:
            DrawSettingsRow(row, top, L"Add task enabled", L"Append a checklist item to today's daily note or the target note",
                {}, true, settings_.taskAddEnabled);
            return;
        case kRowTaskPrefix:
            DrawSettingsRow(row, top, L"Add task prefix", L"Type this followed by a space, then the task text",
                settings_.taskPrefix, false, false, false, true);
            return;
        case kRowTaskPillLabel:
            DrawSettingsRow(row, top, L"Add task label", L"Result-row tag shown next to a pending task",
                settings_.taskPillLabel, false, false, false, true);
            return;
        case kRowTaskPreviewPrefix:
            DrawSettingsRow(row, top, L"Add task preview", L"Text shown before what you typed, e.g. \"Add task: buy milk\"",
                settings_.taskPreviewPrefix, false, false, false, true);
            return;
        case kRowNoteAddSummary:
            DrawObsidianSectionSummary(row, top, kSectionNoteAdd, L"Add to note",
                settings_.noteAddEnabled, settings_.noteAddPrefix, settings_.noteAddPillLabel);
            return;
        case kRowNoteAddEnabled:
            DrawSettingsRow(row, top, L"Add to note enabled",
                L"Append a plain line (not a checklist item) to today's daily note or the target note",
                {}, true, settings_.noteAddEnabled);
            return;
        case kRowNoteAddPrefix:
            DrawSettingsRow(row, top, L"Add to note prefix", L"Type this followed by a space, then the line text",
                settings_.noteAddPrefix, false, false, false, true);
            return;
        case kRowNoteAddPillLabel:
            DrawSettingsRow(row, top, L"Add to note label", L"Result-row tag shown next to a pending line",
                settings_.noteAddPillLabel, false, false, false, true);
            return;
        case kRowNoteAddPreviewPrefix:
            DrawSettingsRow(row, top, L"Add to note preview",
                L"Text shown before what you typed, e.g. \"Add to note: back from the gym\"",
                settings_.noteAddPreviewPrefix, false, false, false, true);
            return;
        case kRowLogSummary:
            DrawObsidianSectionSummary(row, top, kSectionLog, L"Log",
                settings_.logEnabled, settings_.logPrefix, settings_.logPillLabel);
            return;
        case kRowLogEnabled:
            DrawSettingsRow(row, top, L"Log enabled",
                L"Insert a timestamped line after a heading in today's daily note or the target note",
                {}, true, settings_.logEnabled);
            return;
        case kRowLogPrefix:
            DrawSettingsRow(row, top, L"Log prefix", L"Type this followed by a space, then the log text",
                settings_.logPrefix, false, false, false, true);
            return;
        case kRowLogPillLabel:
            DrawSettingsRow(row, top, L"Log label", L"Result-row tag shown next to a pending log entry",
                settings_.logPillLabel, false, false, false, true);
            return;
        case kRowLogPreviewPrefix:
            DrawSettingsRow(row, top, L"Log preview",
                L"Text shown before what you typed, e.g. \"Log: back from a walk\"",
                settings_.logPreviewPrefix, false, false, false, true);
            return;
        case kRowLogHeading:
            DrawSettingsRow(row, top, L"Log heading", L"Exact heading line to insert after, e.g. \"## Log\"",
                settings_.logHeading, false, false, false, true);
            return;
        case kRowQuickOpenTarget:
            DrawSettingsRow(row, top, L"Quick open (" + settings_.vaultSearchPrefix + L" .)",
                quickOpenDropdownOpen_ ? L"Tap to collapse"
                    : L"Note opened in Obsidian when you type the vault search prefix, a space, and a dot",
                QuickOpenTargetLabel(settings_.quickOpenTarget), false, false, true);
            return;
        case kRowTaskTargetNote:
            DrawSettingsRow(row, top, L"Add task target note",
                L"Leave empty for today's daily note, or a vault path, e.g. Inbox/Tasks",
                settings_.taskTargetNote, false, false, false, true);
            return;
        case kRowNoteAddTargetNote:
            DrawSettingsRow(row, top, L"Add to note target note",
                L"Leave empty for today's daily note, or a vault path, e.g. Scratch",
                settings_.noteAddTargetNote, false, false, false, true);
            return;
        case kRowLogTargetNote:
            DrawSettingsRow(row, top, L"Log target note",
                L"Leave empty for today's daily note, or a vault path, e.g. Logs/Work Log",
                settings_.logTargetNote, false, false, false, true);
            return;
        case kRowOverridesSummary: {
            const bool hasOverride = !settings_.dailyNoteFolderOverride.empty() ||
                !settings_.dailyNoteFormatOverride.empty();
            const bool expanded = (obsidianExpandedSection_ == kSectionOverrides);
            DrawSettingsRow(row, top, L"Daily note overrides",
                hasOverride ? L"Custom folder and/or format set" : L"Auto-detected from the vault",
                expanded ? L"▾" : L"›", false, false, true);
            return;
        }
        case kRowDailyNoteFolderOverride:
            DrawSettingsRow(row, top, L"Daily note folder override", L"Leave empty to auto-detect from the vault's daily-notes config",
                settings_.dailyNoteFolderOverride, false, false, false, true);
            return;
        case kRowDailyNoteFormatOverride:
            DrawSettingsRow(row, top, L"Daily note format override", L"Leave empty to auto-detect; e.g. YYYY-MM-DD or YYYY/MMMM/YYYY-MM-DD-dddd",
                settings_.dailyNoteFormatOverride, false, false, false, true);
            return;
        default:
            return;
        }
    }

    // Overlay popup for the vault picker - drawn on top of whatever rows
    // are beneath it rather than pushing them down, so the fixed row-index
    // geometry (SettingsRowTop et al.) never needs to account for it.
    void DrawVaultDropdown() {
        if (!vaultDropdownOpen_ || knownVaults_.empty()) return;
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        target_->PushAxisAlignedClip(
            D2D1::RectF(0, viewportTop, width_, viewportBottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float listTop = VaultDropdownTop();
        const int count = static_cast<int>(knownVaults_.size());
        const int maxVisible = VaultDropdownMaxVisibleItems();
        const int visibleCount = (std::min)(count, maxVisible);
        const bool scrollable = count > maxVisible;
        const float listHeight = static_cast<float>(visibleCount) * kVaultDropdownItemHeight;
        const auto listRect = D2D1::RectF(16, listTop, width_ - 16, listTop + listHeight);

        Fill(listRect, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(0x1C1C1E, 0.98f), 8.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.14f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(listRect, 8.0f, 8.0f), brush_.Get(), 1.0f);

        for (int slot = 0; slot < visibleCount; ++slot) {
            const int i = vaultDropdownScroll_ + slot;
            const float itemTop = listTop + static_cast<float>(slot) * kVaultDropdownItemHeight;
            const bool highlighted = (i == vaultDropdownHighlight_);
            const bool current = (knownVaults_[i] == obsidianVaultPath_);
            const float itemRight = scrollable ? width_ - 24.0f : width_ - 18.0f;
            if (highlighted) {
                Fill(D2D1::RectF(18, itemTop + 1, itemRight, itemTop + kVaultDropdownItemHeight - 1),
                    highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.10f), 5.0f);
            }
            const auto textColor = highContrast_ && highlighted ? SystemColor(COLOR_HIGHLIGHTTEXT)
                : current ? Foreground() : Muted();
            Text(fs::path(knownVaults_[i]).filename().wstring(),
                D2D1::RectF(32, itemTop, itemRight - 20.0f, itemTop + kVaultDropdownItemHeight),
                hintFormat_.Get(), textColor);
            if (current) {
                Text(L"✓", D2D1::RectF(itemRight - 20.0f, itemTop, itemRight, itemTop + kVaultDropdownItemHeight),
                    hintFormat_.Get(), textColor, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        // Scrollbar thumb, mirroring the main settings-panel scrollbar, so an
        // overflowing vault list (e.g. 14+ vaults) reads as scrollable rather
        // than silently truncated.
        if (scrollable) {
            const float trackTop = listTop + 4.0f;
            const float trackBottom = listTop + listHeight - 4.0f;
            const float trackHeight = trackBottom - trackTop;
            const float thumbHeight = (std::max)(20.0f, trackHeight * (static_cast<float>(visibleCount) / static_cast<float>(count)));
            const int maxScroll = count - maxVisible;
            const float thumbTop = trackTop + (trackHeight - thumbHeight) *
                (maxScroll > 0 ? static_cast<float>(vaultDropdownScroll_) / static_cast<float>(maxScroll) : 0.0f);
            const auto thumbRect = D2D1::RectF(width_ - 21.0f, thumbTop, width_ - 18.0f, thumbTop + thumbHeight);
            Fill(thumbRect, highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.22f), 1.5f);
        }
        target_->PopAxisAlignedClip();
    }

    // Mirrors DrawVaultDropdown() for the "Search engine" row's picker
    // (US-016): presets 0..kWebSearchPresetCount-1 plus a trailing "Custom"
    // entry, with a checkmark on whichever one matches the active template.
    void DrawWebSearchDropdown() {
        if (!webSearchDropdownOpen_) return;
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        target_->PushAxisAlignedClip(
            D2D1::RectF(0, viewportTop, width_, viewportBottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float listTop = WebSearchDropdownTop();
        const int presetCount = static_cast<int>(takeoff::kWebSearchPresetCount);
        const int itemCount = presetCount + 1;
        const float listHeight = static_cast<float>(itemCount) * kVaultDropdownItemHeight;
        const auto listRect = D2D1::RectF(16, listTop, width_ - 16, listTop + listHeight);

        Fill(listRect, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(0x1C1C1E, 0.98f), 8.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.14f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(listRect, 8.0f, 8.0f), brush_.Get(), 1.0f);

        const int activePreset = takeoff::FindWebSearchPresetIndex(settings_.webSearchUrlTemplate);
        for (int i = 0; i < itemCount; ++i) {
            const float itemTop = listTop + static_cast<float>(i) * kVaultDropdownItemHeight;
            const bool highlighted = (i == webSearchDropdownHighlight_);
            const bool current = (i < presetCount) ? (i == activePreset) : (activePreset < 0);
            const std::wstring_view name = (i < presetCount) ? takeoff::kWebSearchPresets[i].name : L"Custom";
            if (highlighted) {
                Fill(D2D1::RectF(18, itemTop + 1, width_ - 18, itemTop + kVaultDropdownItemHeight - 1),
                    highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.10f), 5.0f);
            }
            const auto textColor = highContrast_ && highlighted ? SystemColor(COLOR_HIGHLIGHTTEXT)
                : current ? Foreground() : Muted();
            Text(name, D2D1::RectF(32, itemTop, width_ - 44, itemTop + kVaultDropdownItemHeight),
                hintFormat_.Get(), textColor);
            if (current) {
                Text(L"✓", D2D1::RectF(width_ - 44, itemTop, width_ - 24, itemTop + kVaultDropdownItemHeight),
                    hintFormat_.Get(), textColor, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        target_->PopAxisAlignedClip();
    }

    // Mirrors DrawWebSearchDropdown() for the Quick open row (US-026).
    void DrawQuickOpenDropdown() {
        if (!quickOpenDropdownOpen_) return;
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        target_->PushAxisAlignedClip(
            D2D1::RectF(0, viewportTop, width_, viewportBottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float listTop = QuickOpenDropdownTop();
        const float listHeight = static_cast<float>(kQuickOpenTargetCount) * kVaultDropdownItemHeight;
        const auto listRect = D2D1::RectF(16, listTop, width_ - 16, listTop + listHeight);

        Fill(listRect, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(0x1C1C1E, 0.98f), 8.0f);
        brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.14f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(listRect, 8.0f, 8.0f), brush_.Get(), 1.0f);

        for (int i = 0; i < kQuickOpenTargetCount; ++i) {
            const float itemTop = listTop + static_cast<float>(i) * kVaultDropdownItemHeight;
            const bool highlighted = (i == quickOpenDropdownHighlight_);
            const bool current = (i == settings_.quickOpenTarget);
            if (highlighted) {
                Fill(D2D1::RectF(18, itemTop + 1, width_ - 18, itemTop + kVaultDropdownItemHeight - 1),
                    highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.10f), 5.0f);
            }
            const auto textColor = highContrast_ && highlighted ? SystemColor(COLOR_HIGHLIGHTTEXT)
                : current ? Foreground() : Muted();
            Text(QuickOpenTargetLabel(i), D2D1::RectF(32, itemTop, width_ - 44, itemTop + kVaultDropdownItemHeight),
                hintFormat_.Get(), textColor);
            if (current) {
                Text(L"\u2713", D2D1::RectF(width_ - 44, itemTop, width_ - 24, itemTop + kVaultDropdownItemHeight),
                    hintFormat_.Get(), textColor, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        target_->PopAxisAlignedClip();
    }

    void DrawSettings() {
        const float viewportTop = kSettingsHeaderHeight;
        const float viewportBottom = FooterTop();
        const float offsetY = viewportTop - settingsScroll_;

        // Scrollable content area clipped cleanly between header and footer
        target_->PushAxisAlignedClip(
            D2D1::RectF(0, viewportTop, width_, viewportBottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        auto drawCard = [this, offsetY](std::wstring_view header, float headerY, float cardY, int count) {
            // Section title
            Text(header, D2D1::RectF(24, headerY + offsetY, width_ - 24, headerY + 18.0f + offsetY),
                hintFormat_.Get(), Muted());

            // Card container background
            const float cardHeight = count * kSettingsRowHeight;
            const auto cardRect = D2D1::RectF(16, cardY + offsetY, width_ - 16, cardY + cardHeight + offsetY);
            Fill(cardRect, highContrast_ ? SystemColor(COLOR_BTNFACE) : D2D1::ColorF(1, 1, 1, 0.035f), 8.0f);
            brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.07f));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), brush_.Get(), 1.0f);

            // Row dividers inside card
            for (int i = 1; i < count; ++i) {
                const float divY = cardY + i * kSettingsRowHeight + offsetY;
                Line(28, divY, width_ - 28, divY, D2D1::ColorF(1, 1, 1, 0.045f));
            }
        };

        // Card-based tabs: frames first, then each tab's rows at their layout position.
        const auto cardList = CardsFor(settingsCategory_);
        for (std::size_t i = 0; i < cardList.count; ++i) {
            drawCard(cardList.cards[i].header,
                leanlauncher::settings_layout::CardHeaderTop(cardList.cards, cardList.count, i),
                leanlauncher::settings_layout::CardTop(cardList.cards, cardList.count, i),
                cardList.cards[i].count);
        }
        const auto rowY = [&](int row) { return SettingsRowTop(row) + offsetY; };

        if (settingsCategory_ == SettingsCategory::General) {
            DrawSettingsRow(0, rowY(0), L"Open Lean Launcher",
                L"Global shortcut that opens or closes the launcher",
                quicklaunch::FormatBinding(settings_.launcherHotkey));
            DrawSettingsRow(1, rowY(1), L"Actions menu",
                L"Show actions for the selected application",
                quicklaunch::FormatBinding(settings_.actionsHotkey));
            DrawSettingsRow(2, rowY(2), L"Open as administrator",
                L"Launch the selected application with elevation",
                quicklaunch::FormatAdminBinding(settings_.administratorHotkey));
            DrawSettingsRow(3, rowY(3), L"Quick launch",
                L"Open one of the eight visible results directly",
                quicklaunch::FormatQuickLaunchBinding(settings_.quickLaunchHotkey));
            DrawSettingsRow(4, rowY(4), L"Run at startup",
                L"Start Lean Launcher when you sign in to Windows", {}, true, settings_.runAtStartup);
            DrawSettingsRow(5, rowY(5), L"Notification area icon",
                L"Show Lean Launcher in the hidden icons area", {}, true, settings_.showTrayIcon);
            DrawSettingsRow(kRowAboutExportSettings, rowY(kRowAboutExportSettings), L"Export settings...",
                L"Save your settings to a file, to move them to another PC", {}, false, false, true);
            DrawSettingsRow(kRowAboutImportSettings, rowY(kRowAboutImportSettings),
                L"Import settings...", L"Load settings from a file - you see what changes before anything is applied",
                {}, false, false, true);
        }

        if (settingsCategory_ == SettingsCategory::Search) {
            DrawSettingsRow(7, rowY(7), L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, rowY(8), L"Web search",
                L"Open " + settings_.webSearchEngineName + L" when no results match your query",
                {}, true, settings_.enableWebSearch);
            DrawSettingsRow(kRowWebSearchEngine, rowY(kRowWebSearchEngine), L"Search engine",
                webSearchDropdownOpen_ ? L"Tap to collapse"
                    : L"Search engine used for the \"Web search\" fallback",
                settings_.webSearchEngineName, false, false, false, true);
            DrawSettingsRow(kRowFileSearchPrefix, rowY(kRowFileSearchPrefix), L"File search prefix",
                L"Type this followed by a space to show only files and folders",
                settings_.fileSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowWebSearchPrefix, rowY(kRowWebSearchPrefix), L"Web search prefix",
                L"Type this followed by a space to force a \"" + settings_.webSearchEngineName + L"\" search",
                settings_.webSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowAppSearchPrefix, rowY(kRowAppSearchPrefix), L"App search prefix",
                L"Type this followed by a space to show only installed apps",
                settings_.appSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowFileSearchEditExclusions, rowY(kRowFileSearchEditExclusions), L"Edit exclusions...",
                L"Add your own folder and file-type exclusions on top of the built-in list",
                {}, false, false, true);
            DrawSettingsRow(kRowFileSearchHelp, rowY(kRowFileSearchHelp), L"Help",
                L"Learn how file search exclusions work",
                {}, false, false, true);
            DrawSettingsRow(kRowPreviewEnabled, rowY(kRowPreviewEnabled), L"Preview panel",
                L"A note, image, or file preview next to your results",
                {}, true, settings_.enablePreview);
            DrawSettingsRow(kRowPreviewHotkey, rowY(kRowPreviewHotkey), L"Preview panel shortcut",
                L"Show or hide a preview of the selected result",
                quicklaunch::FormatBinding(settings_.previewHotkey));
        }

        if (settingsCategory_ == SettingsCategory::Tools) {
            DrawSettingsRow(kRowUnitConverterEnabled, rowY(kRowUnitConverterEnabled), L"Unit converter",
                L"Convert units offline, like 5 km in mi or 72 f to c",
                {}, true, settings_.enableUnitConverter);
            DrawSettingsRow(kRowTimeZonesEnabled, rowY(kRowTimeZonesEnabled), L"Time zone converter",
                L"Times in other places, like time in Tokyo or 10am PST in CET",
                {}, true, settings_.enableTimeZones);
            DrawSettingsRow(kRowTypedUrlsEnabled, rowY(kRowTypedUrlsEnabled), L"Typed URLs",
                L"Open web addresses you type, like github.com or https://...",
                {}, true, settings_.enableTypedUrls);
            DrawSettingsRow(kRowPathCompletionEnabled, rowY(kRowPathCompletionEnabled), L"Path completion",
                L"Complete folder paths as you type, like C:\\Us or %APPDATA%\\",
                {}, true, settings_.enablePathCompletion);
            DrawSettingsRow(kRowSystemCommandsEnabled, rowY(kRowSystemCommandsEnabled), L"System commands",
                L"Lock, sleep, restart, shut down, sign out, and empty the Recycle Bin",
                {}, true, settings_.enableSystemCommands);
            DrawSettingsRow(kRowSystemCommandsPrefix, rowY(kRowSystemCommandsPrefix), L"System commands prefix",
                L"Type this followed by a space to list system commands",
                settings_.systemCommandsPrefix, false, false, false, true);
            DrawSettingsRow(kRowPomodoroEnabled, rowY(kRowPomodoroEnabled), L"Pomodoro timer",
                L"Focus timers with a tray reminder, like pomo 25 write intro",
                {}, true, settings_.enablePomodoro);
            DrawSettingsRow(kRowPomodoroPrefix, rowY(kRowPomodoroPrefix), L"Pomodoro prefix",
                L"Type this followed by a space to start or check a timer",
                settings_.pomodoroPrefix, false, false, false, true);
            DrawSettingsRow(kRowPomodoroFocusMinutes, rowY(kRowPomodoroFocusMinutes), L"Focus length",
                L"Minutes for a focus timer when you don't type a number",
                settings_.pomodoroFocusMinutes, false, false, false, true);
            DrawSettingsRow(kRowPomodoroBreakMinutes, rowY(kRowPomodoroBreakMinutes), L"Break length",
                L"Minutes for a break",
                settings_.pomodoroBreakMinutes, false, false, false, true);
            DrawSettingsRow(kRowPomodoroLog, rowY(kRowPomodoroLog), L"Log finished Pomodoros",
                L"Add finished focus timers to your log heading - only if today's note exists",
                {}, true, settings_.pomodoroLog);
        }

        if (settingsCategory_ == SettingsCategory::Snippets) {
            DrawSettingsRow(kRowSnippetsEnabled, rowY(kRowSnippetsEnabled), L"Snippets (text expander)",
                L"Type a trigger like :sig in any app to expand it. Installs a keyboard listener while on",
                {}, true, settings_.enableSnippets);
            DrawSettingsRow(kRowSnippetsPrefix, rowY(kRowSnippetsPrefix), L"Snippets prefix",
                L"Type this followed by a space to search your snippets",
                settings_.snippetsPrefix, false, false, false, true);
            DrawSettingsRow(kRowSnippetsFile, rowY(kRowSnippetsFile), L"Snippets file",
                L"Full path to a .yml file. Leave empty for the default in %APPDATA%\\LeanLauncher",
                settings_.snippetsPath.empty() ? std::wstring(L"(default)") : settings_.snippetsPath, false, false, false, true);
            DrawSettingsRow(kRowSnippetsOpen, rowY(kRowSnippetsOpen), L"Edit snippets",
                L"Open the snippets file in your default editor",
                {}, false, false, true);
            DrawSettingsRow(kRowSnippetsImport, rowY(kRowSnippetsImport), L"Import from Espanso",
                L"Add your existing Espanso text matches (variables are skipped)",
                {}, false, false, true);
        }

        if (settingsCategory_ == SettingsCategory::Obsidian) {
            const auto& visibleRows = ObsidianVisibleRows();
            drawCard(L"OBSIDIAN", 16.0f, 36.0f, static_cast<int>(visibleRows.size()));
            for (size_t i = 0; i < visibleRows.size(); ++i) {
                DrawObsidianRow(visibleRows[i], 36.0f + static_cast<float>(i) * kSettingsRowHeight + offsetY);
            }
        }

        if (settingsCategory_ == SettingsCategory::About) {
            Text(std::wstring(L"Lean Launcher v") + takeoff::kAppVersion,
                D2D1::RectF(24, 16.0f + offsetY, width_ - 24, 34.0f + offsetY),
                resultFormat_.Get(), Foreground());
            Text(L"Made by LeanProductivity - Sascha D. Kasper",
                D2D1::RectF(24, 38.0f + offsetY, width_ - 24, 56.0f + offsetY),
                hintFormat_.Get(), Muted());

            drawCard(L"UPDATES", 66.0f, AboutUpdatesCardTop(), 2);
            DrawSettingsRow(kRowCheckForUpdatesOnStart, AboutUpdatesCardTop() + offsetY, L"Check on startup",
                L"Check for updates when Lean Launcher starts", {}, true, settings_.checkForUpdates);
            DrawSettingsRow(kRowAboutCheckUpdates, AboutUpdatesCardTop() + kSettingsRowHeight + offsetY, L"Check now",
                takeoff::FormatLastUpdateCheck(lastUpdateCheck_),
                takeoff::UpdateRowText(updateState_, updateTag_), false, false, true);

            drawCard(L"LINKS", AboutLinksHeaderTop(), AboutLinksCardTop(), 1);
            DrawSettingsRow(kRowAboutGithubLink, AboutLinksCardTop() + offsetY, L"View on GitHub",
                L"Open the Lean Launcher repository in your browser",
                L"github.com/sdkasper/lean-launcher", false, false, true);

            // Index counts (US-027): read-only lines, not selectable rows.
            // Both are snapshot sizes behind a lock - cheap to read per paint.
            // A first pass still in flight shows "Indexing..." rather than a
            // partial number (no live counter - US-018 AC #2 stays declined).
            std::wstring filesValue = L"Off";
            if (settings_.enableFileSearch) {
                const auto& fileIndex = FileIndex::Instance();
                filesValue = fileIndex.IsReady() ? FormatCount(fileIndex.Count()) : std::wstring(L"Indexing\u2026");
            }
            std::wstring notesValue = L"No vault";
            if (settings_.obsidianEnabled && !obsidianVaultPath_.empty()) {
                const auto& noteIndex = leanlauncher::obsidian::NoteIndex::Instance();
                notesValue = noteIndex.IsReady() ? FormatCount(noteIndex.Count()) : std::wstring(L"Indexing\u2026");
            }
            drawCard(L"INDEX", AboutIndexHeaderTop(), AboutIndexCardTop(), 2);
            const std::pair<const wchar_t*, const std::wstring*> indexLines[] = {
                {L"Indexed files", &filesValue},
                {L"Indexed notes", &notesValue},
            };
            for (int i = 0; i < 2; ++i) {
                const float lineTop = AboutIndexCardTop() + i * kSettingsRowHeight + offsetY;
                const auto lineRect = D2D1::RectF(32, lineTop, width_ - 32, lineTop + kSettingsRowHeight);
                Text(indexLines[i].first, lineRect, resultFormat_.Get(), Foreground());
                Text(*indexLines[i].second, lineRect, hintFormat_.Get(), Muted(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }

        target_->PopAxisAlignedClip();

        // Subtle modern scrollbar thumb if content exceeds viewport
        const float maxScroll = SettingsMaxScroll();
        if (maxScroll > 0.0f) {
            const float trackTop = kSettingsHeaderHeight + 6.0f;
            const float trackBottom = FooterTop() - 6.0f;
            const float trackHeight = trackBottom - trackTop;
            const float viewportHeight = SettingsViewportHeight();
            const float contentHeight = SettingsContentHeight();
            const float thumbHeight = (std::max)(28.0f, trackHeight * (viewportHeight / contentHeight));
            const float thumbTop = trackTop + (trackHeight - thumbHeight) * (settingsScroll_ / maxScroll);
            const auto thumbRect = D2D1::RectF(width_ - 7.0f, thumbTop, width_ - 3.0f, thumbTop + thumbHeight);
            Fill(thumbRect, highContrast_
                ? SystemColor(COLOR_HIGHLIGHT)
                : (settingsDraggingScroll_ ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(1, 1, 1, 0.18f)),
                2.0f);
        }

        // Fixed header drawn above scrollable content (subtle translucent tint - NO BLACK BARS!)
        Fill(D2D1::RectF(1, 1, width_ - 1, kSettingsHeaderHeight), highContrast_ ? SystemColor(COLOR_WINDOW) :
            D2D1::ColorF(0, 0, 0, 0.10f));
        Line(1, kSettingsHeaderHeight, width_ - 1, kSettingsHeaderHeight,
            D2D1::ColorF(1, 1, 1, 0.07f));

        // Back button with subtle hover feedback
        const auto backRect = D2D1::RectF(12.0f, 8.0f, 42.0f, 38.0f);
        const bool backHover = mouseKnown_ && mouseX_ >= backRect.left && mouseX_ <= backRect.right && mouseY_ >= backRect.top && mouseY_ <= backRect.bottom;
        if (backHover) {
            Fill(backRect, D2D1::ColorF(1, 1, 1, 0.08f), 6.0f);
        }
        const auto arrowColor = backHover ? Foreground() : Muted();
        Line(21, 23, 33, 23, arrowColor, 1.6f);
        Line(21, 23, 26, 18, arrowColor, 1.6f);
        Line(21, 23, 26, 28, arrowColor, 1.6f);

        // Header title
        Text(L"Settings", D2D1::RectF(48, 0, 150, kSettingsHeaderHeight),
            resultFormat_.Get(), Foreground());

        // Category tabs
        for (std::size_t i = 0; i < std::size(kSettingsCategories); ++i) {
            const auto cat = kSettingsCategories[i];
            const auto tabRect = CategoryTabRect(cat);
            const bool active = (settingsCategory_ == cat);
            const bool tabHover = mouseKnown_ && mouseX_ >= tabRect.left && mouseX_ <= tabRect.right && mouseY_ >= tabRect.top && mouseY_ <= tabRect.bottom;
            if (active) {
                Fill(tabRect, highContrast_ ? SystemColor(COLOR_HIGHLIGHT) : D2D1::ColorF(1, 1, 1, 0.12f), 12.0f);
                brush_->SetColor(highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : D2D1::ColorF(1, 1, 1, 0.20f));
                target_->DrawRoundedRectangle(D2D1::RoundedRect(tabRect, 12.0f, 12.0f), brush_.Get(), 1.0f);
            } else if (tabHover) {
                Fill(tabRect, D2D1::ColorF(1, 1, 1, 0.06f), 12.0f);
            }
            Text(kSettingsCategoryLabels[i], tabRect, hintFormat_.Get(),
                active ? (highContrast_ ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground()) : (tabHover ? Foreground() : Muted()),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Reset to default button
        const auto resetRect = ResetButtonRect();
        const bool resetSelected = (settingsSelected_ == kRowResetToDefaults);
        const bool resetHover = mouseKnown_ && mouseX_ >= resetRect.left && mouseX_ <= resetRect.right && mouseY_ >= resetRect.top && mouseY_ <= resetRect.bottom;
        const bool resetHighlight = resetSelected || resetHover;
        Fill(resetRect, highContrast_
            ? (resetHighlight ? SystemColor(COLOR_HIGHLIGHT) : SystemColor(COLOR_BTNFACE))
            : resetHighlight ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.04f), 5.0f);
        brush_->SetColor(highContrast_
            ? (resetHighlight ? SystemColor(COLOR_HIGHLIGHTTEXT) : Foreground())
            : resetHighlight ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(1, 1, 1, 0.10f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(resetRect, 5.0f, 5.0f), brush_.Get(), 1.0f);
        Text(L"Reset to default", resetRect, hintFormat_.Get(),
            highContrast_ && resetHighlight ? SystemColor(COLOR_HIGHLIGHTTEXT) : (resetHighlight ? Foreground() : Muted()),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        // Fixed footer drawn above scrollable content (subtle translucent tint - NO BLACK BARS!)
        const float top = FooterTop();
        Fill(D2D1::RectF(1, top, width_ - 1, height_ - 1), highContrast_ ? SystemColor(COLOR_WINDOW) :
            D2D1::ColorF(0, 0, 0, 0.10f));
        Line(1, top, width_ - 1, top, D2D1::ColorF(1, 1, 1, 0.07f));
        const std::wstring footerMsg = !settingsStatus_.empty() ? settingsStatus_
            : recordingRow_ >= 0
                ? (recordingRow_ >= 2
                    ? L"Press keys \u00B7 Backspace to disable \u00B7 Esc to cancel"
                    : L"Press keys \u00B7 Esc to cancel")
                : L"Changes are saved automatically";
        Text(footerMsg,
            D2D1::RectF(18, top, width_ - 18, height_), hintFormat_.Get(), Muted(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        const std::wstring versionText = std::wstring(L"v") + takeoff::kAppVersion;
        Text(versionText,
            D2D1::RectF(width_ - 120, top, width_ - 20, height_), hintFormat_.Get(), Muted(),
            DWRITE_TEXT_ALIGNMENT_TRAILING);

        DrawVaultDropdown();
        DrawWebSearchDropdown();
        DrawQuickOpenDropdown();
    }

    void Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        if (EnsureTarget()) {
            target_->BeginDraw();
            target_->Clear(highContrast_ ? SystemColor(COLOR_WINDOW) :
                acrylic_ ? D2D1::ColorF(0x171719, 0.66f) : D2D1::ColorF(0x252527));
            if (page_ == Page::Settings) {
                DrawSettings();
            } else {
                DrawSearch();
                DrawResults();
                DrawPreviewPanel();
                DrawFooter();
                DrawActions();
                if (ShouldShowHotkeyWarning()) {
                    DrawHotkeyWarningModal();
                }
            }
            brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.24f));
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, width_ + panelWidth_ - 0.5f, height_ - 0.5f), 8, 8),
                brush_.Get(), 1);
            const HRESULT result = target_->EndDraw();
            if (FAILED(result)) DiscardTarget();
        }
        EndPaint(hwnd_, &paint);
        if (!target_) SetTimer(hwnd_, kRenderRetryTimer, 250, nullptr);
    }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float width_ = kWidth;  // the launcher area; the window is width_ + panelWidth_ wide
    // US-045 preview panel: kPanelWidth in side mode, 0 when closed or when
    // the panel is drawn as an overlay over the results (previewOverlay_).
    float panelWidth_ = 0;
    bool previewOverlay_ = false;
    float height_ = 482;
    int visibleRows_ = kVisibleRows;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> searchFormat_, resultFormat_, hintFormat_, calcResultFormat_;
    ComPtr<IDWriteTextFormat> previewMonoFormat_;  // US-045: Text files in the preview panel
    std::unordered_map<std::wstring, IconEntry> iconCache_;
    std::thread iconThread_;
    std::mutex iconMutex_;
    std::condition_variable iconCv_;
    std::deque<IconRequest> iconQueue_;
    // US-041 system commands: the second-Enter gate and the Recycle Bin
    // row's count/size (-1 = not fetched since the launcher last showed).
    leanlauncher::syscmd::ConfirmGate commandGate_;
    long long recycleBinItems_ = -1;
    long long recycleBinBytes_ = 0;
    bool recycleBinQueryPending_ = false;
    bool modalDialogOpen_ = false;  // an export/import dialog is open (US-044)
    leanlauncher::timezones::ZoneCache timeZones_;  // US-048, empty until the first time query
    // US-049 Pomodoro: the running timer (none = no timers set), the pending
    // "replace running timer" confirmation, and the break offer on the balloon.
    std::optional<leanlauncher::pomodoro::State> pomodoro_;
    // US-050 snippets. The index is shared with the hook thread through
    // Expander; everything is released when the feature is turned off.
    std::shared_ptr<const leanlauncher::snippets::SnippetIndex> snippetIndex_;
    std::vector<std::wstring> snippetWarnings_;
    std::filesystem::file_time_type snippetsWriteTime_{};
    HWND snippetTarget_ = nullptr;  // the window that had focus before the launcher showed
    leanlauncher::snippets::Expander expander_;
    bool pomodoroReplacePending_ = false;
    std::wstring pomodoroReplacePath_;
    unsigned long long pomodoroReplaceAt_ = 0;
    bool pomodoroBreakOffer_ = false;
    bool balloonTempIcon_ = false;
    // US-043 path completion: the one cached folder listing (freed on Hide()
    // and when the feature is turned off) and the request in flight.
    std::wstring pathListedFolder_;
    std::vector<leanlauncher::typed::PathEntry> pathEntries_;
    DWORD pathError_ = 0;
    bool pathTruncated_ = false;
    bool pathListingReady_ = false;
    bool pathTimedOut_ = false;
    std::wstring pathRequestedFolder_;
    unsigned pathGeneration_ = 0;
    int pathThreadsRunning_ = 0;
    // US-045 preview (Task 6): the shown content, the stale-result gate, and
    // the workers still reading. All UI-thread only; freed by ClosePreview.
    std::unique_ptr<PreviewContent> preview_;
    leanlauncher::preview::PreviewGate previewGate_;
    int previewThreadsRunning_ = 0;
    std::wstring previewKey_;  // the row shown or loading (category|path|name|vault)
    float previewScroll_ = 0;
    // The body layout (built once per content and width) and the brushes its
    // drawing effects hold. Freed by ReleasePreviewLayout.
    ComPtr<IDWriteTextLayout> previewLayout_;
    ComPtr<ID2D1SolidColorBrush> previewAccent_, previewMuted_;
    float previewLayoutWidth_ = 0, previewBodyHeight_ = 0, previewMaxScroll_ = 0;
    std::vector<PreviewImage> previewImages_;  // US-046: at most 2 thumbnails
    std::unordered_set<std::wstring> iconPending_;
    std::vector<AppEntry> apps_;
    std::vector<size_t> results_, recent_;
    // Pinned results (US-024), newest first. pinnedAppIndex_[i] is pins_[i]'s
    // index into apps_ (npos for file pins, or an app missing from the
    // current scan); pinnedNames_[i] is its normalized file name, used to
    // match pinned files while typing. Both are rebuilt by RefreshPins().
    std::vector<leanlauncher::pins::Pin> pins_;
    std::vector<size_t> pinnedAppIndex_;
    std::vector<std::wstring> pinnedNames_;
    std::vector<std::wstring> recentPaths_;
    size_t baseAppsCount_ = 0;
    SearchInput input_;
    SearchInput settingsEdit_;  // scratch buffer for the Settings row currently being edited
    Settings settings_;
    std::wstring obsidianVaultPath_;
    leanlauncher::obsidian::DailyNoteConfig dailyNoteConfig_;
    std::vector<std::wstring> knownVaults_;  // <-- new in this task
    Page page_ = Page::Launcher;
    wchar_t pendingSurrogate_ = 0;
    std::wstring composition_, status_, settingsStatus_;
    int selected_ = 0, firstVisible_ = 0, actionSelected_ = 0, wheelDelta_ = 0;
    int settingsSelected_ = 0;
    SettingsCategory settingsCategory_ = SettingsCategory::General;
    int recordingRow_ = -1;
    int editingRow_ = -1;
    bool vaultDropdownOpen_ = false;
    int vaultDropdownHighlight_ = -1;  // index into knownVaults_ while the dropdown is open
    // First visible index into knownVaults_ when the list overflows the
    // settings viewport (e.g. a user with many vaults) - see
    // VaultDropdownMaxVisibleItems().
    int vaultDropdownScroll_ = 0;
    bool webSearchDropdownOpen_ = false;
    // Index into [0, kWebSearchPresetCount] while the dropdown is open -
    // kWebSearchPresetCount itself is the trailing "Custom" entry.
    int webSearchDropdownHighlight_ = -1;
    bool quickOpenDropdownOpen_ = false;
    int quickOpenDropdownHighlight_ = -1;  // index into QuickOpenTargetLabel while open
    int obsidianExpandedSection_ = -1;  // kSection* of the expanded action block, or -1
    // Memoized ObsidianVisibleRows() result; empty means "not built yet".
    mutable std::vector<int> obsidianRowsCache_;
    mutable bool obsidianRowsCacheEnabled_ = false;
    mutable int obsidianRowsCacheSection_ = -1;
    float textScroll_ = 0, caretX_ = kTextLeft, mouseX_ = 0, mouseY_ = 0;
    float settingsScroll_ = 0.0f;
    bool settingsDraggingScroll_ = false;
    bool acrylic_ = false, nativeCorners_ = false, highContrast_ = false;
    bool backdropApplied_ = false, allowBlur_ = false;
    bool indexReady_ = false, caretVisible_ = true, hotkeyRegistered_ = false;
    bool hotkeyWarningDismissed_ = false;
    bool trackingMouse_ = false, mouseKnown_ = false, dragging_ = false;
    bool actionsOpen_ = false, composing_ = false;
    bool actionsPositioned_ = false;
    bool trayIconAdded_ = false;
    bool iconStop_ = false;
    int hoverLockRow_ = -1;
    float actionsX_ = 0, actionsY_ = 0;
    bool updateAvailable_ = false;
    bool updateDownloaded_ = false;
    std::wstring downloadedUpdatePath_;
    // SHA-256 of the bytes received when downloadedUpdatePath_ was downloaded in
    // this session; empty when unknown. The elevated update helper checks it.
    std::string downloadedUpdateSha256_;
    // About tab update row (US-029). updateTag_/updateReleaseUrl_ come from
    // the latest check that reached GitHub.
    takeoff::UpdateCheckState updateState_ = takeoff::UpdateCheckState::Idle;
    std::wstring updateTag_;
    std::wstring updateReleaseUrl_;
    bool updateHovered_ = false;
    bool webSearchCardHovered_ = false;
    bool adminActionHovered_ = false;
    std::shared_ptr<std::atomic<bool>> updateInProgress_ = std::make_shared<std::atomic<bool>>(false);
    std::thread updateThread_;
    uint64_t lastUpdateCheck_ = 0;
    std::wstring releasesUrl_ = takeoff::kDefaultReleasesUrl;
    std::wstring apiHost_ = takeoff::kDefaultApiHost;
    std::wstring apiPath_ = takeoff::kDefaultApiPath;
    std::thread indexWorkerThread_;
    HANDLE indexStopEvent_ = nullptr;
    HANDLE indexTriggerEvent_ = nullptr;
    ULONG shellNotifyId_ = 0;
    std::chrono::steady_clock::time_point lastIndexTime_{};
};
