// ============================================================================
// recorder.cpp — 錄製器的實作
// ============================================================================
//
// 這裡的流程是從 src/cli/main.cpp 抽出來的，語意**刻意保持一致**。改動任何
// 時序（零點、音訊起跑點、時長算法）之前先讀 docs/08 §6.6 —— 那裡記了三個
// 看起來很合理但實際量錯的做法。
// ============================================================================

#include "recorder.h"

#include "aac_encoder.h"
#include "capture_wgc.h"
#include "codec_select.h"
#include "mft_encoder.h"

#include <d3d11.h>
#include <mfapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")

namespace capwgc {
namespace {

// QPC 換成 100ns。WGC 的 SystemRelativeTime 用的就是這個時鐘，所以影像的 pts
// 可以直接跟這裡的回傳值相減 —— 這是量 A/V 偏移的基礎。
int64_t Qpc100ns() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 10000000.0 / freq);
}

// 兩個裝置的時鐘不會一樣快，所以 slave 的樣本用「環形緩衝 + 丟最舊」吸收漂移。
// 容量 1 秒：正常情況下只會用到幾十毫秒，滿出來代表 slave 比 master 快。
class PcmRing {
public:
    void Init(size_t capFrames, int channels) {
        std::lock_guard<std::mutex> lk(mu_);
        ch_ = channels;
        cap_ = capFrames;
        buf_.assign(cap_ * (size_t)ch_, 0);
        head_ = count_ = 0;
    }

    void Push(const int16_t* p, size_t frames) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cap_) return;
        if (frames > cap_) {                    // 一次塞爆：只留最後 cap_
            p += (frames - cap_) * (size_t)ch_;
            frames = cap_;
            head_ = count_ = 0;
        }
        if (count_ + frames > cap_) {           // 滿了 → 丟最舊的
            const size_t drop = count_ + frames - cap_;
            head_ = (head_ + drop) % cap_;
            count_ -= drop;
        }
        const size_t tail = (head_ + count_) % cap_;
        const size_t first = std::min(frames, cap_ - tail);
        std::memcpy(&buf_[tail * ch_], p, first * ch_ * sizeof(int16_t));
        if (first < frames) {
            std::memcpy(&buf_[0], p + first * ch_, (frames - first) * ch_ * sizeof(int16_t));
        }
        count_ += frames;
    }

    // 把 ring 裡的 frames 個 frame 加到 dst（dst 已經是 master 的樣本）。
    // 不夠就當成靜音 —— 補 0 比補上一段聲音自然。
    void MixInto(int16_t* dst, size_t frames) {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t take = std::min(frames, count_);
        for (size_t i = 0; i < take; ++i) {
            const int16_t* src = &buf_[((head_ + i) % cap_) * ch_];
            for (int c = 0; c < ch_; ++c) {
                const int v = (int)dst[i * ch_ + c] + (int)src[c];
                dst[i * ch_ + c] = (int16_t)std::max(-32768, std::min(32767, v));
            }
        }
        head_ = (head_ + take) % cap_;
        count_ -= take;
    }

private:
    std::mutex mu_;
    std::vector<int16_t> buf_;
    size_t cap_ = 0, head_ = 0, count_ = 0;
    int ch_ = 2;
};

// 把 sc 聲道轉成 mc 聲道（1↔2 就複製 / 平均，其餘當成已經對齊）。
void ConvertChannels(const int16_t* src, size_t frames, int sc, int mc,
                     std::vector<int16_t>& out) {
    out.resize(frames * (size_t)mc);
    if (sc == mc) {
        std::memcpy(out.data(), src, out.size() * sizeof(int16_t));
    } else if (sc == 1 && mc == 2) {
        for (size_t i = 0; i < frames; ++i) {
            out[i * 2 + 0] = out[i * 2 + 1] = src[i];
        }
    } else if (sc == 2 && mc == 1) {
        for (size_t i = 0; i < frames; ++i) {
            out[i] = (int16_t)(((int)src[i * 2] + (int)src[i * 2 + 1]) / 2);
        }
    } else {
        out.assign(frames * (size_t)mc, 0);
    }
}


// 音畫同步的診斷日誌。
//
// 為什麼要有：A/V 偏移是「音訊內容起點 − 影像零點」，兩邊都在別的執行緒上，
// 出問題時只能看到「不同步」這個結果，看不到是哪一段延遲造成的。把三個關鍵
// QPC 寫出來，才分得出是「音訊太晚起跑」還是「pts 基準錯了」。
//
// 預設關閉：這是錄影路徑，每次寫檔都是不必要的 I/O，而且 avsync.log 會愈長
// 愈大。要診斷時設環境變數：
//     set CAPWGC_AVSYNC_LOG=1
// out/verify_sync.sh 會自動設它。
static bool SyncLogEnabled() {
    static const bool on = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"CAPWGC_AVSYNC_LOG", v, 8) > 0 && v[0] != L'0';
    }();
    return on;
}

