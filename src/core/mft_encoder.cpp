// ============================================================================
// mft_encoder.cpp — 自驅動 IMFTransform（async）+ 自寫 MP4 muxer
// ============================================================================

#include "mft_encoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

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

// Quality 模式下這只是「不准超過」的上限，不是目標值。
int AutoBitrate(int w, int h, int fps) {
    const double bpp = 0.12;
    long long b = (long long)((double)w * h * fps * bpp);
    b = std::max<long long>(b, 1500000);
    b = std::min<long long>(b, 80000000);
    return (int)b;
}

const GUID& SubtypeFor(Codec c) {
    switch (c) {
        case Codec::AV1:  return MFVideoFormat_AV1;
        case Codec::HEVC: return MFVideoFormat_HEVC;
        default:          return MFVideoFormat_H264;
    }
}

const char* CodecStr(Codec c) {
    switch (c) {
        case Codec::AV1:  return "AV1";
        case Codec::HEVC: return "HEVC";
        default:          return "H.264";
    }
}

UINT32 ProfileFor(Codec c) {
    switch (c) {
        case Codec::HEVC: return eAVEncH265VProfile_Main_420_8;
        case Codec::H264: return eAVEncH264VProfile_High;
        default:          return 0;
    }
}

// async MFT 的輸入貼圖在 ProcessInput 返回後仍可能被讀。ring 的每個 slot 只有在
// 「outputs 越過它的序號」之後才確定被消費完 —— 覆寫一張還在被讀的貼圖，症狀是
// 偶發的破格，而那是最難查的一種 bug。
constexpr int kRing = 4;

} // namespace

// ============================================================================

struct MftEncoder::Impl {
    // ---- encoder
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> evg;
    ComPtr<IMFDXGIDeviceManager> mgr;
    ComPtr<ICodecAPI> codec;
    MFT_OUTPUT_STREAM_INFO si{};
    bool mftAllocates = false;
    DWORD outCap = 0;
    int credits = 0;

    // ---- device / colour conversion
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VideoDevice> videoDev;
    ComPtr<ID3D11VideoContext> videoCtx;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    bool useNv12 = false;

    ComPtr<ID3D11Texture2D> bgra[kRing];
    ComPtr<ID3D11Texture2D> nv12[kRing];
    ComPtr<ID3D11VideoProcessorInputView> inView[kRing];
    ComPtr<ID3D11VideoProcessorOutputView> outView[kRing];
    long long slotSeq[kRing] = {-1, -1, -1, -1};

    // ---- sink
    Mp4Muxer mux;

    // ---- 同步
    // MF 的 transform 不保證 ProcessInput 與 ProcessOutput 可以並行，而 WGC 的
    // 回呼執行緒與排水幫浦正好同時在做這兩件事。少了這把鎖，症狀是偶爾的破格
    // 加一次 0x80004005，兩者都不會指向「鎖不夠」。
    mutable std::mutex mu;
    std::thread pump;
    std::atomic<bool> stop{false};

    // ---- 狀態
    EncoderConfig cfg;
    std::wstring path;
    std::string codecName = "?";
    std::string encoderName = "?";
    bool opened = false;
    bool qualityMode = false;
    bool qpPinned = false;
    bool began = false;

    std::atomic<long long> outputs{0};
    long long submitted = 0;
    long long encoded = 0;
    long long droppedNoCredit = 0;
    long long droppedNoSlot = 0;
    long long processInputRetries = 0;
    long long encodeErrors = 0;
    long long bufferTooSmall = 0;
    long long lastInputDur = 0;
    int64_t t0 = -1;
    HRESULT lastHr = S_OK;
    std::string lastError;

    bool fail(std::string* err, const std::string& what, HRESULT hr) {
        lastError = what + " failed " + HrStr(hr);
        if (err) *err = lastError;
        return false;
    }

    int DrainEventsLocked();
    int CollectOneLocked();
    void DrainAndCollectLocked();
    int PickSlotLocked();
    bool SetCodecApiLocked(const GUID& key, const char* name, LONG value);
    void DumpCodecApiLocked();
    void ApplyRateControlLocked(const char* phase);
    void ReadRateControlLocked(const char* phase);
    bool InitMftLocked(ID3D11Device* device, std::string* err);
    bool InitColorLocked(ID3D11Device* device, std::string* err);
    void PumpLoop();
};

// ---------------------------------------------------------------- MFT 事件

int MftEncoder::Impl::DrainEventsLocked() {
    int ready = 0;
    for (;;) {
        IMFMediaEvent* ev = nullptr;
        if (evg->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev) != S_OK) break;
        MediaEventType t = MEUnknown;
        ev->GetType(&t);
        ev->Release();
        if (t == METransformNeedInput) credits++;
        else if (t == METransformHaveOutput) ready++;
    }
    return ready;
}

