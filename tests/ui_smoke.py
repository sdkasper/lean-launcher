"""Interactive Windows smoke test. Captures only our fixture window.

Usage: py tests/ui_smoke.py build/Release/TakeoffUiTests.exe
Requires Pillow and tkinter. Does not launch apps or use the clipboard.
"""

import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import subprocess
import sys
import time
import tkinter as tk

from PIL import ImageChops, ImageGrab, ImageStat

u = C.WinDLL("user32", use_last_error=True)
dwm = C.WinDLL("dwmapi")
u.SetProcessDpiAwarenessContext.argtypes = [W.HANDLE]
u.SetProcessDpiAwarenessContext(C.c_void_p(-4))
u.FindWindowW.argtypes = [W.LPCWSTR, W.LPCWSTR]
u.FindWindowW.restype = W.HWND
u.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SendMessageW.restype = C.c_ssize_t
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.GetDpiForWindow.argtypes = [W.HWND]
u.SetForegroundWindow.argtypes = [W.HWND]
u.GetForegroundWindow.restype = W.HWND
u.IsWindowVisible.argtypes = [W.HWND]
u.GetWindowLongW.argtypes = [W.HWND, C.c_int]
u.GetGUIThreadInfo.argtypes = [W.DWORD, C.c_void_p]
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.UpdateWindow.argtypes = [W.HWND]
dwm.DwmGetWindowAttribute.argtypes = [W.HWND, W.DWORD, C.c_void_p, W.DWORD]


class GUIInfo(C.Structure):
    _fields_ = [("cbSize", W.DWORD), ("flags", W.DWORD),
                ("hwndActive", W.HWND), ("hwndFocus", W.HWND),
                ("hwndCapture", W.HWND), ("hwndMenuOwner", W.HWND),
                ("hwndMoveSize", W.HWND), ("hwndCaret", W.HWND),
                ("rcCaret", W.RECT)]


def check(condition, description):
    if not condition:
        raise AssertionError(description)
    print("PASS:", description, flush=True)


