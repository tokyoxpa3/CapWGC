// ============================================================================
// overlay.h — 區域框選覆蓋層
// ============================================================================
//
// 用法（錄影）：
//   overlay.Begin([](bool ok, const RECT& rect, HWND win) { ... });
//
// 用法（截圖 / OCR）：
//   overlay.BeginSnip(預設動作, [](SnipOutcome) { ... });
//
// 兩種選法（跟 Snipaste 一樣，不需要額外的模式切換）：
//   按住拖曳 → 選一個矩形
//   單擊     → 選游標底下的那個視窗（win 非空，rect 是它的外框）
//
// 為什麼遮罩是「四塊」而不是「一整片 + 挖洞」：一整片要做到中間透出桌面就得
// 用 per-pixel alpha（UpdateLayeredWindow + 32bpp DIB），而均勻變暗用
// LWA_ALPHA 一個 API 就夠。四塊面板各自是普通矩形，跟著選取框移動即可，
// 沒有 alpha 合成的問題，也不需要每次重算 region。
//
// 截圖模式多一顆頂端工具列（截圖 / OCR）—— 它是**模式選擇器**，不是動作按鈕：
//   * 進到框選畫面後，先在工具列點「截圖」或「OCR」（或按 Tab / 左右鍵切換）
//   * 然後才去框選；滑鼠放開的那一刻，就用選定的動作處理那一塊
//   * 沒碰工具列就直接框選 → 用預設動作（截圖）
//   * Enter / 雙擊 = 用「目前選定的模式」直接完成（不必碰工具列）
//
// 為什麼不做成「點工具列就立刻執行」：滑鼠停在工具列上時，底下懸停的
// 「視窗」其實是整個桌面，點一下等於對全螢幕動手，使用者根本還沒框選。
// 先把模式選定、再框選，才符合直覺（微軟 Win+Shift+S 也是這個順序）。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui.h"

#include <windows.h>
#include <functional>
#include <string>

namespace app {

class Overlay {
public:
    // ok = false → 使用者取消（Esc / 右鍵）。win 非空 = 選了視窗而不是矩形。
    using DoneFn = std::function<void(bool ok, const RECT& rectScreen, HWND win)>;

    // 截圖模式的結果。action 決定呼叫端要「進剪貼簿」還是「送 OCR」。
    struct SnipOutcome {
        bool ok = false;
        RECT rect{};
        HWND win = nullptr;
        ui::Toolbar::Action action = ui::Toolbar::Action::Snip;
    };
    using SnipDoneFn = std::function<void(const SnipOutcome&)>;

    ~Overlay();

    bool Begin(DoneFn fn);
    // 截圖 / OCR 模式。defaultAction 決定工具列一開始選中哪一邊。
    bool BeginSnip(ui::Toolbar::Action defaultAction, SnipDoneFn fn);
    bool active() const { return owner_ != nullptr; }
    bool snipMode() const { return snip_; }
    // OCR 引擎不可用時把那個分段畫成灰的（不讓使用者點進一條註定失敗的路）。
    // 只能在 snip 模式、且工具列已建立時有效。
    void SetOcrAvailable(bool available);
    void Cancel();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    bool BeginCommon();
    void OnCreate();
    void UpdateVisuals();
    void SetSelection(const RECT& r);
    void Finish(bool ok);
    // 框選模式的唯一出口：滑鼠放開 / 單擊選視窗 / Enter / 雙擊都走這裡，
    // 一律用當下的 defaultAction_（使用者在工具列選的模式）。
    void Submit(const RECT& rect, HWND win);
    void DestroyWindows();
    // 目前有效的選取範圍（有拖曳用拖曳的，否則用懸停視窗）。沒有就回 false。
    bool EffectiveRect(RECT* out, HWND* win) const;

    DoneFn done_;
    SnipDoneFn snipDone_;
    bool snip_ = false;
    HWND owner_ = nullptr;
    ui::DimPanel dim_[4];
    ui::BorderWindow border_;
    ui::TextBadge label_;
    bool labelReady_ = false;
    ui::Toolbar toolbar_;
    bool toolbarReady_ = false;
    ui::Toolbar::Action defaultAction_ = ui::Toolbar::Action::Snip;

    bool dragging_ = false;
    POINT anchor_{};
    RECT sel_{};
    bool hasSel_ = false;

    HWND hoverWin_ = nullptr;
    RECT hoverRect_{};
    RECT virtual_{};
    RECT workArea_{};

    // 待處理的結果（從 WndProc 內 post 出來，避免在自己視窗的訊息裡拆自己）
    bool pending_ = false;
    bool pendingOk_ = false;
    RECT pendingRect_{};
    HWND pendingWin_ = nullptr;
    ui::Toolbar::Action pendingAction_ = ui::Toolbar::Action::Snip;
};

} // namespace app