int MftEncoder::Impl::CollectOneLocked() {
    ComPtr<IMFSample> owned;
    MFT_OUTPUT_DATA_BUFFER odb{};
    odb.dwStreamID = 0;

    if (!mftAllocates) {
        if (FAILED(MFCreateSample(owned.GetAddressOf()))) return -1;
        ComPtr<IMFMediaBuffer> ob;
        if (FAILED(MFCreateMemoryBuffer(outCap, ob.GetAddressOf()))) return -1;
        owned->AddBuffer(ob.Get());
        odb.pSample = owned.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
    if (odb.pEvents) { odb.pEvents->Release(); odb.pEvents = nullptr; }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
    if (hr == MF_E_BUFFERTOOSMALL) {
        // 宣告的 cbSize 不是承諾：720p 的 IDR 就可能好幾百 KB。放大重試，
        // 而不是丟掉這一幀。
        bufferTooSmall++;
        outCap *= 4;
        return 0;
    }
    if (FAILED(hr)) {
        lastHr = hr;
        return -1;
    }

    LONGLONG ts = 0;
    if (odb.pSample) odb.pSample->GetSampleTime(&ts);

    UINT32 clean = 0;
    bool key = false;
    if (odb.pSample &&
        SUCCEEDED(odb.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean))) {
        key = clean != 0;
    }

    ComPtr<IMFMediaBuffer> ob;
    if (!odb.pSample || FAILED(odb.pSample->ConvertToContiguousBuffer(ob.GetAddressOf()))) {
        lastHr = E_FAIL;
        return -1;
    }
    BYTE* p = nullptr;
    DWORD cur = 0;
    if (FAILED(ob->Lock(&p, nullptr, &cur))) {
        lastHr = E_FAIL;
        return -1;
    }
    if (t0 < 0) t0 = (int64_t)ts;
    const int64_t pts = (int64_t)ts - t0;

    std::string merr;
    if (!mux.WriteSample(p, cur, pts, lastInputDur, key, &merr)) {
        lastError = merr;
        ob->Unlock();
        return -1;
    }
    ob->Unlock();
    encoded++;
    return 1;
}

void MftEncoder::Impl::DrainAndCollectLocked() {
    // 迴圈 drain -> collect -> drain。單趟會讓管線每**兩**個回呼才前進一格：
    // ProcessOutput 釋放 slot 之後 MFT 才補上 METransformNeedInput，所以
    // 只做一趟的話，一半的幀會因為沒有 credit 被丟掉，而且不報任何錯。
    for (int pass = 0; pass < 4; ++pass) {
        const int ready = DrainEventsLocked();
        if (ready <= 0) break;
        int done = 0;
        for (int i = 0; i < ready; ++i) {
            const int r = CollectOneLocked();
            if (r < 0) { encodeErrors++; break; }
            if (r > 0) { outputs++; done++; }
        }
        if (done == 0) break;
    }
}

int MftEncoder::Impl::PickSlotLocked() {
    for (int i = 0; i < kRing; ++i) {
        if (slotSeq[i] < 0 || outputs.load() >= slotSeq[i] + 1) return i;
    }
    return -1;
}

// --------------------------------------------------------------- ICodecAPI

bool MftEncoder::Impl::SetCodecApiLocked(const GUID& key, const char* name, LONG value) {
    if (!codec) return false;
    VARIANT v;
    VariantInit(&v);
    // CODECAPI 的數值屬性都是 VT_UI4。用 VT_I4 送過去，SetValue 照樣回 S_OK，
    // 但值會被靜默丟掉（wgcenc 實測：MeanBitRate 的 readback 變 VT_EMPTY）。
    v.vt = VT_UI4;
    v.ulVal = (ULONG)value;
    const HRESULT hr = codec->SetValue(&key, &v);

    VARIANT rb;
    VariantInit(&rb);
    const HRESULT gr = codec->GetValue(&key, &rb);
    long long got = -1;
    if (SUCCEEDED(gr)) {
        if (rb.vt == VT_UI4) got = (long long)rb.ulVal;
        else if (rb.vt == VT_I4) got = (long long)rb.lVal;
    }
    VariantClear(&rb);

    fprintf(stderr, "[mft] codecapi %-22s = %-8ld %s (readback=%lld)\n", name, value,
            SUCCEEDED(hr) ? "ok" : "REJECTED", got);
    return SUCCEEDED(hr) && got == (long long)value;
}

