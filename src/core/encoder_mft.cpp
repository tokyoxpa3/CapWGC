// ============================================================================
// encoder_mft.cpp
// ============================================================================

#include "encoder_mft.h"
#include "codec_select.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace capwgc {

namespace {

std::string HrStr(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 給 MFT 一個合理的碼率上限。Quality 模式下這只是「不准超過」，
// 不是目標值 —— 但設太小會把質量壓死，設太大等於沒有限制。
int AutoBitrate(int w, int h, int fps) {
    // 0.12 bits/pixel/frame 對桌面內容是「平衡」檔的上限。
    // 1080p60 -> 1920*1080*60*0.12 = 14.9 Mbps
    const double bpp = 0.12;
    long long b = (long long)((double)w * h * fps * bpp);
    b = std::max<long long>(b, 1500000);
    b = std::min<long long>(b, 80000000);
    return (int)b;
}

} // namespace

struct Mp4Encoder::Impl {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr;
    ComPtr<ICodecAPI> codecApi;
    UINT resetToken = 0;
    DWORD streamIndex = 0;

    EncoderConfig cfg;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;

    // BGRA → NV12 走 GPU。讓 SinkWriter 自己插轉換器會用軟體的
    // Color Converter DSP（720p 約 1.5-2 ms/frame），而且色彩矩陣不可控。
    ComPtr<ID3D11VideoDevice> videoDev;
    ComPtr<ID3D11VideoContext> videoCtx;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    bool useVideoProc = false;

    // 2 個輪替：WriteSample 理論上同步消費，但不同 MFT 的 async 深度不同，
    // 輪替可以避免「上一張還在被讀就覆蓋」。
    ComPtr<ID3D11Texture2D> ring[2];      // crop 後的 BGRA
    ComPtr<ID3D11Texture2D> nv12[2];      // 餵 encoder 的 NV12
    ComPtr<ID3D11VideoProcessorInputView> bgraView[2];
    ComPtr<ID3D11VideoProcessorOutputView> nv12View[2];
    int ringIdx = 0;

    uint64_t samples = 0;
    bool opened = false;
    bool began = false;
    std::wstring path;
    std::string codecName = "?";
    std::string encoderName = "?";

