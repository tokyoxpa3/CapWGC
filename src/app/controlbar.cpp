// ============================================================================
// controlbar.cpp — 錄製中漂浮控件的實作
// ============================================================================

#include "controlbar.h"
#include "ui.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")

namespace app {
namespace {

constexpr wchar_t kClass[] = L"CapWGCBar";

// 全部是 96 DPI 下的 DIP，實際尺寸再乘 scale_。
constexpr int kBtnDip = 28;
constexpr int kPadDip = 4;
constexpr int kGapDip = 2;
constexpr int kMarginDip = 8;      // 與擷取範圍之間的距離
constexpr int kDragSlopDip = 4;    // 位移小於這個就當成「點一下」

// 膠囊的形狀以前靠 SetWindowRgn 的圓角 region 裁出來，代價是邊緣**沒有
// 抗鋸齒**：region 是 1-bit 遮罩，圓角在 100% DPI 下是一格一格的階梯
// （使用者原話：「像素感太強」）。改成 per-pixel alpha：
//   32bpp 的 DIB（premultiplied）→ D2D DC render target 畫 → UpdateLayeredWindow
// 形狀由 alpha 通道決定，圓角與描邊就都是平滑的。
constexpr BYTE kAlpha = 240;   // 整體不透明度（UpdateLayeredWindow 的常數 alpha）

const D2D1_COLOR_F kBg = D2D1::ColorF(0.07f, 0.07f, 0.08f, 1.0f);
const D2D1_COLOR_F kStop = D2D1::ColorF(0.90f, 0.24f, 0.24f, 1.0f);
const D2D1_COLOR_F kIcon = D2D1::ColorF(0.95f, 0.95f, 0.96f, 1.0f);
const D2D1_COLOR_F kIconOff = D2D1::ColorF(0.50f, 0.50f, 0.54f, 1.0f);
const D2D1_COLOR_F kHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);

ID2D1Factory* Factory() {
    static ID2D1Factory* f = [] {
        ID2D1Factory* p = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &p);
        return p;
    }();
    return f;
}

// 麥克風圖示：膠囊 + 半圓弧 + 支架。muted 時整組轉灰並畫一條斜線。
//
// 膠囊的底端一定要在弧的圓心**上方**，否則兩塊會黏成一顆蘑菇（實際踩過：
// 第一版把膠囊畫到弧心，截圖看起來像一把雨傘，完全看不出是麥克風）。
void DrawMic(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
             ID2D1StrokeStyle* round, D2D1_POINT_2F c, float s, bool muted) {
    const float w = 3.0f * s;          // 膠囊半寬
    const float top = c.y - 8.6f * s;
    const float bot = c.y - 1.6f * s;
    D2D1_ROUNDED_RECT body =
        D2D1::RoundedRect(D2D1::RectF(c.x - w, top, c.x + w, bot), w, w);
    rt->FillRoundedRectangle(body, brush);

    // 弧：圓心在膠囊底端，從左邊往**下**掃到右邊。
    //
    // 這裡一定要 COUNTER_CLOCKWISE：D2D 的角度是「從 +x 軸順時針量」，而 y 軸
    // 向下，所以順時針 180°→360° 會從上面繞過去，畫出來是一個拱門（看起來像
    // 雨傘）。逆時針才是從下面兜住麥克風的那條弧。
    const float r = 5.6f * s;
    const D2D1_POINT_2F ctr = D2D1::Point2F(c.x, bot);
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(Factory()->CreatePathGeometry(&path)) &&
        SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(ctr.x - r, ctr.y), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(ctr.x + r, ctr.y),
                                      D2D1::SizeF(r, r), 0.0f,
                                      D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        rt->DrawGeometry(path, brush, 1.5f * s, round);
    }
    if (sink) sink->Release();
    if (path) path->Release();

    rt->DrawLine(D2D1::Point2F(c.x, ctr.y + r),
                 D2D1::Point2F(c.x, c.y + 6.6f * s), brush, 1.5f * s, round);
    rt->DrawLine(D2D1::Point2F(c.x - 3.0f * s, c.y + 6.6f * s),
                 D2D1::Point2F(c.x + 3.0f * s, c.y + 6.6f * s), brush, 1.5f * s, round);

    if (muted) {
        rt->DrawLine(D2D1::Point2F(c.x - 6.0f * s, c.y + 7.2f * s),
                     D2D1::Point2F(c.x + 6.0f * s, c.y - 7.8f * s), brush,
                     1.7f * s, round);
    }
}

} // namespace

ControlBar::~ControlBar() { Destroy(); }

