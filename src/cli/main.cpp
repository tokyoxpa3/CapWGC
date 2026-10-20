// ============================================================================
// main.cpp — CapCli：命令列錄製器
// ============================================================================
//
// 用法：
//   CapCli.exe --out rec.mp4                    錄主螢幕 10 秒
//   CapCli.exe --out rec.mp4 --hwnd 0x1234      錄單一視窗
//   CapCli.exe --out rec.mp4 --seconds 30 --codec h264 --quality 65
//   CapCli.exe --list-windows                   列出可錄的視窗
//   CapCli.exe --list-monitors                  列出螢幕
//
// 設計要點：
//   - encoder 延遲到第一幀才建（需要知道 item 尺寸）
//   - PTS 用 WGC 的 SystemRelativeTime，duration 由前後差決定 → VFR
//   - 靜止畫面沒有幀 → 沒有 sample → 這是壓縮率的來源，不是 bug
// ============================================================================

#include "../core/aac_encoder.h"
#include "../core/audio_capture.h"
#include "../core/capture_wgc.h"
#include "../core/codec_select.h"
#include "../core/encoder_mft.h"
#include "../core/mft_encoder.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

std::atomic<bool> g_stop{false};

// ---- A/V 偏移的校準檔 ------------------------------------------------------
//
// 為什麼要有一個檔案：這個偏移是「這台機器的兩條擷取管線延遲差」，用時間戳
// 推導不出來（見 docs/08 §6.6），只能量。量完之後存起來，之後每次錄影直接用，
// 換機器就重新量一次 —— 這就是「換硬體也不會有問題」的實際做法。
//
// 檔案：%LOCALAPPDATA%\CapWGC\av-offset.txt，內容是一行毫秒數（可為負）。
// 產生它的工具是 out/calibrate.sh，不是使用者手動編輯的。
std::wstring CalibrationPath() {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return {};
    std::wstring p = dir;
    p += L"\\CapWGC";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\av-offset.txt";
    return p;
}

// 回傳 true = 讀到了。*ms 可為負（音訊偏早）。
bool ReadCalibration(double* ms) {
    const std::wstring p = CalibrationPath();
    if (p.empty()) return false;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char line[128] = {};
    const size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    if (n == 0) return false;
    char* end = nullptr;
    const double v = strtod(line, &end);
    if (end == line) return false;
    *ms = v;
    return true;
}

BOOL WINAPI OnCtrl(DWORD) {
    g_stop.store(true);
    return TRUE;
}

struct WinEntry {
    HWND hwnd;
    std::wstring title;
    RECT rect;
};

BOOL CALLBACK EnumWinProc(HWND h, LPARAM p) {
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 255);
    if (title[0] == 0) return TRUE;
    RECT r{};
    GetWindowRect(h, &r);
    if (r.right - r.left < 32 || r.bottom - r.top < 32) return TRUE;
    auto* v = reinterpret_cast<std::vector<WinEntry>*>(p);
    v->push_back({h, title, r});
    return TRUE;
}

struct MonEntry {
    HMONITOR hmon;
    RECT rect;
    bool primary;
};

