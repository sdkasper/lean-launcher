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
            !CreateFormat(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, calcResultFormat_)) return false;

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
            FileIndex::Instance().Start(hwnd_);
            if (settings_.obsidianEnabled && !obsidianVaultPath_.empty()) {
                leanlauncher::obsidian::NoteIndex::Instance().Start(obsidianVaultPath_, hwnd_);
            }
        }
        SetTimer(hwnd_, kHotkeyTimer, 2000, nullptr);
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
            std::unique_ptr<std::wstring> pathPtr(reinterpret_cast<std::wstring*>(lParam));
            if (wParam == 2) {
                updateDownloaded_ = true;
                updateAvailable_ = true;
                if (pathPtr && !pathPtr->empty()) {
                    downloadedUpdatePath_ = *pathPtr;
                }
            } else if (wParam == 1) {
                updateAvailable_ = true;
                updateDownloaded_ = false;
            } else {
                updateAvailable_ = false;
                updateDownloaded_ = false;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd_)) Hide();
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
        case WM_MOUSEWHEEL:
            if (page_ == Page::Launcher && !actionsOpen_ && !results_.empty()) {
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
                const bool button = page_ == Page::Settings ||
                    ResultAtPoint(x, y) >= 0 || y >= FooterTop() || x > width_ - 84;
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

    void CheckForUpdatesAsync(bool force = false) {
        if constexpr (kUiTest) return;
        if (!settings_.checkForUpdates) return;
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        // Throttle to at most once per 24h unless explicitly forced (startup
        // used to pass force=true unconditionally, so this check was never
        // actually consulted and the update endpoint got hit on every launch).
        if (!force && !takeoff::ShouldCheckForUpdates(lastUpdateCheck_, now, settings_.checkForUpdates)) {
            return;
        }
        if (updateInProgress_->exchange(true)) {
            return; // Already checking or downloading, do not block UI
        }
        if (updateThread_.joinable()) {
            updateThread_.join();
        }
        lastUpdateCheck_ = now;
        SaveLastUpdateCheck(now);
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
                if (takeoff::QueryLatestReleaseInfo(host, path, tag, htmlUrl, assetUrl)) {
                    if (takeoff::IsNewerVersion(tag, takeoff::kAppVersion)) {
                        const std::wstring stagingPath = takeoff::GetUpdateStagingPath(tag);
                        // The handler takes ownership of the posted path, so
                        // only release it once the post is known to have
                        // succeeded - it fails if shutdown got there first.
                        if (!stagingPath.empty() && takeoff::ValidateExecutableFile(stagingPath)) {
                            auto p = std::make_unique<std::wstring>(stagingPath);
                            if (PostMessageW(hwnd, kUpdateCheckCompletedMessage, 2,
                                    reinterpret_cast<LPARAM>(p.get()))) {
                                p.release();
                            }
                            return;
                        }
                        if (!assetUrl.empty() && !stagingPath.empty()) {
                            if (takeoff::DownloadUpdateFile(assetUrl, stagingPath)) {
                                auto p = std::make_unique<std::wstring>(stagingPath);
                                if (PostMessageW(hwnd, kUpdateCheckCompletedMessage, 2,
                                        reinterpret_cast<LPARAM>(p.get()))) {
                                    p.release();
                                }
                                return;
                            }
                        }
                        PostMessageW(hwnd, kUpdateCheckCompletedMessage, 1, 0);
                        return;
                    }
                }
                PostMessageW(hwnd, kUpdateCheckCompletedMessage, 0, 0);
            });
        } catch (const std::system_error&) {
            *updateInProgress_ = false;
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
            if (!settings_.showTrayIcon) {
                RemoveTrayIcon();
                return;
            }
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
            wcscpy_s(data.szTip, L"Lean Launcher");
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
        if (!targetPath.empty() && takeoff::ApplyUpdateAndRestart(targetPath)) {
            DestroyWindow(hwnd_);
            return;
        }
        ShellExecuteW(nullptr, L"open", releasesUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void HandleTrayMessage(UINT message) {
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
        width_ = (std::min)(kWidth, ToDip(info.rcWork.right - info.rcWork.left) - 32.0f);
        visibleRows_ = std::clamp(static_cast<int>(
            (workHeight - 64 - kSearchHeight - kSectionHeight - kFooterHeight - 8) / kRowHeight),
            1, kVisibleRows);
        height_ = ResultsTop() + visibleRows_ * kRowHeight + 8 + kFooterHeight;
        const int width = ToPixel(width_), height = ToPixel(height_);
        const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
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

    enum class SettingsCategory : uint8_t { All, Shortcuts, System, Search, Obsidian, About };

    // Settings rows 0-3: keyboard shortcuts. 4-6: system. 7-14: search (7
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
    // position comes from ObsidianVisibleRows(), not the number. 44 is the
    // About tab's one row (not part of "All" - see IsRowInCategory). 45 is
    // the Reset button, handled as a sentinel row rather than a real
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
    static constexpr int kRowAboutGithubLink = 44;
    static constexpr int kSettingsMaxRow = 44;
    static constexpr int kRowResetToDefaults = 45;

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
        if (cat == SettingsCategory::All) {
            if (row < kRowObsidianEnabled) return row >= 0;
            return ObsidianRowRank(row) >= 0;
        }
        if (cat == SettingsCategory::Shortcuts) return row >= 0 && row <= 3;
        if (cat == SettingsCategory::System) return row >= 4 && row <= 6;
        if (cat == SettingsCategory::Search) return row >= 7 && row <= kRowFileSearchHelp;
        if (cat == SettingsCategory::Obsidian) return ObsidianRowRank(row) >= 0;
        if (cat == SettingsCategory::About) return row == kRowAboutGithubLink;
        return false;
    }

    int FirstRowInCategory(SettingsCategory cat) const {
        if (cat == SettingsCategory::Shortcuts) return 0;
        if (cat == SettingsCategory::System) return 4;
        if (cat == SettingsCategory::Search) return 7;
        if (cat == SettingsCategory::Obsidian) return kRowObsidianEnabled;
        if (cat == SettingsCategory::About) return kRowAboutGithubLink;
        return 0;
    }

    int LastRowInCategory(SettingsCategory cat) const {
        if (cat == SettingsCategory::Shortcuts) return 3;
        if (cat == SettingsCategory::System) return 6;
        if (cat == SettingsCategory::Search) return kRowFileSearchHelp;
        if (cat == SettingsCategory::About) return kRowAboutGithubLink;
        // Obsidian, and the fallback used for "All" (whose last row is
        // whatever the Obsidian section's current last row is).
        const auto& rows = ObsidianVisibleRows();
        return rows.empty() ? kRowObsidianEnabled : rows.back();
    }

    int NextSettingsRow(int current, int delta) const {
        std::vector<int> activeRows;
        for (int r = 0; r <= kSettingsMaxRow; ++r) {
            if (IsRowInCategory(r, settingsCategory_)) {
                activeRows.push_back(r);
            }
        }
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
        float w = 40.0f;
        if (cat == SettingsCategory::Shortcuts) {
            x = 160.0f + 40.0f + 6.0f;
            w = 82.0f;
        } else if (cat == SettingsCategory::System) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f;
            w = 68.0f;
        } else if (cat == SettingsCategory::Search) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f;
            w = 68.0f;
        } else if (cat == SettingsCategory::Obsidian) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f + 68.0f + 6.0f;
            w = 84.0f;  // wider than the old 68px "Vault" tab to fit "Obsidian"
        } else if (cat == SettingsCategory::About) {
            x = 160.0f + 40.0f + 6.0f + 82.0f + 6.0f + 68.0f + 6.0f + 68.0f + 6.0f + 84.0f + 6.0f;
            w = 58.0f;
        }
        return D2D1::RectF(x, y, x + w, y + h);
    }

    D2D1_RECT_F ResetButtonRect() const {
        return D2D1::RectF(width_ - 136.0f, 10.0f, width_ - 20.0f, 36.0f);
    }

    float SettingsContentBottom() const {
        if (settingsCategory_ == SettingsCategory::All) {
            // 855 = fixed header offset for the OBSIDIAN card in the All view
            // (Search card start 441 + 8 rows * 47 + 38 gap, see the Search
            // branch below); +16 bottom padding.
            return 855.0f + ObsidianRowCount() * kSettingsRowHeight + 16.0f;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 240.0f;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 193.0f;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            // 8 rows (File search, Web search, Search engine - US-016; File/Web/App
            // search prefix - US-017; Edit exclusions, Help - US-019) + 16 bottom padding.
            return 428.0f;
        } else if (settingsCategory_ == SettingsCategory::Obsidian) {
            // 36 header offset + N rows + 16 bottom padding.
            return 36.0f + ObsidianRowCount() * kSettingsRowHeight + 16.0f;
        } else if (settingsCategory_ == SettingsCategory::About) {
            // INDEX card (US-027) sits below the one-row LINKS card; 2 rows + 16 bottom padding.
            return AboutIndexCardTop() + 2 * kSettingsRowHeight + 16.0f;
        }
        return 200.0f;
    }

    // About tab layout: version/author lines, "LINKS" header@66, its one-row
    // card@86, then the "INDEX" header 18px below that card and its card 20px
    // below the header - the same header/card spacing the All view uses.
    static constexpr float AboutIndexHeaderTop() { return 86.0f + kSettingsRowHeight + 18.0f; }
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
        if (settingsCategory_ == SettingsCategory::All) {
            if (row < 4) return 36.0f + row * kSettingsRowHeight;
            if (row < 7) return 262.0f + (row - 4) * kSettingsRowHeight;
            if (row < kRowObsidianEnabled) return 441.0f + (row - 7) * kSettingsRowHeight;
            // Obsidian section, All-view only: header@835, card@855.
            return 855.0f + ObsidianRowRank(row) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Shortcuts) {
            return 36.0f + row * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::System) {
            return 36.0f + (row - 4) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Search) {
            return 36.0f + (row - 7) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::Obsidian) {
            return 36.0f + ObsidianRowRank(row) * kSettingsRowHeight;
        } else if (settingsCategory_ == SettingsCategory::About) {
            return 86.0f;
        }
        return 0.0f;
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
        float sectionHeaderTop = rTop;
        if (settingsCategory_ == SettingsCategory::All) {
            if (row == 0) sectionHeaderTop = 16.0f;
            else if (row == 4) sectionHeaderTop = 242.0f;
            else if (row == 7) sectionHeaderTop = 421.0f;
            else if (row == kRowObsidianEnabled) sectionHeaderTop = 835.0f;
        } else {
            if (row == 0 || row == 4 || row == 7 || row == kRowObsidianEnabled || row == kRowAboutGithubLink) sectionHeaderTop = 16.0f;
        }
        const float visibleTop = sectionHeaderTop;
        const float visibleBottom = rBottom + 8.0f;

        if (visibleTop - settingsScroll_ < 2.0f) {
            settingsScroll_ = (std::max)(0.0f, visibleTop - 2.0f);
        } else if (visibleBottom - settingsScroll_ > SettingsViewportHeight() - 2.0f) {
            settingsScroll_ = (std::min)(maxScroll, visibleBottom - (SettingsViewportHeight() - 2.0f));
        }
    }

    void OpenSettings(SettingsCategory targetCategory = SettingsCategory::All) {
        page_ = Page::Settings;
        actionsOpen_ = false;
        dragging_ = false;
        settingsCategory_ = targetCategory;
        settingsSelected_ = (targetCategory == SettingsCategory::All) ? 0 : FirstRowInCategory(targetCategory);
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
        composing_ = false;
        pendingSurrogate_ = 0;
        composition_.clear();
        actionsOpen_ = false;
        adminActionHovered_ = false;
        dragging_ = false;
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
        ResetCaret();
        UpdateResults();
    }

    void UpdateResults() {
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
                if (std::find(recent_.begin(), recent_.end(), i) == recent_.end() && PinRank(i) < 0) {
                    results_.push_back(i);
                }
            }
        } else if (!query.empty()) {
            std::vector<RankedResult> ranked;
            ranked.reserve(apps_.size());
            if (!fileSearchOnly) {
                for (size_t i = 0; i < apps_.size(); ++i) {
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
        selected_ = std::clamp(selected_, 0, (std::max)(0, static_cast<int>(results_.size()) - 1));
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        LockHoverAtPointer();
        selected_ = wrap ? (selected_ + delta + count) % count
                         : std::clamp(selected_ + delta, 0, count - 1);
        EnsureVisible();
        PrepareVisibleIcons();
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        RegisterShortcut();
        SaveSettings();
        UpdateTrayIcon();
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
        default: return nullptr;
        }
    }

    bool IsPrefixRow(int row) const {
        return row == kRowVaultSearchPrefix || row == kRowTaskPrefix || row == kRowNoteAddPrefix ||
            row == kRowLogPrefix || row == kRowWebSearchPrefix || row == kRowFileSearchPrefix ||
            row == kRowAppSearchPrefix;
    }

    bool IsTargetNoteRow(int row) const {
        return row == kRowTaskTargetNote || row == kRowNoteAddTargetNote || row == kRowLogTargetNote;
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
        if (IsPrefixRow(editingRow_)) {
            std::wstring vaultSearchCandidate = settings_.vaultSearchPrefix;
            std::wstring taskCandidate = settings_.taskPrefix;
            std::wstring noteAddCandidate = settings_.noteAddPrefix;
            std::wstring logCandidate = settings_.logPrefix;
            std::wstring webSearchCandidate = settings_.webSearchPrefix;
            std::wstring fileSearchCandidate = settings_.fileSearchPrefix;
            std::wstring appSearchCandidate = settings_.appSearchPrefix;
            if (editingRow_ == kRowVaultSearchPrefix) vaultSearchCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowTaskPrefix) taskCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowNoteAddPrefix) noteAddCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowLogPrefix) logCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowWebSearchPrefix) webSearchCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowFileSearchPrefix) fileSearchCandidate = settingsEdit_.text;
            else if (editingRow_ == kRowAppSearchPrefix) appSearchCandidate = settingsEdit_.text;
            if (const wchar_t* error = leanlauncher::obsidian::FindPrefixConflict(
                    {vaultSearchCandidate, taskCandidate, noteAddCandidate, logCandidate,
                     webSearchCandidate, fileSearchCandidate, appSearchCandidate})) {
                settingsStatus_ = error;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;  // stay in edit mode so the user can fix it
            }
        }
        if (editingRow_ == kRowWebSearchEngine) {
            if (const wchar_t* error = takeoff::FindWebSearchUrlError(settingsEdit_.text)) {
                settingsStatus_ = error;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;  // stay in edit mode so the user can fix it
            }
        }
        // Target notes (US-025): empty/whitespace clears the target (back to
        // the daily note); anything else must normalize to a note inside the
        // vault, or the edit is rejected rather than silently falling back.
        const bool isTargetNoteRow = IsTargetNoteRow(editingRow_);
        std::wstring normalizedTarget;
        if (isTargetNoteRow &&
            settingsEdit_.text.find_first_not_of(L" \t") != std::wstring::npos &&
            !leanlauncher::obsidian::NormalizeTargetNoteRef(settingsEdit_.text, normalizedTarget)) {
            settingsStatus_ = L"Use a note path inside the vault, e.g. Inbox/Tasks";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;  // stay in edit mode so the user can fix it
        }
        const bool wasLogHeadingRow = (editingRow_ == kRowLogHeading);
        const bool wasWebSearchEngineRow = (editingRow_ == kRowWebSearchEngine);
        *field = isTargetNoteRow ? normalizedTarget : settingsEdit_.text;
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
        if (row == kRowAboutGithubLink) {
            ShellExecuteW(nullptr, L"open", takeoff::kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
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
        if (std::wstring* field = SettingsTextFieldForRow(row)) {
            BeginEditingRow(row, *field);
            return;
        }
        if (row < 4) {
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
                updateAvailable_ = false;
                updateDownloaded_ = false;
            } else {
                CheckForUpdatesAsync(true);
            }
            break;
        case 7:
            settings_.enableFileSearch = !settings_.enableFileSearch;
            SaveSettings();
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
        if (recordingRow_ <= 1) {
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
            if (recordingRow_ == 0) RegisterShortcut();
            recordingRow_ = -1;
            SaveSettings();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (recordingRow_ <= 1 && quicklaunch::IsSystemReserved(proposed.modifiers, proposed.key)) {
            settingsStatus_ = L"That shortcut is reserved by Windows.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (recordingRow_ == 2 && quicklaunch::IsSystemReserved(proposed.modifiers, VK_RETURN)) {
            settingsStatus_ = L"That shortcut is reserved by Windows.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (recordingRow_ == 1 && quicklaunch::IsReservedInApp(proposed)) {
            settingsStatus_ = L"That shortcut is reserved for text editing.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (const wchar_t* conflict = quicklaunch::HasInternalConflict(recordingRow_, proposed, settings_)) {
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
        }

        recordingRow_ = -1;
        SaveSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool MatchesActionsHotkey(WPARAM key, bool control, bool shift, bool alt) const {
        if (settings_.actionsHotkey.disabled || settings_.actionsHotkey.key == 0) return false;
        uint16_t mods = 0;
        if (control) mods |= quicklaunch::kModControl;
        if (alt) mods |= quicklaunch::kModAlt;
        if (shift) mods |= quicklaunch::kModShift;
        return mods == settings_.actionsHotkey.modifiers &&
               static_cast<uint16_t>(key) == settings_.actionsHotkey.key;
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
            LaunchSelected(MatchesAdministratorHotkey(control, shift, alt)); return 0;
        case VK_UP: MoveSelection(-1, true); return 0;
        case VK_DOWN: MoveSelection(1, true); return 0;
        case VK_TAB: MoveSelection(shift ? -1 : 1, true); return 0;
        case VK_PRIOR: MoveSelection(-visibleRows_, false); return 0;
        case VK_NEXT: MoveSelection(visibleRows_, false); return 0;
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
                // strand the user on an unrelated "All" settings view.
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
        if (page_ != Page::Launcher) return -1;
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
                const SettingsCategory categories[] = {
                    SettingsCategory::All,
                    SettingsCategory::Shortcuts,
                    SettingsCategory::System,
                    SettingsCategory::Search,
                    SettingsCategory::Obsidian,
                    SettingsCategory::About
                };
                for (auto cat : categories) {
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
        } else if (y >= FooterTop() && x < width_ / 2) {
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
        }
    }

    bool CreateFormat(float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& format) {
        if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight,
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

                Text(L"Calculation", D2D1::RectF(textLeft, top + 8, leftTextRight, top + 26),
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
                Text(app.name, D2D1::RectF(60, top, width_ - 158, top + 40), resultFormat_.Get(), textColor);
                const bool pinned = PinRank(results_[i]) >= 0;
                const bool recent = input_.text.empty() &&
                    std::find(recent_.begin(), recent_.end(), results_[i]) != recent_.end();
                const wchar_t* categoryLabel = pinned ? L"Pinned" : recent ? L"Recent"
                    : (app.category == takeoff::AppCategory::System ? L"System"
                    : (app.category == takeoff::AppCategory::Folder ? L"Folder"
                    : (app.category == takeoff::AppCategory::File ? L"File"
                    : (app.category == takeoff::AppCategory::TaskAdd ? settings_.taskPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteAdd ? settings_.noteAddPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::LogAdd ? settings_.logPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::NoteJump ? settings_.vaultSearchPillLabel.c_str()
                    : (app.category == takeoff::AppCategory::WebSearch ? settings_.webSearchPillLabel.c_str() : L"Application"))))))));
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
        const wchar_t** labels = isCalc ? calcLabels
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
            DrawSettingsRow(row, top, L"Daily note format override", L"Leave empty to auto-detect; supports YYYY/MM/DD tokens",
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

        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Shortcuts) {
            const float hY = 16.0f;
            const float cY = 36.0f;
            drawCard(L"KEYBOARD SHORTCUTS", hY, cY, 4);

            DrawSettingsRow(0, cY + offsetY, L"Open Lean Launcher",
                L"Global shortcut that opens or closes the launcher",
                quicklaunch::FormatBinding(settings_.launcherHotkey));
            DrawSettingsRow(1, cY + kSettingsRowHeight + offsetY, L"Actions menu",
                L"Show actions for the selected application",
                quicklaunch::FormatBinding(settings_.actionsHotkey));
            DrawSettingsRow(2, cY + 2 * kSettingsRowHeight + offsetY, L"Open as administrator",
                L"Launch the selected application with elevation",
                quicklaunch::FormatAdminBinding(settings_.administratorHotkey));
            DrawSettingsRow(3, cY + 3 * kSettingsRowHeight + offsetY, L"Quick launch",
                L"Open one of the eight visible results directly",
                quicklaunch::FormatQuickLaunchBinding(settings_.quickLaunchHotkey));
        }

        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::System) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 242.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 262.0f : 36.0f;
            drawCard(L"SYSTEM", hY, cY, 3);

            DrawSettingsRow(4, cY + offsetY, L"Run at startup",
                L"Start Lean Launcher when you sign in to Windows", {}, true, settings_.runAtStartup);
            DrawSettingsRow(5, cY + kSettingsRowHeight + offsetY, L"Notification area icon",
                L"Show Lean Launcher in the hidden icons area", {}, true, settings_.showTrayIcon);
            DrawSettingsRow(6, cY + 2 * kSettingsRowHeight + offsetY, L"Check for updates",
                L"Check for updates when Lean Launcher starts", {}, true, settings_.checkForUpdates);
        }

        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Search) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 421.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 441.0f : 36.0f;
            drawCard(L"SEARCH & FEATURES", hY, cY, 8);

            DrawSettingsRow(7, cY + offsetY, L"File search",
                L"Search files and folders on your computer", {}, true, settings_.enableFileSearch);
            DrawSettingsRow(8, cY + kSettingsRowHeight + offsetY, L"Web search",
                L"Open " + settings_.webSearchEngineName + L" when no results match your query",
                {}, true, settings_.enableWebSearch);
            DrawSettingsRow(kRowWebSearchEngine, cY + 2 * kSettingsRowHeight + offsetY, L"Search engine",
                webSearchDropdownOpen_ ? L"Tap to collapse"
                    : L"Search engine used for the \"Web search\" fallback",
                settings_.webSearchEngineName, false, false, false, true);
            DrawSettingsRow(kRowFileSearchPrefix, cY + 3 * kSettingsRowHeight + offsetY, L"File search prefix",
                L"Type this followed by a space to show only files and folders",
                settings_.fileSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowWebSearchPrefix, cY + 4 * kSettingsRowHeight + offsetY, L"Web search prefix",
                L"Type this followed by a space to force a \"" + settings_.webSearchEngineName + L"\" search",
                settings_.webSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowAppSearchPrefix, cY + 5 * kSettingsRowHeight + offsetY, L"App search prefix",
                L"Type this followed by a space to show only installed apps",
                settings_.appSearchPrefix, false, false, false, true);
            DrawSettingsRow(kRowFileSearchEditExclusions, cY + 6 * kSettingsRowHeight + offsetY, L"Edit exclusions...",
                L"Add your own folder and file-type exclusions on top of the built-in list",
                {}, false, false, true);
            DrawSettingsRow(kRowFileSearchHelp, cY + 7 * kSettingsRowHeight + offsetY, L"Help",
                L"Learn how file search exclusions work",
                {}, false, false, true);
        }

        if (settingsCategory_ == SettingsCategory::All || settingsCategory_ == SettingsCategory::Obsidian) {
            const float hY = (settingsCategory_ == SettingsCategory::All) ? 835.0f : 16.0f;
            const float cY = (settingsCategory_ == SettingsCategory::All) ? 855.0f : 36.0f;
            const auto& visibleRows = ObsidianVisibleRows();
            drawCard(L"OBSIDIAN", hY, cY, static_cast<int>(visibleRows.size()));
            for (size_t i = 0; i < visibleRows.size(); ++i) {
                DrawObsidianRow(visibleRows[i], cY + static_cast<float>(i) * kSettingsRowHeight + offsetY);
            }
        }

        if (settingsCategory_ == SettingsCategory::About) {
            Text(std::wstring(L"Lean Launcher v") + takeoff::kAppVersion,
                D2D1::RectF(24, 16.0f + offsetY, width_ - 24, 34.0f + offsetY),
                resultFormat_.Get(), Foreground());
            Text(L"Made by LeanProductivity - Sascha D. Kasper",
                D2D1::RectF(24, 38.0f + offsetY, width_ - 24, 56.0f + offsetY),
                hintFormat_.Get(), Muted());

            drawCard(L"LINKS", 66.0f, 86.0f, 1);
            DrawSettingsRow(kRowAboutGithubLink, 86.0f + offsetY, L"View on GitHub",
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
        const SettingsCategory categories[] = {
            SettingsCategory::All,
            SettingsCategory::Shortcuts,
            SettingsCategory::System,
            SettingsCategory::Search,
            SettingsCategory::Obsidian,
            SettingsCategory::About
        };
        const wchar_t* catLabels[] = {L"All", L"Shortcuts", L"System", L"Search", L"Obsidian", L"About"};
        for (int i = 0; i < 6; ++i) {
            const auto cat = categories[i];
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
            Text(catLabels[i], tabRect, hintFormat_.Get(),
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
                DrawFooter();
                DrawActions();
                if (ShouldShowHotkeyWarning()) {
                    DrawHotkeyWarningModal();
                }
            }
            brush_->SetColor(highContrast_ ? Foreground() : D2D1::ColorF(1, 1, 1, 0.24f));
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, width_ - 0.5f, height_ - 0.5f), 8, 8),
                brush_.Get(), 1);
            const HRESULT result = target_->EndDraw();
            if (FAILED(result)) DiscardTarget();
        }
        EndPaint(hwnd_, &paint);
        if (!target_) SetTimer(hwnd_, kRenderRetryTimer, 250, nullptr);
    }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float width_ = kWidth;
    float height_ = 482;
    int visibleRows_ = kVisibleRows;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> searchFormat_, resultFormat_, hintFormat_, calcResultFormat_;
    std::unordered_map<std::wstring, IconEntry> iconCache_;
    std::thread iconThread_;
    std::mutex iconMutex_;
    std::condition_variable iconCv_;
    std::deque<IconRequest> iconQueue_;
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
    SettingsCategory settingsCategory_ = SettingsCategory::All;
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
