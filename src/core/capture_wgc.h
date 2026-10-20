// ============================================================================
// capture_wgc.h — Windows.Graphics.Capture 擷取層
// ============================================================================
//
// 契約：
//   - Start() 之後，onFrame 會在 WGC 的執行緒上被呼叫（free-threaded pool）。
//   - onFrame 收到的是「借用的」ID3D11Texture2D，回呼返回後即失效。
//     呼叫端必須在回呼內把像素複製走（CopySubresourceRegion 到自己的 ring）。
//   - 同一時間最多一個 onFrame 在執行（內部 busy guard，重入直接丟幀）。
//   - formatChanged == true 代表解析度變了，呼叫端必須重建 encoder 與 ring。
//
// 為什麼不做「內部重建」：重建 encoder 是呼叫端的事（它持有 MFT 與 ring），
// 這裡只負責回報事實。擷取層與編碼層的責任邊界要清楚，否則重建時的
// 資源生命週期會變成兩邊互相猜。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace capwgc {

struct CaptureConfig {
    // hwnd 非空 -> CreateForWindow（單視窗）
    // hwnd 為空且 monitor 非空 -> CreateForMonitor（整個螢幕）
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;

    // 裁切（相對於 item 左上角）。cropW/cropH 為 0 代表不裁切。
    // 注意：item 通常比 client 大 1px 邊框，這裡的座標是 item 座標。
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;

    bool captureCursor = false;  // 預設關：游標移動不該觸發新幀
    int maxFps = 60;             // Win11 24H2 需要 MinUpdateInterval 才不受 60 限制
};

struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr;  // 借用，回呼返回後失效
    int64_t pts100ns = 0;                // WGC 的 SystemRelativeTime
    uint32_t width = 0, height = 0;      // item 的原始尺寸（未裁切）
    bool formatChanged = false;          // 相對上一次回呼，尺寸變了
};

// 幀率閘門：把「來源送幀的節奏」壓到目標幀率以下。
//
// 不能用「距上一幀不足目標週期就丟」這種硬下界 —— 它會把目標幀率量化成
// 刷新率的因數，而且症狀完全不像「設定沒生效」：
//   239Hz 螢幕想錄 144 → 每 4.18ms 來一幀，6.94ms 的下界永遠不滿足，
//                         每兩幀收一幀 = 119fps
//   165Hz 螢幕想錄 144 → 6.06ms < 6.94ms，同樣每兩幀收一幀 = 82fps
// 使用者只會覺得「選了 144 卻卡卡的」，看不出是閘門自己在丟。
//
// 改成「信用」制：每過 dt 就存下 dt，真的出幀時才扣掉一個目標週期。信用
// 會累積，所以 239Hz 底下收幀的節奏是「收收放、收收放」，長期平均剛好
// 是 144。容差 5% 是給「剛好 60fps」的來源用的：來源週期 16.67ms、目標
// 16.667ms，只要有一點 jitter 就落到 16.6ms，嚴格比較會把它們全判成太早
// （歷史上真的把 60fps 的視窗錄成 48fps）。
struct FrameGate {
    int64_t targetUs = 0;   // 0 = 不限
    int64_t creditUs = 0;

    void Init(int fps) {
        targetUs = fps > 0 ? 1000000 / fps : 0;
        creditUs = 0;
    }

    // sinceUs：距「上一幀到達」的時間（不管那一幀有沒有被收下）。
    // 每一幀都要呼叫一次，否則信用不會累積，就又變回硬下界了。
    bool Admit(int64_t sinceUs) {
        if (targetUs <= 0) return true;
        creditUs += sinceUs;
        if (creditUs < targetUs - targetUs / 20) return false;
        creditUs -= targetUs;
        // 來源卡住之後不要一次爆一堆（信用上限一個週期）
        if (creditUs > targetUs) creditUs = targetUs;
        if (creditUs < -targetUs) creditUs = -targetUs;
        return true;
    }
};

class WgcCapture {
public:
    using FrameCallback = std::function<void(const CapturedFrame&)>;

    WgcCapture();
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    bool Start(const CaptureConfig& cfg, ID3D11Device* device,
               FrameCallback onFrame, std::string* err);
    void Stop();

    bool running() const;
    bool closed() const;       // item 被關閉（視窗關了 / 顯示拓撲變了 / GPU 重置）
    uint64_t frames() const;
    uint64_t dropped() const;  // busy guard 丟掉的次數
    uint64_t throttled() const;  // 幀率閘門丟掉的次數（內容重複，不是損失）
    std::string caps() const;  // 能力探測結果，給 log 用

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
