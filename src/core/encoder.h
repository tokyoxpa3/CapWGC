// ============================================================================
// encoder.h — 編碼層的共同型別與介面
// ============================================================================
//
// 為什麼要抽這一層：M2 之後同時存在兩個實作
//   encoder_mft.cpp  SinkWriter 包裝（M1，留著當對照組）
//   mft_encoder.cpp  自驅動 IMFTransform + 自寫 muxer（M2）
// 兩者的 Open/Write/Close 契約完全相同，呼叫端不該為了換引擎改寫整段流程。
//
// 契約（兩個實作都必須遵守）：
//   - Open() 之後才能 Write()；Open() 會依 cfg.width/height 建好所有資源。
//   - Write() 吃的是 WGC 給的整張 BGRA 紋理，內部負責 crop 與色彩轉換。
//   - pts100ns / dur100ns 都是 100ns 單位；pts 必須單調遞增（VFR 允許跳躍）。
//   - Close() 必須把檔尾寫完（moov 或 fMP4 的收尾），之後 bytes() 才準。
//   - 所有方法都不是執行緒安全的，呼叫端負責序列化。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace capwgc {

enum class Codec {
    AV1,    // 壓縮率最高，需要 Ada (RTX 40) / Arc / RX 7000 以上
    HEVC,   // 兼容性與壓縮率的平衡點
    H264,   // 相容性最好
};

// 碼控模式。掃 rate-PSNR 曲線時這個選擇比 quality 值本身重要。
//
// Quality  : MFT 原生的 CODECAPI_AVEncCommonQuality。旋鈕本身分段 —— 實測
//            NVIDIA H.264 MFT 在 UI 內容上 q85 會多花 20% 位元卻掉 0.18 dB，
//            q96 又只多花 4.7% 就跳 +2.84 dB。曲線不能用它當連續軸。
// ConstantQp: UnconstrainedVBR + 把 MinQP/MaxQP 釘成同一個值，位元率上限拉高到
//            不會觸發。這才是「一個 QP 一個點」的乾淨掃描軸。
enum class RcMode {
    Quality,
    ConstantQp,
};

// 音軌設定。decoderConfig 是 AudioSpecificConfig（AAC 的 2 bytes 或更長），
// 由 MF AAC encoder 的輸出型別 MF_MT_USER_DATA 取得 —— 不是我們自己算的。
struct AudioTrackConfig {
    int sampleRate = 48000;
    int channels = 2;
    std::vector<uint8_t> decoderConfig;

    // 顯示用名稱，寫進 hdlr。播放器的「音軌」選單看的就是這個。
    // 多條音軌時沒有名字就只能靠順序猜哪條是麥克風。
    std::string name;

    // 要不要讓播放器「預設就播這一條」（tkhd 的 Track_enabled 位元）。
    //
    // 分軌輸出時只有混音軌是 true，其餘（單獨的麥克風 / 系統聲）是 false：
    // 混音軌已經包含它們的內容，兩條都播出來會聽到兩次麥克風。關掉 enabled
    // 的軌，規格上要「當作不存在」，剪輯軟體則照樣列得出來。
    // 注意這只是請求，不是強制 —— 不吃這個旗標的播放器還是會全播。
    bool enabled = true;

    // 這條音軌要比檔案起點晚多少才開始播（100ns）。
    //
    // 為什麼需要：音訊串流是在第一幀之後才 Start 的，中間隔著 encoder Open
    // （實測 74~112ms，隨 GPU/驅動/codec 變）。不補償的話聲音會比畫面早那麼多。
    //
    // 注意這不是「把 sample 的 pts 加上去」—— MP4 的 stts 只存相鄰差，第一筆的
    // decode time 恆為 0。延遲要靠容器表達：progressive 寫 edts/elst 的 empty
    // edit，fMP4 把第一個 tfdt 的起點往後推。
    int64_t startOffset100ns = 0;
};

