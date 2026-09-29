#pragma once

#include "snippets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The keyboard side of snippets (US-050). One opt-in low-level hook on its own
// thread watches typing; on a match a worker thread deletes the trigger and
// inserts the replacement. The hook callback only appends to a small buffer
// and does a table lookup, because a slow hook stalls typing system-wide.
// Nothing here runs unless Start() is called, and Stop() removes every trace.
namespace leanlauncher {
namespace snippets {

// Marks the keystrokes we inject, so the hook never reacts to its own output.
inline constexpr ULONG_PTR kInjectTag = 0x4C4C5350;  // "LLSP"

class Expander {
public:
    ~Expander() { Stop(); }

    bool Running() const { return running_.load(); }

    bool Start(HWND ignoreWindow, std::shared_ptr<const SnippetIndex> index) {
        if (running_.load()) {
            SetIndex(std::move(index));
            return true;
        }
        ignoreWindow_ = ignoreWindow;
        SetIndex(std::move(index));
        stop_ = false;
        stopRequested_ = false;
        std::promise<bool> ready;
        std::future<bool> installed = ready.get_future();
        hookThread_ = std::thread([this, ready = std::move(ready)]() mutable { HookMain(ready); });
        if (!installed.get()) {
            hookThread_.join();
            return false;
        }
        worker_ = std::thread([this] { WorkerMain(); });
        running_ = true;
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        stopRequested_ = true;  // cheap for Expand and PasteViaClipboard to observe
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stop_ = true;
            jobs_.clear();
        }
        jobCv_.notify_all();
        for (int attempt = 0; attempt < 20 && !PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0); ++attempt) Sleep(10);
        hookThread_.join();
        // The worker may be inside EmptyClipboard, which SENDs WM_DESTROYCLIPBOARD to the
        // owner window on this (UI) thread. Keep servicing sent messages while waiting,
        // or the two threads would deadlock.
        HANDLE workerHandle = static_cast<HANDLE>(worker_.native_handle());
        for (int waitedMs = 0; waitedMs < 5000; waitedMs += 50) {
            const DWORD r = MsgWaitForMultipleObjects(1, &workerHandle, FALSE, 50, QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0) break;  // the worker has finished
            if (r == WAIT_OBJECT_0 + 1) {
                MSG msg;
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // dispatches pending SENT messages only
            }
        }
        // If the wait ever timed out the worker is still running; it is never terminated,
        // join() simply waits for it (unreachable in practice: every step is bounded).
        worker_.join();
        SetIndex(nullptr);
    }

    void SetIndex(std::shared_ptr<const SnippetIndex> index) {
        std::lock_guard<std::mutex> lock(indexMutex_);
        index_ = std::move(index);
    }

    // Types or pastes `text` into the foreground window. Blocking (a paste
    // waits ~150 ms before restoring the clipboard): call from a worker.
    // `clipboardOwner` must be a real window (a NULL owner makes SetClipboardData fail).
    // `cancel` (optional) stops a paste before it touches the clipboard.
    static bool InsertText(HWND clipboardOwner, std::wstring_view text, const std::atomic<bool>* cancel = nullptr) {
        if (text.empty()) return true;
        if (PlanInsert(text) == InsertMode::Keystrokes) return SendUnicode(text);
        if (PasteViaClipboard(clipboardOwner, text, cancel)) return true;
        if (cancel && cancel->load()) return false;
        return SendUnicode(text);  // the clipboard holds non-text or is locked: never clobber it
    }

    // For the launcher search: the launcher has already hidden itself. Waits
    // for `target` to be the foreground window again, then inserts; if that
    // never happens the text is left on the clipboard instead.
    static void InsertIntoWindowAsync(HWND clipboardOwner, HWND target, std::wstring text) {
        std::thread([clipboardOwner, target, text = std::move(text)] {
            for (int waited = 0; waited < 500 && GetForegroundWindow() != target; waited += 10) Sleep(10);
            // Never inject with a held modifier: fall back to the clipboard instead.
            if (GetForegroundWindow() != target || !WaitFor(&ModifierStillDown, 0, 300) ||
                !InsertText(clipboardOwner, text)) {
                CopyPlain(clipboardOwner, text);
            }
        }).detach();
    }

private:
    struct Job {
        std::wstring replace;
        size_t triggerLength = 0;
        unsigned vk = 0;  // the key that completed the trigger
        unsigned keyCount = 0;     // keyDownCount_ right after that key was counted
        HWND foreground = nullptr; // foreground window at match time
    };

