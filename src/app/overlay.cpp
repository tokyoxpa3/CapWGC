// ============================================================================
// overlay.cpp — 區域框選覆蓋層的實作
// ============================================================================

#include "overlay.h"

#include <windowsx.h>

#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace app {
namespace {

constexpr UINT WM_OVERLAY_DONE = WM_APP + 10;
constexpr UINT WM_OVERLAY_SNIP = WM_APP + 11;
constexpr int kThickness = 2;
constexpr COLORREF kAccent = RGB(0, 174, 255);       // 選取中：亮藍
constexpr COLORREF kAccentWin = RGB(120, 200, 90);   // 懸停視窗：綠
constexpr int kMinSize = 4;                          // 小於此視窗算「單擊」

} // namespace

Overlay::~Overlay() { DestroyWindows(); }

bool Overlay::Begin(DoneFn fn) {
    if (owner_) return false;
    snip_ = false;
    snipDone_ = nullptr;
    done_ = std::move(fn);
    return BeginCommon();
}

bool Overlay::BeginSnip(ui::Toolbar::Action defaultAction, SnipDoneFn fn) {
    if (owner_) return false;
    snip_ = true;
    done_ = nullptr;
    snipDone_ = std::move(fn);
    defaultAction_ = (defaultAction == ui::Toolbar::Action::Ocr)
                         ? ui::Toolbar::Action::Ocr
                         : ui::Toolbar::Action::Snip;
    return BeginCommon();
}

bool Overlay::BeginCommon() {
    virtual_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtual_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtual_.right = virtual_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtual_.bottom = virtual_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"CapWGCSelect";
    RegisterClassExW(&wc);

    // 蓋住整個虛擬桌面的「隱形」owner：alpha 只給 1（完全 0 會變成滑鼠穿透，
    // 就收不到拖曳了），肉眼看不到，但吃得到滑鼠與鍵盤。
    owner_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                             L"CapWGCSelect", L"", WS_POPUP,
                             virtual_.left, virtual_.top,
                             virtual_.right - virtual_.left,
                             virtual_.bottom - virtual_.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!owner_) {
        done_ = nullptr;
        snipDone_ = nullptr;
        return false;
    }
    SetLayeredWindowAttributes(owner_, 0, 1, LWA_ALPHA);
    SetWindowLongPtrW(owner_, GWLP_USERDATA, (LONG_PTR)this);

    OnCreate();
    ShowWindow(owner_, SW_SHOW);
    SetForegroundWindow(owner_);
    SetFocus(owner_);
    // 不在這裡 SetCapture：按下之前就抓滑鼠，會把使用者的第一次點擊吃掉
    // （實測症狀是「框選一打開就自己結束，而且選到一個沒碰過的視窗」）。
    UpdateVisuals();
    return true;
}

void Overlay::OnCreate() {
    for (auto& d : dim_) d.Create(110);
    border_.Create(kAccent, kThickness);
    labelReady_ = label_.Create(L"Consolas", 14.0f,
                                D2D1::ColorF(0.08f, 0.09f, 0.10f, 1.0f),
                                D2D1::ColorF(1, 1, 1, 1));

    // 工具列。owner 設成 owner_：owned window 永遠在 owner 之上，不必跟
    // 一堆 topmost 視窗搶 z-order。工具列點下去不搶焦點（WS_EX_NOACTIVATE
    // 建在 Toolbar 內部），所以框選的鍵盤焦點還在。
    if (snip_) {
        // 工作區取「虛擬桌面所在主要螢幕」的 —— 工具列要貼在頂端中央，
        // 用主螢幕才符合直覺（多螢幕時最左邊那顆不一定放得下）。
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) workArea_ = mi.rcWork;
        else workArea_ = virtual_;

        toolbarReady_ = toolbar_.Create(owner_, [this](ui::Toolbar::Action a) {
            // 點工具列 = 切換「要用哪個動作」，不是立刻執行。
            //
            // 這裡修過一次設計錯誤：原本點工具列會直接用「當下有效的範圍」
            // 完成，而滑鼠停在工具列上時，底下懸停的「視窗」其實是整個桌面
            // —— 於是「點一下 OCR」就等於「對整張螢幕做 OCR」，使用者根本
            // 還沒框選就噴結果（實測就是這樣炸掉的）。
            //
            // 正確順序（微軟 Win+Shift+S 也是這樣）：先選模式 → 再框選 →
            // 放開才執行。所以這裡只改 defaultAction_ 與工具列高亮，真正
            // 送出的時機在「滑鼠放開 / 單擊選視窗 / Enter / 雙擊」那幾條路。
            if (a == ui::Toolbar::Action::None) return;
            defaultAction_ = a;
            toolbar_.SetActive(a);
        });
        if (toolbarReady_) {
            // OCR 沒引擎時畫成灰的，而不是點下去才失敗。引擎就緒狀態由
            // main.cpp 在 BeginSnip 之後用 SetOcrAvailable 更新。
            toolbar_.Show(workArea_, defaultAction_);
        }
    }
}

