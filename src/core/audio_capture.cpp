// ============================================================================
// audio_capture.cpp — WASAPI 擷取實作（loopback / 麥克風共用一條管線）
// ============================================================================

#include "audio_capture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <thread>
#include <vector>

#pragma comment(lib, "propsys.lib")

namespace capwgc {

namespace {

// 現在時刻，換成 100ns。跟 WGC 的 SystemRelativeTime 同一個時鐘，可以直接相減。
// 診斷用：把 packet 時間戳寫進 avsync.log（與 recorder 共用同一個檔）。
// 預設關閉，設 CAPWGC_AVSYNC_LOG=1 才開 —— 錄影路徑不該有常態性的寫檔。
void LogPkt(const char* fmt, ...) {
    static const bool on = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"CAPWGC_AVSYNC_LOG", v, 8) > 0 && v[0] != L'0';
    }();
    if (!on) return;
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring p = std::wstring(dir) + L"\\CapWGC\\avsync.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc(10, f);
    fclose(f);
}

int64_t Qpc100nsLocal() {
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? f.QuadPart : 1;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 10000000.0 / (double)freq);
}

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

// 只把 ASCII 大寫轉小寫。中文沒有大小寫，逐 byte 比較仍然正確。
std::string LowerAscii(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return o;
}

// 不想為了兩個 GUID 去 include ksmedia.h（它和 mmreg.h 的順序很挑）。
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010,
                      {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// PKEY_Device_FriendlyName。定義在 functiondiscoverykeys_devpkey.h，那個標頭要靠
// INITGUID 才生得出符號；這裡直接寫死 GUID，省掉一個編譯期開關。
const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string FriendlyName(IMMDevice* d) {
    if (!d) return {};
    IPropertyStore* ps = nullptr;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
        out = WideToUtf8(v.pwszVal);
    }
    PropVariantClear(&v);
    ps->Release();
    return out;
}

std::string EndpointId(IMMDevice* d) {
    if (!d) return {};
    LPWSTR id = nullptr;
    std::string out;
    if (SUCCEEDED(d->GetId(&id)) && id) {
        out = WideToUtf8(id);
        CoTaskMemFree(id);
    }
    return out;
}

EDataFlow FlowOf(AudioSourceKind k) {
    return k == AudioSourceKind::Input ? eCapture : eRender;
}

// 把 spec 解析成端點。spec 可以是完整 endpoint ID、數字索引（列舉順序）、
// 或 friendly name 的子字串。
IMMDevice* ResolveDevice(IMMDeviceEnumerator* enumr, AudioSourceKind kind,
                         const std::string& spec, std::string* err) {
    IMMDeviceCollection* col = nullptr;
    HRESULT hr = enumr->EnumAudioEndpoints(FlowOf(kind), DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col) {
        if (err) *err = "EnumAudioEndpoints " + Hex(hr);
        return nullptr;
    }

    UINT32 n = 0;
    col->GetCount(&n);

    IMMDevice* found = nullptr;
    const bool numeric = !spec.empty() &&
                         spec.find_first_not_of("0123456789") == std::string::npos;

    // 第一輪：完整 ID 或數字索引。
    if (numeric) {
        const UINT32 idx = (UINT32)strtoul(spec.c_str(), nullptr, 10);
        if (idx < n) col->Item(idx, &found);
    } else {
        for (UINT32 i = 0; i < n && !found; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            if (EndpointId(d) == spec) found = d;
            else d->Release();
        }
    }

    // 第二輪：名稱子字串。找不到唯一的就算了 —— 猜錯裝置比報錯更糟。
    if (!found && !numeric) {
        const std::string needle = LowerAscii(spec);
        IMMDevice* hit = nullptr;
        int hits = 0;
        for (UINT32 i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            const std::string name = LowerAscii(FriendlyName(d));
            if (!name.empty() && name.find(needle) != std::string::npos) {
                if (!hit) hit = d; else { ++hits; d->Release(); }
            } else {
                d->Release();
            }
        }
        if (hit && hits == 0) {
            found = hit;
        } else if (hit) {
            hit->Release();
            if (err) *err = "device spec \"" + spec + "\" matches " + std::to_string(hits + 1) +
                            " endpoints -- use the index from --list-audio-devices";
            col->Release();
            return nullptr;
        }
    }

    col->Release();
    if (!found && err) {
        *err = "no " + std::string(kind == AudioSourceKind::Input ? "capture" : "render") +
               " endpoint matches \"" + spec + "\"";
    }
    return found;
}

} // namespace

