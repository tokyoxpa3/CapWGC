// ============================================================================
// mft_encoder.h — 自驅動 IMFTransform + 自寫 muxer（M2）
// ============================================================================
//
// 為什麼不用 SinkWriter（M1 的三個致命限制，全部指向同一個解法）：
//   1. quality 傳不進 encoder。GetServiceForStream 拿 ICodecAPI 回
//      MF_E_INVALIDSTREAMNUMBER，MF_SINK_WRITER_ENCODER_CONFIG 塞 CODECAPI 的
//      GUID 被靜默忽略。後果是 HEVC 反而比 H.264 大 40% —— 兩邊都跑各自的預設。
//   2. moov 在檔尾，錄到一半崩潰整個檔案不可播。
//   3. 拿不到 B 幀（省 15-25%）與 AQ。
//
// 這裡直接持有 IMFTransform，所以 ICodecAPI 是第一等公民：
// RateControlMode = Quality、Quality = 0..100、GOPSize、BPictureCount 都能設，
// 而且每個都讀回驗證 —— 「SetValue 回 S_OK」和「encoder 真的照做」是兩件事。
//
// 兩個必踩的坑（照 wgcenc 的解法）：
//
// * drain pump。async MFT 要 ProcessOutput 之後才發下一次 NeedInput，而 WGC 是
//   變更驅動的（畫面靜止就沒有回呼）。只做「排空→收集」會變成每兩次回呼才前進
//   一步，一半的幀被靜默丟掉（實測 captured 62/s、encoded 33/s、零錯誤訊息）。
//   解法是「排空→收集→再排空」+ 一條 5ms 的排水執行緒。
//
// * VFR 下碼率。MFT 用 MF_MT_FRAME_RATE 算每幀預算：靜止 8.5fps、動態 59fps，
//   宣告固定 60 會讓碼率失控。Quality 模式下這件事不成立（不按幀配額），所以
//   這裡走 Quality 模式；真的退回 CBR 時才需要量測實際出幀率回推。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "encoder.h"
#include "mp4_muxer.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>

namespace capwgc {

class MftEncoder : public Encoder {
public:
    MftEncoder();
    ~MftEncoder() override;
    MftEncoder(const MftEncoder&) = delete;
    MftEncoder& operator=(const MftEncoder&) = delete;

    bool Open(const std::wstring& path, const EncoderConfig& cfg,
              ID3D11Device* device, std::string* err) override;
    bool Write(ID3D11Texture2D* src, int64_t pts100ns, int64_t dur100ns,
               std::string* err) override;
    bool Close(std::string* err) override;

    bool WriteAudio(int track, const uint8_t* data, size_t size, uint32_t frames,
                    int64_t pts100ns, std::string* err) override;
    int audioTrackCount() const override;
    uint64_t audioSamples() const override;
    bool SetAudioOffset(int track, int64_t offset100ns) override;

    uint64_t bytes() const override;
    uint64_t samples() const override;
    std::string codecName() const override;
    std::string encoderName() const override;
    std::string stats() const override;

    // 碼控是否真的生效（RateControlMode 讀回 == Quality）。false 代表這次錄製的
    // 檔案大小不能拿來跟另一種 codec 比 —— 兩邊都跑預設值，比了沒意義。
    bool qualityModeActive() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