void LogSync(const char* fmt, ...) {
    if (!SyncLogEnabled()) return;
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring p = std::wstring(dir) + L"\\CapWGC\\avsync.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// 把「螢幕座標的區域」換算成「某顆螢幕的 item 裁切矩形」。
//
// 為什麼要挑螢幕而不是直接用區域：WGC 一次只能錄一個 item（一顆螢幕或一個
// 視窗），跨螢幕的區域在 API 層就表達不了。實務上取中心點所在的那顆，並把
// 矩形夾進該螢幕範圍 —— 使用者拖超過邊界時錄到的是「螢幕內的那一塊」，不是
// 失敗，也不是錄到隔壁。
bool ResolveRegion(const RECT& r, HMONITOR* outMon,
                   int* cx, int* cy, int* cw, int* ch, std::string* err) {
    const LONG w = r.right - r.left;
    const LONG h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        if (err) *err = "選取的區域是空的";
        return false;
    }
    POINT center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        if (err) *err = "GetMonitorInfo 失敗";
        return false;
    }

    RECT inter{};
    if (!IntersectRect(&inter, &r, &mi.rcMonitor)) {
        if (err) *err = "選取的區域不在任何螢幕上";
        return false;
    }

    LONG ox = inter.left - mi.rcMonitor.left;
    LONG oy = inter.top - mi.rcMonitor.top;
    LONG ow = inter.right - inter.left;
    LONG oh = inter.bottom - inter.top;

    // 4:2:0 的編碼器對奇數寬高不一定吃得下（有的 MFT 直接拒絕，有的自己補
    // 邊再裁回來）。這裡先向下取偶，代價是最多 1px，換來的是不用在編碼器
    // 初始化失敗時去猜原因。
    ow &= ~1L;
    oh &= ~1L;
    if (ow <= 0 || oh <= 0) {
        if (err) *err = "選取的區域太小（寬高至少 2px）";
        return false;
    }

    *outMon = mon;
    *cx = (int)ox;
    *cy = (int)oy;
    *cw = (int)ow;
    *ch = (int)oh;
    return true;
}

// 線性內插的取樣率轉換。只用在「把麥克風併進系統音」這種場合，語音不需要
// 更講究的濾波器；要的是不爆音、相位連續（跨 packet 不能每次歸零）。
class Resampler {
public:
    void Reset(double step, int channels) {
        step_ = step;
        ch_ = channels;
        pos_ = 0.0;
        pend_.clear();
    }

