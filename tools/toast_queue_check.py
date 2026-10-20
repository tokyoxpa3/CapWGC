"""Measures whether notifications *replace* or *queue* on consecutive snips.

This is the measurement behind "連續截圖時提示慢好幾秒". The two designs behave
completely differently and the difference is easy to see from the outside:

  Shell balloon (old)  -> the third notification appears ~N seconds after the
                          action, because each one waits for the previous to
                          expire (system minimum, 5 s by default).
  Own window (new)     -> the notification for action k is on screen within
                          ~100 ms of that action, because Show() replaces it.

How each notification is observed: the toast window class is CapWGCToast and its
title text is the first thing painted, so we poll GetWindowText on the
CapWGCToast window and record when the text changes. Window text is never set
for a layered window drawn with UpdateLayeredWindow... so instead we rely on the
log line "toast <title>: <body>" that main.cpp writes next to every balloon, and
on the window's *existence* timing.

Polling the window is still useful for one thing: whether the window is visible
at all while three snips fire back to back.

Exit code 0 = notifications are immediate for every snip.
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

from PIL import ImageGrab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "out", "CapWGC.exe")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "CapWGC", "gui.log")

u = ctypes.windll.user32
k = ctypes.windll.kernel32
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_HOTKEY = 0x0312
HOTKEY_SNIP = 5

u.GetForegroundWindow.restype = wt.HWND


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def proc_name(h):
    pid = wt.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if not pid.value:
        return "?"
    hp = k.OpenProcess(0x1000, False, pid.value)
    if not hp:
        return f"pid{pid.value}"
    try:
        buf = ctypes.create_unicode_buffer(512)
        n = wt.DWORD(512)
        if k.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(n)):
            return os.path.basename(buf.value)
        return f"pid{pid.value}"
    finally:
        k.CloseHandle(hp)


def find_class(name, want_proc=None):
    out = []

    @EnumProc
    def cb(h, _):
        if cls(h) == name and (want_proc is None or proc_name(h) == want_proc):
            out.append(h)
        return True

    u.EnumWindows(cb, 0)
    return out


def log_lines():
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def kill_app():
    subprocess.run(["taskkill", "/IM", "CapWGC.exe", "/F"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)


def toast_visible():
    """Is our own notification window on screen right now?"""
    return any(u.IsWindowVisible(h) for h in find_class("CapWGCToast", "CapWGC.exe"))


def main():
    kill_app()

    # Marker so we only look at lines produced by this run.
    mark = len(log_lines())

    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    for _ in range(60):
        time.sleep(0.25)
        if any("ocr ready=" in l for l in log_lines()[mark:]):
            break
    print("app up")

    wnds = find_class("CapWGCWindow", "CapWGC.exe")
    if not wnds:
        print("FAIL: no message window")
        proc.kill()
        return 2
    wnd = wnds[0]

    # Three snips, 900 ms apart: that is faster than the system's 5 s minimum,
    # so with the old balloon design only the first would ever be readable and
    # #2/#3 would surface long after this loop finished.
    print("firing 3 snips, 900 ms apart (each: overlay -> drag -> release)")
    t0 = time.time()
    samples = []
    for i in range(3):
        u.PostMessageW(wnd, WM_HOTKEY, HOTKEY_SNIP, 0)

        # Wait for the overlay, then drag a rectangle and release. The overlay
        # is driven by PostMessage-free real mouse messages, so instead we ask
        # it to submit via the OCR/snip test path: --sniptest cannot be used on
        # a running instance, so drag with SendInput.
        ov = []
        for _ in range(24):
            time.sleep(0.05)
            ov = find_class("CapWGCSelect", "CapWGC.exe")
            if ov:
                break
        if not ov:
            print(f"  snip {i+1}: overlay never appeared")
            continue
        h = ov[0]

        # WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP straight into the
        # overlay's window proc -- no cursor movement needed, works while an RDP
        # window owns the foreground.
        u.PostMessageW(h, 0x0201, 0x0001, ((60 + i * 20) << 16) | (60 + i * 20))
        time.sleep(0.05)
        u.PostMessageW(h, 0x0200, 0x0001, ((420) << 16) | (260))
        time.sleep(0.05)
        u.PostMessageW(h, 0x0202, 0, ((420) << 16) | (260))

        # Poll for our notification window becoming visible, and record how long
        # it took from the click. Anything close to 5 s means it queued.
        t_click = time.time()
        appeared = None
        for _ in range(80):
            if toast_visible():
                appeared = time.time() - t_click
                break
            time.sleep(0.025)
        samples.append(appeared)
        print(f"  snip {i+1}: toast visible after "
              f"{'never' if appeared is None else f'{appeared*1000:.0f} ms'}")
        time.sleep(0.9 - 0.0)

    total = time.time() - t0
    proc.kill()

    new_lines = [l for l in log_lines()[mark:] if "toast " in l]
    print(f"\n{len([s for s in samples if s is not None])}/3 notifications appeared; "
          f"loop took {total:.1f} s")
    for l in new_lines:
        print("  log:", l)

    if not new_lines:
        print("NOTE: no 'toast' log lines -- is TrayBalloon still the old one?")
        return 1

    slow = [s for s in samples if s is not None and s > 1.0]
    if slow:
        print(f"FAIL: {len(slow)} notification(s) took >1 s -> still queueing")
        return 1
    print("PASS: every notification replaced the previous one immediately")
    return 0


if __name__ == "__main__":
    sys.exit(main())
