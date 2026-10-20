// ============================================================================
// main.cpp — CapWGC GUI（托盤 + 全域熱鍵 + 區域框選 + 錄製指示）
// ============================================================================
//
// 這是「最短閉環」：能選區域、能錄、能停、能拿到檔案。沒有時間軸、沒有編輯器，
// 那些是下一層的事。
//
// 狀態機只有四個狀態，任何時候只有一條合法路徑可以前進：
//
//        ┌──────────── 熱鍵 A ───────────┐
//        ▼                               │
//     Selecting ──(選好/取消)──> Idle ──(熱鍵 R)──> Recording
//                                                    │
//                                                (熱鍵 S / 逾時 / 視窗關閉)
//                                                    ▼
//                                                 Stopping
//                                                    │
//                                              (worker 收尾完成)
//                                                    ▼
//                                                   Idle
//
// 為什麼 Stop 一定要在 worker thread：它會 join WGC 的擷取執行緒、沖完 AAC、
// 寫 moov，實測 100~300ms。在 UI 執行緒做，托盤會「卡住不回應」——使用者會
// 以為程式當了，然後去工作管理員殺掉它，正好毀掉剛錄好的檔案。
// ============================================================================

#include "controlbar.h"
#include "i18n.h"
#include "ocr.h"
#include "overlay.h"
#include "picker.h"
#include "settings.h"
#include "snip.h"
#include "ui.h"

#include "../core/codec_select.h"
#include "../core/capture_wgc.h"   // --gatestest 要驗 FrameGate 的數學
#include "../core/recorder.h"

#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_REC_DONE = WM_APP + 2;
constexpr UINT WM_APP_OCR = WM_APP + 3;       // OCR 引擎事件（見 OcrEngine::HandleMessage）
constexpr UINT WM_APP_SNIP_OCR_TEST = WM_APP + 4;   // QA：啟動後自動進入「截圖＋OCR」
// 「請開始截圖」（wParam 非 0 = 直接選 OCR）。托盤選單用它，而不是直接呼叫
// BeginSnip：選擇器開著的時候，這個訊息會被 picker 的訊息迴圈攔下來，等它
// 關掉之後才真正執行 —— 見 BeginWindowPicker。
constexpr UINT WM_APP_SNIP = WM_APP + 5;
constexpr UINT TRAY_ID = 1;
constexpr UINT_PTR TIMER_TICK = 1;

constexpr int HOTKEY_REGION = 1;
constexpr int HOTKEY_SCREEN = 2;
constexpr int HOTKEY_STOP = 3;
constexpr int HOTKEY_PICKER = 4;
constexpr int HOTKEY_SNIP = 5;

// 選單項目
constexpr int IDM_REGION = 40001;
constexpr int IDM_SCREEN = 40002;
constexpr int IDM_STOP = 40003;
constexpr int IDM_SETTINGS = 40004;
constexpr int IDM_FOLDER = 40005;
constexpr int IDM_QUIT = 40006;
constexpr int IDM_PICKER = 40007;
constexpr int IDM_RESETBAR = 40008;
constexpr int IDM_SNIP = 40009;
constexpr int IDM_SNIP_OCR = 40010;

enum class Mode { Idle, Selecting, Picking, Recording, Stopping, SnipSelect };

const wchar_t* kClassName = L"CapWGCWindow";

struct App {
    HWND msgwnd = nullptr;
    app::Settings settings;
    // 錄製中唯一的 UI：兩顆圖示（停止 / 麥克風）。沒有外框、沒有計時徽章 ——
    // 外框會讓被錄的畫面看起來被裁掉，而且全螢幕時根本畫不到外面。
    // 計時改成托盤提示文字（見 TrayText）。
    app::ControlBar bar;
    app::Overlay overlay;

    // 提示訊息用自己的視窗，不用 Shell 的托盤氣球。
    //
    // 原因見 ui.h 的 Toast 說明：Win10/11 的托盤氣球其實是 Action Center 的
    // toast，走通知佇列 —— 新的一則要排隊等舊的講完（單則預設 5 秒），所以
    // 連續截圖／OCR 時提示會「慢好幾步」，看起來像程式反應變慢。
    // 自己畫的視窗沒有佇列，Show() 就是立刻換掉畫面那一則。
    ui::Toast toast;

    // ---- 截圖 / OCR ----
    //
    // 整個流程：熱鍵 → 先抓一張「沒有覆蓋層」的桌面 → 開框選 → 選完之後從
    // 那張原圖裁出選取範圍 → 進剪貼簿或送 OCR。
    //
    // 為什麼要先抓、而不是選完再抓：框選用的遮罩會把畫面變暗。選完才抓就會
    // 抓到變暗的版本（實測症狀是「截出來的圖比看到的暗一階」）。所以順序必須
    // 是「抓原圖 → 才蓋遮罩」。代價是選取期間畫面會變（例如影片在播），
    // 截到的是按鍵當下的那一格 —— 那正是「截圖」的定義。
    app::ScreenBitmap snipFull;
    int snipOcrId = 0;             // 正在等結果的請求編號
    app::OcrEngine ocr;
    bool ocrReady = false;
    bool ocrStarting = false;
    bool ocrAvailable = false;     // 引擎可用（決定工具列 OCR 是否可點）
    // OCR 結果不開面板：文字直接進剪貼簿（見 DeliverOcrText）。
    // 這裡只需要記住「送出去辨識的那張暫存圖」，用完就刪。

    std::unique_ptr<capwgc::Recorder> rec;
    std::thread stopper;

    Mode mode = Mode::Idle;
    HICON iconIdle = nullptr;
    HICON iconRec = nullptr;
    bool trayAdded = false;

    ULONGLONG startTick = 0;
    bool warnedNoFrame = false;
    bool micMuted = false;
    int traySec = -1;          // 托盤提示上一次寫的秒數（只在整秒變化時更新）
    std::wstring lastPath;
    RECT lastRegion{};
    bool hasLastRegion = false;
    RECT captureRect{};        // 這次錄影的擷取範圍（重設控件位置時要重排）
    std::string lastError;
};

App* g_app = nullptr;

// ---- 狀態日誌 --------------------------------------------------------------
//
// GUI 的失敗幾乎都是無聲的：覆蓋層沒開起來、熱鍵沒收到、編碼器初始化失敗。
// 使用者只看到「按了沒反應」，而 Windows 不給任何線索。所以每個狀態轉換都寫
// 一行到 %LOCALAPPDATA%\CapWGC\gui.log（超過 256KB 就覆蓋重寫，不無限長大）。
void Log(const char* fmt, ...) {
    static wchar_t path[MAX_PATH] = {};
    static bool init = false;
    if (!init) {
        init = true;
        wchar_t dir[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            std::wstring d = std::wstring(dir) + L"\\CapWGC";
            CreateDirectoryW(d.c_str(), nullptr);
            std::wstring p = d + L"\\gui.log";
            wcscpy_s(path, p.c_str());
        }
    }
    if (!path[0]) return;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > 256 * 1024) {
        DeleteFileW(path);
    }

    FILE* f = _wfopen(path, L"a");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Log 是窄字元介面（vfprintf），所以寬字串要先自己轉 UTF-8 再走 %s。
//
// 直接寫 "%ls" 會在 "C" locale 下卡住：MSVCRT 拿當前 locale 做寬→窄轉換，
// 遇到非 ASCII 就轉不動，訊息從第一個中文字開始整段消失（下面 PW 那裡對
// stdout 有同一件事的說明）。症狀是 log 只寫到「ocr ready=0 detail=OCR 」，
// 而最需要看的原因不見了。gui.log 本來就是 UTF-8 檔，轉一次不會有副作用。
std::string U8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// 帶參數的介面字串。**整句**進語系檔（`"%s 選區域錄影"`），不要在程式裡把
// 句子切成片段再拼起來：語序與格位每種語言都不一樣，拼出來的每種語言都怪。
// 實作在 i18n（長度由字串決定，不用固定緩衝區 —— 見 i18n.h 的說明）。
std::wstring WFmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = app::i18n::FmtV(fmt, ap);
    va_end(ap);
    return s;
}

// ---- A/V 偏移 -------------------------------------------------------------
//
// 這裡**刻意不讀** out/calibrate.sh 產生的校準檔。
//
// 2026-10 之前，音軌的「起跑差」（gap = firstPacketQpc − videoOrigin）沒有補，
// 全靠校準檔去抵，而那個檔是在「音訊本來就該補 gap」的錯誤前提下量出來的。
// 現在 Recorder 自己會補 gap（docs/09 §4.7），再把舊校準值疊上去等於補兩次。
//
// GUI 也沒有任何校正 UI，偷偷套用一個使用者不記得的檔案正是這個專案一直在避免
// 的「無聲行為」。要微調就走 RecorderConfig::avOffset100ns 明確指定。

// ---- 熱鍵 ------------------------------------------------------------------
//
// 熱鍵是使用者設定的（settings.ini 的 [hotkeys]），所以「註冊」這件事必須
// 可失敗、可回報：
//
//   1. 先全部反註冊（舊組合不先放掉，新舊一樣時會自己撞自己）
//   2. 逐個註冊，**失敗的那一個跳過、其餘照樣生效** —— 一個組合被別的程式
//      佔用不該讓另外三個一起廢掉
//   3. 設定視窗按下確定時若有人失敗，整套退回舊的（ApplyHotkeys）：
//      使用者剛改完就看到「有一半生效」比「完全沒改」更難理解
//
// 失敗一定要講出來：RegisterHotKey 失敗時程式看起來完全正常，只是「按了沒
// 反應」，而使用者不會想到是別的程式佔用了組合鍵。
struct HkSlot {
    int id;
    const wchar_t* name;
    app::Hotkey hk;
};

std::vector<HkSlot> Slots(const app::Hotkeys& k) {
    return {{HOTKEY_REGION, app::i18n::T(L"選區域"), k.region},
            {HOTKEY_PICKER, app::i18n::T(L"視窗清單"), k.picker},
            {HOTKEY_SCREEN, app::i18n::T(L"錄主螢幕"), k.screen},
            {HOTKEY_STOP, app::i18n::T(L"停止"), k.stop},
            {HOTKEY_SNIP, app::i18n::T(L"截圖 / OCR"), k.snip}};
}

const int kHotkeyIds[5] = {HOTKEY_REGION, HOTKEY_PICKER, HOTKEY_SCREEN, HOTKEY_STOP,
                           HOTKEY_SNIP};

void UnregisterAll(HWND w) {
    for (int id : kHotkeyIds) UnregisterHotKey(w, id);
}

// 回傳空字串 = 全部成功；否則回傳「第一個被佔用的動作名稱」。
// 失敗的那一個跳過、其餘照樣註冊 —— 一個熱鍵被佔用不該讓另外三個一起廢掉。
std::wstring RegisterAll(HWND w, const app::Hotkeys& k) {
    UnregisterAll(w);
    std::wstring first;
    for (const auto& s : Slots(k)) {
        if (!s.hk.set()) {
            Log("hotkey id=%d unset", s.id);
            continue;
        }
        if (!RegisterHotKey(w, s.id, s.hk.mods, s.hk.vk)) {
            const DWORD err = GetLastError();
            Log("RegisterHotKey FAILED id=%d mods=0x%X vk=0x%X err=%lu", s.id, s.hk.mods,
                s.hk.vk, err);
            if (first.empty()) first = s.name;
            continue;
        }
        Log("hotkey id=%d %ls ok", s.id, app::HotkeyText(s.hk).c_str());
    }
    return first;
}

bool ApplyHotkeys(App& a, const app::Hotkeys& next, std::wstring* failed);
void OpenSettings(App& a);
bool EngagePrtScn(App& a, std::wstring* why);
bool ReleasePrtScn(App& a, std::wstring* why);
void ReconcilePrtScn(App& a);

// ---- 托盤 -----------------------------------------------------------------

// 「按什麼開始錄影」的一句話提示。現在 region 預設是空的，所以不能像以前
// 那樣直接印 HotkeyText(hotkeys.region) —— 那會變成「（未設定） 選區域錄影」。
// 沒有熱鍵時改講真正的入口（選擇視窗），它一定在。
std::wstring StartHint(const App& a) {
    const app::Hotkeys& hk = a.settings.hotkeys;
    if (hk.region.set())
        return WFmt(app::i18n::T(L"%s 選區域錄影"), app::HotkeyText(hk.region).c_str());
    if (hk.picker.set())
        return WFmt(app::i18n::T(L"%s 選擇視窗"), app::HotkeyText(hk.picker).c_str());
    return app::i18n::T(L"點兩下開啟");
}