    // 回傳 out 裡的 frame 數。
    size_t Process(const int16_t* in, size_t frames, std::vector<int16_t>& out) {
        pend_.insert(pend_.end(), in, in + frames * (size_t)ch_);
        const size_t have = pend_.size() / (size_t)ch_;
        out.clear();
        size_t i = (size_t)pos_;
        double frac = pos_ - (double)i;
        while (i + 1 < have) {
            for (int c = 0; c < ch_; ++c) {
                const double a = pend_[i * ch_ + c];
                const double b = pend_[(i + 1) * ch_ + c];
                out.push_back((int16_t)std::lround(a + (b - a) * frac));
            }
            pos_ += step_;
            i = (size_t)pos_;
            frac = pos_ - (double)i;
        }
        // 已消化的整數部分丟掉，小數相位留給下一個 packet
        const size_t drop = (size_t)pos_;
        if (drop > 0) {
            pend_.erase(pend_.begin(), pend_.begin() + (ptrdiff_t)(drop * (size_t)ch_));
            pos_ -= (double)drop;
        }
        return out.size() / (size_t)ch_;
    }

private:
    double step_ = 1.0;
    double pos_ = 0.0;
    int ch_ = 2;
    std::vector<int16_t> pend_;
};

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct Recorder::Impl {
    RecorderConfig cfg;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    WgcCapture capture;

    std::unique_ptr<Encoder> enc;
    Codec usedCodec = Codec::H264;
    std::string encoderName;

    struct Audio {
        RecAudioSpec spec;
        std::unique_ptr<AudioCapture> cap;
        std::unique_ptr<AacEncoder> aac;
        int track = -1;
        // master 專用：混音軌（aac/track）之外，自己再留一條「未混音」的分軌。
        // 為什麼 master 也要分軌：只有「混音軌 + 麥克風軌」是無法還原出系統聲的
        // （A = sys + mic，B = mic，A − B 不是乾淨的 sys）。要讓剪輯真的能用，
        // 三條都要有：混音 / 系統 / 麥克風。
        std::unique_ptr<AacEncoder> aacSplit;
        int splitTrack = -1;
        bool started = false;
        bool isInput = false;      // true = 麥克風（會被靜音鈕影響）
        // 靜音時用的零緩衝。放在這裡而不是 callback 裡：那條路徑每個 packet
        // 都會跑，每次配置一塊記憶體是白花錢。
        std::vector<int16_t> muteBuf;
        int64_t startDelay100ns = 0;
        bool syncLogged = false;
        bool offsetApplied = false;
        std::atomic<uint64_t> au{0};
        std::atomic<uint64_t> errors{0};
        std::string lastError;

        // ---- 混音（多個來源合成一條軌）----
        // slave：把樣本推進 ring 讓 master 混；分軌模式下自己也編一條軌。
        bool slave = false;
        bool hasSlaves = false;               // master 專用
        std::vector<Audio*> slaves;           // master 專用
        std::vector<int16_t> mixBuf;          // master 的混音暫存
        PcmRing ring;                         // slave → master 的樣本佇列
        Resampler rs;                         // slave 的取樣率轉換
        bool rsIdentity = true;
        int mixCh = 2;                        // master 的聲道數
        std::vector<int16_t> chBuf, outBuf;   // slave 的暫存（避免每包配置）
    };
    std::vector<std::unique_ptr<Audio>> audio;

    // ---- 影像時間軸 ----
    bool inited = false;
    bool firstWriteSeen = false;
    int staleSkips = 0;
    int64_t firstPts = 0;
    int64_t lastPts = 0;
    int64_t lastDur = 0;
    int64_t videoOriginQpc = 0;
    uint64_t writeErrors = 0;
    int outW = 0, outH = 0;
    // 裁切矩形（item 座標）。cropW/cropH 為 0 = 不裁切，用 item 原尺寸。
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;

    // ---- 狀態（跨執行緒讀）----
    std::atomic<bool> running{false};
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> initedFlag{false};
    std::atomic<bool> closedFlag{false};
    std::atomic<bool> micMuted{false};
    std::atomic<bool> hasMic{false};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> totalAU{0};
    // 寫進檔案（＝告訴編碼器）的音軌條數。Stop() 最後會把 d.enc 放掉，所以
    // 不能事後去問編碼器，得在建立音軌時就記下來。
    std::atomic<int> trackCount{0};
    std::atomic<int64_t> originQpc{0};   // 第一筆寫入的牆上時刻（算秒數用）
    DWORD startedTick = 0;
    mutable std::mutex mu;
    std::string error;
    // 音源開不起來時的說明。不能靜默跳過：使用者勾了麥克風卻沒有聲音，
    // 只會以為程式壞了，而真正的原因（端點不存在 / 被獨佔）藏在這裡。
    std::string audioNote;

    bool startAudio(std::string* err);
    void applyAudioOffset(Audio* s);
    void onFrame(const CapturedFrame& f);
};

