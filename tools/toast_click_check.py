"""End-to-end check: the "已儲存（點一下開啟）" toast really opens the file.

Why this exists
---------------
Replacing the Shell balloon silently deleted the click path. The old design got
`NIN_BALLOONUSERCLICK` in the tray callback and called OpenLastFile(); once the
balloon became our own ui::Toast window, nothing sent that message any more, so
the title kept promising "點一下開啟" while a click only hid the toast. The fix
is a callback on the toast (ui::Toast::Action).

toast_click_test.exe already proves the *wiring* (left click runs the callback,
right click doesn't, the callback dies with the toast). What it cannot prove is
that main.cpp connects the right callback at the right call site -- that is a
one-line change which the unit test cannot see. So this script drives the real
app end to end:

  1. settings.ini gets snip.save_file=1, so DeliverSnip writes a PNG.
  2. Fire the snip hotkey, drag a rectangle.
  3. Wait for the toast window to appear and read the log for
     "toast 已截圖（點一下開啟）".
  4. Read the file name out of that log line, then click the toast window in
     the middle (WM_LBUTTONDOWN straight into the window proc -- no cursor
     movement, works under RDP).
  5. Assert the log gained "open.last: <that same path>".

The assertion is on the log, not on "a picture viewer opened": ShellExecuteW
hands off to whatever the user associated with .png and we cannot observe that
reliably. What we can observe is that our click path reached OpenLastFile with
the file we just wrote -- which is the whole defect.

Exit code 0 = the click opened the file that was just saved.
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "out", "CapWGC.exe")
LOCAL = os.path.join(os.environ["LOCALAPPDATA"], "CapWGC")
LOG = os.path.join(LOCAL, "gui.log")
INI = os.path.join(LOCAL, "settings.ini")

u = ctypes.windll.user32
k = ctypes.windll.kernel32
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_HOTKEY = 0x0312
HOTKEY_SNIP = 5


def cls(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, b, 256)
    return b.value


def find_class(name):
    out = []

    @EnumProc
    def cb(h, _):
        if cls(h) == name:
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


def ensure_save_enabled():
    """Turn on snip.save_file without touching anything else in the INI.

    Written by hand rather than through the settings window: the window needs
    clicks we cannot reliably inject, and the INI is the documented interface
    (docs/09). The original file is restored on exit.
    """
    os.makedirs(LOCAL, exist_ok=True)
    orig = None
    if os.path.exists(INI):
        with open(INI, encoding="utf-8", errors="replace") as f:
            orig = f.read()

    text = orig or "[snip]\n"
    if re.search(r"(?m)^\[snip\]\s*$", text):
        if re.search(r"(?m)^save_file\s*=", text):
            text = re.sub(r"(?m)^save_file\s*=.*$", "save_file=1", text)
        else:
            text = re.sub(r"(?m)^(\[snip\]\s*)$", r"\1\nsave_file=1", text, count=1)
    else:
        text += "\n[snip]\nsave_file=1\n"
    with open(INI, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)
    return orig


def restore_ini(orig):
    if orig is None:
        if os.path.exists(INI):
            os.remove(INI)
        return
    with open(INI, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(orig)


def main():
    if not os.path.exists(EXE):
        print(f"FAIL: {EXE} not found (run build.bat all)")
        return 2

    kill_app()
    orig = ensure_save_enabled()
    mark = len(log_lines())

    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    try:
        for _ in range(80):
            time.sleep(0.25)
            if any("ocr ready=" in l or "hotkey id=" in l for l in log_lines()[mark:]):
                break
        print("app up")

        wnds = find_class("CapWGCWindow")
        if not wnds:
            print("FAIL: no message window (class CapWGCWindow)")
            return 2
        wnd = wnds[0]

        print("firing one snip (hotkey -> drag -> release)")
        u.PostMessageW(wnd, WM_HOTKEY, HOTKEY_SNIP, 0)

        ov = []
        for _ in range(40):
            time.sleep(0.05)
            ov = find_class("CapWGCSelect")
            if ov:
                break
        if not ov:
            print("FAIL: overlay never appeared")
            return 2
        h = ov[0]

        u.PostMessageW(h, 0x0201, 0x0001, (60 << 16) | 60)      # WM_LBUTTONDOWN
        time.sleep(0.05)
        u.PostMessageW(h, 0x0200, 0x0001, (420 << 16) | 260)    # WM_MOUSEMOVE
        time.sleep(0.05)
        u.PostMessageW(h, 0x0202, 0, (420 << 16) | 260)          # WM_LBUTTONUP

        # Wait for the toast that promises a click.
        #
        # The body is multi-line: Log() writes "toast <title>: <body>" and the
        # body itself contains "\n已存檔：…", so the file name lands on the
        # NEXT log line. Matching single lines would silently never find it.
        promise = None
        for _ in range(60):
            time.sleep(0.05)
            text = "\n".join(log_lines()[mark:])
            if "toast 已截圖（點一下開啟）" in text:
                promise = text
                break
        if not promise:
            print("FAIL: no '已截圖（點一下開啟）' toast -- "
                  "snip.save_file did not take effect, or the title fell back")
            for l in [x for x in log_lines()[mark:] if "toast " in x]:
                print("  log:", l)
            return 1
        print("  toast:", [x for x in promise.splitlines()
                           if "已截圖（點一下開啟）" in x][0])

        # The saved file name is in the toast body, after "已存檔：".
        m = re.search(r"已存檔：\s*(\S+?\.png)", promise)
        if not m:
            print("FAIL: toast has no '已存檔：<name>' -- nothing to open")
            return 1
        name = m.group(1)
        print(f"  saved: {name}")

        tw = find_class("CapWGCToast")
        tw = [x for x in tw if u.IsWindowVisible(x)]
        if not tw:
            print("FAIL: toast window is not visible, cannot click it")
            return 1
        th = tw[0]
        r = wt.RECT()
        u.GetWindowRect(th, ctypes.byref(r))
        pt = ((r.left + r.right) // 2, (r.top + r.bottom) // 2)

        before = len(log_lines())
        u.PostMessageW(th, 0x0201, 0x0001, (pt[1] << 16) | (pt[0] & 0xFFFF))
        print(f"clicking the toast at {pt}")

        opened = None
        for _ in range(40):
            time.sleep(0.05)
            for l in log_lines()[before:]:
                if "open.last:" in l:
                    opened = l
                    break
            if opened:
                break

        if not opened:
            print("FAIL: click did not reach OpenLastFile "
                  "(the '點一下開啟' promise is still a lie)")
            for l in [x for x in log_lines()[before:] if "toast " in x or "open.last" in x]:
                print("  log:", l)
            return 1
        print("  ", opened)

        if name not in opened:
            print(f"FAIL: opened a different file (expected {name})")
            return 1
        if u.IsWindowVisible(th):
            print("FAIL: toast still visible after the click")
            return 1

        print("\nPASS: clicking the toast opened the file it named")
        return 0
    finally:
        kill_app()
        restore_ini(orig)


if __name__ == "__main__":
    sys.exit(main())