std::wstring TrayText(const App& a, bool recording, int seconds = -1) {
    if (recording) {
        std::wstring t = app::i18n::T(L"CapWGC — 錄影中");
        if (seconds >= 0) {
            wchar_t buf[32];
            swprintf_s(buf, L" %02d:%02d", seconds / 60, seconds % 60);
            t += buf;
        }
        if (a.micMuted) t += app::i18n::T(L"（麥克風靜音）");
        // 沒綁熱鍵時不要印「（未設定） 停止」—— 控制項上的停止鈕就是入口。
        if (a.settings.hotkeys.stop.set())
            t += L"　" + WFmt(app::i18n::T(L"%s 停止"),
                              app::HotkeyText(a.settings.hotkeys.stop).c_str());
        return t;
    }
    return L"CapWGC — " + StartHint(a);
}

void TrayAdd(App& a) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = a.msgwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = a.iconIdle;
    wcsncpy_s(nid.szTip, TrayText(a, false).c_str(), _TRUNCATE);
    a.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void TrayUpdate(App& a, bool recording, int seconds = -1) {
    if (!a.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = a.msgwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = recording ? a.iconRec : a.iconIdle;
    wcsncpy_s(nid.szTip, TrayText(a, recording, seconds).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayRemove(App& a) {
    if (!a.trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = a.msgwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    a.trayAdded = false;
}

// 提示訊息：走自己的視窗（ui::Toast），不走 Shell 的托盤氣球。
//
// 為什麼換掉 —— 這是實測出來的，不是偏好：
//   在 Windows 10/11 上，托盤氣球已經不是當年的 balloon tip，而是被轉成一則
//   Action Center 的 toast，走**通知佇列**。於是兩個關鍵行為都變了：
//     1. 新的一則不會取代畫面上那一則，而是排隊等它講完；
//     2. 單則最短顯示時間是系統設定（預設 5 秒），`uTimeout` 自 Vista 起就
//        被忽略，`NIF_REALTIME` 也不再是文件寫的「立刻取代」。
//   使用者看到的就是「前一個提示還掛著，後面幾次的提示慢好幾秒才跳出來」，
//   連續截圖／OCR 時尤其明顯 —— 那不是程式慢，是提示在排隊。
//
//   自己畫的視窗完全沒有佇列：Show() 立刻換掉畫面上那一則。順帶還拿到三個
//   好處 —— 可以點一下就關、可以精準控制停留時間、錄影中還能自動讓開漂浮控件。
//
// flags 仍然保留（呼叫端一律傳 NIIF_*），在這裡映射成 Toast::Kind。
//
// 交接點：以前「已儲存（點一下開啟）」的開啟動作是靠 Shell 氣球回的
// `NIN_BALLOONUSERCLICK`。換成自家視窗之後那則通知永遠不會來 —— 標題還在
// 承諾「點一下開啟」，點下去卻只把提示關掉。所以動作要從呼叫端傳進來
// （onClick），見 FinishRecording / DeliverSnip。
void OpenLastFile(App& a);

void TrayBalloon(App& a, const wchar_t* title, const std::wstring& text,
                 DWORD flags, ui::Toast::Action onClick = {}) {
    ui::Toast::Kind kind = ui::Toast::Kind::Info;
    if (flags & NIIF_ERROR) kind = ui::Toast::Kind::Error;
    else if (flags & NIIF_WARNING) kind = ui::Toast::Kind::Warn;

    // 停留時間：一則訊息該看多久取決於有幾個字。固定 5 秒對「已截圖」太長
    // （會蓋住畫面又沒必要），對多行的錯誤又太短。用「基礎 + 每字」估。
    const size_t chars = text.size() + (title ? wcslen(title) : 0);
    int ms = 2200 + (int)chars * 28;
    if (kind != ui::Toast::Kind::Info) ms += 1200;   // 錯誤要多留一點時間讀
    if (ms > 7000) ms = 7000;

    a.toast.Show(title ? title : L"", text, kind, ms, std::move(onClick));

    // 同時留一行 log：提示會自己消失，事後要追「到底有沒有提示過」只能靠這裡。
    Log("toast %s: %s", U8(title ? title : L"").c_str(), U8(text).c_str());
}

// 套用一組熱鍵：成功就吃下，失敗就整套退回舊的（見上面的說明）。
bool ApplyHotkeys(App& a, const app::Hotkeys& next, std::wstring* failed) {
    const app::Hotkeys prev = a.settings.hotkeys;
    const std::wstring bad = RegisterAll(a.msgwnd, next);
    if (bad.empty()) {
        a.settings.hotkeys = next;
        TrayUpdate(a, a.mode == Mode::Recording);
        return true;
    }
    RegisterAll(a.msgwnd, prev);   // 回滾：寧可維持「確定可用」的舊狀態
    if (failed) *failed = bad;
    return false;
}

// ---- PrtScn 接管 -----------------------------------------------------------
//
// 這個功能的難點不在改登錄值（三行就寫完），而在**它的失敗是無聲的**：
// RegisterHotKey(VK_SNAPSHOT) 會回成功，然後什麼都不會發生。所以使用者看到
// 的是「我勾了接管、按 PrtScn 沒反應」—— 如果我們不主動驗證，他永遠不知道
// 是哪裡壞了，只會得到「這軟體的截圖壞了」的結論。
//
// 因此接管流程刻意做成：先確認熱鍵真的註冊成功 → 再改系統設定 → 明確回報。
// 順序反過來的話（先改系統設定再註冊），註冊失敗時使用者的 PrtScn 就被我們
// 拔掉了卻沒有任何東西接手 —— 那是最糟的結局。
bool EngagePrtScn(App& a, std::wstring* why) {
    const app::PrtScnState st = app::PrtScnQuery();
    if (st == app::PrtScnState::Error) {
        if (why) *why = app::i18n::T(L"讀不到系統設定（登錄檔的 Control Panel\\Keyboard）");
        return false;
    }

    // 1. 先把「截圖 / OCR」綁到 PrtScn 並註冊。這一步成功只代表「鑰匙孔是
    //    我們的」，還不代表按得下去（見上面的說明）。
    const app::Hotkeys prev = a.settings.hotkeys;   // 步驟 2 失敗時要還原成這個
    app::Hotkeys next = a.settings.hotkeys;
    next.snip = app::Hotkey{0, VK_SNAPSHOT};
    std::wstring bad;
    if (!ApplyHotkeys(a, next, &bad)) {
        if (why) *why = app::i18n::T(L"PrtScn 已被其他程式註冊");
        return false;
    }

    // 2. 讓 Windows 不要再把 PrtScn 送給剪取工具。
    if (st != app::PrtScnState::Disabled && !app::PrtScnTake()) {
        // 改不到系統設定 → 把熱鍵退回原本的，不要留下「按了沒反應」的狀態。
        //
        // 這裡一定要用上面存下來的 prev，不能用 a.settings.hotkeys ——
        // ApplyHotkeys 成功時已經把 members 換成 next 了，拿它去「還原」等於
        // 還原成同一個值，熱鍵就停在一個按了沒反應的 PrtScn 上（實測過）。
        ApplyHotkeys(a, prev, nullptr);
        if (why) *why = app::i18n::T(L"沒有權限修改系統設定（可能被群組原則鎖住）");
        return false;
    }

    a.settings.Save();
    // 記下「問過了」：之後啟動就不會再自動遷移（使用者若又改回別的組合鍵，
    // 那是他的選擇，不該每次開機被翻回 PrtScn）。
    app::PrtScnSetChosen(true);
    Log("prtsc: engaged (reg was %d)", (int)st);
    return true;
}

bool ReleasePrtScn(App& a, std::wstring* why) {
    // 還原系統設定**先做**。就算熱鍵那一步出問題，使用者的 PrtScn 也該先
    // 回到 Windows 手上 —— 那是他原本的預期。
    bool ok = true;
    if (app::PrtScnOwned()) {
        ok = app::PrtScnGiveBack();
        if (!ok && why) *why = app::i18n::T(L"還原系統設定失敗，PrScn 可能仍由 CapWGC 接管");
    }

    // 熱鍵退回 Ctrl+Alt+X（不是清空 —— 使用者還是要能截圖）。
    // 注意這裡不能用 Hotkeys::Default().snip：預設值現在就是 PrtScn。
    app::Hotkeys next = a.settings.hotkeys;
    next.snip = app::SnipFallbackHotkey();
    ApplyHotkeys(a, next, nullptr);
    a.settings.Save();
    // 使用者明確關掉了接管 —— 這也是「問過了」，之後啟動不要再自動接管。
    app::PrtScnSetChosen(true);
    Log("prtsc: released ok=%d", ok ? 1 : 0);
    return ok;
}

// 啟動時對帳：ini 說「我們接管著」，但登錄值已經不是 0（使用者自己關掉了、
// 或另一個程式改回來了）。這種不同步要嘛修好、要嘛放手，不能兩邊都半套。
void ReconcilePrtScn(App& a) {
    const bool owned = app::PrtScnOwned();
    const app::PrtScnState st = app::PrtScnQuery();
    if (owned && st == app::PrtScnState::Enabled) {
        Log("prtsc: ini says owned but registry is enabled -> letting go");
        app::PrtScnSetOwned(false);
        if (a.settings.hotkeys.snip.vk == VK_SNAPSHOT) {
            app::Hotkeys h = a.settings.hotkeys;
            h.snip = app::SnipFallbackHotkey();
            ApplyHotkeys(a, h, nullptr);
            a.settings.Save();
        }
    } else if (owned && st == app::PrtScnState::Absent) {
        // 我們上次改過，但值被刪掉了（系統還原、群組原則重設）。視為已放手，
        // 否則下次「關閉接管」會去刪一個本來就不存在的值 —— 那反而正確，
        // 但 owned 的語意就錯了。
        Log("prtsc: value vanished, clearing owned flag");
        app::PrtScnSetOwned(false);
    }
}

// 預設接管 PrtScn。
//
// 「用 PrtScn 取代 Windows 剪取工具」現在是**預設開啟**的，但這件事不能只靠
// 一個預設值：Windows 11 預設會把 PrtScn 交給剪取工具，而 RegisterHotKey 在
// 那個狀態下**回成功、卻永遠收不到 WM_HOTKEY**（見 settings.h 的實測記錄）。
// 也就是說「只把熱鍵預設成 PrtScn」會得到一個看起來正常、按下去完全沒反應的
// 程式 —— 那正是這個功能最惡毒的失敗模式。所以啟動時要主動把系統設定接過來。
//
// 三種情況要分清楚：
//
//   1. 全新安裝（ini 不存在）→ snip 預設就是 PrtScn → 直接接管。
//   2. 從舊版升級（ini 裡是舊預設 Ctrl+Alt+X，或截圖鍵是空的）→ 那是「從來
//      沒改過」的樣子，遷移到 PrtScn 並接管。使用者不會因此失去什麼：舊的
//      組合鍵不是他選的。
//   3. 使用者自己設過別的組合（例如 Ctrl+Shift+S）→ **不動**。尊重明確的選擇
//      比貫徹預設值重要；他要換成 PrtScn 就自己在設定裡勾。
//
// 判斷「有沒有自己設過」不能只看 ini 有沒有值（舊版存檔一定會寫一行），所以
// 另外記一個 [prtsc] chosen 記號：有這個記號＝問過了，不再自動遷移。
void AutoEngagePrtScn(App& a) {
    if (app::PrtScnOwned()) {
        app::PrtScnSetChosen(true);
        return;
    }

    const app::Hotkey snip = a.settings.hotkeys.snip;
    const bool isPrtsc = (snip.vk == VK_SNAPSHOT && snip.mods == 0);

    if (!isPrtsc) {
        // 不是 PrtScn。分兩種：已經問過 → 放手；沒問過 → 看看是不是「沒改過」。
        if (app::PrtScnChosen()) {
            Log("prtsc: auto-engage skipped (user chose a different key)");
            return;
        }
        const app::Hotkey oldDefault = app::SnipFallbackHotkey();
        if (snip.set() && !snip.same(oldDefault)) {
            // 使用者自己設了別的組合 → 尊重它，並記下「問過了」。
            app::PrtScnSetChosen(true);
            Log("prtsc: auto-engage skipped (custom snip hotkey %ls)",
                app::HotkeyText(snip).c_str());
            return;
        }
        // 舊預設或空的 → 遷移到新預設。
        app::Hotkeys h = a.settings.hotkeys;
        h.snip = app::Hotkey{0, VK_SNAPSHOT};
        ApplyHotkeys(a, h, nullptr);
        a.settings.Save();
        Log("prtsc: migrated snip hotkey to PrtScn (was %ls)",
            snip.set() ? app::HotkeyText(snip).c_str() : L"unset");
    }

    const app::PrtScnState st = app::PrtScnQuery();
    if (st == app::PrtScnState::Error) {
        Log("prtsc: auto-engage skipped (registry unreadable)");
        return;
    }
    if (st == app::PrtScnState::Disabled) {
        // 系統設定已經是「不給剪取工具」——可能上次是我們改的但 ini 被清掉了。
        // 補記 owned，這樣使用者關閉接管時才還原得回去。
        app::PrtScnSetOwned(true);
        app::PrtScnSetChosen(true);
        Log("prtsc: auto-engage adopted existing disabled state");
        return;
    }

    std::wstring why;
    if (EngagePrtScn(a, &why)) {
        app::PrtScnSetChosen(true);
        Log("prtsc: auto-engaged (default on)");
        return;
    }
    Log("prtsc: auto-engage FAILED: %s", U8(why).c_str());
    // 接管失敗（被別的程式佔用、群組原則鎖住）→ 退回一個真的按得下去的鍵。
    // 不彈提示：這是預設行為，不是使用者按了什麼，一則看不懂的警告只會讓人
    // 以為程式壞了；設定視窗裡看得到目前的狀態。
    app::Hotkeys h = a.settings.hotkeys;
    h.snip = app::SnipFallbackHotkey();
    ApplyHotkeys(a, h, nullptr);
    a.settings.Save();
    app::PrtScnSetChosen(true);
}

// ---- 錄製 -----------------------------------------------------------------

RECT MonitorRect(HMONITOR mon) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
    RECT r{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// 把設定 + 目標組成 RecorderConfig。輸出目錄在這裡確保存在 —— 目錄不存在時
// 編碼器開檔會失敗，而那個錯誤碼（MF_E_UNSUPPORTED_BYTESTREAM_TYPE）看起來
// 像「格式不支援」，完全指不到真正的原因。
bool BuildConfig(App& a, bool useRegion, const RECT& region, HWND target,
                 capwgc::RecorderConfig* cfg, std::string* err) {
    cfg->outPath.clear();
    cfg->useRegion = useRegion;
    cfg->regionScreen = region;
    cfg->hwnd = useRegion ? nullptr : target;
    cfg->monitor = nullptr;
    if (!useRegion && !target) {
        cfg->monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    switch (a.settings.codec) {
    case 1: cfg->autoCodec = false; cfg->codec = capwgc::Codec::AV1; break;
    case 2: cfg->autoCodec = false; cfg->codec = capwgc::Codec::HEVC; break;
    case 3: cfg->autoCodec = false; cfg->codec = capwgc::Codec::H264; break;
    default: cfg->autoCodec = true; break;
    }
    cfg->quality = a.settings.quality;
    cfg->fps = a.settings.fps;
    cfg->gopSeconds = a.settings.gop;
    cfg->captureCursor = a.settings.captureCursor;
    cfg->fragmented = true;   // fMP4：被 kill 也還能播，錄影軟體該有的預設

    cfg->audio.clear();
    if (a.settings.audioSystem) {
        cfg->audio.push_back({capwgc::AudioSourceKind::Loopback, std::string()});
    }
    if (a.settings.audioMic) {
        cfg->audio.push_back({capwgc::AudioSourceKind::Input, std::string()});
    }
    cfg->separateTracks = a.settings.separateTracks;

    cfg->avOffset100ns = 0;   // gap 由 Recorder 自己補，這裡只留額外校正的接口

    std::wstring dir = a.settings.outputDir;
    if (dir.empty()) dir = app::Settings::DefaultOutputDir();
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = "無法建立輸出資料夾";
        return false;
    }
    cfg->outPath = capwgc::MakeDefaultOutputPath(dir);
    return true;
}

void BeginRecording(App& a, const capwgc::RecorderConfig& cfg, const RECT& captureRect) {
    a.rec = std::make_unique<capwgc::Recorder>();
    std::string err;
    if (!a.rec->Start(cfg, &err)) {
        Log("rec.Start FAILED: %s", err.c_str());
        a.rec.reset();
        a.mode = Mode::Idle;
        a.lastError = err;
        TrayBalloon(a, app::i18n::T(L"無法開始錄影"), std::wstring(err.begin(), err.end()), NIIF_ERROR);
        return;
    }
    Log("rec.Start ok path=%s hud=%ld,%ld,%ld,%ld", U8(cfg.outPath).c_str(),
        captureRect.left, captureRect.top, captureRect.right, captureRect.bottom);
    a.mode = Mode::Recording;
    a.startTick = GetTickCount64();
    a.warnedNoFrame = false;
    a.lastError.clear();
    a.traySec = -1;
    a.micMuted = false;
    a.bar.SetMicMuted(false);
    // 有麥克風軌才讓那顆鈕可按。音源開不起來（端點被獨佔）時這裡就是 false，
    // 按下去會提示原因 —— 總比畫成一顆按了沒反應的鈕好。
    //
    // 位置：使用者拖過就沿用（相對擷取範圍右下角），沒拖過用預設右下角外側。
    // 兩條路都會夾回工作區，所以不會出現「控件跑到畫面外、拉不回來」。
    a.captureRect = captureRect;
    POINT off{a.settings.barDx, a.settings.barDy};
    a.bar.Show(captureRect, a.rec->hasMic(), a.settings.hasBarPos ? &off : nullptr);
    // 錄影中漂浮控件就停在右下角，而提示也在右下角 —— 不讓開的話提示會直接
    // 蓋住「停止」那顆鈕，錄影當下最需要按的東西反而被擋住。
    // 控件高 36 DIP、離邊界 8 DIP，所以讓開 36 + 8 + 8 = 52。
    a.toast.SetBottomInset(52);
    TrayUpdate(a, true, 0);
}

void StartRegionRecording(App& a, const RECT& region) {
    capwgc::RecorderConfig cfg;
    std::string err;
    if (!BuildConfig(a, true, region, nullptr, &cfg, &err)) {
        TrayBalloon(a, app::i18n::T(L"無法開始錄影"), std::wstring(err.begin(), err.end()), NIIF_ERROR);
        return;
    }
    a.lastRegion = region;
    a.hasLastRegion = true;
    BeginRecording(a, cfg, region);
}

void StartMonitorRecording(App& a, HMONITOR mon, const RECT& rect) {
    capwgc::RecorderConfig cfg;
    std::string err;
    if (!BuildConfig(a, false, RECT{}, nullptr, &cfg, &err)) {
        TrayBalloon(a, app::i18n::T(L"無法開始錄影"), std::wstring(err.begin(), err.end()), NIIF_ERROR);
        return;
    }
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    cfg.monitor = mon;
    BeginRecording(a, cfg, IsRectEmpty(&rect) ? MonitorRect(mon) : rect);
}

void StartScreenRecording(App& a) {
    StartMonitorRecording(a, MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), RECT{});
}

void StartWindowRecording(App& a, HWND target, const RECT& rect) {
    capwgc::RecorderConfig cfg;
    std::string err;
    if (!BuildConfig(a, false, RECT{}, target, &cfg, &err)) {
        TrayBalloon(a, app::i18n::T(L"無法開始錄影"), std::wstring(err.begin(), err.end()), NIIF_ERROR);
        return;
    }
    BeginRecording(a, cfg, rect);
}

// 視窗清單（Discord 式縮圖牆）。這是同步 modal，跟設定視窗一樣有自己的
// 訊息迴圈；期間 mode 停在 Picking，熱鍵因此不會再開第二個。
void BeginSelection(App& a);

void BeginWindowPicker(App& a) {
    if (a.mode != Mode::Idle) return;
    a.mode = Mode::Picking;
    // 選擇器開著的期間要讓「截圖 / OCR」還能用。
    //
    // 這件事不是自動成立的：全域熱鍵的 WM_HOTKEY 是送給主訊息窗的，而
    // picker 有自己的 modal 訊息迴圈，那個訊息會在它的迴圈裡被抽出來、
    // DispatchMessage 給主訊息窗，然後被「mode != Idle」的判斷默默丟掉。
    // 使用者看到的就是「開著選擇視窗的時候按截圖完全沒反應」。
    //
    // 所以這裡明確把 snip 熱鍵接回來。**只接 snip**：其他幾組（選區域、
    // 錄主螢幕、選擇視窗）在選擇器裡都有等價的入口（分頁與卡片），再攔一次
    // 只會變成「按了卻跳出兩個東西」。
    const app::PickResult r = app::PickSource(
        a.msgwnd, /*live=*/true, [&a](const MSG& m) {
            // 兩個入口都要接：熱鍵（WM_HOTKEY）與托盤的「截圖 / OCR」
            // （WM_APP_SNIP）。只接熱鍵的話，托盤那條路在選擇器開著時仍然
            // 會被 mode != Idle 擋掉，變成「選單可以點、點了沒反應」。
            (void)a;
            if (m.message == WM_HOTKEY) return (int)m.wParam == HOTKEY_SNIP;
            if (m.message == WM_APP_SNIP) return true;
            return false;
        });
    a.mode = Mode::Idle;
    Log("picker done ok=%d region=%d settings=%d sysmsg=%d hwnd=%p mon=%p rect=%ld,%ld,%ld,%ld",
        r.ok ? 1 : 0, r.region ? 1 : 0, r.settings ? 1 : 0, r.sysMsg ? 1 : 0, (void*)r.hwnd,
        (void*)r.monitor, r.rect.left, r.rect.top, r.rect.right, r.rect.bottom);
    // 攔到的熱鍵：選擇器已經關掉了，現在才處理 —— 這樣截圖拍到的是原本的
    // 桌面，不會把選擇器自己拍進去。
    //
    // 用 PostMessage 而不是直接呼叫 WndProc：我們現在還在原本那個 WM_HOTKEY
    // 的處理函式裡面（BeginWindowPicker 就是它叫的），直接重入會變成
    // 「熱鍵 → 開 picker → picker 攔到熱鍵 → 再進一次熱鍵處理」的巢狀堆疊，
    // 出問題時很難看出是怎麼繞回來的。丟回佇列讓它跑一次乾淨的。
    if (r.sysMsg) {
        Log("picker -> replay msg=0x%04X w=%llu", r.msg.message,
            (unsigned long long)r.msg.wParam);
        PostMessageW(a.msgwnd, r.msg.message, r.msg.wParam, r.msg.lParam);
        return;
    }
    // 按了「設定」：picker 已經關掉了，現在才是開設定視窗的時機。順序反過來
    // 的話設定視窗會變成 picker 的 owner，關掉設定時 picker 也會一起走。
    if (r.settings) {
        OpenSettings(a);
        return;
    }
    if (!r.ok) return;
    // 「區域」只是「去框選」這個動作的入口，還沒框 —— picker 已經關掉了，
    // 現在才輪到覆蓋層上場。順序反過來的話使用者框到的會是 picker 自己。
    if (r.region) {
        BeginSelection(a);
        return;
    }
    if (r.hwnd) StartWindowRecording(a, r.hwnd, r.rect);
    else if (r.monitor) StartMonitorRecording(a, r.monitor, r.rect);
}

void BeginSelection(App& a) {
    a.mode = Mode::Selecting;
    const bool ok = a.overlay.Begin([&a](bool okk, const RECT& r, HWND win) {
        a.mode = Mode::Idle;
        Log("overlay done ok=%d rect=%ld,%ld,%ld,%ld win=%p", okk ? 1 : 0,
            r.left, r.top, r.right, r.bottom, (void*)win);
        if (!okk) return;
        if (win) StartWindowRecording(a, win, r);
        else StartRegionRecording(a, r);
    });
    Log("overlay.Begin -> %d", ok ? 1 : 0);
    if (!ok) a.mode = Mode::Idle;
}

void FinishRecording(App& a);
void StopRecording(App& a);

// ---- 截圖 / OCR -------------------------------------------------------------

// 把一張裁好的圖送進剪貼簿，並（可選）存檔。回報使用者成敗 —— 截圖是「按下
// 去就期待有東西」的動作，靜默失敗會讓人以為剪貼簿壞了。
//
// 順序很重要：**先存檔、再進剪貼簿**。剪貼簿除了圖片之外還要放一個 CF_HDROP
// （讓檔案總管能貼上），而 CF_HDROP 帶的是檔名 —— 磁碟上沒有那個檔案就貼不
// 出東西。舊版是先寫剪貼簿再存檔，那個順序做不到「同時貼到小畫家與資料夾」。
void DeliverSnip(App& a, const app::ScreenBitmap& bmp) {
    std::wstring extra;
    bool saved = false;              // 只有真的寫出檔案才是 true
    std::wstring savedPath;          // 給剪貼簿的 CF_HDROP 用

    if (a.settings.snipSaveFile) {
        // 存檔失敗不算整件事失敗：圖片還是進得了剪貼簿，只是檔案總管貼不上。
        // 所以這裡不 return，只把它寫成提示的一部分。
        const std::wstring dir = a.settings.SnipDir();
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        // 檔名：CapWGC_20261020_143012.png（排序就是時間序）
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[128];
        swprintf_s(name, L"CapWGC_%04u%02u%02u_%02u%02u%02u.png", st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::wstring path = dir + L"\\" + name;
        if (bmp.SavePng(path)) {
            a.lastPath = path;
            savedPath = path;
            saved = true;
            extra = L"\n" + WFmt(app::i18n::T(L"已存檔：%s"), name);
        } else {
            extra = std::wstring(L"\n") + app::i18n::T(L"（存檔失敗，但已進剪貼簿）");
        }
    }

    std::wstring err;
    if (!app::CopyToClipboard(a.msgwnd, bmp, savedPath, &err)) {
        Log("snip clipboard FAILED: %s", U8(err).c_str());
        TrayBalloon(a, app::i18n::T(L"截圖失敗"), err, NIIF_ERROR);
        return;
    }
    Log("snip clipboard ok %dx%d hdrop=%d", bmp.width(), bmp.height(),
        savedPath.empty() ? 0 : 1);

    const std::wstring full =
        app::i18n::Fmt(app::i18n::T(L"%d × %d 像素，已複製到剪貼簿"), bmp.width(),
                       bmp.height()) + extra;

    if (!saved) {
        // 沒真正寫出檔案就沒有「開啟」可做 —— 硬給 onClick 只會開到上一次的
        // 檔案，那比不能點更糟。所以標題也不提「點一下」。
        TrayBalloon(a, app::i18n::T(L"已截圖"), full,
                    a.settings.snipSaveFile ? NIIF_WARNING : NIIF_INFO);
        return;
    }
    // 捕獲**指標**不是引用：這個 lambda 會被存進 Toast 活到提示消失，而 `a`
    // 是函式參數，函式一返回就沒了。
    App* ap = &a;
    TrayBalloon(a, app::i18n::T(L"已截圖（點一下開啟）"), full, NIIF_INFO,
                [ap] { OpenLastFile(*ap); });
}

// 把裁好的圖丟給 OCR 引擎。像素直接從記憶體過去，不落地成檔案。
void SendSnipToOcr(App& a, const app::ScreenBitmap& bmp) {
    if (!a.ocrAvailable || !a.ocr.running()) {
        TrayBalloon(a, app::i18n::T(L"OCR 不可用"),
                    app::i18n::T(L"OCR 模型沒有載入成功，請看托盤提示或 log"),
                    NIIF_WARNING);
        return;
    }
    // 直接把記憶體裡的像素送過去。舊版是先寫一張暫存 PNG、再讓 Python 用
    // Pillow 讀回來 —— 一來一回多兩次編解碼（4K 圖各要幾十毫秒），而且
    // %TEMP% 會積圖。現在整條路都在行程內，這個暫存檔就沒有存在理由了。
    //
    // 這裡刻意**不彈「辨識中…」**。理由跟換掉托盤氣球是同一件事的兩面：
    // 推論只要 0.4~0.7 秒，而任何一則提示都會被使用者看到（我們的自己的視窗
    // 也一樣），掛在那裡四秒反而變成「字都貼好了它還說在辨識」。一個不到
    // 一秒的動作不需要進度指示 —— 工具列上點的就是 OCR 那一格，按下去就知道
    // 接下來會發生什麼。
    //
    // 真的等很久的情況只有一種：請求排在別的請求後面（連續按兩次）。那時
    // 也不需要進度 —— 上一次的結果提示還在畫面上，本身就是進度。
    a.snipOcrId = a.ocr.Request((const uint8_t*)bmp.bits(), bmp.width(), bmp.height(),
                                bmp.width() * 4);
    Log("ocr request id=%d %dx%d", a.snipOcrId, bmp.width(), bmp.height());
    if (a.snipOcrId <= 0) {
        TrayBalloon(a, app::i18n::T(L"OCR 失敗"), app::i18n::T(L"無法送出辨識請求"), NIIF_ERROR);
    }
}

// 使用者框選完成（或取消）之後進來。rect 是螢幕座標。
void OnSnipDone(App& a, const app::Overlay::SnipOutcome& o) {
    a.mode = Mode::Idle;
    Log("snip done ok=%d action=%d rect=%ld,%ld,%ld,%ld", o.ok ? 1 : 0,
        (int)o.action, o.rect.left, o.rect.top, o.rect.right, o.rect.bottom);
    if (!o.ok) {
        a.snipFull.Free();
        return;
    }
    if (!a.snipFull.valid()) {
        // 理論上不會發生（熱鍵那條路一定先抓了圖），但覆蓋層是獨立元件，
        // 之後可能有人別的呼叫路徑忘了抓圖 —— 那時要能自己補救。
        std::wstring err;
        if (!a.snipFull.CaptureVirtualScreen(&err)) {
            TrayBalloon(a, app::i18n::T(L"截圖失敗"), err, NIIF_ERROR);
            return;
        }
    }

    app::ScreenBitmap crop;
    if (!a.snipFull.CropTo(o.rect, &crop)) {
        Log("snip crop FAILED rect=%ld,%ld,%ld,%ld", o.rect.left, o.rect.top,
            o.rect.right, o.rect.bottom);
        TrayBalloon(a, app::i18n::T(L"截圖失敗"), app::i18n::T(L"選取範圍是空的"), NIIF_WARNING);
        a.snipFull.Free();
        return;
    }
    a.snipFull.Free();   // 全螢幕那張用完了，早點還記憶體（一張 4K 是 33MB）

    if (o.action == ui::Toolbar::Action::Ocr) {
        SendSnipToOcr(a, crop);
    } else {
        DeliverSnip(a, crop);
    }
}

// wantOcr：這次進來時工具列要不要一開始就選在 OCR。
//
// 一般熱鍵 / 托盤都要傳 false —— 也就是「預設是截圖」。只有托盤的
// 「截圖並取字」那個選項傳 true。
//
// 為什麼不用 settings 裡的旗標決定？因為那條路一定會被污染：只要有任何
// 一支程式碼把「使用者這次選了什麼」寫回去，之後每次進來就再也不是
// 「預設截圖」了（第一版就是這樣壞的）。把它變成一個**呼叫端明確傳入**
// 的參數，預設值就不會隨使用歷史漂移。
void BeginSnip(App& a, bool wantOcr) {
    if (a.mode != Mode::Idle) return;
    a.mode = Mode::SnipSelect;

    // 先抓原圖（在遮罩出現之前）。見 App 裡 snipFull 的說明。
    std::wstring err;
    if (!a.snipFull.CaptureVirtualScreen(&err)) {
        Log("snip capture FAILED: %s", U8(err).c_str());
        a.mode = Mode::Idle;
        TrayBalloon(a, app::i18n::T(L"截圖失敗"), err, NIIF_ERROR);
        return;
    }

    // 工具列一開始選中哪一邊：預設「截圖」，要取字才由呼叫端傳 wantOcr。
    // 引擎沒就緒時一律退回截圖（OCR 那一格本來就是灰的，點不到）。
    const ui::Toolbar::Action def =
        (wantOcr && a.ocrAvailable) ? ui::Toolbar::Action::Ocr
                                    : ui::Toolbar::Action::Snip;

    const bool ok = a.overlay.BeginSnip(def, [&a](const app::Overlay::SnipOutcome& o) {
        OnSnipDone(a, o);
    });
    Log("overlay.BeginSnip -> %d ocrReady=%d def=%d", ok ? 1 : 0,
        a.ocrAvailable ? 1 : 0, (int)def);
    if (!ok) {
        a.mode = Mode::Idle;
        a.snipFull.Free();
        return;
    }
    // 引擎還沒好就把 OCR 畫成灰的（而不是讓它點下去才失敗）。
    a.overlay.SetOcrAvailable(a.ocrAvailable);
}

// 把文字放進剪貼簿。成功回 true。
//
// 抽出來是因為它現在是 OCR 的**唯一**交付管道（以前還會彈一個面板），
// 而且面板那條路失敗時也要靠它當退路。
bool PutTextOnClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

// OCR 結果的交付：**只進剪貼簿**，不在畫面上畫東西。
//
// 為什麼拿掉面板：使用者按 OCR 的唯一目的就是「把字拿去別的地方用」，
// 而文字已經在剪貼簿裡了。再彈一個蓋在畫面正中央的面板，等於強迫他
// 先處理這個面板（點一下或按 Esc 才消失）才能繼續做事 —— 是純粹的干擾。
// 而且面板會蓋住他剛剛框選的那塊內容，想對照反而看不到了。
//
// 所以現在：進剪貼簿 → 一則提示講一句「已複製」就結束。要看結果就貼到
// 任何編輯器去，那才是使用者本來就要做的下一個動作。
void ShowOcrResult(App& a, const app::OcrResult& r) {
    std::wstring text;
    for (size_t i = 0; i < r.lines.size(); ++i) {
        if (i) text += L"\n";
        text += r.lines[i];
    }

    // 沒辨識到字不算失敗（可能框到的是圖片），但不能靜默 —— 否則使用者
    // 會以為剪貼簿裡有東西，貼下去才發現是舊的。
    if (text.empty()) {
        Log("ocr clip: no text");
        TrayBalloon(a, app::i18n::T(L"沒有辨識到文字"), app::i18n::T(L"換一個範圍再試一次"),
                    NIIF_WARNING);
        return;
    }

    if (!PutTextOnClipboard(a.msgwnd, text)) {
        // 剪貼簿被別的程式佔住。文字不能就這樣消失 —— 退回提示，至少把前
        // 120 字顯示出來讓使用者手動抄。
        //
        // 為什麼是 120 而不是 200：提示窗會折行，200 字大約是七、八行，
        // 那已經是一塊蓋住畫面的面板而不是「一則提示」了。手抄的實用上限
        // 也就是幾行，再多就得靠剪貼簿，而剪貼簿這條路剛好失敗了。
        Log("ocr clip: FAILED");
        std::wstring head = text.substr(0, 120);
        if (text.size() > 120) head += L"…";
        TrayBalloon(a, app::i18n::T(L"OCR 完成（複製失敗，需手動抄）"), head, NIIF_WARNING);
        return;
    }

    Log("ocr clip ok n_lines=%zu chars=%zu", r.lines.size(), text.size());
    // 只講「幾行、幾個字」，不重複內容 —— 內容已經在剪貼簿，貼出來就看到了。
    // 前面若有一則「已截圖」還在畫面上，這一則會直接換掉它（自己的視窗
    // 沒有佇列，見 TrayBalloon 的說明）。
    const std::wstring msg =
        app::i18n::Fmt(app::i18n::T(L"已複製 %zu 行／%zu 字，直接貼上即可"),
                       r.lines.size(), text.size());
    TrayBalloon(a, app::i18n::T(L"辨識完成"), msg, NIIF_INFO);
}

// ---- 漂浮控件的點擊 ---------------------------------------------------------
//
// 只在 Recording 狀態有效。兩顆鈕都是即時動作，沒有需要 worker 的重活。
void OnControlBar(App& a, app::ControlBar::Button b) {
    if (a.mode != Mode::Recording) return;

    if (b == app::ControlBar::Button::Stop) {
        Log("bar: stop");
        StopRecording(a);
        return;
    }
    if (b != app::ControlBar::Button::Mic) return;

    if (!a.rec || !a.rec->hasMic()) {
        // 這次錄影沒有麥克風軌（設定沒勾，或端點被獨佔開不起來）。鈕畫成灰的
        // 很容易被當成「壞了」，所以點下去要講清楚原因。
        const capwgc::RecorderStats st = a.rec ? a.rec->stats() : capwgc::RecorderStats{};
        std::wstring msg = app::i18n::T(L"這次錄影沒有麥克風音軌");
        if (!st.audioNote.empty()) {
            msg += L"\n" + std::wstring(st.audioNote.begin(), st.audioNote.end());
        } else {
            msg += std::wstring(L"\n") + app::i18n::T(L"在設定裡勾選「麥克風」後再錄一次");
        }
        TrayBalloon(a, app::i18n::T(L"沒有麥克風"), msg, NIIF_INFO);
        return;
    }

    a.micMuted = !a.micMuted;
    a.rec->SetMicMuted(a.micMuted);
    a.bar.SetMicMuted(a.micMuted);
    TrayUpdate(a, true, a.traySec);
    Log("bar: mic muted=%d", a.micMuted ? 1 : 0);
}

void StopRecording(App& a) {
    if (a.mode != Mode::Recording) return;
    a.mode = Mode::Stopping;
    // 立刻收掉控件：使用者按下停止卻還看到那顆膠囊，會以為沒收到點擊。
    a.bar.Hide();
    // 控件走了，提示可以回到正常的右下角位置（「已儲存」那則就在下面）。
    a.toast.SetBottomInset(0);
    if (a.stopper.joinable()) a.stopper.join();
    App* ap = &a;
    a.stopper = std::thread([ap] {
        if (ap->rec) ap->rec->Stop();
        PostMessageW(ap->msgwnd, WM_APP_REC_DONE, 0, 0);
    });
}

void FinishRecording(App& a) {
    if (a.stopper.joinable()) a.stopper.join();

    capwgc::RecorderStats st;
    if (a.rec) st = a.rec->stats();
    const std::wstring path = a.rec ? a.rec->path() : std::wstring();

    Log("rec.Finish inited=%d frames=%llu seconds=%.2f bytes=%llu au=%llu err=%s",
        st.inited ? 1 : 0, (unsigned long long)st.frames, st.seconds,
        (unsigned long long)st.bytes, (unsigned long long)st.audioAU, st.error.c_str());
    a.bar.Hide();
    a.toast.SetBottomInset(0);
    a.mode = Mode::Idle;
    a.traySec = -1;
    TrayUpdate(a, false);

    if (!st.error.empty() || !st.inited || st.frames == 0) {
        // frames == 0 也要當失敗：被錄的視窗若完全靜止，WGC 可能只送那一幀
        // 用來開編碼器的畫面，成品就是一個沒有影像的空檔。報「已儲存」會讓
        // 人以為錄到了，等打開才發現什麼都沒有。
        a.lastError = !st.error.empty() ? st.error
                     : (st.inited ? "沒有寫入任何畫面（目標可能完全靜止）"
                                  : "沒有收到任何畫面");
        TrayBalloon(a, app::i18n::T(L"錄影失敗"), std::wstring(a.lastError.begin(), a.lastError.end()),
                    NIIF_ERROR);
    } else {
        a.lastPath = path;
        const double mb = (double)st.bytes / 1048576.0;
        // 檔名可以很長（MAX_PATH），翻譯也可以很長 —— 不要用固定緩衝區。
        const std::wstring buf = app::i18n::Fmt(
            app::i18n::T(L"%s  %.1f 秒  %.1f MB  %hs"),
            path.substr(path.find_last_of(L'\\') + 1).c_str(), st.seconds, mb,
            st.codecName.c_str());
        if (!st.audioNote.empty()) {
            // 畫面錄到了但音源沒開起來：不要讓它安靜地變成一個沒有聲音的檔案
            TrayBalloon(a, app::i18n::T(L"已儲存（音源有問題）"),
                        buf + L"\n" +
                            std::wstring(st.audioNote.begin(), st.audioNote.end()),
                        NIIF_WARNING);
        } else {
            // 標題寫「點一下開啟」就得真的開得起來。拿掉這個 onClick 的話，
            // 點下去只會把提示關掉（見 TrayBalloon 上方那段說明）。
            //
            // 捕獲**指標**不是引用：這個 lambda 會被存進 Toast，活到提示消失
            // 為止，而 `a` 是函式參數，函式一返回就沒了。
            App* ap = &a;
            TrayBalloon(a, app::i18n::T(L"已儲存（點一下開啟）"), buf, NIIF_INFO,
                        [ap] { OpenLastFile(*ap); });
        }
    }
    a.rec.reset();
}

void OpenOutputFolder(App& a) {
    std::wstring dir = a.settings.outputDir;
    if (dir.empty()) dir = app::Settings::DefaultOutputDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenLastFile(App& a) {
    if (a.lastPath.empty()) {
        // 不該發生：只有真的寫出檔案才會接 onClick。真的發生就是接了線但
        // 沒有目標，靜默什麼都不做會讓「點一下開啟」看起來像壞掉。
        Log("open.last: no path (toast was clickable but lastPath is empty)");
        return;
    }
    // 留痕：這條路徑沒有其他可觀察的結果（ShellExecute 成功也只是交出控制權），
    // 要驗「點一下到底有沒有開」只能靠這行。
    Log("open.last: %s", U8(a.lastPath).c_str());
    ShellExecuteW(nullptr, L"open", a.lastPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 控件位置的逃生門。
//
// 拖曳與顯示兩條路都會把控件夾回工作區，正常情況不會跑到畫面外。這個選單
// 項目是給「換螢幕 / 拔掉第二顆螢幕 / 手改過 INI」之後的保險：使用者不必
// 知道設定檔在哪，按一下就能回到預設位置。錄影中按也會立刻歸位。
void ResetBarPos(App& a) {
    a.settings.hasBarPos = false;
    a.settings.barDx = 0;
    a.settings.barDy = 0;
    a.settings.Save();
    if (a.mode == Mode::Recording && a.rec) {
        a.bar.Show(a.captureRect, a.rec->hasMic(), nullptr);
    }
    Log("bar position reset");
    TrayBalloon(a, app::i18n::T(L"控件位置已重設"),
                app::i18n::T(L"回到右下角（錄影中會立刻歸位）"), NIIF_INFO);
}

void ShowTrayMenu(App& a) {
    HMENU m = CreatePopupMenu();
    const bool rec = (a.mode == Mode::Recording);
    const bool busy = (a.mode != Mode::Idle);
    const app::Hotkeys& hk = a.settings.hotkeys;

    // 選單上的熱鍵文字要跟著設定走：寫死 Ctrl+Alt+A 的話，使用者改完熱鍵
    // 會看到兩個互相矛盾的說明，然後開始懷疑自己有沒有改成功。
    const std::wstring mRegion = std::wstring(app::i18n::T(L"錄製區域")) + L"\t" +
                                 app::HotkeyText(hk.region);
    const std::wstring mPicker = std::wstring(app::i18n::T(L"選擇視窗…")) + L"\t" +
                                 app::HotkeyText(hk.picker);
    const std::wstring mScreen = std::wstring(app::i18n::T(L"錄製主螢幕")) + L"\t" +
                                 app::HotkeyText(hk.screen);
    const std::wstring mStop = std::wstring(app::i18n::T(L"停止")) + L"\t" +
                               app::HotkeyText(hk.stop);
    const std::wstring mSnip = std::wstring(app::i18n::T(L"截圖 / OCR")) + L"\t" +
                               app::HotkeyText(hk.snip);

    // 「選擇視窗」放第一位：它是最常用的入口（托盤單擊也是它），而且它不
    // 會立刻開始做任何事 —— 先選來源、再決定要錄什麼，順序上就該排最前面。
    // 錄影類的三項排在後面，需要「已經想清楚要錄哪一塊」才會點。
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), IDM_PICKER, mPicker.c_str());
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), IDM_REGION, mRegion.c_str());
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), IDM_SCREEN, mScreen.c_str());
    AppendMenuW(m, MF_STRING | (rec ? 0 : MF_GRAYED), IDM_STOP, mStop.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // 「截圖 / OCR」在選擇器開著的時候**不變灰**：那正是需求裡「選視窗時不要
    // 禁止截圖 / OCR」的托盤那一半。實際執行會排到選擇器關掉之後（見
    // WM_APP_SNIP 的處理），所以不會出現「兩個 modal 疊在一起」。
    const bool snipBusy = (a.mode != Mode::Idle && a.mode != Mode::Picking);
    AppendMenuW(m, MF_STRING | (snipBusy ? MF_GRAYED : 0), IDM_SNIP, mSnip.c_str());
    AppendMenuW(m, MF_STRING | (snipBusy ? MF_GRAYED : 0), IDM_SNIP_OCR,
                app::i18n::T(L"截圖後直接 OCR"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), IDM_SETTINGS, app::i18n::T(L"設定…"));
    AppendMenuW(m, MF_STRING, IDM_RESETBAR, app::i18n::T(L"重設控件位置"));
    AppendMenuW(m, MF_STRING, IDM_FOLDER, app::i18n::T(L"開啟輸出資料夾"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, app::i18n::T(L"結束"));

    POINT pt{};
    GetCursorPos(&pt);
    // 沒有 SetForegroundWindow，選單不會因為點到別處而消失（經典的托盤選單坑）。
    SetForegroundWindow(a.msgwnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, a.msgwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(a.msgwnd, WM_NULL, 0, 0);

    switch (cmd) {
    case IDM_REGION:
        if (a.mode == Mode::Idle) BeginSelection(a);
        break;
    case IDM_PICKER:
        BeginWindowPicker(a);
        break;
    case IDM_SCREEN:
        if (a.mode == Mode::Idle) StartScreenRecording(a);
        break;
    case IDM_STOP:
        StopRecording(a);
        break;
    case IDM_SNIP:
        // 一律走訊息而不是直接呼叫：選擇器開著時，這則訊息會被它的訊息迴圈
        // 攔下來，等選擇器關掉之後才真正執行（見 BeginWindowPicker）。
        if (a.mode == Mode::Idle || a.mode == Mode::Picking) {
            PostMessageW(a.msgwnd, WM_APP_SNIP, 0, 0);
        }
        break;
    case IDM_SNIP_OCR:
        // 托盤的「截圖並取字」：這次進來就選在 OCR，但**不**寫回設定。
        if (a.mode == Mode::Idle || a.mode == Mode::Picking) {
            PostMessageW(a.msgwnd, WM_APP_SNIP, 1, 0);
        }
        break;
    case IDM_SETTINGS:
        if (a.mode == Mode::Idle) OpenSettings(a);
        break;
    case IDM_RESETBAR:
        ResetBarPos(a);
        break;
    case IDM_FOLDER:
        OpenOutputFolder(a);
        break;
    case IDM_QUIT:
        PostMessageW(a.msgwnd, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

// 設定：改的是一份複本，確定之後才套用。熱鍵另外走 ApplyHotkeys，
// 它會在被佔用時回滾 —— 但只回滾熱鍵，其他設定照收。
void OpenSettings(App& a) {
    app::Settings next = a.settings;
    bool wantPrtsc = false;
    if (!app::ShowSettingsDialog(a.msgwnd, &next, &wantPrtsc)) {
        // 設定視窗裡換語言是**當場生效**的（重建頁面），按取消就要退回去，
        // 否則會出現「介面變了但 ini 沒變」—— 下次啟動又變回來，很像 bug。
        app::i18n::Load(a.settings.lang.c_str());
        return;
    }

    // PrtScn 接管要先處理，而且**要在其他設定套用之前**判斷它成不成功：
    // 它牽涉到系統設定與熱鍵兩種副作用，失敗時 next 會被打回原狀的部分
    // 必須是最終結果，不能先存檔再回頭改。
    const bool wasOwned = app::PrtScnOwned();
    if (wantPrtsc && !wasOwned) {
        std::wstring why;
        if (!EngagePrtScn(a, &why)) {
            next.hotkeys = a.settings.hotkeys;   // 熱鍵已經回滾過了，這裡同步欄位
            TrayBalloon(a, app::i18n::T(L"PrtScn 沒有接手成功"), why, NIIF_WARNING);
            // 其餘設定照收 —— 一個系統設定被鎖住不該讓使用者白填整個頁面。
        }
    } else if (!wantPrtsc && wasOwned) {
        std::wstring why;
        if (!ReleasePrtScn(a, &why)) {
            TrayBalloon(a, app::i18n::T(L"PrtScn 還原失敗"), why, NIIF_WARNING);
        }
        next.hotkeys = a.settings.hotkeys;
    }

    std::wstring failed;
    const app::Hotkeys want = next.hotkeys;
    if (!ApplyHotkeys(a, want, &failed)) {
        next.hotkeys = a.settings.hotkeys;   // 保留舊的可用組合
        TrayBalloon(a, app::i18n::T(L"熱鍵沒有套用"),
                    WFmt(app::i18n::T(L"「%s」的組合已被其他程式使用，已維持原本的熱鍵"),
                         failed.c_str()),
                    NIIF_WARNING);
    }
    a.settings = next;
    a.settings.Save();
    TrayUpdate(a, a.mode == Mode::Recording);
}

// ---- 視窗訊息 -------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = g_app;
    if (!a) return DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_HOTKEY:
        Log("hotkey id=%llu mode=%d", (unsigned long long)w, (int)a->mode);
        if (a->mode == Mode::Idle && w == HOTKEY_REGION) {
            BeginSelection(*a);
        } else if (a->mode == Mode::Idle && w == HOTKEY_PICKER) {
            BeginWindowPicker(*a);
        } else if (a->mode == Mode::Idle && w == HOTKEY_SCREEN) {
            StartScreenRecording(*a);
        } else if (a->mode == Mode::Idle && w == HOTKEY_SNIP) {
            BeginSnip(*a, false);
        } else if (w == HOTKEY_STOP) {
            StopRecording(*a);
        }
        return 0;

    case WM_APP_OCR:
        // OCR 引擎的結果與就緒通知（都在這個 UI 執行緒被叫）。
        a->ocr.HandleMessage(w, l);
        return 0;

    case WM_APP_SNIP:
        // 托盤的「截圖 / OCR」。wParam 非 0 = 直接選 OCR。
        //
        // 走訊息而不是直接呼叫 BeginSnip，是為了讓**選擇器開著時**也能用：
        // picker 有自己的 modal 訊息迴圈，它會攔下這則訊息、關掉自己，再把
        // 訊息放回來 —— 那時才是真正可以截圖的時機（見 BeginWindowPicker）。
        if (a->mode == Mode::Idle) BeginSnip(*a, w != 0);
        else Log("snip ignored (mode=%d)", (int)a->mode);
        return 0;

    case WM_APP_SNIP_OCR_TEST:
        // QA 專用入口（--snipocr）。走的是和托盤「截圖後直接 OCR」完全一樣的
        // 那條呼叫，只是不由選單觸發 —— 選單是 TrackPopupMenu 直取回傳值，
        // PostMessage 打不到，自動化測試需要一個等效的入口。
        if (a->mode == Mode::Idle) BeginSnip(*a, true);
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(*a);
            return 0;
        // 托盤單擊 = 開主視窗（選擇視窗），這是 Windows 上托盤圖示的通用約定：
        // 使用者點它心裡想的是「把程式叫出來」，不是「開始錄影」。所以這裡
        // **不再**轉成 HOTKEY_REGION —— 那會讓人點一下就進框選，選錯還得取消。
        //
        // 用 PostMessage 而不是直接呼叫：BeginWindowPicker 是同步 modal（內部
        // 有自己的訊息迴圈），在托盤通知的處理函式裡開它等於在 WM_LBUTTONUP
        // 中間岔出一段迴圈，滑鼠放開的處理會變得很難預期。
        case WM_LBUTTONUP:
            if (a->mode == Mode::Idle) PostMessageW(h, WM_HOTKEY, HOTKEY_PICKER, 0);
            return 0;
        case WM_LBUTTONDBLCLK:
            // 單擊已經開主視窗了，雙擊走到這裡大多是「連點兩下」的副作用。
            // 保留這個分支只是為了讓它和單擊一致，而不是掉進 default 變成沒反應。
            if (a->mode == Mode::Idle) PostMessageW(h, WM_HOTKEY, HOTKEY_PICKER, 0);
            return 0;
        case NIN_BALLOONUSERCLICK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // 已經沒有 Shell 氣球了，這幾則理論上不會再來。留著是為了「萬一
            // 某天又有人用 NIF_INFO」時行為不會默默變成沒反應 —— 但也因此
            // 不能當成「點一下開啟」的實作，那條現在在 ui::Toast 的
            // WM_LBUTTONDOWN 裡（見 TrayBalloon 上方的說明）。
            OpenLastFile(*a);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_APP_REC_DONE:
        FinishRecording(*a);
        return 0;

    case WM_TIMER:
        if (w == TIMER_TICK && a->mode == Mode::Recording && a->rec) {
            const capwgc::RecorderStats st = a->rec->stats();

            // 計時改放在托盤提示（畫面上只留兩顆圖示）。Shell_NotifyIcon 一次
            // 幾十微秒，但沒必要每秒叫它四次 —— 整秒變了才更新。
            const int sec = (int)st.seconds;
            if (sec != a->traySec) {
                a->traySec = sec;
                TrayUpdate(*a, true, sec);
            }

            // 擷取目標消失（視窗關了 / 顯示拓撲變了）：直接收尾，不要讓它
            // 掛在那裡看起來像還在錄。
            if (st.captureClosed) {
                StopRecording(*a);
                return 0;
            }
            // 4 秒還收不到任何一幀 → 目標大概選錯了（例如錄到自己）。
            if (!st.inited && !a->warnedNoFrame &&
                GetTickCount64() - a->startTick > 4000) {
                a->warnedNoFrame = true;
                TrayBalloon(*a, app::i18n::T(L"等不到畫面"),
                        app::i18n::T(L"擷取目標沒有送出任何幀，已停止"),
                            NIIF_WARNING);
                StopRecording(*a);
            }
            if (!st.error.empty()) a->lastError = st.error;
        }
        return 0;

    case WM_CLOSE:
        if (a->mode == Mode::Recording) StopRecording(*a);
        if (a->stopper.joinable()) a->stopper.join();
        a->bar.Destroy();
        a->toast.Destroy();
        TrayRemove(*a);
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- 自我測試（不需要互動，用來驗證 Recorder 這條路徑）---------------------
//
// 為什麼要有：GUI 的錄製路徑如果只能靠人手動點，每次改動都得「開程式、拖框、
// 等 5 秒、看檔案」，而且沒辦法在 CI 或腳本裡跑。這個模式直接把 Recorder
// 跑一輪，輸出與 CLI 同格式的統計。
//
// WINDOWS 子系統的程式沒有 stdout，所以同時寫一份 selftest.log 到 exe 旁邊 ——
// AttachConsole 只在「從主控台啟動」時有效，從工作排程或測試腳本叫起來就沒有。
FILE* g_selftestLog = nullptr;
FILE* g_selftestLogW = nullptr;   // 寬字元版：見 PW

void P(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_selftestLog) {
        va_start(ap, fmt);
        vfprintf(g_selftestLog, fmt, ap);
        va_end(ap);
        fflush(g_selftestLog);
    }
}

// 需要印中文字串時用這個。
//
// `printf("%ls", 中文)` 在 "C" locale 下會把非 ASCII 字元換成 '?'（MSVCRT 的
// 窄 printf 拿當前 locale 做寬→窄轉換，轉不動就填 '?'）——症狀是「測試輸出
// 的中文全部變成問號」，看起來像編碼設定錯了，其實是 stream 的走向不對。
//
// 兩個必要的動作：
//   - stdout 用 _setmode(_O_U8TEXT) 切成寬字元模式
//   - 日誌檔用 "ccs=UTF-8" 開（純 "w" 開的是位元組流，vfwprintf 一樣會填 '?'）
// 這兩個 stream 從此只給 PW 用；P 只用於純 ASCII 的 selftest，兩者不混。
void PWOpenWide() {
    _setmode(_fileno(stdout), _O_U8TEXT);
}

void PW(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_selftestLogW) {
        va_start(ap, fmt);
        vfwprintf(g_selftestLogW, fmt, ap);
        va_end(ap);
        fflush(g_selftestLogW);
    }
}

// 只顯示錄製中的漂浮控件，不錄影。用來單獨檢查 UI 元件有沒有畫出來 ——
// 整條流程一起測的時候，分不清是「元件沒畫」還是「狀態機沒走到」。
//
// 加 --allow-capture 才截得到圖（預設控件被排除在擷取之外，截圖會是空的）。
int RunHudTest(int seconds) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
    }
    const RECT mon = MonitorRect(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    RECT r{mon.left + 120, mon.top + 90, mon.right - 200, mon.bottom - 160};
    // 順便放一塊遮罩在左上角：用來確認「分層視窗（LWA_ALPHA）到底有沒有被
    // 截圖抓到」。抓不到的話，之後所有遮罩的 QA 都會誤判成「沒畫出來」。
    ui::DimPanel dim;
    dim.Create(110);
    dim.SetRect(RECT{mon.left, mon.top, mon.left + (mon.right - mon.left) / 2,
                     mon.top + (mon.bottom - mon.top) / 2});

    app::ControlBar bar;
    bar.Create([](app::ControlBar::Button b) {
        printf("hudtest: click %s\n", b == app::ControlBar::Button::Stop ? "stop" : "mic");
        fflush(stdout);
    });
    bar.Show(r, true, nullptr);

    for (int i = 0; i <= seconds * 4; ++i) {
        // 前兩秒不動，之後每半秒切一次靜音，順便驗圖示會不會跟著換。
        if (i >= 8) bar.SetMicMuted((i / 2) % 2 == 1);
        // 沒有訊息迴圈就沒有 WM_PAINT：視窗會「存在但一片空白」，
        // 看起來跟「元件壞了」一模一樣。
        MSG m{};
        const ULONGLONG until = GetTickCount64() + 250;
        while (GetTickCount64() < until) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(10);
        }
    }
    bar.Hide();
    printf("hudtest: done\n");
    return 0;
}

// 只開截圖框選（含工具列）不真的截圖，用來單獨檢查工具列有沒有畫出來。
// 選完把結果印出來就結束 —— 這樣 QA 可以純粹驗「工具列外觀 + 命中測試」。
//
// 加 --allow-capture 才截得到圖嗎？不用：工具列刻意**沒有**做 display
// exclusion（它本來就該出現在截圖裡），所以一般截圖就看得到。
int RunSnipTest(int seconds) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
    }
    app::Overlay ov;
    const bool ok = ov.BeginSnip(ui::Toolbar::Action::Snip,
                                 [](const app::Overlay::SnipOutcome& o) {
        printf("sniptest: done ok=%d action=%d rect=%ld,%ld,%ld,%ld\n", o.ok ? 1 : 0,
               (int)o.action, o.rect.left, o.rect.top, o.rect.right, o.rect.bottom);
        fflush(stdout);
    });
    printf("sniptest: BeginSnip -> %d\n", ok ? 1 : 0);
    fflush(stdout);

    // 讓框選跑一段時間（沒有訊息迴圈就沒有 WM_PAINT，工具列會「存在但空白」）。
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)seconds * 1000;
    MSG m{};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
        if (!ov.active()) break;   // 使用者按了 Esc / 選完了
    }
    ov.Cancel();
    // 再抽一次訊息，讓 Cancel 的 PostMessage 有機會被處理（拆掉視窗）。
    for (int i = 0; i < 50; ++i) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
    printf("sniptest: exit\n");
    return 0;
}