bool Recorder::Impl::startAudio(std::string* err) {
    for (auto& sp : audio) {
        Audio* s = sp.get();
        if (s->started) continue;

        const bool ok = s->cap->Start(
            [s, this](const int16_t* pcm, size_t frames, int64_t pts) {
                const AudioFormat af = s->cap->format();
                const int ch = af.channels;

                // 靜音 = 把樣本換成 0 再送進編碼器，不是跳過不編。
                //
                // 跳過的話那條軌的「累計取樣點」會停住，之後每個 AU 的 dts 都
                // 往前擠，聽起來是整段聲音被加速；而且尾巴補靜音時長度也算錯。
                // 換成零樣本則是完全等價的一段無聲，時間軸一個 tick 都不動。
                if (s->isInput && ch > 0 && micMuted.load()) {
                    const size_t n = frames * (size_t)ch;
                    if (s->muteBuf.size() < n) {
                        s->muteBuf.assign(n, 0);
                    } else {
                        std::fill(s->muteBuf.begin(), s->muteBuf.begin() + n, int16_t(0));
                    }
                    pcm = s->muteBuf.data();
                }

                // 把一段 PCM 編碼並寫進指定那條軌。一個來源可能有兩條軌
                // （混音軌 / 自己的分軌），所以編碼器與軌號都當參數傳。
                auto writeAu = [s, this](AacEncoder* aac, int track, const int16_t* p,
                                         size_t n, int64_t t) {
                    if (!aac || track < 0) return;
                    std::string eerr;
                    if (!aac->Encode(
                            p, n, t,
                            [s, this, track](const uint8_t* d, size_t sz, int64_t ts) {
                                // 第一個 AU 之前把偏移定案：容器一旦收了樣本就不能
                                // 再改宣告值（SetAudioOffset 會拒絕），而那時才有
                                // 機會知道 pre-roll。
                                applyAudioOffset(s);
                                if (!enc || track >= enc->audioTrackCount()) return;
                                if (!s->syncLogged) {
                                    s->syncLogged = true;
                                    const int64_t q0 = s->cap->firstPacketQpc100ns();
                                    LogSync(
                                        "first AU[%d]: t=%.1fms  videoOrigin=%lld  "
                                        "firstPacketQpc=%lld  "
                                        "firstPacket-videoOrigin=%.1fms  "
                                        "startDelay=%.1fms  lat=%.1fms",
                                        s->track, ts / 10000.0, (long long)videoOriginQpc,
                                        (long long)q0,
                                        q0 ? (q0 - videoOriginQpc) / 10000.0 : -1.0,
                                        s->startDelay100ns / 10000.0,
                                        s->cap->streamLatency100ns() / 10000.0);
                                }
                                std::string werr;
                                if (!enc->WriteAudio(track, d, sz, 1024, ts, &werr)) {
                                    s->errors.fetch_add(1);
                                    s->lastError = werr;
                                    return;
                                }
                                s->au.fetch_add(1);
                                totalAU.fetch_add(1);
                            },
                            &eerr)) {
                        s->errors.fetch_add(1);
                        s->lastError = eerr;
                    }
                };

                // ---- slave：推樣本給混音軌，順便（可選）留一條自己的分軌 ----
                if (s->slave) {
                    // 分軌用**原生取樣率**，不經過重取樣 —— 剪輯要沒被動過的素材
                    writeAu(s->aac.get(), s->track, pcm, frames, pts);
                    ConvertChannels(pcm, frames, ch, s->mixCh, s->chBuf);
                    if (s->rsIdentity) {
                        s->ring.Push(s->chBuf.data(), frames);
                    } else {
                        const size_t n = s->rs.Process(s->chBuf.data(), frames, s->outBuf);
                        if (n) s->ring.Push(s->outBuf.data(), n);
                    }
                    return;
                }

                // ---- master：先留自己的分軌（未混音），再把自己 + 所有 slave 疊起來 ----
                //
                // 順序不能顛倒：mixBuf 一旦蓋掉 pcm，原始的系統聲就沒了。
                writeAu(s->aacSplit.get(), s->splitTrack, pcm, frames, pts);
                if (s->hasSlaves) {
                    const size_t n = frames * (size_t)ch;
                    if (s->mixBuf.size() < n) s->mixBuf.resize(n);
                    std::memcpy(s->mixBuf.data(), pcm, n * sizeof(int16_t));
                    for (Audio* sl : s->slaves) sl->ring.MixInto(s->mixBuf.data(), frames);
                    pcm = s->mixBuf.data();
                }
                writeAu(s->aac.get(), s->track, pcm, frames, pts);
            }, err);
        s->started = ok;
        if (!ok) {
            std::lock_guard<std::mutex> lk(mu);
            if (!audioNote.empty()) audioNote += "; ";
            audioNote += "音源啟動失敗";
            if (err && !err->empty()) audioNote += ": " + *err;
            return false;
        }
        s->startDelay100ns = Qpc100ns() - videoOriginQpc;
        LogSync("audio start[%d]: startDelay=%.1fms  videoOrigin=%lld", s->track,
                s->startDelay100ns / 10000.0, (long long)videoOriginQpc);
    }
    return true;
}

// 把「音軌內容真正從哪一刻開始」寫進容器。
//
// 這是整個音畫同步的核心，也是之前一直搞錯的地方。
//
// 兩條軌的時間軸零點不是同一個瞬間：
//   影像 t=0 = 第一筆寫入的幀的**擷取時刻**（= videoOriginQpc）
//   音訊 t=0 = 第一個 WASAPI packet 的**內容時刻**（= firstPacketQpc）
// 而音訊是在第一筆影像寫入之後才 Start() 的，所以
//   A = firstPacketQpc − videoOriginQpc > 0
// 也就是「檔案時間 0 的那一刻，音軌根本還沒有資料」。
//
// 但 muxer 收到第一筆 AU 時一定把它放在 dts=0。結果是整條音軌早了 A：
// 檔案時間 0 的畫面是 videoOrigin 的內容，聲音卻是 videoOrigin + A 的內容。
// 聽起來就是「聲音比嘴型快」。
//
// 這裡的 A 必須逐條軌量，不能共用一個值：多條音源不是同時起跑的（第二條
// Start() 可能晚幾十毫秒），共用會讓第二條軌偏掉。
//
// 為什麼之前會覺得「不用補償」：那時的驗證素材（anim_window --sync）自己的嗶聲
// 有 ~90ms 的 PlaySound 延遲，而 CLI 那條路的 A 剛好也是 ~89ms —— 兩個誤差互相
// 抵掉，量出來是「對齊」。GUI 的 A 只有 17ms，抵不掉，於是在同一個素材上量出
// 「音訊晚 70ms」（= 90 − 17）。兩個檔案其實都偏了 A，只是一個剛好被量測誤差
// 蓋住。素材源現在已改成樣本鎖定（見 anim_window.cpp），這個遮蔽就消失了。
void Recorder::Impl::applyAudioOffset(Audio* s) {
    if (!s || s->offsetApplied) return;
    s->offsetApplied = true;
    if (!enc) return;

    const int64_t q0 = s->cap->firstPacketQpc100ns();
    const int64_t gap = q0 ? (q0 - videoOriginQpc) : s->startDelay100ns;
    const int64_t off = gap + cfg.avOffset100ns;

    // 同一個裝置的兩條軌（混音軌 / 自己的分軌）延遲完全一樣，一次算、兩條都套。
    // 只套其中一條的症狀很隱蔽：播放器播混音軌聽起來是準的，剪輯軟體拉分軌
    // 出來才發現差了 A 毫秒。
    const int ts[2] = {s->track, s->splitTrack};
    bool logged = false;
    for (int t : ts) {
        if (t < 0 || t >= enc->audioTrackCount()) continue;
        if (!logged) {
            logged = true;
            LogSync("offset[%d]: gap=%.1fms cal=%.1fms -> %+.1fms", t,
                    gap / 10000.0, cfg.avOffset100ns / 10000.0, off / 10000.0);
        }
        if (off == 0) continue;
        if (!enc->SetAudioOffset(t, off)) {
            s->errors.fetch_add(1);
            s->lastError = "SetAudioOffset rejected: audio samples already written";
            LogSync("offset[%d]: REJECTED（樣本已寫入，這條軌會偏 %.1fms）", t,
                    off / 10000.0);
        }
    }
}