void MftEncoder::Impl::DumpCodecApiLocked() {
    if (!codec) return;
    struct Item { const GUID* g; const char* name; };
    const Item items[] = {
        {&CODECAPI_AVEncCommonRateControlMode, "RateControlMode"},
        {&CODECAPI_AVEncCommonMeanBitRate,     "MeanBitRate"},
        {&CODECAPI_AVEncCommonQuality,         "Quality"},
        {&CODECAPI_AVEncCommonQualityVsSpeed,  "QualityVsSpeed"},
        {&CODECAPI_AVEncMPVGOPSize,            "GOPSize"},
        {&CODECAPI_AVEncMPVDefaultBPictureCount, "BPictureCount"},
        {&CODECAPI_AVEncVideoMaxNumRefFrame,   "MaxNumRefFrame"},
        {&CODECAPI_AVEncCommonLowLatency,      "LowLatency"},
        {&CODECAPI_AVEncAdaptiveMode,          "AdaptiveMode"},
        // Quality 模式走不通時的另一條路：直接釘住 QP（等於 constant-QP 編碼）。
        // 這才是「quality 這個旋鈕真的有用」的證據所在。
        {&CODECAPI_AVEncVideoEncodeQP,         "EncodeQP"},
        {&CODECAPI_AVEncVideoMinQP,            "MinQP"},
        {&CODECAPI_AVEncVideoMaxQP,            "MaxQP"},
        {&CODECAPI_AVEncMPVSceneDetection,     "SceneDetection"},
        {&CODECAPI_AVEncH264CABACEnable,       "CABAC"},
    };
    for (const Item& it : items) {
        const HRESULT sup = codec->IsSupported(it.g);
        const HRESULT mod = codec->IsModifiable(it.g);
        VARIANT cur; VariantInit(&cur);
        const HRESULT gv = codec->GetValue(it.g, &cur);
        long long got = -1;
        if (SUCCEEDED(gv)) {
            if (cur.vt == VT_UI4) got = (long long)cur.ulVal;
            else if (cur.vt == VT_I4) got = (long long)cur.lVal;
        }
        VariantClear(&cur);
        fprintf(stderr, "[mft] codecapi %-16s supported=%-3s modifiable=%-3s current=%lld\n",
                it.name,
                sup == S_OK ? "yes" : (sup == S_FALSE ? "no" : "err"),
                mod == S_OK ? "yes" : (mod == S_FALSE ? "no" : "err"),
                got);
    }
}

// ------------------------------------------------------------------ colour

