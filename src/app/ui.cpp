// ============================================================================
// ui.cpp — GUI 共用元件的實作
// ============================================================================

#include "ui.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace ui {
namespace {

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
bool g_captureExclusion = true;

void ExcludeFromCapture(HWND h) {
    if (g_captureExclusion) SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
}

bool EnsureFactories() {
    if (!g_d2d &&
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
        return false;
    }
    if (!g_dw &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dw)))) {
        return false;
    }
    return true;
}

// 註冊視窗類別（同一個 process 只註冊一次）。
void RegisterOnce(const wchar_t* name, WNDPROC proc, HBRUSH bg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = bg;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);   // 已註冊會失敗，忽略
}

// D2D / DWrite 的失敗幾乎都是「畫面一片空白」，沒有 HRESULT 就完全查不到原因。
// 只在 FAILED 時寫一行，正常路徑不碰檔案。
void LogHr(const char* what, HRESULT hr) {
    if (!FAILED(hr)) return;
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring p = std::wstring(dir) + L"\\CapWGC\\ui.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    fprintf(f, "%s failed 0x%08lX\n", what, (unsigned long)hr);
    fclose(f);
}

inline BYTE Clamp8(float v) {
    return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

} // namespace

void SetCaptureExclusion(bool enabled) { g_captureExclusion = enabled; }

void ApplyCaptureExclusion(HWND h) {
    if (h) ExcludeFromCapture(h);
}

// ============================================================================
// 圖示
// ============================================================================