void Overlay::DestroyWindows() {
    ReleaseCapture();
    if (toolbarReady_) { toolbar_.Hide(); toolbar_.Destroy(); toolbarReady_ = false; }
    for (auto& d : dim_) d.Destroy();
    border_.Destroy();
    if (labelReady_) { label_.Destroy(); labelReady_ = false; }
    if (owner_) {
        HWND h = owner_;
        owner_ = nullptr;
        DestroyWindow(h);
    }
    dragging_ = false;
    hasSel_ = false;
    hoverWin_ = nullptr;
    snip_ = false;
}

void Overlay::SetOcrAvailable(bool available) {
    if (!toolbarReady_) return;
    toolbar_.SetActionEnabled(ui::Toolbar::Action::Ocr, available);
    // 如果預設動作正好是 OCR 但引擎不在，退回截圖 —— 否則使用者一進來就
    // 面對一條不能用的路。
    if (!available && defaultAction_ == ui::Toolbar::Action::Ocr) {
        defaultAction_ = ui::Toolbar::Action::Snip;
        toolbar_.SetActive(defaultAction_);
    }
}

void Overlay::Cancel() {
    if (!owner_) return;
    pending_ = true;
    pendingOk_ = false;
    pendingRect_ = RECT{};
    pendingWin_ = nullptr;
    PostMessageW(owner_, WM_OVERLAY_DONE, 0, 0);
}

void Overlay::SetSelection(const RECT& r) {
    sel_ = r;
    hasSel_ = (r.right - r.left) >= 1 && (r.bottom - r.top) >= 1;
    UpdateVisuals();
}

// 目前「使用者意思上選了什麼」。拖曳過 → 矩形；沒拖過但懸停在某視窗 →
// 那個視窗的外框。工具列的點擊就是靠這個決定要處理哪一塊。
bool Overlay::EffectiveRect(RECT* out, HWND* win) const {
    if (hasSel_) {
        *out = sel_;
        if (win) *win = nullptr;
        return true;
    }
    if (hoverWin_) {
        *out = hoverRect_;
        if (win) *win = hoverWin_;
        return true;
    }
    return false;
}