    // ---- hook thread ------------------------------------------------------
    // Only one Expander may exist per process (the hook callback is a plain function).
    static inline std::atomic<Expander*> active_{nullptr};

    static LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            if (Expander* self = active_.load()) {
                try {
                    self->OnKey(*reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam));
                } catch (...) {  // never let an exception escape a hook callback
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);  // never swallow a key
    }

    static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    void HookMain(std::promise<bool>& ready) {
        hookThreadId_ = GetCurrentThreadId();
        active_ = this;
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &HookProc, GetModuleHandleW(nullptr), 0);
        if (!hook) {
            active_ = nullptr;
            ready.set_value(false);
            return;
        }
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue before Stop() can post
        ready.set_value(true);
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        }  // low-level hooks are delivered while this loop pumps
        UnhookWindowsHookEx(hook);
        active_ = nullptr;
        buffer_.Clear();
    }

    void OnKey(const KBDLLHOOKSTRUCT& key) {
        // Only OUR tagged injections are ignored: keys injected by other tools
        // (PowerToys remaps, the touch keyboard) ARE buffered, deliberately.
        if ((key.flags & LLKHF_INJECTED) && key.dwExtraInfo == kInjectTag) return;
        // Modifier and lock keys are not counted: they never move the caret, so a
        // Shift pressed before the trigger's last key is up must not cancel the expansion.
        const unsigned keyCount = IsModifierVk(key.vkCode) ? keyDownCount_.load() : ++keyDownCount_;
        if (resetRequested_.exchange(false)) buffer_.Clear();

        HWND foreground = GetForegroundWindow();
        if (foreground == ignoreWindow_) {  // typing in the launcher itself
            buffer_.Clear();
            return;
        }
        if (foreground != lastForeground_) {
            buffer_.Clear();
            lastForeground_ = foreground;
        }
        if (IsModifierVk(key.vkCode)) return;

        const bool ctrl = Down(VK_CONTROL), alt = Down(VK_MENU), shift = Down(VK_SHIFT);
        const bool win = Down(VK_LWIN) || Down(VK_RWIN);
        if (ClassifyModifiers(ctrl, alt, win) == ModifierAction::Reset) {
            buffer_.Clear();
            return;
        }
        if (key.vkCode == VK_BACK) {
            buffer_.Backspace();
            return;
        }
        if (IsResetKey(key.vkCode)) {
            buffer_.Clear();
            return;
        }

        std::shared_ptr<const SnippetIndex> index;
        {
            std::lock_guard<std::mutex> lock(indexMutex_);
            index = index_;
        }
        if (!index || index->Empty()) return;

        wchar_t text[8]{};
        const int count = Translate(key, shift, ctrl, alt, foreground, text);
        if (count < 0) {  // a dead key: the next key composes with it, so stop guessing
            buffer_.Clear();
            return;
        }
        if (count == 0) return;
        for (int i = 0; i < count; ++i) {
            if (text[i] < L' ') {
                buffer_.Clear();
                return;
            }
        }
        buffer_.SetCapacity(index->MaxTriggerLength());
        buffer_.Append(std::wstring_view(text, static_cast<size_t>(count)));
        if (const Snippet* hit = index->MatchSuffix(buffer_.View())) {
            Job job{hit->replace, hit->trigger.size(), key.vkCode, keyCount, foreground};
            buffer_.Clear();
            {
                std::lock_guard<std::mutex> lock(jobMutex_);
                jobs_.push_back(std::move(job));
            }
            jobCv_.notify_one();
        }
    }

    // What the key would type on the foreground window's layout. Built from
    // async key state because the hook thread has no keyboard state of its own.
    static int Translate(const KBDLLHOOKSTRUCT& key, bool shift, bool ctrl, bool alt, HWND foreground, wchar_t* out) {
        BYTE state[256]{};
        if (shift) state[VK_SHIFT] = 0x80;
        if (ctrl) state[VK_CONTROL] = 0x80;
        if (alt) state[VK_MENU] = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
        const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
        // Flag 0x4: do not change the keyboard state (leave dead keys alone).
        return ToUnicodeEx(key.vkCode, key.scanCode, state, out, 7, 0x4, GetKeyboardLayout(thread));
    }

    // ---- worker thread ----------------------------------------------------
    void WorkerMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Expand(job);
            {
                std::lock_guard<std::mutex> lock(jobMutex_);
                jobs_.clear();  // keys typed while expanding are not part of a new trigger
            }
            resetRequested_ = true;
        }
    }

    static bool WaitFor(bool (*stillBusy)(unsigned), unsigned arg, int maxMs) {
        for (int waited = 0; waited <= maxMs; waited += 10) {
            if (!stillBusy(arg)) return true;
            Sleep(10);
        }
        return false;
    }
    static bool KeyStillDown(unsigned vk) { return Down(static_cast<int>(vk)); }
    static bool ModifierStillDown(unsigned) {
        return Down(VK_SHIFT) || Down(VK_CONTROL) || Down(VK_MENU) || Down(VK_LWIN) || Down(VK_RWIN);
    }

    void Expand(const Job& job) {
        // The hook fires on key-down, before the app has the last trigger
        // character. Wait for the key to come up (so the app has it), then for
        // modifiers, or the Backspaces would race the character or become
        // Shift+Backspace.
        if (!WaitFor(&KeyStillDown, job.vk, 300) || stopRequested_.load()) return;
        if (!WaitFor(&ModifierStillDown, 0, 300) || stopRequested_.load()) return;  // never inject with a held modifier
        // Another key (rollover) or a focus change since the match: the Backspaces
        // would delete the wrong text, so abandon the expansion.
        if (keyDownCount_.load() != job.keyCount || GetForegroundWindow() != job.foreground) return;
        if (stopRequested_.load()) return;
        if (!SendBackspaces(job.triggerLength)) return;
        if (stopRequested_.load()) return;
        InsertText(ignoreWindow_, job.replace, &stopRequested_);
    }

    // ---- injection --------------------------------------------------------
    static INPUT KeyEvent(WORD vk, WORD scan, DWORD flags) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = scan;
        in.ki.dwFlags = flags;
        in.ki.dwExtraInfo = kInjectTag;
        return in;
    }

    static bool Send(const std::vector<INPUT>& events) {
        if (events.empty()) return true;
        // One call, so real keystrokes cannot interleave with the injected ones.
        const UINT count = static_cast<UINT>(events.size());
        return SendInput(count, const_cast<INPUT*>(events.data()), sizeof(INPUT)) == count;
    }

    static bool SendBackspaces(size_t count) {
        std::vector<INPUT> events;
        for (size_t i = 0; i < count; ++i) {
            events.push_back(KeyEvent(VK_BACK, 0, 0));
            events.push_back(KeyEvent(VK_BACK, 0, KEYEVENTF_KEYUP));
        }
        return Send(events);
    }

    // One Unicode key event per UTF-16 unit (surrogate pairs go through as two
    // units); line breaks become Enter, tabs become Tab.
    static bool SendUnicode(std::wstring_view text) {
        std::vector<INPUT> events;
        for (size_t i = 0; i < text.size(); ++i) {
            const wchar_t ch = text[i];
            if (ch == L'\r') continue;  // a following \n carries the break
            if (ch == L'\n' || ch == L'\t') {
                const WORD vk = ch == L'\n' ? VK_RETURN : VK_TAB;
                events.push_back(KeyEvent(vk, 0, 0));
                events.push_back(KeyEvent(vk, 0, KEYEVENTF_KEYUP));
                continue;
            }
            events.push_back(KeyEvent(0, ch, KEYEVENTF_UNICODE));
            events.push_back(KeyEvent(0, ch, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
        }
        return Send(events);
    }

    static void SendCtrlV() {
        Send({KeyEvent(VK_CONTROL, 0, 0), KeyEvent('V', 0, 0), KeyEvent('V', 0, KEYEVENTF_KEYUP),
              KeyEvent(VK_CONTROL, 0, KEYEVENTF_KEYUP)});
    }

    // ---- clipboard --------------------------------------------------------
    // The owner must be a real window: with a NULL owner EmptyClipboard makes
    // SetClipboardData fail.
    static bool OpenClipboardRetry(HWND owner) {
        for (int i = 0; i < 10; ++i) {
            if (OpenClipboard(owner)) return true;
            Sleep(10);
        }
        return false;
    }

    // Allowlist: the clipboard (must be open) may be emptied and restored only
    // when it is empty or holds nothing but plain text. Any other format (files,
    // images, HTML, RTF, virtual files, app-private data) would be lost.
    static bool ClipboardHoldsOnlyText() {
        for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) {
            // Our own marker formats (flags only, recreated by SetText) do not count as content.
            if (f != CF_UNICODETEXT && f != CF_TEXT && f != CF_OEMTEXT && f != CF_LOCALE && !IsMarkerFormat(f)) return false;
        }
        return true;
    }

    static bool SetFormat(UINT format, const void* data, size_t bytes) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!mem) return false;
        void* p = GlobalLock(mem);
        if (!p) {  // never publish uninitialised memory
            GlobalFree(mem);
            return false;
        }
        memcpy(p, data, bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(format, mem)) {
            GlobalFree(mem);
            return false;
        }
        return true;
    }

    // The three marker formats SetText adds when `hidden`. One table, so the
    // names used to write them and to recognise them cannot drift.
    struct MarkerFormat {
        UINT id;
        DWORD value;
    };
    static const MarkerFormat* MarkerFormats() {
        static const MarkerFormat formats[] = {
            {RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), 1},
            {RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), 0},
            {RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), 0},
        };
        return formats;
    }
    static constexpr int kMarkerFormatCount = 3;

    static bool IsMarkerFormat(UINT f) {
        for (int i = 0; i < kMarkerFormatCount; ++i) {
            if (MarkerFormats()[i].id != 0 && MarkerFormats()[i].id == f) return true;
        }
        return false;
    }

    // Empties the (open) clipboard and sets `text`. With `hidden`, marks it so
    // clipboard history, cloud sync and clipboard monitors skip it.
    static bool SetText(const std::wstring& text, bool hidden) {
        EmptyClipboard();
        if (!SetFormat(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t))) return false;
        if (hidden) {
            for (int i = 0; i < kMarkerFormatCount; ++i) {
                const MarkerFormat& m = MarkerFormats()[i];
                if (m.id) SetFormat(m.id, &m.value, sizeof(m.value));
            }
        }
        return true;
    }

    static void CopyPlain(HWND owner, const std::wstring& text) {
        if (!OpenClipboardRetry(owner)) return;
        SetText(ToClipboardText(text), false);
        CloseClipboard();
    }

    // False when the clipboard could not be used without losing content (it
    // holds more than plain text, or it is locked); the caller then types instead.
    // `cancel`, when set, is checked before the clipboard is first modified. Once
    // it has been modified the restore always runs.
    static bool PasteViaClipboard(HWND owner, std::wstring_view text, const std::atomic<bool>* cancel = nullptr) {
        if (!OpenClipboardRetry(owner)) return false;
        if (!ClipboardHoldsOnlyText()) {
            CloseClipboard();
            return false;
        }
        std::wstring saved;
        bool hadText = false;
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            const HANDLE h = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* p = h ? static_cast<const wchar_t*>(GlobalLock(h)) : nullptr;
            if (!p) {  // text we cannot read back: emptying the clipboard would lose it
                CloseClipboard();
                return false;
            }
            const size_t maxChars = GlobalSize(h) / sizeof(wchar_t);  // do not trust a terminator
            size_t length = 0;
            while (length < maxChars && p[length] != 0) ++length;
            saved.assign(p, length);
            GlobalUnlock(h);
            hadText = true;
        }
        if (cancel && cancel->load()) {  // stopping: bail out before touching the clipboard
            CloseClipboard();
            return false;
        }
        if (!SetText(ToClipboardText(text), true)) {
            // Put back what was there before closing, so a failed set loses nothing.
            if (hadText) SetText(saved, true);
            else EmptyClipboard();
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        const DWORD sequence = GetClipboardSequenceNumber();  // after close: our own write is included
        SendCtrlV();
        Sleep(150);  // let the app read the clipboard before it is restored
        // Restore only if nobody else has written to the clipboard meanwhile.
        if (GetClipboardSequenceNumber() == sequence && OpenClipboardRetry(owner)) {
            if (hadText) SetText(saved, true);
            else EmptyClipboard();
            CloseClipboard();
        }
        return true;
    }

    // ---- state ------------------------------------------------------------
    std::atomic<bool> running_{false};
    std::thread hookThread_, worker_;
    DWORD hookThreadId_ = 0;
    HWND ignoreWindow_ = nullptr;
    HWND lastForeground_ = nullptr;  // hook thread only
    KeyBuffer buffer_;               // hook thread only
    std::atomic<bool> resetRequested_{false};
    std::atomic<unsigned> keyDownCount_{0};
    std::atomic<bool> stopRequested_{false};

    std::mutex indexMutex_;
    std::shared_ptr<const SnippetIndex> index_;

    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    bool stop_ = false;
};

}  // namespace snippets
}  // namespace leanlauncher
