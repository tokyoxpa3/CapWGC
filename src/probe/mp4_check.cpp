// ============================================================================
// mp4_check.cpp — 用 Media Foundation 解回 MP4，驗證並量測
// ============================================================================
//
// 為什麼需要這支工具：
//
// 1. 「檔案產生了」不等於「檔案解得開」。muxer 是自己寫的，stbl 的一張表錯了
//    照樣會生出一個大小合理的檔案，只是播放器打不開或畫面是綠的。用系統的
//    decoder 解一遍，是唯一能證明容器正確的方式 —— 而且用的是別人寫的 decoder，
//    不是自己驗自己。
//
// 2. 「HEVC 比 H.264 小」這句話單獨沒有意義。小多少必須配一個品質數字，
//    否則把 QP 調到 51 每個 codec 都很小。這裡用 PSNR 對一份近無損的參考檔
//    做比較：同一個 PSNR 之下誰的檔案小，才是真的贏。
//
// 用法：
//   mp4_check.exe <file.mp4>                  解一遍，印出幀數、時長、大小
//   mp4_check.exe <ref.mp4> <test.mp4>        逐幀比對，印出 luma PSNR
//
// 兩個檔案必須同解析度。幀數不同時取小的那邊，並把差距印出來 —— 那通常代表
// 時間軸有問題，比 PSNR 本身更值得看。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

// --head：印開頭 300ms 的每 10ms 峰值（看音軌起跑處是不是靜音）
bool g_head = false;

// ---- Goertzel：只量單一頻率的振幅 -------------------------------------------
//
// 為什麼不用「取樣點絕對值的峰值」：那是寬頻量測，環境裡任何聲音都會觸發。
// 這台機器實測底噪 RMS −35 dBFS、峰值 −10.6 dBFS（音樂/人聲/系統音效），
// 而原本的哔聲只有 −6.9 dBFS，兩者只差 4 dB —— 於是「靜音樣本」也量到 11 個
// 「哔聲」。改成素材源播 3000 Hz 純音、量測端只取 3000 Hz 的能量，才分得開。
//
// 回傳正規化振幅（純音振幅 A 會回傳 ≈ A）。
double goertzelAmp(const int16_t* x, int n, int stride, double freq, double rate) {
    if (n <= 0 || rate <= 0.0) return 0.0;
    const double w = 2.0 * 3.14159265358979323846 * freq / rate;
    const double cw = cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = (double)x[(size_t)i * (size_t)stride] / 32768.0 + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

// ---- 讀出「容器自己宣告的音訊位移」 -----------------------------------------
//
// 這是量測的另外一半。toneOnsets 量的是「音訊在媒體時間軸上偏了多少」，
// 這裡讀的是「容器說它偏了多少」—— 兩者相等，補償才算對。
//
// 兩種容器表達位移的地方不同，所以兩條路都要走：
//   progressive → edts/elst
//       正位移：第一筆 media_time = -1 的 empty edit（segment_duration 用 mvhd timescale）
//       負位移：一筆 media_time > 0 的 edit = 從媒體的那個位置開始播，等於裁掉開頭
//               （media_time 用 **mdhd** timescale，不是 mvhd —— 這裡最容易寫錯）
//   fMP4        → 第一個 moof 裡該軌 traf 的 tfdt baseMediaDecodeTime
//                 （單位是 mdhd timescale，而且它在媒體時間軸上，Media Foundation
//                  會直接套用）
//
// 回傳 true = 讀到了，值寫進 *outMs（可為負）。false = 讀不出來，*how 說為什麼。
//
// 這裡刻意不用「回傳 -1 表示錯誤」的寫法：負的位移是完全合法的值（音訊偏早），
// 用 -1 當哨兵會讓「真實的 −150ms」與「讀不到」無法區分 —— 而症狀是量測端
// 印出 E = ?，看起來像解析器還沒實作，實際上是把合法值吃掉了。
bool declaredAudioDelayMs(const std::wstring& path, int64_t* outMs, std::string* how) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { *how = "open failed"; return false; }
    std::vector<unsigned char> buf;
    {
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (long)(512 << 20)) { fclose(f); *how = "size"; return false; }
        buf.resize((size_t)sz);
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
            fclose(f); *how = "read"; return false;
        }
    }
    fclose(f);

    auto be32 = [&](size_t o) -> uint32_t {
        if (o + 4 > buf.size()) return 0;
        return ((uint32_t)buf[o] << 24) | ((uint32_t)buf[o + 1] << 16) |
               ((uint32_t)buf[o + 2] << 8) | (uint32_t)buf[o + 3];
    };
    auto be64 = [&](size_t o) -> uint64_t {
        if (o + 8 > buf.size()) return 0;
        return ((uint64_t)be32(o) << 32) | (uint64_t)be32(o + 4);
    };
    auto type4 = [&](size_t o) -> std::string {
        if (o + 4 > buf.size()) return std::string();
        return std::string((const char*)&buf[o], 4);
    };
    auto findBox = [&](size_t from, size_t to, const char* want,
                       size_t* body, size_t* end) -> bool {
        size_t o = from;
        while (o + 8 <= to) {
            uint64_t sz = be32(o);
            const std::string t = type4(o + 4);
            size_t hdr = 8;
            if (sz == 1) { sz = be64(o + 8); hdr = 16; }
            else if (sz == 0) sz = to - o;
            if (sz < hdr || o + sz > to) return false;
            if (t == want) { *body = o + hdr; *end = o + (size_t)sz; return true; }
            o += (size_t)sz;
        }
        return false;
    };

    size_t moovB = 0, moovE = 0;
    if (!findBox(0, buf.size(), "moov", &moovB, &moovE)) { *how = "no moov"; return false; }

    // mvhd timescale：elst 的 segment_duration 用它
    uint32_t movieTs = 1000;
    {
        size_t b = 0, e = 0;
        if (findBox(moovB, moovE, "mvhd", &b, &e)) {
            const int ver = buf[b];
            movieTs = be32(b + (ver == 1 ? 20 : 12));
            if (!movieTs) movieTs = 1000;
        }
    }

    uint32_t audioTrackId = 0;
    uint32_t audioTs = 0;
    size_t o = moovB;
    while (o + 8 <= moovE) {
        uint64_t sz = be32(o);
        const std::string t = type4(o + 4);
        size_t hdr = 8;
        if (sz == 1) { sz = be64(o + 8); hdr = 16; }
        else if (sz == 0) sz = moovE - o;
        if (sz < hdr || o + sz > moovE) break;
        if (t == "trak") {
            const size_t tb = o + hdr, te = o + (size_t)sz;
            uint32_t id = 0;
            size_t b = 0, e = 0;
            if (findBox(tb, te, "tkhd", &b, &e)) id = be32(b + (buf[b] == 1 ? 20 : 12));
            std::string handler;
            uint32_t ts = 0;
            size_t mb = 0, me = 0;
            if (findBox(tb, te, "mdia", &mb, &me)) {
                if (findBox(mb, me, "hdlr", &b, &e)) handler = type4(b + 8);
                if (findBox(mb, me, "mdhd", &b, &e)) {
                    ts = be32(b + (buf[b] == 1 ? 20 : 12));
                }
            }
            if (handler == "soun" && !audioTrackId) {
                audioTrackId = id;
                audioTs = ts;
                // 先試 progressive 的 elst
                size_t eb = 0, ee = 0;
                if (findBox(tb, te, "edts", &eb, &ee)) {
                    size_t lb = 0, le = 0;
                    if (findBox(eb, ee, "elst", &lb, &le)) {
                        const int ver = buf[lb];
                        const uint32_t n = be32(lb + 4);
                        size_t p = lb + 8;
                        uint64_t delay = 0;
                        int64_t trim = 0;
                        for (uint32_t i = 0; i < n && p + (ver == 1 ? 20u : 12u) <= le; ++i) {
                            const uint32_t segDur = be32(p);
                            const int64_t mt = ver == 1 ? (int64_t)be64(p + 4)
                                                        : (int64_t)(int32_t)be32(p + 4);
                            p += ver == 1 ? 20 : 12;
                            if (mt < 0) delay += segDur;
                            else if (mt > 0 && trim == 0) trim = mt;   // 裁掉開頭
                        }
                        // 三種寫法：empty edit（正延遲）、media_time > 0（負偏移，
                        // 等於裁掉開頭）、兩者都沒有。media_time 的單位是 media
                        // timescale，不是 movie timescale —— 這裡最容易寫錯。
                        if (delay > 0) {
                            *how = "elst empty edit";
                            *outMs = (int64_t)(delay * 1000ULL / movieTs);
                            return true;
                        }
                        if (trim > 0 && ts) {
                            *how = "elst media_time（裁掉開頭）";
                            // media_time 是 **media** timescale（= mdhd.timescale，
                            // 音軌就是取樣率），不是 movie timescale。用錯的話
                            // 44.1k 的軌會少算 44 倍。
                            *outMs = -(int64_t)((uint64_t)trim * 1000ULL / ts);
                            return true;
                        }
                        *how = "elst 無位移";
                        *outMs = 0;
                        return true;
                    }
                }
            }
        }
        o += (size_t)sz;
    }

    if (!audioTrackId) { *how = "no audio trak"; return false; }

    // fMP4：走第一個 moof，找這條軌的 traf，讀 tfdt
    size_t p = 0;
    while (p + 8 <= buf.size()) {
        uint64_t sz = be32(p);
        const std::string t = type4(p + 4);
        size_t hdr = 8;
        if (sz == 1) { sz = be64(p + 8); hdr = 16; }
        else if (sz == 0) sz = buf.size() - p;
        if (sz < hdr || p + sz > buf.size()) break;
        if (t == "moof") {
            const size_t mb = p + hdr, me = p + (size_t)sz;
            size_t q = mb;
            while (q + 8 <= me) {
                uint64_t s2 = be32(q);
                const std::string t2 = type4(q + 4);
                size_t h2 = 8;
                if (s2 == 1) { s2 = be64(q + 8); h2 = 16; }
                else if (s2 == 0) s2 = me - q;
                if (s2 < h2 || q + s2 > me) break;
                if (t2 == "traf") {
                    const size_t tb = q + h2, te = q + (size_t)s2;
                    size_t b = 0, e = 0;
                    uint32_t id = 0;
                    if (findBox(tb, te, "tfhd", &b, &e)) id = be32(b + 4);
                    if (id == audioTrackId && findBox(tb, te, "tfdt", &b, &e)) {
                        const int ver = buf[b];
                        const uint64_t bmdt = ver == 1 ? be64(b + 4) : be32(b + 4);
                        *how = "tfdt of first moof";
                        if (!audioTs) return false;
                        *outMs = (int64_t)(bmdt * 1000ULL / audioTs);
                        return true;
                    }
                }
                q += (size_t)s2;
            }
            // 第一個 moof 沒有這條軌就別再往下找 —— 後面的分片 tfdt 是累加的
            *how = "first moof has no audio traf";
            *outMs = 0;
            return true;
        }
        p += (size_t)sz;
    }
    *how = "no elst, no moof";
    *outMs = 0;
    return true;
}

} // namespace