HICON MakeDotIcon(bool recording, int size) {
    if (size <= 0) size = 16;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;              // 負 = top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;

    // 閒置 = 空心紅環（錄影機的紅點，淺色與深色工作列上都看得見）
    // 錄製中 = 實心紅圓 + 白方塊（停止）
    const float c = size * 0.5f;
    const float rOuter = size * 0.46f;
    const float rInner = size * 0.28f;
    const float half = size * 0.155f;

    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (float)x + 0.5f - c;
            const float dy = (float)y + 0.5f - c;
            const float d = std::sqrt(dx * dx + dy * dy);

            // 抗鋸齒：距離邊界 1px 內做線性過渡
            float a = std::min(1.0f, rOuter - d + 0.5f);
            if (!recording) {
                // 挖掉中間 → 空心環
                a = std::min(a, std::max(0.0f, d - rInner + 0.5f));
            }
            if (a <= 0.0f) continue;

            float r = 220, g = 58, b = 58;
            if (recording && std::fabs(dx) < half && std::fabs(dy) < half) {
                r = g = b = 255;   // 白方塊
            }
            // UpdateLayeredWindow 之外的圖示也要 premultiplied alpha
            px[y * size + x] = ((uint32_t)Clamp8(a * 255.0f) << 24) |
                               ((uint32_t)Clamp8(r * a) << 16) |
                               ((uint32_t)Clamp8(g * a) << 8) |
                               (uint32_t)Clamp8(b * a);
        }
    }

    // 單色遮罩：全 0 = 由 alpha 通道決定透明度
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (mask) {
        const int stride = ((size + 15) / 16) * 2;
        std::vector<BYTE> zeros((size_t)stride * size, 0);
        SetBitmapBits(mask, (DWORD)zeros.size(), zeros.data());
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

// ============================================================================
// 半透明遮罩
// ============================================================================

namespace {
LRESULT CALLBACK DimProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
} // namespace

bool DimPanel::Create(BYTE alpha) {
    RegisterOnce(L"CapWGCDim", DimProc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            L"CapWGCDim", L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    // 刻意不設 SetWindowDisplayAffinity：遮罩永遠蓋在擷取範圍之外，本來就不會
    // 入鏡；而這個 API 會讓視窗連「使用者截圖」都看不到，QA 時等於瞎了。
    return true;
}

bool DimPanel::SetRect(const RECT& r, HWND below) {
    if (!hwnd_) return false;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        if (!visible_) return false;      // 本來就隱藏 → 沒有變化
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        // 標成「無效」：下次再給同一個空矩形時上面的 early-out 不能誤判成
        // 「沒變、不用做事」。
        last_ = RECT{};
        return true;
    }

    // 矩形沒變就什麼都不做。
    //
    // 為什麼要這個判斷：拖曳框選時每一格滑鼠移動都會走 UpdateVisuals()，
    // 而「四塊遮罩」裡通常只有兩塊真的變了、另兩塊完全沒動。以前每一塊都
    // 無條件 SetWindowPos(HWND_TOPMOST, ..., SWP_SHOWWINDOW)，等於每格都
    // 做一次「重新插到最上層 + 重新顯示」—— 既把它們抬到工具列頭上，又
    // 逼系統重畫一次，這就是肉眼看到的閃爍。
    if (visible_ && r.left == last_.left && r.top == last_.top &&
        r.right == last_.right && r.bottom == last_.bottom) {
        return false;
    }

    // 關鍵：只有「從隱藏變成顯示」那一次才需要動 z-order；之後的移動一律
    // SWP_NOZORDER。
    //
    // 顯示時插在 below（工具列）的正下方，而不是 HWND_TOPMOST。兩者的差別：
    //   HWND_TOPMOST  → 遮罩跑到 topmost 佇列最前面，蓋住工具列，呼叫端必須
    //                   馬上把工具列抬回來。兩次 z-order 操作 = 一幀閃爍。
    //   below         → 遮罩直接落在工具列下面，工具列從頭到尾沒被蓋住，
    //                   一次補救都不需要。
    // 視窗建立時就帶 WS_EX_TOPMOST，本來就在 topmost 這一層，移動不需要重排。
    if (!visible_) {
        // below 可能是已經被銷毀的舊句柄（工具列重建過）。傳一個無效句柄給
        // SetWindowPos 會讓整個呼叫失敗、視窗根本不會出現，所以先驗一次。
        HWND anchor = (below && IsWindow(below)) ? below : HWND_TOPMOST;
        SetWindowPos(hwnd_, anchor, r.left, r.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(hwnd_, nullptr, r.left, r.top, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    visible_ = true;
    last_ = r;
    return true;
}

void DimPanel::Show(bool on) {
    if (!hwnd_) return;
    if (visible_ == on) return;
    ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = on;
    if (!on) last_ = RECT{};
}

void DimPanel::Destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    // 這三行不是多餘的。DestroyWindow 之後 visible_ 還是 true、last_ 還是上一次
    // 的矩形，而 SetRect 的 early-out 是「矩形沒變就什麼都不做」——
    // 於是下一次 Begin()（遮罩重新 Create 一個新視窗）如果拿到同一個矩形
    // （預設整片變暗就是整個虛擬桌面，每次完全一樣），就會直接 return，
    // 新視窗永遠不會被 ShowWindow：症狀就是「遮罩開起來了但畫面不變暗」，
    // 而且是間歇性的 —— 只要上一次框選的範圍剛好、或第二次開在同一個螢幕上
    // 就中。狀態必須跟物件一起歸零，不能只清 hwnd_。
    visible_ = false;
    last_ = RECT{};
}

// ============================================================================
// 中空外框
// ============================================================================

namespace {
LRESULT CALLBACK BorderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        auto* self = reinterpret_cast<BorderWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) {
            // 視窗已經被 region 裁成「框」，所以整片填滿就是畫框。
            HBRUSH b = (HBRUSH)GetPropW(h, L"brush");
            if (b) FillRect(dc, &rc, b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

bool BorderWindow::Create(COLORREF color, int thickness) {
    thick_ = thickness < 1 ? 1 : thickness;
    RegisterOnce(L"CapWGCBorder", BorderProc, nullptr);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_TRANSPARENT,
                            L"CapWGCBorder", L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    // 同上：外框是往外畫的，像素全在擷取範圍外，不需要 display affinity。
    SetColor(color);
    return true;
}

void BorderWindow::SetColor(COLORREF color) {
    if (!hwnd_) return;
    // 顏色沒變就直接回去。這個函式在 UpdateVisuals() 裡每一格滑鼠移動都會被
    // 呼叫；若照樣「建新 brush → 刪舊 brush → InvalidateRect(TRUE)」，
    // 等於整條外框每秒被強制重畫幾十次 —— 拖曳時看到框在閃就是這樣來的。
    if (color == color_) return;
    color_ = color;
    HBRUSH b = CreateSolidBrush(color);
    if (brush_) DeleteObject(brush_);
    brush_ = b;
    SetPropW(hwnd_, L"brush", (HANDLE)brush_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void BorderWindow::SetOuterRect(const RECT& r, HWND below) {
    if (!hwnd_) return;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        if (visible_) {
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
            last_ = RECT{};
        }
        return;
    }

    // 矩形沒變就完全不做。這裡的動作（SetWindowPos + 重建 region + SetWindowRgn）
    // 在拖曳時每格都會被呼叫，全部跳過才不會讓外框跟著閃。
    if (visible_ && r.left == last_.left && r.top == last_.top &&
        r.right == last_.right && r.bottom == last_.bottom) {
        return;
    }

    // 已經顯示就不再帶 SWP_SHOWWINDOW / 重排 z-order：
    //   1. SHOW 會多觸發一次顯示與重繪；
    //   2. 每次都插到最上層會蓋住工具列，呼叫端又得把工具列抬回來 ——
    //      一來一回就是閃爍。
    // 顯示時插在 below（工具列）下面，工具列就永遠不會被這條框蓋到。
    const bool becomingVisible = !visible_;
    const bool resized = !visible_ || w != (last_.right - last_.left) ||
                         h != (last_.bottom - last_.top);
    if (becomingVisible) {
        HWND anchor = (below && IsWindow(below)) ? below : HWND_TOPMOST;
        SetWindowPos(hwnd_, anchor, r.left, r.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(hwnd_, nullptr, r.left, r.top, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    visible_ = true;
    last_ = r;

    // 尺寸沒變就不必重建 region —— SetWindowRgn 每次都會讓視窗重新計算
    // 可見區域並重畫一次，拖曳時每格做一次就是外框在閃。
    if (!resized) return;

    // 挖掉中間：只有框那幾 px 屬於這個視窗。框裡面完全沒有覆蓋 ——
    // 這就是「指示不會入鏡」的機制，不是靠 display affinity。
    HRGN outer = CreateRectRgn(0, 0, w, h);
    HRGN ring = nullptr;
    if (w > thick_ * 2 && h > thick_ * 2) {
        HRGN inner = CreateRectRgn(thick_, thick_, w - thick_, h - thick_);
        ring = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(ring, outer, inner, RGN_DIFF);
        DeleteObject(inner);
        DeleteObject(outer);
    } else {
        ring = outer;
    }
    SetWindowRgn(hwnd_, ring, TRUE);   // 成功後 ring 歸系統所有
}

void BorderWindow::Show(bool on) {
    if (!hwnd_) return;
    if (visible_ == on) return;   // 狀態沒變就不必再 ShowWindow 一次
    ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = on;
    // 隱藏後 last_ 失效：下次顯示時即使矩形相同也必須重新套用一次。
    if (!on) last_ = RECT{};
}

void BorderWindow::Destroy() {
    if (brush_) { DeleteObject(brush_); brush_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    // 同 DimPanel::Destroy：visible_ 留著 true 會讓下一次 SetOuterRect 的
    // 「矩形沒變」early-out 把新視窗永遠擋在畫面外（框就不見了）。
    visible_ = false;
    last_ = RECT{};
}

// ============================================================================
// 文字徽章
// ============================================================================

bool TextBadge::Create(const wchar_t* font, float sizeDip, D2D1_COLOR_F bg,
                       D2D1_COLOR_F fg, float radiusDip) {
    if (!EnsureFactories()) return false;
    bg_ = bg;
    fg_ = fg;
    sizeDip_ = sizeDip;
    radiusDip_ = radiusDip;

    RegisterOnce(L"CapWGCBadge", WndProc, nullptr);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_TRANSPARENT,
                            L"CapWGCBadge", L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    ExcludeFromCapture(hwnd_);

    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    const HRESULT hrFmt = g_dw->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL,
                                                 sizeDip_ * scale, L"zh-TW", &fmt_);
    if (FAILED(hrFmt)) {
        LogHr("CreateTextFormat", hrFmt);
        return false;
    }
    fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 不自動折行：這是「貼紙」不是段落。折行的話 SetText 量到的寬度會變成
    // 折行後的寬（看起來夠用），但真正原因是被 4096 上限切斷 —— 一行長文字
    // 應該把視窗撐寬，而不是折成好幾行。
    fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void TextBadge::SetText(const std::wstring& text) {
    if (!hwnd_ || !fmt_) return;
    // 文字沒變就什麼都不用做。
    // 拖曳框選時 UpdateVisuals() 每一格都會用「一樣的」尺寸字串呼叫這裡；
    // 若照樣重建 text layout、重設視窗尺寸、InvalidateRect，等於每格都讓
    // 這顆標籤閃一下。w_ > 0 代表已經真的算過尺寸（第一次呼叫要放行）。
    if (text == text_ && w_ > 0) return;
    text_ = text;

    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    const int padX = (int)std::lround(10.0f * scale);
    const int padY = (int)std::lround(5.0f * scale);

    float textW = 40.0f * scale;
    float textH = sizeDip_ * scale * 1.6f;
    if (!text_.empty()) {
        // 先量「單行不限寬」的自然尺寸，再依實際行數決定高度。
        //
        // 這裡踩過一次坑：原本高度寫死成 sizeDip * 1.6（單行），於是多行的
        // OCR 結果（3 行）被塞進 34px 高的視窗，畫是畫了、但只有第一行露出來，
        // 看起來像「OCR 沒結果」。高度必須來自文字排版量到的行高。
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(g_dw->CreateTextLayout(text_.c_str(), (UINT32)text_.size(), fmt_,
                                             4096.0f, 4096.0f, &layout)) &&
            layout) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(layout->GetMetrics(&tm))) {
                textW = tm.width;
                if (tm.height > 0.0f) textH = tm.height;
            }
            layout->Release();
        }
    }
    const int w = (int)std::ceil(textW) + padX * 2;
    const int h = (int)std::ceil(textH) + padY * 2;

    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, w_, h_,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
        HRGN rgn = CreateRoundRectRgn(0, 0, w_ + 1, h_ + 1,
                                      (int)(radiusDip_ * scale * 2),
                                      (int)(radiusDip_ * scale * 2));
        SetWindowRgn(hwnd_, rgn, TRUE);
        if (rt_) { rt_->Release(); rt_ = nullptr; }
        if (bgBrush_) { bgBrush_->Release(); bgBrush_ = nullptr; }
        if (fgBrush_) { fgBrush_->Release(); fgBrush_ = nullptr; }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TextBadge::MoveTo(int x, int y) {
    if (!hwnd_) return;
    // 同一個位置就不用再動 —— SetWindowPos 即使座標沒變也會讓視窗走一次
    // 位置更新流程，拖曳時每格都呼叫就是白白製造閃爍。
    if (x == x_ && y == y_) return;
    x_ = x;
    y_ = y;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

void TextBadge::Show(bool on) {
    if (!hwnd_) return;
    if (visible_ == on) return;    // 狀態沒變就不必再 ShowWindow 一次
    ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = on;
}

void TextBadge::Destroy() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (bgBrush_) { bgBrush_->Release(); bgBrush_ = nullptr; }
    if (fgBrush_) { fgBrush_->Release(); fgBrush_ = nullptr; }
    if (fmt_) { fmt_->Release(); fmt_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void TextBadge::Paint() {
    if (!hwnd_) return;
    if (!rt_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;
        // DPI 固定 96：下面所有座標都用像素算，避免 DIP 換算來回掉 1px。
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        const HRESULT hrRt = g_d2d->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
            &rt_);
        if (FAILED(hrRt)) {
            LogHr("CreateHwndRenderTarget", hrRt);
            return;
        }
        LogHr("CreateSolidColorBrush(bg)", rt_->CreateSolidColorBrush(bg_, &bgBrush_));
        LogHr("CreateSolidColorBrush(fg)", rt_->CreateSolidColorBrush(fg_, &fgBrush_));
    }

    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    rt_->BeginDraw();
    rt_->Clear(bg_);
    if (bgBrush_) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(0, 0, (float)w_, (float)h_), radiusDip_, radiusDip_);
        rt_->FillRoundedRectangle(rr, bgBrush_);
    }
    if (fgBrush_ && fmt_ && !text_.empty()) {
        // 內縮 padding 再畫：SetText 量出來的 w_/h_ 是「文字 + 兩側 padding」，
        // 直接用 0,0,w_,h_ 畫會讓字貼著圓角邊緣（多行時尤其明顯）。
        const float scale = GetDpiForWindow(hwnd_) / 96.0f;
        const float padX = 10.0f * scale;
        const float padY = 5.0f * scale;
        rt_->DrawText(text_.c_str(), (UINT32)text_.size(), fmt_,
                      D2D1::RectF(padX, padY, (float)w_ - padX, (float)h_ - padY),
                      fgBrush_);
    }
    const HRESULT hr = rt_->EndDraw();
    EndPaint(hwnd_, &ps);
    LogHr("EndDraw", hr);
    if (hr == D2DERR_RECREATE_TARGET) {
        if (bgBrush_) { bgBrush_->Release(); bgBrush_ = nullptr; }
        if (fgBrush_) { fgBrush_->Release(); fgBrush_ = nullptr; }
        if (rt_) { rt_->Release(); rt_ = nullptr; }
    }
}

LRESULT CALLBACK TextBadge::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<TextBadge*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (self) self->Paint();
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================================
// 頂端工具列（截圖 / OCR）
// ============================================================================

namespace {

// 全部是 96 DPI 下的 DIP，實際尺寸再乘 scale_。
constexpr int kSegHDip = 40;      // 分段高度
constexpr int kPadDip = 5;        // 膠囊內緣留白
constexpr int kIconDip = 20;      // 圖示框的大小
constexpr int kGapIconTextDip = 8;
constexpr int kPadXTextDip = 14;  // 文字左右留白
constexpr int kRadiusDip = 8;     // 膠囊圓角（不是半圓，微軟那顆是圓角矩形）
constexpr int kMarginTopDip = 12; // 距工作區頂端

const D2D1_COLOR_F kTbBg = D2D1::ColorF(0.11f, 0.11f, 0.13f, 1.0f);
const D2D1_COLOR_F kTbSep = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
const D2D1_COLOR_F kTbFg = D2D1::ColorF(0.78f, 0.79f, 0.82f, 1.0f);      // 未選
const D2D1_COLOR_F kTbFgOn = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);       // 選中
const D2D1_COLOR_F kTbFgOff = D2D1::ColorF(0.42f, 0.43f, 0.46f, 1.0f);   // 停用
const D2D1_COLOR_F kTbActive = D2D1::ColorF(0.24f, 0.47f, 0.86f, 0.95f); // 選中底
const D2D1_COLOR_F kTbHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);

// 截圖圖示：四角的裁切框（viewfinder）。用角標而不是整圈外框 —— 整圈外框
// 會跟下面 OCR 的文件框撞形，兩個分段在縮圖尺寸下就分不出來了。
void DrawSnipIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                  ID2D1StrokeStyle* round, D2D1_POINT_2F c, float s) {
    const float r = 7.0f * s;      // 半寬 / 半高
    const float arm = 4.5f * s;    // 每條角標的長度
    const float lw = 1.8f * s;
    const D2D1_POINT_2F tl = D2D1::Point2F(c.x - r, c.y - r);
    const D2D1_POINT_2F tr = D2D1::Point2F(c.x + r, c.y - r);
    const D2D1_POINT_2F bl = D2D1::Point2F(c.x - r, c.y + r);
    const D2D1_POINT_2F br = D2D1::Point2F(c.x + r, c.y + r);
    // 左上
    rt->DrawLine(tl, D2D1::Point2F(tl.x + arm, tl.y), brush, lw, round);
    rt->DrawLine(tl, D2D1::Point2F(tl.x, tl.y + arm), brush, lw, round);
    // 右上
    rt->DrawLine(tr, D2D1::Point2F(tr.x - arm, tr.y), brush, lw, round);
    rt->DrawLine(tr, D2D1::Point2F(tr.x, tr.y + arm), brush, lw, round);
    // 左下
    rt->DrawLine(bl, D2D1::Point2F(bl.x + arm, bl.y), brush, lw, round);
    rt->DrawLine(bl, D2D1::Point2F(bl.x, bl.y - arm), brush, lw, round);
    // 右下
    rt->DrawLine(br, D2D1::Point2F(br.x - arm, br.y), brush, lw, round);
    rt->DrawLine(br, D2D1::Point2F(br.x, br.y - arm), brush, lw, round);
}

// OCR 圖示：三條粗細不一的橫線（文字列）+ 左側一條直立裝訂線，讀起來像
// 「一段被掃下來的文字」。刻意不用字母「T」—— 中文字型下那個字形很吃字重，
// 20px 高會糊成一團。
void DrawOcrIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                 ID2D1StrokeStyle* round, D2D1_POINT_2F c, float s) {
    const float lw = 1.8f * s;
    const float x0 = c.x - 7.0f * s;
    const float x1 = c.x + 7.0f * s;
    // 左邊一條直線，像掃描出來的欄位邊界
    rt->DrawLine(D2D1::Point2F(x0 - 2.6f * s, c.y - 6.0f * s),
                 D2D1::Point2F(x0 - 2.6f * s, c.y + 6.0f * s), brush, lw, round);
    rt->DrawLine(D2D1::Point2F(x0, c.y - 5.0f * s), D2D1::Point2F(x1, c.y - 5.0f * s),
                 brush, lw, round);
    rt->DrawLine(D2D1::Point2F(x0, c.y), D2D1::Point2F(x0 + 9.0f * s, c.y), brush, lw,
                 round);
    rt->DrawLine(D2D1::Point2F(x0, c.y + 5.0f * s), D2D1::Point2F(x1 - 2.0f * s, c.y + 5.0f * s),
                 brush, lw, round);
}

} // namespace

Toolbar::~Toolbar() { Destroy(); }

bool Toolbar::Create(HWND owner, std::function<void(Action)> onPick) {
    onPick_ = std::move(onPick);
    if (hwnd_) return true;
    if (!EnsureFactories()) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    wc.lpszClassName = L"CapWGCToolbar";
    RegisterClassExW(&wc);

    // WS_EX_NOACTIVATE：點工具列不搶焦點（搶了的話框選的 owner 會失焦）。
    // WS_EX_LAYERED：per-pixel alpha，見 Paint()。
    // WS_EX_TOPMOST：框選的四塊遮罩也是 topmost。工具列必須先站進 topmost
    // 這一層，遮罩顯示時才能以它為錨點插在「它下面」（見 DimPanel::SetRect
    // 的 below 參數）—— 否則遮罩會蓋住工具列，症狀是下半部變暗、看得到但
    // 不對。
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED,
                            L"CapWGCToolbar", L"", WS_POPUP, 0, 0, 0, 0,
                            owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    // 刻意**不**呼叫 SetWindowDisplayAffinity：這顆是使用者自己要看的 UI，
    // 不是錄影指示。截圖時它就該出現在畫面裡（微軟的工具列也是這樣）。
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    scale_ = scale;
    if (FAILED(g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                      DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      13.0f * scale, L"zh-TW", &fmt_))) {
        // 沒有 YaHei 的系統（罕見）退回 DWrite 的預設字型，不要讓整顆開不起來。
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               13.0f * scale, L"zh-TW", &fmt_);
    }
    return true;
}

void Toolbar::Show(const RECT& workArea, Action active) {
    if (!hwnd_) return;
    active_ = (active == Action::Ocr) ? Action::Ocr : Action::Snip;
    Layout(workArea);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    SetHover(Action::None);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Toolbar::SetActive(Action active) {
    if (active == Action::None || active_ == active) return;
    active_ = active;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Toolbar::SetActionEnabled(Action action, bool enabled) {
    if (action == Action::Snip) snipEnabled_ = enabled;
    else if (action == Action::Ocr) ocrEnabled_ = enabled;
    else return;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Toolbar::Hide() {
    if (!hwnd_) return;
    visible_ = false;
    pressed_ = Action::None;
    SetHover(Action::None);
    ShowWindow(hwnd_, SW_HIDE);
}

void Toolbar::Destroy() {
    ReleaseTarget();
    ReleaseSurface();
    if (fmt_) { fmt_->Release(); fmt_ = nullptr; }
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void Toolbar::ReleaseTarget() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (bg_) { bg_->Release(); bg_ = nullptr; }
    if (sep_) { sep_->Release(); sep_ = nullptr; }
    if (fg_) { fg_->Release(); fg_ = nullptr; }
    if (fgOn_) { fgOn_->Release(); fgOn_ = nullptr; }
    if (fgOff_) { fgOff_->Release(); fgOff_ = nullptr; }
    if (actBg_) { actBg_->Release(); actBg_ = nullptr; }
    if (hoverBg_) { hoverBg_->Release(); hoverBg_ = nullptr; }
    if (round_) { round_->Release(); round_ = nullptr; }
}

void Toolbar::ReleaseSurface() {
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

bool Toolbar::EnsureSurface(int w, int h) {
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
    bi.bmiHeader.biHeight = -h;   // top-down
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

unsigned char Toolbar::AlphaAt(int x, int y) const {
    if (!bits_ || x < 0 || y < 0 || x >= dibW_ || y >= dibH_) return 0;
    const auto* px = reinterpret_cast<const uint32_t*>(bits_);
    return (unsigned char)(px[(size_t)y * dibW_ + x] >> 24);
}

void Toolbar::Layout(const RECT& workArea) {
    const float s = scale_;
    const int segH = (int)std::lround(kSegHDip * s);
    const int pad = (int)std::lround(kPadDip * s);
    const int icon = (int)std::lround(kIconDip * s);
    const int gapIT = (int)std::lround(kGapIconTextDip * s);
    const int padX = (int)std::lround(kPadXTextDip * s);

    // 兩段的寬度 = 圖示 + 間距 + 文字。文字寬度用 DWrite 量，避免「截圖」與
    // 「OCR」兩個標籤不一樣長時，分段寬度寫死會有一邊特別擠。
    auto textW = [&](const wchar_t* t) {
        float w = 40.0f * s;
        if (g_dw && fmt_) {
            IDWriteTextLayout* lay = nullptr;
            if (SUCCEEDED(g_dw->CreateTextLayout(t, (UINT32)wcslen(t), fmt_, 4096.0f,
                                                 4096.0f, &lay)) &&
                lay) {
                DWRITE_TEXT_METRICS tm{};
                if (SUCCEEDED(lay->GetMetrics(&tm))) w = tm.width;
                lay->Release();
            }
        }
        return (int)std::ceil(w);
    };
    const int wSnip = padX + icon + gapIT + textW(L"截圖") + padX;
    const int wOcr = padX + icon + gapIT + textW(L"OCR") + padX;

    w_ = pad * 2 + wSnip + wOcr;
    h_ = pad * 2 + segH;

    segSnip_ = RECT{pad, pad, pad + wSnip, pad + segH};
    segOcr_ = RECT{pad + wSnip, pad, pad + wSnip + wOcr, pad + segH};

    // 頂端中央。左邊對齊工作區中線再減去自己一半寬 —— 用 workArea 而不是
    // 整顆螢幕，雙螢幕或工作列在上面時才不會壓到工作列。
    const int marginTop = (int)std::lround(kMarginTopDip * s);
    int x = workArea.left + ((workArea.right - workArea.left) - w_) / 2;
    int y = workArea.top + marginTop;
    if (x < workArea.left) x = workArea.left;
    if (x + w_ > workArea.right) x = workArea.right - w_;
    if (y + h_ > workArea.bottom) y = workArea.bottom - h_;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE);
}

Toolbar::Action Toolbar::HitTest(int x, int y) const {
    const POINT p{x, y};
    if (snipEnabled_ && PtInRect(&segSnip_, p)) return Action::Snip;
    if (ocrEnabled_ && PtInRect(&segOcr_, p)) return Action::Ocr;
    return Action::None;
}

void Toolbar::SetHover(Action a) {
    if (hoverSeg_ == a) return;
    hoverSeg_ = a;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Toolbar::Paint() {
    if (!hwnd_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    if (!EnsureSurface(rc.right, rc.bottom)) return;

    if (!rt_) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(g_d2d->CreateDCRenderTarget(&props, &rt_))) return;
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        rt_->CreateSolidColorBrush(kTbBg, &bg_);
        rt_->CreateSolidColorBrush(kTbSep, &sep_);
        rt_->CreateSolidColorBrush(kTbFg, &fg_);
        rt_->CreateSolidColorBrush(kTbFgOn, &fgOn_);
        rt_->CreateSolidColorBrush(kTbFgOff, &fgOff_);
        rt_->CreateSolidColorBrush(kTbActive, &actBg_);
        rt_->CreateSolidColorBrush(kTbHover, &hoverBg_);

        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.startCap = D2D1_CAP_STYLE_ROUND;
        sp.endCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        g_d2d->CreateStrokeStyle(sp, nullptr, 0, &round_);
    }

    const RECT bindRc{0, 0, rc.right, rc.bottom};
    if (FAILED(rt_->BindDC(memDC_, &bindRc))) return;

    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float s = scale_;

    // 膠囊本體
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, (float)w_ - 0.5f, (float)h_ - 0.5f),
        (float)std::lround(kRadiusDip * s), (float)std::lround(kRadiusDip * s));
    if (bg_) rt_->FillRoundedRectangle(pill, bg_);
    if (sep_) rt_->DrawRoundedRectangle(pill, sep_, 1.0f);

    // 各分段的底色：選中 = 藍底；懸停 = 淡白
    auto segRect = [&](Action a) -> const RECT& {
        return (a == Action::Ocr) ? segOcr_ : segSnip_;
    };
    auto segEnabled = [&](Action a) {
        return (a == Action::Snip) ? snipEnabled_ : ocrEnabled_;
    };
    if (active_ != Action::None && actBg_ && segEnabled(active_)) {
        const RECT& r = segRect(active_);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom),
            4.0f * s, 4.0f * s);
        rt_->FillRoundedRectangle(rr, actBg_);
    }

    if (hoverSeg_ != Action::None && hoverBg_ && hoverSeg_ != active_) {
        const RECT& r = segRect(hoverSeg_);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom),
            4.0f * s, 4.0f * s);
        rt_->FillRoundedRectangle(rr, hoverBg_);
    }

    // 兩段之間的分隔線
    if (sep_) {
        const float dx = (float)segOcr_.left;
        const float dy0 = (float)(segOcr_.top + 8 * s);
        const float dy1 = (float)(segOcr_.bottom - 8 * s);
        rt_->DrawLine(D2D1::Point2F(dx, dy0), D2D1::Point2F(dx, dy1), sep_, 1.0f);
    }

    // 各段的圖示與文字
    auto drawSeg = [&](Action a, const wchar_t* label) {
        const RECT& r = segRect(a);
        const bool en = segEnabled(a);
        ID2D1SolidColorBrush* brush = !en ? fgOff_ : ((a == active_) ? fgOn_ : fg_);
        if (!brush) return;

        const int padX = (int)std::lround(kPadXTextDip * s);
        const int icon = (int)std::lround(kIconDip * s);
        const int gapIT = (int)std::lround(kGapIconTextDip * s);
        const float cy = (r.top + r.bottom) / 2.0f;
        const D2D1_POINT_2F ic = D2D1::Point2F((float)(r.left + padX + icon / 2), cy);
        if (a == Action::Snip) DrawSnipIcon(rt_, brush, round_, ic, s);
        else DrawOcrIcon(rt_, brush, round_, ic, s);

        if (fmt_) {
            const float tx = (float)(r.left + padX + icon + gapIT);
            D2D1_RECT_F tr = D2D1::RectF(tx, (float)r.top, (float)r.right - 2.0f, (float)r.bottom);
            rt_->DrawText(label, (UINT32)wcslen(label), fmt_, tr, brush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };
    drawSeg(Action::Snip, L"截圖");
    drawSeg(Action::Ocr, L"OCR");

    const HRESULT hr = rt_->EndDraw();
    EndPaint(hwnd_, &ps);
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        return;
    }

    RECT wr{};
    if (!GetWindowRect(hwnd_, &wr)) return;
    HDC screen = GetDC(nullptr);
    if (screen) {
        POINT dst{wr.left, wr.top};
        SIZE size{rc.right, rc.bottom};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }
}

