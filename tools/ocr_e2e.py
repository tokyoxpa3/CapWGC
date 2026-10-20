"""End-to-end check: snip + OCR through the real UI, no Python backend.

Flow (all Win32 messages, no Python OCR involved):
  1. press Ctrl+Alt+X  -> app enters snip mode (it registered that hotkey)
  2. click the OCR segment on the floating toolbar -> switches action to OCR
  3. post a drag to the overlay window -> crop + submit
  4. app runs the in-process ONNX engine and copies the text to the clipboard
  5. read gui.log for "ocr request id=" / "ocr ok lines="

The clipboard is also read back at the end, which is the thing the user
actually cares about.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

u = ctypes.windll.user32
k = ctypes.windll.kernel32
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_X = 0x58
KEYEVENTF_KEYUP = 0x0002


def cls(h):
    buf = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, buf, 256)
    return buf.value


def find(name):
    out = []

    @EnumWindowsProc
    def cb(h, _):
        if cls(h) == name:
            out.append(h)
        return True

    u.EnumWindows(cb, 0)
    return out


def lp(x, y):
    return (y << 16) | (x & 0xFFFF)


def press_snip_hotkey():
    u.keybd_event(VK_CONTROL, 0, 0, 0)
    u.keybd_event(VK_MENU, 0, 0, 0)
    u.keybd_event(VK_X, 0, 0, 0)
    time.sleep(0.05)
    u.keybd_event(VK_X, 0, KEYEVENTF_KEYUP, 0)
    u.keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0)
    u.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)


def read_clipboard():
    CF_UNICODETEXT = 13
    if not u.OpenClipboard(None):
        return ""
    try:
        h = u.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return ""
        p = k.GlobalLock(h)
        if not p:
            return ""
        try:
            return ctypes.wstring_at(p)
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


def main():
    log_path = sys.argv[1]
    before = open(log_path, encoding="utf-8", errors="replace").read()

    press_snip_hotkey()
    time.sleep(1.2)

    ov = find("CapWGCSelect")
    if not ov:
        print("FAIL: snip overlay did not appear (hotkey not delivered?)")
        return 1
    bar = find("CapWGCToolbar")
    if not bar:
        print("FAIL: no toolbar window")
        return 1

    r = wt.RECT()
    u.GetWindowRect(bar[0], ctypes.byref(r))
    bw, bh = r.right - r.left, r.bottom - r.top
    print(f"overlay=0x{ov[0]:X} toolbar=0x{bar[0]:X} {bw}x{bh}")

    # Right half of the capsule = the OCR segment.
    ox, oy = int(bw * 0.75), bh // 2
    u.PostMessageW(bar[0], WM_LBUTTONDOWN, 1, lp(ox, oy))
    time.sleep(0.05)
    u.PostMessageW(bar[0], WM_LBUTTONUP, 0, lp(ox, oy))
    time.sleep(0.3)

    x0, y0 = 60, 60
    u.PostMessageW(ov[0], WM_LBUTTONDOWN, 1, lp(x0, y0))
    for i in range(1, 21):
        u.PostMessageW(ov[0], WM_MOUSEMOVE, 1, lp(x0 + i * 18, y0 + i * 9))
        time.sleep(0.01)
    u.PostMessageW(ov[0], WM_LBUTTONUP, 0, lp(x0 + 360, y0 + 180))

    for _ in range(60):
        time.sleep(0.25)
        new = open(log_path, encoding="utf-8", errors="replace").read()[len(before):]
        if "ocr ok lines=" in new or "ocr FAILED" in new:
            break

    new = open(log_path, encoding="utf-8", errors="replace").read()[len(before):]
    for line in new.splitlines():
        if "ocr" in line.lower() or "snip done" in line:
            print("  log:", line)

    if "ocr ok lines=" not in new:
        print("FAIL: no OCR result in the log")
        return 1

    text = read_clipboard()
    print("--- clipboard ---")
    print(text if text else "(empty)")
    print("-----------------")
    print("PASS: in-process OCR produced a result")
    return 0


if __name__ == "__main__":
    sys.exit(main())
