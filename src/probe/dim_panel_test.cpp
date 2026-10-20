// ============================================================================
// dim_panel_test.cpp — 驗證遮罩面板「重建之後還畫得出來」
// ============================================================================
//
// 為什麼需要這個測試：
//   DimPanel 的生命週期是「每次框選 Create 一次、結束 Destroy 一次」。這種
//   「同一個物件反覆建立又拆掉」的用法最容易壞在狀態沒歸零：Destroy 只清了
//   hwnd_，而 visible_ 還是 true、last_ 還是上一次的矩形。SetRect 的
//   early-out 是「矩形沒變就什麼都不做」，於是新建的視窗永遠不會被
//   ShowWindow —— 症狀是「遮罩開起來了，但畫面沒有變暗」。
//
//   「矩形沒變」在真實使用裡是很常見的情況：還沒拖曳時整片變暗的矩形就是
//   整個虛擬桌面，每次完全一樣。所以這個 bug 是間歇性的，手動點幾次不見得
//   撞得到，只有把它縮成「同一個矩形套用兩次」才穩定重現。
//
// 這個測試不需要人互動，也不需要看畫面：直接問 IsWindowVisible。
//   build.bat dimtest -> out\dim_panel_test.exe
// ============================================================================

#include "../app/ui.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

// 一次完整的「Create -> SetRect -> Destroy」。回傳面板有沒有真的顯示出來。
bool Cycle(ui::DimPanel& d, const RECT& r, const char* label) {
    if (!d.Create(110)) {
        printf("  %s: Create FAILED\n", label);
        return false;
    }
    const bool moved = d.SetRect(r, nullptr);
    const bool vis = IsWindowVisible(d.hwnd()) != FALSE;
    printf("  %s: SetRect moved=%d visible=%d hwnd=%p\n", label, moved ? 1 : 0,
           vis ? 1 : 0, (void*)d.hwnd());
    d.Destroy();
    return moved && vis;
}

} // namespace

int main() {
    const RECT full{0, 0, 1600, 900};

    printf("dim_panel_test: same rect, two create/destroy cycles\n");

    // 這正是出問題的形狀：兩次都用同一個矩形。
    ui::DimPanel d;
    Check("cycle 1 applies the rect and shows", Cycle(d, full, "cycle 1"));
    Check("cycle 2 (same rect) also shows", Cycle(d, full, "cycle 2"));
    Check("cycle 3 (same rect) also shows", Cycle(d, full, "cycle 3"));

    // 反過來也要成立：矩形變了之後一定要動。
    printf("dim_panel_test: rect change after reuse\n");
    ui::DimPanel e;
    Check("first rect", Cycle(e, RECT{0, 0, 800, 600}, "small"));
    Check("different rect after reuse",
          Cycle(e, RECT{0, 0, 1600, 900}, "full"));

    // 空矩形必須真的隱藏，而且之後給回正常矩形要能再顯示（這是框選結束時
    // 「四塊遮罩裡沒有選取的那幾塊」走的路）。
    printf("dim_panel_test: hide on empty rect, then show again\n");
    ui::DimPanel third;
    if (!third.Create(110)) {
        printf("  Create FAILED\n");
        return 1;
    }
    third.SetRect(full, nullptr);
    const bool hiddenOk = !third.SetRect(RECT{0, 0, 0, 0}, nullptr) ||
                          !IsWindowVisible(third.hwnd());
    Check("empty rect hides the panel", hiddenOk);
    const bool reshowOk = third.SetRect(full, nullptr) &&
                          IsWindowVisible(third.hwnd());
    Check("same rect shows again after hiding", reshowOk);
    third.Destroy();

    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: %d check(s)\n", g_failures);
    return 1;
}
