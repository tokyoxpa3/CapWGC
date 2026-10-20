// ============================================================================
// ocr.h — OCR 引擎（行程內 ONNX 推論，不需要 Python）
// ============================================================================
//
// 架構：Start() 起一個專屬工作執行緒，在那裡把 det/rec 兩個 ONNX session
// 建好並暖機（CPU 上約 0.6~1.1 秒），之後每次辨識就只是排隊 → 推論 → 回報。
//
// 為什麼改成行程內：
//   舊版把 tools/ocr_cli.py 用外部 Python 跑起來，優點是換引擎不必重編，
//   代價是「一個錄屏軟體」多出 Python + paddleocr + 模型下載這一整串依賴。
//   對「下載解壓就能用」來說，那是唯一說不通的地方。現在整條路在行程內完成，
//   連暫存 PNG 都不用寫（直接吃記憶體裡的 BGRA）。
//
// 執行緒模型（重要）：
//   - Start() 在 UI 執行緒呼叫，立刻返回；載入模型在工作執行緒做。
//   - Request() 非阻塞：把像素複製進佇列就返回。截圖後 UI 還要能畫「辨識中」、
//     還能被 Esc 取消，不能卡在這裡。
//   - 結果用 PostMessage 送回 UI 執行緒，HandleMessage 在 UI 執行緒呼叫回呼。
//     工作執行緒**不碰**任何 UI 物件。
//   - OcrOnnx 實例只在工作執行緒使用，不需要鎖。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {

// 一次辨識的結果。
struct OcrResult {
    int id = 0;                     // 呼叫端給的編號（對應哪一次請求）
    bool ok = false;
    std::vector<std::wstring> lines;
    std::vector<float> scores;      // 與 lines 等長；可能為空
    int ms = 0;                     // 純推論時間（不含模型載入）
    std::wstring error;
};

class OcrEngine {
public:
    // 辨識完成時被呼叫（在 UI 執行緒）。
    using DoneFn = std::function<void(const OcrResult&)>;
    // 引擎就緒 / 起不來時被呼叫（在 UI 執行緒）。ready=false 時 detail 有原因。
    using ReadyFn = std::function<void(bool ready, const std::wstring& detail)>;

    ~OcrEngine();

    // modelsDir 空字串 = 自動偵測（exe 旁邊的 models\，見 ocr.cpp）。
    // notifyWnd：用來 PostMessage 回 UI 執行緒的視窗；必須是有效視窗。
    bool Start(HWND notifyWnd, UINT msgId, DoneFn onDone, ReadyFn onReady,
               const std::wstring& modelsDir);

    // 丟一張 BGRA（top-down）去辨識。非阻塞；結果之後由 DoneFn 送回來。
    // 回傳這次請求的 id（>0），失敗回 0。
    // 像素會在呼叫當下複製走，呼叫端不必保留緩衝。
    int Request(const uint8_t* bgra, int w, int h, int stride);

    // UI 執行緒收到 msgId 時呼叫這個，它會（在 UI 執行緒）叫 done_ / ready_。
    // wParam 1 = 結果（lParam = OcrResult*），2 = 就緒，3 = 失敗。
    // 這個函式負責 delete lParam 帶過來的堆積物件。
    void HandleMessage(WPARAM wParam, LPARAM lParam);

    // 取消「尚未開始」的請求；已在跑的那一次會跑完（殺不掉，也不該殺）。
    void CancelPending();

    bool ready() const { return ready_ != 0; }
    // 佇列 + 正在跑的那一個。給 UI 顯示「還在忙」用。
    int pending() const;
    void Stop();
    // 引擎存在（載入中或已就緒）。UI 用它決定 OCR 這個選項要不要畫成可用。
    bool running() const { return worker_ != nullptr; }

private:
    void WorkerMain();
    void PostDone(const OcrResult& r);
    void PostReady(bool ready, const std::wstring& detail);

    struct Job {
        int id = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> bgra;
    };

    HWND wnd_ = nullptr;
    UINT msg_ = 0;
    DoneFn done_;
    ReadyFn onReady_;

    std::wstring models_;

    HANDLE worker_ = nullptr;     // 工作執行緒
    HANDLE wake_ = nullptr;       // 叫醒 worker 的事件（有新工作或要停）

    CRITICAL_SECTION lock_;
    bool lockReady_ = false;
    std::vector<Job> queue_;      // 受 lock_ 保護
    volatile LONG pendingCount_ = 0;
    volatile LONG stop_ = 0;
    volatile LONG ready_ = 0;
    int nextId_ = 1;
};

} // namespace app
