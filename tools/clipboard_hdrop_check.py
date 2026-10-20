"""Proves the snip lands in the clipboard as BOTH a bitmap and a file (CF_HDROP).

Why this needs its own check
----------------------------
"可以在檔案總管貼上嗎" is not answerable by reading the code: the clipboard can
hold several formats at once, and whether Explorer's Ctrl+V does anything depends
entirely on whether CF_HDROP is present AND whether the file it names still
exists on disk. Both halves can silently fail while the bitmap half keeps
working, so the snip still "works" and nobody notices.

What is asserted
----------------
  1. CF_DIB or CF_BITMAP is present  -> paste into Paint still works.
  2. CF_HDROP is present             -> paste into Explorer does something.
  3. The path in CF_HDROP exists and is a .png under the snip folder.
  4. The PNG is a real image with the expected size (parsed from the header),
     so "the file exists" cannot be satisfied by a zero-byte stub.
  5. The file name follows the CapWGC_YYYYMMDD_HHMMSS.png convention, which is
     what makes Explorer's paste produce a sortable name instead of "image (3)".

CF_HDROP is read through DragQueryFileW rather than parsing the DROPFILES blob
by hand: the blob has an ANSI and a UTF-16 flavour and the priority order is
easy to get wrong, while DragQueryFileW is the same API Explorer itself calls.
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
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
shell32 = ctypes.windll.shell32
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

WM_HOTKEY = 0x0312
HOTKEY_SNIP = 5

CF_DIB = 8
CF_BITMAP = 2
CF_HDROP = 15

u.GetClipboardOwner.restype = wt.HWND
u.GetClipboardData.restype = wt.HANDLE
u.GetClipboardData.argtypes = [wt.UINT]
# Same 64-bit trap as DragQueryFileW below: GlobalAlloc/GlobalLock take an
# HGLOBAL, so their signatures must be declared or the handle is truncated.
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalLock.argtypes = [wt.HGLOBAL]
k.GlobalUnlock.restype = wt.BOOL
k.GlobalUnlock.argtypes = [wt.HGLOBAL]
# Every one of these matters on 64-bit: without restype the HDROP comes back as
# a truncated 32-bit int, and DragQueryFileW then dereferences garbage --
# observed as "access violation reading 0x...D2E0", not as a clean failure.
shell32.DragQueryFileW.restype = wt.UINT
shell32.DragQueryFileW.argtypes = [wt.HANDLE, wt.UINT, wt.LPWSTR, wt.UINT]


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


def set_save(on):
    """Force snip.save_file to the value under test, returning the old text."""
    os.makedirs(LOCAL, exist_ok=True)
    orig = None
    if os.path.exists(INI):
        with open(INI, encoding="utf-8", errors="replace") as f:
            orig = f.read()
    text = orig or "[snip]\n"
    val = "1" if on else "0"
    if re.search(r"(?m)^\[snip\]\s*$", text):
        if re.search(r"(?m)^save_file\s*=", text):
            text = re.sub(r"(?m)^save_file\s*=.*$", f"save_file={val}", text)
        else:
            text = re.sub(r"(?m)^(\[snip\]\s*)$", rf"\1\nsave_file={val}", text, count=1)
    else:
        text += f"\n[snip]\nsave_file={val}\n"
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


def clipboard_formats():
    """Format ids currently on the clipboard."""
    out = []
    if not u.OpenClipboard(None):
        return out
    try:
        fmt = 0
        while True:
            fmt = u.EnumClipboardFormats(fmt)
            if not fmt:
                break
            out.append(fmt)
    finally:
        u.CloseClipboard()
    return out


def hdrop_paths():
    """Every path in CF_HDROP, via the same API Explorer uses."""
    paths = []
    if not u.OpenClipboard(None):
        return paths
    try:
        h = u.GetClipboardData(CF_HDROP)
        if not h:
            return paths
        n = shell32.DragQueryFileW(h, 0xFFFFFFFF, None, 0)
        for i in range(n):
            buf = ctypes.create_unicode_buffer(1024)
            if shell32.DragQueryFileW(h, i, buf, 1024):
                paths.append(buf.value)
    finally:
        u.CloseClipboard()
    return paths


def dib_size():
    """(w, h) from the CF_DIB header, or None."""
    if not u.OpenClipboard(None):
        return None
    try:
        h = u.GetClipboardData(CF_DIB)
        if not h:
            return None
        p = k.GlobalLock(h)
        if not p:
            return None
        try:
            # BITMAPINFOHEADER: biSize, biWidth, biHeight are the first 12 bytes.
            raw = ctypes.string_at(p, 12)
            _sz, w, hh = struct.unpack("<Iii", raw)
            return (w, abs(hh))
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


def png_size(path):
    """(w, h) from the PNG IHDR chunk, or None if it is not a real PNG."""
    try:
        with open(path, "rb") as f:
            head = f.read(33)
    except OSError:
        return None
    if len(head) < 33 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h = struct.unpack(">II", head[16:24])
    return (w, h)


def makelparam(x, y):
    """MAKELPARAM(x, y) -- x in the LOW word, y in the HIGH word.

    Getting this backwards still "works" (you just drag a transposed rectangle),
    which is exactly why the size assertion below is worth keeping: the first
    version of this script injected (420 << 16) | 260, i.e. a 200x360 drag, and
    then asserted 360x200. The app was right and the test was wrong.
    """
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


def do_snip(wnd):
    u.PostMessageW(wnd, WM_HOTKEY, HOTKEY_SNIP, 0)
    ov = []
    for _ in range(40):
        time.sleep(0.05)
        ov = find_class("CapWGCSelect")
        if ov:
            break
    if not ov:
        return False
    h = ov[0]
    u.PostMessageW(h, 0x0201, 0x0001, makelparam(60, 60))
    time.sleep(0.05)
    u.PostMessageW(h, 0x0200, 0x0001, makelparam(420, 260))
    time.sleep(0.05)
    u.PostMessageW(h, 0x0202, 0, makelparam(420, 260))
    return True


def run_case(save_on):
    print(f"\n== snip.save_file={1 if save_on else 0} ==")
    kill_app()
    orig = set_save(save_on)
    mark = len(log_lines())
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    try:
        for _ in range(80):
            time.sleep(0.25)
            if any("hotkey id=" in l for l in log_lines()[mark:]):
                break
        wnds = find_class("CapWGCWindow")
        if not wnds:
            print("FAIL: no message window")
            return 1

        # Clear the clipboard first: a stale CF_HDROP from a previous run would
        # make the "format is present" assertion pass without our code working.
        if u.OpenClipboard(None):
            u.EmptyClipboard()
            u.CloseClipboard()

        if not do_snip(wnds[0]):
            print("FAIL: overlay never appeared")
            return 1
        time.sleep(0.8)

        fmts = clipboard_formats()
        paths = hdrop_paths()
        size = dib_size()
        ok = 0
        bad = 0

        def chk(cond, what):
            nonlocal ok, bad
            print(f"  {'ok  ' if cond else 'FAIL'} {what}")
            if cond:
                ok += 1
            else:
                bad += 1

        chk(CF_DIB in fmts or CF_BITMAP in fmts,
            f"bitmap format present (DIB={CF_DIB in fmts}, BITMAP={CF_BITMAP in fmts})")
        chk(size == (360, 200), f"CF_DIB measures 360x200 (got {size})")

        if save_on:
            chk(CF_HDROP in fmts, "CF_HDROP present (Explorer can paste)")
            chk(len(paths) == 1, f"exactly one file in CF_HDROP (got {len(paths)})")
            if paths:
                p = paths[0]
                chk(os.path.isfile(p), f"the file exists: {p}")
                chk(os.path.basename(os.path.dirname(p)) == "Snip",
                    f"lives in the Snip folder (dir={os.path.basename(os.path.dirname(p))})")
                chk(bool(re.fullmatch(r"CapWGC_\d{8}_\d{6}\.png",
                                      os.path.basename(p))),
                    f"timestamp name (got {os.path.basename(p)})")
                chk(png_size(p) == (360, 200),
                    f"real PNG with 360x200 (got {png_size(p)})")
        else:
            # The whole point of the option: with saving off there is no file,
            # so promising a paste would be a lie.
            chk(CF_HDROP not in fmts,
                "CF_HDROP absent (nothing to paste into a folder)")

        new = [l for l in log_lines()[mark:] if "snip clipboard ok" in l]
        for l in new:
            print("  log:", l)
        return 0 if bad == 0 else 1
    finally:
        proc.kill()
        time.sleep(0.4)
        restore_ini(orig)


def main():
    if not os.path.exists(EXE):
        print(f"FAIL: {EXE} not found (run build.bat all)")
        return 2
    rc1 = run_case(True)
    rc2 = run_case(False)
    print()
    if rc1 == 0 and rc2 == 0:
        print("PASS: bitmap + file both on the clipboard when saving; "
              "bitmap only when not")
        return 0
    print("FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
