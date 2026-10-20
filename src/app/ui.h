// ============================================================================
// ui.h — GUI 的共用小元件
// ============================================================================
//
// 這裡刻意只放「錄屏軟體一定會用到、而且很容易寫錯」的四個東西：
//
//   MakeDotIcon   托盤圖示。執行期畫，不用 .ico 資源 —— 這樣 build.bat 不必
//                 多接一個 rc.exe，圖示也能隨狀態換色（錄製中變紅）。
//   DimPanel      半透明遮罩。用 WS_EX_LAYERED + LWA_ALPHA 做「整片均勻變暗」，
//                 不用 per-pixel alpha —— 均勻變暗本來就不需要。
//   BorderWindow  中空外框。用 SetWindowRgn 挖掉中間，所以只有框那幾 px 屬於
//                 這個視窗，框裡面完全沒有覆蓋，也就沒有「畫上去會入鏡」的問題。
//   TextBadge     深底白字的小徽章。用 D2D + DirectWrite（文字邊緣比 GDI 乾淨），
//                 圓角靠視窗 region 做，不需要 per-pixel alpha。
//
// 為什麼遮罩與外框用 GDI、文字用 D2D：前兩者只是純色填滿，GDI 一個 FillRect
// 就結束；文字才是 D2D 有價值的地方（DirectWrite 的字距與抗鋸齒）。
// 全部都自己畫、不拉框架，維持「小、秒開」這個相對 OBS 的賣點。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <climits>
#include <functional>
#include <string>

namespace ui {

// ---- 共用 style ----
// 最上層、不搶焦點、不進工作列。tool window 是關鍵：沒有它，alt-tab 會看到
// 一堆「遮罩」視窗。
constexpr DWORD kOverlayStyle = WS_POPUP;
constexpr DWORD kOverlayExStyle =
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;

// 是否把我們自己的視窗排除在螢幕擷取之外。
//
// 預設 true：錄整個螢幕時，計時徽章可能被夾進畫面裡，排除掉才不會錄到自己的
// UI。但這個 API 連「使用者截圖」都會排除 —— QA 截圖會看不到任何東西，
// 誤判成「元件沒畫出來」。所以留一個旗標，開發／自我測試時關掉它。
void SetCaptureExclusion(bool enabled);

// 把單一視窗標成「不進螢幕擷取」。給錄製中的漂浮控件用：全螢幕錄影時它疊在
// 畫面裡，只能靠 WDA_EXCLUDEFROMCAPTURE 把自己拿掉。
void ApplyCaptureExclusion(HWND h);

// ---- 托盤圖示 ----
// recording = true 畫紅底白方塊（停止），false 畫深底紅點（可錄）。
HICON MakeDotIcon(bool recording, int size = 16);

// ---- 半透明遮罩 ----
class DimPanel {
public:
    bool Create(BYTE alpha = 110);
    // 回傳 true = 這次真的動了（位置/大小變了，或從隱藏變成顯示）。
    //
    // below：顯示時要插在「哪個視窗的正下方」。傳工具列的 hwnd 進來，遮罩就
    // 永遠排在工具列下面 —— 這是拖曳不閃爍的根本解：不必先把遮罩插到最上層、
    // 再回頭把工具列抬回來（那一來一回就是肉眼看到的閃）。
    bool SetRect(const RECT& r, HWND below = nullptr);   // 螢幕座標；空矩形 = 隱藏
    void Show(bool on);
    void Destroy();
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    RECT last_{};      // 上一次真的套用的矩形（見 SetRect 的說明）
    bool visible_ = false;
};

// ---- 中空外框 ----
// SetOuterRect 給的是「外緣」。框會往內長 thickness 像素，所以傳入
// expand(擷取區域, thickness) 就得到「完全落在擷取範圍之外」的框 ——
// 這是錄製指示不會入鏡的原因，不依賴 SetWindowDisplayAffinity。
class BorderWindow {
public:
    bool Create(COLORREF color, int thickness);
    void SetColor(COLORREF color);
    // below：顯示時插在這個視窗的正下方（通常傳工具列），避免蓋住工具列。
    void SetOuterRect(const RECT& r, HWND below = nullptr);
    void Show(bool on);
    void Destroy();
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    HBRUSH brush_ = nullptr;
    int thick_ = 3;
    COLORREF color_ = CLR_INVALID;   // 目前顏色（同色不重建 brush、不重畫）
    RECT last_{};        // 上次真的套用的矩形（同矩形不重做 region）
    bool visible_ = false;
};

// ---- 文字徽章 ----
class TextBadge {
public:
    // font 建議 "Segoe UI"（標籤）或 "Consolas"（計時，等寬不會跳）。
    bool Create(const wchar_t* font, float sizeDip,
                D2D1_COLOR_F bg, D2D1_COLOR_F fg, float radiusDip = 6.0f);
    void SetText(const std::wstring& text);   // 依文字量自動調整視窗寬度
    void MoveTo(int x, int y);                // 左上角，螢幕座標
    void Show(bool on);
    void Destroy();
    HWND hwnd() const { return hwnd_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint();

    HWND hwnd_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* bgBrush_ = nullptr;
    ID2D1SolidColorBrush* fgBrush_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;
    std::wstring text_;
    D2D1_COLOR_F bg_{}, fg_{};
    float sizeDip_ = 13.0f;
    float radiusDip_ = 6.0f;
    int w_ = 0, h_ = 0;
    int x_ = INT_MIN, y_ = INT_MIN;   // 上次 MoveTo 的座標（同位置不重複移動）
    bool visible_ = false;
};

// ---- 頂端工具列（截圖 / OCR）----
//
// 兩個分段、一顆膠囊：左邊「截圖」、右邊「OCR」。跟微軟的 Win+Shift+S 一樣，
// 進到框選畫面就先給一顆看得見的動作列，不用記熱鍵。
//
// 為什麼是獨立視窗而不是畫在 Overlay 上：Overlay 的 owner 是「整片虛擬桌面」
// 的隱形視窗，畫在上面的東西要自己處理命中測試與重繪。獨立成一顆小視窗，
// 形狀交給 alpha 通道（見下方 per-pixel alpha），滑鼠事件直接進來，命中測試
// 就是 PtInRect 兩個矩形。
//
// 圓角與描邊用 per-pixel alpha（32bpp DIB + UpdateLayeredWindow），不用
// SetWindowRgn —— region 是 1-bit 遮罩，圓角在 100% DPI 下一定是階梯。
class Toolbar {
public:
    enum class Action { None, Snip, Ocr };