namespace {

struct Frame {
    std::vector<uint8_t> rgb;   // Y 平面，緊湊排列 width*height
    int w = 0, h = 0;
    int64_t ts = 0;
};

struct Decoder {
    ComPtr<IMFSourceReader> reader;
    int w = 0, h = 0;        // coded size（含 macroblock/CTU padding）
    int visW = 0, visH = 0;  // 真正可見的區域
    int64_t duration = 0;

    bool open(const wchar_t* path, std::string* err) {
        // 沒有這兩個 attribute，SourceReader 不會為了輸出 RGB32 而插入
        // Video Processor，SetCurrentMediaType 直接回 MF_E_INVALIDMEDIATYPE
        // —— 看起來像「檔案壞了」，其實只是少了色彩轉換。
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(attrs.GetAddressOf(), 3);
        if (FAILED(hr)) { *err = "MFCreateAttributes"; return false; }
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        hr = MFCreateSourceReaderFromURL(path, attrs.Get(), reader.GetAddressOf());
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof(b), "MFCreateSourceReaderFromURL 0x%08lX", (unsigned long)hr);
            *err = b;
            return false;
        }

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        ComPtr<IMFMediaType> native;
        hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, native.GetAddressOf());
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof(b), "GetNativeMediaType 0x%08lX (no video stream?)",
                     (unsigned long)hr);
            *err = b;
            return false;
        }
        UINT32 tw = 0, th = 0;
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &tw, &th);
        w = (int)tw; h = (int)th;

        // 原生 subtype 是「這個檔案被解析成什麼編碼」的唯一證據。fMP4 出問題時
        // MF 只回 MF_E_INVALIDMEDIATYPE，看不出是容器錯還是編碼器缺席。
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        const bool isHevc = (sub == MFVideoFormat_HEVC) || (sub == MFVideoFormat_HEVC_ES);
        const bool isH264 = (sub == MFVideoFormat_H264);
        const bool isAv1 = (sub == MFVideoFormat_AV1);
        char subName[80];
        snprintf(subName, sizeof(subName),
                 "%s (%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X)",
                 isAv1 ? "AV1" : (isHevc ? "HEVC" : (isH264 ? "H.264" : "unknown")),
                 (unsigned long)sub.Data1, sub.Data2, sub.Data3,
                 sub.Data4[0], sub.Data4[1], sub.Data4[2], sub.Data4[3],
                 sub.Data4[4], sub.Data4[5], sub.Data4[6], sub.Data4[7]);

        // 解碼器回報的是 **coded** 尺寸，不是畫面尺寸：H.264 對齊到 16
        // （802 -> 816），HEVC 對齊到 64（802 -> 832）。兩個 codec 錄同一段
        // 內容會得到不同的 w/h，於是逐像素比較就變成拿 H.264 的 padding 去比
        // HEVC 的 padding —— 那個 PSNR 是假的。真正的畫面大小在
        // MF_MT_MINIMUM_DISPLAY_APERTURE 裡。
        visW = w;
        visH = h;
        {
            MFVideoArea area{};
            UINT32 len = 0;
            if (SUCCEEDED(native->GetBlobSize(MF_MT_MINIMUM_DISPLAY_APERTURE, &len)) &&
                len >= sizeof(area)) {
                UINT32 got = 0;
                if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                              (BYTE*)&area, sizeof(area), &got))) {
                    if (area.Area.cx > 0 && area.Area.cy > 0) {
                        visW = (int)area.Area.cx;
                        visH = (int)area.Area.cy;
                    }
                }
            }
        }

        ComPtr<IMFMediaType> out;
        hr = MFCreateMediaType(out.GetAddressOf());
        if (FAILED(hr)) { *err = "MFCreateMediaType"; return false; }
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, tw, th);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
        if (FAILED(hr)) {
            char b[128];
            snprintf(b, sizeof(b), "SetCurrentMediaType(NV12) 0x%08lX (native codec: %s)",
                     (unsigned long)hr, subName);
            *err = b;
            return false;
        }
        return true;
    }

    // 回傳 1 = 有一幀，0 = 結束，-1 = 錯誤
    int next(Frame& f) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                        &flags, &ts, sample.GetAddressOf());
        if (FAILED(hr)) return -1;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return 0;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> cur;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                      cur.GetAddressOf()))) {
                UINT32 tw = 0, th = 0;
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &tw, &th);
                w = (int)tw; h = (int)th;
            }
        }
        if (!sample) return 1;   // 串流事件，沒有畫面

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) return -1;
        ComPtr<IMF2DBuffer> b2d;
        if (FAILED(buf.As(&b2d)) || !b2d) return -1;

        BYTE* p = nullptr;
        LONG pitch = 0;
        if (FAILED(b2d->Lock2D(&p, &pitch))) return -1;

        DWORD bufLen = 0;
        buf->GetCurrentLength(&bufLen);
        // DXGI surface 形式的輸出（AV1 解碼器就是這種）GetCurrentLength 回 0，
        // 因為長度由 surface 決定而不是 buffer 自己。改用 GetMaxLength 當上界，
        // 否則下面的合理性檢查會把每一幀都判成壞帧（症狀是 len=0）。
        if (bufLen == 0) buf->GetMaxLength(&bufLen);

        // 這裡一定要擋。解碼器給的緩衝區大小與 MF_MT_FRAME_SIZE 不保證一致
        // （H.264 會回報未對齊的 802x632，而實際配置是 832x640 的 pitch），
        // 直接照 w 逐列 memcpy 會踩出去，症狀是 segfault 而不是「解碼失敗」。
        const int srcPitch = pitch < 0 ? -pitch : pitch;
        if (w <= 0 || h <= 0 || srcPitch < w ||
            (DWORD)((size_t)(h - 1) * srcPitch + (size_t)w) > bufLen) {
            fprintf(stderr, "[dec] implausible frame: %dx%d pitch=%ld len=%lu\n",
                    w, h, (long)pitch, (unsigned long)bufLen);
            b2d->Unlock2D();
            return -1;
        }

        const int cw = visW > 0 && visW <= w ? visW : w;
        const int ch = visH > 0 && visH <= h ? visH : h;
        f.w = cw;
        f.h = ch;
        f.ts = ts;
        f.rgb.resize((size_t)cw * ch);
        const BYTE* base = pitch < 0 ? p + (size_t)(h - 1) * srcPitch : p;
        for (int y = 0; y < ch; ++y) {
            memcpy(f.rgb.data() + (size_t)y * cw, base + (size_t)y * srcPitch, (size_t)cw);
        }
        b2d->Unlock2D();
        return 1;
    }
};

