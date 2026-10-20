// ============================================================================
// anim_window.cpp — 確定性內容變化源
// ============================================================================
//
// 為什麼需要它：WGC 是變更驅動的，靜止桌面幾乎不產幀。要量編碼器的
// 壓縮率與 CPU，必須有一個「每秒變化次數已知」的內容源，否則兩次跑的
// 結果不能比。
//
// 畫面刻意做成桌面 UI 的樣子（淺底、深色文字、移動的色塊），
// 因為錄屏的壓縮率主要取決於內容類型 —— 用遊戲畫面測出來的數字
// 對「錄 PPT」沒有參考價值。
//
// 用法：anim_window.exe [--size 800x600] [--seconds 30] [--hard]
//
// --hard 會把內容換成高熵版本（漸層 + 64 個移動方塊 + 每幀變動的密集文字）。
// 預設的內容在 720p 只有 100 kbps 左右，在那個位元率下量 codec 的壓縮效率
// 會被標頭開銷主導 —— 測出來的差異不是編碼器本身的差異。
//
// --sync 是「A/V 同步測試源」：整屏黑↔白反轉的同一瞬間播一聲哔。
// 為什麼不另外寫一支 GUI：WGC 只在畫面有變化時出幀，純反轉的畫面靜止 500ms，
// 擷取會掉到 2~3 fps，於是「第一筆寫入的幀」落在半秒之後，量出來的 A/V 延遲
// 變成 0。所以反轉之外還留著一條一直在跑的方塊，保證每幀都有變化。
// 錄下來用 mp4_check.exe --sync 量兩邊事件的呈現時間。
// 輸出：hwnd=0x... 到 stdout，餵給 CapCli --hwnd
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <timeapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static int g_frame = 0;
static int g_hard = 0;
static int g_borderless = 0;   // --borderless：WS_POPUP，讓 1080p 塞得進螢幕
static int g_sync = 0;         // --sync：整屏反轉 + 哔聲（A/V 同步測試源）
static int g_invert = 0;
static int g_flips = 0;

// --sync 時把「每一次反轉的牆上時刻」寫出來，讓錄影端可以拿自己的 origin 對照。
//
// 為什麼需要：mp4_check --sync 只能量「檔案內部」的音畫差，量不出「哪一條軌的
// 時間軸在說謊」。把 flip 的真實 QPC 與 beep 的真實 QPC 記下來，兩邊各自跟
// 錄影端記的 videoOrigin / firstPacketQpc 相減，就能分辨是影像軌還是音軌偏移。
static FILE* g_qlog = nullptr;
static long long g_qpcFreq = 0;

static long long Qpc100() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    if (g_qpcFreq <= 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart ? f.QuadPart : 1;
    }
    return (long long)((double)n.QuadPart * 10000000.0 / (double)g_qpcFreq);
}