    bool fail(std::string* err, const char* what, HRESULT hr) {
        if (err) *err = std::string(what) + " failed " + HrStr(hr);
        return false;
    }
};

Mp4Encoder::Mp4Encoder() : impl_(std::make_unique<Impl>()) {}
Mp4Encoder::~Mp4Encoder() { Close(nullptr); }

bool Mp4Encoder::Open(const std::wstring& path, const EncoderConfig& cfg,
                      ID3D11Device* device, std::string* err) {
    if (!device) { if (err) *err = "null device"; return false; }

    auto& I = *impl_;
    I.cfg = cfg;
    I.dev = device;
    I.path = path;
    I.codecName = CodecName(cfg.codec);

    // H.264/HEVC 的 4:2:0 要求偶數尺寸。裁切後的寬高若不是偶數，
    // 直接減一（少一行像素，肉眼不可見）比讓 encoder 拒絕好。
    I.cfg.width = cfg.width & ~1;
    I.cfg.height = cfg.height & ~1;
    if (I.cfg.width <= 0 || I.cfg.height <= 0) {
        if (err) *err = "invalid output size";
        return false;
    }

    device->GetImmediateContext(I.ctx.GetAddressOf());
    if (!I.ctx) { if (err) *err = "no immediate context"; return false; }

    // ---- ring（BGRA）+ NV12 + GPU 色彩轉換
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)I.cfg.width;
    td.Height = (UINT)I.cfg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i) {
        HRESULT hr = device->CreateTexture2D(&td, nullptr, I.ring[i].GetAddressOf());
        if (FAILED(hr)) return I.fail(err, "CreateTexture2D(ring)", hr);
    }

    // NV12 的 BindFlags 必須含 RENDER_TARGET，VideoProcessor 才能寫進去；
    // 同時要能當 encoder 的輸入，所以加 SHADER_RESOURCE。
    D3D11_TEXTURE2D_DESC nd = td;
    nd.Format = DXGI_FORMAT_NV12;
    nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i) {
        HRESULT hr = device->CreateTexture2D(&nd, nullptr, I.nv12[i].GetAddressOf());
        if (FAILED(hr)) return I.fail(err, "CreateTexture2D(nv12)", hr);
    }

    {
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(I.videoDev.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = I.ctx->QueryInterface(IID_PPV_ARGS(I.videoCtx.GetAddressOf()));
        if (SUCCEEDED(hr) && I.videoDev && I.videoCtx) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = (UINT)I.cfg.width;
            cd.InputHeight = (UINT)I.cfg.height;
            cd.OutputWidth = (UINT)I.cfg.width;
            cd.OutputHeight = (UINT)I.cfg.height;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            hr = I.videoDev->CreateVideoProcessorEnumerator(&cd, I.vpEnum.GetAddressOf());
            if (SUCCEEDED(hr)) {
                hr = I.videoDev->CreateVideoProcessor(I.vpEnum.Get(), 0, I.vp.GetAddressOf());
            }
            if (SUCCEEDED(hr)) {
                // 桌面內容是 sRGB full range，編碼用 BT.709 limited。
                // 標錯會讓播放器多做一次 range 轉換，畫面發灰。
                D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
                inCs.Usage = 0;
                inCs.RGB_Range = 0;              // full
                inCs.YCbCr_Matrix = 1;           // BT.709
                inCs.YCbCr_xvYCC = 0;
                inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
                I.videoCtx->VideoProcessorSetStreamColorSpace(I.vp.Get(), 0, &inCs);

                D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = inCs;
                outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
                I.videoCtx->VideoProcessorSetOutputColorSpace(I.vp.Get(), &outCs);

                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                ovd.Texture2D.MipSlice = 0;
                for (int i = 0; i < 2; ++i) {
                    HRESULT vhr = I.videoDev->CreateVideoProcessorOutputView(
                        I.nv12[i].Get(), I.vpEnum.Get(), &ovd, I.nv12View[i].GetAddressOf());
                    if (FAILED(vhr)) { hr = vhr; break; }
                }

                // input view 也快取：ring 是固定的兩張，每幀重建只是浪費。
                if (SUCCEEDED(hr)) {
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
                    ivd.FourCC = 0;
                    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivd.Texture2D.MipSlice = 0;
                    ivd.Texture2D.ArraySlice = 0;
                    for (int i = 0; i < 2; ++i) {
                        HRESULT vhr = I.videoDev->CreateVideoProcessorInputView(
                            I.ring[i].Get(), I.vpEnum.Get(), &ivd, I.bgraView[i].GetAddressOf());
                        if (FAILED(vhr)) { hr = vhr; break; }
                    }
                }
                if (SUCCEEDED(hr)) I.useVideoProc = true;
            }
        }
        fprintf(stderr, "[enc] color convert: %s\n",
                I.useVideoProc ? "GPU VideoProcessor (BGRA->NV12, BT.709)"
                               : "SinkWriter software converter");
    }

    // ---- DXGI device manager
    HRESULT hr = MFCreateDXGIDeviceManager(&I.resetToken, I.dxgiMgr.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateDXGIDeviceManager", hr);
    hr = I.dxgiMgr->ResetDevice(device, I.resetToken);
    if (FAILED(hr)) return I.fail(err, "ResetDevice", hr);

    ComPtr<IMFAttributes> attrs;
    hr = MFCreateAttributes(attrs.GetAddressOf(), 4);
    if (FAILED(hr)) return I.fail(err, "MFCreateAttributes", hr);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, I.dxgiMgr.Get());

    // 嘗試一：把 CODECAPI 的 key 當成 attribute 塞給 encoder。
    // MF_SINK_WRITER_ENCODER_CONFIG 是「傳給 encoder 的 IMFAttributes」，
    // 有些 MFT 會用它初始化內部參數。文件沒保證 CODECAPI 的 GUID 能這樣用，
    // 但不支援時只是被忽略，不會失敗 —— 值得試。
    {
        ComPtr<IMFAttributes> encCfg;
        if (SUCCEEDED(MFCreateAttributes(encCfg.GetAddressOf(), 4)) && encCfg) {
            encCfg->SetUINT32(CODECAPI_AVEncCommonRateControlMode,
                              (UINT32)eAVEncCommonRateControlMode_Quality);
            encCfg->SetUINT32(CODECAPI_AVEncCommonQuality,
                              (UINT32)std::clamp(I.cfg.quality, 0, 100));
            encCfg->SetUINT32(CODECAPI_AVEncMPVGOPSize,
                              (UINT32)(I.cfg.nominalFps * std::max(1, I.cfg.gopSeconds)));
            attrs->SetUnknown(MF_SINK_WRITER_ENCODER_CONFIG, encCfg.Get());
        }
    }

    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), I.writer.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateSinkWriterFromURL", hr);

    // ---- output type
    const int bitrate = I.cfg.bitrate > 0 ? I.cfg.bitrate
                                          : AutoBitrate(I.cfg.width, I.cfg.height, I.cfg.nominalFps);

    ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(outType.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateMediaType(out)", hr);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    switch (I.cfg.codec) {
        case Codec::AV1:  outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AV1);  break;
        case Codec::HEVC: outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC); break;
        default:          outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); break;
    }
    outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, (UINT32)I.cfg.width, (UINT32)I.cfg.height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, (UINT32)I.cfg.nominalFps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = I.writer->AddStream(outType.Get(), &I.streamIndex);
    if (FAILED(hr)) {
        // 明確回報是哪個 codec 掛了，讓呼叫端可以降級到下一個候選，
        // 而不是丟一個看不懂的 HRESULT。
        if (err) {
            *err = std::string("AddStream(") + CodecName(I.cfg.codec) + ") failed " + HrStr(hr) +
                   " (encoder unavailable, or container does not accept this codec)";
        }
        return false;
    }

    // ---- 嘗試接管碼控
    //
    // SinkWriter 沒有公開介面設 CODECAPI，但 GetServiceForStream 在部分
    // 實作上可以拿到內部的 encoder。拿到就把它從 CBR 改成 Quality 模式 ——
    // 這是「靜態畫面不再燒碼率」的關鍵。拿不到就只靠 output type 的 bitrate。
    hr = I.writer->GetServiceForStream(I.streamIndex, GUID_NULL, IID_PPV_ARGS(I.codecApi.GetAddressOf()));
    if (SUCCEEDED(hr) && I.codecApi) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;

        v.ulVal = eAVEncCommonRateControlMode_Quality;
        HRESULT h1 = I.codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);

        v.ulVal = (ULONG)std::clamp(I.cfg.quality, 0, 100);
        HRESULT h2 = I.codecApi->SetValue(&CODECAPI_AVEncCommonQuality, &v);

        v.ulVal = (ULONG)(I.cfg.nominalFps * std::max(1, I.cfg.gopSeconds));
        HRESULT h3 = I.codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

        // 讀回驗證：「呼叫沒報錯」不等於生效。
        VARIANT rb;
        VariantInit(&rb);
        ULONG mode = 0xFFFFFFFF, q = 0xFFFFFFFF;
        if (SUCCEEDED(I.codecApi->GetValue(&CODECAPI_AVEncCommonRateControlMode, &rb)) &&
            rb.vt == VT_UI4) {
            mode = rb.ulVal;
        }
        VariantClear(&rb);
        VariantInit(&rb);
        if (SUCCEEDED(I.codecApi->GetValue(&CODECAPI_AVEncCommonQuality, &rb)) &&
            rb.vt == VT_UI4) {
            q = rb.ulVal;
        }
        VariantClear(&rb);

        fprintf(stderr,
                "[enc] codecapi: rateControl set=%s quality set=%s gop set=%s "
                "-> readback mode=%lu quality=%lu\n",
                SUCCEEDED(h1) ? "ok" : "FAIL", SUCCEEDED(h2) ? "ok" : "FAIL",
                SUCCEEDED(h3) ? "ok" : "FAIL", mode, q);
    } else {
        fprintf(stderr, "[enc] codecapi: GetServiceForStream unavailable (%s); "
                        "falling back to bitrate-only control\n", HrStr(hr).c_str());
    }

    // ---- 內部 encoder 名稱（純診斷用）
    {
        ComPtr<IMFTransform> mft;
        if (SUCCEEDED(I.writer->GetServiceForStream(I.streamIndex, GUID_NULL,
                                                    IID_PPV_ARGS(mft.GetAddressOf()))) && mft) {
            ComPtr<IMFAttributes> ma;
            if (SUCCEEDED(mft->GetAttributes(ma.GetAddressOf())) && ma) {
                LPWSTR nm = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(ma->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len))) {
                    I.encoderName = Narrow(nm);
                    CoTaskMemFree(nm);
                }
            }
        }
    }

    // ---- input type（BGRA，讓 SinkWriter 自己插色彩轉換）
    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(inType.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateMediaType(in)", hr);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, I.useVideoProc ? MFVideoFormat_NV12 : MFVideoFormat_ARGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, (UINT32)I.cfg.width, (UINT32)I.cfg.height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, (UINT32)I.cfg.nominalFps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = I.writer->SetInputMediaType(I.streamIndex, inType.Get(), nullptr);
    if (FAILED(hr)) return I.fail(err, "SetInputMediaType(ARGB32)", hr);

    hr = I.writer->BeginWriting();
    if (FAILED(hr)) return I.fail(err, "BeginWriting", hr);

    I.opened = true;
    I.began = true;
    fprintf(stderr, "[enc] %s %dx%d @%dfps q%d bitrate<=%d -> %s\n",
            I.codecName.c_str(), I.cfg.width, I.cfg.height, I.cfg.nominalFps,
            I.cfg.quality, bitrate, Narrow(path.c_str()).c_str());
    return true;
}