struct EncoderConfig {
    Codec codec = Codec::HEVC;

    int width = 0;         // 輸出尺寸（crop 之後）
    int height = 0;
    int nominalFps = 60;   // 只當碼率預算與 GOP 的基準，不是實際幀率

    // 碼控。quality 模式優先：MFT 的 Quality 是 0..100，
    // 70 大約對應 x264 crf 23 的觀感。bitrate 只在 quality 不支援時當 fallback。
    int quality = 70;
    int bitrate = 0;       // 0 = 依解析度與 fps 自動算一個上限

    int gopSeconds = 2;    // 按「時間」算 GOP，因為 VFR 下幀數不固定
    int bFrames = 0;       // 0 = 不啟用 B 幀（PTS 單調，muxer 不必寫 ctts）

    // 容器佈局。false = moov 在檔尾（相容性最好）；true = fMP4，每 GOP 一片，
    // 行程被 kill 也還能播。錄影軟體的預設應該是 true —— 見 mp4_muxer.h。
    bool fragmented = true;

    // 從 item 座標裁切（BGRA 源）
    int cropX = 0, cropY = 0;

    bool forceHardware = true;

    // 診斷用：把 MinQP/MaxQP 釘成同一個值，讓 quality 變成「精確的 QP 旋鈕」。
    // 預設 true —— 掃 quality 曲線時要的是可重現的量化步長，不是 MFT 自己的
    // 位元率調節。關掉它才看得到 MFT 原生 Quality 旋鈕的行為。
    bool pinQp = true;

    // 見 RcMode 的說明。ConstantQp 會忽略 quality，改用下面的 qp 值。
    RcMode rcMode = RcMode::Quality;
    int qp = 0;            // 只在 rcMode == ConstantQp 時有意義；0 = 依 quality 推

    // 音軌。空 = 純視訊檔。可以有多條，每條是一個獨立的 trak：
    // 系統混音與麥克風分開成兩條，播放器可以各自調音量或只留一條。
    // 順序就是 WriteAudio() 的 track 索引，也是容器的 track_ID - 2。
    std::vector<AudioTrackConfig> audioTracks;
};

class Encoder {
public:
    virtual ~Encoder() = default;

    virtual bool Open(const std::wstring& path, const EncoderConfig& cfg,
                      ID3D11Device* device, std::string* err) = 0;
    virtual bool Write(ID3D11Texture2D* src, int64_t pts100ns, int64_t dur100ns,
                       std::string* err) = 0;
    virtual bool Close(std::string* err) = 0;

    // 送一個音訊 access unit（AAC raw）到第 track 條音軌（0 起算）。
    // frames 是這個 AU 的取樣點數（AAC-LC 固定 1024）。
    // 沒有音軌、或 track 超出範圍的實作回 false。
    virtual bool WriteAudio(int track, const uint8_t* data, size_t size, uint32_t frames,
                            int64_t pts100ns, std::string* err) {
        (void)track; (void)data; (void)size; (void)frames; (void)pts100ns;
        if (err) *err = "this engine has no audio track";
        return false;
    }

    virtual int audioTrackCount() const { return 0; }
    virtual uint64_t audioSamples() const { return 0; }

    // 設定音軌的起始延遲（100ns），見 AudioTrackConfig::startOffset100ns。
    // 必須在第一筆音訊 sample 之前呼叫；不支援音軌的引擎回 false。
    virtual bool SetAudioOffset(int track, int64_t offset100ns) {
        (void)track; (void)offset100ns;
        return false;
    }

    bool hasAudio() const { return audioTrackCount() > 0; }

    virtual uint64_t bytes() const = 0;
    virtual uint64_t samples() const = 0;
    virtual std::string codecName() const = 0;
    virtual std::string encoderName() const = 0;

    // 一行診斷字串（丟幀數、muxer 位元組、碼控回讀…）。沒有就回空字串。
    virtual std::string stats() const { return {}; }
};

} // namespace capwgc