LRESULT CALLBACK Toolbar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Toolbar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self && m == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<Toolbar*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }

    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (self) self->Paint();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        if (!self) break;
        // 圓角外面那幾塊 alpha = 0，要讓點擊穿過去，不然使用者會在「看起來是
        // 桌面」的地方點到一塊看不見的視窗，把框選的第一下吃掉。
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (self->AlphaAt(p.x, p.y) < 8) return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && self) {
            POINT pt{};
            GetCursorPos(&pt);
            POINT cp = pt;
            ScreenToClient(h, &cp);
            const Action a = self->HitTest(cp.x, cp.y);
            SetCursor(LoadCursorW(nullptr, (LPCWSTR)(a == Action::None ? IDC_ARROW : IDC_HAND)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!self) return 0;
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
    case WM_MOUSELEAVE:
        if (self) {
            self->tracking_ = false;
            self->SetHover(Action::None);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        self->pressed_ = self->HitTest(pt.x, pt.y);
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self) return 0;
        ReleaseCapture();
        const Action pressed = self->pressed_;
        self->pressed_ = Action::None;
        if (pressed == Action::None) return 0;
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (self->HitTest(pt.x, pt.y) != pressed) return 0;
        auto fn = self->onPick_;
        if (fn) fn(pressed);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================================
// 自己的提示（取代 Shell 氣球）
// ============================================================================

namespace {

constexpr int kToastMaxWDip = 380;     // 超過就折行，不要長成一條橫幅
constexpr int kToastPadXDip = 14;
constexpr int kToastPadYDip = 11;
constexpr int kToastGapDotDip = 10;    // 色點與文字之間
constexpr int kToastDotRDip = 4;
constexpr int kToastGapTitleBodyDip = 3;
constexpr int kToastRadiusDip = 9;
constexpr int kToastMarginDip = 16;    // 距工作區右下角

const D2D1_COLOR_F kToastBg = D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.97f);
const D2D1_COLOR_F kToastBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f);
const D2D1_COLOR_F kToastTitle = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
const D2D1_COLOR_F kToastBody = D2D1::ColorF(0.76f, 0.78f, 0.82f, 1.0f);

// 色點就是「這則是什麼」。誤用顏色比誤用圖示容易（一眼掃過去只看顏色）。
D2D1_COLOR_F AccentFor(Toast::Kind k) {
    switch (k) {
    case Toast::Kind::Warn:  return D2D1::ColorF(0.98f, 0.71f, 0.20f, 1.0f);
    case Toast::Kind::Error: return D2D1::ColorF(0.94f, 0.35f, 0.32f, 1.0f);
    default:                 return D2D1::ColorF(0.30f, 0.68f, 0.98f, 1.0f);
    }
}

} // namespace