bool ControlBar::Create(ClickFn onClick, MoveFn onMoved) {
    onClick_ = std::move(onClick);
    onMoved_ = std::move(onMoved);
    if (hwnd_) return true;
    if (!Factory()) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // 已註冊會失敗，忽略

    // 沒有 WS_EX_TRANSPARENT：這顆要收滑鼠。WS_EX_NOACTIVATE：點它不會把
    // 使用者的前景視窗搶走（錄影時搶焦點會毀掉正在錄的操作）。
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED,
                            kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    // 不能呼叫 SetLayeredWindowAttributes：它和 UpdateLayeredWindow 互斥，
    // 混用會讓整個視窗變成不更新（畫什麼都看不到）。
    // 全螢幕錄影時它就疊在畫面裡，只能靠這個把它從擷取中拿掉。
    // 注意：它同時會讓一般截圖看不到（見 docs/09），QA 要用 --allow-capture。
    ui::ApplyCaptureExclusion(hwnd_);
    return true;
}

void ControlBar::Show(const RECT& captureRect, bool micAvailable,
                      const POINT* savedOffset) {
    if (!hwnd_) return;
    micAvailable_ = micAvailable;
    if (!micAvailable_) micMuted_ = false;
    capture_ = captureRect;
    Layout(captureRect, savedOffset);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    SetHover(Button::None);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ControlBar::SetMicMuted(bool muted) {
    if (micMuted_ == muted) return;
    micMuted_ = muted;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ControlBar::Hide() {
    if (!hwnd_) return;
    visible_ = false;
    dragging_ = false;
    pressed_ = Button::None;
    SetHover(Button::None);
    ShowWindow(hwnd_, SW_HIDE);
}

void ControlBar::Destroy() {
    ReleaseTarget();
    ReleaseSurface();
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void ControlBar::ReleaseTarget() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (bg_) { bg_->Release(); bg_ = nullptr; }
    if (icon_) { icon_->Release(); icon_ = nullptr; }
    if (iconOff_) { iconOff_->Release(); iconOff_ = nullptr; }
    if (stop_) { stop_->Release(); stop_ = nullptr; }
    if (hover_) { hover_->Release(); hover_ = nullptr; }
    if (round_) { round_->Release(); round_ = nullptr; }
}

void ControlBar::ReleaseSurface() {
    if (memDC_) {
        if (oldBmp_) SelectObject(memDC_, oldBmp_);
        oldBmp_ = nullptr;
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    dibW_ = dibH_ = 0;
}

bool ControlBar::EnsureSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (dib_ && memDC_ && dibW_ == w && dibH_ == h) return true;
    ReleaseSurface();

    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!memDC_) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down：列索引 = y，AlphaAt 才好算
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_ || !bits) {
        ReleaseSurface();
        return false;
    }
    oldBmp_ = SelectObject(memDC_, dib_);
    bits_ = bits;
    dibW_ = w;
    dibH_ = h;
    return true;
}

unsigned char ControlBar::AlphaAt(int x, int y) const {
    if (!bits_ || x < 0 || y < 0 || x >= dibW_ || y >= dibH_) return 0;
    const auto* px = reinterpret_cast<const uint32_t*>(bits_);
    return (unsigned char)(px[(size_t)y * dibW_ + x] >> 24);   // BGRA 裡 alpha 在最高位
}

