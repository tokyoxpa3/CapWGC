// ============================================================================
// probe_encoders.cpp — 列出這台機器上所有可用的視訊編碼器 MFT 與其能力
// ============================================================================
//
// 為什麼要先做這支：CapCore 的編碼層參數（subtype、碼控模式、B 幀、GOP）
// 不能寫死，因為不同 GPU 世代差異很大：
//   - Turing/Ampere 有 H.264 + HEVC 硬編，沒有 AV1
//   - Ada (RTX 40) 才有 AV1 硬編
//   - Intel QSV / AMD AMF 走同一套 MF 介面，但支援的 CODECAPI 不同
// 先量測再決定，而不是假設。
//
// 輸出：每個 encoder 一行摘要 + 逐項能力明細。
// 用法：probe_encoders.exe [--verbose]
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <oleauto.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <d3d11.h>
#include <strsafe.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")

// ---------------------------------------------------------------- helpers

static bool g_verbose = false;

static std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string HrStr(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

// CODECAPI 的 VARIANT 可能是 VT_UI4 / VT_I4 / VT_UI8 / VT_I8，統一取成 long long。
// 只用 ullVal / llVal，不用 uhVal / hVal：後兩者是 LARGE_INTEGER 結構，
// 在精簡標頭組合下不一定看得到。
static bool VariantToLL(const VARIANT& v, long long& out) {
    switch (v.vt) {
        case VT_UI4: out = (long long)v.ulVal; return true;
        case VT_I4:  out = (long long)v.lVal;  return true;
        case VT_UI8: out = (long long)v.ullVal; return true;
        case VT_I8:  out = (long long)v.llVal;  return true;
        case VT_UI2: out = (long long)v.uiVal; return true;
        case VT_I2:  out = (long long)v.iVal;  return true;
        case VT_BOOL: out = v.boolVal ? 1 : 0; return true;
        case VT_R4:  out = (long long)v.fltVal; return true;
        case VT_R8:  out = (long long)v.dblVal; return true;
        default: return false;
    }
}

static const char* SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_H264)   return "H.264";
    if (g == MFVideoFormat_HEVC)   return "HEVC";
    if (g == MFVideoFormat_AV1)    return "AV1";
    if (g == MFVideoFormat_VP90)   return "VP9";
    if (g == MFVideoFormat_NV12)   return "NV12";
    if (g == MFVideoFormat_ARGB32) return "ARGB32";
    if (g == MFVideoFormat_RGB32)  return "RGB32";
    if (g == MFVideoFormat_YUY2)   return "YUY2";
    return nullptr;
}

struct CodecApiProbe {
    const GUID* guid;
    const char* name;
};

// 這些是決定「壓縮率與質量」的關鍵參數，逐項問 MFT 支不支援。
static const CodecApiProbe kCodecApiItems[] = {
    { &CODECAPI_AVEncCommonRateControlMode,      "RateControlMode" },
    { &CODECAPI_AVEncCommonQuality,              "Quality" },
    { &CODECAPI_AVEncCommonMeanBitRate,          "MeanBitRate" },
    { &CODECAPI_AVEncCommonMaxBitRate,           "MaxBitRate" },
    { &CODECAPI_AVEncCommonBufferSize,           "BufferSize" },
    { &CODECAPI_AVEncMPVDefaultBPictureCount,    "BPictureCount" },
    { &CODECAPI_AVEncMPVGOPSize,                 "GOPSize" },
    { &CODECAPI_AVEncVideoEncodeQP,              "EncodeQP" },
    { &CODECAPI_AVEncVideoMinQP,                 "MinQP" },
    { &CODECAPI_AVEncVideoMaxQP,                 "MaxQP" },
    { &CODECAPI_AVEncAdaptiveMode,               "AdaptiveMode" },
    { &CODECAPI_AVEncVideoForceKeyFrame,         "ForceKeyFrame" },
    { &CODECAPI_AVEncCommonLowLatency,           "LowLatency" },
    { &CODECAPI_AVEncMPVLevel,                   "MPVLevel" },
    { &CODECAPI_AVEncMPVProfile,                 "MPVProfile" },
};

struct MftInfo {
    std::string name;
    GUID clsid{};
    bool d3d11Aware = false;
    bool hardware = false;
    bool async = false;
    bool transcodeOnly = false;