Toast::~Toast() { Destroy(); }

bool Toast::Create() {
    if (hwnd_) return true;
    if (!EnsureFactories()) return false;

    RegisterOnce(L"CapWGCToast", WndProc, nullptr);

    // 跟 Toolbar 同一套：topmost、不搶焦點、不進工作列、layered 做 alpha。
    // WS_EX_TRANSPARENT 不設 —— 這顆要能吃掉點擊（點一下立刻關掉），
    // 不然它會蓋住底下的東西又不能點，比 Shell 的氣球更煩。
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED,
                            L"CapWGCToast", L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // 刻意**不**做 display exclusion：提示是使用者要看的一則訊息，截圖時
    // 它就該出現在畫面裡（跟 Shell 的氣球一樣）。真正不該入鏡的只有錄影中
    // 的漂浮控件，那顆在 controlbar.cpp 自己處理。
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    scale_ = scale;

    g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           13.0f * scale, L"zh-TW", &fmtTitle_);
    if (!fmtTitle_) {
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               13.0f * scale, L"zh-TW", &fmtTitle_);
    }
    g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           12.0f * scale, L"zh-TW", &fmtBody_);
    if (!fmtBody_) {
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               12.0f * scale, L"zh-TW", &fmtBody_);
    }
    if (!fmtTitle_) return false;
    if (fmtBody_) {
        // body 要折行（「已存檔：CapWGC_20261020_113000.png」這種長字串）；
        // title 不折行，它是短標籤。
        fmtBody_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fmtBody_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    fmtTitle_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmtTitle_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return true;
}