    ~Toolbar();

    // owner：Overlay 的 owner 視窗。設了 owner，這顆一定會蓋在 Overlay 之上
    // （owned window 永遠在其 owner 之上），不必跟一堆 topmost 搶 z-order。
    // onPick 在使用者點到某個分段時被呼叫（在 UI 執行緒）。
    bool Create(HWND owner, std::function<void(Action)> onPick);

    // 在 workArea（螢幕工作區）的頂端中央顯示。active = 目前預選的分段。
    void Show(const RECT& workArea, Action active);
    void SetActive(Action active);
    void SetActionEnabled(Action action, bool enabled);   // OCR 不可用時畫成灰的
    void Hide();
    void Destroy();

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Layout(const RECT& workArea);
    void Paint();
    bool EnsureSurface(int w, int h);
    void ReleaseSurface();
    void ReleaseTarget();
    unsigned char AlphaAt(int x, int y) const;
    Action HitTest(int x, int y) const;
    void SetHover(Action a);

    HWND hwnd_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    ID2D1SolidColorBrush* bg_ = nullptr;
    ID2D1SolidColorBrush* sep_ = nullptr;
    ID2D1SolidColorBrush* fg_ = nullptr;
    ID2D1SolidColorBrush* fgOn_ = nullptr;
    ID2D1SolidColorBrush* fgOff_ = nullptr;
    ID2D1SolidColorBrush* actBg_ = nullptr;
    ID2D1SolidColorBrush* hoverBg_ = nullptr;
    ID2D1StrokeStyle* round_ = nullptr;
    IDWriteTextFormat* fmt_ = nullptr;

    std::function<void(Action)> onPick_;
    bool visible_ = false;
    float scale_ = 1.0f;
    int w_ = 0, h_ = 0;
    RECT segSnip_{};
    RECT segOcr_{};

