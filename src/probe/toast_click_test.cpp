// ============================================================================
// toast_click_test.cpp — 證明 Toast 的「點一下開啟」真的會執行動作
// ============================================================================
//
// 這個測試存在的原因：
//   `已儲存（點一下開啟）` 這個標題是舊時代留下來的。以前它靠托盤氣球回的
//   `NIN_BALLOONUSERCLICK`，改用自家 `ui::Toast` 之後那則通知**永遠不會來**
//   —— 標題在承諾一個不存在的動作，點下去只把提示關掉。修法是在 Toast 上
//   加 onClick 回呼（見 ui.h 的 Toast::Action）。
//
//   但這種「接了線」的修正最容易自我感覺良好：程式碼看起來對，實際點下去
//   沒反應也說得過去。所以要有一支不需要人去點的自動測試 —— 用
//   `PostMessageW(WM_LBUTTONDOWN)` 直接打在 Toast 的視窗上，數回呼有沒有響。
//
// 驗什麼（每一項都會印出來，最後一行 PASS/FAIL）：
//   1. 純通知（沒有 onClick）：點左鍵 → 回呼不存在，提示要關掉，不能當掉。
//   2. 有 onClick：點左鍵 → 回呼**剛好一次**，且提示關掉。
//   3. 有 onClick：點右鍵 → 回呼**零次**，提示關掉。
//      （可點擊的提示旁邊常常是別的東西，右鍵誤觸不該開檔。）
//   4. 提示關掉之後回呼要作廢：再點一次不能又叫起來。
//      這一項抓的是「onClick_ 沒清乾淨，殘留到下一次」——
//      那會變成點到新的一則卻執行上一則的動作。
//
// 用法：out\toast_click_test.exe
// ============================================================================

#include "../app/ui.h"

#include <windows.h>

#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// 點一下提示。座標取視窗中心 —— 那一定在 HTCLIENT 裡。
void ClickAt(HWND h, WPARAM btn) {
    RECT r{};
    GetWindowRect(h, &r);
    const LPARAM pt = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    PostMessageW(h, btn, 0, pt);
    // PostMessage 是排進佇列，不是同步執行。這裡要把它抽乾 —— 不然「回呼
    // 有沒有響」會讀到動作之前的狀態，測試就永遠是綠的。
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ui::Toast toast;
    if (!toast.Create()) {
        printf("FAIL: Toast::Create failed\n");
        return 1;
    }

    printf("toast_click_test: left click runs the action, right click only closes\n");

    // ---- 1. 純通知：沒有 onClick ----
    {
        printf("\n[1] plain notification (no onClick)\n");
        toast.Show(L"已截圖", L"100 x 100 像素，已複製到剪貼簿", ui::Toast::Kind::Info,
                   0 /* 不自動消失，讓測試來關 */);
        Check(toast.visible(), "shown");
        Check(!toast.clickable(), "clickable() is false");
        ClickAt(toast.hwnd(), WM_LBUTTONDOWN);
        Check(!toast.visible(), "left click closed it");
    }

    // ---- 2. 有 onClick：左鍵要執行 ----
    {
        printf("\n[2] actionable toast, left click\n");
        int calls = 0;
        toast.Show(L"已儲存（點一下開啟）", L"rec.mp4  3.2 秒  1.4 MB", ui::Toast::Kind::Info,
                   0, [&calls] { ++calls; });
        Check(toast.visible(), "shown");
        Check(toast.clickable(), "clickable() is true");
        ClickAt(toast.hwnd(), WM_LBUTTONDOWN);
        Check(calls == 1, "action ran exactly once");
        Check(!toast.visible(), "left click closed it");
    }

    // ---- 3. 有 onClick：右鍵只關掉，不執行 ----
    {
        printf("\n[3] actionable toast, right click\n");
        int calls = 0;
        toast.Show(L"已儲存（點一下開啟）", L"rec.mp4  3.2 秒  1.4 MB", ui::Toast::Kind::Info,
                   0, [&calls] { ++calls; });
        Check(toast.visible(), "shown");
        ClickAt(toast.hwnd(), WM_RBUTTONDOWN);
        Check(calls == 0, "action did NOT run");
        Check(!toast.visible(), "right click closed it");
    }

    // ---- 4. 關掉之後回呼作廢 ----
    {
        printf("\n[4] action must not survive Hide()\n");
        int calls = 0;
        toast.Show(L"已儲存（點一下開啟）", L"rec.mp4", ui::Toast::Kind::Info, 0,
                   [&calls] { ++calls; });
        toast.Hide();
        Check(!toast.visible(), "hidden");
        // Hide 之後點同一個視窗（視窗還在，只是 SW_HIDE）。這裡是「殘留
        // 回呼」最容易漏掉的情境：視窗物件還在，onClick_ 也還在的話，
        // 下一次顯示的任何一則提示被點到都會跑這個舊動作。
        Check(!toast.clickable(), "clickable() is false after Hide()");
        ClickAt(toast.hwnd(), WM_LBUTTONDOWN);
        Check(calls == 0, "action not called after Hide()");
    }

    // ---- 5. 顯示新的一則，不能帶上舊的動作 ----
    {
        printf("\n[5] a new plain toast must not inherit the old action\n");
        int calls = 0;
        toast.Show(L"已儲存（點一下開啟）", L"rec.mp4", ui::Toast::Kind::Info, 0,
                   [&calls] { ++calls; });
        toast.Show(L"已截圖", L"100 x 100 像素", ui::Toast::Kind::Info, 0);
        Check(!toast.clickable(), "new toast is not clickable");
        ClickAt(toast.hwnd(), WM_LBUTTONDOWN);
        Check(calls == 0, "old action did not run on the new toast");
    }

    toast.Destroy();
    CoUninitialize();

    printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
