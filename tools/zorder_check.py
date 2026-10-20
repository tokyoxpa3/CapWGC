"""Measure z-order churn between the toolbar and the dim panels during a drag.

The flicker the user reported was: every WM_MOUSEMOVE -> dim panels SetWindowPos
(HWND_TOPMOST) -> toolbar pushed below -> toolbar.Raise() -> pushed back. That is
a z-order flip on every mouse move. This script drives a synthetic drag into the
overlay window and records the toolbar's z-order rank after every step.

Expected after the fix: the toolbar stays above all dim panels and its rank does
not change at all between steps.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

u = ctypes.windll.user32

EnumWindows = u.EnumWindows
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
GetClassNameW = u.GetClassNameW
FindWindowW = u.FindWindowW
PostMessageW = u.PostMessageW
GetTopWindow = u.GetTopWindow
GetWindow = u.GetWindow
GetDesktopWindow = u.GetDesktopWindow
IsWindowVisible = u.IsWindowVisible

GW_HWNDNEXT = 2
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201


def cls(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    GetClassNameW(hwnd, buf, 256)
    return buf.value


def find_by_class(name):
    out = []

    @EnumWindowsProc
    def cb(h, _):
        if cls(h) == name:
            out.append(h)
        return True

    EnumWindows(cb, 0)
    return out


def zorder(visible_only=True):
    """Top-level windows, front (topmost-most-recent) first.

    Hidden windows are skipped by default: they cannot cover anything, and
    keeping them in the list would make the rank numbers jump around for
    reasons that have nothing to do with flicker.
    """
    order = []
    h = GetTopWindow(GetDesktopWindow())
    while h:
        if not visible_only or IsWindowVisible(h):
            order.append(h)
        h = GetWindow(h, GW_HWNDNEXT)
    return order


def lp(x, y):
    return (y << 16) | (x & 0xFFFF)


def main():
    overlay = find_by_class("CapWGCSelect")
    if not overlay:
        print("FAIL: no CapWGCSelect window (is --sniptest running?)")
        return 1
    ov = overlay[0]

    dims = find_by_class("CapWGCDim")
    bars = find_by_class("CapWGCToolbar")
    print(f"overlay=0x{ov:X} dims={len(dims)} toolbar={len(bars)}")
    if not bars:
        print("FAIL: no toolbar window")
        return 1
    bar = bars[0]

    order = zorder()
    print("initial toolbar rank:", order.index(bar) if bar in order else "absent")
    print("initial dim ranks:", [order.index(d) if d in order else "hidden" for d in dims])

    # 模擬拖曳：按下 → 連續移動。用 PostMessage 直接餵給 overlay 的視窗程序，
    # 不動真實滑鼠，避免干擾桌面。
    PostMessageW(ov, WM_LBUTTONDOWN, 1, lp(200, 200))

    ranks = []
    above_all = 0
    for i in range(1, 81):
        x, y = 200 + i * 6, 200 + i * 4
        PostMessageW(ov, WM_MOUSEMOVE, 1, lp(x, y))
        time.sleep(0.012)
        order = zorder()
        r_bar = order.index(bar) if bar in order else -1
        r_dims = [order.index(d) if d in order else -1 for d in dims]
        visible_dims = [r for r in r_dims if r >= 0]
        # 工具列必須排在所有「存在」的遮罩前面（rank 數字越小越靠上）
        ok = r_bar >= 0 and all(r_bar < r for r in visible_dims)
        above_all += 1 if ok else 0
        ranks.append((r_bar, tuple(r_dims)))

    distinct = []
    for r in ranks:
        if not distinct or distinct[-1] != r[0]:
            distinct.append(r[0])

    print(f"steps={len(ranks)} toolbar-above-all-dims={above_all}/{len(ranks)}")
    print("toolbar rank transitions:", distinct)
    print("rank samples (every 10th):", [r[0] for r in ranks[::10]])

    churn = len(distinct) - 1
    print(f"z-order churn (rank changes during drag) = {churn}")

    if "--submit" in sys.argv:
        # 完整走一次「拖曳 → 放開」的提交路徑，確認沒有把截圖流程改壞。
        PostMessageW(ov, 0x0202, 0, lp(200 + 80 * 6, 200 + 80 * 4))  # WM_LBUTTONUP
        time.sleep(1.0)
        print("submitted (see the sniptest log for the outcome)")
        return 0

    if above_all != len(ranks):
        print("FAIL: toolbar was covered by a dim panel at some point")
        return 1
    if churn != 0:
        print(f"FAIL: toolbar z-order still changed {churn} times while dragging")
        return 1
    print("PASS: no z-order churn, toolbar stayed on top for the whole drag")
    return 0


if __name__ == "__main__":
    sys.exit(main())
