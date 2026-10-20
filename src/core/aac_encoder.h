// ============================================================================
// aac_encoder.h — MF 的 AAC encoder MFT 包裝
// ============================================================================
//
// 為什麼用 MF 的 AAC 而不是自己寫：AAC-LC 的編碼器是 Windows 內建的
// （MFT_CATEGORY_AUDIO_ENCODER + MFAudioFormat_AAC），不用額外授權，
// 也不用把 FAAC/libfdk 拉進來（那會讓授權從 MIT 變成混合）。
//
// 兩個關鍵設定：
//   MF_MT_AAC_PAYLOAD_TYPE = 0    輸出是 raw AAC，不含 ADTS 標頭。
//                                 MP4 要的就是 raw —— 帶 ADTS 的 AU 進 mdat
//                                 播放器會把 ADTS 當成音訊資料解，結果是雜音。
//   MF_MT_USER_DATA               輸出型別的這個 blob 裡有 AudioSpecificConfig，
//                                 muxer 的 esds 直接用它。自己推 ASC 很容易跟
//                                 編碼器實際用的參數不一致（尤其是 SBR 或
//                                 非 48k 的情況）。
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

class AacEncoder {
public:
    using PacketFn = std::function<void(const uint8_t* data, size_t size, int64_t pts100ns)>;

    AacEncoder();
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    bool Open(int sampleRate, int channels, int bitrate, std::string* err);
    void Close();

    // AudioSpecificConfig（esds 的 DecoderSpecificInfo）。Open 之後才有值。
    const std::vector<uint8_t>& config() const;
    int sampleRate() const;
    int channels() const;
    std::string name() const;
    uint64_t packets() const;
    std::string stats() const;

    // 送進 PCM（16-bit interleaved）。輸出透過 fn 回呼，一次呼叫可能吐 0~2 個 AU。
    bool Encode(const int16_t* pcm, size_t frames, int64_t pts100ns, PacketFn fn,
                std::string* err);

    // 沖出編碼器內部還沒吐完的 AU。關檔前一定要呼叫，否則尾巴會少一兩幀。
    //
    // targetFrames > 已送出的取樣點數時，會先用靜音補到那個長度再收尾。
    // 為什麼需要：AAC 一次吃 1024 個取樣點，最後不足一幀的部分留在編碼器裡，
    // END_OF_STREAM 不一定會把它吐出來 —— 實測 6 秒的錄影會少掉 58 ms 的尾巴。
    // 補到「影像的長度」讓兩條軌的時長一致，代價是最後多幾毫秒靜音。
    bool Flush(int64_t targetFrames, PacketFn fn, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
