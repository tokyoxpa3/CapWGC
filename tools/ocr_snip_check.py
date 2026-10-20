"""End-to-end check of the *app* path: snip -> in-process ONNX -> clipboard.

Difference from ocr_e2e.py: that one drives the global hotkey, which only works
when CapWGC is the foreground window (it is not, during automated runs -- the
keystrokes go to whatever is focused, and under RDP they get eaten entirely).
This one launches `CapWGC.exe --snipocr`, which calls BeginSnip(a, true)
directly and therefore needs no hotkey and no foreground.

Usage:
    python tools/ocr_snip_check.py                    (launches out/CapWGC.exe)
    python tools/ocr_snip_check.py <path to CapWGC.exe>

Pass the second form to check a *packaged* build from its own folder -- that is
the only way to prove models\ and onnxruntime.dll are found relative to the exe
rather than through the dev tree.

What it proves: the shipped exe, started the way a user starts it, produces
text from a dragged region and puts it on the clipboard. Reading gui.log alone
is not enough -- the log can say "ocr ok" while the clipboard is empty.
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

u = ctypes.windll.user32
k = ctypes.windll.kernel32
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "out", "CapWGC.exe")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "CapWGC", "gui.log")


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


def read_clipboard():
    CF_UNICODETEXT = 13
    # These three need explicit signatures: ctypes defaults restype to c_int,
    # which truncates a 64-bit handle/pointer to 32 bits and then reading it
    # faults with "access violation". That is not a clipboard problem at all,
    # it just looks like one.
    u.GetClipboardData.restype = ctypes.c_void_p
    u.GetClipboardData.argtypes = [wt.UINT]
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]

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
    subprocess.run(["taskkill", "/IM", "CapWGC.exe", "/F"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

    # --snipocr waits for the engine to become ready before opening the overlay,
    # so there is no race here: once CapWGCSelect exists, OCR is usable.
    proc = subprocess.Popen([EXE, "--snipocr"], cwd=os.path.dirname(EXE))

    ov = None
    for _ in range(60):
        time.sleep(0.25)
        ov = find("CapWGCSelect")
        if ov:
            break
    if not ov:
        print("FAIL: overlay never appeared (engine not ready or models missing)")
        proc.kill()
        return 1

    bar = find("CapWGCToolbar")
    if not bar:
        print("FAIL: overlay up but no toolbar")
        proc.kill()
        return 1

    r = wt.RECT()
    u.GetWindowRect(bar[0], ctypes.byref(r))
    bw, bh = r.right - r.left, r.bottom - r.top
    print(f"overlay=0x{ov[0]:X} toolbar=0x{bar[0]:X} {bw}x{bh}")

    # --snipocr already defaults to the OCR action, but click it anyway so this
    # script also covers the toolbar hit-testing.
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

    for _ in range(80):
        time.sleep(0.25)
        with open(LOG, encoding="utf-8", errors="replace") as f:
            tail = f.read()[-2000:]
        if "ocr ok lines=" in tail or "ocr FAILED" in tail:
            break

    with open(LOG, encoding="utf-8", errors="replace") as f:
        tail = f.read()[-2000:]
    for line in tail.splitlines():
        if "ocr" in line.lower() or "snip done" in line:
            print("  log:", line)

    time.sleep(0.4)     # clipboard write happens right after the log line
    text = read_clipboard()
    print("--- clipboard ---")
    print(text if text else "(empty)")
    print("-----------------")

    proc.kill()
    ok = "ocr ok lines=" in tail and bool(text.strip())
    print("PASS" if ok else "FAIL: no OCR result / empty clipboard")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