const char* AudioSourceKindName(AudioSourceKind k) {
    return k == AudioSourceKind::Input ? "mic" : "loopback";
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices(std::string* err) {
    std::vector<AudioDeviceInfo> out;

    IMMDeviceEnumerator* enumr = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumr);
    if (FAILED(hr) || !enumr) {
        if (err) *err = "MMDeviceEnumerator " + Hex(hr);
        return out;
    }

    for (int pass = 0; pass < 2; ++pass) {
        const EDataFlow flow = pass == 0 ? eRender : eCapture;

        std::string defId;
        {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(flow, eConsole, &d)) && d) {
                defId = EndpointId(d);
                d->Release();
            }
        }

        IMMDeviceCollection* col = nullptr;
        if (FAILED(enumr->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col) continue;

        UINT32 n = 0;
        col->GetCount(&n);
        for (UINT32 i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            AudioDeviceInfo info;
            info.render = (flow == eRender);
            info.id = EndpointId(d);
            info.name = FriendlyName(d);
            info.isDefault = !defId.empty() && info.id == defId;
            out.push_back(std::move(info));
            d->Release();
        }
        col->Release();
    }

    enumr->Release();
    return out;
}

struct AudioCapture::Impl {
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE evt = nullptr;

    // 靜音渲染串流（只有 loopback 需要）。用途見 Open() 裡的說明 —— 沒有它，
    // 系統安靜時 loopback 一個 packet 都不會送。
    IAudioClient* rclient = nullptr;
    IAudioRenderClient* rrender = nullptr;
    UINT32 rbufferFrames = 0;
    bool keepAlive = false;

    WAVEFORMATEX* wf = nullptr;      // CoTaskMemFree
    int srcChannels = 2;
    int outChannels = 2;
    bool isFloat = true;
    int bytesPerSample = 4;

    // 串流延遲（100ns）。這是「聲音被算出來」到「它出現在擷取緩衝區」之間的
    // 時間，也就是 loopback 天生的落後量。A/V 偏移的計算一定要扣掉它，否則
    // 會把「loopback 自己的延遲」誤當成「音訊比影像晚開始」。
    //
    // 不能靠 IAudioClient::GetStreamLatency：實測 loopback 回 0。所以改成
    // 「量第一次 Drain 拿到多少 frame」—— client 的緩衝區在 Initialize 之後
    // 就由音訊引擎持續填寫，Start() 當下裡面已經有一段先前的音訊，那一段的
    // 長度就是 pre-roll，也就是內容 t=0 比 Start() 早了多少。
    int64_t streamLatency100ns = 0;
    UINT32 capBufferFrames = 0;
    int64_t firstBurstFrames = -1;   // 第一次 Drain 拿到的 frame 數（= pre-roll）

    // 第一個 packet 的 QPC 時間戳（100ns）。這是把「音訊時間軸」對到
    // 「牆上時鐘」唯一的可靠方式 —— 見 Drain() 裡的說明。
    int64_t firstPacketQpc = 0;
    bool qpcIsRawCounts = false;     // 驅動回的是 QPC 原始計數而不是 100ns

    AudioSourceKind kind = AudioSourceKind::Loopback;
    AudioFormat fmt;
    std::string devName;
    std::string devId;
    std::string err;

    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<uint64_t> frames{0};
    PacketFn fn;
    std::vector<int16_t> conv;