void Recorder::Impl::onFrame(const CapturedFrame& f) {
    {
        static int dbgN = 0;
        if (dbgN < 12) {
            LogSync("frame %d: age=%.1fms", dbgN, (Qpc100ns() - f.pts100ns) / 10000.0);
            ++dbgN;
        }
    }
    if (!inited) {
        // 第一幀才知道 item 尺寸，編碼器只能在這裡開。
        const int w = cropW > 0 ? cropW : (int)f.width;
        const int h = cropH > 0 ? cropH : (int)f.height;

        EncoderConfig ec;
        ec.width = w;
        ec.height = h;
        ec.cropX = cropX;
        ec.cropY = cropY;
        ec.nominalFps = cfg.fps;
        ec.quality = cfg.quality;
        ec.bitrate = cfg.bitrate;
        ec.gopSeconds = cfg.gopSeconds;
        ec.bFrames = 0;          // B 幀仍被 MF 拒絕，見 HANDOFF 待辦 4
        ec.fragmented = cfg.fragmented;
        ec.rcMode = RcMode::Quality;

        ec.audioTracks.clear();
        // 推入順序必須和上面配軌號的順序一模一樣（混音軌 → master 分軌 → slave
        // 分軌），因為軌號就是靠「誰先 push 誰是 0」決定的。
        auto addTrack = [&](Impl::Audio* sp, AacEncoder* aac, bool enabled,
                            const char* suffix) {
            if (!aac) return;
            AudioTrackConfig at;
            at.sampleRate = sp->cap->format().sampleRate;
            at.channels = sp->cap->format().channels;
            at.decoderConfig = aac->config();
            at.name = std::string(AudioSourceKindName(sp->spec.kind)) + ": " +
                      sp->cap->deviceName() + suffix;
            // 只有混音軌是 enabled。分軌標成「不要預設播」：混音軌已經包含它的
            // 內容，兩條都播出來會聽到兩次麥克風。一般播放器會跳過，剪輯軟體
            // 照樣看得到。
            at.enabled = enabled;
            ec.audioTracks.push_back(std::move(at));
        };
        for (const auto& sp : audio) {
            if (!sp->slave) addTrack(sp.get(), sp->aac.get(), true, "");
            if (sp->aacSplit) addTrack(sp.get(), sp->aacSplit.get(), false, "（分軌）");
            if (sp->slave) addTrack(sp.get(), sp->aac.get(), false, "（分軌）");
        }

        // 降級鏈：首選不可用就往下試，最後一個能開的算數。這樣「有 AV1 就用
        // AV1」不需要在編譯期知道是哪張卡，也不會因為首選初始化失敗就整個
        // 錄製失敗。
        std::vector<Codec> order;
        if (cfg.autoCodec) {
            for (const auto& c : ProbeCodecs()) order.push_back(c.codec);
        } else {
            order.push_back(cfg.codec);
            for (const auto& c : ProbeCodecs()) {
                if (c.codec != cfg.codec) order.push_back(c.codec);
            }
        }

        std::string lastErr = "no encoder available";
        for (Codec c : order) {
            ec.codec = c;
            auto e = std::make_unique<MftEncoder>();
            std::string oerr;
            if (e->Open(cfg.outPath, ec, device, &oerr)) {
                enc = std::move(e);
                usedCodec = c;
                break;
            }
            lastErr = oerr;
        }
        if (!enc) {
            std::lock_guard<std::mutex> lk(mu);
            error = lastErr;
            stopFlag.store(true);
            return;
        }
        encoderName = enc->encoderName();
        outW = w;
        outH = h;
        firstPts = f.pts100ns;
        lastPts = f.pts100ns;
        inited = true;
        initedFlag.store(true);
        return;   // 第一幀只拿來初始化，不寫入（duration 未知）
    }

    if (f.formatChanged) {
        {
            std::lock_guard<std::mutex> lk(mu);
            error = "擷取尺寸中途改變，尚未實作重建編碼器";
        }
        stopFlag.store(true);
        return;
    }

    // 丟掉「開編碼器時積壓」的舊幀。
    //
    // 症狀：第一幀（只拿來開編碼器）之後，WGC 會先送一幀在初始化期間就拍好的
    // 畫面，它的 pts 比「現在」舊上百毫秒。若拿它當影像時間軸零點，音訊卻是在
    // 「現在」才起跑 —— 整個音軌就會晚 age 那麼多（實測 158~315ms，隨編碼器
    // 初始化時間浮動）。這正是「錄出來的檔案嘴型對不上聲音」的原因。
    //
    // 判準用「這一幀比現在舊多少」而不是固定跳過第一幀：初始化快的時候那一幀
    // 是新鮮的，跳掉反而多丟畫面。上限 3 幀，避免慢機器上永遠等不到新鮮幀。
    if (!firstWriteSeen && staleSkips < 10) {
        const int64_t age = Qpc100ns() - f.pts100ns;
        const int64_t limit = 3 * 10000000LL / (cfg.fps > 0 ? cfg.fps : 60);
        if (age > limit) {
            ++staleSkips;
            lastPts = f.pts100ns;   // 不更新它，下一幀的 duration 會暴衝
            LogSync("skip stale frame: age=%.1fms (第 %d 次)", age / 10000.0, staleSkips);
            return;
        }
    }

    int64_t dur = f.pts100ns - lastPts;
    if (dur <= 0) dur = 10000000LL / (cfg.fps > 0 ? cfg.fps : 60);
    lastPts = f.pts100ns;
    lastDur = dur;

    if (!firstWriteSeen) {
        firstWriteSeen = true;
        // 影像時間軸的零點 = 這一幀。容器裡第一筆影像樣本的 decode time 恆為 0
        // （stts 只存相鄰差），所以「這一幀的擷取時刻」才等於檔案時間 0。
        // 拿初始幀當零點會讓音軌多補一幀 —— 那就是過度補償。
        firstPts = f.pts100ns;
        videoOriginQpc = f.pts100ns;
        originQpc.store(f.pts100ns);
        // 跳過積壓幀之後，這一幀的 duration 會算成「積壓幀到現在」的整段空檔
        // （實測 342ms）。第一筆樣本沒有前一筆可以對齊，那個值沒有意義，只會
        // 在檔案開頭留下一個 300ms 的定格 —— 用名目幀長蓋掉。
        if (staleSkips > 0) dur = 10000000LL / (cfg.fps > 0 ? cfg.fps : 60);
        LogSync("first write: frameAge=%.1fms dur=%.1fms (pts 與現在的差)",
                (Qpc100ns() - f.pts100ns) / 10000.0, dur / 10000.0);

        // 音訊延到現在才起跑。若在第一幀就 Start()，量延遲時沒有基準可減，
        // 容器也來不及收到宣告值。代價是開頭多丟一幀。
        std::string aerr;
        startAudio(&aerr);
    }

    std::string werr;
    if (!enc->Write(f.texture, f.pts100ns - firstPts, dur, &werr)) {
        writeErrors++;
        std::lock_guard<std::mutex> lk(mu);
        error = werr;
    } else {
        samples.store(enc->samples());
        bytes.store(enc->bytes());
        frames.fetch_add(1);
    }
    if (writeErrors > 30) stopFlag.store(true);
}