void MftEncoder::Impl::ApplyRateControlLocked(const char* phase) {
    if (!codec) return;
    fprintf(stderr, "[mft] --- rate control apply (%s)\n", phase);

    // ---- 常數 QP：真的固定量化步長
    //
    // 為什麼不沿用 Quality 模式再釘 MinQP/MaxQP：那個組合實測不單調
    // （NVIDIA H.264 MFT，UI 內容，同一批幀：QP18 → 51.19 dB，QP17 → 54.03 dB，
    //  QP16 → 50.19 dB）。原因是 Quality 模式底下 MFT 會自己挑內部 preset，
    //  preset 一換，同樣的 QP 就換到另一個 rate-PSNR 點上。
    //
    // UnconstrainedVBR + MinQP=MaxQP 把 MFT 逼進「只有 QP 能動」的角落：位元率
    // 上限拉到不會觸發，QP 上下界收成一點，剩下能變的只有量化步長。
    if (cfg.rcMode == RcMode::ConstantQp) {
        const int qpMax = (cfg.codec == Codec::AV1) ? 63 : 51;
        int qp = cfg.qp;
        if (qp <= 0) {
            const double slope = (cfg.codec == Codec::AV1)  ? 0.62
                               : (cfg.codec == Codec::HEVC) ? 0.44
                                                            : 0.50;
            qp = qpMax - (int)std::lround(std::clamp(cfg.quality, 0, 100) * slope);
        }
        qp = std::clamp(qp, 1, qpMax);
        // q100 的參考檔永遠 QP 1（理由同下面 Quality 分支的註解：尺本身要有損
        // 就量不到真值）。
        if (cfg.quality >= 99) qp = 1;
        SetCodecApiLocked(CODECAPI_AVEncCommonRateControlMode,
                          "RateControlMode(VBR)", eAVEncCommonRateControlMode_UnconstrainedVBR);
        // 位元率上限拉到 200 Mbps：1296x696@60 的內容永遠碰不到，等於關掉它。
        SetCodecApiLocked(CODECAPI_AVEncCommonMeanBitRate, "MeanBitRate", 200000000);
        SetCodecApiLocked(CODECAPI_AVEncVideoMinQP, "MinQP", qp);
        SetCodecApiLocked(CODECAPI_AVEncVideoMaxQP, "MaxQP", qp);
        SetCodecApiLocked(CODECAPI_AVEncMPVGOPSize, "GOPSize",
                          cfg.nominalFps * std::max(1, cfg.gopSeconds));
        SetCodecApiLocked(CODECAPI_AVEncMPVDefaultBPictureCount, "BPictureCount", cfg.bFrames);
        SetCodecApiLocked(CODECAPI_AVEncVideoMaxNumRefFrame, "MaxNumRefFrame", 3);
        fprintf(stderr, "[mft] constant-QP mode: QP=%d (位元率上限已拉高到不觸發)\n", qp);
        return;
    }

    SetCodecApiLocked(CODECAPI_AVEncCommonRateControlMode, "RateControlMode(Quality)",
                      eAVEncCommonRateControlMode_Quality);
    SetCodecApiLocked(CODECAPI_AVEncCommonQuality, "Quality",
                      std::clamp(cfg.quality, 0, 100));
    SetCodecApiLocked(CODECAPI_AVEncMPVGOPSize, "GOPSize",
                      cfg.nominalFps * std::max(1, cfg.gopSeconds));
    SetCodecApiLocked(CODECAPI_AVEncMPVDefaultBPictureCount, "BPictureCount", cfg.bFrames);
    SetCodecApiLocked(CODECAPI_AVEncVideoMaxNumRefFrame, "MaxNumRefFrame", 3);
    SetCodecApiLocked(CODECAPI_AVEncCommonLowLatency, "LowLatency", 0);
    // NVENC 的 speed/quality preset。實測這張 MFT 的預設值是 33（滿分 100），
    // 也就是「偏快」那一側 —— 螢幕錄影不在乎延遲，同一個位元率換畫質才是重點，
    // 所以推到 100。
    //
    // 注意這不是「品質」旋鈕，是「同一份碼率預算下願意花多少算力做 RD 決策」。
    // 它和 Quality/QP 是兩個獨立維度：前者影響 RD 最佳化，後者影響量化步長。
    SetCodecApiLocked(CODECAPI_AVEncCommonQualityVsSpeed, "QualityVsSpeed", 100);
    // quality → QP 的單調映射（H.264/HEVC 都是 0..51，越小越好）。
    //
    // 上界刻意停在 1 而不是 0：QP 0 會被這張 MFT 當成「未設定」而回退到預設值，
    // 症狀是 quality=100 的檔案比 quality=85 還小（實測 82KB vs 91KB）——
    // 一個看起來像「品質旋鈕壞了」的結果，其實是邊界值。少 1 個 QP 的差別
    // 肉眼不可見。
    //
    // ★ 兩個 codec 不能用同一條直線。同一個 QP 對 HEVC 比對 H.264 保守得多：
    // 1280x720 高熵內容、同一批幀、QP 都 26，HEVC 45.3 dB / H.264 42.2 dB，
    // 而 HEVC 只多花 6% 位元組就換到 3 dB。共用一條線的結果是「HEVC 檔案比
    // H.264 大 6~11%」——不是編碼器差，是兩個旋鈕的刻度不一樣。
    //
    // 把 HEVC 的斜率做陡一點（0.44 vs 0.50），同一個 --quality 就落在相近的
    // 畫質上，HEVC 的大小優勢才表現得出來（實測同等 PSNR 下 63~71%）。
    const double q = std::clamp(cfg.quality, 0, 100);
    // AV1 的 QP 上界是 63，不是 51。硬套 H.264/HEVC 的 0..51 會讓 AV1 的最高 QP
    // 只有 51，等於整個「低碼率」區間被截掉 —— 檔案偏大但畫質看起來沒問題，
    // 是最不容易被發現的那種錯。
    const int qpMax = (cfg.codec == Codec::AV1) ? 63 : 51;
    const double slope = (cfg.codec == Codec::AV1)  ? 0.62
                       : (cfg.codec == Codec::HEVC) ? 0.44
                                                    : 0.50;
    int qp = qpMax - (int)std::lround(q * slope);
    // 近無損：不要讓斜率差異把頂端推離，quality=100 三個 codec 都給 QP 1。
    if (q >= 99.0) qp = 1;
    qp = std::clamp(qp, 1, qpMax);
    // q100 的「近無損參考檔」永遠釘死 QP 1，不受 pinQp 影響。
    //
    // 理由：pinQp 這個開關是為了讓 --quality 變成可重現的量化步長，那是「掃曲線」
    // 用的。參考檔不是曲線上的一點，它是量測的尺 —— 尺本身有損，PSNR 就被壓在
    // 一個假的天花板上（實測：參考檔跟著不釘 QP 時只剩 325 KB，同一批 q65 量到
    // 47.57 dB，而釘死 QP 1 的參考檔是 1126 KB、同一個 q65 是 51.19 dB）。
    // 兩組數字不能互相比，也不能拿去跟舊資料併。
    const bool pin = cfg.pinQp || q >= 99.0;
    if (pin) {
        SetCodecApiLocked(CODECAPI_AVEncVideoMinQP, "MinQP", qp);
        SetCodecApiLocked(CODECAPI_AVEncVideoMaxQP, "MaxQP", qp);
    } else {
        // 對照組：只給 Quality，讓 MFT 自己決定每一幀的 QP。
        // 用來分辨「曲線非單調」是釘死 QP 造成的，還是 Quality 旋鈕本身如此。
        fprintf(stderr, "[mft] pinQp=0 -> MinQP/MaxQP 不設定，只給 Quality=%d\n",
                (int)q);
    }
}