bool Mp4Encoder::Write(ID3D11Texture2D* src, int64_t pts100ns, int64_t dur100ns,
                       std::string* err) {
    auto& I = *impl_;
    if (!I.opened || !src) { if (err) *err = "not open / null texture"; return false; }

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    // crop box 必須落在源紋理內。WGC 在解析度變更的瞬間可能給舊尺寸的幀，
    // 這裡擋掉比讓 CopySubresourceRegion 回 E_INVALIDARG 好。
    if (I.cfg.cropX < 0 || I.cfg.cropY < 0 ||
        I.cfg.cropX + I.cfg.width > (int)sd.Width ||
        I.cfg.cropY + I.cfg.height > (int)sd.Height) {
        if (err) {
            char b[160];
            snprintf(b, sizeof(b), "crop %d,%d %dx%d outside source %ux%u",
                     I.cfg.cropX, I.cfg.cropY, I.cfg.width, I.cfg.height, sd.Width, sd.Height);
            *err = b;
        }
        return false;
    }

    const int slot = I.ringIdx;
    I.ringIdx ^= 1;
    ID3D11Texture2D* dst = I.ring[slot].Get();

    if (I.cfg.cropX == 0 && I.cfg.cropY == 0 &&
        (int)sd.Width == I.cfg.width && (int)sd.Height == I.cfg.height) {
        I.ctx->CopyResource(dst, src);
    } else {
        D3D11_BOX box{};
        box.left = (UINT)I.cfg.cropX;
        box.top = (UINT)I.cfg.cropY;
        box.front = 0;
        box.right = (UINT)(I.cfg.cropX + I.cfg.width);
        box.bottom = (UINT)(I.cfg.cropY + I.cfg.height);
        box.back = 1;
        I.ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
    }

    // BGRA → NV12 在 GPU 上做，CPU 全程不碰像素。
    ID3D11Texture2D* feed = dst;
    if (I.useVideoProc) {
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = I.bgraView[slot].Get();
        HRESULT vhr = I.videoCtx->VideoProcessorBlt(I.vp.Get(), I.nv12View[slot].Get(), 0, 1, &st);
        if (FAILED(vhr)) return I.fail(err, "VideoProcessorBlt", vhr);
        feed = I.nv12[slot].Get();
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), feed, 0, FALSE, buf.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateDXGISurfaceBuffer", hr);

    // 必須設 current length，否則某些 MFT 會認為 buffer 是空的。
    ComPtr<IMF2DBuffer> b2d;
    if (SUCCEEDED(buf.As(&b2d)) && b2d) {
        DWORD clen = 0;
        if (SUCCEEDED(b2d->GetContiguousLength(&clen))) buf->SetCurrentLength(clen);
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateSample", hr);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(dur100ns > 0 ? dur100ns : (10000000LL / I.cfg.nominalFps));

    hr = I.writer->WriteSample(I.streamIndex, sample.Get());
    if (FAILED(hr)) return I.fail(err, "WriteSample", hr);

    I.samples++;
    return true;
}

bool Mp4Encoder::Close(std::string* err) {
    auto& I = *impl_;
    if (!I.opened) return true;
    I.opened = false;

    HRESULT hr = I.writer->Finalize();
    if (FAILED(hr)) return I.fail(err, "Finalize", hr);
    return true;
}

uint64_t Mp4Encoder::bytes() const {
    if (impl_->path.empty()) return 0;
    HANDLE h = CreateFileW(impl_->path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (uint64_t)sz.QuadPart;
}

uint64_t Mp4Encoder::samples() const { return impl_->samples; }
std::string Mp4Encoder::codecName() const { return impl_->codecName; }
std::string Mp4Encoder::encoderName() const { return impl_->encoderName; }

} // namespace capwgc
