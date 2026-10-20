// ============================================================================
// audio_capture.h — WASAPI 擷取層（系統混音 loopback / 麥克風）
// ============================================================================
//
// 兩個來源，同一條管線：
//
//   Loopback  render 端點 + AUDCLNT_STREAMFLAGS_LOOPBACK → 「電腦發出的聲音」
//   Input     capture 端點，一般擷取                   → 麥克風 / 線路輸入
//
// 為什麼 loopback 是預設而不是麥克風：錄屏軟體的預設要錄「電腦發出的聲音」。
// WASAPI loopback 是唯一不用驅動、不用虛擬音效卡就能拿到系統混音的方法
// （Vista 之後內建）。麥克風是**另一條**軌，不是替代品 —— 兩條可以同時錄。
//
// 幾個必須知道的限制：
//
// * 取樣率與聲道數由「裝置的混音格式」決定，不是我們指定。要求 48k 立體而裝置
//   是 44.1k 6ch 時，硬要 48k 只會拿到 AUDCLNT_E_UNSUPPORTED_FORMAT。所以這裡
//   一律照 mix format 開，再把 >2 聲道降到立體（取前兩聲道）。
//
// * 靜音時 loopback 仍然會送資料，只是 flag 帶 AUDCLNT_BUFFERFLAGS_SILENT，
//   緩衝區內容是未定義的。不看 flag 直接送出去會得到雜音 —— 這是 loopback
//   最常見的 bug。
//
// * loopback 在系統完全安靜時**一個 packet 都不送**（音訊引擎 idle），不是送
//   靜音。所以要自建一條靜音渲染串流當 keep-alive。麥克風不需要：capture 端點
//   自己會持續送資料。
//
// * 時間戳這裡用「累計取樣點 / 取樣率」算，不是 QPC。音訊是連續的，
//   用裝置時鐘才不會在長錄影裡慢慢漂走；代價是它與影像的對齊精度只有
//   「啟動延遲」那個量級（實測 10~30 ms）。
//
// * 多條來源各自是獨立的時鐘。兩條軌的起點差就是兩次 Start() 的間隔（毫秒級），
//   不會隨時間累積。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace capwgc {

enum class AudioSourceKind {
    Loopback,   // 系統混音（render 端點的 loopback）
    Input,      // 麥克風 / 線路輸入（capture 端點）
};

const char* AudioSourceKindName(AudioSourceKind k);

struct AudioDeviceInfo {
    std::string id;         // endpoint ID（UTF-8），可直接餵給 device 欄位
    std::string name;       // friendly name
    bool render = true;     // true = 播放端點（可 loopback），false = 錄音端點
    bool isDefault = false;
};

// 列舉所有「使用中」的音訊端點。順序固定：先播放端點、再錄音端點。
// 這個順序就是 --audio-device 的數字索引，所以不要在中間插入排序。
std::vector<AudioDeviceInfo> EnumerateAudioDevices(std::string* err);

struct AudioFormat {
    int sampleRate = 48000;
    int channels = 2;       // 降混之後的聲道數（1 或 2）
};

struct AudioCaptureConfig {
    AudioSourceKind kind = AudioSourceKind::Loopback;

    // 空字串 = 該種類的預設端點。否則依序比對：完整 endpoint ID → 數字索引
    // （列舉順序）→ friendly name 的子字串（不分大小寫）。
    std::string device;
};

class AudioCapture {
public:
    // pcm 是 16-bit interleaved，frames 是「每聲道取樣點數」。
    using PacketFn = std::function<void(const int16_t* pcm, size_t frames, int64_t pts100ns)>;

    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // 只建立裝置與格式，不開始串流。失敗時 err 會帶上 HRESULT 與原因。
    bool Open(const AudioCaptureConfig& cfg, std::string* err);

    // 開始串流並起背景執行緒。fn 在該執行緒上被呼叫，必須自行確保下游執行緒安全。
    bool Start(PacketFn fn, std::string* err);
    void Stop();

    bool running() const;
    AudioFormat format() const;
    uint64_t frames() const;
    // 串流延遲（100ns）與緩衝區大小（frame）。loopback 的內容天生比「現在」
    // 晚一個 latency，算 A/V 偏移時要扣掉，見 docs/08 §6.4。
    int64_t streamLatency100ns() const;
    uint32_t bufferFrames() const;
    // 第一個 packet 的 QPC 時間戳（100ns）。這是「音訊 pts 0 對應哪個牆上
    // 時刻」唯一的直接證據，算 A/V 偏移用它，不要用 streamLatency100ns()
    // 去猜 —— 實測猜出來是 10.2ms，真實值 60ms 左右。0 = 還沒拿到。
    int64_t firstPacketQpc100ns() const;
    bool qpcWasRawCounts() const;   // 驅動回的是 QPC 原始計數（已換算過）
    AudioSourceKind kind() const;
    std::string deviceName() const;   // friendly name
    std::string deviceId() const;     // endpoint ID
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