    void Close() {
        if (client) client->Stop();
        stop.store(true);
        if (th.joinable()) th.join();
        running.store(false);

        if (rclient) rclient->Stop();
        if (rrender) { rrender->Release(); rrender = nullptr; }
        if (rclient) { rclient->Release(); rclient = nullptr; }
        if (evt) { CloseHandle(evt); evt = nullptr; }
        if (capture) { capture->Release(); capture = nullptr; }
        if (client) { client->Release(); client = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
        if (enumr) { enumr->Release(); enumr = nullptr; }
        if (wf) { CoTaskMemFree(wf); wf = nullptr; }
    }

    // 把裝置格式轉成 16-bit interleaved。float32 → 16-bit 的縮放用 32767，
    // 不用 32768：後者會讓 +1.0 溢位成 -32768，聽起來是短促的爆音。
    void Convert(const BYTE* src, UINT32 nframes, bool silent) {
        const size_t total = (size_t)nframes * (size_t)outChannels;
        conv.resize(total);

        if (silent) {
            memset(conv.data(), 0, total * sizeof(int16_t));
            return;
        }

        if (isFloat) {
            const float* f = reinterpret_cast<const float*>(src);
            for (size_t i = 0; i < total; ++i) {
                const size_t s = (i / (size_t)outChannels) * (size_t)srcChannels +
                                 (i % (size_t)outChannels);
                float v = f[s];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                conv[i] = (int16_t)(v * 32767.0f);
            }
        } else {
            const int16_t* p = reinterpret_cast<const int16_t*>(src);
            for (size_t i = 0; i < total; ++i) {
                const size_t s = (i / (size_t)outChannels) * (size_t)srcChannels +
                                 (i % (size_t)outChannels);
                conv[i] = p[s];
            }
        }
    }

    void Drain() {
        UINT32 packet = 0;
        int64_t drained = 0;
        while (!stop.load()) {
            HRESULT hr = capture->GetNextPacketSize(&packet);
            if (FAILED(hr) || packet == 0) break;

            BYTE* data = nullptr;
            UINT32 nframes = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;
            hr = capture->GetBuffer(&data, &nframes, &flags, &devPos, &qpcPos);
            if (FAILED(hr)) break;

            // 記下第一個 packet 的 QPC。這是「音訊時間軸的 0 對應哪個牆上時刻」
            // 唯一的直接證據：
            //   pu64QPCPosition 是「這個 packet 第一個 frame」的時間戳，
            //   而我們送出去的 pts 也正好是從 0 開始累加 frame 數 ——
            //   兩者對的是同一個 frame，所以 音訊 pts 0 ↔ qpcPos。
            //
            // 之前用「第一次 Drain 拿到多少 frame」當 pre-roll，量到 10.2ms，
            // 但實測真正的差距是 60ms 左右 —— 於是每一份檔案都被多補了 50~60ms
            // 的延遲（A/B 對照：不補償的檔案量到 −1.3ms，補償後變成 +53.8ms）。
            // 猜一個量，不如直接拿驅動告訴我們的時間。
            if (firstPacketQpc == 0 && qpcPos != 0) {
                // 規格說這個值是 100ns，但有些驅動直接回 QPC 計數。兩者差三個
                // 數量級，用「跟現在差多少」判斷就好，不必相信文件。
                const int64_t now100ns = Qpc100nsLocal();
                if (llabs((int64_t)qpcPos - now100ns) > 20000000LL) {   // > 2 秒
                    LARGE_INTEGER f{};
                    QueryPerformanceFrequency(&f);
                    firstPacketQpc = f.QuadPart > 0
                        ? (int64_t)((double)qpcPos * 10000000.0 / (double)f.QuadPart)
                        : now100ns;
                    qpcIsRawCounts = true;
                } else {
                    firstPacketQpc = (int64_t)qpcPos;
                }
            }

            if (nframes > 0 && fn) {
                // 診斷：前 40 個 packet 的「封包年齡」＝ 現在 − 驅動給的 QPC。
                // >0 表示這個封包在緩衝區躺了一段（內容比 QPC 舊）；≈0 表示
                // 內容時間就是 QPC。分辨「pre-roll」與「時間戳本身偏移」用。
                static int dbgPkt = 0;
                if (dbgPkt < 40) {
                    const int64_t age = Qpc100nsLocal() - (int64_t)qpcPos;
                    if (age > -1000000LL && age < 10000000LL) {
                        LogPkt("pkt %2d: age=%6.1fms nframes=%u", dbgPkt, age / 10000.0, nframes);
                    }
                    ++dbgPkt;
                }
                Convert(data, nframes, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                const uint64_t base = frames.load();
                const int64_t pts = (int64_t)((base * 10000000ull) / (uint64_t)fmt.sampleRate);
                fn(conv.data(), (size_t)nframes, pts);
                frames.store(base + nframes);
            }
            capture->ReleaseBuffer(nframes);
            drained += (int64_t)nframes;
        }
        // 第一次 Drain 拿到的是「Start() 之前就已經躺在緩衝區裡」的音訊。
        // 它的長度就是 loopback 的 pre-roll：內容 t=0 對應的時刻是
        // Start() 減掉這段。A/V 偏移要扣掉它，見 docs/08 §6.4。
        if (firstBurstFrames < 0) firstBurstFrames = drained;
    }

    // 把靜音渲染串流的緩衝區填滿。這不是為了發聲，是為了讓音訊引擎保持運轉。
    void KeepAliveFill() {
        if (!keepAlive || !rrender || !rclient || rbufferFrames == 0) return;
        UINT32 pad = 0;
        if (FAILED(rclient->GetCurrentPadding(&pad))) return;
        const UINT32 avail = pad < rbufferFrames ? rbufferFrames - pad : 0;
        if (avail == 0) return;
        BYTE* p = nullptr;
        if (FAILED(rrender->GetBuffer(avail, &p)) || !p) return;
        memset(p, 0, (size_t)avail * (size_t)(wf ? wf->nBlockAlign : 4));
        rrender->ReleaseBuffer(avail, 0);
    }

    void ThreadMain() {
        while (!stop.load()) {
            DWORD w = WaitForSingleObject(evt, 20);
            if (stop.load()) break;
            if (w == WAIT_OBJECT_0) Drain();
            else if (w != WAIT_TIMEOUT) break;
            KeepAliveFill();
        }
        // 收尾：把還沒讀完的資料抽乾，否則最後 20~40 ms 會不見。
        if (capture) Drain();
        running.store(false);
    }
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { impl_->Close(); }

bool AudioCapture::Open(const AudioCaptureConfig& cfg, std::string* err) {
    auto& I = *impl_;
    I.kind = cfg.kind;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&I.enumr);
    if (FAILED(hr)) {
        if (err) *err = "MMDeviceEnumerator " + Hex(hr);
        return false;
    }

    if (cfg.device.empty()) {
        hr = I.enumr->GetDefaultAudioEndpoint(FlowOf(cfg.kind), eConsole, &I.dev);
        if (FAILED(hr)) {
            // 沒有預設端點（例如純 RDP 且未轉送音訊、或沒有麥克風）。這不是致命
            // 錯誤，呼叫端應該降級成「少一條軌」而不是整個錄製失敗。
            if (err) *err = std::string("no default ") +
                            (cfg.kind == AudioSourceKind::Input ? "capture" : "render") +
                            " endpoint " + Hex(hr);
            return false;
        }
    } else {
        std::string rerr;
        I.dev = ResolveDevice(I.enumr, cfg.kind, cfg.device, &rerr);
        if (!I.dev) {
            if (err) *err = rerr;
            return false;
        }
    }

    I.devId = EndpointId(I.dev);
    I.devName = FriendlyName(I.dev);
    if (I.devName.empty()) I.devName = I.devId;

    hr = I.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&I.client);
    if (FAILED(hr)) {
        if (err) *err = "Activate(IAudioClient) " + Hex(hr);
        return false;
    }

    hr = I.client->GetMixFormat(&I.wf);
    if (FAILED(hr)) {
        if (err) *err = "GetMixFormat " + Hex(hr);
        return false;
    }

    I.srcChannels = I.wf->nChannels;
    I.fmt.sampleRate = (int)I.wf->nSamplesPerSec;
    I.outChannels = I.srcChannels >= 2 ? 2 : 1;
    I.fmt.channels = I.outChannels;

    const bool pcm = I.wf->wFormatTag == WAVE_FORMAT_PCM ||
                     (I.wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(I.wf)->SubFormat,
                                  kSubPcm));
    I.isFloat = !pcm;
    I.bytesPerSample = I.wf->wBitsPerSample / 8;