// 熱鍵字串的往返測試。這一段沒有 UI，失敗時只會表現成「INI 裡的值被默默換掉」，
// 所以用一個不需要互動的入口把它釘住：
//   HotkeyText(HotkeyFromText(s)) == s 的標準形式
// 解析失敗必須回 false（而不是回一個半套的 Hotkey，那會讓使用者的設定
// 在某次啟動後變成別的東西）。
//
// 輸出全部走 PW（寬字元）：這個測試有一半的字串是中文，用窄 printf 會在第一個
// 中文字元截斷（見 PW 的說明）。
int RunHotkeyTest() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    logPath = logPath.substr(0, logPath.find_last_of(L'\\') + 1) + L"hotkeytest.log";
    // "ccs=UTF-8"：開成寬字元流，不然 vfwprintf 的中文會變成 '?'（見 PW）
    g_selftestLogW = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
    PWOpenWide();

    struct Case {
        const wchar_t* in;
        const wchar_t* out;   // nullptr = 應該解析失敗
    };
    // 期待值是 HotkeyText 的標準形式：修飾鍵固定 Ctrl → Alt → Shift → Win，
    // 鍵名固定用 VkName 的寫法。所以 "Win+Alt+P" 會被正規化成 "Alt+Win+P"。
    static const Case kCases[] = {
        {L"Ctrl+Alt+A", L"Ctrl+Alt+A"},
        {L"ctrl+alt+a", L"Ctrl+Alt+A"},
        {L"Ctrl + Alt + A", L"Ctrl+Alt+A"},
        {L"Control+Alt+A", L"Ctrl+Alt+A"},
        {L"Ctrl+Shift+F5", L"Ctrl+Shift+F5"},
        {L"Win+Alt+P", L"Alt+Win+P"},
        {L"Windows+Ctrl+S", L"Ctrl+Win+S"},
        {L"Ctrl+Alt+Space", L"Ctrl+Alt+Space"},
        {L"Ctrl+Alt+PageDown", L"Ctrl+Alt+PageDown"},
        {L"Ctrl+Alt+VK42", L"Ctrl+Alt+B"},
        {L"Ctrl+Alt+PrtSc", L"Ctrl+Alt+PrintScreen"},
        {L"Ctrl+Alt+;", L"Ctrl+Alt+;"},
        {L"Ctrl+Alt+Num5", L"Ctrl+Alt+Num5"},
        {L"Ctrl+Alt+Enter", L"Ctrl+Alt+Enter"},
        {L"Ctrl+Alt+Escape", L"Ctrl+Alt+Esc"},
        // 大小寫與空白都吃得住（手改 INI 最常見的兩種寫法）
        {L" ctrl+alt+F9 ", L"Ctrl+Alt+F9"},
        {L"CTRL+ALT+NUMPAD7", L"Ctrl+Alt+Num7"},
        // 應該失敗：沒有修飾鍵 / 沒有鍵 / 兩個鍵 / 保留鍵 / 垃圾
        {L"A", nullptr},
        {L"Ctrl", nullptr},
        {L"Ctrl+Alt", nullptr},
        {L"Ctrl+A+B", nullptr},
        {L"Ctrl+Alt+F12", nullptr},
        {L"Ctrl+Alt+NotAKey", nullptr},
        {L"Ctrl+Ctrl+S", L"Ctrl+S"},   // 重複的修飾鍵不算錯，正規化掉就好
        {L"", nullptr},
    };

    int bad = 0, n = 0;
    for (const auto& c : kCases) {
        ++n;
        app::Hotkey hk;
        const bool ok = app::HotkeyFromText(c.in, &hk);
        if (c.out == nullptr) {
            if (ok) {
                PW(L"FAIL parse [%ls] should fail, got [%ls]\n", c.in,
                   app::HotkeyText(hk).c_str());
                ++bad;
            } else {
                PW(L"ok   reject [%ls]\n", c.in);
            }
            continue;
        }
        if (!ok) {
            PW(L"FAIL parse [%ls] -> failed, want [%ls]\n", c.in, c.out);
            ++bad;
            continue;
        }
        const std::wstring got = app::HotkeyText(hk);
        if (got != c.out) {
            PW(L"FAIL text [%ls] -> [%ls], want [%ls]\n", c.in, got.c_str(), c.out);
            ++bad;
        } else {
            PW(L"ok   [%ls] -> [%ls]\n", c.in, got.c_str());
        }
    }

    // 重複檢查：兩個動作綁同一組時要抓得出來（設定視窗就是靠它擋在按確定之前）
    app::Hotkeys dup = app::Hotkeys::Default();
    dup.stop = dup.region;
    std::wstring which;
    if (dup.Duplicate(&which)) {
        PW(L"ok   duplicate detection: %ls\n", which.c_str());
        ++n;
    } else {
        PW(L"FAIL duplicate detection missed a conflict\n");
        ++n;
        ++bad;
    }

    app::Hotkeys uniq = app::Hotkeys::Default();
    std::wstring w2;
    if (uniq.Duplicate(&w2)) {
        PW(L"FAIL default set reports a duplicate (%ls)\n", w2.c_str());
        bad += 1;
    } else {
        PW(L"ok   default set is unique\n");
    }
    ++n;

    // 只有修飾鍵的組合（HK 清空）也必須被當成「有問題」，不能讓它進 RegisterHotKey
    const app::Hotkey modOnly{MOD_CONTROL, 0};
    if (app::HotkeyUsable(modOnly) || modOnly.set()) {
        PW(L"FAIL modifier-only combination is treated as usable\n");
        ++bad;
    } else {
        PW(L"ok   modifier-only rejected\n");
    }
    ++n;

    PW(L"hotkeytest: %d/%d\n", n - bad, n);
    PW(L"hotkeytest: %s\n", bad == 0 ? L"PASS" : L"FAIL");
    if (g_selftestLogW) { fclose(g_selftestLogW); g_selftestLogW = nullptr; }
    return bad == 0 ? 0 : 1;
}