void Toast::SetBottomInset(int dip) {
    if (bottomInset_ == dip) return;
    bottomInset_ = dip;
    if (visible_) Relayout();
}

void Toast::Show(const std::wstring& title, const std::wstring& body, Kind kind,
                 int afterMs, Action onClick) {
    if (!Create()) return;
    title_ = title;
    body_ = body;
    kind_ = kind;
    onClick_ = std::move(onClick);
    // 可點擊的提示要多留一點時間：使用者得先看到「點一下開啟」才來得及點，
    // 而且那種訊息常常出現在使用者還在做別的事的時候。
    if (afterMs > 0 && onClick_) afterMs += 2500;

    Relayout();

    // 先畫好再顯示：UpdateLayeredWindow 是在 Paint() 裡做的，若先 ShowWindow
    // 再 InvalidateRect，會有一帧「空視窗」閃過去。
    Paint();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;

    // 單一 timer 編號：連續 Show() 時 SetTimer 用同一個 id 就是「重設」，
    // 不會累積一堆 timer 一起到期把新的一則提早關掉。
    if (afterMs > 0) SetTimer(hwnd_, 1, (UINT)afterMs, nullptr);
    else KillTimer(hwnd_, 1);
}

void Toast::Hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, 1);
    // 回呼跟著這一則一起作廢。不清的話它會活到下一則 Show() 為止，而中間
    // 任何一次「殘留視窗被點到」都會執行上一則的動作。
    onClick_ = nullptr;
    if (!visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

void Toast::Dismiss(bool leftClick) {
    // 先抄一份再 Hide()：Hide() 會把 onClick_ 清掉，而且動作本身可能又要
    // Show() 一則新的提示（例如開啟檔案失敗）。
    Action act = leftClick ? onClick_ : Action{};
    Hide();
    if (act) act();
}

void Toast::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        visible_ = false;
        DestroyWindow(h);
    }
    ReleaseTarget();
    ReleaseSurface();
    if (fmtTitle_) { fmtTitle_->Release(); fmtTitle_ = nullptr; }
    if (fmtBody_) { fmtBody_->Release(); fmtBody_ = nullptr; }
}