    Action pressed_ = Action::None;
    Action hoverSeg_ = Action::None;
    Action active_ = Action::Snip;
    bool snipEnabled_ = true;
    bool ocrEnabled_ = true;
    bool tracking_ = false;
};

// ---- 自己的提示（取代 Shell_NotifyIcon 的氣球）----
//
// 為什麼不用 Shell 的氣球：
//   Win10/11 之後托盤氣球其實就是 Action Center 的一則 toast，走的是**通知
//   佇列**。新的一則不會取代畫面上那一則，而是排隊等它講完；而單則的最短
//   顯示時間是系統設定（預設 5 秒），`uTimeout` 在 Vista 之後就失效，
//   `NIF_REALTIME` 也不再是文件上寫的「立刻取代」。實測結果就是使用者回報的
//   「連續截圖／OCR 時提示慢好幾秒」——不是我們慢，是氣球在排隊。
//
//   自己畫視窗就完全没有佇列：Show() 永遠是立刻換掉畫面那一則。
//
// 形狀與色彩沿用 Toolbar 那顆膠囊（per-pixel alpha + UpdateLayeredWindow），
// 所以圓角不會有 region 的階梯感。
class Toast {
public:
    enum class Kind { Info, Warn, Error };

    // 「點一下」要做的事。沒給就是純通知 —— 點一下只會關掉它。
    //
    // 這是取代 Shell 氣球之後必須自己補回來的一段：以前「已儲存（點一下開啟）」
    // 靠的是 `NIN_BALLOONUSERCLICK` 這個托盤通知，但那個通知只有 Shell 畫的
    // 氣球才會發。改用自家視窗之後沒人發這則訊息，於是標題寫著「點一下開啟」、
    // 點下去卻只把提示關掉 —— 訊息在承諾一個不存在的動作。
    using Action = std::function<void()>;

    ~Toast();

    bool Create();
    // 立刻顯示（取代目前那一則），afterMs 之後自動消失。body 可為空。
    // onClick 只有**左鍵**會觸發；右鍵／中鍵一律只關掉（誤觸不該開啟檔案）。
    void Show(const std::wstring& title, const std::wstring& body, Kind kind,
              int afterMs = 2600, Action onClick = {});
    // 錄影中畫面右下角有漂浮控件時，把提示往上讓開（DIP）。
    void SetBottomInset(int dip);
    void Hide();
    void Destroy();

    bool visible() const { return visible_; }
    bool clickable() const { return onClick_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    // 關掉提示，並在左鍵時把 onClick_ 叫起來。抽出來是為了讓三種按鍵共用。
    void Dismiss(bool leftClick);
    void Relayout();
    void Paint();
    bool EnsureSurface(int w, int h);
    void ReleaseSurface();
    void ReleaseTarget();

    HWND hwnd_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* bg_ = nullptr;
    ID2D1SolidColorBrush* border_ = nullptr;
    ID2D1SolidColorBrush* fgTitle_ = nullptr;
    ID2D1SolidColorBrush* fgBody_ = nullptr;
    ID2D1SolidColorBrush* accent_ = nullptr;
    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtBody_ = nullptr;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;

    std::wstring title_;
    std::wstring body_;
    Kind kind_ = Kind::Info;
    Action onClick_;
    float scale_ = 1.0f;
    int w_ = 0, h_ = 0;
    int bottomInset_ = 0;
    bool visible_ = false;

    // 版面（像素，相對於視窗左上角），Relayout 算好、Paint 直接用
    int padX_ = 0, padY_ = 0;
    float dotCx_ = 0.0f, dotCy_ = 0.0f, dotR_ = 0.0f;
    int textLeft_ = 0;
    float titleTop_ = 0.0f, titleH_ = 0.0f;
    float bodyTop_ = 0.0f, bodyH_ = 0.0f;
};

// 找「游標底下真正的那個視窗」，跳過自己建立的覆蓋層。
// WindowFromPoint 不行：owner 視窗蓋住整個螢幕，永遠回傳它。
HWND WindowUnderPoint(POINT pt);

// 螢幕座標的四邊外擴（負的 delta 就是內縮）。
RECT Inflate(const RECT& r, int dx, int dy);
RECT Normalize(const POINT& a, const POINT& b);

} // namespace ui