// ============================================================================
// 樣本精確的嗶聲來源
// ============================================================================
//
// 這裡走過一段冤枉路，值得寫清楚：原本用
//   PlaySoundW(wav, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)
// 每次反轉播一次。PlaySound **呼叫**回來只要 1~3ms，所以當初就認為「啟動延遲
// 1~3ms」。但那是呼叫的延遲，不是出聲的延遲 —— 實際量到「呼叫 → 嗶聲出現在
// loopback 串流」是 85~90ms（每次都要重開裝置）。
//
// 後果非常惡劣：那個延遲會被量測端原封不動算成「音訊偏晚」。於是
//   - 一份完全對齊的檔案被量成 +85ms；
//   - CLI 那條路的音軌剛好也早 89ms（見 recorder.cpp 的 applyAudioOffset），
//     兩個誤差互相抵掉 → 量成 +5ms「對齊」，bug 被藏了好幾輪。
//
// 現在的做法：把嗶聲**寫進一條一直在播的靜音串流**（waveOut），並用
// waveOutGetPosition 讀「已經播到第幾個樣點」。畫面的反轉就發生在播放位置
// 到達嗶聲起點的那一刻。
//
// 這樣輸出延遲同時作用在兩邊而自動抵消 —— 我們不需要知道裝置延遲是多少，
// 只需要知道「第 N 個樣點什麼時候播出來」，而那正是 waveOutGetPosition 說的。
//
// 殘差只剩：輪詢粒度（~1ms）＋ DWM 合成延遲（畫面比 UpdateWindow 晚幾 ms
// 才真的顯示）。實測總殘差 < 15ms，比原本的 90ms 好一個數量級。
//
// `--flip-lead`（預設 36ms）就是這個殘差的校正量：把反轉提前幾個樣點，讓
// 「畫面被看到」與「嗶聲被聽到」落在同一刻。它只補「位置回報落後 − 合成延遲」
// 的差，與錄影端無關 —— 錄影端不該補這個。
static HWAVEOUT g_wo = nullptr;
static const int kSyncRate = 44100;
static const int kSyncBuf = 512;       // 每個緩衝 11.6ms
static const int kSyncNbuf = 6;        // 佇列深度 ~70ms
static std::vector<short> g_pat;       // 一個週期（立體聲交錯，開頭是嗶聲）
static int g_patLen = 0;               // 週期樣點數
static WAVEHDR g_wh[kSyncNbuf] = {};
static std::vector<short> g_wb[kSyncNbuf];
static int64_t g_written = 0;          // 已寫進串流的樣點數
static int64_t g_flipAt = 0;           // 下一次反轉的串流位置（0 = 還沒定）
static int g_flipLead = 0;             // 反轉要提前幾個樣點（補位置回報的落後）
static double g_flipLeadMs = 36.0;     // 上面那個值的來源（ms）
static bool g_audioOk = false;

static void SyncAudioBuild(int intervalMs, int beepMs) {
    g_patLen = kSyncRate * intervalMs / 1000;
    if (g_patLen < kSyncRate / 10) g_patLen = kSyncRate / 10;
    g_pat.assign((size_t)g_patLen * 2, 0);

    // 參數為什麼是這樣（都是被量測失敗逼出來的，見 docs/08 §6.5）：
    //   freq = 3000 Hz  量測端用 Goertzel 只取這個窄頻帶。環境底噪在這台機器上
    //                   實測 RMS −35 dBFS、峰值 −10.6 dBFS，寬頻峰值法分不出
    //                   嗶聲；窄帶 + 高頻就分得開。
    //   ms   = 150      40ms 只跨 2 個 AAC AU（23ms 一個），能量在中間會掉回
    //                   門檻，同一個嗶被數成兩次。150ms 橫跨 6 個 AU，起點明確。
    //   amp  = 0.85     原本 0.45（−6.9 dBFS）只比底噪峰值高 4 dB，太危險。
    const int n = kSyncRate * beepMs / 1000;
    const int fade = kSyncRate * 3 / 1000;
    for (int i = 0; i < n && i < g_patLen; ++i) {
        double env = 1.0;
        if (i < fade) env = (double)i / fade;
        else if (i > n - fade) env = (double)(n - i) / fade;
        double v = 0.85 * env * sin(2.0 * 3.14159265358979 * 3000.0 * i / kSyncRate);
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        const short s = (short)(v * 32767.0);
        g_pat[(size_t)i * 2 + 0] = s;
        g_pat[(size_t)i * 2 + 1] = s;
    }
}

static void SyncAudioFill(int idx) {
    std::vector<short>& b = g_wb[idx];
    b.resize((size_t)kSyncBuf * 2);
    for (int i = 0; i < kSyncBuf; ++i) {
        const int p = (int)((g_written + i) % g_patLen);
        b[(size_t)i * 2 + 0] = g_pat[(size_t)p * 2 + 0];
        b[(size_t)i * 2 + 1] = g_pat[(size_t)p * 2 + 1];
    }
    g_written += kSyncBuf;
}