void Overlay::UpdateVisuals() {
    if (!owner_) return;

    // ---- 四塊遮罩 ----
    // 有選取時：選取框以外變暗，框內完全不受影響（就是「透出來」的效果）。
    // 沒有選取時：整片變暗，提示「現在在選取模式」。
    RECT top{}, bottom{}, left{}, right{};
    if (hasSel_) {
        top = {virtual_.left, virtual_.top, virtual_.right, sel_.top};
        bottom = {virtual_.left, sel_.bottom, virtual_.right, virtual_.bottom};
        left = {virtual_.left, sel_.top, sel_.left, sel_.bottom};
        right = {sel_.right, sel_.top, virtual_.right, sel_.bottom};
    } else {
        top = virtual_;
    }
    // 遮罩與外框顯示時都插在工具列「正下方」，所以工具列從頭到尾不會被蓋住，
    // 也就完全不需要在每次更新後把它抬回最上層（那正是閃爍的來源）。
    // 工具列還沒建好（極早期）時傳 nullptr，退回 HWND_TOPMOST 也無所謂。
    HWND below = toolbarReady_ ? toolbar_.hwnd() : nullptr;
    dim_[0].SetRect(top, below);
    dim_[1].SetRect(bottom, below);
    dim_[2].SetRect(left, below);
    dim_[3].SetRect(right, below);

    // ---- 邊框與尺寸標籤 ----
    RECT target{};
    bool show = false;
    bool hovering = false;
    if (hasSel_) {
        target = sel_;
        show = true;
    } else if (hoverWin_) {
        target = hoverRect_;
        show = true;
        hovering = true;
    }

    if (show) {
        border_.SetColor(hovering ? kAccentWin : kAccent);
        // 框畫在選取範圍「內緣」：選取中只是視覺提示，開始錄製時整個覆蓋層
        // 就消失了，所以不會入鏡。
        border_.SetOuterRect(target, below);
        border_.Show(true);

        if (labelReady_) {
            wchar_t buf[64];
            swprintf_s(buf, L"%d × %d", (int)(target.right - target.left),
                       (int)(target.bottom - target.top));
            label_.SetText(buf);
            int x = target.right - label_.width() - 6;
            int y = target.bottom - label_.height() - 6;
            x = std::max(x, (int)virtual_.left + 4);
            y = std::max(y, (int)virtual_.top + 4);
            label_.MoveTo(x, y);
            label_.Show(true);
        }
    } else {
        border_.Show(false);
        if (labelReady_) label_.Show(false);
    }

    // 這裡刻意不再呼叫 toolbar_.Raise()。
    //
    // 舊版每一格滑鼠移動都做「遮罩插到 topmost 最前面 → 工具列抬回來」這一對
    // 操作，肉眼看到的就是工具列在閃。現在遮罩與外框都是插在工具列下方
    // （見上面 below），工具列的層級從頭到尾沒變過，一次都不用抬。
}

void Overlay::Finish(bool ok) {
    DoneFn fn = std::move(done_);
    SnipDoneFn sfn = std::move(snipDone_);
    const RECT r = pendingRect_;
    HWND win = pendingWin_;
    const bool wasSnip = snip_;
    const ui::Toolbar::Action act = pendingAction_;
    const bool wasOk = pendingOk_;
    pending_ = false;
    done_ = nullptr;
    snipDone_ = nullptr;
    DestroyWindows();
    if (wasSnip) {
        if (sfn) {
            SnipOutcome o;
            o.ok = wasOk;
            o.rect = r;
            o.win = win;
            o.action = act;
            sfn(o);
        }
        return;
    }
    if (fn) fn(ok, r, win);
}

// 送出結果。這是框選模式的**唯一**出口：
//   滑鼠放開、單擊選視窗、Enter、雙擊 —— 四條路都走這裡。
//
// 兩種模式共用同一個出口，差別只在「有沒有動作可以選」：
//   * 截圖 / OCR：一律用 defaultAction_（＝使用者在工具列上選的模式，預設
//     「截圖」），因為模式的選擇發生在「框選之前」，框完就只差送出。
//   * 錄影：沒有工具列、沒有動作，結果直接走 done_(ok, rect, win)。
//
// 這裡修過一次嚴重缺陷：原本第一行是 `if (!snip_ || !owner_) return;`，於是
// **錄影模式的四條路全部靜默失敗** —— 拖曳框選放開沒反應、單擊視窗沒反應、
// Enter 沒反應，覆蓋層卡在畫面上只能按 Esc。錄影模式看起來就像「功能沒做完」，
// 原因其實是這一行守衛把結果吃掉了（Finish() 早就支援 done_ 那條分支）。
void Overlay::Submit(const RECT& rect, HWND win) {
    if (!owner_) return;
    pending_ = true;
    pendingOk_ = true;
    pendingRect_ = rect;
    pendingWin_ = win;
    // 只有截圖模式需要「用哪個動作」；錄影模式留原值即可（Finish 不會讀）。
    if (snip_) pendingAction_ = defaultAction_;
    PostMessageW(owner_, WM_OVERLAY_DONE, 0, 0);
}