void MftEncoder::Impl::ReadRateControlLocked(const char* phase) {
    if (!codec) return;
    auto num = [&](const GUID& g) -> long long {
        VARIANT v;
        VariantInit(&v);
        long long r = -1;
        if (SUCCEEDED(codec->GetValue(&g, &v))) {
            if (v.vt == VT_UI4) r = (long long)v.ulVal;
            else if (v.vt == VT_I4) r = (long long)v.lVal;
        }
        VariantClear(&v);
        return r;
    };
    const long long mode = num(CODECAPI_AVEncCommonRateControlMode);
    const long long q = num(CODECAPI_AVEncCommonQuality);
    const long long lo = num(CODECAPI_AVEncVideoMinQP);
    const long long hi = num(CODECAPI_AVEncVideoMaxQP);

    qualityMode = (mode == eAVEncCommonRateControlMode_Quality);
    qpPinned = (lo == hi && lo >= 0);
    fprintf(stderr,
            "[mft] rate control readback (%s): mode=%lld quality=%lld qp=[%lld..%lld] -> %s\n",
            phase, mode, q, lo, hi,
            qualityMode ? "QUALITY MODE"
                        : (qpPinned ? "constant-QP" : "NEITHER (bitrate is the only knob)"));
}

// ------------------------------------------------------------------ colour

bool MftEncoder::Impl::InitColorLocked(ID3D11Device* device, std::string* err) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)cfg.width;
    td.Height = (UINT)cfg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < kRing; ++i) {
        HRESULT hr = device->CreateTexture2D(&td, nullptr, bgra[i].GetAddressOf());
        if (FAILED(hr)) return fail(err, "CreateTexture2D(bgra)", hr);
    }

    D3D11_TEXTURE2D_DESC nd = td;
    nd.Format = DXGI_FORMAT_NV12;
    nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < kRing; ++i) {
        HRESULT hr = device->CreateTexture2D(&nd, nullptr, nv12[i].GetAddressOf());
        if (FAILED(hr)) return fail(err, "CreateTexture2D(nv12)", hr);
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(videoDev.GetAddressOf()));
    if (FAILED(hr)) return fail(err, "QueryInterface(ID3D11VideoDevice)", hr);
    hr = ctx->QueryInterface(IID_PPV_ARGS(videoCtx.GetAddressOf()));
    if (FAILED(hr)) return fail(err, "QueryInterface(ID3D11VideoContext)", hr);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = (UINT)cfg.width;
    cd.InputHeight = (UINT)cfg.height;
    cd.OutputWidth = (UINT)cfg.width;
    cd.OutputHeight = (UINT)cfg.height;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDev->CreateVideoProcessorEnumerator(&cd, vpEnum.GetAddressOf());
    if (FAILED(hr)) return fail(err, "CreateVideoProcessorEnumerator", hr);
    hr = videoDev->CreateVideoProcessor(vpEnum.Get(), 0, vp.GetAddressOf());
    if (FAILED(hr)) return fail(err, "CreateVideoProcessor", hr);

    // 桌面內容是 sRGB full range，編碼用 BT.709 limited。標錯會讓播放器多做
    // 一次 range 轉換，畫面發灰 —— 而且是「看起來只是不夠漂亮」那種錯，
    // 不會有人回報。
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.Usage = 0;
    inCs.RGB_Range = 0;              // full
    inCs.YCbCr_Matrix = 1;           // BT.709
    inCs.YCbCr_xvYCC = 0;
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoCtx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = inCs;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoCtx->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;
    for (int i = 0; i < kRing; ++i) {
        hr = videoDev->CreateVideoProcessorOutputView(nv12[i].Get(), vpEnum.Get(), &ovd,
                                                      outView[i].GetAddressOf());
        if (FAILED(hr)) return fail(err, "CreateVideoProcessorOutputView", hr);
        hr = videoDev->CreateVideoProcessorInputView(bgra[i].Get(), vpEnum.Get(), &ivd,
                                                     inView[i].GetAddressOf());
        if (FAILED(hr)) return fail(err, "CreateVideoProcessorInputView", hr);
    }
    return true;
}

// --------------------------------------------------------------------- MFT