static bool SyncAudioStart(int intervalMs) {
    SyncAudioBuild(intervalMs, 150);
    g_flipLead = (int)(g_flipLeadMs * (double)kSyncRate / 1000.0);

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kSyncRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = kSyncRate * 4;

    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wo = nullptr;
        return false;
    }
    for (int i = 0; i < kSyncNbuf; ++i) {
        SyncAudioFill(i);
        g_wh[i].lpData = (LPSTR)g_wb[i].data();
        g_wh[i].dwBufferLength = (DWORD)(g_wb[i].size() * sizeof(short));
        if (waveOutPrepareHeader(g_wo, &g_wh[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            return false;
        }
        waveOutWrite(g_wo, &g_wh[i], sizeof(WAVEHDR));
    }
    g_flipAt = 0;
    g_audioOk = true;
    return true;
}

// 把播完的緩衝填新內容再排回去。回傳目前已經寫到第幾個樣點。
static void SyncAudioPump() {
    if (!g_wo) return;
    for (int i = 0; i < kSyncNbuf; ++i) {
        if (!(g_wh[i].dwFlags & WHDR_DONE)) continue;
        g_wh[i].dwFlags &= ~WHDR_DONE;
        SyncAudioFill(i);
        g_wh[i].lpData = (LPSTR)g_wb[i].data();
        g_wh[i].dwBufferLength = (DWORD)(g_wb[i].size() * sizeof(short));
        waveOutWrite(g_wo, &g_wh[i], sizeof(WAVEHDR));
    }
}

static int64_t SyncAudioPos() {
    if (!g_wo) return -1;
    MMTIME t{};
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(g_wo, &t, sizeof(t)) != MMSYSERR_NOERROR) return -1;
    return (int64_t)t.u.sample;
}