void ControlBar::Layout(const RECT& cap, const POINT* savedOffset) {
    HMONITOR mon = MonitorFromRect(&cap, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT wa{};
    if (GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    }

    // DPI 用「擷取範圍所在那顆螢幕」的，不是主螢幕的：多螢幕混 DPI 時，
    // 用錯的那個會讓控件大小差一截。
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiX = dpiY = 96;
    }
    scale_ = (float)dpiX / 96.0f;

    const int btn = (int)std::lround(kBtnDip * scale_);
    const int pad = (int)std::lround(kPadDip * scale_);
    const int gap = (int)std::lround(kGapDip * scale_);
    w_ = pad * 2 + btn * 2 + gap;
    h_ = pad * 2 + btn;
    btnStop_ = RECT{pad, pad, pad + btn, pad + btn};
    btnMic_ = RECT{pad + btn + gap, pad, pad + btn * 2 + gap, pad + btn};

    const int margin = (int)std::lround(kMarginDip * scale_);
    int x, y;
    RECT target = wa;   // 最後要把控件夾進哪顆螢幕的工作區
    if (savedOffset) {
        // 使用者拖過 → 沿用「相對擷取範圍右下角」的位移。
        x = cap.right + savedOffset->x;
        y = cap.bottom + savedOffset->y;
        // 允許拖到別顆螢幕（多螢幕時那是合理偏好），但一定要落在**某一顆**
        // 螢幕的工作區裡：拔掉螢幕、改解析度、換 DPI 之後，舊位移可能指向
        // 一個已經不存在的位置，那時就夾到最近的那顆螢幕。
        const RECT want{x, y, x + w_, y + h_};
        MONITORINFO mi2{};
        mi2.cbSize = sizeof(mi2);
        if (GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi2)) {
            target = mi2.rcWork;
        }
    } else {
        x = cap.right - w_;              // 右對齊擷取範圍
        y = cap.bottom + margin;         // 先試下方外側
        if (y + h_ > wa.bottom) y = cap.top - margin - h_;   // 下方沒空間 → 上方
        if (y < wa.top) y = cap.bottom - margin - h_;        // 上下都沒有 → 疊進裡面
    }
    // 一律夾回工作區。這一步是「控件不可能跑到畫面外」的保證：工作區不含
    // 工作列，所以連被工作列蓋住的情況都不會發生。
    if (x + w_ > target.right) x = target.right - w_;
    if (x < target.left) x = target.left;
    if (y + h_ > target.bottom) y = target.bottom - h_;
    if (y < target.top) y = target.top;
    // 螢幕比控件還窄的極端情況：上面的順序會讓 x 停在 left，至少左上角可見。
    if (x < target.left) x = target.left;
    if (y < target.top) y = target.top;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE);

    // 形狀交給 alpha 通道（見檔頭 kAlpha 的說明），這裡不再設 region。
    // 代價是「視窗是矩形的」，圓角外面那幾塊透明區域也會收到滑鼠 ——
    // 所以 WM_NCHITTEST 要用 AlphaAt 判斷，透明的部分回 HTTRANSPARENT。
}

ControlBar::Button ControlBar::HitTest(int x, int y) const {
    const POINT p{x, y};
    if (PtInRect(&btnStop_, p)) return Button::Stop;
    if (PtInRect(&btnMic_, p)) return Button::Mic;
    return Button::None;
}

void ControlBar::SetHover(Button b) {
    if (hoverBtn_ == b) return;
    hoverBtn_ = b;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ControlBar::Paint() {
    if (!hwnd_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    if (!EnsureSurface(rc.right, rc.bottom)) return;

    if (!rt_) {
        // DPI 固定 96：下面的座標全部是已經乘過 scale_ 的像素，不要再被
        // render target 自己縮放一次。
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(Factory()->CreateDCRenderTarget(&props, &rt_))) return;
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        rt_->CreateSolidColorBrush(kBg, &bg_);
        rt_->CreateSolidColorBrush(kStop, &stop_);
        rt_->CreateSolidColorBrush(kIcon, &icon_);
        rt_->CreateSolidColorBrush(kIconOff, &iconOff_);
        rt_->CreateSolidColorBrush(kHover, &hover_);

        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.startCap = D2D1_CAP_STYLE_ROUND;
        sp.endCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        Factory()->CreateStrokeStyle(sp, nullptr, 0, &round_);
    }

    const RECT bindRc{0, 0, rc.right, rc.bottom};
    if (FAILED(rt_->BindDC(memDC_, &bindRc))) return;

    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    rt_->BeginDraw();
    // 全透明：圓角外面必須是「看不見」而不是黑色，這樣 ULW 才有東西可混。
    rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float s = scale_;
    const float rad = (float)std::lround(6.0f * s);

    // 膠囊本體：左右是半圓，圓角半徑 = 高度的一半。
    // 內縮 0.5px 是為了讓 1px 的描邊完整落在像素內，不會被邊界裁掉半條。
    if (bg_) {
        const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, (float)w_ - 0.5f, (float)h_ - 0.5f),
            (float)h_ / 2.0f - 0.5f, (float)h_ / 2.0f - 0.5f);
        rt_->FillRoundedRectangle(pill, bg_);
    }

    // 一點點描邊：深色桌布上純黑膠囊會糊成一團，看不出邊界。
    if (hover_) {
        D2D1_ROUNDED_RECT edge = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, (float)w_ - 0.5f, (float)h_ - 0.5f),
            (float)h_ / 2.0f - 0.5f, (float)h_ / 2.0f - 0.5f);
        rt_->DrawRoundedRectangle(edge, hover_, 1.0f);
    }

    // 懸停高亮
    if (hoverBtn_ != Button::None && hover_) {
        const RECT& r = (hoverBtn_ == Button::Stop) ? btnStop_ : btnMic_;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom),
            rad, rad);
        rt_->FillRoundedRectangle(rr, hover_);
    }

    // ---- 停止：圓角方塊 ----
    if (stop_) {
        const float half = 5.5f * s;
        const D2D1_POINT_2F c = D2D1::Point2F(
            (btnStop_.left + btnStop_.right) / 2.0f, (btnStop_.top + btnStop_.bottom) / 2.0f);
        D2D1_ROUNDED_RECT sq = D2D1::RoundedRect(
            D2D1::RectF(c.x - half, c.y - half, c.x + half, c.y + half), 2.2f * s, 2.2f * s);
        rt_->FillRoundedRectangle(sq, stop_);
    }

    // ---- 麥克風 ----
    {
        ID2D1SolidColorBrush* br = (micMuted_ || !micAvailable_) ? iconOff_ : icon_;
        if (br) {
            const D2D1_POINT_2F c = D2D1::Point2F(
                (btnMic_.left + btnMic_.right) / 2.0f, (btnMic_.top + btnMic_.bottom) / 2.0f);
            DrawMic(rt_, br, round_, c, s, micMuted_);
        }
    }

    const HRESULT hr = rt_->EndDraw();
    EndPaint(hwnd_, &ps);
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        return;
    }

    // 把 DIB 貼上螢幕。位置每次重讀：拖曳時視窗是 SetWindowPos 搬的，
    // 自己記的 x_/y_ 會過期。
    RECT wr{};
    if (!GetWindowRect(hwnd_, &wr)) return;
    HDC screen = GetDC(nullptr);
    if (screen) {
        POINT dst{wr.left, wr.top};
        SIZE size{rc.right, rc.bottom};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, kAlpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }
}