bool MftEncoder::Impl::InitMftLocked(ID3D11Device* device, std::string* err) {
    const GUID& outSub = SubtypeFor(cfg.codec);

    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, outSub};
    MFT_REGISTER_TYPE_INFO inNv12{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO inArgb{MFMediaType_Video, MFVideoFormat_ARGB32};

    IMFActivate** acts = nullptr;
    UINT32 nActs = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           &inNv12, &outInfo, &acts, &nActs);
    if (FAILED(hr) || nActs == 0) {
        if (acts) { CoTaskMemFree(acts); acts = nullptr; }
        nActs = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                       &inArgb, &outInfo, &acts, &nActs);
    }
    if (FAILED(hr) || nActs == 0) {
        if (acts) { CoTaskMemFree(acts); acts = nullptr; }
        nActs = 0;
        // 最後再試一次「不篩輸入格式」：有些 MFT 在註冊表裡只宣告輸出。
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &outInfo, &acts, &nActs);
    }
    if (FAILED(hr)) return fail(err, "MFTEnumEx", hr);
    if (nActs == 0) {
        if (err) *err = std::string("no hardware encoder MFT for ") + CodecStr(cfg.codec);
        return false;
    }

    hr = acts[0]->ActivateObject(__uuidof(IMFTransform), (void**)mft.GetAddressOf());
    for (UINT32 i = 0; i < nActs; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return fail(err, "ActivateObject", hr);

    // async MFT 在解鎖之前拒絕跑，而事件介面不在 IMFTransform 上，要另外 QI。
    ComPtr<IMFAttributes> mattr;
    hr = mft->GetAttributes(mattr.GetAddressOf());
    if (FAILED(hr)) return fail(err, "GetAttributes", hr);
    hr = mattr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) return fail(err, "MF_TRANSFORM_ASYNC_UNLOCK", hr);
    hr = mft->QueryInterface(IID_PPV_ARGS(evg.GetAddressOf()));
    if (FAILED(hr)) return fail(err, "QueryInterface(IMFMediaEventGenerator)", hr);

    UINT resetToken = 0;
    hr = MFCreateDXGIDeviceManager(&resetToken, mgr.GetAddressOf());
    if (FAILED(hr)) return fail(err, "MFCreateDXGIDeviceManager", hr);
    hr = mgr->ResetDevice(device, resetToken);
    if (FAILED(hr)) return fail(err, "ResetDevice", hr);
    // 順序有意義：encoder 依「有沒有 D3D manager」決定要不要配置內部 surface，
    // 所以這一步必須在設型別之前。
    hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr.Get());
    if (FAILED(hr)) return fail(err, "MFT_MESSAGE_SET_D3D_MANAGER", hr);

    // ---- output type
    const int bitrate = cfg.bitrate > 0 ? cfg.bitrate
                                        : AutoBitrate(cfg.width, cfg.height, cfg.nominalFps);

    // ★ 碼控要在 SetOutputType **之前**設。
    //
    //   MF 的 MFT 常常在型別協商時把 CODECAPI 的值快照進內部設定，之後的寫入
    //   只更新外殼、不影響已經配置好的 encoder。M1 用 SinkWriter 時看不到這件事
    //   （SinkWriter 把 encoder 藏在後面），自己持有 IMFTransform 才發現順序有差。
    //   這裡先設一次，型別協商完再讀回來確認 —— 兩次都印，因為「哪一次生效」
    //   是這個檔案裡最值錢的一條資訊。
    hr = mft->QueryInterface(IID_PPV_ARGS(codec.GetAddressOf()));
    if (FAILED(hr)) codec.Reset();
    if (codec) {
        DumpCodecApiLocked();
        ApplyRateControlLocked("pre-type");
    }

    ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(outType.GetAddressOf());
    if (FAILED(hr)) return fail(err, "MFCreateMediaType(out)", hr);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, outSub);
    outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    const UINT32 profile = ProfileFor(cfg.codec);
    if (profile) outType->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, (UINT32)cfg.width, (UINT32)cfg.height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, (UINT32)cfg.nominalFps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) return fail(err, "SetOutputType", hr);

    // ---- input type：NV12 優先（VideoProcessor 已做過 BT.709），退 ARGB32
    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(inType.GetAddressOf());
    if (FAILED(hr)) return fail(err, "MFCreateMediaType(in)", hr);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, (UINT32)cfg.width, (UINT32)cfg.height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, (UINT32)cfg.nominalFps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    useNv12 = true;
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[mft] NV12 input rejected (%s), trying ARGB32\n", HrStr(hr).c_str());
        useNv12 = false;
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        hr = mft->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) return fail(err, "SetInputType(NV12/ARGB32)", hr);
    }

    // ---- 型別協商之後再確認一次（見上面 pre-type 的說明）
    if (codec) {
        ReadRateControlLocked("post-type");
        if (!qualityMode && !qpPinned) {
            ApplyRateControlLocked("post-type retry");
            ReadRateControlLocked("post-type retry");
        }
    }

    // ---- SPS/PPS：MF 通常會給 out-of-band 的 config record，muxer 直接用它
    std::vector<uint8_t> configBlob;
    {
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, cur.GetAddressOf())) && cur) {
            UINT32 len = 0;
            if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &len)) && len) {
                configBlob.resize(len);
                UINT32 got = 0;
                if (FAILED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, configBlob.data(), len, &got)) ||
                    got != len) {
                    configBlob.clear();
                } else if (cfg.codec != Codec::AV1 && configBlob[0] != 1) {
                    // Annex-B 形式（罕見）：交給 muxer 從 in-band 的 NAL 自己組。
                    //
                    // AV1 不走這條判斷：它的 config 是 OBU，開頭是 OBU header
                    // （sequence header 通常是 0x0A），不會是 avcC 的 version 1。
                    fprintf(stderr, "[mft] sequence header is not a config record "
                                    "(first byte 0x%02X); muxer will build it from "
                                    "in-band parameter sets\n", configBlob[0]);
                    configBlob.clear();
                }
            }
        }
        fprintf(stderr, "[mft] sequence header: %s (%zu bytes)\n",
                configBlob.empty() ? "none" : "out-of-band", configBlob.size());
    }

    hr = mft->GetOutputStreamInfo(0, &si);
    if (FAILED(hr)) return fail(err, "GetOutputStreamInfo", hr);
    mftAllocates = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    outCap = si.cbSize ? si.cbSize : (1u << 20);

    // ---- 檔頭 / muxer
    if (!mux.Open(path, cfg.codec, cfg.width, cfg.height, configBlob, err, cfg.fragmented,
                  cfg.audioTracks.empty() ? nullptr : &cfg.audioTracks)) {
        return false;
    }

    hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return fail(err, "NOTIFY_BEGIN_STREAMING", hr);
    hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return fail(err, "NOTIFY_START_OF_STREAM", hr);
    began = true;

    // 編碼器名稱
    {
        LPWSTR nm = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(mattr->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len))) {
            encoderName = Narrow(nm);
            CoTaskMemFree(nm);
        }
    }

    fprintf(stderr, "[mft] %s [%s] %dx%d @%dfps q%d gop=%df bframes=%d input=%s "
                    "mftAllocates=%s cbSize=%lu\n",
            codecName.c_str(), encoderName.c_str(), cfg.width, cfg.height,
            cfg.nominalFps, cfg.quality,
            cfg.nominalFps * std::max(1, cfg.gopSeconds), cfg.bFrames,
            useNv12 ? "NV12" : "ARGB32", mftAllocates ? "yes" : "no",
            (unsigned long)si.cbSize);
    return true;
}