    if (!(I.isFloat && I.wf->wBitsPerSample == 32) &&
        !(pcm && I.wf->wBitsPerSample == 16)) {
        if (err) {
            char b[128];
            snprintf(b, sizeof(b), "unsupported mix format: %u-bit %s", I.wf->wBitsPerSample,
                     I.isFloat ? "float" : "pcm");
            *err = b;
        }
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (cfg.kind == AudioSourceKind::Loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    // 200 ms 的緩衝：太短會在系統忙的時候斷音，太長則 kill 時損失更多。
    hr = I.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, I.wf, nullptr);
    if (FAILED(hr)) {
        if (err) *err = "IAudioClient::Initialize " + Hex(hr);
        return false;
    }

    hr = I.client->GetService(__uuidof(IAudioCaptureClient), (void**)&I.capture);
    if (FAILED(hr)) {
        if (err) *err = "GetService(IAudioCaptureClient) " + Hex(hr);
        return false;
    }

    // 這兩個數字是 A/V 偏移能不能算對的關鍵：
    //   GetStreamLatency  = 引擎 + 端點的處理延遲（聲音算出來 → 進到緩衝區）
    //   GetBufferSize     = 緩衝區有幾個 frame（本程式要求 200ms）
    // loopback 的內容天生比「現在」晚一個 latency，所以
    //   真正的 A/V 偏移 = (Start 延遲) − (stream latency)
    // 少了這一項，會把 loopback 的固有延遲當成「音訊比較晚開始」而去補償，
    // 補出來的檔案反而音畫不同步。
    {
        REFERENCE_TIME lat = 0;
        if (SUCCEEDED(I.client->GetStreamLatency(&lat)) && lat > 0) {
            I.streamLatency100ns = (int64_t)lat;
        }
        UINT32 bf = 0;
        if (SUCCEEDED(I.client->GetBufferSize(&bf))) I.capBufferFrames = bf;
    }

    // ---- 靜音渲染串流（keep-alive），只有 loopback 需要
    //
    // WASAPI 的 loopback 只在「音訊引擎正在跑」的時候送資料。系統完全安靜
    // （沒有任何程式在放聲音）時，引擎會進入 idle，loopback 一個 packet 都不送 ——
    // 不是送靜音，是完全沒有事件。症狀是錄出來的音軌長度 0，而影像完全正常。
    //
    // 解法是自己在同一個端點上開一條播放串流，持續餵靜音。它混進系統混音裡
    // 什麼都聽不到，但引擎會一直運轉，loopback 就會穩定送資料（安靜時是靜音）。
    //
    // 麥克風不需要：capture 端點自己會持續送資料（送的是底噪或靜音）。
    if (cfg.kind == AudioSourceKind::Loopback) {
        HRESULT khr = I.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      (void**)&I.rclient);
        if (SUCCEEDED(khr)) {
            khr = I.rclient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, I.wf, nullptr);
            if (SUCCEEDED(khr)) {
                khr = I.rclient->GetBufferSize(&I.rbufferFrames);
                if (SUCCEEDED(khr)) {
                    khr = I.rclient->GetService(__uuidof(IAudioRenderClient),
                                                (void**)&I.rrender);
                }
            }
            if (SUCCEEDED(khr) && I.rrender && I.rbufferFrames > 0) {
                I.keepAlive = true;
            } else {
                fprintf(stderr, "[audio] keep-alive unavailable (0x%08lX) -- 安靜時可能收不到音訊\n",
                        (unsigned long)khr);
            }
        }
    }

    I.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!I.evt) {
        if (err) *err = "CreateEvent failed";
        return false;
    }
    hr = I.client->SetEventHandle(I.evt);
    if (FAILED(hr)) {
        if (err) *err = "SetEventHandle " + Hex(hr);
        return false;
    }

    return true;
}