// ============================================================================
// Recorder
// ============================================================================

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { Stop(); }

bool Recorder::Start(const RecorderConfig& cfg, std::string* err) {
    Impl& d = *impl_;
    if (d.running.load()) {
        if (err) *err = "已經在錄了";
        return false;
    }
    if (cfg.outPath.empty()) {
        if (err) *err = "沒有指定輸出檔";
        return false;
    }
    d.cfg = cfg;
    d.error.clear();
    d.writeErrors = 0;
    d.inited = false;
    d.initedFlag.store(false);
    d.closedFlag.store(false);
    d.stopFlag.store(false);
    d.firstWriteSeen = false;
    d.staleSkips = 0;
    d.frames.store(0);
    d.bytes.store(0);
    d.samples.store(0);
    d.totalAU.store(0);
    d.trackCount.store(0);
    d.originQpc.store(0);
    d.audioNote.clear();
    d.micMuted.store(false);
    d.hasMic.store(false);
    d.startedTick = GetTickCount();

    // ---- D3D11 ----
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   &d.device, &fl, &d.ctx);
    if (FAILED(hr)) {
        if (err) {
            char buf[96];
            snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX",
                     (unsigned long)hr);
            *err = buf;
        }
        return false;
    }

    // ---- 擷取目標 ----
    CaptureConfig ccfg;
    ccfg.captureCursor = cfg.captureCursor;
    ccfg.maxFps = cfg.fps;

    HMONITOR mon = cfg.monitor;
    if (cfg.useRegion) {
        // 區域 → 某顆螢幕 + 該螢幕的 item 座標裁切矩形。換算在這裡做完，呼叫端
        // 只給螢幕座標（GUI 拿到的是滑鼠座標）。
        if (!ResolveRegion(cfg.regionScreen, &mon, &d.cropX, &d.cropY,
                           &d.cropW, &d.cropH, err)) {
            return false;
        }
        ccfg.monitor = mon;
    } else if (cfg.hwnd) {
        ccfg.hwnd = cfg.hwnd;
    } else {
        if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        ccfg.monitor = mon;
    }

    // ---- 音訊（必須在 encoder Open 之前，音軌是 moov 的一部分）----
    //
    // 先只開裝置，開完才決定「混音」還是「分軌」—— 混音需要知道誰是 master
    // 以及 master 的取樣率/聲道數，那是 Open 之後才有的資訊。
    for (size_t i = 0; i < cfg.audio.size(); ++i) {
        auto s = std::make_unique<Impl::Audio>();
        s->spec = cfg.audio[i];

        AudioCaptureConfig ac;
        ac.kind = s->spec.kind;
        ac.device = s->spec.device;
        std::string aerr;
        s->cap = std::make_unique<AudioCapture>();
        if (!s->cap->Open(ac, &aerr)) {
            // 沒聲音總比沒畫面好，但一定要說出來
            if (!d.audioNote.empty()) d.audioNote += "; ";
            d.audioNote += std::string(AudioSourceKindName(s->spec.kind)) +
                           " 無法開啟: " + aerr;
            continue;
        }
        s->isInput = (s->spec.kind == AudioSourceKind::Input);
        if (s->isInput) d.hasMic.store(true);
        d.audio.push_back(std::move(s));
    }

    // 多個來源一律混成**一條**軌（cfg.mixAudio）。
    //
    // 為什麼不每條來源一條軌：MP4 有多條音軌時，大部分播放器只播第一條
    // （Windows 內建播放器、手機播放器都是），而第一條通常是 loopback。
    // 使用者「勾了麥克風、播放卻聽不到」就是這樣來的 —— 聲音在檔案裡，
    // 只是播放器沒去播那一條。混成一條之後任何播放器都對。
    const bool wantMix = cfg.mixAudio && d.audio.size() >= 2;
    Impl::Audio* master = d.audio.empty() ? nullptr : d.audio[0].get();

    // 一個來源可能有兩個編碼器：混音軌（master 的 aac）與自己的分軌（aacSplit）。
    // 用 slot 參數而不是寫死 s->aac，兩種情況才能共用同一段開檔邏輯。
    auto openInto = [&](Impl::Audio* s, std::unique_ptr<AacEncoder>& slot) -> bool {
        const AudioFormat af = s->cap->format();
        slot = std::make_unique<AacEncoder>();
        std::string aerr;
        if (!slot->Open(af.sampleRate, af.channels, cfg.audioBitrate, &aerr)) {
            if (!d.audioNote.empty()) d.audioNote += "; ";
            d.audioNote += std::string("AAC 編碼器無法開啟: ") + aerr;
            slot.reset();
            return false;
        }
        return true;
    };
    auto openAac = [&](Impl::Audio* s) -> bool { return openInto(s, s->aac); };

    const bool mixing = wantMix && master && openAac(master);
    int tracks = 0;
    for (size_t i = 0; i < d.audio.size(); ++i) {
        Impl::Audio* s = d.audio[i].get();
        if (mixing && s != master) {
            s->slave = true;      // 只餵樣本給混音軌
            s->track = -1;
            // 要分軌才順便自己編一條（原生取樣率，不經過重取樣）。
            // 這條會標成「不要預設播」，只有剪輯軟體會去用它。
            if (cfg.separateTracks && openAac(s)) s->track = tracks++;
            continue;
        }
        // master 在決定混音時就開好了（openAac），這裡只補其他來源
        if (!s->aac && !openAac(s)) continue;
        s->track = tracks++;
        // master 自己那條「未混音」的分軌。順序刻意排在混音軌後面：
        // 一般播放器挑第一條 → 永遠是混音軌，不會只播到系統聲。
        if (mixing && cfg.separateTracks && openInto(s, s->aacSplit)) {
            s->splitTrack = tracks++;
        }
    }
    d.trackCount.store(tracks);

    if (mixing) {
        const int mch = master->cap->format().channels;
        const int mrate = master->cap->format().sampleRate;
        master->hasSlaves = true;
        for (auto& sp : d.audio) {
            Impl::Audio* s = sp.get();
            if (!s->slave) continue;
            s->mixCh = mch;
            const int srate = s->cap->format().sampleRate;
            s->rsIdentity = (srate == mrate);
            if (!s->rsIdentity) s->rs.Reset((double)srate / (double)mrate, mch);
            s->ring.Init((size_t)mrate, mch);   // 容量 1 秒
            master->slaves.push_back(s);
        }
    }

    Impl* dp = &d;
    const bool ok = d.capture.Start(
        ccfg, d.device, [dp](const CapturedFrame& f) { dp->onFrame(f); }, err);
    if (!ok) return false;

    d.running.store(true);
    return true;
}