BOOL CALLBACK EnumMonProc(HMONITOR h, HDC, LPRECT r, LPARAM p) {
    auto* v = reinterpret_cast<std::vector<MonEntry>*>(p);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(h, &mi);
    v->push_back({h, *r, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

double ProcCpuMs() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime;   kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;   uu.HighPart = u.dwHighDateTime;
    return (double)(kk.QuadPart + uu.QuadPart) / 10000.0;
}

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

void Usage() {
    printf(
        "CapCli — WGC screen recorder\n"
        "\n"
        "  --out <file.mp4>        輸出檔（必填）\n"
        "  --hwnd <hex>            錄單一視窗（0x 前綴可省略）\n"
        "  --monitor               錄主螢幕（預設，若沒有 --hwnd）\n"
        "  --crop L,T,W,H          從 item 座標裁切\n"
        "  --codec auto|av1|hevc|h264\n"
        "                          預設 auto：AV1 > HEVC > H.264，硬體優先\n"
        "  --quality 0-100         預設 70（Quality 模式，不是 CBR）\n"
        "  --bitrate <bps>         覆寫碼率上限（0 = 自動）\n"
        "  --fps <n>               名義幀率，預設 60（只影響碼率預算與 GOP）\n"
        "  --seconds <n>           錄多久，預設 10\n"
        "  --gop <sec>             GOP 秒數，預設 2\n"
        "  --bframes <n>           B 幀數，預設 0（mft 引擎才有意義）\n"
        "  --engine mft|sink       mft = 自驅動 MFT + 自寫 muxer（預設）\n"
        "                          sink = M1 的 SinkWriter（對照組）\n"
        "  --progressive           容器改成 moov 在檔尾（相容性最好，但被 kill 就毀了）\n"
        "                          預設是 fMP4：每個 GOP 一片，行程被 kill 仍可播\n"
        "  --av-offset <ms|auto>   在「自動補 gap」之上再位移 N ms（校準用）。\n"
        "                          N > 0：往後推（音訊本來偏早時用）—— progressive 寫\n"
        "                            edts/elst 的 empty edit，fMP4 把第一個 tfdt 往後推\n"
        "                          N < 0：往前拉 = 裁掉開頭 |N| ms（音訊本來偏晚時用）——\n"
        "                            progressive 寫 elst 的 media_time（資料不丟），\n"
        "                            fMP4 真的丟 AU，精度到 AU 邊界（44.1k 約 23ms）\n"
        "                          auto = 讀校準檔 %%LOCALAPPDATA%%\\CapWGC\\av-offset.txt\n"
        "                          預設（不給這個旗標）= 只補 gap、不額外校正。\n"
        "                          gap = firstPacketQpc − videoOrigin，是「音軌比影像\n"
        "                          晚起跑」的量，不補它整條音軌就會早 gap 那麼多。\n"
        "                          校準檔由 out/calibrate.sh 產生，換硬體／驅動重量一次\n"
        "  --no-av-offset          完全不補（連 gap 都不補）= 原始時間軸，A/B 對照用\n"
        "  --audio-delay <ms>      Start() 之前先等 N ms（模擬慢速硬體，驗補償用）\n"
        "  --ab <codec> --out2 <f> 同一批擷取幀同時編兩種 codec（公平比較用）\n"
        "  --q2 <n>                --out2 那一路的 quality（預設跟隨 --quality）\n"
        "  --ref <f>               額外輸出一份 quality=100 的近無損參考檔\n"
        "                          三路同時寫，是 PSNR 量測的前提：\n"
        "                          兩次獨立錄製的動畫相位對不上，跨檔比較沒有意義\n"
        "  --dump-codecapi         印出 MFT 自己說它支援哪些碼控參數\n"
        "  --no-pin-qp             診斷用：不把 MinQP/MaxQP 釘死，只給 Quality\n"
        "                          預設會釘死 QP，讓 --quality 是可重現的量化步長\n"
        "  --qp <n>                真正的常數 QP（UnconstrainedVBR + MinQP=MaxQP）\n"
        "                          掃 rate-PSNR 曲線用這個，不要用 --quality\n"
        "  --audio                 錄系統聲音（WASAPI loopback，AAC-LC）\n"
        "                          預設關：既有的 PSNR / 大小數據都是純視訊\n"
        "  --mic                   錄麥克風（預設錄音裝置），與 --audio 可同時用\n"
        "                          兩者各成一條軌，播放器可以分開調音量\n"
        "  --audio-src <spec>      額外一條音源，可重複；spec = loopback[:裝置] | mic[:裝置]\n"
        "                          --audio 等於 --audio-src loopback，--mic 等於 mic\n"
        "  --audio-device <dev>    loopback 用哪個播放裝置（索引 / 名稱子字串 / endpoint ID）\n"
        "  --mic-device <dev>      麥克風用哪個錄音裝置\n"
        "  --audio-bitrate <bps>   每條音軌的 AAC 碼率，預設 128000\n"
        "  --list-audio-devices    列出所有音訊端點（播放 + 錄音）\n"
        "  --cursor                把游標錄進去（預設關）\n"
        "  --list-windows          列出可錄視窗\n"
        "  --list-monitors         列出螢幕\n");
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Windows 傳給 main 的 argv 是「ANSI 命令列」（CP_ACP）轉出來的，不是 UTF-8。
    // 中文裝置名或中文路徑一進程式就變成亂碼，拿去做字串比對永遠比不到 ——
    // 症狀是「--mic-device 麥克風 說找不到裝置」，但 --list-audio-devices 明明
    // 印得出來。改用 GetCommandLineW + CommandLineToArgvW 拿真正的 Unicode，
    // 再統一轉成 UTF-8，字串處理就只需要一套編碼。
    std::vector<std::string> argStore;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            argStore.reserve((size_t)wargc);
            for (int i = 0; i < wargc; ++i) {
                const wchar_t* w = wargv[i];
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                std::string s;
                if (n > 1) {
                    s.resize((size_t)n - 1);
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
                }
                argStore.push_back(std::move(s));
            }
            LocalFree(wargv);
        }
    }
    std::vector<char*> argvStore;
    if (!argStore.empty()) {
        argvStore.reserve(argStore.size());
        for (auto& s : argStore) argvStore.push_back(s.data());
        argc = (int)argvStore.size();
        argv = argvStore.data();
    }

    // Media Foundation 必須先啟動，否則 MFCreateSinkWriterFromURL 會回
    // MF_E_UNSUPPORTED_BYTESTREAM_TYPE（0xC00D3E85）—— 看起來像「格式不支援」，
    // 實際是「平台沒起來」。這個錯誤碼很容易誤導。
    HRESULT mfhr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfhr)) {
        printf("MFStartup failed 0x%08lX\n", (unsigned long)mfhr);
        return 1;
    }

    std::wstring outPath;
    HWND hwnd = nullptr;
    bool useMonitor = false;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int fps = 60;
    int quality = 70;
    int bitrate = 0;
    int seconds = 10;
    int gop = 2;
    int bframes = 0;
    bool useMft = true;
    bool fragmented = true;
    bool dumpCodecApi = false;
    bool pinQp = true;
    capwgc::RcMode rcMode = capwgc::RcMode::Quality;
    int qp = 0;
    std::wstring outPath2;
    std::wstring refPath;
    capwgc::Codec abCodec = capwgc::Codec::HEVC;
    bool abCodecSet = false;
    int quality2 = -1;      // -1 = 跟隨 --quality
    int qp2 = -1;           // -1 = 跟隨 --qp
    bool cursor = false;
    int audioBitrate = 128000;
    // A/V 偏移補償。三種模式：
    //   0 = 不補償（預設）
    //   1 = auto：用 startDelay / WASAPI QPC 推導
    //   2 = 指定毫秒數
    //
    // 為什麼預設改成「不補償」：推導出來的值在這台機器上是 58~72ms，但實際
    // 量測（mp4_check --sync，3kHz 純音對整屏反轉）顯示真正的偏移是 −1.3ms
    // ——也就是說檔案本來就是對齊的，補了 60ms 反而讓音訊晚了 54~82ms。
    // 推導之所以錯，是因為「WGC 給的幀時間戳」與「loopback packet 的 QPC」
    // 都不是內容時間：兩者各自帶著自己的管線延遲，相減不會得到音畫差。
    // 量不到的東西就不要猜，需要的人可以用 --av-offset 指定。
    int avOffsetMode = 0;
    int64_t avOffsetFixed = 0;      // mode 1/2 實際寫進容器的量（100ns）
    std::string avOffsetSource = "不補償";
    // 自動補「音軌比影像晚起跑」的那一段（= firstPacketQpc − videoOrigin）。
    //
    // 上面的註解（「預設不補償」）是被一個抵銷誤差騙出來的結論，2026-10 重新
    // 量到真因：驗證素材 anim_window 的嗶聲自己帶 ~90ms 的 PlaySound 延遲，而
    // 這條 CLI 路的 gap 剛好也是 ~89ms，兩個誤差互相抵掉 → 量出來「對齊」。
    // GUI 那條路的 gap 只有 ~17ms，抵不掉，使用者看到的就是聲音晚 70ms。
    // 現在預設把 gap 補掉；要重現舊行為（純原始時間軸）用 --no-av-offset。
    bool avAutoGap = true;
    // 測試用：在 Start() 之前刻意等 N 毫秒，模擬「encoder 初始化很慢的機器」。
    // 沒有它，這台機器量到的偏移只有幾毫秒，沒辦法驗證補償到底對不對。
    int audioDelayMs = 0;

    // 音源清單。每一項最後都會變成容器裡的一條 trak，順序就是 track 索引。
    struct AudioSrcSpec {
        capwgc::AudioSourceKind kind = capwgc::AudioSourceKind::Loopback;
        std::string device;      // 空 = 預設端點
    };
    std::vector<AudioSrcSpec> audioSpecs;
    std::string loopbackDevice;  // --audio-device
    std::string micDevice;       // --mic-device
    bool listAudioDevices = false;

    bool autoSelect = true;
    capwgc::Codec prefer = capwgc::Codec::AV1;
    bool listWindows = false, listMonitors = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if (!strcmp(a, "--out")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            outPath.resize((size_t)n - 1);
            MultiByteToWideChar(CP_UTF8, 0, s, -1, outPath.data(), n);
        } else if (!strcmp(a, "--hwnd")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            hwnd = (HWND)(uintptr_t)_strtoui64(s, nullptr, 16);
        } else if (!strcmp(a, "--monitor")) {
            useMonitor = true;
        } else if (!strcmp(a, "--crop")) {
            const char* s = next();
            if (!s || sscanf(s, "%d,%d,%d,%d", &cropX, &cropY, &cropW, &cropH) != 4) {
                Usage(); return 2;
            }
        } else if (!strcmp(a, "--codec")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (!_stricmp(s, "auto")) {
                autoSelect = true;
            } else if (!_stricmp(s, "av1")) {
                autoSelect = false; prefer = capwgc::Codec::AV1;
            } else if (!_stricmp(s, "hevc") || !_stricmp(s, "h265")) {
                autoSelect = false; prefer = capwgc::Codec::HEVC;
            } else if (!_stricmp(s, "h264") || !_stricmp(s, "avc")) {
                autoSelect = false; prefer = capwgc::Codec::H264;
            } else {
                printf("unknown codec: %s\n", s);
                return 2;
            }
        } else if (!strcmp(a, "--quality")) {
            const char* s = next(); if (s) quality = atoi(s);
        } else if (!strcmp(a, "--bitrate")) {
            const char* s = next(); if (s) bitrate = atoi(s);
        } else if (!strcmp(a, "--fps")) {
            const char* s = next(); if (s) fps = atoi(s);
        } else if (!strcmp(a, "--seconds")) {
            const char* s = next(); if (s) seconds = atoi(s);
        } else if (!strcmp(a, "--gop")) {
            const char* s = next(); if (s) gop = atoi(s);
        } else if (!strcmp(a, "--bframes")) {
            const char* s = next(); if (s) bframes = atoi(s);
        } else if (!strcmp(a, "--engine")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (!_stricmp(s, "sink")) useMft = false;
            else if (!_stricmp(s, "mft")) useMft = true;
            else { printf("unknown engine: %s\n", s); return 2; }
        } else if (!strcmp(a, "--out2")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            outPath2.resize((size_t)n - 1);
            MultiByteToWideChar(CP_UTF8, 0, s, -1, outPath2.data(), n);
        } else if (!strcmp(a, "--ref")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            refPath.resize((size_t)n - 1);
            MultiByteToWideChar(CP_UTF8, 0, s, -1, refPath.data(), n);
        } else if (!strcmp(a, "--q2")) {
            const char* s = next(); if (s) quality2 = atoi(s);
        } else if (!strcmp(a, "--ab")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (!_stricmp(s, "av1")) { abCodec = capwgc::Codec::AV1; abCodecSet = true; }
            else if (!_stricmp(s, "hevc") || !_stricmp(s, "h265")) { abCodec = capwgc::Codec::HEVC; abCodecSet = true; }
            else if (!_stricmp(s, "h264") || !_stricmp(s, "avc")) { abCodec = capwgc::Codec::H264; abCodecSet = true; }
            else { printf("unknown --ab codec: %s\n", s); return 2; }
        } else if (!strcmp(a, "--progressive")) {
            fragmented = false;
        } else if (!strcmp(a, "--no-av-offset")) {
            avOffsetMode = 0;
            avAutoGap = false;      // 連 gap 都不補：純原始時間軸，A/B 對照用
        } else if (!strcmp(a, "--av-offset")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (!_stricmp(s, "auto")) {
                // auto = 讀校準檔。沒有校準檔就不補償，並且明講 —— 靜默當成 0
                // 會讓人以為「已經校準過了」。
                avOffsetMode = 1;
                double ms = 0.0;
                if (ReadCalibration(&ms)) {
                    avOffsetFixed = (int64_t)(ms * 10000.0);
                    avOffsetSource = "校準檔";
                } else {
                    avOffsetMode = 0;
                    avOffsetSource = "沒有校準檔（先跑 out/calibrate.sh）";
                }
            } else {
                avOffsetMode = 2;
                avOffsetFixed = (int64_t)atof(s) * 10000LL;
                avOffsetSource = "--av-offset 指定";
            }
        } else if (!strcmp(a, "--audio-delay")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            audioDelayMs = atoi(s);
            if (audioDelayMs < 0) audioDelayMs = 0;
        } else if (!strcmp(a, "--no-pin-qp")) {
            pinQp = false;
        } else if (!strcmp(a, "--qp")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            rcMode = capwgc::RcMode::ConstantQp;
            qp = atoi(s);
        } else if (!strcmp(a, "--qp2")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            rcMode = capwgc::RcMode::ConstantQp;
            qp2 = atoi(s);
        } else if (!strcmp(a, "--rc")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            if (!_stricmp(s, "qp")) rcMode = capwgc::RcMode::ConstantQp;
            else if (!_stricmp(s, "quality")) rcMode = capwgc::RcMode::Quality;
            else { printf("unknown --rc: %s\n", s); return 2; }
        } else if (!strcmp(a, "--dump-codecapi")) {
            dumpCodecApi = true;
        } else if (!strcmp(a, "--audio")) {
            audioSpecs.push_back({capwgc::AudioSourceKind::Loopback, std::string()});
        } else if (!strcmp(a, "--mic")) {
            audioSpecs.push_back({capwgc::AudioSourceKind::Input, std::string()});
        } else if (!strcmp(a, "--audio-src")) {
            const char* s = next();
            if (!s) { Usage(); return 2; }
            std::string spec = s;
            const size_t colon = spec.find(':');
            std::string kindStr = colon == std::string::npos ? spec : spec.substr(0, colon);
            std::string dev = colon == std::string::npos ? std::string() : spec.substr(colon + 1);
            AudioSrcSpec as;
            if (!_stricmp(kindStr.c_str(), "loopback") || !_stricmp(kindStr.c_str(), "sys")) {
                as.kind = capwgc::AudioSourceKind::Loopback;
            } else if (!_stricmp(kindStr.c_str(), "mic") || !_stricmp(kindStr.c_str(), "input")) {
                as.kind = capwgc::AudioSourceKind::Input;
            } else {
                printf("unknown --audio-src kind: %s（要用 loopback[:dev] 或 mic[:dev]）\n",
                       kindStr.c_str());
                return 2;
            }
            as.device = dev;
            audioSpecs.push_back(std::move(as));
        } else if (!strcmp(a, "--audio-device")) {
            const char* s = next(); if (s) loopbackDevice = s;
        } else if (!strcmp(a, "--mic-device")) {
            const char* s = next(); if (s) micDevice = s;
        } else if (!strcmp(a, "--audio-bitrate")) {
            const char* s = next(); if (s) audioBitrate = atoi(s);
        } else if (!strcmp(a, "--list-audio-devices")) {
            listAudioDevices = true;
        } else if (!strcmp(a, "--cursor")) {
            cursor = true;
        } else if (!strcmp(a, "--list-windows")) {
            listWindows = true;
        } else if (!strcmp(a, "--list-monitors")) {
            listMonitors = true;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            Usage(); return 0;
        } else {
            printf("unknown arg: %s\n", a);
            Usage();
            return 2;
        }
    }

    if (listWindows) {
        std::vector<WinEntry> v;
        EnumWindows(EnumWinProc, (LPARAM)&v);
        printf("%-12s %-8s %s\n", "hwnd", "size", "title");
        for (const auto& e : v) {
            char title[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, e.title.c_str(), -1, title, sizeof(title), nullptr, nullptr);
            printf("0x%08llX  %4dx%-4d %s\n", (unsigned long long)(uintptr_t)e.hwnd,
                   (int)(e.rect.right - e.rect.left), (int)(e.rect.bottom - e.rect.top), title);
        }
        return 0;
    }

    if (listMonitors) {
        std::vector<MonEntry> v;
        EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&v);
        for (size_t i = 0; i < v.size(); ++i) {
            printf("[%zu] %dx%d at (%d,%d)%s\n", i,
                   (int)(v[i].rect.right - v[i].rect.left),
                   (int)(v[i].rect.bottom - v[i].rect.top),
                   (int)v[i].rect.left, (int)v[i].rect.top,
                   v[i].primary ? "  (primary)" : "");
        }
        return 0;
    }

    if (listAudioDevices) {
        std::string aerr;
        const auto devs = capwgc::EnumerateAudioDevices(&aerr);
        if (devs.empty()) {
            printf("no audio endpoints (%s)\n", aerr.empty() ? "none" : aerr.c_str());
            return 1;
        }
        printf("%-8s %-4s %-46s %s\n", "flow", "idx", "name", "note");
        int renderIdx = 0, captureIdx = 0;
        for (const auto& d : devs) {
            const int idx = d.render ? renderIdx++ : captureIdx++;
            printf("%-8s %-4d %-46s%s\n", d.render ? "render" : "capture", idx,
                   d.name.empty() ? "(unnamed)" : d.name.c_str(),
                   d.isDefault ? "  (default)" : "");
        }
        printf("\nrender  = 播放端點，可 loopback（--audio / --audio-device）\n");
        printf("capture = 錄音端點（--mic / --mic-device）\n");
        printf("idx 是該種類裡的序號；也可以直接給名稱子字串或 endpoint ID\n");
        return 0;
    }

    if (outPath.empty()) { Usage(); return 2; }

    HMONITOR hmon = nullptr;
    if (!hwnd) {
        useMonitor = true;
        POINT pt{0, 0};
        hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    if (!hwnd && !hmon) { printf("no capture target\n"); return 2; }

    // ---- D3D11 device
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &ctx);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    printf("D3D11 device OK (feature level 0x%X)\n", (unsigned)fl);

    // 診斷：RDP session 裡的 D3D11 可能不是真的那顆 GPU。
    // 沒有這個資訊，所有效能數字都會被誤讀。
    const capwgc::GpuInfo gpu = capwgc::QueryGpu(device);
    printf("GPU     : %s [%s] %llu MB\n", gpu.name.c_str(), gpu.generation.c_str(),
           (unsigned long long)gpu.vramMB);
    printf("session : %s\n", GetSystemMetrics(SM_REMOTESESSION) ? "RDP (remote)" : "console");

    // ---- 編碼器探測：硬體能力只能問，不能猜
    const auto avail = capwgc::ProbeCodecs();
    if (avail.empty()) {
        printf("no video encoder MFT available\n");
        return 1;
    }
    printf("encoders:\n");
    for (const auto& c : avail) {
        printf("  %-6s %-9s %s\n", capwgc::CodecName(c.codec),
               c.hardware ? "hardware" : "software", c.encoderName.c_str());
    }

    // 候選順序：auto 就照探測結果的優先序；明確指定就把指定項排第一，
    // 後面接其他可用項作為降級鏈（指定項初始化失敗時不會整個錄製失敗）。
    std::vector<capwgc::Codec> candidates;
    if (autoSelect) {
        for (const auto& c : avail) candidates.push_back(c.codec);
    } else {
        candidates.push_back(prefer);
        for (const auto& c : avail) {
            if (c.codec != prefer) candidates.push_back(c.codec);
        }
    }

    const bool mfHasAv1 = !avail.empty() && avail.front().codec == capwgc::Codec::AV1;
    if (!mfHasAv1 && gpu.expectedAv1) {
        printf("note    : %s 的硬體支援 AV1，但 Media Foundation 沒有暴露 AV1 encoder MFT\n",
               gpu.generation.c_str());
        printf("          → 要拿到 AV1 需要 NVENC 直通（M3），目前先降一級\n");
    }
    printf("prefer  : %s%s\n", capwgc::CodecName(candidates.front()),
           autoSelect ? " (auto)" : " (forced)");

    // ---- capture
    capwgc::CaptureConfig ccfg;
    ccfg.hwnd = hwnd;
    ccfg.monitor = hmon;
    ccfg.captureCursor = cursor;
    ccfg.maxFps = fps;

    // ---- 輸出目標
    //
    // 一般情況只有一個。--ab 會多一個：**同一批擷取幀同時餵給兩個編碼器**。
    //
    // 這是唯一能公平比較兩種 codec 的方式。兩次獨立錄製的起始時間不同，動畫
    // 會差好幾幀，逐幀 PSNR 會低到沒有意義（實測 21 dB —— 看起來像其中一個
    // codec 壞了，其實是拿「方塊在左邊」比「方塊在右邊」）。同一批幀進去，
    // 時間軸、內容、色彩轉換全部一致，剩下的差異才真的來自編碼器。
    struct Target {
        std::wstring path;
        capwgc::Codec prefer = capwgc::Codec::H264;
        int quality = -1;          // -1 = 用全域 --quality
        int qp = -1;               // -1 = 用全域 --qp
        std::unique_ptr<capwgc::Encoder> enc;
        capwgc::Codec used = capwgc::Codec::H264;
        uint64_t written = 0;
        std::string lastError;
    };
    std::vector<Target> targets;
    {
        Target t;
        t.path = outPath;
        t.prefer = candidates.front();
        targets.push_back(std::move(t));
        if (abCodecSet) {
            Target t2;
            t2.path = outPath2;
            t2.prefer = abCodec;
            t2.quality = quality2;
            t2.qp = qp2;
            targets.push_back(std::move(t2));
        }
        if (!refPath.empty()) {
            Target t3;
            t3.path = refPath;
            t3.prefer = capwgc::Codec::H264;
            t3.quality = 100;      // QP 0，當作「來源」用
            targets.push_back(std::move(t3));
        }
    }

    bool inited = false;
    int64_t lastPts = 0;
    int64_t lastDur = 0;
    int64_t firstPts = 0;
    uint64_t writeErrors = 0;
    std::string lastError;
    int outW = 0, outH = 0;

    capwgc::WgcCapture capture;

    // ---- 音訊（選用，可多條）
    //
    // 必須在 encoder Open 之前決定：音軌是 moov 的一部分，檔案開好之後
    // 就加不了新的軌。
    //
    // 每一條來源 = 一條 WASAPI 串流 + 一個 AAC 編碼器 + 容器裡的一條 trak。
    // 開不起來的來源直接跳過（沒聲音總比沒畫面好），後面的軌號往前遞補 ——
    // track 索引是「實際開成功的順序」，不是命令列上的順序。
    struct AudioSource {
        capwgc::AudioSourceKind kind = capwgc::AudioSourceKind::Loopback;
        std::string spec;               // 命令列給的裝置（可能是空的）
        std::unique_ptr<capwgc::AudioCapture> cap;
        std::unique_ptr<capwgc::AacEncoder> aac;
        int track = -1;
        bool started = false;
        int64_t startDelay100ns = 0;    // 影像零點 → Start() 回來
        int64_t offset100ns = 0;        // 真正的 A/V 偏移 = 音訊 pts 0 的 QPC − 影像零點
        bool usedQpc = false;           // 上面的值是用 WASAPI 的 QPC 算的（不是猜的）
        bool offsetApplied = false;
        double clockDriftMs = 0.0;      // 音訊裝置時鐘 vs QPC 的差（正值 = 音訊跑得快）
        std::string label;              // 寫進 hdlr 的名字
        std::atomic<uint64_t> au{0};
        std::atomic<uint64_t> errors{0};
        std::string lastError;
    };
    std::vector<std::unique_ptr<AudioSource>> audioSrcs;

    for (size_t si = 0; si < audioSpecs.size(); ++si) {
        const AudioSrcSpec& spec = audioSpecs[si];
        auto s = std::make_unique<AudioSource>();
        s->kind = spec.kind;
        s->spec = spec.device;

        capwgc::AudioCaptureConfig ac;
        ac.kind = spec.kind;
        ac.device = spec.device;
        if (ac.device.empty()) {
            ac.device = spec.kind == capwgc::AudioSourceKind::Input ? micDevice : loopbackDevice;
        }

        std::string aerr;
        s->cap = std::make_unique<capwgc::AudioCapture>();
        if (!s->cap->Open(ac, &aerr)) {
            printf("audio[%zu]: %s unavailable (%s) -- 略過這條音源\n",
                   si, capwgc::AudioSourceKindName(spec.kind), aerr.c_str());
            continue;
        }

        const capwgc::AudioFormat af = s->cap->format();
        s->aac = std::make_unique<capwgc::AacEncoder>();
        if (!s->aac->Open(af.sampleRate, af.channels, audioBitrate, &aerr)) {
            printf("audio[%zu]: %s AAC encoder failed (%s) -- 略過這條音源\n",
                   si, capwgc::AudioSourceKindName(spec.kind), aerr.c_str());
            continue;
        }

        s->track = (int)audioSrcs.size();
        s->label = std::string(capwgc::AudioSourceKindName(spec.kind)) + ": " +
                   s->cap->deviceName();

        printf("audio[%d] : %-8s %s  %d Hz %d ch  asc=%zuB  %s\n",
               s->track, capwgc::AudioSourceKindName(spec.kind), s->cap->deviceName().c_str(),
               af.sampleRate, af.channels, s->aac->config().size(), s->aac->name().c_str());
        audioSrcs.push_back(std::move(s));
    }

    // ---- A/V 偏移量測與補償
    //
    // 兩條時間軸的零點不是同一個瞬間：
    //   影像 t=0 = 第一筆「真的寫進檔案」的幀的擷取時刻
    //   音訊 t=0 = 音訊串流真的開始跑的那一刻
    // 中間隔著 encoder Open（MFT 初始化、muxer 開檔、AAC 建構）—— 這段時間
    // 就是音訊內容比影像晚的量。它隨硬體、驅動、codec 變（同一台機器上跑
    // 不同參數就差 38ms），所以不能寫死，只能量。
    int64_t avInitFrameQpc = 0;    // 初始幀（只拿來開編碼器，不進檔案）
    int64_t avVideoOriginQpc = 0;  // 第一筆寫入的幀 = 檔案時間 0
    int64_t avAudioDelay = 0;      // >0 = 音訊比影像零點晚開始 = 要補償的量
    bool avFirstWriteSeen = false;

    // ---- 把真正的 A/V 偏移寫進容器
    //
    // 為什麼要等到「第一個 AAC AU 產生」才做，而不是 Start() 一回來就做：
    // loopback 的 pre-roll 只有在第一次 Drain 之後才量得到（見 audio_capture.cpp
    // 的 Drain）。Start() 剛回來時那個數字還是未知的，而那時算出來的偏移會多算
    // 一整個 pre-roll —— 實測這台是 66ms，比要補的量還大。
    //
    // 時序上安全：一個 AU 要 1024 個取樣點（44.1k 下 23ms），第一次 Drain 在
    // Start() 後 1ms 內就完成了，所以呼叫這裡的時候 pre-roll 一定已經有值，
    // 而且容器裡還沒有任何樣本（SetAudioStartOffset 會拒絕晚到的設定）。
    auto applyOffset = [&](AudioSource* s) {
        if (s->offsetApplied) return;
        s->offsetApplied = true;

        const int64_t lat = s->cap->streamLatency100ns();
        // 偏移 = 「音訊 pts 0 的牆上時刻」−「影像時間軸 0 的牆上時刻」。
        // 前者由 WASAPI 的第一個 packet QPC 直接給（firstPacketQpc100ns），
        // 後者是 avVideoOriginQpc。兩個都是 QPC 同一個時鐘，直接相減。
        //
        // 不要退回「startDelay − 猜的 pre-roll」：那條路走過，量到 10.2ms
        // 的 pre-roll 而真實差距 60ms，結果每一份檔案都多補 50~60ms ——
        // A/B 對照量出來是「不補償 −1.3ms、補償 +53.8ms」，補償反而把
        // 對齊的檔案弄歪。猜一個量不如用驅動告訴我們的時間。
        const int64_t q0 = s->cap->firstPacketQpc100ns();
        const int64_t raw = (q0 != 0) ? (q0 - avVideoOriginQpc) : (s->startDelay100ns - lat);
        // 這個值只印出來對照（不是最終寫進容器的量）。保留正負號：截成 0
        // 會把「音訊偏早」的診斷資訊蓋掉。
        s->offset100ns = raw;
        s->usedQpc = (q0 != 0);

        // 寫進容器的量 = 自動補的 gap + 使用者指定的校正值。
        //
        // gap 的意義：音軌第一筆樣本的內容時刻是 avVideoOriginQpc + raw，但
        // muxer 一定把它放在 dts=0，所以整條音軌早了 raw。把 raw 往後推就回到
        // 「檔案時間 0 的音訊內容 == 影像內容」。
        //
        // 校正值（--av-offset / 校準檔）疊在上面：那是「這個裝置/播放器還要再
        // 微調多少」的經驗值，與 gap 無關，所以是相加而不是取代。
        const int64_t d = (avAutoGap ? raw : 0) +
                          ((avOffsetMode == 0) ? 0 : avOffsetFixed);
        printf("offset[%d] : gap=%+.1fms cal=%+.1fms -> %+.1fms%s\n", s->track,
               raw / 10000.0, (double)avOffsetFixed / 10000.0, d / 10000.0,
               avAutoGap ? "" : "（--no-av-offset：不補 gap）");
        for (auto& tg : targets) {
            if (!tg.enc || s->track >= tg.enc->audioTrackCount()) continue;
            if (!tg.enc->SetAudioOffset(s->track, d)) {
                s->errors.fetch_add(1);
                s->lastError = "SetAudioOffset rejected: audio samples already written";
                printf("audio[%d] : 延遲沒寫進容器（樣本先到了）\n", s->track);
            }
        }
    };

    auto startAudio = [&]() {
        for (auto& sp : audioSrcs) {
            AudioSource* s = sp.get();
            if (s->started) continue;
            std::string aerr;
            // 測試用：模擬「這台機器開編碼器很慢」，把音訊起跑往後推。
            if (audioDelayMs > 0) Sleep((DWORD)audioDelayMs);
            const bool ok = s->cap->Start(
                [s, &targets, &applyOffset](const int16_t* pcm, size_t frames, int64_t pts) {
                    std::string eerr;
                    if (!s->aac->Encode(pcm, frames, pts,
                                        [s, &targets, &applyOffset](const uint8_t* d, size_t n,
                                                                    int64_t t) {
                            // 第一個 AU 之前把偏移定案（那時才量得到 pre-roll）
                            applyOffset(s);
                            // AU 數是「這條來源產生了幾個 AU」，不是「寫了幾次」：
                            // --ab 同時寫兩個檔時，同一個 AU 會被寫兩次，寫在迴圈裡
                            // 數就會變成兩倍（au=428 對上 pcm=5.02s 就是這個症狀）。
                            bool any = false;
                            for (auto& tg : targets) {
                                if (!tg.enc || s->track >= tg.enc->audioTrackCount()) continue;
                                std::string werr;
                                if (tg.enc->WriteAudio(s->track, d, n, 1024, t, &werr)) {
                                    any = true;
                                } else {
                                    s->errors.fetch_add(1);
                                    s->lastError = werr;
                                }
                            }
                            if (any) s->au.fetch_add(1);
                        }, &eerr)) {
                        s->errors.fetch_add(1);
                        s->lastError = eerr;
                    }
                }, &aerr);
            s->started = ok;
            if (!ok) {
                printf("audio[%d] : start failed (%s)\n", s->track, aerr.c_str());
                continue;
            }

            // 這條軌的音訊內容從「影像零點之後多久」才開始。多條軌之間如果差
            // 很多，就是它們彼此不同步 —— 這個數字比 A/V 偏移更該貼近 0。
            //
            // 注意這個值還不是 A/V 偏移：它量的是「影像零點 → Start() 回來」，
            // 而 loopback 擷取到的內容天生比「現在」晚一個 pre-roll。真正的
            // 偏移要扣掉那一段，在 applyOffset() 裡算（那時才量得到）。
            s->startDelay100ns = Qpc100ns() - avVideoOriginQpc;
        }
    };

    auto onFrame = [&](const capwgc::CapturedFrame& f) {
        if (!inited) {
            // 第一幀才知道 item 尺寸
            const int64_t qpcFirstFrame = Qpc100ns();
            avInitFrameQpc = qpcFirstFrame;
            const int w = cropW > 0 ? cropW : (int)f.width;
            const int h = cropH > 0 ? cropH : (int)f.height;

            capwgc::EncoderConfig ec;
            ec.width = w;
            ec.height = h;
            ec.cropX = cropX;
            ec.cropY = cropY;
            ec.nominalFps = fps;
            ec.quality = quality;
            ec.bitrate = bitrate;
            ec.gopSeconds = gop;
            ec.bFrames = bframes;
            ec.fragmented = fragmented;
            ec.pinQp = pinQp;
            ec.rcMode = rcMode;
            ec.qp = qp;

            ec.audioTracks.clear();
            for (const auto& sp : audioSrcs) {
                capwgc::AudioTrackConfig at;
                at.sampleRate = sp->cap->format().sampleRate;
                at.channels = sp->cap->format().channels;
                at.decoderConfig = sp->aac->config();
                at.name = sp->label;
                ec.audioTracks.push_back(std::move(at));
            }

            // 依候選順序逐個試。第一個能初始化成功的就用 —— 這樣「有 AV1 就用
            // AV1」不需要在編譯期知道是哪張卡，也不會因為首選不可用就整個錄製失敗。
            for (auto& t : targets) {
                std::vector<capwgc::Codec> order;
                order.push_back(t.prefer);
                for (capwgc::Codec c : candidates) {
                    if (c != t.prefer) order.push_back(c);
                }
                ec.quality = t.quality >= 0 ? t.quality : quality;
                ec.qp = t.qp > 0 ? t.qp : qp;
                for (capwgc::Codec c : order) {
                    ec.codec = c;
                    std::unique_ptr<capwgc::Encoder> e;
                    if (useMft) e = std::make_unique<capwgc::MftEncoder>();
                    else        e = std::make_unique<capwgc::Mp4Encoder>();
                    std::string oerr;
                    if (e->Open(t.path, ec, device, &oerr)) {
                        t.enc = std::move(e);
                        t.used = c;
                        break;
                    }
                    fprintf(stderr, "[enc] %s unavailable: %s\n",
                            capwgc::CodecName(c), oerr.c_str());
                    t.lastError = oerr;
                }
                if (!t.enc) { g_stop.store(true); return; }
            }

            outW = ec.width;
            outH = ec.height;
            firstPts = f.pts100ns;
            lastPts = f.pts100ns;
            printf("capturing item %ux%u -> %dx%d @(%d,%d)\n",
                   f.width, f.height, outW, outH, cropX, cropY);
            for (const auto& t : targets) {
                std::string encName = t.enc->encoderName();
                if (encName.empty() || encName == "?") {
                    for (const auto& ac : avail) {
                        if (ac.codec == t.used) { encName = ac.encoderName; break; }
                    }
                }
                printf("using   : %-6s %-30s -> %ls\n", t.enc->codecName().c_str(),
                       encName.empty() ? "(unknown)" : encName.c_str(), t.path.c_str());
            }
            inited = true;
            // 音訊刻意不在這裡起跑。檔案時間的零點是「第一筆寫入的幀」，而
            // 那一幀要等下一幀到了才知道是哪一刻 —— 音訊若在這裡就開始，量到
            // 的延遲沒有基準可減，容器也還沒機會把延遲寫進去。晚一幀（~16.7ms）
            // 的代價只是開頭多丟一小段，換來的是延遲量得準、補得掉。
            return;   // 第一幀只拿來初始化，不寫入（duration 未知）
        }

        if (f.formatChanged) {
            lastError = "capture size changed mid-recording; encoder rebuild not implemented";
            g_stop.store(true);
            return;
        }

        int64_t dur = f.pts100ns - lastPts;
        if (dur <= 0) dur = 10000000LL / fps;
        lastPts = f.pts100ns;
        lastDur = dur;

        if (!avFirstWriteSeen) {
            avFirstWriteSeen = true;
            // 影像時間軸的零點定在這一幀。容器裡第一筆影像樣本的 decode time
            // 恆為 0（stts 只存相鄰差），所以「這一幀的擷取時刻」才等於檔案
            // 時間 0。拿初始幀當零點會讓整條影像軌虛報早一幀，音軌也就跟著
            // 多補一幀的延遲 —— 那就是過度補償。
            firstPts = f.pts100ns;
            avVideoOriginQpc = f.pts100ns;

            const int64_t qpcBeforeAudio = Qpc100ns();
            startAudio();
            const int64_t qpcAfterAudio = Qpc100ns();
            // 影像 pts 用 WGC 的 SystemRelativeTime（和 QPC 同一個時鐘、同一個
            // 原點），所以「音訊起點 − 影像零點」直接就是延遲，不用再校正。
            avAudioDelay = qpcAfterAudio - avVideoOriginQpc;
            if (!audioSrcs.empty()) {
                printf("sync    : enc_open=%.1fms  audio_start=%.1fms  "
                       "raw_delay=%.1fms（是管線時間差，不是音畫差）\n",
                       (qpcBeforeAudio - avInitFrameQpc) / 10000.0,
                       (qpcAfterAudio - avVideoOriginQpc) / 10000.0,
                       avAudioDelay / 10000.0);
                // 絕對時刻：讓 anim_window 的 anim_sync.log 可以直接對照，
                // 分辨「是影像軌還是音軌偏掉」。沒有這個數字就只能互相推導，
                // 而互相推導會把「PlaySound 的物理延遲」算進去。
                printf("sync    : videoOrigin=%lld  firstFrameQpc=%lld\n",
                       (long long)avVideoOriginQpc, (long long)avInitFrameQpc);
            }
        }

        for (auto& t : targets) {
            std::string err;
            if (!t.enc->Write(f.texture, f.pts100ns - firstPts, dur, &err)) {
                writeErrors++;
                t.lastError = err;
                lastError = err;
            } else {
                t.written++;
            }
        }
        if (writeErrors > 30) g_stop.store(true);
    };

    std::string err;
    if (!capture.Start(ccfg, device, onFrame, &err)) {
        printf("capture start failed: %s\n", err.c_str());
        return 1;
    }
    printf("capture caps: %s\n", capture.caps().c_str());

    SetConsoleCtrlHandler(OnCtrl, TRUE);

    // ---- 主迴圈
    const DWORD t0 = GetTickCount();
    const double cpu0 = ProcCpuMs();
    DWORD lastReport = t0;

    while (!g_stop.load()) {
        Sleep(20);
        if (capture.closed()) {
            printf("capture item closed -> stop\n");
            break;
        }
        const DWORD now = GetTickCount();
        if (seconds > 0 && (now - t0) >= (DWORD)seconds * 1000) break;

        if (now - lastReport >= 1000) {
            lastReport = now;
            const double secs = (now - t0) / 1000.0;
            printf("[%5.1fs] captured=%llu dropped=%llu throttled=%llu",
                   secs,
                   (unsigned long long)capture.frames(),
                   (unsigned long long)capture.dropped(),
                   (unsigned long long)capture.throttled());
            for (const auto& t : targets) {
                const uint64_t b = t.enc ? t.enc->bytes() : 0;
                printf("  %s=%.2fMB", t.enc ? t.enc->codecName().c_str() : "?", b / 1048576.0);
            }
            if (!audioSrcs.empty()) {
                printf("  aud=");
                for (const auto& sp : audioSrcs) {
                    printf("%s%lluAU/%lluer", sp->track == 0 ? "" : " ",
                           (unsigned long long)sp->au.load(),
                           (unsigned long long)sp->errors.load());
                }
            }
            printf("\n");
        }
    }

    const double cpu1 = ProcCpuMs();
    const double secs = (GetTickCount() - t0) / 1000.0;

    capture.Stop();

    // 每一條來源各自收尾：先停串流，再把編碼器裡剩下的 AU 沖出來。
    //
    // 沖的時候 muxer 還開著（encoder 還沒 Close），所以寫得進去。
    // 補到「影像的長度」：兩條軌的時長不一致時，播放器通常會以短的為準，
    // 聽起來就是尾巴被切掉。補靜音比少一段安全。每條軌的取樣率可能不同，
    // 所以換算要用各自的 rate。
    const int64_t video100 = (lastPts > firstPts ? lastPts - firstPts : 0) + lastDur;
    for (auto& sp : audioSrcs) {
        AudioSource* s = sp.get();
        if (!s->started) continue;
        s->cap->Stop();

        // 音訊裝置時鐘 vs QPC 的漂移：累計取樣點 / 取樣率 與牆鐘經過的時間比。
        // 短錄影看不出來，長錄影會變成「聲音慢慢超前畫面」。
        const int64_t qpcStop = Qpc100ns();
        const double wallSec =
            (qpcStop - (avVideoOriginQpc + s->startDelay100ns)) / 10000000.0;
        const double pcmSec = (double)s->cap->frames() / (double)s->cap->format().sampleRate;
        s->clockDriftMs = (pcmSec - wallSec) * 1000.0;

        const int64_t targetFrames =
            video100 * (int64_t)s->cap->format().sampleRate / 10000000;
        std::string ferr;
        s->aac->Flush(targetFrames, [s, &targets, &applyOffset](const uint8_t* d, size_t n,
                                                               int64_t t) {
            applyOffset(s);   // 全程沒有 AU 時，收尾這一批才補設定
            bool any = false;
            for (auto& tg : targets) {
                if (!tg.enc || s->track >= tg.enc->audioTrackCount()) continue;
                std::string werr;
                if (tg.enc->WriteAudio(s->track, d, n, 1024, t, &werr)) any = true;
                else s->errors.fetch_add(1);
            }
            if (any) s->au.fetch_add(1);
        }, &ferr);
        s->aac->Close();
    }

    for (auto& t : targets) {
        std::string cerr;
        if (t.enc && !t.enc->Close(&cerr)) {
            printf("encoder close failed (%ls): %s\n", t.path.c_str(), cerr.c_str());
            t.lastError = cerr;
        }
    }

    uint64_t samples = 0;
    for (const auto& t : targets) samples = std::max<uint64_t>(samples, t.enc ? t.enc->samples() : 0);

    printf("\n================ done ================\n");
    printf("duration      : %.2f s\n", secs);
    printf("captured      : %llu frames (%.1f fps)\n",
           (unsigned long long)capture.frames(),
           secs > 0 ? capture.frames() / secs : 0.0);
    printf("dropped(busy) : %llu\n", (unsigned long long)capture.dropped());
    printf("throttled     : %llu (幀率閘門，內容重複非損失)\n",
           (unsigned long long)capture.throttled());
    printf("write errors  : %llu\n", (unsigned long long)writeErrors);
    printf("cpu           : %.1f ms total, %.3f ms/frame\n",
           cpu1 - cpu0, samples > 0 ? (cpu1 - cpu0) / (double)samples : 0.0);
    printf("engine        : %s\n", useMft ? "mft (self-driven IMFTransform + own muxer)"
                                            : "sink (SinkWriter)");
    if (!audioSrcs.empty()) {
        // 三個數字都要印：
        //   raw_delay  = 影像零點 → Start() 回來（含開編碼器的空檔，不是音畫差）
        //   audio_qpc0 = 音訊 pts 0 的牆上時刻 − 影像零點（這才是音畫差）
        //   offset     = 實際寫進容器的量
        // 只看 raw_delay 會補錯 —— 它含了 encoder Open 的時間，而那一段對
        // 影像與音訊是同時發生的，不是偏移。
        const auto* s0 = audioSrcs.front().get();
        printf("a/v offset    : raw_delay=%.1f ms  qpc0=%.1f ms（兩個都不是音畫差）  "
               "寫入=%+.1f ms  (%s)\n",
               avAudioDelay / 10000.0,
               s0->offset100ns / 10000.0,
               (double)avOffsetFixed / 10000.0,
               avOffsetSource.c_str());
        if (avOffsetMode != 0 && avOffsetFixed >= 0) {
            printf("              寫進容器：progressive 走 edts/elst 的 empty edit，"
                   "fMP4 走第一個 tfdt\n");
        } else if (avOffsetMode != 0) {
            printf("              寫進容器：負偏移 = 把音訊開頭裁掉 %+.1f ms —— "
                   "progressive 用 elst 的 media_time（資料不丟），"
                   "fMP4 真的丟 AU（精度到 AU 邊界 %.1f ms）\n",
                   (double)avOffsetFixed / 10000.0,
                   1024.0 * 1000.0 / (double)s0->cap->format().sampleRate);
        } else {
            printf("              沒寫進容器：推導值不可信，預設不補償（見 docs/08 §6.6）\n");
        }
        if (avOffsetMode != 0) {
            printf("              這個值是 per-machine 量測結果，換硬體／驅動要重新量"
                   "（out/calibrate.sh）\n");
        }
    }

    for (const auto& t : targets) {
        const uint64_t bytes = t.enc ? t.enc->bytes() : 0;
        printf("output        : %-6s %8.2f MB  %7.3f Mbps  %6llu samples  %ls\n",
               t.enc ? t.enc->codecName().c_str() : "?", bytes / 1048576.0,
               secs > 0 ? (bytes * 8.0 / secs / 1e6) : 0.0,
               (unsigned long long)(t.enc ? t.enc->samples() : 0), t.path.c_str());
        if (t.enc) {
            const std::string st = t.enc->stats();
            if (!st.empty()) printf("  enc stats   : %s\n", st.c_str());
            // 音訊 AU 的「產生數」與「寫進容器數」不是同一個數字：負偏移的裁切
            // 會在 muxer 裡吃掉開頭幾個 AU（fMP4）或只改 elst（progressive）。
            // 兩個都印出來，才看得出裁切到底有沒有發生。
            if (t.enc->audioTrackCount() > 0) {
                printf("  audio AUs   : %llu written to container\n",
                       (unsigned long long)t.enc->audioSamples());
            }
            // 碼控沒生效時，兩個 codec 的檔案大小根本不可比 —— 那是兩邊各自跑
            // 預設值，不是「同一個 quality 下的兩種編碼」。這種比較比不比更糟。
            auto* me = dynamic_cast<capwgc::MftEncoder*>(t.enc.get());
            if (me && !me->qualityModeActive()) {
                printf("  !! 碼控沒生效：RateControlMode 讀回不是 Quality。\n"
                       "     這個檔案的大小不能拿來跟另一種 codec 比。\n");
            }
        }
        if (!t.lastError.empty()) printf("  last error  : %s\n", t.lastError.c_str());
    }
    for (const auto& sp : audioSrcs) {
        const AudioSource* s = sp.get();
        // qpc0 印的是「相對影像零點」而不是絕對 QPC —— 絕對值是一個上億的數，
        // 單看沒有意義，還會讓人以為單位錯了。
        printf("audio track %d   : %-8s raw=+%.1fms qpc0=+%.1fms（都不是音畫差）  "
               "drift=%+.1fms  %llu AU  %s  errors=%llu\n",
               s->track, capwgc::AudioSourceKindName(s->kind),
               s->startDelay100ns / 10000.0,
               s->offset100ns / 10000.0, s->clockDriftMs,
               (unsigned long long)s->au.load(), s->aac->stats().c_str(),
               (unsigned long long)s->errors.load());
        if (!s->lastError.empty()) printf("  audio error : %s\n", s->lastError.c_str());
        // 絕對錨點：和 anim_sync.log 的 beep QPC 直接相減，就能算出
        // 「音訊內容在檔案時間 0 的真實位置」，不必再猜 PlaySound 的延遲。
        printf("audio anchor %d : firstPacketQpc=%lld  (%s)\n", s->track,
               (long long)s->cap->firstPacketQpc100ns(),
               s->usedQpc ? "QPC" : "fallback");
    }

    if (!lastError.empty()) printf("last error    : %s\n", lastError.c_str());

    if (ctx) ctx->Release();
    if (device) device->Release();
    MFShutdown();
    CoUninitialize();
    return lastError.empty() ? 0 : 1;
}
