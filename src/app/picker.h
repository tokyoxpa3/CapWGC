// ============================================================================
// picker.h — Discord 式「選來源」縮圖牆
// ============================================================================
//
// 為什麼要另開一個選擇器，而不是沿用覆蓋層的「單擊選視窗」：
// 單擊選視窗要求使用者先「瞄準」，而被選的視窗常常被別的視窗蓋住一半、
// 或是標題記不起來。縮圖牆把「有哪些東西可以錄」一次攤開，選的是「看起來
// 對的那一個」，這正是 Discord / OBS 的做法。
//
// 用法：
//   PickResult r = app::PickSource(hwnd);
//   if (r.ok) { r.hwnd ? 錄視窗 : 錄 r.monitor }
//
// 這是同步 modal：內部有自己的訊息迴圈（跟設定視窗一樣），呼叫端不要在中間
// 期待主迴圈會跑。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace app {

struct PickResult {
    bool ok = false;
    // 選了「自己拉一個範圍」。這時候 hwnd / monitor 都是空的，呼叫端要接著
    // 開框選覆蓋層 —— picker 自己不做框選（它蓋在螢幕上，先關掉才框得乾淨）。
    bool region = false;
    // 按了 footer 的「設定」。ok 會是 false（沒有選任何來源），呼叫端看到這個
    // 旗標就開設定視窗。用旗標而不是「請呼叫端自己去猜目前模式」：picker 是
    // 同步 modal，它關掉之後外面沒有任何線索知道剛才按的是哪一顆。
    bool settings = false;
    HWND hwnd = nullptr;         // 非空 = 選了視窗
    HMONITOR monitor = nullptr;  // 非空 = 選了整個螢幕
    RECT rect{};                 // 螢幕座標（HUD 用）
};

// 縮圖會在顯示自己之前抓完：先顯示再抓，自己就擋在目標前面了。
//
// live = true（預設）：開啟後每 500ms 重掃視窗清單、每 100ms 補抓一批縮圖，
// 視窗開了/關了/還原了都會自己跟上。live = false 只抓一次快照，是給 QA
// 截圖用的（要可重現的畫面，不能被中途的刷新干擾）。
PickResult PickSource(HWND owner, bool live = true);

} // namespace app
