// ============================================================================
// controlbar.h — 錄製中的漂浮控件（停止 / 麥克風靜音）
// ============================================================================
//
// 為什麼不再是「外框 + 計時徽章」：
//
//   外框會讓被錄的畫面看起來像被裁掉一塊，而且錄整個螢幕時它根本畫不到外面去。
//   改成單一顆小膠囊：兩顆圖示（停止、麥克風），可以拖到任何位置。
//
// 它一定不會入鏡，靠的是 WDA_EXCLUDEFROMCAPTURE（SetWindowDisplayAffinity）：
// WGC 在合成擷取畫面時會把這個視窗整個拿掉。注意這是「拿掉」不是「變黑」，
// 所以膠囊後面原本的畫面會正常出現在成品裡。
//
// 位置策略：優先放在擷取範圍**外側**（下方，放不下就上方）。真的沒地方放
// （錄整個螢幕就是這種情況）才疊在右下角 —— 那時就完全靠 display affinity。
// 兩條路都留著的理由：外側那條在舊系統／某些驅動上仍然有效，能少依賴一個
// 「失敗時無聲」的 API。
//
// 互動（點擊不搶焦點，WS_EX_NOACTIVATE）：
//   * 左鍵點圖示 = 動作
//   * 左鍵拖曳膠囊本體 = 移動（位移 < 4px 視為點擊，不會誤判成拖曳）
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>

#include <functional>

namespace app {

class ControlBar {
public:
    enum class Button { None, Stop, Mic };
    using ClickFn = std::function<void(Button)>;
    // 拖曳結束時回報「新位置相對擷取範圍右下角的位移」，呼叫端負責持久化。
    using MoveFn = std::function<void(int dx, int dy)>;

    ~ControlBar();

    // 建立視窗（只建一次）。onClick / onMoved 在 UI 執行緒被呼叫。
    bool Create(ClickFn onClick, MoveFn onMoved = nullptr);

    // captureRect：擷取範圍（螢幕座標）。micAvailable = 這次錄影有麥克風軌；
    // false 時麥克風鈕畫成灰的，點了由呼叫端決定要提示什麼。
    // savedOffset 非空 = 上次拖到的位置（相對右下角），空 = 用預設位置。
    void Show(const RECT& captureRect, bool micAvailable, const POINT* savedOffset);
    void SetMicMuted(bool muted);
    void Hide();
    void Destroy();

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Layout(const RECT& captureRect, const POINT* savedOffset);
    void Paint();
    bool EnsureSurface(int w, int h);
    void ReleaseSurface();
    void ReleaseTarget();
    // 這個像素是不是膠囊的一部分（alpha 幾乎為 0 就讓點擊穿過去）。
    unsigned char AlphaAt(int x, int y) const;
    Button HitTest(int x, int y) const;
    void SetHover(Button b);

    HWND hwnd_ = nullptr;
    // 畫在 32bpp 的 DIB 上、再用 UpdateLayeredWindow 送出去（per-pixel alpha）。
    // 這條路是為了圓角邊緣的抗鋸齒：SetWindowRgn 的 region 是 1-bit，
    // 圓角一定會有一格一格的階梯。
    ID2D1DCRenderTarget* rt_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    ID2D1SolidColorBrush* bg_ = nullptr;
    ID2D1SolidColorBrush* icon_ = nullptr;
    ID2D1SolidColorBrush* iconOff_ = nullptr;
    ID2D1SolidColorBrush* stop_ = nullptr;
    ID2D1SolidColorBrush* hover_ = nullptr;
    ID2D1StrokeStyle* round_ = nullptr;

    ClickFn onClick_;
    MoveFn onMoved_;
    bool visible_ = false;
    bool micAvailable_ = false;
    bool micMuted_ = false;
    RECT capture_{};   // 這次錄影的擷取範圍（算相對位移用）

    float scale_ = 1.0f;
    int w_ = 0, h_ = 0;
    RECT btnStop_{};
    RECT btnMic_{};

    Button pressed_ = Button::None;
    Button hoverBtn_ = Button::None;
    bool dragging_ = false;
    bool moved_ = false;
    bool tracking_ = false;
    POINT dragAnchor_{};
    RECT dragStart_{};
};

} // namespace app
