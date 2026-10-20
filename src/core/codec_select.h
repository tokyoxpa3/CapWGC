// ============================================================================
// codec_select.h — 執行期探測這台機器能用哪些編碼器，並依優先序選一個
// ============================================================================
//
// 為什麼不能寫死：同一個 CapCli.exe 要在 RTX 3060 Ti（無 AV1）與 RTX 4070 Ti
// （有 AV1）上跑，還要能在 Intel QSV / AMD AMF / 純軟編的機器上跑。
// 硬體能力只能問，不能猜。
//
// 優先序：AV1 > HEVC > H.264；同一個 codec 內硬體優先。
// 每個 codec 只保留「最好的一個」encoder，避免清單裡出現五個 H.264。
// ============================================================================

#pragma once

#include "encoder_mft.h"

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace capwgc {

struct CodecCapability {
    Codec codec = Codec::H264;
    bool hardware = false;
    bool d3d11Aware = false;
    std::string encoderName;
};

struct GpuInfo {
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t vramMB = 0;
    std::string generation;    // 顯示用，例如 "Ada (RTX 40)"
    bool expectedAv1 = false;  // 依世代推斷的「應該有 AV1」，不是保證
};

GpuInfo QueryGpu(ID3D11Device* device);

// 需要 MFStartup 已經呼叫過。回傳已排序、已去重的可用清單。
std::vector<CodecCapability> ProbeCodecs();

// 在清單裡挑第一個符合偏好的：
//   prefer = Codec::AV1  → AV1 > HEVC > H264
//   prefer = Codec::HEVC → HEVC > H264（使用者明確指定時不降級到更低）
// 回傳 nullptr 代表一個都沒有。
const CodecCapability* PickCodec(const std::vector<CodecCapability>& list, Codec prefer);

const char* CodecName(Codec c);

} // namespace capwgc