static void SyncAudioStop() {
    if (!g_wo) return;
    waveOutReset(g_wo);
    for (int i = 0; i < kSyncNbuf; ++i) {
        if (g_wh[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_wh[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_wo);
    g_wo = nullptr;
    g_audioOk = false;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);

        if (g_sync) {
            // 整屏反轉（同步事件）+ 一直在跑的方塊（讓 WGC 每幀都有變化）
            const COLORREF bgc = g_invert ? RGB(255, 255, 255) : RGB(16, 16, 16);
            const COLORREF fgc = g_invert ? RGB(0, 0, 0) : RGB(255, 255, 255);
            HBRUSH bg = CreateSolidBrush(bgc);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, fgc);

            // 反轉次數：暫停畫面時可以讀出「第幾次」，跟音軌裡第幾個哔對照
            HFONT big = CreateFontA(-(rc.bottom * 45 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH, "Consolas");
            HGDIOBJ oldFont = SelectObject(dc, big);
            char num[32];
            snprintf(num, sizeof(num), "%d", g_flips);
            DrawTextA(dc, num, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(big);

            // 每幀都換位置的方塊。少了它，反轉之間畫面是靜止的，WGC 會把
            // 擷取降到 2~3 fps（實測 8 秒只收到 22 幀），於是「第一筆寫入的
            // 幀」落在半秒之後，量出來的 A/V 延遲變成 0 甚至負的。
            const int bw = 120;
            const int bx = (g_frame * 13) % (rc.right > bw ? rc.right - bw : 1);
            HBRUSH bb = CreateSolidBrush(g_invert ? RGB(190, 20, 20) : RGB(235, 60, 60));
            RECT br{bx, 0, bx + bw, rc.bottom};
            FillRect(dc, &br, bb);
            DeleteObject(bb);

            HFONT small = CreateFontA(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, DEFAULT_PITCH, "Consolas");
            oldFont = SelectObject(dc, small);
            char info[96];
            snprintf(info, sizeof(info), "sync flip=%d frame=%d", g_flips, g_frame);
            TextOutA(dc, 12, 12, info, (int)strlen(info));
            SelectObject(dc, oldFont);
            DeleteObject(small);

            EndPaint(h, &ps);
            return 0;
        }

        HBRUSH bg = CreateSolidBrush(RGB(248, 248, 250));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(dc, TRANSPARENT);

        // 一行標題文字（高頻細節，測文字壓縮）
        SetTextColor(dc, RGB(24, 24, 28));
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "CapWGC codec benchmark   frame %06d   %dx%d   %s",
                 g_frame, (int)rc.right, (int)rc.bottom,
                 g_frame % 2 ? "ALPHA" : "BRAVO");
        TextOutA(dc, 32, 24, buf, (int)strlen(buf));

        // 大面積單色塊（測平坦區域的碼率）
        HBRUSH panel = CreateSolidBrush(RGB(236, 238, 242));
        RECT pr{32, 64, rc.right - 32, rc.bottom - 32};
        FillRect(dc, &pr, panel);
        DeleteObject(panel);

        if (g_hard) {
            // 高熵模式：漸層 + 大量小方塊 + 每幀變動的密集文字。
            //
            // 為什麼需要它：原本的內容（兩塊色塊 + 一行字）在 720p 只有
            // 100 kbps 左右，那個位元率下 HEVC 的標頭與 CTU 開銷佔比太高，
            // 測出來的「HEVC 比較差」只反映了極低位元率，不是編碼器本身。
            // 真實錄屏（視窗裡有影片、有滾動、有圖表）的位元率是它的 20 倍。
            const int bandH = 6;
            for (int y = pr.top; y < pr.bottom; y += bandH) {
                const int t = (y + g_frame * 3) & 0xFF;
                HBRUSH b = CreateSolidBrush(RGB(60 + t / 2, 90 + (t * 3) % 120, 140 + t / 3));
                RECT r{pr.left, y, pr.right, y + bandH};
                FillRect(dc, &r, b);
                DeleteObject(b);
            }

            // 64 個各自移動的小方塊：運動補償 + 遮蔽，接近真實影片的預測難度
            for (int i = 0; i < 64; ++i) {
                const int dir = (i * 7) % 4;
                const int spd = 3 + (i % 5);
                const int ph = g_frame * spd + i * 37;
                int x, y;
                switch (dir) {
                    case 0: x = (ph * 5) % (rc.right - 80); y = 100 + (i % 8) * 60; break;
                    case 1: x = rc.right - 120 - (ph * 4) % (rc.right - 160); y = 100 + (i % 8) * 60; break;
                    case 2: x = 80 + (i % 12) * 90; y = (ph * 6) % (rc.bottom - 140); break;
                    default: x = 80 + (i % 12) * 90; y = rc.bottom - 140 - (ph * 5) % (rc.bottom - 200); break;
                }
                HBRUSH b = CreateSolidBrush(RGB((i * 53) & 0xFF, (i * 97) & 0xFF, (i * 29) & 0xFF));
                RECT r{x, y, x + 48, y + 48};
                FillRect(dc, &r, b);
                DeleteObject(b);
            }

            // 每幀都不同的密集文字：把高頻細節的熵拉上來
            SetTextColor(dc, RGB(255, 255, 255));
            for (int i = 0; i < 14; ++i) {
                snprintf(buf, sizeof(buf),
                         "%03d  the quick brown fox jumps over the lazy dog  %08X  %d.%03d",
                         i, (unsigned)(g_frame * 2654435761u + i * 40503u), g_frame, i * 37);
                TextOutA(dc, 48, 80 + i * 22, buf, (int)strlen(buf));
            }
        }

        // 水平移動的方塊（測運動補償）
        int x = (g_frame * 9) % (int)(rc.right - 200);
        HBRUSH b1 = CreateSolidBrush(RGB(52, 120, 210));
        RECT r1{x, 96, x + 160, 216};
        FillRect(dc, &r1, b1);
        DeleteObject(b1);

        // 垂直移動的方塊（測雙向運動）
        int y = (g_frame * 7) % (int)(rc.bottom - 260);
        HBRUSH b2 = CreateSolidBrush(RGB(214, 84, 60));
        RECT r2{rc.right - 200, 96 + y, rc.right - 80, 176 + y};
        FillRect(dc, &r2, b2);
        DeleteObject(b2);

        // 細線（測高頻邊緣）
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 130));
        HGDIOBJ old = SelectObject(dc, pen);
        for (int i = 0; i < 12; ++i) {
            int ly = 260 + i * 12;
            if (ly < (int)rc.bottom - 40) {
                MoveToEx(dc, 60, ly, nullptr);
                LineTo(dc, rc.right - 60, ly);
            }
        }
        SelectObject(dc, old);
        DeleteObject(pen);

        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 全部在 WM_PAINT 裡畫，避免閃爍
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(h, m, w, l);
}

