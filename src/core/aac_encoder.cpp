// ============================================================================
// aac_encoder.cpp — MF AAC encoder MFT
// ============================================================================

#include "aac_encoder.h"

#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace capwgc {

namespace {

std::string Hex(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 取樣率 → AudioSpecificConfig 的 samplingFrequencyIndex。
// 只在 MF_MT_USER_DATA 裡拿不到 ASC 時才用得到（備援路徑）。
int FreqIndex(int rate) {
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < (int)(sizeof(kRates) / sizeof(kRates[0])); ++i) {
        if (kRates[i] == rate) return i;
    }
    return -1;
}

} // namespace

struct AacEncoder::Impl {
    ComPtr<IMFTransform> mft;
    DWORD inId = 0;
    DWORD outId = 0;
    MFT_OUTPUT_STREAM_INFO osi{};
    std::vector<uint8_t> asc;
    int rate = 48000;
    int channels = 2;
    std::string name;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t pcmFrames = 0;
    bool started = false;

    bool fail(std::string* err, const std::string& m) {
        if (err) *err = m;
        return false;
    }

    // 把編碼器手上還沒吐完的 AU 全部收出來。
    bool Drain(PacketFn& fn, std::string* err) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = outId;

            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            const bool provides = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!provides) {
                if (FAILED(MFCreateSample(sample.GetAddressOf()))) {
                    return fail(err, "MFCreateSample failed");
                }
                if (FAILED(MFCreateMemoryBuffer(osi.cbSize ? osi.cbSize : 64 * 1024,
                                                buffer.GetAddressOf()))) {
                    return fail(err, "MFCreateMemoryBuffer failed");
                }
                sample->AddBuffer(buffer.Get());
                ob.pSample = sample.Get();
            }

            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);

            if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (provides && ob.pSample) ob.pSample->Release();
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (provides && ob.pSample) ob.pSample->Release();
                continue;
            }
            if (FAILED(hr)) {
                if (provides && ob.pSample) ob.pSample->Release();
                return fail(err, "ProcessOutput " + Hex(hr));
            }

            ComPtr<IMFSample> outSample;
            if (provides) {
                outSample.Attach(ob.pSample);
            } else {
                outSample = sample;
            }

            ComPtr<IMFMediaBuffer> cont;
            if (SUCCEEDED(outSample->ConvertToContiguousBuffer(cont.GetAddressOf()))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(cont->Lock(&p, nullptr, &len)) && len > 0) {
                    LONGLONG t = 0;
                    outSample->GetSampleTime(&t);
                    if (fn) fn(p, (size_t)len, (int64_t)t);
                    ++packets;
                    bytes += len;
                    cont->Unlock();
                }
            }

            if (status & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) continue;
        }
    }
};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() { Close(); }

bool AacEncoder::Open(int sampleRate, int channels, int bitrate, std::string* err) {
    auto& I = *impl_;
    I.rate = sampleRate;
    I.channels = channels;

    MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Audio, MFAudioFormat_PCM};
    MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Audio, MFAudioFormat_AAC};

    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SORTANDFILTER,
                           &inInfo, &outInfo, &acts, &count);
    if (FAILED(hr) || count == 0) {
        if (acts) CoTaskMemFree(acts);
        return I.fail(err, "no AAC encoder MFT (MFTEnumEx " + Hex(hr) + ")");
    }

    hr = acts[0]->ActivateObject(__uuidof(IMFTransform), (void**)I.mft.GetAddressOf());
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return I.fail(err, "ActivateObject " + Hex(hr));

    {
        IMFAttributes* attr = nullptr;
        if (SUCCEEDED(I.mft->GetAttributes(&attr)) && attr) {
            LPWSTR w = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(attr->GetString(MFT_FRIENDLY_NAME_Attribute, nullptr, 0, &len)) &&
                len > 0) {
                w = (LPWSTR)CoTaskMemAlloc(len * sizeof(wchar_t));
                if (w && SUCCEEDED(attr->GetString(MFT_FRIENDLY_NAME_Attribute, w, len, nullptr))) {
                    I.name = WideToUtf8(w);
                }
                if (w) CoTaskMemFree(w);
            }
            attr->Release();
        }
    }

    // 先設輸出型別再設輸入型別 —— encoder MFT 的慣例，反過來會回
    // MF_E_TRANSFORM_TYPE_NOT_SET 或更難懂的 MF_E_INVALIDMEDIATYPE。
    ComPtr<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(out.GetAddressOf()))) return I.fail(err, "MFCreateMediaType");
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(bitrate / 8));
    out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                  // raw，不含 ADTS
    out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC

    hr = I.mft->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) return I.fail(err, "SetOutputType(AAC) " + Hex(hr));

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(in.GetAddressOf()))) return I.fail(err, "MFCreateMediaType");
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(channels * 2));
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(sampleRate * channels * 2));

    hr = I.mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) return I.fail(err, "SetInputType(PCM) " + Hex(hr));

    // ---- AudioSpecificConfig
    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(I.mft->GetOutputCurrentType(0, cur.GetAddressOf()))) {
        UINT32 sz = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_USER_DATA, &sz)) && sz > 0) {
            std::vector<uint8_t> blob(sz);
            if (SUCCEEDED(cur->GetBlob(MF_MT_USER_DATA, blob.data(), sz, nullptr))) {
                // HEAACWAVEINFO 的尾部佔 12 bytes，ASC 接在後面。
                if (blob.size() > 12) {
                    I.asc.assign(blob.begin() + 12, blob.end());
                } else {
                    // 有些版本只回 12 bytes 的 HEAACWAVEINFO，沒有 ASC。
                    // AAC-LC 的 ASC 可以從取樣率與聲道數唯一決定，自己拼。
                    const int fi = FreqIndex(sampleRate);
                    if (fi >= 0 && channels >= 1 && channels <= 7) {
                        const uint16_t v = (uint16_t)((2 << 11) | (fi << 7) |
                                                      ((channels & 0x0F) << 3));
                        I.asc = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
                    }
                }
                fprintf(stderr, "[aac] MF_MT_USER_DATA %u bytes -> ASC %zu bytes%s\n",
                        sz, I.asc.size(), blob.size() > 12 ? "" : " (derived from rate/ch)");
            }
        }
    }
    if (I.asc.empty()) return I.fail(err, "cannot determine AudioSpecificConfig");

    hr = I.mft->GetOutputStreamInfo(0, &I.osi);
    if (FAILED(hr)) return I.fail(err, "GetOutputStreamInfo " + Hex(hr));

    I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    I.started = true;
    return true;
}