def pump(seconds=0.12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        background.update()
        time.sleep(0.01)


def key(code):
    u.SendMessageW(hwnd, 0x100, code, 0)
    pump()


def type_text(text):
    for char in text:
        u.SendMessageW(hwnd, 0x102, ord(char), 0)
    pump()


def query():
    text = C.create_unicode_buffer(2048)
    u.SendMessageW(hwnd, 0xD, len(text), C.addressof(text))
    return text.value


def click(x, y):
    point = int(x * scale) | (int(y * scale) << 16)
    u.SendMessageW(hwnd, 0x201, 1, point)
    u.SendMessageW(hwnd, 0x202, 0, point)
    pump()


def capture(name=None):
    check(u.GetForegroundWindow() == hwnd, "fixture window owns focus")
    u.UpdateWindow(hwnd)
    dwm.DwmFlush()
    image = ImageGrab.grab(bbox=bounds)
    if name:
        image.save(output / name)
    return image


exe = Path(sys.argv[1]).resolve()
check(exe.name in ("TakeoffUiTests.exe", "QuickLaunchUiTests.exe"), "isolated test executable")
check(not u.FindWindowW("TakeoffTestWindow", None), "no existing test instance")
output = exe.parent.parent / "ui-smoke"
output.mkdir(exist_ok=True)
background = tk.Tk()
background.title("Takeoff test backdrop")
background.overrideredirect(True)
background.geometry(f"{u.GetSystemMetrics(0)}x{u.GetSystemMetrics(1)}+0+0")
canvas = tk.Canvas(background, highlightthickness=0, bg="#8998ac")
canvas.pack(fill="both", expand=True)
canvas.create_rectangle(0, 0, 960, 1080, fill="#7651bd", outline="")
canvas.create_rectangle(960, 0, 2200, 1400, fill="#55aa99", outline="")
for x in range(0, 2200, 12):
    canvas.create_rectangle(x, 0, x + 6, 1400, fill="#d6ced9", outline="")
pump()
process = subprocess.Popen([str(exe)])
try:
    hwnd = None
    for _ in range(100):
        hwnd = u.FindWindowW("TakeoffTestWindow", None)
        if hwnd and u.IsWindowVisible(hwnd):
            break
        pump(0.05)
    check(bool(hwnd), "fixture window created")
    u.keybd_event(0x12, 0, 0, 0)
    u.keybd_event(0x12, 0, 2, 0)
    u.SetForegroundWindow(hwnd)
    pump(1.0)
    rect = W.RECT()
    u.GetWindowRect(hwnd, C.byref(rect))
    bounds = (rect.left, rect.top, rect.right, rect.bottom)
    scale = u.GetDpiForWindow(hwnd) / 96
    check(not (u.GetWindowLongW(hwnd, -20) & 0x80000), "window is not layered")
    backdrop = W.DWORD()
    result = dwm.DwmGetWindowAttribute(hwnd, 38, C.byref(backdrop), C.sizeof(backdrop))
    print(f"Backdrop: HRESULT={result}, type={backdrop.value} (3 = native acrylic)", flush=True)
    check(query() == "", "empty search on open")
    capture("01-empty.png")

    # Drive the same timer message deterministically rather than race the OS blink.
    u.KillTimer.argtypes = [W.HWND, C.c_size_t]
    u.KillTimer(hwnd, 1)
    a = capture("caret-before.png")
    u.SendMessageW(hwnd, 0x113, 1, 0)
    pump(0.2)
    b = capture("caret-after.png")
    info = GUIInfo()
    info.cbSize = C.sizeof(info)
    u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), C.byref(info))
    print(f"Focus={info.hwndFocus}, test={hwnd}, caret=({info.rcCaret.left}, {info.rcCaret.top})", flush=True)
    caret_box = tuple(int(v * scale) for v in (47, 19, 52, 46))
    check(ImageChops.difference(a.crop(caret_box), b.crop(caret_box)).getbbox() is not None,
          "caret visibly blinks in empty search")

    type_text("cal")
    capture("02-results.png")
    check(query() == "cal", "typing updates search")
    before = capture()
    key(0x28)
    after = capture()
    rows = tuple(int(v * scale) for v in (8, 96, 735, 180))
    check(ImageChops.difference(before.crop(rows), after.crop(rows)).getbbox() is not None,
          "arrow navigation changes highlighted result")
    key(0x24)  # Home
    type_text("x")
    check(query() == "xcal", "Home inserts at start")
    key(0x2E)  # Delete
    check(query() == "xal", "Delete removes next character")
    key(0x23)  # End
    key(0x25)  # Left
    type_text("z")
    check(query() == "xazl", "caret inserts in middle")
    key(0x1B)
    check(query() == "" and u.IsWindowVisible(hwnd), "Escape clears before dismissing")
    type_text("zzzzzzzz")
    capture("03-no-results.png")
    no_results_rect = W.RECT()
    u.GetWindowRect(hwnd, C.byref(no_results_rect))
    check(no_results_rect.bottom - no_results_rect.top == rect.bottom - rect.top,
          "no-results state preserves panel height")
    click(680, 32)
    check(query() == "", "clear button resets query")

    # Open the settings page from the search header and exercise keyboard editing.
    click(722, 32)
    settings_before = capture("04-settings.png")
    key(0x27)  # Right cycles the selected launcher shortcut.
    settings_after = capture()
    settings_rows = tuple(int(v * scale) for v in (8, 46, 742, 272))
    check(ImageChops.difference(settings_before.crop(settings_rows),
                                settings_after.crop(settings_rows)).getbbox() is not None,
          "settings values can be changed with the keyboard")
    key(0x1B)
    check(u.IsWindowVisible(hwnd) and query() == "", "Escape returns from settings to search")

    # Click the actual footer action, then exercise menu navigation and dismissal.
    footer_y = (rect.bottom - rect.top) / scale - 21
    click(640, footer_y)
    capture("05-actions.png")
    key(0x28)
    key(0x1B)
    check(u.IsWindowVisible(hwnd) and query() == "", "Escape dismisses actions only")
    key(0x22)  # Page Down
    capture("06-scrolled.png")
    type_text("w" * 100)
    info = GUIInfo()
    info.cbSize = C.sizeof(info)
    u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), C.byref(info))
    check(48 * scale <= info.rcCaret.left <= 659 * scale,
          "long-query caret stays within search viewport")
    capture("07-long-query.png")
    key(0x1B)

    # Acrylic must pick up the backdrop color but suppress its high-frequency stripes.
    image = capture()
    sample = image.crop(tuple(int(v * scale) for v in (400, 325, 550, 385)))
    deviation = ImageStat.Stat(sample).stddev
    check(max(deviation) < 16, "backdrop stripes are blurred rather than simply transparent")
    canvas.configure(bg="#bc663f")
    canvas.itemconfigure("all", fill="#bc663f")
    pump(0.8)
    changed = capture("08-warm-backdrop.png")
    diff = ImageStat.Stat(ImageChops.difference(image.crop(tuple(int(v * scale) for v in (400, 325, 550, 385))),
                                              changed.crop(tuple(int(v * scale) for v in (400, 325, 550, 385))))).mean
    if backdrop.value == 3:
        check(max(diff) > 2, "native acrylic responds to live backdrop color")

    # Exercise DPI resource recreation without changing the user's display settings.
    u.SendMessageW(hwnd, 0x02E0, 144 | (144 << 16), C.addressof(rect))
    pump(0.4)
    scaled_rect = W.RECT()
    u.GetWindowRect(hwnd, C.byref(scaled_rect))
    check(scaled_rect.right - scaled_rect.left == 1125, "150% DPI layout scales consistently")
    bounds = (scaled_rect.left, scaled_rect.top, scaled_rect.right, scaled_rect.bottom)
    capture("09-scaled-150.png")
    u.SendMessageW(hwnd, 0x02E0, 96 | (96 << 16), C.addressof(rect))
    pump()

    key(0x1B)
    check(not u.IsWindowVisible(hwnd), "Escape dismisses empty launcher")
    print(f"UI smoke tests passed. Screenshots: {output}", flush=True)
finally:
    if process.poll() is None:
        u.PostMessageW(hwnd, 0x10, 0, 0)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
    background.destroy()