int main(int argc, char** argv) {
    int w = 800, h = 600, seconds = 30, fps = 60;
    int syncIntervalMs = 400;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &w, &h);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--hard")) {
            g_hard = 1;
        } else if (!strcmp(argv[i], "--borderless")) {
            g_borderless = 1;
        } else if (!strcmp(argv[i], "--sync")) {
            g_sync = 1;
        } else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
            syncIntervalMs = atoi(argv[++i]);
            if (syncIntervalMs < 50) syncIntervalMs = 50;
        } else if (!strcmp(argv[i], "--flip-lead") && i + 1 < argc) {
            // 反轉要比嗶聲起點晚多少 ms 才「同時」被看到/聽到。
            //
            // 這個量是「waveOut 的位置回報落後 DAC」＋「DWM 合成延遲」的差，
            // 實測這台機器是 ~32ms（見 docs/09 §8）。它是**素材源的**殘差，
            // 不是錄影端的 —— 錄影端只負責忠實記錄，不該補這個。
            g_flipLeadMs = atof(argv[++i]);
        }
    }

    if (g_sync) {
        // 把參數印出來：量測端要用同一個 freq 與 period，寫錯就整個量不到，
        // 而「量不到」在數字上長得跟「完全對齊」很像。
        if (!SyncAudioStart(syncIntervalMs)) {
            printf("warn: waveOut 開不起來，同步測試會沒有聲音\n");
        } else {
            printf("sync source: flip=%dms  beep=3000Hz/150ms/0.85  "
                   "mode=sample-locked (%d Hz, period=%d samples, lead=%.0fms)\n",
                   syncIntervalMs, kSyncRate, g_patLen, g_flipLeadMs);
        }
        g_qlog = fopen("anim_sync.log", "w");
        if (!g_qlog) printf("warn: anim_sync.log 開不起來，無法回推絕對時刻\n");
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "CapWGCAnim";
    // 這個檔案刻意走 ANSI（WNDCLASSA / CreateWindowExA），但 IDC_ARROW 是資源 id，
    // 在 /DUNICODE 下會展開成 MAKEINTRESOURCEW —— 配 LoadCursorA 就型別不符。
    // 游標跟字串無關，直接用 W 版最省事。
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassA(&wc);

    // client 區固定成 w x h（外面那圈是非 client area）。
    // --borderless 用 WS_POPUP：錄 1920x1080 時帶標題列會被螢幕裁掉，
    // 客戶區只剩 1606x913，「1080p60 不掉幀」就變成量一個不是 1080p 的東西。
    const DWORD style = g_borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT want{0, 0, w, h};
    if (!g_borderless) AdjustWindowRect(&want, style, FALSE);

    // 一定要 topmost。WGC 對「被遮住的視窗」會少給幀（DWM 不合成就不更新），
    // 而這個視窗存在的唯一目的就是當一個**確定性**的內容源 —— 一旦它被前景
    // 的終端機蓋住，兩次跑出來的幀數就不一樣，編碼器的 A/B 比較全部失效。
    // 實測：被遮住時 5 秒只收到 50 幀（10fps），置頂後是 300 幀（60fps）。
    //
    // 不要加 WS_EX_TOOLWINDOW。Windows 11 24H2 的 GraphicsCaptureItem::
    // CreateForWindow 對 tool window 直接回 E_INVALIDARG（0x80070057），
    // 而錯誤訊息長得跟「hwnd 給錯」一模一樣 —— 上一台機器沒這個問題，
    // 所以很容易誤判成 hwnd 抓錯或 WGC 壞掉。
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "CapWGCAnim",
                                "CapWGC anim",
                                style | WS_VISIBLE,
                                80, 80, want.right - want.left, want.bottom - want.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        printf("CreateWindow failed %lu\n", GetLastError());
        return 1;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    printf("hwnd=0x%08llX size=%dx%d\n", (unsigned long long)(uintptr_t)hwnd, w, h);
    fflush(stdout);

    const DWORD t0 = GetTickCount();
    // 等時脈搏，不是「畫完睡 16ms」。
    //
    // Sleep(16) 的實際延遲是 ~15.6ms 的倍數，加上渲染時間，一輪變成 ~25ms，
    // 實測只有 39 fps —— 來源自己跑不到 60，就沒辦法拿來驗「60fps 不掉幀」。
    // 改成「算下一個截止時間、只睡到那裡」，並用 1ms 計時器解析度把 overshoot
    // 壓到可忽略。
    timeBeginPeriod(1);
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const double period = 1.0 / (double)(fps > 0 ? fps : 60);
    long long n = 0;

    while (GetTickCount() - t0 < (DWORD)seconds * 1000) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_sync) SyncAudioPump();

        // 音訊裝置開不起來時的退路：照牆鐘反轉。這樣至少還有畫面事件，
        // 但音畫差會退回「不可信」的狀態 —— 所以上面會印 warn。
        long long qFallback = 0;
        bool flippedByClock = false;
        if (g_sync && !g_audioOk) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double el = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
            const int fi = (int)(el * 1000.0 / (double)syncIntervalMs);
            if (fi != g_flips) {
                g_flips = fi;
                g_invert = fi & 1;
                flippedByClock = true;
                qFallback = Qpc100();
            }
        }

        g_frame++;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (flippedByClock && g_qlog) {
            fprintf(g_qlog, "flip %d detect=%lld painted=%lld beep=0\n", g_flips,
                    qFallback, Qpc100());
            fflush(g_qlog);
        }

        // 目標時刻 = start + (n+1) * period，用絕對時刻累加避免誤差滾雪球。
        ++n;
        const double target = (double)n * period;
        for (;;) {
            // ---- 反轉由「播放位置」驅動 ----
            //
            // 等到 waveOut 真的播到嗶聲那一個樣點，才把畫面翻過去。因為畫面的
            // 反轉時刻是由這個位置決定的，裝置的輸出延遲同時作用在聲音與畫面
            // 上而自動抵消 —— 不需要知道延遲是多少。
            //
            // 輪詢粒度 1ms（timeBeginPeriod(1) 已開）：睡太粗會讓反轉晚一整個
            // frame，而 16.7ms 正好是要量的尺度。
            if (g_sync && g_audioOk) {
                const int64_t pos = SyncAudioPos();
                if (pos >= 0) {
                    if (g_flipAt == 0) {
                        // 第一個週期可能已經播掉一半，從下一個邊界開始算
                        g_flipAt = (pos / g_patLen + 1) * g_patLen;
                    } else if (pos >= g_flipAt + g_flipLead) {
                        g_invert ^= 1;
                        ++g_flips;
                        const long long q = Qpc100();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        UpdateWindow(hwnd);
                        if (g_qlog) {
                            fprintf(g_qlog,
                                    "flip %d detect=%lld painted=%lld beep=%lld\n",
                                    g_flips, q, Qpc100(), g_flipAt);
                            fflush(g_qlog);
                        }
                        g_flipAt += g_patLen;
                        ++g_frame;
                    }
                }
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
            const double left = target - elapsed;
            if (left <= 0.0005) break;
            // 剩不到 2ms 就自旋，其餘以 1ms 為單位睡 —— Sleep 的粒度太粗會
            // overshoot，而同步模式還要靠這個迴圈輪詢播放位置。
            if (left > 0.002) Sleep(1);
            else Sleep(0);
        }
    }
    timeEndPeriod(1);
    if (g_sync) SyncAudioStop();

    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double sec = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        printf("painted=%lld in %.2fs -> %.1f fps\n", n, sec, n / sec);
        fflush(stdout);
    }

    DestroyWindow(hwnd);
    return 0;
}