bool AudioCapture::Start(PacketFn fn, std::string* err) {
    auto& I = *impl_;
    if (!I.client) {
        if (err) *err = "audio not opened";
        return false;
    }
    if (I.running.load()) return true;

    I.fn = std::move(fn);
    I.stop.store(false);
    I.frames.store(0);

    if (I.keepAlive && I.rclient) {
        I.rclient->Start();
        I.KeepAliveFill();
    }

    HRESULT hr = I.client->Start();
    if (FAILED(hr)) {
        if (err) *err = "IAudioClient::Start " + Hex(hr);
        return false;
    }

    I.running.store(true);
    I.th = std::thread([&I] { I.ThreadMain(); });
    return true;
}

void AudioCapture::Stop() {
    auto& I = *impl_;
    if (!I.client) return;
    I.stop.store(true);
    if (I.th.joinable()) I.th.join();
    I.client->Stop();
    I.running.store(false);
}

bool AudioCapture::running() const { return impl_->running.load(); }
int64_t AudioCapture::streamLatency100ns() const {
    auto& I = *impl_;
    if (I.firstBurstFrames >= 0 && I.fmt.sampleRate > 0) {
        return I.firstBurstFrames * 10000000LL / I.fmt.sampleRate;
    }
    // 還沒有第一次 Drain（剛 Start 完）就退回 GetStreamLatency 的值，
    // 它對 loopback 通常是 0，至少不會給出錯的數字。
    return I.streamLatency100ns;
}
uint32_t AudioCapture::bufferFrames() const { return impl_->capBufferFrames; }
int64_t AudioCapture::firstPacketQpc100ns() const { return impl_->firstPacketQpc; }
bool AudioCapture::qpcWasRawCounts() const { return impl_->qpcIsRawCounts; }
AudioFormat AudioCapture::format() const { return impl_->fmt; }
uint64_t AudioCapture::frames() const { return impl_->frames.load(); }
AudioSourceKind AudioCapture::kind() const { return impl_->kind; }
std::string AudioCapture::deviceName() const { return impl_->devName; }
std::string AudioCapture::deviceId() const { return impl_->devId; }
const std::string& AudioCapture::lastError() const { return impl_->err; }

} // namespace capwgc
