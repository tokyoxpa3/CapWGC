// ============================================================================
// codec_select.cpp
// ============================================================================

#include "codec_select.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace capwgc {

namespace {

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const GUID& SubtypeOf(Codec c) {
    switch (c) {
        case Codec::AV1:  return MFVideoFormat_AV1;
        case Codec::HEVC: return MFVideoFormat_HEVC;
        default:          return MFVideoFormat_H264;
    }
}

int Priority(Codec c) {
    switch (c) {
        case Codec::AV1:  return 0;
        case Codec::HEVC: return 1;
        default:          return 2;
    }
}

// 探測用：只問「這個 MFT 接不接受這個 subtype」，不真的初始化串流。
bool TryOutputType(IMFTransform* mft, const GUID& sub, UINT32 w, UINT32 h) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t)) || !t) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    const HRESULT hr = mft->SetOutputType(0, t, MFT_SET_TYPE_TEST_ONLY);
    t->Release();
    return SUCCEEDED(hr);
}

bool HasToken(const char* hay, const char* needle) {
    return hay && needle && strstr(hay, needle) != nullptr;
}

} // namespace

const char* CodecName(Codec c) {
    switch (c) {
        case Codec::AV1:  return "AV1";
        case Codec::HEVC: return "HEVC";
        default:          return "H.264";
    }
}

GpuInfo QueryGpu(ID3D11Device* device) {
    GpuInfo gi;
    if (!device) return gi;

    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev) {
        return gi;
    }
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            gi.name = Narrow(ad.Description);
            gi.vendorId = ad.VendorId;
            gi.deviceId = ad.DeviceId;
            gi.vramMB = ad.DedicatedVideoMemory / 1048576;
        }
        adapter->Release();
    }
    dxgiDev->Release();

    // 世代推斷只用來「顯示」與「提示」，實際能力一律以 ProbeCodecs 為準。
    const char* n = gi.name.c_str();
    if (gi.vendorId == 0x10DE) {           // NVIDIA
        if (HasToken(n, "RTX 50"))       { gi.generation = "Blackwell (RTX 50)"; gi.expectedAv1 = true; }
        else if (HasToken(n, "RTX 40"))  { gi.generation = "Ada (RTX 40)";       gi.expectedAv1 = true; }
        else if (HasToken(n, "RTX 30"))  { gi.generation = "Ampere (RTX 30)"; }
        else if (HasToken(n, "RTX 20"))  { gi.generation = "Turing (RTX 20)"; }
        else if (HasToken(n, "GTX 16"))  { gi.generation = "Turing (GTX 16)"; }
        else if (HasToken(n, "GTX 10"))  { gi.generation = "Pascal (GTX 10)"; }
    } else if (gi.vendorId == 0x8086) {    // Intel
        if (HasToken(n, "Arc"))          { gi.generation = "Xe (Arc)"; gi.expectedAv1 = true; }
        else if (HasToken(n, "Iris"))    { gi.generation = "Xe (Iris)"; }
        else if (HasToken(n, "UHD"))     { gi.generation = "Gen9/11 (UHD)"; }
    } else if (gi.vendorId == 0x1002) {    // AMD
        if (HasToken(n, "RX 9"))         { gi.generation = "RDNA4 (RX 9000)"; gi.expectedAv1 = true; }
        else if (HasToken(n, "RX 7"))    { gi.generation = "RDNA3 (RX 7000)"; gi.expectedAv1 = true; }
        else if (HasToken(n, "RX 6"))    { gi.generation = "RDNA2 (RX 6000)"; }
        else if (HasToken(n, "RX 5"))    { gi.generation = "RDNA (RX 5000)"; }
    }
    if (gi.generation.empty()) gi.generation = "unknown";
    return gi;
}

std::vector<CodecCapability> ProbeCodecs() {
    std::vector<CodecCapability> all;

    UINT32 count = 0;
    IMFActivate** acts = nullptr;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL,
                           nullptr, nullptr, &acts, &count);
    if (FAILED(hr) || !acts) return all;

    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* act = acts[i];
        CodecCapability base;

        wchar_t* wname = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &len))) {
            base.encoderName = Narrow(wname);
            CoTaskMemFree(wname);
        }

        UINT32 v = 0;
        base.hardware = SUCCEEDED(act->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &v)) &&
                        (v & MFT_ENUM_FLAG_HARDWARE) != 0;

        act->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

        IMFTransform* mft = nullptr;
        if (SUCCEEDED(act->ActivateObject(IID_PPV_ARGS(&mft))) && mft) {
            IMFAttributes* attrs = nullptr;
            if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
                if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &v))) base.d3d11Aware = v != 0;
                attrs->Release();
            }

            for (Codec c : {Codec::AV1, Codec::HEVC, Codec::H264}) {
                if (TryOutputType(mft, SubtypeOf(c), 1920, 1080)) {
                    CodecCapability cc = base;
                    cc.codec = c;
                    all.push_back(std::move(cc));
                }
            }
            mft->Release();
        }
        act->Release();
    }
    CoTaskMemFree(acts);

    // 排序：codec 優先序，其次硬體優先。
    // 注意比較器必須是嚴格弱序：用 `a.hw && !b.hw` 會在兩者都 false 時
    // 同時回 false，看起來能用但 std::sort 的行為未定義。
    std::sort(all.begin(), all.end(), [](const CodecCapability& a, const CodecCapability& b) {
        const int pa = Priority(a.codec), pb = Priority(b.codec);
        if (pa != pb) return pa < pb;
        if (a.hardware != b.hardware) return a.hardware;
        return false;
    });

    // 去重：每個 codec 只留第一個（已排序，所以是最好的那個）。
    std::vector<CodecCapability> out;
    for (const auto& c : all) {
        bool seen = false;
        for (const auto& o : out) {
            if (o.codec == c.codec) { seen = true; break; }
        }
        if (!seen) out.push_back(c);
    }
    return out;
}

const CodecCapability* PickCodec(const std::vector<CodecCapability>& list, Codec prefer) {
    if (list.empty()) return nullptr;
    const int want = Priority(prefer);
    for (const auto& c : list) {
        if (Priority(c.codec) <= want) return &c;
    }
    // 偏好拿不到就退回清單裡最好的（不是退回最差的）。
    return &list.front();
}

} // namespace capwgc
