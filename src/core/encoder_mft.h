// ============================================================================
// encoder_mft.h — MF SinkWriter 包裝：H.264 / HEVC 硬編 → MP4
// ============================================================================
//
// 這一層刻意做得很薄，因為 M2 會換成 NVENC 直通（能拿到 AQ / lookahead /
// 4:4:4 / B 幀，這些 MF 都不暴露）。薄的意思是好換。
//
// 對外契約：
//   Open()   建立 SinkWriter，選硬體 MFT，設碼控
//   Write()  吃一張 BGRA 的 D3D11 紋理 + 真實 PTS，寫入 MP4
//   Close()  Finalize
//
// VFR：PTS 用 WGC 的 SystemRelativeTime（100ns），duration 由前後幀差決定。
// 靜止畫面沒有幀 → 沒有 sample → 播放器重複上一幀。這是壓縮率的核心。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "encoder.h"

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>

namespace capwgc {

class Mp4Encoder : public Encoder {
public:
    Mp4Encoder();
    ~Mp4Encoder() override;
    Mp4Encoder(const Mp4Encoder&) = delete;
    Mp4Encoder& operator=(const Mp4Encoder&) = delete;

    bool Open(const std::wstring& path, const EncoderConfig& cfg,
              ID3D11Device* device, std::string* err) override;

    // src 是 WGC 給的整張紋理；內部會 crop 到 cfg.width x cfg.height。
    bool Write(ID3D11Texture2D* src, int64_t pts100ns, int64_t dur100ns,
               std::string* err) override;

    bool Close(std::string* err) override;

    uint64_t bytes() const override;
    uint64_t samples() const override;
    std::string codecName() const override;
    std::string encoderName() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capwgc
