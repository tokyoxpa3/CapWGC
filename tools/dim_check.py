"""Reproduces "dim overlay stops working after the Microsoft Snipping Tool".

Which entry point this uses, and why
------------------------------------
Tapping Ctrl+Alt+X through keybd_event does not work here: this session's
foreground window belongs to an RDP window manager, so injected keystrokes go
there and CapWGC's RegisterHotKey never fires. That is an environment
limitation and it made earlier runs useless.

So this script drives the **real tray app** and delivers WM_HOTKEY directly to
its message window. That is the same code path the OS hotkey runs -- the
WM_HOTKEY handler in main.cpp is the entire hotkey branch, so posting the
message skips only the OS delivery step, not any of our logic:

    WM_HOTKEY -> BeginSnip(a, false) -> CaptureVirtualScreen
              -> Overlay::BeginSnip -> DimPanel::SetRect x4

Two sequences are compared:

    A) baseline:      hotkey -> measure luma
    B) MS tool first: PrintScreen (real global hotkey, owned by ScreenSketch)
                      -> Esc -> hotkey -> measure luma

Luminance is the only honest signal. The overlay is a 110/255 black alpha
layer, so a working dim drops mean luminance by ~40%. Losing that drop while
the panels are still visible is the reported symptom.
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "out", "CapWGC.exe")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "CapWGC", "gui.log")

u = ctypes.windll.user32
k = ctypes.windll.kernel32
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

VK_SNAPSHOT = 0x2C
VK_ESCAPE = 0x1B
KEYEVENTF_KEYUP = 0x0002
WM_HOTKEY = 0x0312

# Hotkey ids from src/app/main.cpp's HotkeyId enum.
HOTKEY_SNIP = 5

# ctypes defaults to c_int for restype, which truncates 64-bit handles.
u.GetForegroundWindow.restype = wt.HWND


def cls(h):
    buf = ctypes.create_unicode_buffer(256)
    u.GetClassNameW(h, buf, 256)
    return buf.value


def title(h):
    buf = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, buf, 256)
    return buf.value


def proc_name(h):
    pid = wt.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if not pid.value:
        return "?"
    hproc = k.OpenProcess(0x1000, False, pid.value)   # QUERY_LIMITED_INFORMATION
    if not hproc:
        return f"pid{pid.value}"
    try:
        buf = ctypes.create_unicode_buffer(512)
        size = wt.DWORD(512)
        if k.QueryFullProcessImageNameW(hproc, 0, buf, ctypes.byref(size)):
            return os.path.basename(buf.value)
        return f"pid{pid.value}"
    finally:
        k.CloseHandle(hproc)


def find_class(name, want_proc=None):
    out = []

    @EnumWindowsProc
    def cb(h, _):
        if cls(h) == name:
            if want_proc is None or proc_name(h) == want_proc:
                out.append(h)
        return True

    u.EnumWindows(cb, 0)
    return out


def fg_report(tag):
    h = u.GetForegroundWindow()
    print(f"    fg[{tag}] class={cls(h)!r} proc={proc_name(h)!r} title={title(h)!r}")


def luma(box):
    # include_layered_windows=True is NOT optional. Pillow's BitBlt path skips
    # layered windows unless asked, and CapWGCDim is WS_EX_LAYERED with
    # LWA_ALPHA. Without the flag the grab returns the undimmed desktop, so
    # every run reports drop ~= 0 and looks exactly like the reported bug.
    im = ImageGrab.grab(bbox=box, include_layered_windows=True,
                        all_screens=True).convert("L")
    data = im.tobytes()
    return sum(data) / len(data), im.size


def box_center():
    w = u.GetSystemMetrics(0)
    h = u.GetSystemMetrics(1)
    cx, cy = w // 2, h // 2
    return (cx - 300, cy - 150, cx + 300, cy + 150)


def tap(vk):
    u.keybd_event(vk, 0, 0, 0)
    time.sleep(0.04)
    u.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)


def kill_app():
    subprocess.run(["taskkill", "/IM", "CapWGC.exe", "/F"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)


def log_tail(n=6):
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except FileNotFoundError:
        return []


def find_msgwnd():
    """CapWGC's hidden message window (class "CapWGCWindow", see kClassName in
    main.cpp). Tray balloons, hotkeys and timers are all serviced here."""
    hits = find_class("CapWGCWindow", "CapWGC.exe")
    return hits[0] if hits else None


def overlay_probe():
    dims = find_class("CapWGCDim", "CapWGC.exe")
    shown = [h for h in dims if u.IsWindowVisible(h)]
    detail = []
    for h in shown:
        r = wt.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        detail.append(f"{r.right - r.left}x{r.bottom - r.top}@({r.left},{r.top})")
    topmost = 0
    for h in shown:
        ex = u.GetWindowLongW(h, -20)          # GWL_EXSTYLE
        if ex & 0x00000008:                    # WS_EX_TOPMOST
            topmost += 1
    return dict(dims=len(dims), shown=len(shown), topmost=topmost,
                detail=detail, select=len(find_class("CapWGCSelect", "CapWGC.exe")))


def run_once(label, ms_first):
    """One cycle: [optional MS snip] -> start app -> baseline -> hotkey -> measure."""
    print(f"\n== {label} ==")
    kill_app()
    time.sleep(0.6)

    if ms_first:
        print("  step 1: PrintScreen -> Microsoft Snipping Tool")
        before = u.GetForegroundWindow()
        tap(VK_SNAPSHOT)
        appeared = False
        for _ in range(24):
            time.sleep(0.25)
            h = u.GetForegroundWindow()
            if h != before:
                appeared = True
                break
        fg_report("after PrintScreen")
        print(f"    MS tool appeared={appeared}")
        tap(VK_ESCAPE)
        time.sleep(1.2)
        fg_report("after Esc")
        if not appeared:
            print("    NOTE: PrintScreen did not bring up the MS tool either --")
            print("    this run did NOT test the reported sequence.")

    print("  step 2: start CapWGC.exe (real tray app)")
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    ready = False
    for _ in range(60):
        time.sleep(0.25)
        if any("ocr ready=" in l for l in log_tail(40)):
            ready = True
            break
    print(f"    app up ready={ready}")
    fg_report("app up")

    # Baseline MUST be measured with no overlay on screen. The app is resident
    # in the tray here, so nothing is dimmed yet.
    print("  step 3: baseline luminance (app resident, no overlay)")
    b = box_center()
    base, size = luma(b)
    print(f"    luma={base:6.1f}  box={size[0]}x{size[1]}")

    wnd = find_msgwnd()
    if not wnd:
        print("    FAIL: could not find CapWGC's message window")
        proc.kill()
        return None
    print(f"  step 4: post WM_HOTKEY({HOTKEY_SNIP}) to 0x{wnd:X} (same as the hotkey)")

    ok = u.PostMessageW(wnd, WM_HOTKEY, HOTKEY_SNIP, 0)
    if not ok:
        print("    FAIL: PostMessage refused")
        proc.kill()
        return None

    ov = []
    for _ in range(24):
        time.sleep(0.25)
        ov = find_class("CapWGCSelect", "CapWGC.exe")
        if ov:
            break
    if not ov:
        print("    FAIL: CapWGCSelect never appeared")
        for line in log_tail(8):
            print("    log:", line)
        proc.kill()
        return None

    time.sleep(1.5)            # let the dim settle

    # A stray click can land on the freshly-shown overlay and immediately
    # select whatever window is under the cursor, which closes it before we
    # can measure (log shows "snip done ok=1 rect=..."). That is an artifact of
    # driving this over RDP, not the bug under test, so report it as a retry
    # instead of a FAIL -- otherwise a broken run looks like a broken product.
    if log_tail(3) and any("snip done" in l for l in log_tail(3)):
        print("    self-terminated: a stray click selected a window")
        proc.kill()
        return "retry"

    p = overlay_probe()
    if p["select"] == 0:
        print("    self-terminated: CapWGCSelect vanished before measuring")
        proc.kill()
        return "retry"

    dim, _ = luma(b)
    drop = (base - dim) / base * 100.0 if base else 0.0
    print(f"  step 5: with overlay  luma={dim:6.1f}  drop={drop:5.1f}%")
    print(f"    DimPanel windows={p['dims']} visible={p['shown']} topmost={p['topmost']}"
          f"  CapWGCSelect={p['select']}")
    if p["detail"]:
        print("    rects:", ", ".join(p["detail"]))

    for line in log_tail(10):
        if "hotkey" in line or "overlay" in line or "snip" in line:
            print("    log:", line)

    # Close the overlay the way a user would, then let the app keep running so
    # the next cycle starts from the same state a user would be in.
    u.PostMessageW(ov[0], 0x0100, VK_ESCAPE, 0)     # WM_KEYDOWN VK_ESCAPE
    time.sleep(0.6)
    proc.kill()

    return dict(base=base, dim=dim, drop=drop, **p)


def main():
    print(f"screen {u.GetSystemMetrics(0)}x{u.GetSystemMetrics(1)}  "
          f"session={os.environ.get('SESSIONNAME', '?')}")

    def attempt(label, ms_first, tries=3):
        for i in range(tries):
            r = run_once(label if i == 0 else f"{label} (retry {i})", ms_first)
            if r != "retry":
                return r
            time.sleep(1.0)
        print(f"  giving up on {label}: the overlay kept closing itself")
        return None

    a = attempt("A: baseline (no MS tool)", ms_first=False)
    if not a:
        return 2
    b = attempt("B: after PrintScreen -> MS Snipping Tool", ms_first=True)
    if not b:
        return 2

    print("\n== verdict ==")
    print(f"  A drop={a['drop']:5.1f}%  (visible {a['shown']}/4 topmost {a['topmost']})")
    print(f"  B drop={b['drop']:5.1f}%  (visible {b['shown']}/4 topmost {b['topmost']})")

    # shown == 1 is the correct count while nothing is selected: UpdateVisuals
    # dims the whole virtual screen with one panel (dim_[0]) and hides the
    # other three. It becomes 4 only after a drag.
    a_ok = a["drop"] > 20.0 and a["shown"] >= 1
    b_ok = b["drop"] > 20.0 and b["shown"] >= 1
    if a_ok and b_ok:
        print("  PASS: dim works in both runs -- not reproduced here")
        return 0
    if a_ok and not b_ok:
        print("  FAIL: reproduced -- the MS tool broke dimming")
        if b["shown"] >= 1:
            print("  the panel is still visible but not darkening, so the problem")
            print("  is alpha/z-order, not ShowWindow -- that points at DWM or at")
            print("  our LWA_ALPHA state, not at BeginSnip")
        return 1
    print("  FAIL: dim was already broken in the baseline run")
    return 1


if __name__ == "__main__":
    sys.exit(main())
