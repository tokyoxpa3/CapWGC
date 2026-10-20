"""prtsc_takeover_check.py -- PrtScn 接管真的能用嗎

為什麼要這支腳本：這個功能的失敗全部是無聲的。三種壞法都長得一樣 ——
「按了 PrtScn，什麼都沒發生」：

  1. 登錄值沒改成 0        → Shell 先吃掉按鍵，WM_HOTKEY 不會來
  2. 熱鍵沒註冊成功         → 按鍵進了剪取工具
  3. 註冊成功但被搶         → 這是實測真的會發生的（見 prtsc_probe.cpp）

它們在 log 裡的差別很小，只看 log 很容易把 1 判成 3。所以這支腳本量的是
**可觀察的結果**：真的送一次 PrtScn，然後看程式有沒有動（剪貼簿有沒有換、
log 裡有沒有 snip 那一行）。

用法：
    python tools/prtsc_takeover_check.py [/path/to/CapWGC.exe]
"""

import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

# ---------------------------------------------------------------------------
# 這幾行不是裝飾。ctypes 沒宣告 restype 的話，64 位元的 handle 會被截成
# 32 位元，然後在下一次呼叫時去讀垃圾位址 —— 症狀是 access violation，
# 看起來完全像「程式碼有記憶體錯誤」。（這個坑在 clipboard_hdrop_check
# 踩過一次，這裡先宣告好。）
# ---------------------------------------------------------------------------
user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetClipboardData.restype = wintypes.HANDLE
user32.GetClipboardData.argtypes = [wintypes.UINT]
user32.OpenClipboard.restype = wintypes.BOOL
user32.OpenClipboard.argtypes = [wintypes.HWND]
user32.CloseClipboard.restype = wintypes.BOOL
user32.keybd_event.argtypes = [wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_void_p]

VK_SNAPSHOT = 0x2C
KEYEVENTF_KEYUP = 0x0002

# 截圖熱鍵只會**打開框選覆蓋層**，不會自己完成截圖 —— 還要選一個範圍。
# 第一版只送了 PrtScn 就等「snip clipboard ok」，於是每次都在「覆蓋層開了、
# 卻沒人框選」的狀態下逾時，看起來像「熱鍵沒生效」。log 裡
# `overlay.BeginSnip -> 1` 是關鍵證據：熱鍵其實成功了。
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
user32.mouse_event.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                               wintypes.DWORD, ctypes.c_void_p]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]

REG_PATH = r"Control Panel\Keyboard"
REG_VALUE = "PrintScreenKeyForSnippingEnabled"


def read_reg():
    """回傳 None（值不存在）/ 0 / 1 / 'error'。

    「不存在」和「1」對系統看起來一樣，但對我們的還原邏輯完全不一樣 ——
    所以這裡一定要分得出來，不能用「讀不到就當 0」打馬虎眼。
    """
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ) as k:
            try:
                v, _ = winreg.QueryValueEx(k, REG_VALUE)
                return int(v)
            except FileNotFoundError:
                return None
    except OSError:
        return "error"


def write_reg(value):
    """value=None → 刪掉這個值（還原成「從來沒設過」）。"""
    import winreg

    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_SET_VALUE) as k:
        if value is None:
            try:
                winreg.DeleteValue(k, REG_VALUE)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(k, REG_VALUE, 0, winreg.REG_DWORD, int(value))


def log_path():
    la = os.environ.get("LOCALAPPDATA")
    if not la:
        return None
    return os.path.join(la, "CapWGC", "gui.log")


def log_lines():
    p = log_path()
    if not p or not os.path.exists(p):
        return []
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def wait_for(pred, timeout=6.0, poll=0.08):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(poll)
    return None


def send_prtsc():
    """送一次 PrtScn。keybd_event 送出的是硬體層級的事件，會走
    RegisterHotKey 那條路 —— 這一點由 prtsc_probe 的對照組證明過
    （Ctrl+Alt+W 用同樣方式注入，receives=1）。"""
    user32.keybd_event(VK_SNAPSHOT, 0, 0, None)
    time.sleep(0.05)
    user32.keybd_event(VK_SNAPSHOT, 0, KEYEVENTF_KEYUP, None)


def drag_region():
    """框一個範圍並放開 —— 截圖流程的最後一步。

    一定要有這一步：PrtScn 只是把覆蓋層打開（見上面的說明），沒有框選就
    不會產生截圖。拖曳用 SetCursorPos + mouse_event，不能用 PostMessage
    送 client 座標 —— 覆蓋層讀的是 GetCursorPos 與 SetCapture。
    """
    x1, y1, x2, y2 = 200, 200, 560, 380
    user32.SetCursorPos(x1, y1)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, None)
    # 中間要分段移動：一次跳到終點有些實作只看到「按下就在終點」，
    # 而且拖曳的軌跡會影響覆蓋層的即時預覽。
    steps = 6
    for i in range(1, steps + 1):
        user32.SetCursorPos(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
        time.sleep(0.04)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, None)