double Psnr(const uint8_t* a, const uint8_t* b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = (double)a[i] - (double)b[i];
        se += d * d;
    }
    const double mse = se / (double)n;
    if (mse <= 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// 音軌驗證。容器寫對不等於解得開：esds 的 ASC 少一個 byte、或 trak 的
// timescale 與 stts 不一致，MF 都會照樣回一個「解碼成功」的空資料流。
// 所以這裡同時檢查「解得出幾幀」與「波形是不是真的非零」。
struct AudioProbe {
    ComPtr<IMFSourceReader> reader;
    DWORD stream = 0;
    int rate = 0, ch = 0;

    bool open(const wchar_t* path, DWORD streamIndex, std::string* err) {
        stream = streamIndex;
        HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, reader.GetAddressOf());
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof(b), "MFCreateSourceReaderFromURL 0x%08lX", (unsigned long)hr);
            *err = b;
            return false;
        }

        ComPtr<IMFMediaType> native;
        hr = reader->GetNativeMediaType(stream, 0, native.GetAddressOf());
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof(b), "no stream %lu (0x%08lX)", (unsigned long)stream,
                     (unsigned long)hr);
            *err = b;
            return false;
        }

        UINT32 r = 0, c = 0;
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        rate = (int)r;
        ch = (int)c;

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(stream, TRUE);

        ComPtr<IMFMediaType> pcm;
        if (FAILED(MFCreateMediaType(pcm.GetAddressOf()))) { *err = "MFCreateMediaType"; return false; }
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, r);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, c);

        hr = reader->SetCurrentMediaType(stream, nullptr, pcm.Get());
        if (FAILED(hr)) {
            char b[128];
            snprintf(b, sizeof(b), "SetCurrentMediaType(PCM) 0x%08lX (native %d Hz %d ch)",
                     (unsigned long)hr, rate, ch);
            *err = b;
            return false;
        }
        return true;
    }

    // 回傳解出的每聲道取樣點數；失敗回 -1。
    int64_t run(double* peakDbfs, double* rmsDbfs, std::string* err) {
        int64_t frames = 0;
        int peak = 0;
        long double sumsq = 0.0L;
        int64_t nsamp = 0;

        // --head：印出開頭 300ms 的每 10ms 峰值。
        //
        // 用途：分辨「音畫不同步」的兩種成因 —— 音軌開頭有一段靜音（起跑延遲），
        // 還是內容整體位移（時間戳基準錯）。兩者在檔案裡長得一樣，只有看波形
        // 開頭才分得出來。
        const int64_t winSamples = (int64_t)(rate > 0 ? rate / 100 : 0) * (ch > 0 ? ch : 1);
        int64_t winLeft = (g_head && winSamples > 0) ? winSamples : 0;
        int winPeak = 0, winNo = 0;

        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(stream, 0, nullptr,
                                            &flags, &ts, sample.GetAddressOf());
            if (FAILED(hr)) {
                char b[96];
                snprintf(b, sizeof(b), "ReadSample 0x%08lX", (unsigned long)hr);
                *err = b;
                return -1;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) continue;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) continue;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
            const int16_t* q = reinterpret_cast<const int16_t*>(p);
            const size_t n = len / sizeof(int16_t);
            for (size_t i = 0; i < n; ++i) {
                const int v = q[i];
                const int a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                sumsq += (long double)v * v;
                if (winLeft > 0) {
                    if (a > winPeak) winPeak = a;
                    if (--winLeft == 0) {
                        printf("    head %3dms  peak=%6.1f dBFS\n", (winNo + 1) * 10,
                               winPeak > 0 ? 20.0 * log10((double)winPeak / 32768.0) : -999.0);
                        ++winNo;
                        winPeak = 0;
                        if (winNo < 30) winLeft = winSamples;
                    }
                }
            }
            nsamp += (int64_t)n;
            if (ch > 0) frames += (int64_t)(n / (size_t)ch);
            buf->Unlock();
        }

        const double pn = peak / 32768.0;
        *peakDbfs = peak > 0 ? 20.0 * log10(pn) : -999.0;
        const double rms = nsamp > 0 ? (double)sqrt((double)(sumsq / (long double)nsamp)) / 32768.0 : 0.0;
        *rmsDbfs = rms > 0 ? 20.0 * log10(rms) : -999.0;
        return frames;
    }

    // 找「指定頻率的純音從安靜變成有聲」的時刻，回傳 reader 給的 ts（100ns）。
    //
    // 兩件事必須先講清楚，不然這支工具的輸出會被誤讀：
    //
    // (1) reader 給的 ts 是 **media time**，不是 presentation time。實測把
    //     elst 的 empty edit 從 0 改到 560ms，第一個音訊樣本的 ts 一律是 0
    //     ——MF 的 SourceReader 不套用編輯清單（那是 Media Session 的事）。
    //     所以這個模式量到的是「音訊在媒體時間軸上偏了多少」，要用它來判斷
    //     「播放器認不認 elst」是錯的；elst 寫得對不對要看 --tracks。
    //     兩者一起看才有意義：宣告的延遲要等於這裡量到的偏移。
    //
    // (2) 用 Goertzel 只取 freq 這個窄帶，不用寬頻峰值。原因見 goertzelAmp。
    //
    // gap100ns 是防抖：同一個哔若被切斷會數成兩次。200ms 遠小於反轉間隔。
    bool toneOnsets(double freq, int64_t gap100ns, std::vector<int64_t>* out,
                    int64_t* firstTs, double* medAmp, double* maxAmp,
                    std::string* err) {
        struct Au { int64_t ts; std::vector<int16_t> pcm; };
        std::vector<Au> aus;
        *firstTs = -1;

        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(stream, 0, nullptr, &flags, &ts,
                                            sample.GetAddressOf());
            if (FAILED(hr)) {
                char b[96];
                snprintf(b, sizeof(b), "ReadSample 0x%08lX", (unsigned long)hr);
                *err = b;
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) continue;
            if (*firstTs < 0) *firstTs = (int64_t)ts;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) continue;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
            const int16_t* q = reinterpret_cast<const int16_t*>(p);
            const size_t n = len / sizeof(int16_t);
            Au au;
            au.ts = (int64_t)ts;
            au.pcm.assign(q, q + n);
            buf->Unlock();
            aus.push_back(std::move(au));
            if (aus.size() > 40000) break;   // 8 分鐘上限，避免吃光記憶體
        }

        out->clear();
        if (aus.empty()) { *medAmp = 0.0; *maxAmp = 0.0; return true; }
        if (ch <= 0 || rate <= 0) { *err = "bad audio format"; return false; }

        std::vector<double> amp(aus.size());
        for (size_t i = 0; i < aus.size(); ++i) {
            amp[i] = goertzelAmp(aus[i].pcm.data(), (int)(aus[i].pcm.size() / (size_t)ch),
                                 ch, freq, (double)rate);
        }

        std::vector<double> sorted(amp);
        std::sort(sorted.begin(), sorted.end());
        // 用「接近靜音的那一端」當中位數，不能用真正的中位數：哔聲如果佔了
        // 一半的 AU（200ms / 400ms），中位數本身就會落在有聲的區間，拿它乘
        // 一個倍數當門檻會直接超過最大值 —— 實測就是這樣，beeps 變成 0，
        // 而「0 個事件」在輸出上長得跟「完全對齊」很像，非常容易誤判。
        const double quiet = sorted[sorted.size() / 10];
        *medAmp = quiet;
        *maxAmp = sorted.back();

        // 門檻取「靜音端」與「峰值」的中間偏下。純音的 AU 振幅接近 max，
        // 靜音的 AU 接近 quiet，中間沒有任何東西，所以取哪裡都一樣。
        double thr = quiet + 0.45 * (*maxAmp - quiet);
        if (*maxAmp <= 0.0) thr = 1.0;

        bool loud = false;
        int64_t last = -1;
        for (size_t i = 0; i < aus.size(); ++i) {
            const bool nowLoud = amp[i] > thr;
            if (nowLoud && !loud) {
                // AU 是 1024 個 frame（23ms）。只回 AU 的 ts 會讓每個起點都帶
                // 23ms 的量化誤差 —— 跟要量的 60ms 同一個量級，不能接受。
                // 所以在這個 AU 內用 5.8ms 的窗、1.5ms 的步進再找一次起點。
                int64_t t = aus[i].ts;
                const int frames = (int)(aus[i].pcm.size() / (size_t)ch);
                const int win = rate / 170;          // 約 5.9ms
                const int hop = rate / 680;          // 約 1.5ms
                if (frames >= win && win > 0 && hop > 0) {
                    for (int s = 0; s + win <= frames; s += hop) {
                        const double a = goertzelAmp(aus[i].pcm.data() + (size_t)s * (size_t)ch,
                                                     win, ch, freq, (double)rate);
                        if (a > thr) {
                            t += (int64_t)s * 10000000LL / rate;
                            break;
                        }
                    }
                }
                if (last < 0 || t - last >= gap100ns) {
                    out->push_back(t);
                    last = t;
                }
            }
            loud = nowLoud;
        }
        return true;
    }
};

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { printf("MFStartup failed 0x%08lX\n", (unsigned long)hr); return 1; }

    if (argc < 2) {
        printf("usage:\n  mp4_check.exe <file.mp4>\n"
               "  mp4_check.exe <ref.mp4> <test.mp4>\n"
               "  mp4_check.exe --audio <file.mp4> [--head]\n"
               "                                     逐條解音軌並量波形（驗證 esds / trak）\n"
               "                                     --head 印開頭 300ms 的每 10ms 峰值\n"
               "  mp4_check.exe --sync  <file.mp4> [--freq 3000] [--period 400] [-v]\n"
               "                                     音畫事件對齊（配 anim_window --sync 用）\n");
        return 2;
    }

    // ---- trak 清單：直接讀 moov，不經過 Media Foundation
    //
    // 為什麼要自己解析：MF 給的 stream 索引不照 moov 的 trak 順序（實測雙音軌
    // 檔是「音軌在前、影像在後」），所以「stream 1 = 第一條音軌」這個假設是錯的。
    // 要知道檔案裡到底有幾條軌、每條叫什麼名字、時間軸多長，只能直接讀 box。
    if (argc >= 3 && !strcmp(argv[1], "--tracks")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { printf("%-28s cannot open\n", argv[2]); MFShutdown(); CoUninitialize(); return 1; }
        fseek(f, 0, SEEK_END);
        const long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf((size_t)(fsz > 0 ? fsz : 0));
        if (!buf.empty()) {
            if (fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
        }
        fclose(f);
        if (buf.size() < 16) { printf("%-28s too small\n", argv[2]); MFShutdown(); CoUninitialize(); return 1; }

        auto be32 = [&buf](size_t o) -> uint32_t {
            if (o + 4 > buf.size()) return 0;
            return ((uint32_t)buf[o] << 24) | ((uint32_t)buf[o + 1] << 16) |
                   ((uint32_t)buf[o + 2] << 8) | buf[o + 3];
        };
        auto be64 = [&](size_t o) -> uint64_t {
            return ((uint64_t)be32(o) << 32) | be32(o + 4);
        };
        auto type4 = [&buf](size_t o) {
            if (o + 4 > buf.size()) return std::string();
            return std::string((const char*)&buf[o], 4);
        };
        // 在 [from,to) 找第一個 type 的 box，回傳 body 起點與 box 結束位置。
        auto findBox = [&](size_t from, size_t to, const char* want, size_t* body, size_t* end) {
            size_t o = from;
            while (o + 8 <= to) {
                uint64_t sz = be32(o);
                const std::string t = type4(o + 4);
                size_t hdr = 8;
                if (sz == 1) { sz = be64(o + 8); hdr = 16; }
                else if (sz == 0) sz = to - o;
                if (sz < hdr || o + sz > to) return false;
                if (t == want) { *body = o + hdr; *end = o + (size_t)sz; return true; }
                o += (size_t)sz;
            }
            return false;
        };

        size_t moovBody = 0, moovEnd = 0;
        if (!findBox(0, buf.size(), "moov", &moovBody, &moovEnd)) {
            printf("%-28s no moov (被 kill 的分片檔只有 ftyp + moof)\n", argv[2]);
            MFShutdown(); CoUninitialize();
            return 1;
        }

        int idx = 0;
        size_t o = moovBody;
        while (o + 8 <= moovEnd) {
            uint64_t sz = be32(o);
            const std::string t = type4(o + 4);
            size_t hdr = 8;
            if (sz == 1) { sz = be64(o + 8); hdr = 16; }
            else if (sz == 0) sz = moovEnd - o;
            if (sz < hdr || o + sz > moovEnd) break;
            if (t == "trak") {
                const size_t tb = o + hdr, te = o + (size_t)sz;
                size_t b = 0, e = 0;
                uint32_t trackId = 0, w = 0, h = 0, tkhdFlags = 0;
                uint64_t tkhdDur = 0;
                if (findBox(tb, te, "tkhd", &b, &e)) {
                    const int ver = buf[b];
                    tkhdFlags = be32(b) & 0x00FFFFFFu;   // Track_enabled = bit 0
                    trackId = be32(b + (ver == 1 ? 20 : 12));
                    // body: ver/flags(4) creation modification track_ID reserved duration ...
                    tkhdDur = ver == 1 ? be64(b + 28) : be32(b + 20);
                    if (e >= b + 8) {
                        const uint32_t ws = be32(e - 8), hs = be32(e - 4);
                        w = ws >> 16; h = hs >> 16;
                    }
                }
                uint32_t ts = 0;
                uint64_t dur = 0;
                size_t mdiaB = 0, mdiaE = 0;
                if (findBox(tb, te, "mdia", &mdiaB, &mdiaE)) {
                    size_t mb = 0, me = 0;
                    if (findBox(mdiaB, mdiaE, "mdhd", &mb, &me)) {
                        const int ver = buf[mb];
                        ts = be32(mb + (ver == 1 ? 20 : 12));
                        dur = ver == 1 ? be64(mb + 24) : be32(mb + 16);
                    }
                }
                std::string handler, name;
                if (mdiaE > mdiaB && findBox(mdiaB, mdiaE, "hdlr", &b, &e)) {
                    // body: version/flags(4) pre_defined(4) handler_type(4) reserved(12) name
                    handler = type4(b + 8);
                    size_t n = b + 24;
                    while (n < e && buf[n]) name.push_back((char)buf[n++]);
                }
                // stsd 第一個 sample entry：音軌拿聲道/取樣率，影像拿編碼名
                std::string codec;
                uint32_t chans = 0, rate = 0;
                size_t minfB = 0, minfE = 0;
                if (mdiaE > mdiaB && findBox(mdiaB, mdiaE, "minf", &minfB, &minfE)) {
                    size_t stblB = 0, stblE = 0;
                    if (findBox(minfB, minfE, "stbl", &stblB, &stblE)) {
                        size_t stsdB = 0, stsdE = 0;
                        if (findBox(stblB, stblE, "stsd", &stsdB, &stsdE) && stsdE > stsdB + 44) {
                            // body: ver/flags(4) entry_count(4) | entry: size(4) type(4)
                            //       reserved(6) dri(2) ver(2) rev(2) vendor(4)
                            //       channels(2) samplesize(2) predef(2) reserved(2) rate(4, 16.16)
                            codec = type4(stsdB + 12);
                            if (handler == "soun") {
                                chans = be32(stsdB + 32) >> 16;
                                rate = be32(stsdB + 40) >> 16;
                            }
                        }
                    }
                }
                printf("trak %d  id=%-2u %-4s %-5s ts=%-6u %8.3fs  %ux%u  %s%s%s\n",
                       idx, trackId, handler.c_str(), codec.c_str(), ts,
                       ts ? (double)dur / ts : 0.0, w, h,
                       name.empty() ? "(unnamed)" : name.c_str(),
                       handler == "soun" ? "" : "",
                       (tkhdFlags & 1) ? "" : "  [disabled]");
                if (handler == "soun") {
                    printf("          audio: %u ch @ %u Hz\n", chans, rate);
                }

                // edts/elst：這是「這條軌晚多久才開始」唯一的表達方式。
                // 第一筆 media_time = -1 是 empty edit（只佔時間軸、沒有內容），
                // 它的長度就是延遲；第二筆才是真正的媒體。
                // tkhd.duration 是呈現長度（含延遲），mdhd.duration 是媒體本身
                // 的長度 —— 兩者相減就是延遲，可以拿來交叉驗證 elst 對不對。
                size_t eb = 0, ee = 0;
                if (findBox(tb, te, "edts", &eb, &ee)) {
                    size_t lb = 0, le = 0;
                    if (findBox(eb, ee, "elst", &lb, &le)) {
                        const int ver = buf[lb];
                        const uint32_t n = be32(lb + 4);
                        printf("          edit: %u entry\n", n);
                        size_t p = lb + 8;
                        for (uint32_t i = 0; i < n && p + (ver == 1 ? 20u : 12u) <= le; ++i) {
                            const uint32_t segDur = be32(p);
                            const int64_t mediaTime =
                                ver == 1 ? (int64_t)be64(p + 4) : (int64_t)(int32_t)be32(p + 4);
                            p += ver == 1 ? 20 : 12;
                            printf("                seg=%.3fs  media_time=%lld%s\n",
                                   segDur / 1000.0, (long long)mediaTime,
                                   mediaTime < 0 ? "  (empty edit = 延遲)" : "");
                        }
                    }
                }
                if (tkhdDur) {
                    const double presMs = (double)tkhdDur;
                    const double mediaMs = ts ? (double)dur * 1000.0 / ts : 0.0;
                    printf("          dur: tkhd=%.1fms  mdhd=%.1fms  -> 延遲 %.1fms\n",
                           presMs, mediaMs, presMs - mediaMs);
                }
                ++idx;
            }
            o += (size_t)sz;
        }
        MFShutdown(); CoUninitialize();
        return 0;
    }

    // ---- stream 清單：診斷「trak 順序 ≠ stream 索引」這類問題
    //
    // MF 的 MPEG-4 source 給的 stream 索引不保證照 moov 的 trak 順序（實測
    // 雙音軌檔是「兩條音軌在前、影像在後，而且兩條音軌是反的」），只靠序號
    // 對應會把麥克風當成系統聲。這個子命令把每個索引的真正格式印出來。
    if (argc >= 3 && !strcmp(argv[1], "--streams")) {
        wchar_t wp[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wp, MAX_PATH);
        ComPtr<IMFSourceReader> r;
        if (FAILED(MFCreateSourceReaderFromURL(wp, nullptr, r.GetAddressOf()))) {
            printf("%-28s cannot open\n", argv[2]);
            MFShutdown(); CoUninitialize();
            return 1;
        }
        auto fourcc = [](const GUID& g) {
            char b[5];
            b[0] = (char)(g.Data1 >> 24); b[1] = (char)(g.Data1 >> 16);
            b[2] = (char)(g.Data1 >> 8);  b[3] = (char)g.Data1;
            b[4] = 0;
            for (int i = 0; i < 4; ++i) if (b[i] < 32 || b[i] > 126) { b[0] = 0; break; }
            return std::string(b);
        };
        for (DWORD i = 0; i < 32; ++i) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(r->GetNativeMediaType(i, 0, mt.GetAddressOf()))) break;
            GUID major{}, sub{};
            mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 rate = 0, ch = 0, w = 0, h = 0;
            mt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            mt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            mt->GetUINT32(MF_MT_FRAME_SIZE, &w);
            if (major == MFMediaType_Audio) {
                printf("stream %-2lu audio  %-5s %u Hz %u ch\n", (unsigned long)i,
                       fourcc(sub).c_str(), rate, ch);
            } else if (major == MFMediaType_Video) {
                printf("stream %-2lu video  %-5s %ux%u\n", (unsigned long)i,
                       fourcc(sub).c_str(), w >> 16, w & 0xFFFF);
            } else {
                printf("stream %-2lu other  %s\n", (unsigned long)i, fourcc(sub).c_str());
            }
        }
        r.Reset();
        MFShutdown(); CoUninitialize();
        return 0;
    }

    // ---- 音軌：驗證能不能解、波形是不是真的非零
    //
    // 一個檔可以有多條音軌（系統聲 + 麥克風）。這裡逐條列舉 stream index
    // （MP4 的 stream 0 通常是影像，1..N 是音軌），每條各自解一次。
    if (argc >= 3 && !strcmp(argv[1], "--audio")) {
        for (int i = 3; i < argc; ++i) {
            if (!strcmp(argv[i], "--head")) g_head = true;
        }
        auto toW2 = [](const char* s) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
            return std::wstring(w);
        };
        const std::wstring path = toW2(argv[2]);

        // 先問「這個檔有幾條音訊 stream」。GetNativeMediaType 逐個問到失敗為止，
        // 不用 MF_SOURCE_READER_FIRST_AUDIO_STREAM —— 那個常數只能拿到第一條。
        std::vector<DWORD> streams;
        {
            ComPtr<IMFSourceReader> probe;
            if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr,
                                                   probe.GetAddressOf()))) {
                printf("%-28s cannot open\n", argv[2]);
                MFShutdown();
                CoUninitialize();
                return 1;
            }
            for (DWORD i = 0; i < 32; ++i) {
                ComPtr<IMFMediaType> mt;
                if (FAILED(probe->GetNativeMediaType(i, 0, mt.GetAddressOf()))) break;
                GUID major{};
                if (SUCCEEDED(mt->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                    major == MFMediaType_Audio) {
                    streams.push_back(i);
                }
            }
        }

        if (streams.empty()) {
            printf("%-28s no audio stream\n", argv[2]);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        int ok = 0;
        for (size_t k = 0; k < streams.size(); ++k) {
            AudioProbe ap;
            std::string aerr;
            if (!ap.open(path.c_str(), streams[k], &aerr)) {
                printf("%-28s track %zu (stream %lu)  open failed: %s\n", argv[2], k,
                       (unsigned long)streams[k], aerr.c_str());
                continue;
            }
            double peak = 0, rms = 0;
            const int64_t frames = ap.run(&peak, &rms, &aerr);
            if (frames < 0) {
                printf("%-28s track %zu (stream %lu)  decode failed: %s\n", argv[2], k,
                       (unsigned long)streams[k], aerr.c_str());
                continue;
            }
            const double secs = ap.rate > 0 ? (double)frames / ap.rate : 0.0;
            // 印 stream index 而不是「第幾條」：MF 給的 stream 編號不一定照 moov 的
            // trak 順序（實測這個檔是反的），只寫序號會讓兩條軌對不上。
            printf("%-28s stream %-2lu %d Hz %d ch  frames=%-7lld %.3fs  peak=%.1f dBFS  "
                   "rms=%.1f dBFS  %s\n",
                   argv[2], (unsigned long)streams[k], ap.rate, ap.ch, (long long)frames, secs,
                   peak, rms,
                   frames == 0 ? "NO AUDIO" : (peak < -90.0 ? "decodes OK (silent)" : "decodes OK"));
            if (frames > 0) ++ok;
            ap.reader.Reset();
        }

        MFShutdown();
        CoUninitialize();
        return ok == (int)streams.size() ? 0 : 1;
    }

    // ---- 音畫事件對齊（--sync）
    //
    // 這個模式回答的問題只有一個：**音訊在時間軸上偏了多少**。
    //
    // 做法：素材源（anim_window --sync）在整屏黑↔白反轉的同一瞬間播一個
    // 3kHz 純音。解回來之後，影像端找亮度跳變的幀、音訊端找純音起點，兩邊
    // 相減就是偏移。
    //
    // 有一個很容易搞錯的地方，先寫在最前面：
    //
    //   SourceReader 給的 ts 是 **media time**，不是 presentation time。
    //   把 elst 的 empty edit 從 0 改成 560ms，第一個音訊樣本的 ts 一律還是 0
    //   —— MF 的 SourceReader 不套用編輯清單（那是 Media Session 的工作）。
    //   所以「用 first_ts 判斷 MF 認不認 elst」是錯的，這個模式量到的是
    //   補償**之前**的原始偏移。
    //
    // 那要怎麼判斷補償對不對？把兩個數字放在一起看：
    //   量到的原始偏移 D（負 = 音訊偏早，要往後推）
    //   容器宣告的延遲 E（elst empty edit / 第一個 tfdt）
    //   呈現差 = D + E，等於 0 就代表宣告的延遲剛好把偏移抵掉。
    //   E 是不是合規地寫進容器，由 --tracks 負責；這裡只負責比對數值。
    if (argc >= 3 && !strcmp(argv[1], "--sync")) {
        auto toW3 = [](const char* s) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
            return std::wstring(w);
        };
        const std::wstring path = toW3(argv[2]);

        double toneHz = 3000.0;
        int periodMs = 400;
        bool verbose = false;
        for (int i = 3; i < argc; ++i) {
            if (!strcmp(argv[i], "--freq") && i + 1 < argc) toneHz = atof(argv[++i]);
            else if (!strcmp(argv[i], "--period") && i + 1 < argc) periodMs = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-v")) verbose = true;
            else if (argv[i][0] != '-') toneHz = atof(argv[i]);   // 舊用法：位置參數當頻率
        }
        if (periodMs < 50) periodMs = 50;

        // 第一條音訊 stream
        DWORD audioStream = 0;
        bool haveAudio = false;
        {
            ComPtr<IMFSourceReader> probe;
            if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr,
                                                   probe.GetAddressOf()))) {
                printf("%-28s cannot open\n", argv[2]);
                MFShutdown();
                CoUninitialize();
                return 1;
            }
            for (DWORD i = 0; i < 32; ++i) {
                ComPtr<IMFMediaType> mt;
                if (FAILED(probe->GetNativeMediaType(i, 0, mt.GetAddressOf()))) break;
                GUID major{};
                if (SUCCEEDED(mt->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                    major == MFMediaType_Audio) {
                    audioStream = i;
                    haveAudio = true;
                    break;
                }
            }
        }

        // 影像：每幀平均亮度，跳變就是一次反轉
        std::vector<int64_t> flips;
        {
            Decoder d;
            std::string derr;
            if (!d.open(path.c_str(), &derr)) {
                printf("%-28s video open failed: %s\n", argv[2], derr.c_str());
                MFShutdown();
                CoUninitialize();
                return 1;
            }
            Frame f;
            int prev = -1;
            int mn = 999, mx = -1;
            int64_t frames = 0;
            for (;;) {
                const int r = d.next(f);
                if (r == 0) break;
                if (r < 0) break;
                if (f.rgb.empty()) continue;
                double sum = 0;
                for (size_t i = 0; i < f.rgb.size(); ++i) sum += f.rgb[i];
                const int mean = (int)(sum / (double)f.rgb.size());
                if (mean < mn) mn = mean;
                if (mean > mx) mx = mean;
                ++frames;
                if (prev >= 0 && (mean - prev > 40 || prev - mean > 40)) {
                    flips.push_back(f.ts);
                }
                prev = mean;
            }
            // 亮度範圍是「這個檔案裡有沒有可見的變化」最便宜的證據。抓不到
            // 反轉時，先看這裡：min≈max 表示畫面根本沒動（錄到別的畫面、
            // 或全螢幕視窗沒真的顯示），而不是偵測邏輯壞了。
            printf("%-28s video frames=%lld  luma %d..%d\n", argv[2], (long long)frames, mn, mx);
        }

        std::vector<int64_t> beeps;
        int64_t firstTs = -1;
        double medAmp = 0.0, maxAmp = 0.0;
        if (haveAudio) {
            AudioProbe ap;
            std::string aerr;
            if (!ap.open(path.c_str(), audioStream, &aerr)) {
                printf("%-28s audio open failed: %s\n", argv[2], aerr.c_str());
                MFShutdown();
                CoUninitialize();
                return 1;
            }
            if (!ap.toneOnsets(toneHz, 1200000LL, &beeps, &firstTs, &medAmp, &maxAmp, &aerr)) {
                printf("%-28s audio decode failed: %s\n", argv[2], aerr.c_str());
                MFShutdown();
                CoUninitialize();
                return 1;
            }
            printf("%-28s audio %d Hz %d ch  media_ts[0]=%.1fms  tone=%.0fHz  "
                   "amp quiet=%.4f max=%.4f (%.1fdB)\n",
                   argv[2], ap.rate, ap.ch, firstTs / 10000.0, toneHz, medAmp, maxAmp,
                   maxAmp > 0.0 ? 20.0 * log10(maxAmp) : -999.0);
            ap.reader.Reset();

            // 素材源與底噪分不開時，後面所有數字都沒有意義，寧可先講出來。
            if (maxAmp > 0.0 && medAmp > 0.0 && maxAmp / medAmp < 4.0) {
                printf("%-28s 警告：純音只比底噪高 %.1fdB，偵測結果不可信\n",
                       argv[2], 20.0 * log10(maxAmp / medAmp));
            }
        }

        printf("%-28s flips=%zu  beeps=%zu\n", argv[2], flips.size(), beeps.size());
        if (verbose) {
            printf("  flips:");
            for (auto t : flips) printf(" %.1f", t / 10000.0);
            printf("\n  beeps:");
            for (auto t : beeps) printf(" %.1f", t / 10000.0);
            printf("\n");
        }

        // 配對不用「第 i 個對第 i 個」（數量不一定相同，一錯就整體錯位），
        // 也不用「最近的」（兩邊週期相同時，任何偏移在 mod 週期之後都一樣）。
        // 改成掃描：對每個候選偏移 d，數「有多少哔聲能在 d 附近找到一個反轉」，
        // 取命中數最多的那個。
        //
        // 掃描範圍限制在 ±period/2 —— 這是唯一能真正去混疊的做法。哔聲與反轉
        // 同週期 P 時，候選偏移是 d0 + P·k，在 ±P/2 之內只會有一個。之前想靠
        // 「哔聲改成每 3 次才響一次」來避免混疊是錯的：1500 是 500 的整數倍，
        // 哔聲仍是反轉格點的子集，500ms 的偏移照樣對得上。
        const int64_t halfRange = (int64_t)periodMs * 10000LL / 2;
        std::vector<double> deltas;
        int64_t bestD = 0;
        int bestHits = -1;
        if (!flips.empty() && !beeps.empty()) {
            const int64_t tol = 150000;        // ±15ms 算命中
            for (int64_t d = -halfRange; d <= halfRange; d += 10000) {   // 1ms 步進
                int hits = 0;
                for (auto b : beeps) {
                    for (auto f : flips) {
                        const int64_t diff = b - d - f;
                        if (diff >= -tol && diff <= tol) { ++hits; break; }
                    }
                }
                // 同分時取絕對值小的（偏移通常不大，先驗）
                if (hits > bestHits || (hits == bestHits && std::llabs(d) < std::llabs(bestD))) {
                    bestHits = hits;
                    bestD = d;
                }
            }

            // 用最佳偏移把每一對的殘差算出來，殘差的中位數就是真正落點
            for (auto b : beeps) {
                int64_t bestRes = 0;
                bool have = false;
                for (auto f : flips) {
                    const int64_t diff = b - bestD - f;
                    if (diff >= -tol && diff <= tol) {
                        if (!have || std::llabs(diff) < std::llabs(bestRes)) {
                            bestRes = diff;
                            have = true;
                        }
                    }
                }
                if (have) deltas.push_back(bestRes / 10000.0);
            }
        }

        double measured = 0.0;
        bool haveMeasured = false;
        if (!deltas.empty()) {
            std::vector<double> s = deltas;
            std::sort(s.begin(), s.end());
            const double resid = s[s.size() / 2];
            measured = (bestD / 10000.0) + resid;
            haveMeasured = true;
            printf("  grid match: best offset=%+.1fms  hits=%d/%zu  殘差中位數=%+.1fms\n",
                   bestD / 10000.0, bestHits, beeps.size(), resid);
            // 殘差大 = 配對不緊（門檻抓錯、素材源抖動），跟混疊無關。
            if (std::llabs((int64_t)(resid * 10000.0)) > 150000) {   // > 15ms
                printf("  警告：殘差 %.1fms 過大，配對可能不穩\n", resid);
            }
            if (bestHits < (int)beeps.size() * 2 / 3) {
                printf("  警告：只有 %d/%zu 個哔聲配得上，配對可能錯了\n",
                       bestHits, beeps.size());
            }
            // 混疊是這個方法的固有極限，不是 bug：哔聲與反轉同週期 P，所以
            // 「偏移 d」與「偏移 d + P」在檔案裡長得一模一樣。單次量測**無法**
            // 判斷是哪一個（殘差會很小，因為整串哔聲在 d 上自洽）。所以把
            // 等價類直接印出來，讓讀的人自己決定 —— 比印一個看起來很確定的
            // 數字誠實。實測 −500ms 的偏移在 ±200ms 的窗裡會顯示 −92ms。
        }

        std::string how;
        int64_t declared = 0;
        const bool haveDeclared = declaredAudioDelayMs(path, &declared, &how);
        // fMP4 的位移寫在第一個 tfdt，它**在媒體時間軸上**（tfdt 就是
        // baseMediaDecodeTime），所以已經反映在量到的 D 裡面；progressive 的
        // elst 不在媒體時間軸上（SourceReader 不套用），要另外加。
        // 這兩種情況用同一個公式算會差一個 E，剛好是我們要量的東西。
        const bool inMediaTime = how.find("tfdt") != std::string::npos;

        if (haveMeasured) {
            printf("  量到的原始偏移 D = %+.1fms（正 = 音訊偏晚）\n", measured);
        }
        if (haveDeclared) {
            printf("  容器宣告的位移 E = %+.1fms  (%s%s)\n", (double)declared, how.c_str(),
                   inMediaTime ? "，已含在媒體時間裡" : "，不在媒體時間裡");
        } else {
            printf("  容器宣告的位移 E = ?  (%s)\n", how.c_str());
        }
        if (haveMeasured && haveDeclared) {
            // E 的符號與 D 相同（正 = 音訊往後推），所以兩者相加。
            //
            // fMP4 只有「正位移」在媒體時間軸上（tfdt 往後推）；負位移是直接丟掉
            // 開頭幾個 AU，丟掉之後 tfdt 仍然是 0 —— 也就是「音訊已經往前拉」這件事
            // 已經完全反映在 D 裡了。再補一個負的 E 等於減兩次。
            const double res = (inMediaTime && declared > 0) ? measured
                                                             : measured + (double)declared;
            printf("  => 呈現時間差 = %+.1fms  %s\n", res,
                   std::llabs((int64_t)(res * 1000.0)) <= 10000 ? "對齊"
                                                                : (res < 0 ? "音訊偏早" : "音訊偏晚"));
        }
        if (!haveMeasured) {
            printf("  -> 沒有可配對的事件：先確認素材源有在反轉（luma min..max）"
                   "且有播純音（amp max）\n");
        } else {
            // 混疊的等價類。哔聲與反轉同週期 P，所以 D 只能決定到 mod P：
            // 「偏移 d」與「偏移 d + P」在檔案裡完全一樣，單次量測分辨不出來
            // （殘差照樣很小，因為整串哔聲在 d 上自洽）。把候選全部印出來，
            // 比印一個看起來很確定的數字誠實。
            // 實測：真實 −500ms 在 ±200ms 的窗裡顯示 −92ms。
            printf("  注意：D 只能決定到 mod %dms，候選 = %+.1f", periodMs, measured);
            for (int k = -2; k <= 2; ++k) {
                if (k == 0) continue;
                printf(" / %+.1f", measured + (double)k * periodMs);
            }
            printf("\n        要量超過 ±%dms 的偏移，把素材源的 --interval 拉長"
                   "（FLIP=2000 ./out/sync_ab.sh …）\n", periodMs / 2);
        }

        MFShutdown();
        CoUninitialize();
        return 0;
    }

    auto toW = [](const char* s) {
        wchar_t w[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
        return std::wstring(w);
    };

    // ---- 單檔：驗證能不能解
    if (argc == 2) {
        Decoder d;
        std::string err;
        const std::wstring path = toW(argv[1]);
        if (!d.open(path.c_str(), &err)) { printf("open failed: %s\n", err.c_str()); return 1; }

        Frame f;
        int frames = 0;
        int64_t lastTs = 0;
        for (;;) {
            const int r = d.next(f);
            if (r == 0) break;
            if (r < 0) { printf("decode error at frame %d\n", frames); return 1; }
            if (!f.rgb.empty()) { frames++; lastTs = f.ts; }
        }
        printf("%-28s %dx%d (coded %dx%d)  frames=%-5d last_ts=%.3fs  %s\n",
               argv[1], d.visW, d.visH, d.w, d.h, frames, lastTs / 1e7,
               frames > 0 ? "decodes OK" : "NO FRAMES");
        // MF 物件必須在 MFShutdown 之前放掉。反過來做的症狀是行程結束時
        // segfault —— 輸出全部正確，卻回一個失敗的 exit code。
        d.reader.Reset();
        MFShutdown();
        CoUninitialize();
        return frames > 0 ? 0 : 1;
    }

    // ---- 雙檔：PSNR
    //
    // 兩次錄製不可能在同一個毫秒開始，所以同一段動畫在兩個檔案裡會差幾幀。
    // 直接逐幀比對等於拿「方塊在左邊」比「方塊在右邊」，PSNR 會低到沒有意義，
    // 而且看起來像「其中一個 codec 很爛」。
    //
    // 所以先用前 24 幀掃一遍位移量（動畫每幀移動 9px，±12 幀足夠涵蓋），
    // 取平均 PSNR 最高的那個位移，再用它跑完整段。位移量會印出來 ——
    // 如果它大到 ±10 以上，那代表兩次錄製的內容本身不同，PSNR 不可信。
    std::string err;

    auto readHead = [](const wchar_t* path, int n, std::vector<Frame>* out, std::string* e) {
        Decoder d;
        if (!d.open(path, e)) return false;
        out->clear();
        Frame f;
        while ((int)out->size() < n) {
            const int r = d.next(f);
            if (r <= 0) break;
            if (!f.rgb.empty()) out->push_back(f);
        }
        d.reader.Reset();
        return true;
    };

    std::vector<Frame> headRef, headTest;
    if (!readHead(toW(argv[1]).c_str(), 24, &headRef, &err)) {
        printf("open ref failed: %s\n", err.c_str());
        return 1;
    }
    if (!readHead(toW(argv[2]).c_str(), 40, &headTest, &err)) {
        printf("open test failed: %s\n", err.c_str());
        return 1;
    }
    if (headRef.empty() || headTest.empty()) { printf("no frames to align\n"); return 1; }
    if (headRef[0].w != headTest[0].w || headRef[0].h != headTest[0].h) {
        printf("size mismatch: ref %dx%d vs test %dx%d\n",
               headRef[0].w, headRef[0].h, headTest[0].w, headTest[0].h);
        return 1;
    }

    int bestOffset = 0;
    double bestScore = -1;
    for (int d = -12; d <= 12; ++d) {
        double sum = 0;
        int cnt = 0;
        for (int i = 0; i < (int)headRef.size(); ++i) {
            const int j = i + d;
            if (j < 0 || j >= (int)headTest.size()) continue;
            if (headRef[i].rgb.size() != headTest[j].rgb.size()) continue;
            sum += Psnr(headRef[i].rgb.data(), headTest[j].rgb.data(), headRef[i].rgb.size());
            cnt++;
        }
        if (cnt < 8) continue;
        const double score = sum / cnt;
        if (score > bestScore) { bestScore = score; bestOffset = d; }
    }
    printf("align: test frame index = ref index + %d  (head PSNR %.2f dB)\n",
           bestOffset, bestScore);

    Decoder ref, test;
    if (!ref.open(toW(argv[1]).c_str(), &err)) { printf("open ref failed: %s\n", err.c_str()); return 1; }
    if (!test.open(toW(argv[2]).c_str(), &err)) { printf("open test failed: %s\n", err.c_str()); return 1; }
    if (ref.visW != test.visW || ref.visH != test.visH) {
        printf("visible size mismatch: ref %dx%d vs test %dx%d -- PSNR would be meaningless\n",
               ref.visW, ref.visH, test.visW, test.visH);
        return 1;
    }

    // 把兩邊拉到同一個起點
    {
        Frame tmp;
        for (int i = 0; i < bestOffset; ++i) if (test.next(tmp) <= 0) break;
        for (int i = 0; i < -bestOffset; ++i) if (ref.next(tmp) <= 0) break;
    }

    Frame a, b;
    double sum = 0, minP = 999, maxP = 0;
    int n = 0;
    for (;;) {
        const int ra = ref.next(a);
        const int rb = test.next(b);
        if (ra <= 0 || rb <= 0) break;
        if (a.rgb.empty() || b.rgb.empty()) continue;
        if (a.rgb.size() != b.rgb.size()) break;
        const double p = Psnr(a.rgb.data(), b.rgb.data(), a.rgb.size());
        sum += p;
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;
        n++;
    }

    if (n == 0) { printf("no comparable frames\n"); return 1; }
    printf("%-28s vs %-28s frames=%-5d  PSNR avg=%.2f dB  min=%.2f  max=%.2f\n",
           argv[1], argv[2], n, sum / n, minP, maxP);

    ref.reader.Reset();
    test.reader.Reset();
    MFShutdown();
    CoUninitialize();
    return 0;
}