    // subtype -> 是否可設成功
    std::vector<std::pair<const char*, bool>> outputs;

    // 可接受的輸入格式
    std::vector<std::pair<const char*, bool>> inputs;

    // CODECAPI 探測結果
    struct Cap {
        std::string name;
        std::string support;   // yes / no / err
        std::string detail;
    };
    std::vector<Cap> caps;
};

// ------------------------------------------------------- type negotiation

static bool TryOutputType(IMFTransform* mft, const GUID& sub, UINT32 w, UINT32 h) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return false;
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

// 只有在 output type 已經設好（非 TEST_ONLY）的情況下才有意義，
// 所以這裡先正式設 output，再試 input。
static bool TryInputType(IMFTransform* mft, const GUID& sub, UINT32 w, UINT32 h) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    const HRESULT hr = mft->SetInputType(0, t, MFT_SET_TYPE_TEST_ONLY);
    t->Release();
    return SUCCEEDED(hr);
}

static void ProbeCodecApi(IMFTransform* mft, MftInfo& info) {
    // 多數 encoder MFT 直接支援 QI ICodecAPI（NVIDIA / MS 軟編都是）。
    // 不用 IMFGetService：它要 MF_MEDIATYPE_SERVICE_VIDEO_ENCODER，
    // 那個 GUID 在部分 SDK 版本裡不在公開標頭，QI 已經足夠。
    ICodecAPI* api = nullptr;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&api))) || !api) return;

    for (const auto& item : kCodecApiItems) {
        MftInfo::Cap cap;
        cap.name = item.name;

        const HRESULT sup = api->IsSupported(item.guid);
        if (sup == S_OK) cap.support = "yes";
        else if (sup == S_FALSE) cap.support = "no";
        else cap.support = "err " + HrStr(sup);

        if (sup == S_OK) {
            VARIANT cur; VariantInit(&cur);
            if (SUCCEEDED(api->GetValue(item.guid, &cur))) {
                long long v = 0;
                if (VariantToLL(cur, v)) cap.detail = "cur=" + std::to_string(v);
                VariantClear(&cur);
            }
            VARIANT lo, hi, st;
            VariantInit(&lo); VariantInit(&hi); VariantInit(&st);
            if (api->GetParameterRange(item.guid, &lo, &hi, &st) == S_OK) {
                long long l = 0, h = 0, s = 0;
                const bool okL = VariantToLL(lo, l);
                const bool okH = VariantToLL(hi, h);
                const bool okS = VariantToLL(st, s);
                if (okL && okH) {
                    cap.detail += " range=" + std::to_string(l) + ".." + std::to_string(h);
                    if (okS) cap.detail += " step=" + std::to_string(s);
                }
            }
            VariantClear(&lo); VariantClear(&hi); VariantClear(&st);
        }
        info.caps.push_back(std::move(cap));
    }

    api->Release();
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_verbose = true;
    }

    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        printf("CoInitializeEx failed %s\n", HrStr(hr).c_str());
        return 1;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        printf("MFStartup failed %s\n", HrStr(hr).c_str());
        return 1;
    }

    // D3D11 硬體裝置：用來判斷 MFT 是否真的能走 GPU 路徑
    ID3D11Device* d3d = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT dhr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &d3d, &fl, nullptr);
    printf("D3D11 hardware device: %s (feature level 0x%X)\n",
           SUCCEEDED(dhr) ? "OK" : HrStr(dhr).c_str(), (unsigned)fl);

    UINT32 count = 0;
    IMFActivate** acts = nullptr;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, nullptr,
                   &acts, &count);
    if (FAILED(hr)) {
        printf("MFTEnumEx failed %s\n", HrStr(hr).c_str());
        return 1;
    }
    printf("found %u video encoder MFT(s)\n\n", count);

    const UINT32 W = 1920, H = 1080;
    const GUID kSubtypes[] = { MFVideoFormat_H264, MFVideoFormat_HEVC, MFVideoFormat_AV1, MFVideoFormat_VP90 };
    const GUID kInputs[]   = { MFVideoFormat_NV12, MFVideoFormat_ARGB32, MFVideoFormat_RGB32, MFVideoFormat_YUY2 };

    std::vector<MftInfo> infos;

    for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* act = acts[i];
        MftInfo info;

        wchar_t* wname = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &len))) {
            info.name = Narrow(wname);
            CoTaskMemFree(wname);
        }
        if (info.name.empty()) info.name = "(unnamed)";

        UINT32 v = 0;
        info.d3d11Aware = SUCCEEDED(act->GetUINT32(MF_SA_D3D11_AWARE, &v)) && v != 0;
        info.hardware   = SUCCEEDED(act->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &v)) &&
                          (v & MFT_ENUM_FLAG_HARDWARE) != 0;
        act->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &info.clsid);

        // async MFT 必須先解鎖才設得了型別
        act->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

        IMFTransform* mft = nullptr;
        hr = act->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr) || !mft) {
            printf("[%u] %s\n    activate FAILED %s\n\n", i, info.name.c_str(), HrStr(hr).c_str());
            act->Release();
            continue;
        }

        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            UINT32 a = 0;
            // MF_SA_D3D11_AWARE 是 MFT 的 attribute，不是 activate 的。
            // 從 activate 讀會永遠拿到 0（第一版踩過）。
            if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &a))) info.d3d11Aware = a != 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &a))) info.async = a != 0;
            if (SUCCEEDED(attrs->GetUINT32(MFT_ENUM_TRANSCODE_ONLY_ATTRIBUTE, &a)))
                info.transcodeOnly = a != 0;
            attrs->Release();
        }

        // 逐個 subtype 試 output type
        for (const GUID& sub : kSubtypes) {
            const char* nm = SubtypeName(sub);
            if (!nm) continue;
            info.outputs.emplace_back(nm, TryOutputType(mft, sub, W, H));
        }

        // 對第一個可用的 subtype，正式設好 output 之後試 input
        for (const GUID& sub : kSubtypes) {
            if (!TryOutputType(mft, sub, W, H)) continue;

            IMFMediaType* out = nullptr;
            if (SUCCEEDED(MFCreateMediaType(&out))) {
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out->SetGUID(MF_MT_SUBTYPE, sub);
                out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(out, MF_MT_FRAME_SIZE, W, H);
                MFSetAttributeRatio(out, MF_MT_FRAME_RATE, 60, 1);
                MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                out->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
                mft->SetOutputType(0, out, 0);
                out->Release();
            }
            for (const GUID& in : kInputs) {
                const char* nm = SubtypeName(in);
                if (!nm) continue;
                info.inputs.emplace_back(nm, TryInputType(mft, in, W, H));
            }
            ProbeCodecApi(mft, info);
            break;
        }

        mft->Release();
        act->Release();
        infos.push_back(std::move(info));
    }

    CoTaskMemFree(acts);

    // ---- 印摘要
    printf("================ summary ================\n");
    printf("%-46s %-6s %-6s %s\n", "encoder", "d3d11", "hw", "outputs");
    for (const auto& in : infos) {
        std::string outs;
        for (const auto& o : in.outputs) {
            if (o.second) { if (!outs.empty()) outs += ","; outs += o.first; }
        }
        printf("%-46s %-6s %-6s %s\n", in.name.c_str(),
               in.d3d11Aware ? "yes" : "no", in.hardware ? "yes" : "no",
               outs.empty() ? "(none)" : outs.c_str());
    }

    // ---- 印明細
    for (const auto& in : infos) {
        bool anyOut = false;
        for (const auto& o : in.outputs) anyOut |= o.second;
        if (!anyOut) continue;

        printf("\n================ %s ================\n", in.name.c_str());
        printf("  d3d11-aware : %s\n", in.d3d11Aware ? "yes" : "no");
        printf("  hardware    : %s\n", in.hardware ? "yes" : "no");
        printf("  async       : %s\n", in.async ? "yes" : "no");
        printf("  transcode   : %s\n", in.transcodeOnly ? "yes" : "no");

        printf("  outputs     :");
        for (const auto& o : in.outputs) printf(" %s=%s", o.first, o.second ? "Y" : "n");
        printf("\n");

        printf("  inputs      :");
        for (const auto& o : in.inputs) printf(" %s=%s", o.first, o.second ? "Y" : "n");
        printf("\n");

        if (!in.caps.empty()) {
            printf("  codecapi:\n");
            for (const auto& c : in.caps) {
                if (c.support == "no" && !g_verbose) continue;
                printf("    %-18s %-6s %s\n", c.name.c_str(), c.support.c_str(), c.detail.c_str());
            }
        }
    }

    // ---- 解碼器：驗證容器要靠系統 decoder
    //
    // 「有 AV1 編碼器」不代表「解得回來」。Windows 的 AV1 解碼來自商店擴展
    // (AV1 Video Extension)，不是內建 —— 沒有它，mp4_check 只能證明檔案結構
    // 合理，不能證明解碼器吃得下，也量不出 PSNR。
    {
        IMFActivate** dacts = nullptr;
        UINT32 dcount = 0;
        printf("\n================ decoders ================\n");
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL, nullptr, nullptr,
                                &dacts, &dcount))) {
            bool any = false;
            for (UINT32 i = 0; i < dcount; ++i) {
                wchar_t* wn = nullptr;
                UINT32 len = 0;
                std::string nm;
                if (SUCCEEDED(dacts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wn, &len))) {
                    nm = Narrow(wn);
                    CoTaskMemFree(wn);
                }
                dacts[i]->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                IMFTransform* mft = nullptr;
                if (FAILED(dacts[i]->ActivateObject(IID_PPV_ARGS(&mft))) || !mft) {
                    dacts[i]->Release();
                    continue;
                }
                std::string acc;
                for (const GUID& sub : { MFVideoFormat_AV1, MFVideoFormat_HEVC, MFVideoFormat_H264 }) {
                    if (TryInputType(mft, sub, W, H)) {
                        if (!acc.empty()) acc += ",";
                        acc += SubtypeName(sub);
                    }
                }
                if (!acc.empty()) {
                    printf("%-46s %s\n", nm.c_str(), acc.c_str());
                    any = true;
                }
                mft->Release();
                dacts[i]->Release();
            }
            if (!any) printf("(no decoder MFT accepted AV1/HEVC/H.264)\n");
            CoTaskMemFree(dacts);
        }
    }

    // ---- 這台機器該用什麼
    bool hwAv1 = false, hwHevc = false, hwH264 = false;
    bool d3dAv1 = false, d3dHevc = false, d3dH264 = false;
    for (const auto& in : infos) {
        if (!in.hardware) continue;
        for (const auto& o : in.outputs) {
            if (!o.second) continue;
            if (strcmp(o.first, "AV1") == 0)   { hwAv1 = true;  d3dAv1 |= in.d3d11Aware; }
            if (strcmp(o.first, "HEVC") == 0)  { hwHevc = true; d3dHevc |= in.d3d11Aware; }
            if (strcmp(o.first, "H.264") == 0) { hwH264 = true; d3dH264 |= in.d3d11Aware; }
        }
    }

    printf("\n================ verdict ================\n");
    printf("hardware encoders on this machine:\n");
    printf("  AV1   : %s%s\n", hwAv1 ? "yes" : "no",
           hwAv1 ? (d3dAv1 ? " (d3d11-aware)" : " (NOT d3d11-aware)") : "");
    printf("  HEVC  : %s%s\n", hwHevc ? "yes" : "no",
           hwHevc ? (d3dHevc ? " (d3d11-aware)" : " (NOT d3d11-aware)") : "");
    printf("  H.264 : %s%s\n", hwH264 ? "yes" : "no",
           hwH264 ? (d3dH264 ? " (d3d11-aware)" : " (NOT d3d11-aware)") : "");

    if (hwAv1) {
        printf("\n=> 建議預設 AV1。對桌面 UI（大面積同色 + 重複紋理）\n"
               "   AV1 的 palette / intra-block-copy 比 HEVC 更有效，\n"
               "   實際省下的碼率通常高於一般影片的 20-30%%。\n");
    } else if (hwHevc) {
        printf("\n=> 沒有 AV1 硬編。預設 HEVC，它比 H.264 省 25-50%%。\n"
               "   AV1 需要 Ada (RTX 40) 或 Arc；這台不是。\n");
    } else if (hwH264) {
        printf("\n=> 只有 H.264 硬編。相容性最好，碼率最高。\n");
    } else {
        printf("\n=> 沒有可用的硬體編碼器，只能走 MF 軟體 MFT。\n");
    }

    printf("\nlegend: outputs/inputs 的 Y 代表 SetType(TEST_ONLY) 成功\n");
    printf("        codecapi 的 range 是 GetParameterRange 回報的合法區間\n");

    if (d3d) d3d->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
