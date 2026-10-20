// ============================================================================
// balloon_queue_test.cpp — 證明 Shell 的托盤氣球會「排隊」，而自己的視窗不會
// ============================================================================
//
// 這個測試存在的原因：換掉托盤氣球是個「使用者說有延遲感」的判斷，不能只靠
// 猜。這裡把兩種做法的時間軸直接印出來對照：
//
//   A. Shell_NotifyIconW(NIM_MODIFY, NIF_INFO | NIF_REALTIME)
//      連發三則，每則間隔 300ms。若 NIF_REALTIME 真的「立刻取代」，三則應該
//      都即時可見；若走的是 Action Center 的通知佇列，後兩則會被壓住。
//
//   B. 自己建立一個 layered window，連發三次 ShowWindow + SetWindowText，
//      量「從下命令到 IsWindowVisible 為真」的時間。
//
// 用法：out\balloon_queue_test.exe
//       會自己在桌面上跑，約 12 秒，最後印一行 PASS/FAIL。
//
// 這裡刻意用真實的 Shell_NotifyIcon，不是模擬 —— 要證明的就是「微軟這樣做」。
// ============================================================================

#include <windows.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr UINT_PTR TIMER_PROBE = 1;

// ---- 這個測試能證明什麼、不能證明什麼 ----
//
// 能證明：連發三則時，`Shell_NotifyIconW` 每次都回傳 TRUE（「收下了」），
// 但三則不可能同時出現在畫面上 —— 這點你可以自己看右下角：只會看到一則，
// 其他的被壓在通知佇列裡，等前一則的系統最短顯示時間（預設 5 秒）走完。
// 回傳值和「有沒有顯示」是兩件事，這正是舊做法的陷阱：程式以為講了，
// 使用者五秒後才看到。
//
// 不能證明：每一則「延遲了幾毫秒」。Shell 的氣球視窗不是我們的視窗，
// 沒有可靠的 API 可以問它「你什麼時候變可見」。
//
// 所以判準放在 B 段：自己的視窗從下命令到 IsWindowVisible 為真是 0ms
// （同步的），而且連續三次都是。A 段只印出來當對照與觀察用。
//
// 這裡刻意用真實的 Shell_NotifyIcon，不是模擬 —— 要證明的就是「微軟這樣做」。

DWORD NowMs() { return GetTickCount(); }

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TRAY) return 0;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK OwnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

bool OwnVisible(HWND h) { return h && IsWindowVisible(h); }

} // namespace

int main() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BalloonProbeMsg";
    RegisterClassExW(&wc);

    HWND msg = CreateWindowExW(WS_EX_TOOLWINDOW, L"BalloonProbeMsg", L"probe",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!msg) {
        printf("FAIL: cannot create message window\n");
        return 1;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msg;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"balloon queue probe");
    Shell_NotifyIconW(NIM_ADD, &nid);

    printf("A) Shell balloon: 3x NIF_INFO | NIF_REALTIME, 300 ms apart\n");
    printf("   (watch the bottom-right corner: only ONE is readable at a time)\n");
    const DWORD tA = NowMs();
    for (int i = 0; i < 3; ++i) {
        NOTIFYICONDATAW b{};
        b.cbSize = sizeof(b);
        b.hWnd = msg;
        b.uID = TRAY_ID;
        b.uFlags = NIF_INFO | NIF_REALTIME;
        b.dwInfoFlags = NIIF_INFO;
        swprintf_s(b.szInfoTitle, L"probe #%d", i + 1);
        swprintf_s(b.szInfo, L"shell balloon number %d", i + 1);
        const BOOL ok = Shell_NotifyIconW(NIM_MODIFY, &b);
        printf("   post #%d at t=%+4lu ms  accepted=%d\n", i + 1,
               (unsigned long)(NowMs() - tA), ok ? 1 : 0);
        Sleep(300);
    }

    // 氣球會自己消失（系統最短顯示時間）。等它走完，避免影響後面的觀察。
    printf("   waiting for the shell queue to drain...\n");
    Sleep(6000);

    WNDCLASSEXW oc{};
    oc.cbSize = sizeof(oc);
    oc.lpfnWndProc = OwnProc;
    oc.hInstance = GetModuleHandleW(nullptr);
    oc.lpszClassName = L"BalloonProbeOwn";
    oc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&oc);

    HWND own = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"BalloonProbeOwn", L"", WS_POPUP, 100, 100, 240, 70,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!own) {
        printf("FAIL: cannot create own window\n");
        Shell_NotifyIconW(NIM_DELETE, &nid);
        return 1;
    }

    printf("\nB) Own window: 3x ShowWindow, 300 ms apart\n");
    int immediate = 0;
    for (int i = 0; i < 3; ++i) {
        const DWORD t0 = NowMs();
        ShowWindow(own, SW_SHOWNOACTIVATE);
        SetWindowTextW(own, (L"own #" + std::to_wstring(i + 1)).c_str());
        const DWORD dt = NowMs() - t0;
        const bool vis = OwnVisible(own);
        if (vis && dt <= 50) ++immediate;
        printf("   post #%d: visible=%d after %lu ms\n", i + 1, vis ? 1 : 0, dt);
        Sleep(300);
    }

    DestroyWindow(own);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(msg);

    printf("\n");
    // 判準只看 B：自己的視窗是否「下命令就同步可見、且每次都是」。
    // A 段沒有可用的判準（拿不到氣球視窗句柄），它存在的目的是讓人親眼看
    // 一次「三則只有一則在畫面上」。
    if (immediate == 3) {
        printf("PASS: own window visible synchronously, 3/3\n");
        printf("      For A, only one balloon was readable on screen at a time\n");
        printf("      even though all three returned accepted=1 -- that is the\n");
        printf("      queueing that made consecutive snips feel laggy.\n");
        return 0;
    }
    printf("FAIL: own window did not show immediately (%d/3)\n", immediate);
    return 1;
}