void Recorder::Stop() {
    Impl& d = *impl_;
    if (!d.running.load()) return;
    d.running.store(false);
    d.stopFlag.store(true);

    // 先停擷取（join WGC 執行緒），這一步之後就不會再有 onFrame。
    d.capture.Stop();
    if (d.capture.closed()) d.closedFlag.store(true);

    // 影像長度：兩條軌的時長不一致時播放器通常以短的為準，聽起來就是尾巴
    // 被切掉。補靜音比少一段安全，所以每條軌補到影像長度。
    const int64_t video100 =
        (d.lastPts > d.firstPts ? d.lastPts - d.firstPts : 0) + d.lastDur;

    for (auto& sp : d.audio) {
        Impl::Audio* s = sp.get();
        if (!s->started) continue;
        s->cap->Stop();

        const int64_t targetFrames =
            video100 * (int64_t)s->cap->format().sampleRate / 10000000;

        // 一個來源可能有兩條軌（混音軌 + 自己的分軌），兩條都要補滿到影像長度，
        // 少補的那條在剪輯軟體裡會短一截。
        auto flushOne = [&](AacEncoder* aac, int track) {
            if (!aac || track < 0) return;
            std::string ferr;
            aac->Flush(targetFrames, [s, &d, track](const uint8_t* p, size_t n, int64_t t) {
                d.applyAudioOffset(s);
                if (d.enc && track < d.enc->audioTrackCount()) {
                    std::string werr;
                    if (d.enc->WriteAudio(track, p, n, 1024, t, &werr)) {
                        s->au.fetch_add(1);
                        d.totalAU.fetch_add(1);
                    } else {
                        s->errors.fetch_add(1);
                    }
                }
            }, &ferr);
            aac->Close();
        };
        flushOne(s->aac.get(), s->track);
        flushOne(s->aacSplit.get(), s->splitTrack);
    }

    if (d.enc) {
        std::string cerr;
        if (!d.enc->Close(&cerr)) {
            std::lock_guard<std::mutex> lk(d.mu);
            if (d.error.empty()) d.error = cerr;
        }
        d.bytes.store(d.enc->bytes());
        d.samples.store(d.enc->samples());
    }

    if (d.ctx) { d.ctx->Release(); d.ctx = nullptr; }
    if (d.device) { d.device->Release(); d.device = nullptr; }
    d.enc.reset();
    d.audio.clear();
}