LRESULT CALLBACK Overlay::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_LBUTTONDOWN: {
        self->dragging_ = true;
        self->anchor_ = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        self->hoverWin_ = nullptr;
        self->hasSel_ = false;
        self->UpdateVisuals();
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (self->dragging_) {
            self->SetSelection(ui::Normalize(self->anchor_, pt));
        } else {
            // 懸停提示：滑過哪個視窗就把它的外框畫出來，單擊即可選它。
            HWND under = ui::WindowUnderPoint(pt);
            if (under != self->hoverWin_) {
                self->hoverWin_ = under;
                if (under) GetWindowRect(under, &self->hoverRect_);
                else self->hoverRect_ = RECT{};
                self->UpdateVisuals();
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self->dragging_) return 0;
        self->dragging_ = false;
        ReleaseCapture();

        const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT r = ui::Normalize(self->anchor_, pt);
        const int w2 = r.right - r.left;
        const int h2 = r.bottom - r.top;

        if (w2 < kMinSize || h2 < kMinSize) {
            // 沒有拖曳 = 單擊 → 選游標底下的視窗
            HWND under = ui::WindowUnderPoint(pt);
            if (under) {
                RECT wr{};
                GetWindowRect(under, &wr);
                self->Submit(wr, under);
                return 0;
            }
            // 點在桌面上：當成取消，避免「點一下什麼都沒發生」
            self->Cancel();
            return 0;
        }

        self->Submit(r, nullptr);
        return 0;
    }
    case WM_RBUTTONDOWN:
        self->Cancel();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            self->Cancel();
            return 0;
        }
        // Enter：用「目前有效的範圍」直接送出。拖曳過就是那個矩形，沒拖過但
        // 懸停在某個視窗上就是那個視窗 —— 框選與錄影兩種模式都成立
        // （錄影模式下這就是「框好 → Enter」的快速路徑，不必再碰滑鼠）。
        // 什麼都沒選就按 Enter 則維持原樣「什麼都不做」：這裡不能當成取消，
        // 錄影時手滑按到 Enter 就整個覆蓋層消失，比沒反應更糟。
        if (w == VK_RETURN) {
            RECT rr{};
            HWND win = nullptr;
            if (self->EffectiveRect(&rr, &win)) self->Submit(rr, win);
            return 0;
        }
        // 截圖模式：Tab / 左右鍵切換工具列的模式（鍵盤也能選截圖或 OCR）。
        // 跟點工具列一樣只是「改模式」，不會立刻執行 —— 執行一律等框選結束。
        if (self->snip_ && (w == VK_TAB || w == VK_LEFT || w == VK_RIGHT)) {
            self->defaultAction_ =
                (self->defaultAction_ == ui::Toolbar::Action::Snip)
                    ? ui::Toolbar::Action::Ocr
                    : ui::Toolbar::Action::Snip;
            if (self->toolbarReady_) self->toolbar_.SetActive(self->defaultAction_);
            return 0;
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // 雙擊 = 用目前模式送出（滑鼠版的快速路徑，跟 Enter 等價）。
        // 這裡不再限定截圖模式：錄影模式的雙擊也應該等於「就用這個範圍開始錄」。
        // （注意這個視窗類別沒有 CS_DBLCLKS，所以正常情況下第一下 WM_LBUTTONUP
        //   就已經送出了 —— 這段是保險，不是主路徑。）
        if (self->owner_) {
            RECT rr{};
            HWND win = nullptr;
            if (self->EffectiveRect(&rr, &win)) self->Submit(rr, win);
        }
        return 0;
    case WM_OVERLAY_DONE:
        if (self->pending_) self->Finish(self->pendingOk_);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_CROSS));
        return TRUE;
    case WM_CLOSE:
        self->Cancel();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace app