// -------------------------------------------------------------------- pump

void MftEncoder::Impl::PumpLoop() {
    while (!stop.load()) {
        {
            std::lock_guard<std::mutex> lk(mu);
            DrainAndCollectLocked();
        }
        Sleep(5);
    }
}

// ============================================================================

MftEncoder::MftEncoder() : impl_(std::make_unique<Impl>()) {}
MftEncoder::~MftEncoder() { Close(nullptr); }

bool MftEncoder::Open(const std::wstring& path, const EncoderConfig& config,
                      ID3D11Device* device, std::string* err) {
    if (!device) { if (err) *err = "null device"; return false; }
    auto& I = *impl_;

    I.cfg = config;
    I.path = path;
    I.codecName = CodecStr(config.codec);

    // H.264/HEVC 的 4:2:0 要求偶數尺寸。少一行像素肉眼不可見，比讓 encoder
    // 拒絕好。
    I.cfg.width = config.width & ~1;
    I.cfg.height = config.height & ~1;
    if (I.cfg.width <= 0 || I.cfg.height <= 0) {
        if (err) *err = "invalid output size";
        return false;
    }

    I.dev = device;
    device->GetImmediateContext(I.ctx.GetAddressOf());
    if (!I.ctx) { if (err) *err = "no immediate context"; return false; }

    if (!I.InitColorLocked(device, err)) return false;
    if (!I.InitMftLocked(device, err)) return false;

    I.stop.store(false);
    I.pump = std::thread([&I] { I.PumpLoop(); });
    I.opened = true;
    return true;
}

bool MftEncoder::Write(ID3D11Texture2D* src, int64_t pts100ns, int64_t dur100ns,
                       std::string* err) {
    auto& I = *impl_;
    if (!I.opened || !src) { if (err) *err = "not open / null texture"; return false; }

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
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

    std::lock_guard<std::mutex> lk(I.mu);

    // 先把 MFT 已經產出的收乾淨，再送新的。這一步同時是「補 credit」的來源。
    I.DrainAndCollectLocked();

    const int slot = I.PickSlotLocked();
    if (slot < 0) { I.droppedNoSlot++; return true; }
    if (I.credits <= 0) { I.droppedNoCredit++; return true; }

    // crop 到 ring[slot]
    if (I.cfg.cropX == 0 && I.cfg.cropY == 0 &&
        (int)sd.Width == I.cfg.width && (int)sd.Height == I.cfg.height) {
        I.ctx->CopyResource(I.bgra[slot].Get(), src);
    } else {
        D3D11_BOX box{};
        box.left = (UINT)I.cfg.cropX;
        box.top = (UINT)I.cfg.cropY;
        box.front = 0;
        box.right = (UINT)(I.cfg.cropX + I.cfg.width);
        box.bottom = (UINT)(I.cfg.cropY + I.cfg.height);
        box.back = 1;
        I.ctx->CopySubresourceRegion(I.bgra[slot].Get(), 0, 0, 0, 0, src, 0, &box);
    }

    ID3D11Texture2D* feed = I.bgra[slot].Get();
    if (I.useNv12) {
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = I.inView[slot].Get();
        HRESULT vhr = I.videoCtx->VideoProcessorBlt(I.vp.Get(), I.outView[slot].Get(), 0, 1, &st);
        if (FAILED(vhr)) return I.fail(err, "VideoProcessorBlt", vhr);
        feed = I.nv12[slot].Get();
    }

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), feed, 0, FALSE,
                                           buf.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateDXGISurfaceBuffer", hr);

    ComPtr<IMF2DBuffer> b2d;
    if (SUCCEEDED(buf.As(&b2d)) && b2d) {
        DWORD clen = 0;
        if (SUCCEEDED(b2d->GetContiguousLength(&clen))) buf->SetCurrentLength(clen);
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr)) return I.fail(err, "MFCreateSample", hr);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime((LONGLONG)pts100ns);
    sample->SetSampleDuration((LONGLONG)(dur100ns > 0 ? dur100ns : (10000000LL / I.cfg.nominalFps)));

    const long long seq = I.submitted;
    hr = I.mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // async MFT 說「我還沒準備好」。再排空一次（會補回 credit）然後重試一次；
        // 直接丟掉會變成靜默的掉幀。
        I.processInputRetries++;
        I.DrainAndCollectLocked();
        hr = I.mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return I.fail(err, "ProcessInput", hr);

    I.credits--;
    I.slotSeq[slot] = seq;
    I.submitted++;
    I.lastInputDur = dur100ns;
    return true;
}