bool Recorder::running() const { return impl_->running.load(); }

void Recorder::SetMicMuted(bool muted) { impl_->micMuted.store(muted); }

bool Recorder::micMuted() const { return impl_->micMuted.load(); }

bool Recorder::hasMic() const { return impl_->hasMic.load(); }

const std::wstring& Recorder::path() const { return impl_->cfg.outPath; }

RecorderStats Recorder::stats() const {
    const Impl& d = *impl_;
    RecorderStats st;
    st.running = d.running.load();
    st.inited = d.initedFlag.load();
    st.captureClosed = d.closedFlag.load() || d.capture.closed();
    st.frames = d.frames.load();
    st.dropped = d.capture.dropped();
    st.throttled = d.capture.throttled();
    st.bytes = d.bytes.load();
    st.samples = d.samples.load();
    st.outW = d.outW;
    st.outH = d.outH;
    st.codecName = d.enc ? d.enc->codecName() : CodecName(d.usedCodec);
    st.encoderName = d.encoderName;
    st.audioAU = d.totalAU.load();
    st.audioTracks = d.trackCount.load();

    const int64_t o = d.originQpc.load();
    st.seconds = (o != 0) ? (double)(Qpc100ns() - o) / 10000000.0 : 0.0;

    std::lock_guard<std::mutex> lk(d.mu);
    st.error = d.error;
    st.audioNote = d.audioNote;
    return st;
}

std::wstring MakeDefaultOutputPath(const std::wstring& dir) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64] = {};
    swprintf_s(name, L"CapWGC_%04u-%02u-%02u_%02u-%02u-%02u.mp4",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring p = dir;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    p += name;
    return p;
}

} // namespace capwgc