void Toast::ReleaseTarget() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (bg_) { bg_->Release(); bg_ = nullptr; }
    if (border_) { border_->Release(); border_ = nullptr; }
    if (fgTitle_) { fgTitle_->Release(); fgTitle_ = nullptr; }
    if (fgBody_) { fgBody_->Release(); fgBody_ = nullptr; }
    if (accent_) { accent_->Release(); accent_ = nullptr; }
}

void Toast::ReleaseSurface() {
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

bool Toast::EnsureSurface(int w, int h) {
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
    bi.bmiHeader.biHeight = -h;   // top-down
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

// 量出「這則提示要多大」，順便把每一塊的座標算好。Show() 會先呼叫它再 Paint()。
void Toast::Relayout() {
    if (!hwnd_ || !fmtTitle_) return;
    const float s = scale_;

    padX_ = (int)std::lround(kToastPadXDip * s);
    padY_ = (int)std::lround(kToastPadYDip * s);
    const int dotR = (int)std::lround(kToastDotRDip * s);
    const int gapDot = (int)std::lround(kToastGapDotDip * s);
    const int maxW = (int)std::lround(kToastMaxWDip * s);
    const int gapTB = (int)std::lround(kToastGapTitleBodyDip * s);

    textLeft_ = padX_ + dotR * 2 + gapDot;
    const int textMaxW = std::max(40, maxW - textLeft_ - padX_);

    // 標題與內容都用 DWrite 量，量不到就退回固定高度 —— 寧可高度差一點，
    // 也不要整個提示不顯示。
    float titleW = (float)textMaxW;
    float titleH = 13.0f * s * 1.4f;
    if (!title_.empty()) {
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(g_dw->CreateTextLayout(title_.c_str(), (UINT32)title_.size(),
                                             fmtTitle_, (float)textMaxW, 4096.0f, &lay)) &&
            lay) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(lay->GetMetrics(&tm))) {
                titleW = tm.width;
                if (tm.height > 0.0f) titleH = tm.height;
            }
            lay->Release();
        }
    }

    float bodyW = 0.0f;
    float bodyH = 0.0f;
    if (!body_.empty() && fmtBody_) {
        // 折行量測：寬度給 textMaxW，高度上限給大值，讓 GetMetrics 回報
        // 「折行後實際需要多高」。這裡踩過 TextBadge 那個坑 —— 高度寫死單行
        // 的話多行內容只會露出第一行。
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(g_dw->CreateTextLayout(body_.c_str(), (UINT32)body_.size(),
                                             fmtBody_, (float)textMaxW, 4096.0f, &lay)) &&
            lay) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(lay->GetMetrics(&tm))) {
                bodyW = tm.width;
                if (tm.height > 0.0f) bodyH = tm.height;
            }
            lay->Release();
        }
    }

    const float textW = std::max(titleW, bodyW);
    w_ = (int)std::ceil(textLeft_ + textW + padX_);

    titleTop_ = (float)padY_;
    titleH_ = title_.empty() ? 0.0f : titleH;
    bodyTop_ = title_.empty() ? (float)padY_ : (titleTop_ + titleH_ + (float)gapTB);
    bodyH_ = bodyH;
    h_ = (int)std::ceil(bodyTop_ + bodyH_ + padY_);

    // 色點垂直對齊第一行文字的中線（不是整塊的中線）—— 有多行內容時，
    // 對齊中線會讓點飄到中間，看起來跟標題無關。
    dotR_ = (float)dotR;
    dotCx_ = (float)(padX_ + dotR);
    const float firstLineH = titleH_ > 0.0f ? titleH_ : (bodyH_ > 0.0f ? bodyH_ : 0.0f);
    const float firstTop = titleH_ > 0.0f ? titleTop_ : bodyTop_;
    dotCy_ = firstTop + std::max((float)dotR, firstLineH * 0.5f);

    // 右下角。用工作區（不是整顆螢幕）：工作列在下面或右邊時才不會被蓋住。
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int margin = (int)std::lround(kToastMarginDip * s);
    const int inset = (int)std::lround((float)bottomInset_ * s);
    int x = (int)work.right - margin - w_;
    int y = (int)work.bottom - margin - h_ - inset;
    // 夾回工作區：螢幕很小的時候（或 bottomInset 讓開太多）至少還留在畫面上。
    x = std::max(x, (int)work.left + margin);
    y = std::max(y, (int)work.top + margin);

    // 尺寸變了才帶寬高；位置一定要帶（這是每一則都可能不同）。
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE);
}