// 幀率閘門的純數學驗證：不需要真的擷取、不需要螢幕刷新率配合，所以可以
// 一次把所有「來源幀率 × 目標幀率」跑完。這正是硬下界版本的問題所在 ——
// 它只有真的在高刷螢幕上跑才會露出來，而且看起來像「選了 144 卻卡」。
int RunGateTest() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    // WINDOWS 子系統的程式沒有 stdout，從測試腳本叫起來時 AttachConsole 常常
    // 直接失敗，所以同時寫一份 gate.log 到 exe 旁邊（和 selftest.log 同一個做法）。
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    logPath = logPath.substr(0, logPath.find_last_of(L'\\') + 1) + L"gate.log";
    FILE* lf = nullptr;
    _wfopen_s(&lf, logPath.c_str(), L"w");
    auto P2 = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        if (lf) {
            va_start(ap, fmt);
            vfprintf(lf, fmt, ap);
            va_end(ap);
            fflush(lf);
        }
    };

    const int srcs[] = {30, 60, 120, 144, 165, 239, 240, 360};
    const int targets[] = {30, 60, 120, 144, 165, 240};
    const int N = 6000;             // 模擬 6000 幀
    int bad = 0;
    P2("gatestest: 來源 -> 目標 = 實際（理想 = min(來源, 目標)）\n");
    for (int s : srcs) {
        for (int t : targets) {
            capwgc::FrameGate g;
            g.Init(t);
            const int64_t dtUs = 1000000 / s;
            int kept = 0;
            int64_t elapsed = 0;
            for (int i = 0; i < N; ++i) {
                if (i > 0) {
                    elapsed += dtUs;
                    if (g.Admit(dtUs)) ++kept;
                } else {
                    ++kept;          // 第一幀無條件收（與實際路徑一致）
                }
            }
            const double got = (double)kept * 1000000.0 / (double)(elapsed ? elapsed : 1);
            const double want = (double)(s < t ? s : t);
            // 允許 3% 誤差：整數微秒的時間戳本身就有捨入
            const bool ok = got <= want * 1.03 && got >= want * 0.97;
            P2("  %3d -> %3d : %6.1f fps  (want %6.1f)  %s\n", s, t, got, want,
               ok ? "ok" : "FAIL");
            if (!ok) ++bad;
        }
    }
    P2("gatestest: %s\n", bad == 0 ? "PASS" : "FAIL");
    if (lf) fclose(lf);
    return bad == 0 ? 0 : 1;
}

