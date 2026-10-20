// ============================================================================
// recorder.h — 可重用的錄製器（GUI 與 CLI 共用同一套時序語意）
// ============================================================================
//
// 為什麼要有這一層：錄製迴圈裡有三個「看起來可以隨便寫、但寫錯就要重測
// 整個 A/V 對齊」的語意，它們目前只存在於 src/cli/main.cpp：
//
//   1. 第一幀只拿來開編碼器，不寫進檔案（那時還不知道 duration）。
//   2. 檔案時間的零點是「第一筆寫入的幀」，音訊延到那一刻才 Start()。
//      零點若定在第一幀，音軌會多補一幀（16.7ms），變成過度補償。
//   3. 時長由前後 pts 相減得到（VFR），不是 1/fps。
//
// GUI 若自己再寫一份，這三條就有了兩個實作，之後修一處要改兩邊、測兩次。
// 所以抽在這裡，GUI 只負責「給參數、按開始、按停止」。
//
// 與 CLI 的差異（刻意保留，不是缺口）：
//   - 只有一個輸出目標。CLI 的 --ab / --ref 是量測工具，不進 GUI。
//   - A/V 偏移只接受固定值（來自設定），不做 auto 推導 —— 推導值已知不可信，
//     見 docs/08 §6.6。
//
// 執行緒契約：
//   - Start() 在主執行緒呼叫，之後 onFrame 會在 WGC 自己的執行緒上跑。
//   - Stop() 會阻塞（join WGC 執行緒 + 沖音訊 + 寫 moov），**不要**在 UI 執行緒
//     呼叫；GUI 是丟到 worker thread 再 post 訊息回來。
//   - 除了 stats() / running() / path()，其他方法都不是執行緒安全的。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "audio_capture.h"
#include "encoder.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace capwgc {

// 一條音源。每一項最後都會變成容器裡的一條 trak，順序就是 track 索引。
struct RecAudioSpec {
    AudioSourceKind kind = AudioSourceKind::Loopback;
    std::string device;   // 空 = 該種類的預設端點；可用索引 / 名稱子字串 / endpoint ID
};

struct RecorderConfig {
    // ---- 擷取目標（三選一，優先序 region > hwnd > monitor）----
    //
    // regionScreen 是**虛擬桌面的螢幕座標**（不是 item 座標）。Recorder 內部
    // 會挑中心點所在的那顆螢幕，再換算成 WGC item 的裁切矩形 —— 這件事不能
    // 丟給呼叫端做，因為 GUI 拿到的是滑鼠座標，CLI 拿到的是 --crop 的 item
    // 座標，兩者混用會出現「錄到隔壁螢幕」這種難以察覺的錯。
    bool useRegion = false;
    RECT regionScreen{};
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;   // 空 = 主螢幕

    // ---- 編碼 ----
    bool autoCodec = true;        // true = AV1 > HEVC > H.264（探測結果優先序）
    Codec codec = Codec::AV1;     // autoCodec == false 時的第一順位
    int quality = 70;
    int fps = 60;
    int gopSeconds = 2;
    int bitrate = 0;              // 0 = 依解析度與 fps 自動
    bool fragmented = true;       // true = fMP4（被 kill 也還能播）
    bool captureCursor = false;

    // ---- 音訊 ----
    std::vector<RecAudioSpec> audio;
    int audioBitrate = 128000;    // 每條軌的 AAC 碼率
    // 多個來源合成一條軌（預設）。關掉才會每個來源一條 —— 只有量測/除錯需要：
    // MP4 有多條音軌時大部分播放器只播第一條，使用者會覺得「麥克風沒錄到」。
    bool mixAudio = true;
    // 混音軌之外，每個來源再各留一條（給剪輯用）。那些分軌會標成「不要預設
    // 播」（tkhd enabled = 0），所以一般播放器仍然只播混音軌，不會疊音。
    bool separateTracks = false;

    // ---- 輸出 ----
    std::wstring outPath;
    // 寫進容器的 A/V 位移（100ns）。預設 0 = 不補償。
    // 正值往後推、負值裁掉開頭，語意見 docs/08 §6.7。
    int64_t avOffset100ns = 0;
};

struct RecorderStats {
    bool running = false;         // Start 成功且尚未 Stop
    bool inited = false;          // 編碼器已開、已經開始寫第一幀
    bool captureClosed = false;   // 被錄的視窗關了 / 顯示拓撲變了
    double seconds = 0.0;         // 從第一筆寫入起算
    uint64_t frames = 0;
    uint64_t dropped = 0;
    uint64_t throttled = 0;
    uint64_t bytes = 0;
    uint64_t samples = 0;
    uint64_t audioAU = 0;
    // 實際寫進檔案的音軌條數。混音時是 1（只有混音軌），勾了「保留分軌音源」
    // 才會是 1 + N。這個數字和「開了幾個音源」不是同一件事，QA 常常搞混。
    int audioTracks = 0;
    int outW = 0, outH = 0;
    std::string codecName;        // 實際用的（可能與偏好不同，降級鏈選的）
    std::string encoderName;
    std::string error;            // 最近一次錯誤；空 = 沒問題
    // 音源沒有全部開起來時的說明（端點不存在 / 被獨佔 / 啟動失敗）。
    // 空字串 = 全部正常。勾了麥克風卻沒聲音時，答案在這裡。
    std::string audioNote;
};

class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // 需要 MFStartup 已經呼叫過（GUI 在 WinMain 做一次）。
    // 失敗時 err 帶原因，並且不會留下半開的狀態。
    bool Start(const RecorderConfig& cfg, std::string* err);

    // 阻塞收尾。重複呼叫安全（第二次直接返回）。
    void Stop();

    bool running() const;
    RecorderStats stats() const;
    const std::wstring& path() const;

    // ---- 麥克風靜音（錄製中可切換）----
    //
    // 只影響 Input 類的音源，系統聲不動。實作是「把樣本換成 0 再送進編碼器」，
    // 不是停掉擷取：停掉會讓那條軌短一截、時間軸跟影像對不上，而且停／開兩次
    // 之間的那段空白在容器裡是無聲的「不存在」，播放器行為不一致。
    //
    // 三個都可以在錄製中從別的執行緒呼叫。
    void SetMicMuted(bool muted);
    bool micMuted() const;
    // 這次錄影有沒有成功的麥克風軌。沒有時 UI 上的麥克風鈕應該顯示成不可用。
    bool hasMic() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 輸出檔名的預設格式：CapWGC_2026-10-18_03-11-53.mp4
std::wstring MakeDefaultOutputPath(const std::wstring& dir);

} // namespace capwgc