void Toast::Paint() {
    if (!hwnd_ || w_ <= 0 || h_ <= 0) return;
    if (!EnsureSurface(w_, h_)) return;

    if (!rt_) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(g_d2d->CreateDCRenderTarget(&props, &rt_))) {
            LogHr("CreateDCRenderTarget(toast)", E_FAIL);
            return;
        }
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(kToastBg, &bg_);
        rt_->CreateSolidColorBrush(kToastBorder, &border_);
        rt_->CreateSolidColorBrush(kToastTitle, &fgTitle_);
        rt_->CreateSolidColorBrush(kToastBody, &fgBody_);
        const D2D1_COLOR_F acc = AccentFor(kind_);
        rt_->CreateSolidColorBrush(acc, &accent_);
    }
    // 色點顏色隨 kind 變，但 brush 是建一次重複用的 —— 每次 Show 重設顏色。
    if (accent_) accent_->SetColor(AccentFor(kind_));

    const RECT bindRc{0, 0, w_, h_};
    if (FAILED(rt_->BindDC(memDC_, &bindRc))) return;

    rt_->BeginDraw();
    // 整張清成「全透明」：UpdateLayeredWindow 用 AC_SRC_ALPHA，視窗外面那幾
    // 個像素必須真的 alpha=0，不然圓角會露出一圈黑邊。
    rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float s = scale_;
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, (float)w_ - 0.5f, (float)h_ - 0.5f),
        (float)std::lround(kToastRadiusDip * s), (float)std::lround(kToastRadiusDip * s));
    if (bg_) rt_->FillRoundedRectangle(pill, bg_);
    if (border_) rt_->DrawRoundedRectangle(pill, border_, 1.0f);

    if (accent_) {
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx_, dotCy_), dotR_, dotR_),
                         accent_);
    }

    const float tr = (float)(w_ - padX_);
    if (!title_.empty() && fgTitle_ && fmtTitle_) {
        rt_->DrawText(title_.c_str(), (UINT32)title_.size(), fmtTitle_,
                      D2D1::RectF((float)textLeft_, titleTop_, tr, titleTop_ + titleH_),
                      fgTitle_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (!body_.empty() && fgBody_ && fmtBody_) {
        rt_->DrawText(body_.c_str(), (UINT32)body_.size(), fmtBody_,
                      D2D1::RectF((float)textLeft_, bodyTop_, tr, bodyTop_ + bodyH_),
                      fgBody_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        return;
    }

    RECT wr{};
    if (!GetWindowRect(hwnd_, &wr)) return;
    HDC screen = GetDC(nullptr);
    if (screen) {
        POINT dst{wr.left, wr.top};
        SIZE size{w_, h_};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }
}

LRESULT CALLBACK Toast::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Toast*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self && m == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<Toast*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }

    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        // UpdateLayeredWindow 已經把內容推上去了；DefWindowProc 的 BeginPaint
        // 還是要有一對，否則 WM_PAINT 會一直重送變成空轉。
        {
            PAINTSTRUCT ps{};
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
        }
        return 0;
    case WM_TIMER:
        // 自動消失。使用者點一下也走同一條（見下）。
        if (w == 1 && self) self->Hide();
        return 0;
    case WM_LBUTTONDOWN:
        // 左鍵是「照它說的做」。標題寫「點一下開啟」時，這裡就是那個開啟 ——
        // 以前是 Shell 氣球回的 NIN_BALLOONUSERCLICK，改用自家視窗之後那則
        // 通知不會再來，所以動作得自己在這裡接。
        if (self) self->Dismiss(/*leftClick=*/true);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        // 右鍵／中鍵只關掉，不執行。可點擊的提示旁邊常常就是別的東西，誤觸
        // 不該變成「用預設程式開了一個檔」。
        if (self) self->Dismiss(/*leftClick=*/false);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        // 圓角外、以及視窗本身都算「點到了」——這是提示不是操作區，
        // 點哪裡都關掉最直覺。
        return HTCLIENT;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================================
// 幾何 / 命中測試
// ============================================================================

RECT Inflate(const RECT& r, int dx, int dy) {
    RECT o{r.left - dx, r.top - dy, r.right + dx, r.bottom + dy};
    return o;
}

RECT Normalize(const POINT& a, const POINT& b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

namespace {
struct FindCtx {
    POINT pt;
    HWND self;
    HWND found;
};

BOOL CALLBACK FindProc(HWND h, LPARAM p) {
    auto* c = reinterpret_cast<FindCtx*>(p);
    if (h == c->self) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;

    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    // 自己的覆蓋層全部帶 toolwindow，這裡一次濾掉（連同其他程式的浮層）
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (ex & WS_EX_TRANSPARENT) return TRUE;

    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    if (!PtInRect(&r, c->pt)) return TRUE;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) {
        return TRUE;
    }
    c->found = h;
    return FALSE;   // EnumWindows 是 z-order 由前到後，第一個命中就是最上層
}
} // namespace

HWND WindowUnderPoint(POINT pt) {
    FindCtx c{pt, nullptr, nullptr};
    EnumWindows(FindProc, (LPARAM)&c);
    if (!c.found) return nullptr;
    return GetAncestor(c.found, GA_ROOT);
}

} // namespace ui