// 音訊是另一條執行緒送進來的（WASAPI 的回呼），而 mux 不是執行緒安全的。
// 這裡沿用影像路徑的 mu —— 它是唯一同時保護 MFT 與 mux 的鎖。
//
// 多條音源各自在自己的執行緒上呼叫，track 索引由呼叫端決定（0 起算），
// 同一把鎖讓多條軌的 AU 不會在 muxer 裡互相踩。
bool MftEncoder::WriteAudio(int track, const uint8_t* data, size_t size, uint32_t frames,
                            int64_t pts100ns, std::string* err) {
    auto& I = *impl_;
    if (!I.opened) { if (err) *err = "not open"; return false; }
    std::lock_guard<std::mutex> lk(I.mu);
    return I.mux.WriteAudioSample(track, data, size, frames, pts100ns, err);
}

int MftEncoder::audioTrackCount() const { return (int)impl_->cfg.audioTracks.size(); }
uint64_t MftEncoder::audioSamples() const { return impl_->mux.audioSamples(); }

bool MftEncoder::SetAudioOffset(int track, int64_t offset100ns) {
    auto& I = *impl_;
    if (!I.opened) return false;
    std::lock_guard<std::mutex> lk(I.mu);
    return I.mux.SetAudioStartOffset(track, offset100ns, nullptr);
}

bool MftEncoder::Close(std::string* err) {
    auto& I = *impl_;
    if (!I.opened) return true;
    I.opened = false;

    I.stop.store(true);
    if (I.pump.joinable()) I.pump.join();

    // 把 MFT 還握著的 flush 出來。50 × 10ms 是上限，正常 1-2 次就結束。
    for (int i = 0; i < 50; ++i) {
        std::lock_guard<std::mutex> lk(I.mu);
        const long long before = I.encoded;
        I.DrainAndCollectLocked();
        if (I.encoded == before && i > 3) break;
        if (I.encoded == before) {
            // 沒有新東西：給 MFT 一點時間把最後一幀吐出來
        }
        Sleep(10);
    }

    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (I.mft) {
            I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }

    // moov 在這裡寫出去。之前的所有 sample 都只有 mdat —— 這一刻之前檔是不可播的。
    return I.mux.Close(err);
}

uint64_t MftEncoder::bytes() const {
    // 這條會被主迴圈每秒呼叫一次，而 mux 的 FILE* 可能正被影像或音訊執行緒寫。
    // 不加鎖就是在別人的 fwrite 中間做 _ftelli64 —— 檔案位置會亂掉。
    std::lock_guard<std::mutex> lk(impl_->mu);
    const uint64_t muxBytes = impl_->mux.bytes();
    if (muxBytes > 0) return muxBytes;
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

uint64_t MftEncoder::samples() const { return (uint64_t)impl_->encoded; }
std::string MftEncoder::codecName() const { return impl_->codecName; }
std::string MftEncoder::encoderName() const { return impl_->encoderName; }
bool MftEncoder::qualityModeActive() const { return impl_->qualityMode || impl_->qpPinned; }

std::string MftEncoder::stats() const {
    const auto& I = *impl_;
    char b[512];
    snprintf(b, sizeof(b),
             "submitted=%lld encoded=%lld out=%lld drop(credit=%lld slot=%lld) "
             "retry=%lld bufsmall=%lld err=%lld mux=%lluB qmode=%s",
             I.submitted, I.encoded, I.outputs.load(), I.droppedNoCredit, I.droppedNoSlot,
             I.processInputRetries, I.bufferTooSmall, I.encodeErrors,
             (unsigned long long)I.mux.payloadBytes(),
             I.qualityMode ? "on" : "OFF");
    return b;
}

} // namespace capwgc