def snip_once():
    """觸發截圖並完成它。回傳 True = 真的產生了一張截圖。"""
    send_prtsc()
    # 覆蓋層是另一個視窗，等它真的建起來再拖 —— 熱鍵剛按下就拖會拖到桌面。
    if not wait_for(lambda: overlay_open_in_log(), timeout=3.0):
        return False
    drag_region()
    return True


def overlay_open_in_log():
    ls = log_lines()
    return ls and "overlay.BeginSnip" in ls[-1]


def clipboard_has_bitmap():
    if not user32.OpenClipboard(None):
        return False
    try:
        return bool(user32.IsClipboardFormatAvailable(15))  # CF_HDROP=15
    finally:
        user32.CloseClipboard()


def kill_app():
    """確保沒有 CapWGC 在跑。

    這不是「保險」而是**必要步驟**：程式有單一實例互斥體，而 --prtsc-on /
    --prtsc-off 是「改完就退出」的一次性動作。常駐行程活著時再啟動一個，
    它會撞上互斥體、跳出「已經在執行」，然後回傳 0 —— 看起來成功，實際
    什麼都沒做。第一版就是這樣，量到「registry 沒變」還以為是寫入失敗。
    """
    subprocess.run(["taskkill", "/F", "/IM", "CapWGC.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)


def start_app(exe):
    p = subprocess.Popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    return p


def stop_app(p):
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    kill_app()


def snip_fired_since(mark, timeout):
    """從 log 的 mark 之後，有沒有出現「截圖成功」那一行。

    注意這裡比的是 mark 之後的整段，不是單行 —— 提示訊息是多行的，
    「已存檔：…」會落在下一行（這個坑 toast_click_check 踩過）。
    """
    return wait_for(
        lambda: any("snip clipboard ok" in ln for ln in log_lines()[mark:]),
        timeout=timeout)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join("out", "CapWGC.exe")
    exe = os.path.abspath(exe)
    if not os.path.exists(exe):
        print(f"FAIL: no exe at {exe}")
        return 2

    # 記下原始狀態，最後一定要還原 —— 這支腳本動的是使用者的系統設定。
    original = read_reg()
    print(f"registry before: {original!r}")
    if original == "error":
        print("SKIP: cannot read the registry key at all")
        return 2

    fails = []
    proc = None
    try:
        # 從乾淨的「Windows 預設」開始：值不存在 = PrtScn 開剪取工具。
        write_reg(None)
        kill_app()

        # ---- 對照組：接管之前，PrtScn 不該是我們的 ----
        # 這一組是必要的。沒有它，「接管後可用」無法排除「本來就可用」。
        proc = start_app(exe)
        mark = len(log_lines())
        snip_once()
        before = snip_fired_since(mark, 2.5)
        print(f"  before takeover: snip fired = {bool(before)}")
        if before:
            fails.append("PrtScn already worked before takeover")
        stop_app(proc)
        proc = None

        # ---- 接管 ----
        # --prtsc-on 等價於「設定視窗勾了那個框並按確定」：它會把 snip 綁到
        # PrtScn（寫進 ini）並把登錄值設成 0。它註冊熱鍵只是為了確認拿得到，
        # 退出時會註銷，所以下面重開常駐行程才註冊得起來。
        subprocess.run([exe, "--prtsc-on"], timeout=30, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        reg_after = read_reg()
        print(f"  registry after --prtsc-on: {reg_after!r}")
        if reg_after != 0:
            fails.append(f"registry should be 0 after takeover, got {reg_after!r}")

        # ---- 接管之後：真的按一次 PrtScn，並且完成截圖 ----
        proc = start_app(exe)
        mark = len(log_lines())
        snip_once()
        after = snip_fired_since(mark, 8.0)
        print(f"  after takeover: snip fired = {bool(after)}")
        if not after:
            fails.append("PrtScn did not trigger a snip after takeover")
        else:
            for ln in log_lines()[mark:]:
                if "snip clipboard ok" in ln:
                    print(f"    {ln.strip()}")
        stop_app(proc)
        proc = None

        # ---- 還原 ----
        subprocess.run([exe, "--prtsc-off"], timeout=30, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        back = read_reg()
        print(f"  registry after --prtsc-off: {back!r}")
        # 原本不存在 → 還原後必須仍然不存在（不能留一個我們寫的 1）
        if original is None:
            if back is not None:
                fails.append(f"original was ABSENT, expected ABSENT after restore, "
                             f"got {back!r}")
        elif back != original:
            fails.append(f"expected {original!r} after restore, got {back!r}")

        # 還原之後 PrtScn 不該再是我們的（回到 Windows 手上）
        proc = start_app(exe)
        mark = len(log_lines())
        snip_once()
        still = snip_fired_since(mark, 2.5)
        print(f"  after restore: snip fired = {bool(still)}")
        if still:
            fails.append("PrtScn still triggers a snip after release")

    finally:
        stop_app(proc)
        # 無條件還原：就算前面炸了，也不能把使用者的 PrtScn 留在被接管的狀態。
        try:
            write_reg(original if original != "error" else None)
        except OSError as e:
            print(f"WARN: could not restore registry: {e}")

    print()
    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: takeover works, and release restores the original state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