int RunSelfTest(int seconds, bool withAudio, bool withMic, bool splitTracks, int fps) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    logPath = logPath.substr(0, logPath.find_last_of(L'\\') + 1) + L"selftest.log";
    _wfopen_s(&g_selftestLog, logPath.c_str(), L"w");

    capwgc::RecorderConfig cfg;
    cfg.autoCodec = true;
    cfg.fps = fps > 0 ? fps : 60;
    cfg.quality = 70;
    cfg.monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (withAudio) cfg.audio.push_back({capwgc::AudioSourceKind::Loopback, std::string()});
    if (withMic) cfg.audio.push_back({capwgc::AudioSourceKind::Input, std::string()});
    // --split 要兩個源才有意義：一個源沒東西可分。分軌 = 混音軌之外，每個
    // 源再各留一條（tkhd 標成 disabled，只有剪輯軟體會去用）。
    if (splitTracks && cfg.audio.size() >= 2) cfg.separateTracks = true;

    wchar_t dir[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    std::wstring outDir = std::wstring(dir) + L"\\CapWGC";
    CreateDirectoryW(outDir.c_str(), nullptr);
    cfg.outPath = capwgc::MakeDefaultOutputPath(outDir);

    capwgc::Recorder rec;
    std::string err;
    if (!rec.Start(cfg, &err)) {
        P("selftest: Start failed: %s\n", err.c_str());
        if (g_selftestLog) fclose(g_selftestLog);
        return 1;
    }
    P("selftest: recording %d s -> %ls\n", seconds, cfg.outPath.c_str());

    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) {
        Sleep(200);
        const auto st = rec.stats();
        if (st.captureClosed) break;
        if (!st.error.empty()) break;
    }
    rec.Stop();

    const auto st = rec.stats();
    P("selftest: inited=%d frames=%llu dropped=%llu throttled=%llu\n",
      st.inited ? 1 : 0, (unsigned long long)st.frames,
      (unsigned long long)st.dropped, (unsigned long long)st.throttled);
    P("selftest: codec=%s encoder=%s %dx%d\n", st.codecName.c_str(),
      st.encoderName.c_str(), st.outW, st.outH);
    P("selftest: seconds=%.2f bytes=%llu audioAU=%llu audioTracks=%d\n", st.seconds,
      (unsigned long long)st.bytes, (unsigned long long)st.audioAU, st.audioTracks);
    if (!st.audioNote.empty()) P("selftest: audioNote=%s\n", st.audioNote.c_str());
    if (!st.error.empty()) P("selftest: error=%s\n", st.error.c_str());
    P("selftest: %s\n", (st.inited && st.frames > 0 && st.error.empty()) ? "PASS" : "FAIL");
    if (g_selftestLog) { fclose(g_selftestLog); g_selftestLog = nullptr; }
    return (st.inited && st.frames > 0 && st.error.empty()) ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // 先 DPI：所有螢幕座標都必須是實體像素，否則框選的矩形與 WGC item 座標
    // 會差一個縮放倍率，症狀是「錄到的區域比框出來的小一圈」。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ---- 命令列（用 W 版，中文路徑才不會亂碼）----
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selftest = false;
    bool hudtest = false;
    bool allowCapture = false;
    bool settingsTest = false;
    bool pickerTest = false;
    bool pickerStatic = false;
    bool hotkeyTest = false;
    int selftestSeconds = 3;
    bool selftestAudio = false;
    bool selftestSplit = false;
    bool selftestMic = false;
    bool gateTest = false;
    bool snipTest = false;
    bool snipOcrTest = false;
    bool prtscOn = false;
    bool prtscOff = false;
    int selftestFps = 0;      // 0 = 預設 60
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--selftest")) {
            selftest = true;
            if (i + 1 < argc) selftestSeconds = _wtoi(argv[i + 1]);
            if (selftestSeconds <= 0) selftestSeconds = 3;
        } else if (!_wcsicmp(argv[i], L"--hotkeytest")) {
            // QA 用：熱鍵字串的往返與衝突檢查（無 UI，會回傳 PASS/FAIL）
            hotkeyTest = true;
        } else if (!_wcsicmp(argv[i], L"--settings")) {
            settingsTest = true;
        } else if (!_wcsicmp(argv[i], L"--picker")) {
            // QA 用：直接開視窗清單（縮圖牆），截圖完送 Esc 就關
            pickerTest = true;
        } else if (!_wcsicmp(argv[i], L"--picker-static")) {
            // QA 用：開縮圖牆但關掉即時刷新。要可重現的畫面時用這個 ——
            // 即時刷新會在截圖中途改動清單，讓「幾張卡片」這種斷言不穩定。
            pickerTest = true;
            pickerStatic = true;
        } else if (!_wcsicmp(argv[i], L"--prtsc-on")) {
            // QA 用：無頭等價於「設定視窗勾了 PrtScn 接管並按確定」。
            //
            // 為什麼需要這個入口：接管的效果只能用「真的按 PrtScn、真的截到
            // 圖」來驗，而那需要一個已經在跑的程式。用滑鼠去設定視窗裡勾
            // 選項會把測試綁死在座標與時序上，而且一次只能驗一種狀態。
            prtscOn = true;
        } else if (!_wcsicmp(argv[i], L"--prtsc-off")) {
            prtscOff = true;
        } else if (!_wcsicmp(argv[i], L"--allow-capture")) {
            // QA 用：關掉 SetWindowDisplayAffinity，讓截圖看得到我們的 UI。
            allowCapture = true;
        } else if (!_wcsicmp(argv[i], L"--hudtest")) {
            hudtest = true;
            if (i + 1 < argc) selftestSeconds = _wtoi(argv[i + 1]);
            if (selftestSeconds <= 0) selftestSeconds = 5;
        } else if (!_wcsicmp(argv[i], L"--audio")) {
            selftestAudio = true;
        } else if (!_wcsicmp(argv[i], L"--mic")) {
            // QA 用：--selftest --mic → 只收麥克風；和 --audio 一起就是混音。
            selftestMic = true;
        } else if (!_wcsicmp(argv[i], L"--split")) {
            // QA 用：--selftest --audio --split → 系統聲 + 麥克風，混一條軌，
            // 另外再各留一條分軌（tkhd 標成 disabled）。
            selftestSplit = true;
        } else if (!_wcsicmp(argv[i], L"--gatestest")) {
            // QA 用：只驗幀率閘門的數學（不開擷取），把「來源 × 目標」全跑一遍。
            gateTest = true;
        } else if (!_wcsicmp(argv[i], L"--sniptest")) {
            // QA 用：只開截圖框選（含頂端工具列），用來單獨檢查工具列外觀與命中。
            snipTest = true;
            if (i + 1 < argc) selftestSeconds = _wtoi(argv[i + 1]);
            if (selftestSeconds <= 0) selftestSeconds = 15;
        } else if (!_wcsicmp(argv[i], L"--snipocr")) {
            // QA 用：跟托盤的「截圖後直接 OCR」走同一條路（BeginSnip(a, true)），
            // 但不經過托盤選單 —— 選單是 TrackPopupMenu 直取回傳值，沒辦法用
            // PostMessage 觸發。自動化測試要的是「框選 → OCR → 剪貼簿」這條，
            // 而不是選單本身。
            snipOcrTest = true;
        } else if (!_wcsicmp(argv[i], L"--fps")) {
            if (i + 1 < argc) selftestFps = _wtoi(argv[i + 1]);
        }
    }
    if (argv) LocalFree(argv);

    // 介面語言要在**任何 UI 之前**載入：托盤提示、選單、設定視窗都直接查表，
    // 而下面幾行就有 MessageBox（Media Foundation 失敗、單一實例）。
    app::i18n::LoadFromIni();
    // 留一行 log：使用者回報「介面變成英文／還是中文」時，先看這裡是
    // 「選了什麼語言」還是「語系檔沒帶到」（LoadNote 會寫出檔案路徑）。
    Log("i18n %s", U8(app::i18n::LoadNote()).c_str());

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    ui::SetCaptureExclusion(!allowCapture);

    if (settingsTest) {
        app::Settings st;
        st.Load();
        // 這裡刻意用一個 0×0 的 owner 視窗，而不是 nullptr：托盤程式真正的
        // 呼叫路徑就是這樣（a.msgwnd 是 0,0,0,0 的隱藏訊息窗）。之前傳
        // nullptr 讓「以 owner 置中」這條路完全沒被測到，結果 QA 截圖一切
        // 正常、使用者打開卻跑到螢幕外。
        HWND fakeOwner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                         0, 0, 0, 0, nullptr, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
        // 跟托盤那條路一樣把結果存回去 —— 存檔這一步（尤其是 [hotkeys] 的
        // 字串往返）如果不在 QA 路徑上跑一次，坏了也不會有人知道。
        if (app::ShowSettingsDialog(fakeOwner, &st)) st.Save();
        if (fakeOwner) DestroyWindow(fakeOwner);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (pickerTest) {
        app::Settings st;
        st.Load();
        const app::PickResult r = app::PickSource(nullptr, /*live=*/!pickerStatic);
        // picker.log 由 PickSource 自己寫；這裡把結果也補一行到 gui.log
        Log("--picker done ok=%d hwnd=%p mon=%p rect=%ld,%ld,%ld,%ld", r.ok ? 1 : 0,
            (void*)r.hwnd, (void*)r.monitor, r.rect.left, r.rect.top, r.rect.right,
            r.rect.bottom);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (hotkeyTest) {
        const int rc = RunHotkeyTest();
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (gateTest) {
        const int rc = RunGateTest();
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (snipTest) {
        const int rc = RunSnipTest(selftestSeconds);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (hudtest) {
        const int rc = RunHudTest(selftestSeconds);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (selftest) {
        const int rc = RunSelfTest(selftestSeconds, selftestAudio, selftestMic,
                                   selftestSplit, selftestFps);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return rc;
    }

    if (FAILED(mf)) {
        MessageBoxW(nullptr, app::i18n::T(L"Media Foundation 啟動失敗，無法錄影。"), L"CapWGC",
                    MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // 單一實例：兩個托盤圖示會讓人分不清哪個在錄。
    HANDLE once = CreateMutexW(nullptr, TRUE, L"CapWGC.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, app::i18n::T(L"CapWGC 已經在執行（請看系統匣）。"), L"CapWGC",
                    MB_ICONINFORMATION);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return 0;
    }

    App app;
    g_app = &app;
    app.settings.Load();
    // ini 沒有 [ui] lang（第一次執行、或從舊版升級）→ 把偵測到的語言寫回去。
    // 不寫回去的話，使用者下次換了系統語言，介面就會跟著跳 —— 那不是他改的。
    if (app.settings.lang.empty()) app.settings.lang = app::i18n::Current();
    app.iconIdle = ui::LoadTrayIcon(false, GetSystemMetrics(SM_CXSMICON));
    app.iconRec = ui::LoadTrayIcon(true, GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = ui::LoadAppIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = ui::LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // 不顯示的 owner 視窗：托盤訊息、熱鍵、timer 都靠它。用真視窗而不是
    // HWND_MESSAGE，因為托盤選單需要 SetForegroundWindow 才不會殘留。
    app.msgwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"CapWGC", WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    if (!app.msgwnd) return 1;

    // 錄製中的漂浮控件（唯一的 UI）。建立失敗只影響「用滑鼠停止」這條路，
    // 熱鍵仍然可用，所以不讓它變成致命錯誤，但一定要留痕跡。
    if (!app.bar.Create(
            [&app](app::ControlBar::Button b) { OnControlBar(app, b); },
            [&app](int dx, int dy) {
                // 使用者拖到哪裡就記住哪裡（相對擷取範圍右下角），下次錄影沿用。
                app.settings.hasBarPos = true;
                app.settings.barDx = dx;
                app.settings.barDy = dy;
                app.settings.Save();
                Log("bar moved dx=%d dy=%d", dx, dy);
            })) {
        Log("bar.Create FAILED");
    }

    // 提示窗。建立失敗也不要緊 —— TrayBalloon 會是無聲的，但功能本身
    // （截圖、OCR、錄影）完全不受影響，所以只留一行 log。
    if (!app.toast.Create()) Log("toast.Create FAILED (notifications will be silent)");

    // OCR 引擎（常駐 Python 程序）。啟動要 4~6 秒（載入模型），所以在背景
    // 起來，UI 不等它 —— 使用者可能只是想錄影，不會用到 OCR。
    //
    // 起不來不影響錄影與截圖：引擎不就緒時工具列的 OCR 分段畫成灰的。
    app.ocrStarting = true;
    if (!app.ocr.Start(
            app.msgwnd, WM_APP_OCR,
            [&app](const app::OcrResult& r) {
                app.ocrStarting = false;
                if (r.id != app.snipOcrId && app.snipOcrId != 0) return;  // 過期結果
                app.snipOcrId = 0;
                if (!r.ok) {
                    Log("ocr FAILED: %s", U8(r.error).c_str());
                    TrayBalloon(app, app::i18n::T(L"OCR 失敗"), r.error, NIIF_ERROR);
                    return;
                }
                Log("ocr ok lines=%zu ms=%d", r.lines.size(), r.ms);
                ShowOcrResult(app, r);
            },
            [&app](bool ok, const std::wstring& detail, const std::wstring& param) {
                app.ocrStarting = false;
                app.ocrAvailable = ok;
                Log("ocr ready=%d detail=%s", ok ? 1 : 0, U8(detail).c_str());
                if (ok) {
                    // 引擎起來的時候可能已經有一個框選還開著（使用者動作比
                    // 模型載入快）→ 把工具列的 OCR 打開。
                    app.overlay.SetOcrAvailable(true);
                    return;
                }

                // 分兩種失敗，處理方式不一樣：
                //
                //   1. 安裝不完整（models\ 沒帶、或 onnxruntime.dll 不在／版本
                //      不對）—— 這是「這個安裝少了東西」，不會因為使用者多開
                //      幾次就自己好。絕大多數人只錄影截圖，每次開機被同一個
                //      警告打擾比沒有 OCR 更煩，所以只講一次。
                //   2. 其他（模型損壞、推論失敗）—— 可能是環境問題，每次都要講，
                //      否則使用者只會看到 OCR 莫名變灰。
                // detail 是**原文**（ocr.cpp 刻意不翻譯它）：下面那幾個 find()
                // 靠原文分類「安裝不完整（只講一次）」與「執行期失敗（每次都講）」，
                // 翻過的句子不會包含翻過的片段。要顯示時才過 i18n。
                //
                // 含參數的句子（版本字串、載入錯誤）由 param 傳進來，這裡才代入
                // —— 值一旦併進 detail，就永遠查不到語系檔的 key，會顯示中文。
                const std::wstring dtext =
                    param.empty()
                        ? app::i18n::Ts(detail)
                        : app::i18n::Fmt(app::i18n::Ts(detail).c_str(), param.c_str());
                const bool notInstalled =
                    detail.find(L"找不到 models") != std::wstring::npos ||
                    detail.find(L"models 目錄不完整") != std::wstring::npos ||
                    detail.find(L"找不到 onnxruntime.dll") != std::wstring::npos ||
                    detail.find(L"推論引擎版本不相容") != std::wstring::npos;

                if (notInstalled && app.settings.ocrWarned) {
                    // 已經講過一次了。這裡刻意留一行 log：使用者回報「OCR 是灰的」
                    // 時，看 log 就知道是「早就告知過」而不是「這次才壞掉」。
                    Log("ocr warn suppressed (install incomplete, already told once)");
                    return;
                }

                if (notInstalled) {
                    app.settings.ocrWarned = true;
                    app.settings.Save();
                    TrayBalloon(app, app::i18n::T(L"OCR 未啟用（只提示這一次）"),
                                app::i18n::T(L"OCR 需要的檔案不在（models\\ 或 onnxruntime.dll），"
                                        L"工具列的 OCR 會保持灰色。\n"
                                        L"錄影、截圖、框選都不受影響，可以忽略這則訊息。\n"
                                        L"解壓縮時請把 models 目錄與 onnxruntime.dll 一起保留。"),
                                NIIF_INFO);
                } else {
                    TrayBalloon(app, app::i18n::T(L"OCR 未啟用"),
                                WFmt(app::i18n::T(L"%s\n截圖功能不受影響"), dtext.c_str()),
                                NIIF_WARNING);
                }
            },
            // 空字串 = 用 exe 旁邊的 models\。模型目錄不再開放設定（見
            // settings.cpp「OCR 模型」那段的說明），所以這裡固定傳空。
            std::wstring{})) {
        app.ocrAvailable = false;
        app.ocrStarting = false;
        Log("ocr.Start FAILED (see balloon)");
    }

    TrayAdd(app);
    // 熱鍵全部來自設定（settings.ini 的 [hotkeys]）。註冊失敗不能靜默：
    // 使用者只會覺得「按了沒反應」，然後以為程式壞了。
    //
    // 順序：先對帳（ini 說接管著但登錄值被改掉 → 放手）→ 再決定要不要自動
    // 接管（預設開啟）→ 最後才註冊熱鍵。反過來的話會先註冊一個「按了沒反應」
    // 的 PrtScn，接管成功之後才補上第二輪註冊。
    ReconcilePrtScn(app);
    AutoEngagePrtScn(app);
    const std::wstring badHotkey = RegisterAll(app.msgwnd, app.settings.hotkeys);
    SetTimer(app.msgwnd, TIMER_TICK, 250, nullptr);

    // 啟動提示：**只列真的按得下去的鍵**。
    //
    // 原本五組全列。現在預設有三組是空的（錄影類的熱鍵刻意不預設），全列
    // 會變成三行「（未設定）」——那不只沒用，還會讓使用者以為程式沒設定好。
    // 還在學這個程式的人要知道的是「現在按什麼有用」，不是「有哪些欄位存在」。
    const app::Hotkeys& hk = app.settings.hotkeys;
    std::wstring tip;
    auto Add = [&tip](const wchar_t* what, const app::Hotkey& k) {
        if (!k.set()) return;
        if (!tip.empty()) tip += L"\n";
        tip += WFmt(app::i18n::T(L"%s：%s"), what, app::HotkeyText(k).c_str());
    };
    Add(app::i18n::T(L"截圖／OCR"), hk.snip);
    Add(app::i18n::T(L"選擇視窗"), hk.picker);
    Add(app::i18n::T(L"選區域錄影"), hk.region);
    Add(app::i18n::T(L"錄主螢幕"), hk.screen);
    Add(app::i18n::T(L"停止"), hk.stop);
    if (tip.empty()) tip = app::i18n::T(L"目前沒有綁任何熱鍵，可從托盤或設定指定。");
    if (badHotkey.empty()) {
        TrayBalloon(app, app::i18n::T(L"CapWGC 已啟動"), tip, NIIF_INFO);
    } else {
        TrayBalloon(app, app::i18n::T(L"CapWGC 已啟動（有熱鍵被佔用）"),
                    tip + WFmt(app::i18n::T(L"\n\n「%s」被其他程式佔用，請到設定換一組"),
                               badHotkey.c_str()),
                    NIIF_WARNING);
    }

    // QA：等 OCR 引擎把模型載完再進框選，否則工具列的 OCR 還是灰的。
    if (snipOcrTest) {
        const ULONGLONG t0 = GetTickCount64();
        while (!app.ocrAvailable && GetTickCount64() - t0 < 15000) {
            MSG m2{};
            while (PeekMessageW(&m2, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m2);
                DispatchMessageW(&m2);
            }
            Sleep(50);
        }
        PostMessageW(app.msgwnd, WM_APP_SNIP_OCR_TEST, 0, 0);
    }

    // QA：PrtScn 接管 / 還原的無頭入口。跑完就結束行程 —— 這兩個模式是
    // 「做一件事然後退出」，不是常駐狀態；把它們做成常駐會讓下一步的測試
    // 撞上單一實例檢查。
    if (prtscOn || prtscOff) {
        std::wstring why;
        const bool ok = prtscOn ? EngagePrtScn(app, &why) : ReleasePrtScn(app, &why);
        Log("prtsc: %s -> %d (%ls)", prtscOn ? "on" : "off", ok ? 1 : 0,
            why.empty() ? L"ok" : why.c_str());
        // 一次性的 QA 工具不該留下托盤圖示與常駐行程。
        TrayRemove(app);
        app.ocr.Stop();
        UnregisterAll(app.msgwnd);
        if (app.iconIdle) DestroyIcon(app.iconIdle);
        if (app.iconRec) DestroyIcon(app.iconRec);
        if (SUCCEEDED(mf)) MFShutdown();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app.ocr.Stop();
    app.overlay.Cancel();
    app.snipFull.Free();
    UnregisterAll(app.msgwnd);
    if (app.iconIdle) DestroyIcon(app.iconIdle);
    if (app.iconRec) DestroyIcon(app.iconRec);
    if (once) CloseHandle(once);
    if (SUCCEEDED(mf)) MFShutdown();
    CoUninitialize();
    g_app = nullptr;
    return 0;
}