LRESULT CALLBACK ControlBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<ControlBar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self && m == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<ControlBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }

    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (self) self->Paint();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // 點控件不搶前景
    case WM_NCHITTEST: {
        if (!self) break;
        // 圓角外面那幾塊是透明的（alpha = 0），要讓點擊穿過去；不然使用者
        // 會在「看起來是桌面」的地方點到一塊看不見的視窗。
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (self->AlphaAt(p.x, p.y) < 8) return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && self) {
            const POINT pt = [] {
                POINT p{};
                GetCursorPos(&p);
                return p;
            }();
            POINT cp = pt;
            ScreenToClient(h, &cp);
            const Button b = self->HitTest(cp.x, cp.y);
            SetCursor(LoadCursorW(nullptr, (LPCWSTR)(b == Button::None ? IDC_SIZEALL : IDC_HAND)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        self->pressed_ = self->HitTest(pt.x, pt.y);
        self->moved_ = false;
        self->dragging_ = true;
        GetCursorPos(&self->dragAnchor_);
        GetWindowRect(h, &self->dragStart_);
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self) return 0;
        if (self->dragging_) {
            // 拖曳一律用螢幕座標：視窗自己在移動，client 座標會跟著跳。
            POINT cur{};
            GetCursorPos(&cur);
            const int dx = cur.x - self->dragAnchor_.x;
            const int dy = cur.y - self->dragAnchor_.y;
            const int slop = (int)std::lround(kDragSlopDip * self->scale_);
            if (!self->moved_ && (std::abs(dx) > slop || std::abs(dy) > slop)) {
                self->moved_ = true;
            }
            if (self->moved_) {
                int nx = self->dragStart_.left + dx;
                int ny = self->dragStart_.top + dy;
                HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(mon, &mi)) {
                    nx = std::max((int)mi.rcWork.left,
                                  std::min(nx, (int)mi.rcWork.right - self->w_));
                    ny = std::max((int)mi.rcWork.top,
                                  std::min(ny, (int)mi.rcWork.bottom - self->h_));
                }
                SetWindowPos(h, HWND_TOPMOST, nx, ny, 0, 0,
                             SWP_NOACTIVATE | SWP_NOSIZE);
            }
            return 0;
        }
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        self->SetHover(self->HitTest(pt.x, pt.y));
        if (!self->tracking_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            self->tracking_ = true;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self) return 0;
        ReleaseCapture();
        self->dragging_ = false;
        const Button pressed = self->pressed_;
        self->pressed_ = Button::None;
        if (self->moved_) {
            // 拖曳結束：換算成「相對擷取範圍右下角」的位移回報出去。控件自己
            // 不知道設定檔在哪（也不該知道），持久化交給呼叫端。
            self->moved_ = false;
            RECT wr{};
            if (GetWindowRect(h, &wr)) {
                MoveFn fn = self->onMoved_;
                if (fn) {
                    fn((int)(wr.left - self->capture_.right),
                       (int)(wr.top - self->capture_.bottom));
                }
            }
            return 0;
        }
        if (pressed == Button::None) return 0;

        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (self->HitTest(pt.x, pt.y) != pressed) return 0;
        ClickFn fn = self->onClick_;
        if (fn) fn(pressed);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self) {
            self->tracking_ = false;
            self->SetHover(Button::None);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace app