void AacEncoder::Close() {
    auto& I = *impl_;
    if (I.mft) {
        I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        I.mft.Reset();
    }
    I.started = false;
}

bool AacEncoder::Encode(const int16_t* pcm, size_t frames, int64_t pts100ns, PacketFn fn,
                        std::string* err) {
    auto& I = *impl_;
    if (!I.mft || !I.started) return I.fail(err, "encoder not open");
    if (!pcm || frames == 0) return true;

    const size_t bytes = frames * (size_t)I.channels * sizeof(int16_t);

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer((DWORD)bytes, buf.GetAddressOf()))) {
        return I.fail(err, "MFCreateMemoryBuffer failed");
    }
    BYTE* p = nullptr;
    if (FAILED(buf->Lock(&p, nullptr, nullptr))) return I.fail(err, "buffer lock failed");
    memcpy(p, pcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)bytes);

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.GetAddressOf()))) return I.fail(err, "MFCreateSample failed");
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration((LONGLONG)((frames * 10000000ull) / (uint64_t)I.rate));

    HRESULT hr = I.mft->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) return I.fail(err, "ProcessInput " + Hex(hr));

    I.pcmFrames += frames;
    return I.Drain(fn, err);
}

bool AacEncoder::Flush(int64_t targetFrames, PacketFn fn, std::string* err) {
    auto& I = *impl_;
    if (!I.mft || !I.started) return true;

    int64_t want = targetFrames > I.pcmFrames ? targetFrames : I.pcmFrames;
    const int64_t rem = want % 1024;
    if (rem != 0) want += 1024 - rem;

    std::vector<int16_t> silence(1024 * (size_t)(I.channels > 0 ? I.channels : 2), 0);
    while (I.pcmFrames < want) {
        const int64_t left = want - I.pcmFrames;
        const size_t n = (size_t)(left < 1024 ? left : 1024);
        const int64_t pts = (I.pcmFrames * 10000000ll) / (I.rate > 0 ? I.rate : 48000);
        if (!Encode(silence.data(), n, pts, fn, err)) return false;
    }

    I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    return I.Drain(fn, err);
}

const std::vector<uint8_t>& AacEncoder::config() const { return impl_->asc; }
int AacEncoder::sampleRate() const { return impl_->rate; }
int AacEncoder::channels() const { return impl_->channels; }
std::string AacEncoder::name() const { return impl_->name; }
uint64_t AacEncoder::packets() const { return impl_->packets; }

std::string AacEncoder::stats() const {
    char b[192];
    snprintf(b, sizeof(b), "pcm=%.2fs au=%llu bytes=%llu asc=%zuB",
             impl_->rate > 0 ? (double)impl_->pcmFrames / impl_->rate : 0.0,
             (unsigned long long)impl_->packets,
             (unsigned long long)impl_->bytes, impl_->asc.size());
    return b;
}

} // namespace capwgc
