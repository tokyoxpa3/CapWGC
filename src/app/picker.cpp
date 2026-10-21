// ============================================================================
// picker.cpp — Discord 式「選來源」縮圖牆的實作
// ============================================================================
//
// 兩個刻意的順序決定：
//
// 1. **先抓縮圖，再顯示自己**。縮圖是靜態截取（PrintWindow），如果先把選擇器
//    畫到螢幕上再抓，自己就擋在目標前面，卡片裡會出現自己的縮圖 —— 這是
//    「看起來像 bug、其實是順序錯」的典型。
// 2. **縮圖在枚舉時就縮到固定尺寸**（480×270），畫的時候只做一次
//    StretchBlt。反過來（留全尺寸、每次重畫才縮）會在每次滑鼠移動時重算
//    24 張 1080p 的縮放，畫面會明顯鈍。
//
// 沒有用 DWM 即時縮圖（DwmRegisterThumbnail）：那個方案不能保證縮圖落在
// 卡片邊框下面（DWM 自己合成），而且離開選擇器就消失，除錯時完全看不到
// 中間產物。靜態縮圖足以「認出是哪個視窗」，也讓 QA 截圖看得到內容。
//
// 3. **即時更新走「原地刷新」，不是「重建清單」**。每 500ms 重新 EnumWindows，
//    但用 hwnd 當身分去對齊既有項目：既有的留在原位（保留縮圖與選取），
//    新的插到尾巴，消失的移除。若直接重建整個 vector，正在看第一頁的使用者
//    會被「東西跳來跳去」干擾，選取也會亂掉 —— 這是即時更新最常見的失敗。
// 4. **縮圖輪替預算**：一次刷新最多重抓 2 張（新出現的優先），所以就算開著
//    三十個視窗也不會每 100ms 卡一次。抓圖本身是同步的，不能一次做完。
// 5. **監視器的縮圖不即時**。整個螢幕的縮圖只能用螢幕擷取（BitBlt）拿，而
//    選擇器自己就蓋在螢幕正中央 —— 即時重抓會把自己的縮圖拍進去。視窗縮圖
//    用 PrintWindow，不受遮擋影響，所以可以安全地即時更新。
// ============================================================================

#include "picker.h"
#include "i18n.h"
#include "ui.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace app {
namespace {

constexpr int kThumbW = 480;   // 抓下來的縮圖尺寸（畫的時候再縮到卡片大小）
constexpr int kThumbH = 270;
constexpr int kCols = 3;
// 上限只是防爆（每個視窗要抓一次縮圖），訂得比一般桌面會開的視窗數高。
// 被截斷時 picker.log 會有一行，不要靜默丟掉。
constexpr int kMaxWindows = 40;

// 即時更新的兩個節奏。分成兩條是有原因的：
//   kTimerRefresh：重掃「有哪些視窗」。EnumWindows 一趟 10~30ms，沒必要更勤。
//   kTimerThumbs ：補抓縮圖。PrintWindow 一張可能 5~30ms，所以一次只抓一兩張。
// 合在一起做會變成「每 100ms 掃一次清單 + 抓 N 張圖」，視窗一多就卡。
constexpr UINT_PTR kTimerRefresh = 1;
constexpr UINT_PTR kTimerThumbs = 2;
constexpr UINT kRefreshMs = 400;
constexpr UINT kThumbMs = 120;
constexpr int kThumbBatch = 2;   // 每次最多補幾張
constexpr int kThumbRetries = 6; // 過渡性失敗最多重試幾輪，之後就認了（留佔位圖）

// Discord 深色配色。刻意不用系統色：這個視窗是「選來源」的舞台，
// 深底讓縮圖自己發亮，淺底會讓縮圖看起來像貼紙。
const COLORREF kBg = RGB(30, 31, 34);        // #1e1f22
const COLORREF kCard = RGB(43, 45, 49);      // #2b2d31
const COLORREF kCardHi = RGB(56, 58, 64);    // 懸停 / 選取
const COLORREF kLine = RGB(63, 65, 71);      // #3f4147
const COLORREF kText = RGB(242, 243, 245);
const COLORREF kSub = RGB(181, 186, 193);
const COLORREF kAccent = RGB(88, 101, 242);  // #5865f2
const COLORREF kAccentHi = RGB(110, 122, 250);

// ---- 日誌 ------------------------------------------------------------------
//
// 這條路徑的失敗幾乎都是無聲的（縮圖全黑、視窗清單是空的、PrintWindow 被
// 驅動擋掉）。每次開啟都重寫一份 picker.log，一行一個來源。
void LogPath(wchar_t* out, size_t n) {
    wchar_t dir[MAX_PATH] = {};
    out[0] = 0;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring d = std::wstring(dir) + L"\\CapWGC";
    CreateDirectoryW(d.c_str(), nullptr);
    wcscpy_s(out, n, (d + L"\\picker.log").c_str());
}

void LogW(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    LogPath(path, MAX_PATH);
    if (!path[0]) return;
    FILE* f = _wfopen(path, L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

void LogReset() {
    wchar_t path[MAX_PATH] = {};
    LogPath(path, MAX_PATH);
    if (path[0]) DeleteFileW(path);
}

// ---- 小工具 ----------------------------------------------------------------

HBITMAP NewDib(HDC ref, int w, int h, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 負 = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// PrintWindow 對某些視窗「成功但全黑」（GPU 直接合成、DRM、部分 UWP）。
// 這個檢查比相信回傳值可靠：取樣 4000 點，有亮度的不到 1% 就當作沒抓到。
bool MostlyBlack(const void* bits, int w, int h) {
    const auto* p = static_cast<const uint32_t*>(bits);
    const size_t n = (size_t)w * (size_t)h;
    const size_t step = n / 4000 + 1;
    size_t lit = 0, seen = 0;
    for (size_t i = 0; i < n; i += step) {
        ++seen;
        if ((p[i] & 0x00FFFFFFu) > 0x0A0A0Au) ++lit;
    }
    return seen > 0 && lit * 100 < seen;
}

std::wstring ExeNameOf(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return out;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        std::wstring s = buf;
        const size_t pos = s.find_last_of(L'\\');
        out = (pos == std::wstring::npos) ? s : s.substr(pos + 1);
    }
    CloseHandle(p);
    return out;
}

// UWP（ApplicationFrameWindow）的標題掛在子視窗上，GetWindowTextW 對它回空字串。
// 找不到就讓呼叫端退回 exe 名 —— 重點是「視窗要出現在清單裡」。
struct ChildTitleCtx {
    wchar_t* out;
    int n;
};

BOOL CALLBACK ChildTitleProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<ChildTitleCtx*>(lp);
    if (GetWindowTextW(h, c->out, c->n) > 0) return FALSE;
    return TRUE;
}

void ChildTitle(HWND h, wchar_t* out, int n) {
    out[0] = 0;
    ChildTitleCtx c{out, n};
    EnumChildWindows(h, ChildTitleProc, (LPARAM)&c);
}

// 清單被上限截斷時只記一次 —— 靜默截斷正是「怎麼少了幾個」的來源。
bool g_winCapHit = false;

// 拍不到畫面的視窗（最小化、被驅動擋掉、PrintWindow 給黑圖）至少給個圖示，
// 使用者才認得出是哪個程式。一律回傳自己擁有的 handle，之後才能安全釋放。
HICON IconOfHwnd(HWND h) {
    if (!h) return nullptr;
    const UINT flags = SMTO_ABORTIFHUNG | SMTO_BLOCK;
    for (UINT kind : {UINT(ICON_BIG), UINT(ICON_SMALL2), UINT(ICON_SMALL)}) {
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(h, WM_GETICON, kind, 0, flags, 120, &r) && r)
            return CopyIcon((HICON)r);
    }
    if (HICON c = (HICON)GetClassLongPtrW(h, GCLP_HICON)) return CopyIcon(c);
    if (HICON c = (HICON)GetClassLongPtrW(h, GCLP_HICONSM)) return CopyIcon(c);
    return nullptr;
}

struct ChildIconCtx {
    HICON out = nullptr;
    int tries = 0;   // 子視窗可能幾十個，每個都問三次會拖慢列舉
};
BOOL CALLBACK ChildIconProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<ChildIconCtx*>(lp);
    if (++c->tries > 12) return FALSE;
    c->out = IconOfHwnd(h);
    return c->out ? FALSE : TRUE;
}

// UWP 的 ApplicationFrameWindow 自己沒有圖示（實測「小算盤」拿到 null），圖示
// 在子視窗上；再不然就退回「這個 process 的 exe 圖示」——起碼認得出程式。
HICON WindowIcon(HWND h) {
    if (!h) return nullptr;
    if (HICON ic = IconOfHwnd(h)) return ic;

    ChildIconCtx c;
    EnumChildWindows(h, ChildIconProc, (LPARAM)&c);
    if (c.out) return c.out;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return nullptr;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return nullptr;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    if (!ok) return nullptr;

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
        return sfi.hIcon;
    return nullptr;
}

struct Item {
    HWND hwnd = nullptr;
    HMONITOR mon = nullptr;
    // 「這一項不是視窗也不是螢幕，而是去框選」。三者互斥，用同一個 struct
    // 承載是因為卡片繪製、選取、版面全都是同一套邏輯。
    bool region = false;
    RECT rect{};              // 螢幕座標（最小化的視窗用「還原後」的矩形）
    std::wstring title, sub;
    bool primary = false;
    bool minimized = false;   // 選它時要先 SW_RESTORE
    HBITMAP thumb = nullptr;
    bool thumbOk = false;
    // 抓這張縮圖時的來源矩形尺寸。即時刷新時拿來比對：視窗改變大小（或從
    // 最小化還原）→ 舊縮圖已經不對，要重抓。光看 rect 不夠，因為還原後的
    // 視窗常常和 rcNormalPosition 一樣大。
    int thumbSrcW = 0, thumbSrcH = 0;
    bool thumbWasMinimized = false;
    // 這一輪要重抓縮圖（新出現的視窗、改變大小的、剛還原的）
    bool needThumb = false;
    int thumbTries = 0;       // 過渡性失敗（視窗正在重畫）重試幾輪
    HICON icon = nullptr;     // 拍不到畫面時的替代圖（自己 CopyIcon 來的，要 DestroyIcon）
    // 版面（client 座標，不含捲動偏移）
    RECT card{}, thumbRect{}, titleRect{}, subRect{};
};

class Picker {
public:
    Picker(bool live, SysMsgFn onSysMsg)
        : live_(live), onSysMsg_(std::move(onSysMsg)) {}
    PickResult Run();

private:
    // ---- 內容 ----
    void Enumerate();
    void CaptureAll();
    // live=true 時不做螢幕擷取回退（選擇器自己會擋住目標，抄下來就是自己）。
    void CaptureOne(Item& it, bool live);
    void ReleaseThumbs();

    // ---- 即時更新 ----
    // Refresh：重掃清單，用 hwnd 對齊既有項目（不重排、保留選取與縮圖）。
    // PumpThumbs：一次抓一小批縮圖，避免某一輪卡太久。
    void Refresh();
    void PumpThumbs(int budget);
    // 列舉單一視窗 → Item；回傳 false 表示這個視窗不該收（過濾規則都在這裡）。
    // static：純規則、不碰成員狀態，這樣 static 的 EnumWinProc 也能直接呼叫。
    static bool MakeWinItem(HWND h, Item& out);

    // ---- 視窗 ----
    bool CreateAndShow();
    void CreateResources();
    void ReleaseResources();
    void Layout();
    void UpdateScroll();
    void Paint();
    void DrawTabs(HDC dc);
    void DrawGrid(HDC dc);
    void DrawCard(HDC dc, int idx, bool selected, bool hover);
    void DrawButton(HDC dc, const RECT& r, const wchar_t* text, bool primary, bool hover);
    void DrawFooter(HDC dc);
    // 自繪捲軸：見 Layout 的說明，不用 WS_VSCROLL。
    void DrawScrollBar(HDC dc);
    bool HitScrollBar(POINT pt) const;
    int PageHeight() const;
    void DragThumbTo(int y);

    // ---- 互動 ----
    void SetTab(int t);
    // 「區域」分頁那唯一一張卡片。內容固定，所以只需要建一次。
    void BuildRegionCard();
    void MoveSel(int delta);
    void EnsureVisible();
    void ScrollBy(int dy);
    void Confirm();
    // 關掉自己、把「使用者要設定」交給呼叫端。不在這裡直接開設定視窗是刻意的：
    // 設定是**另一個 modal**，它需要一個正常的 owner；而且 picker 的訊息迴圈
    // 還在自己身上跑，兩個 modal 疊在一起之後誰該關誰會變得很難講清楚。
    void AskSettings();
    void Cancel();
    int HitCard(POINT pt) const;
    int HitTab(POINT pt) const;
    int HitButton(POINT pt) const;

    std::vector<Item>& cur() { return tab_ == 0 ? wins_ : (tab_ == 1 ? mons_ : region_); }
    const std::vector<Item>& cur() const {
        return tab_ == 0 ? wins_ : (tab_ == 1 ? mons_ : region_);
    }
    int S(int v) const { return MulDiv(v, dpi_, 96); }
    int ContentHeight() const;

    LRESULT Handle(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static BOOL CALLBACK EnumWinProc(HWND, LPARAM);
    static BOOL CALLBACK EnumMonProc(HMONITOR, HDC, LPRECT, LPARAM);

    HWND hwnd_ = nullptr;
    int dpi_ = 96;

    bool live_ = true;
    // 這一輪 Refresh 有沒有改變清單（決定要不要重畫）。縮圖補完也算改變。
    bool dirty_ = false;
    bool initialDone_ = false;   // 首次抓完縮圖前不啟動輪替

    std::vector<Item> wins_, mons_, region_;
    // 三分頁：視窗 / 螢幕 / 區域。
    //
    // 為什麼「區域」要放在這裡而不是只留熱鍵：使用者要錄影時心裡想的是
    // 「我要錄這個東西」，而「這個東西」可能是視窗、可能是整個螢幕、也可能
    // 只是畫面上的一塊。三者是同一個問題的三種答案，攤在同一個視窗裡選，
    // 比叫使用者「記住三個熱鍵、按對那一個」合理得多。熱鍵仍然在（想直接
    // 跳過這個視窗的人用），但不再是唯一入口。
    //
    // region_ 不是枚舉來的，永遠只有一張卡（見 BuildRegionCard）。用 vector
    // 只是為了讓 cur() / 選取 / 版面這幾段程式不用為它開特例。
    int tab_ = 0;
    int sel_[3] = {0, 0, 0};
    int hover_ = -1;
    int hoverTab_ = -1;
    int hoverBtn_ = 0;
    bool tracking_ = false;
    // 拖曳 / 滾輪期間暫停刷新：一直重畫會讓拖曳頓、捲軸跳。
    int suspend_ = 0;

    int scroll_ = 0;
    int maxScroll_ = 0;
    // 自繪捲軸的幾何（見 DrawScrollBar）。sbThumb_ 由 UpdateScroll 算，
    // 其他兩個在 Layout 算。
    RECT sbTrack_{}, sbThumb_{};
    bool sbDrag_ = false;    // 正在拖拇指
    int sbGrab_ = 0;         // 按下點落在拇指內的偏移，拖動時要保持住
    bool sbHover_ = false;
    int cardW_ = 240, cardH_ = 175, gap_ = 16, gridTop_ = 96;
    RECT viewport_{}, tabRect_[3]{}, btnOk_{}, btnCancel_{}, btnSetup_{}, footer_{};

    HFONT fontTitle_ = nullptr, fontItem_ = nullptr, fontSub_ = nullptr, fontBtn_ = nullptr;
    HBRUSH brBg_ = nullptr, brCard_ = nullptr, brCardHi_ = nullptr, brAccent_ = nullptr,
           brAccentHi_ = nullptr, brLine_ = nullptr;

    PickResult result_;

    // 開著的時候要攔的系統訊息（全域熱鍵）。空 = 不攔。
    SysMsgFn onSysMsg_;
    // 攔到的訊息。**不能**在訊息迴圈裡直接處理：處理的動作是「開截圖覆蓋層
    // 或開始錄影」，那會建立新的視窗並進入另一個 modal 迴圈，而我們自己的
    // 視窗還活著、縮圖還握在手裡 —— 疊在一起之後誰該關誰會變得很難講清楚。
    // 所以先關掉自己、離開迴圈，再把訊息交出去（呼叫端在 Run() 返回之後處理）。
    bool sysMsgHit_ = false;
    MSG sysMsg_{};
};

// ============================================================================
// 內容：枚舉與縮圖
// ============================================================================

// 過濾規則集中在 MakeWinItem，初次列舉與即時刷新才不會各寫一份而走鐘。
BOOL CALLBACK Picker::EnumWinProc(HWND h, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<Item>*>(lp);
    if ((int)v->size() >= kMaxWindows) {
        if (!g_winCapHit) {
            g_winCapHit = true;
            LogW(L"[pick] window list truncated at %d (more windows exist)", kMaxWindows);
        }
        return FALSE;
    }

    Item it;
    if (!MakeWinItem(h, it)) return TRUE;
    v->push_back(std::move(it));
    return TRUE;
}

// 把一個 top-level hwnd 變成 Item。所有「該不該收這個視窗」的判斷都在這裡 ——
// 以前散在回呼裡，即時刷新要再用一次時就得整段複製，兩份一定會漂移。
bool Picker::MakeWinItem(HWND h, Item& out) {
    if (!IsWindowVisible(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;

    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (ex & WS_EX_NOACTIVATE) return false;   // 自己的遮罩 / 控件 / 外框

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;

    // 被 DWM 遮蔽的視窗（其他虛擬桌面、UWP 的背景殼）截出來是全黑，而且
    // 它們本來就不在眼前 —— 這種才是真的不該收。
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked) {
        return false;
    }

    // 最小化的視窗要收。以前跳過它，結果是「我明明開著，清單裡卻沒有」——
    // 使用者的感受就是「不齊全」。它的 GetWindowRect 是圖示大小（實測
    // 160×28），尺寸與縮圖都要用**還原後**的矩形，所以這裡讀 GetWindowPlacement。
    const bool iconic = IsIconic(h) != FALSE;
    RECT r{};
    if (iconic) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(h, &wp)) return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(h, &r)) {
        return false;
    }

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    static const wchar_t* kSkip[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd",
                                     L"Shell_SecondaryTrayWnd", L"IME", L"MSCTFIME UI",
                                     L"Windows.UI.Core.CoreWindow"};
    for (const wchar_t* s : kSkip) {
        if (!wcscmp(cls, s)) return false;
    }

    if (r.right - r.left < 120 || r.bottom - r.top < 80) return false;

    wchar_t title[512] = {};
    GetWindowTextW(h, title, 512);
    if (!title[0]) {
        // UWP 的 ApplicationFrameWindow 標題在子視窗上（實測「設定」「小畫家」
        // 都是這樣）。以前直接跳過，等於整個 App 從清單消失。
        ChildTitle(h, title, 512);
    }

    out.hwnd = h;
    out.rect = r;
    out.minimized = iconic;
    out.title = title;
    out.sub = ExeNameOf(pid);
    if (out.sub.empty()) out.sub = cls;
    if (iconic) out.sub += i18n::T(L"　最小化");
    if (out.title.empty()) out.title = out.sub;   // 連子視窗都沒有標題：至少給個名字
    return true;
}

BOOL CALLBACK Picker::EnumMonProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<Item>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) return TRUE;

    Item it;
    it.mon = m;
    it.rect = mi.rcMonitor;
    it.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    wchar_t title[64];
    swprintf_s(title, i18n::T(L"螢幕 %d"), (int)v->size() + 1);
    it.title = title;

    wchar_t sub[128];
    swprintf_s(sub, L"%d × %d%s", mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top,
                   it.primary ? i18n::T(L"　主螢幕") : L"");
    it.sub = sub;

    v->push_back(std::move(it));
    return TRUE;
}

void Picker::Enumerate() {
    wins_.clear();
    mons_.clear();
    g_winCapHit = false;
    EnumWindows(EnumWinProc, (LPARAM)&wins_);   // z-order：前景的在前
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&mons_);
    // 區域分頁不是枚舉來的（見 BuildRegionCard）；它永遠在，所以不需要任何
    // 「找不到」的退路 —— 這也是把它做成「固定一張卡」而不是「條件式的按鈕」
    // 的另一個好處。
    BuildRegionCard();

    // 最小化的視窗排在最後：它們只有「無法預覽」佔位圖，擺在前面會把整個首頁
    // 塞滿灰塊。兩組內部都保持原本的 z-order（穩定的話前景仍在前面）。
    std::stable_sort(wins_.begin(), wins_.end(),
                     [](const Item& a, const Item& b) { return !a.minimized && b.minimized; });

    // 主螢幕排第一，其餘按左上到右下 —— 跟 Windows 的顯示設定一致
    std::stable_sort(mons_.begin(), mons_.end(), [](const Item& a, const Item& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.top != b.rect.top) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
}

void Picker::CaptureOne(Item& it, bool live) {
    const RECT r = it.rect;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP full = NewDib(screen, w, h, &bits);
    HGDIOBJ oldFull = nullptr;
    if (full && bits) {
        oldFull = SelectObject(mem, full);
        RECT all{0, 0, w, h};
        FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    } else {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }

    bool got = false;
    const char* how = "none";
    if (it.hwnd) {
        if (it.minimized) {
            // 最小化視窗的 PrintWindow 只會給黑圖（實測），而 BitBlt 會抄到
            // 螢幕上那個座標的東西（完全不相干）。直接留 placeholder。
            how = "minimized";
        } else if (IsHungAppWindow(it.hwnd)) {
            how = "hung";   // 卡住的視窗不要碰：PrintWindow 會一起卡住
        } else {
            got = PrintWindow(it.hwnd, mem, PW_RENDERFULLCONTENT) != FALSE;
            how = got ? "printwindow" : "printwindow-fail";
            if (!got) {
                got = PrintWindow(it.hwnd, mem, 0) != FALSE;
                if (got) how = "printwindow0";
            }
        }
        if (got && MostlyBlack(bits, w, h)) {
            got = false;
            how = "black";
        }
        if (!got && !it.minimized && it.hwnd == GetForegroundWindow()) {
            if (live) {
                // 即時刷新時**不能**從螢幕抄：選擇器自己是 topmost，正蓋在
                // 目標上面，抄下來的是選擇器自己那一塊（實測就是這個症狀：
                // 卡片裡出現縮圖牆自己）。這一輪就留空，下一輪再試 PrintWindow。
                how = "live-skip";
            } else {
                // 首次快照時選擇器還沒顯示，螢幕上就是真的那個視窗，可以抄。
                HDC sdc = GetDC(nullptr);
                got = BitBlt(mem, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
                ReleaseDC(nullptr, sdc);
                if (got) how = "screen";
            }
        }
    } else if (!live) {
        // 螢幕（monitor）項目的縮圖只能靠螢幕擷取，而即時刷新時選擇器正蓋在
        // 螢幕中央 —— 所以監視器縮圖只抓一次（見檔頭第 5 點）。
        HDC sdc = GetDC(nullptr);
        got = BitBlt(mem, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
        ReleaseDC(nullptr, sdc);
        how = got ? "screen" : "screen-fail";
    } else {
        how = "live-skip";
    }

    // 畫面拿不到就退回圖示（最小化的視窗就是走這條）
    if (!got && it.hwnd && !it.icon) it.icon = WindowIcon(it.hwnd);

    HDC tmem = CreateCompatibleDC(screen);
    void* tbits = nullptr;
    HBITMAP thumb = NewDib(screen, kThumbW, kThumbH, &tbits);
    if (thumb && tbits) {
        HGDIOBJ told = SelectObject(tmem, thumb);
        RECT tr{0, 0, kThumbW, kThumbH};
        FillRect(tmem, &tr, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (got) {
            const double s = std::min((double)kThumbW / w, (double)kThumbH / h);
            const int dw = std::max(1, (int)(w * s));
            const int dh = std::max(1, (int)(h * s));
            SetStretchBltMode(tmem, HALFTONE);
            SetBrushOrgEx(tmem, 0, 0, nullptr);
            StretchBlt(tmem, (kThumbW - dw) / 2, (kThumbH - dh) / 2, dw, dh, mem, 0, 0, w, h,
                       SRCCOPY);
        }
        SelectObject(tmem, told);
        // 換掉舊的點陣圖。即時刷新會反覆呼叫，不釋放會漏 GDI 物件。
        if (it.thumb) DeleteObject(it.thumb);
        it.thumb = thumb;
        it.thumbOk = got;
    }
    DeleteDC(tmem);

    // 記下「這張縮圖是根據什麼尺寸、什麼狀態拍的」，即時刷新才能判斷它還準不準。
    it.thumbSrcW = w;
    it.thumbSrcH = h;
    it.thumbWasMinimized = it.minimized;
    it.needThumb = false;

    if (oldFull) SelectObject(mem, oldFull);
    DeleteObject(full);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    LogW(L"[pick] item hwnd=%p mon=%p how=%hs thumb=%s %dx%d title=%ls",
         (void*)it.hwnd, (void*)it.mon, how, got ? L"ok" : L"placeholder", w, h,
         it.title.c_str());
}

void Picker::CaptureAll() {
    const ULONGLONG t0 = GetTickCount64();
    for (auto& it : wins_) CaptureOne(it, /*live=*/false);
    for (auto& it : mons_) CaptureOne(it, /*live=*/false);
    LogW(L"[pick] capture %zu windows + %zu monitors in %llu ms", wins_.size(), mons_.size(),
         (unsigned long long)(GetTickCount64() - t0));
}

void Picker::ReleaseThumbs() {
    for (auto& it : wins_) {
        if (it.thumb) DeleteObject(it.thumb);
        it.thumb = nullptr;
        if (it.icon) DestroyIcon(it.icon);
        it.icon = nullptr;
    }
    for (auto& it : mons_) {
        if (it.thumb) DeleteObject(it.thumb);
        it.thumb = nullptr;
        if (it.icon) DestroyIcon(it.icon);
        it.icon = nullptr;
    }
}

// ============================================================================
// 即時更新
// ============================================================================

// 重掃視窗清單，但**不重建** vector —— 用 hwnd 當身分，既有的留在原位。
//
// 為什麼不能重建：使用者可能正在看第二頁、或已經把游標移到某張卡片上。整份
// 重建會讓所有卡片跳位、選取跑掉，體感就是「東西一直亂跳」。所以：
//   - 既有的（hwnd 還在）：原地更新 rect / minimized，縮圖留著不重抓
//   - 新的：插到尾巴（不插前面，免得把使用者正在看的擠走），標記 needThumb
//   - 消失的：移除
// 只有「尺寸變了」或「最小化狀態變了」才丟掉縮圖重抓 —— 不然每個 400ms 都
// 重抓一次 40 張，等於一直在做白工。
void Picker::Refresh() {
    std::vector<Item> fresh;
    EnumWindows(EnumWinProc, (LPARAM)&fresh);

    bool changed = false;
    std::vector<bool> stillHere(fresh.size(), false);

    // 既有項目原地對齊
    for (auto it = wins_.begin(); it != wins_.end();) {
        int match = -1;
        for (size_t j = 0; j < fresh.size(); ++j) {
            if (!stillHere[j] && fresh[j].hwnd == it->hwnd) { match = (int)j; break; }
        }
        if (match < 0) {
            // 視窗關了（或變得不符合過濾規則）
            LogW(L"[pick] refresh: - removed hwnd=%p title=%ls", (void*)it->hwnd,
                 it->title.c_str());
            if (it->thumb) DeleteObject(it->thumb);
            if (it->icon) DestroyIcon(it->icon);
            it = wins_.erase(it);
            changed = true;
            continue;
        }
        stillHere[match] = true;
        const Item& f = fresh[match];
        const int fw = f.rect.right - f.rect.left;
        const int fh = f.rect.bottom - f.rect.top;
        const bool geomChanged = (f.minimized != it->minimized) ||
                                 (fw != it->thumbSrcW) || (fh != it->thumbSrcH);
        if (f.title != it->title || f.sub != it->sub || geomChanged) changed = true;
        it->rect = f.rect;
        it->title = f.title;
        it->sub = f.sub;
        it->primary = f.primary;
        it->minimized = f.minimized;
        if (geomChanged) {
            // 尺寸 / 最小化狀態變了就作廢舊縮圖。不立刻抓，交給 PumpThumbs 分批。
            it->thumbOk = false;
            it->needThumb = true;
            it->thumbTries = 0;
        }
        ++it;
    }

    // 追加新出現的
    for (size_t j = 0; j < fresh.size(); ++j) {
        if (stillHere[j]) continue;
        Item& f = fresh[j];
        f.needThumb = true;
        f.thumbTries = 0;
        LogW(L"[pick] refresh: + added hwnd=%p title=%ls", (void*)f.hwnd, f.title.c_str());
        wins_.push_back(std::move(f));
        changed = true;
    }

    // 監視器的清單也會變（插拔螢幕 / 改解析度），但縮圖不即時重抓：只能靠
    // 螢幕擷取，而選擇器正蓋在螢幕上。所以這裡只在數量 / 尺寸變時更新文字。
    std::vector<Item> freshMons;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&freshMons);
    if (freshMons.size() != mons_.size()) {
        changed = true;
        mons_ = std::move(freshMons);
    } else {
        for (size_t i = 0; i < mons_.size(); ++i) {
            if (mons_[i].title != freshMons[i].title || mons_[i].sub != freshMons[i].sub) {
                mons_[i].title = freshMons[i].title;
                mons_[i].sub = freshMons[i].sub;
                changed = true;
            }
        }
    }

    if (changed) {
        // 選取要留在合法範圍內。清單從有變無時 sel_ 會指到外面。
        for (int t = 0; t < 2; ++t) {
            const int n = (int)(t == 0 ? wins_.size() : mons_.size());
            if (n == 0) sel_[t] = -1;
            else if (sel_[t] < 0) sel_[t] = 0;
            else if (sel_[t] >= n) sel_[t] = n - 1;
        }
        Layout();
        dirty_ = true;

        // hover_ 是**索引**，清單一刪一增就可能指到別的卡片上。不重算的話
        // 症狀是「滑鼠在 A 上，B 卻亮著」—— 只有即時刷新才會出現。用游標
        // 現在的位置重新命中測試一次。
        if (hwnd_) {
            POINT pt{};
            if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                hover_ = PtInRect(&rc, pt) ? HitCard(pt) : -1;
            } else {
                hover_ = -1;
            }
        }
    }
    // 這一行的用意是讓「清單真的跟著變了」可被斷言：QA 不必靠截圖比對卡片數。
    // 只有在真的變動時才寫，免得每 400ms 洗一行。
    if (changed) {
        LogW(L"[pick] refresh: list changed windows=%zu monitors=%zu", wins_.size(),
             mons_.size());
    }
}

// 一次抓一小批縮圖。優先序：新出現的視窗（needThumb 且還沒有圖）先抓，因為
// 使用者最想看的是「剛開的那個」。抓過的就會把 needThumb 清掉。
void Picker::PumpThumbs(int budget) {
    int done = 0;
    for (auto& it : wins_) {
        if (done >= budget) break;
        if (!it.needThumb) continue;
        if (it.thumbTries >= kThumbRetries) {
            // 試夠了還是不行（例如視窗正在切換、或被驅動擋著）。放棄重試，
            // 留住現有的佔位圖，免得無限重抓把 CPU 吃滿。
            it.needThumb = false;
            continue;
        }
        CaptureOne(it, /*live=*/true);
        ++it.thumbTries;
        // 抓成功（有圖或已判定為最小化）就收工；沒有就下一輪再試。
        if (it.thumbOk || it.minimized) it.needThumb = false;
        ++done;
        dirty_ = true;
    }
    if (done > 0) {
        Layout();
        LogW(L"[pick] refresh: pumped %d thumbnail(s)", done);
    }
}

// ============================================================================
// 視窗
// ============================================================================

bool Picker::CreateAndShow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;   // 全部自己畫（避免開窗時閃一下白）
    wc.hIcon = ui::LoadAppIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = ui::LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = L"CapWGCPicker";
    RegisterClassExW(&wc);

    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    if (IsRectEmpty(&work)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    const int dpi = GetDpiForSystem();
    const int w = MulDiv(820, dpi, 96);
    const int h = MulDiv(700, dpi, 96);
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    if (w <= (work.right - work.left)) {
        if (x + w > work.right) x = work.right - w;
        if (x < work.left) x = work.left;
    } else {
        x = work.left;
    }
    if (h <= (work.bottom - work.top)) {
        if (y + h > work.bottom) y = work.bottom - h;
    }
    if (y < work.top) y = work.top;

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CapWGCPicker",
                            i18n::T(L"CapWGC 選擇來源"), WS_POPUP, x, y, w, h, nullptr,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ <= 0) dpi_ = 96;
    CreateResources();
    Layout();

    // 刻意不設 SetWindowDisplayAffinity：這個視窗永遠不會被錄進成品（錄影在
    // 它關掉之後才開始），而設了之後 QA 截圖會整片空白 —— 那個坑踩過一次。
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    UpdateWindow(hwnd_);

    // 首次縮圖全部抓完之後才啟動即時刷新。順序很重要：若在抓圖期間就開始
    // 輪替，PumpThumbs 會跟 CaptureAll 搶同一批 Item（而且選擇器還沒顯示，
    // 首次那批是唯一能從螢幕抄到前景視窗的機會）。
    initialDone_ = true;
    if (live_) {
        SetTimer(hwnd_, kTimerRefresh, kRefreshMs, nullptr);
        SetTimer(hwnd_, kTimerThumbs, kThumbMs, nullptr);
    }
    return true;
}

void Picker::CreateResources() {
    auto font = [this](int pt, int weight) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, dpi_, 72);
        lf.lfWeight = weight;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&lf);
    };
    fontTitle_ = font(14, FW_SEMIBOLD);
    fontItem_ = font(10, FW_NORMAL);
    fontSub_ = font(9, FW_NORMAL);
    fontBtn_ = font(10, FW_SEMIBOLD);

    brBg_ = CreateSolidBrush(kBg);
    brCard_ = CreateSolidBrush(kCard);
    brCardHi_ = CreateSolidBrush(kCardHi);
    brAccent_ = CreateSolidBrush(kAccent);
    brAccentHi_ = CreateSolidBrush(kAccentHi);
    brLine_ = CreateSolidBrush(kLine);
}

void Picker::ReleaseResources() {
    HFONT f[4] = {fontTitle_, fontItem_, fontSub_, fontBtn_};
    for (HFONT h : f) if (h) DeleteObject(h);
    fontTitle_ = fontItem_ = fontSub_ = fontBtn_ = nullptr;

    HBRUSH b[6] = {brBg_, brCard_, brCardHi_, brAccent_, brAccentHi_, brLine_};
    for (HBRUSH h : b) if (h) DeleteObject(h);
    brBg_ = brCard_ = brCardHi_ = brAccent_ = brAccentHi_ = brLine_ = nullptr;
}

int Picker::ContentHeight() const {
    const int n = (int)cur().size();
    if (n <= 0) return 0;
    const int rows = (n + kCols - 1) / kCols;
    return rows * cardH_ + (rows - 1) * gap_;
}

void Picker::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const int padX = S(24);
    const int headerH = S(52);
    const int tabsH = S(44);
    const int footerH = S(46);

    gridTop_ = headerH + tabsH;
    footer_ = RECT{0, rc.bottom - footerH, rc.right, rc.bottom};

    // ---- 右側捲軸（自繪） ----
    //
    // 原本用 WS_VSCROLL 讓系統畫。在 Windows 11 上那條捲軸會變成「自動隱藏」
    // 的覆蓋式細線：寬度只剩 8px、顏色跟隨系統淺色主題（深色視窗右邊一條白
    // 邊），而且滑鼠要壓在那一條線正上方才抓得到 —— 實際用起來就是「看得到、
    // 拉不動」。這裡改成自己畫：固定 10px 寬、深色配色、整條軌道都吃滑鼠
    // （HitScrollBar 會把可點區域再放大），拖拇指、點軌道翻頁、滾輪三種都通。
    //
    // 軌道永遠保留，不隨「有沒有東西可捲」出現／消失：那會讓卡片在捲軸出現的
    // 那一刻整排往左跳一格。
    const int sbW = S(10);
    const int sbRight = rc.right - S(10);
    sbTrack_ = RECT{sbRight - sbW, gridTop_, sbRight, footer_.top - S(8)};
    viewport_ = RECT{padX, gridTop_, sbTrack_.left - S(8), footer_.top - S(8)};
    gap_ = S(16);

    const int contentW = viewport_.right - viewport_.left;
    cardW_ = std::max(S(120), (contentW - gap_ * (kCols - 1)) / kCols);
    // 卡片比例：縮圖 128、文字 58（240 寬時）。
    //
    // 原本是 135 + 40，文字那 40px 要塞「標題一行 + 副標題一行」——標題用
    // 10pt、副標 9pt，兩行加起來就快 32px，只剩 8px 的餘裕。視窗標題短
    // （「記事本」）時看不太出來，但「自訂範圍」這種四字標題一上去，標題
    // 和副標就黏在一起。把縮圖讓 7px 給文字，兩邊都還有空間。
    cardH_ = MulDiv(cardW_, 186, 240);
    const int thumbH = MulDiv(cardW_, 128, 240);

    const int tw = S(72);
    const int th = tabsH - S(14);
    for (int i = 0; i < 3; ++i) {
        const int x = padX + i * (tw + S(12));
        tabRect_[i] = RECT{x, headerH + S(8), x + tw, headerH + S(8) + th};
    }

    // 按鈕寬度照文字量。原本是固定的 112 / 84，那是照中文給的：西班牙文
    // 「Configuración」比「設定」寬一倍，固定寬度會把字切掉（看起來像 bug，
    // 而且是只有那個語言才看得到的 bug）。量一次字寬再加左右留白。
    auto textW = [this](const wchar_t* s) {
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, fontBtn_);
        SIZE sz{};
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        return (int)sz.cx;
    };
    const int btnPad = S(20);
    const int bh = S(32);
    const int bw = std::max(S(112), textW(i18n::T(L"開始錄影")) + btnPad);
    const int bwCancel = std::max(S(84), textW(i18n::T(L"取消")) + btnPad);
    const int bwSetup = std::max(S(84), textW(i18n::T(L"設定")) + btnPad);
    const int by = footer_.top + (footerH - bh) / 2;
    btnOk_ = RECT{rc.right - padX - bw, by, rc.right - padX, by + bh};
    btnCancel_ = RECT{btnOk_.left - S(8) - bwCancel, by, btnOk_.left - S(8), by + bh};
    // 「設定」放在最左邊，和右邊那三顆（取消 / 開始錄影）分開 —— 它不是這個
    // 對話框的主角，但也不該藏進右鍵選單：托盤單擊之後這裡就是「程式主視窗」，
    // 使用者習慣從主視窗找設定，而不是回頭去摸托盤。
    btnSetup_ = RECT{padX, by, padX + bwSetup, by + bh};

    auto& v = cur();
    for (size_t i = 0; i < v.size(); ++i) {
        const int col = (int)i % kCols;
        const int row = (int)i / kCols;
        const int x = viewport_.left + col * (cardW_ + gap_);
        const int y = gridTop_ + row * (cardH_ + gap_);
        Item& it = v[i];
        it.card = RECT{x, y, x + cardW_, y + cardH_};
        it.thumbRect = RECT{x + 1, y + 1, x + cardW_ - 1, y + 1 + thumbH};
        // 兩行文字各給一個不重疊的帶狀區域。原本 title 的帶狀區域下緣（+22）
        // 和 sub 的上緣（+20）差 2px 是重疊的 —— 平常因為 title 是 VCenter、
        // 字又短，看不出來；「自訂範圍」這種四字標題一換上去就擠在一起了。
        it.titleRect = RECT{x + S(10), y + thumbH + S(5), x + cardW_ - S(10),
                            y + thumbH + S(26)};
        it.subRect = RECT{x + S(10), y + thumbH + S(28), x + cardW_ - S(10),
                          y + cardH_ - S(6)};
    }
    UpdateScroll();
}

void Picker::UpdateScroll() {
    if (!hwnd_) return;
    const int viewH = PageHeight();
    const int contentH = ContentHeight();
    maxScroll_ = std::max(0, contentH - viewH);
    if (scroll_ > maxScroll_) scroll_ = maxScroll_;
    if (scroll_ < 0) scroll_ = 0;

    // 拇指幾何：長度按「可見比例」，位置按 scroll_ / maxScroll_。
    // 最短 S(32)：清單只有十幾個項目時，按比例算出來的拇指會細到抓不住。
    const int trackH = std::max(1, (int)(sbTrack_.bottom - sbTrack_.top));
    sbThumb_ = sbTrack_;
    if (maxScroll_ <= 0) {
        sbThumb_.bottom = sbThumb_.top;   // 不需要捲 → 不畫（見 DrawScrollBar）
        return;
    }
    int thumbH = (int)((long long)trackH * viewH / std::max(1, contentH));
    thumbH = std::max(S(32), std::min(trackH, thumbH));
    const int span = trackH - thumbH;
    const int top = sbTrack_.top + (int)((long long)span * scroll_ / maxScroll_);
    sbThumb_ = RECT{sbTrack_.left, top, sbTrack_.right, top + thumbH};
}

int Picker::PageHeight() const {
    return std::max(1, (int)(viewport_.bottom - viewport_.top));
}

bool Picker::HitScrollBar(POINT pt) const {
    if (maxScroll_ <= 0) return false;
    // 軌道左右各放寬 S(6)：10px 的拇指在 4K / 高縮放下還是很細，
    // 讓「往右偏一點」也抓得到，比要求使用者對準好。
    RECT r = sbTrack_;
    r.left -= S(6);
    r.right += S(6);
    return PtInRect(&r, pt) != 0;
}

void Picker::DragThumbTo(int y) {
    const int trackH = (int)(sbTrack_.bottom - sbTrack_.top);
    const int thumbH = (int)(sbThumb_.bottom - sbThumb_.top);
    const int span = trackH - thumbH;
    if (span <= 0) return;
    int top = y - sbGrab_ - sbTrack_.top;
    top = std::max(0, std::min(span, top));
    ScrollBy((int)((long long)top * maxScroll_ / span) - scroll_);
}

// ============================================================================
// 繪製
// ============================================================================

void Picker::DrawTabs(HDC dc) {
    const wchar_t* names[3] = {i18n::T(L"視窗"), i18n::T(L"螢幕"), i18n::T(L"區域")};
    for (int i = 0; i < 3; ++i) {
        const RECT r = tabRect_[i];
        const bool active = (tab_ == i);
        const bool hover = (hoverTab_ == i);

        HGDIOBJ oldBr = SelectObject(dc, (active || hover) ? brCardHi_ : brCard_);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);

        SelectObject(dc, fontItem_);
        SetTextColor(dc, active ? kText : kSub);
        RECT t = r;
        DrawTextW(dc, names[i], -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

        if (active) {
            RECT u{r.left + S(14), r.bottom - S(3), r.right - S(14), r.bottom};
            FillRect(dc, &u, brAccent_);
        }
    }
}

void Picker::DrawCard(HDC dc, int idx, bool selected, bool hover) {
    Item& it = cur()[idx];
    RECT card = it.card;
    RECT th = it.thumbRect;
    OffsetRect(&card, 0, -scroll_);
    OffsetRect(&th, 0, -scroll_);

    const int rad = S(10);
    const int saved = SaveDC(dc);
    HRGN rgn = CreateRoundRectRgn(card.left, card.top, card.right + 1, card.bottom + 1, rad, rad);
    ExtSelectClipRgn(dc, rgn, RGN_AND);
    DeleteObject(rgn);

    FillRect(dc, &card, (selected || hover) ? brCardHi_ : brCard_);

    if (it.thumb && it.thumbOk) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ o = SelectObject(src, it.thumb);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, th.left, th.top, th.right - th.left, th.bottom - th.top, src, 0, 0,
                   kThumbW, kThumbH, SRCCOPY);
        SelectObject(src, o);
        DeleteDC(src);
    } else if (it.region) {
        // 「區域」那張卡沒有縮圖也沒有圖示 —— 它不是一個現成的東西。畫一個
        // 虛線框代表「等一下要去拉一個框」，比任何文字都快被讀懂。
        const int inset = S(28);
        RECT box{th.left + inset, th.top + inset, th.right - inset, th.bottom - inset};
        HPEN pen = CreatePen(PS_DOT, std::max(1, S(1)), kAccent);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, box.left, box.top, box.right, box.bottom);
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        // 四個角畫實線短標，讓它看起來像「框選把手」而不是一個空方框
        HPEN pen2 = CreatePen(PS_SOLID, std::max(2, S(2)), kAccent);
        oldPen = SelectObject(dc, pen2);
        const int arm = S(10);
        auto Corner = [&](int x, int y, int dx, int dy) {
            MoveToEx(dc, x, y, nullptr);
            LineTo(dc, x + dx, y);
            MoveToEx(dc, x, y, nullptr);
            LineTo(dc, x, y + dy);
        };
        Corner(box.left, box.top, arm, arm);
        Corner(box.right, box.top, -arm, arm);
        Corner(box.left, box.bottom, arm, -arm);
        Corner(box.right, box.bottom, -arm, -arm);
        SelectObject(dc, oldPen);
        DeleteObject(pen2);

        SelectObject(dc, fontSub_);
        SetTextColor(dc, kSub);
        RECT t = th;
        DrawTextW(dc, i18n::T(L"拖曳框選"), -1, &t,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    } else if (it.icon) {
        // 有圖示就畫圖示：一整排「無法預覽」根本看不出誰是誰
        const int side = std::min((th.right - th.left) / 3, (th.bottom - th.top) / 2);
        const int cx = (th.left + th.right) / 2;
        const int cy = (th.top + th.bottom) / 2;
        DrawIconEx(dc, cx - side / 2, cy - side / 2, it.icon, side, side, 0, nullptr, DI_NORMAL);
    } else {
        SelectObject(dc, fontSub_);
        SetTextColor(dc, kSub);
        RECT t = th;
        DrawTextW(dc, i18n::T(L"無法預覽"), -1, &t,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);

    if (selected || hover) {
        HPEN pen = CreatePen(PS_SOLID, selected ? S(2) : 1, selected ? kAccent : kLine);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, card.left, card.top, card.right, card.bottom, rad, rad);
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    RECT t = it.titleRect;
    OffsetRect(&t, 0, -scroll_);
    SelectObject(dc, fontItem_);
    SetTextColor(dc, kText);
    DrawTextW(dc, it.title.c_str(), -1, &t,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT s = it.subRect;
    OffsetRect(&s, 0, -scroll_);
    SelectObject(dc, fontSub_);
    SetTextColor(dc, kSub);
    DrawTextW(dc, it.sub.c_str(), -1, &s,
              DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void Picker::DrawGrid(HDC dc) {
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport_.left, viewport_.top, viewport_.right, viewport_.bottom);

    const auto& v = cur();
    if (v.empty()) {
        SelectObject(dc, fontItem_);
        SetTextColor(dc, kSub);
        RECT t = viewport_;
        const wchar_t* msg = tab_ == 0   ? i18n::T(L"找不到可以錄製的視窗")
                            : tab_ == 1 ? i18n::T(L"找不到螢幕")
                                        : i18n::T(L"無法建立框選項目");
        DrawTextW(dc, msg, -1, &t,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    for (size_t i = 0; i < v.size(); ++i) {
        RECT card = v[i].card;
        OffsetRect(&card, 0, -scroll_);
        if (card.bottom < viewport_.top || card.top > viewport_.bottom) continue;   // 視窗外
        DrawCard(dc, (int)i, (int)i == sel_[tab_], (int)i == hover_);
    }
    RestoreDC(dc, saved);
}

void Picker::DrawScrollBar(HDC dc) {
    if (maxScroll_ <= 0 || !hwnd_) return;

    const int rad = (int)(sbTrack_.right - sbTrack_.left);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

    // 軌道：用 kLine，比背景亮一階，看得出來是「可以拉的東西」但不會搶眼。
    HGDIOBJ oldBr = SelectObject(dc, brLine_);
    RoundRect(dc, sbTrack_.left, sbTrack_.top, sbTrack_.right, sbTrack_.bottom, rad, rad);

    // 拇指：平常 kSub，滑過或拖曳中提亮。深色視窗上「亮 = 可互動」比
    // 「暗 = 未啟用」好讀，而且和卡片的 hover 提亮規則一致。
    const COLORREF c = (sbDrag_ || sbHover_) ? kText : kSub;
    HBRUSH br = CreateSolidBrush(c);
    SelectObject(dc, br);
    RoundRect(dc, sbThumb_.left, sbThumb_.top, sbThumb_.right, sbThumb_.bottom, rad, rad);
    DeleteObject(br);

    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
}

void Picker::DrawButton(HDC dc, const RECT& r, const wchar_t* text, bool primary,
                        bool hover) {
    HBRUSH fill = primary ? (hover ? brAccentHi_ : brAccent_) : (hover ? brCardHi_ : brCard_);
    HGDIOBJ oldBr = SelectObject(dc, fill);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);

    SelectObject(dc, fontBtn_);
    SetTextColor(dc, primary ? RGB(255, 255, 255) : kText);
    RECT t = r;
    DrawTextW(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

void Picker::DrawFooter(HDC dc) {
    RECT line{0, footer_.top, footer_.right, footer_.top + 1};
    FillRect(dc, &line, brLine_);

    SelectObject(dc, fontSub_);
    SetTextColor(dc, kSub);
    // 提示文字從「設定」按鈕右邊開始，才不會被按鈕壓住。
    RECT hint{btnSetup_.right + S(16), footer_.top, btnCancel_.left - S(12), footer_.bottom};
    // 第二句不是裝飾：選擇器開著的時候截圖熱鍵**仍然有效**（它會先關掉這個
    // 視窗再截圖），但使用者不會知道 —— 沒有這行字，那條路等於不存在。
    //
    // 這段話在中文只有 26 個字，一行放得下；但德文／法文／俄文翻出來是中文
    // 的兩倍長，硬用 DT_SINGLELINE 就會吃掉後半句「截圖熱鍵仍可用」——正好
    // 是最需要被看到的那半句。所以先量寬度：放得下就一行，放不下就折成兩行
    // （頁尾 46px 夠塞 9pt 兩行，按鈕 32px 置中，視覺上不會歪）。
    const wchar_t* hintText = i18n::T(L"點一下選取，點兩下或按 Enter 開始錄影（截圖熱鍵仍可用）");
    RECT one = hint;
    DrawTextW(dc, hintText, -1, &one, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    if (one.right - one.left <= hint.right - hint.left) {
        DrawTextW(dc, hintText, -1, &hint,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        // DT_CALCRECT 回報的高度是「文字方塊」高度，不含最後一行的下伸部
        // （g、p、括號）；照它置中，第二行會被視窗下緣削掉一點。所以量完再加
        // 幾 px 餘裕，並且不讓它超出頁尾。
        RECT two = hint;
        two.bottom = hint.top + S(400);
        DrawTextW(dc, hintText, -1, &two, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int th = (two.bottom - two.top) + S(4);
        int top = hint.top + ((footer_.bottom - hint.top) - th) / 2;
        if (top < hint.top) top = hint.top;
        RECT fit{hint.left, top, hint.right, top + th};
        DrawTextW(dc, hintText, -1, &fit, DT_WORDBREAK | DT_NOPREFIX);
    }

    DrawButton(dc, btnSetup_, i18n::T(L"設定"), false, hoverBtn_ == 3);
    DrawButton(dc, btnCancel_, i18n::T(L"取消"), false, hoverBtn_ == 2);
    DrawButton(dc, btnOk_, i18n::T(L"開始錄影"), true, hoverBtn_ == 1);
}

void Picker::Paint() {
    if (!hwnd_) return;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    FillRect(mem, &rc, brBg_);

    SelectObject(mem, fontTitle_);
    SetTextColor(mem, kText);
    RECT tr{S(24), 0, rc.right - S(24), S(52)};
    DrawTextW(mem, i18n::T(L"選擇要錄製的畫面"), -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(mem, fontSub_);
    SetTextColor(mem, kSub);
    RECT hr{S(24), 0, rc.right - S(24), S(52)};
    DrawTextW(mem, i18n::T(L"Esc 取消"), -1, &hr,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    RECT hline{0, S(52) - 1, rc.right, S(52)};
    FillRect(mem, &hline, brLine_);

    DrawTabs(mem);
    DrawGrid(mem);
    DrawScrollBar(mem);
    DrawFooter(mem);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

// ============================================================================
// 互動
// ============================================================================

int Picker::HitCard(POINT pt) const {
    const auto& v = cur();
    for (size_t i = 0; i < v.size(); ++i) {
        RECT c = v[i].card;
        OffsetRect(&c, 0, -scroll_);
        if (PtInRect(&c, pt)) return (int)i;
    }
    return -1;
}

int Picker::HitTab(POINT pt) const {
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&tabRect_[i], pt)) return i;
    }
    return -1;
}

int Picker::HitButton(POINT pt) const {
    if (PtInRect(&btnOk_, pt)) return 1;
    if (PtInRect(&btnCancel_, pt)) return 2;
    if (PtInRect(&btnSetup_, pt)) return 3;
    return 0;
}

void Picker::ScrollBy(int dy) {
    scroll_ = std::max(0, std::min(maxScroll_, scroll_ + dy));
    UpdateScroll();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// 「區域」分頁的卡片。它不是枚舉來的 —— 它代表「接下來的動作是去框選，
// 而不是錄某個現成的東西」。
//
// 為什麼做成卡片而不是 footer 上多一顆按鈕：
//   1. 三種選擇（視窗 / 螢幕 / 區域）在畫面上等重，footer 按鈕會讓它看起來
//      像「其他操作」，而不是同一題的第三個答案。
//   2. 卡片可以放預覽圖與說明，使用者不必先知道「區域」是什麼意思 ——
//      這一點對第一次用的人差很多。
//   3. 鍵盤導航（方向鍵＋Enter）免費得到，不用為 footer 按鈕另外寫一套。
void Picker::BuildRegionCard() {
    region_.clear();
    Item it;
    it.region = true;
    it.title = i18n::T(L"自訂範圍");
    it.sub = i18n::T(L"拖曳框選要錄製的區域");
    region_.push_back(std::move(it));
}

void Picker::SetTab(int t) {
    if (t < 0 || t > 2) return;
    tab_ = t;
    scroll_ = 0;
    hover_ = -1;
    Layout();
    EnsureVisible();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    LogW(L"[pick] tab=%d items=%zu", tab_, cur().size());
}

void Picker::EnsureVisible() {
    const auto& v = cur();
    if (sel_[tab_] < 0 || sel_[tab_] >= (int)v.size()) return;
    const RECT& c = v[sel_[tab_]].card;
    if (c.top - scroll_ < viewport_.top) scroll_ = c.top - viewport_.top;
    if (c.bottom - scroll_ > viewport_.bottom) scroll_ = c.bottom - viewport_.bottom;
    scroll_ = std::max(0, std::min(maxScroll_, scroll_));
    UpdateScroll();
}

void Picker::MoveSel(int delta) {
    const auto& v = cur();
    if (v.empty()) return;
    int s = sel_[tab_];
    if (s < 0) s = 0;
    s += delta;
    if (s < 0) s = 0;
    if (s > (int)v.size() - 1) s = (int)v.size() - 1;
    sel_[tab_] = s;
    EnsureVisible();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Picker::Confirm() {
    const auto& v = cur();
    const int s = sel_[tab_];
    if (s < 0 || s >= (int)v.size()) {
        LogW(L"[pick] confirm: no selection");
        return;
    }
    Item& it = cur()[s];
    PickResult r;
    r.ok = true;
    if (it.region) {
        // 「區域」不帶 hwnd / monitor / rect：那三個值要等使用者拖完框才知道。
        // 這裡只回報「他選了框選這個動作」，呼叫端收到之後再開覆蓋層。
        // 注意 picker 一定先關掉才輪到呼叫端開覆蓋層 —— 它蓋在螢幕上，不關
        // 的話使用者框到的會是它自己。
        r.region = true;
        LogW(L"[pick] confirm region");
        result_ = r;
        if (hwnd_) DestroyWindow(hwnd_);
        return;
    }
    r.hwnd = it.hwnd;
    r.monitor = it.mon;
    r.rect = it.rect;
    if (it.hwnd) {
        if (IsWindow(it.hwnd)) {
            // 最小化的視窗：WGC 的 CreateForWindow 對它只回一個 154×25 的 item
            // （見 HANDOFF 的坑），所以要在這裡先還原。還原後再讀矩形，
            // HUD 才會落在正確的位置。
            if (it.minimized || IsIconic(it.hwnd)) {
                ShowWindow(it.hwnd, SW_RESTORE);
                SetForegroundWindow(it.hwnd);
                for (int i = 0; i < 40 && IsIconic(it.hwnd); ++i) Sleep(15);
                LogW(L"[pick] restored minimized window %p iconic=%d", (void*)it.hwnd,
                     IsIconic(it.hwnd) ? 1 : 0);
            }
            RECT now{};
            if (GetWindowRect(it.hwnd, &now)) r.rect = now;   // 枚舉到現在可能搬動過
        } else {
            LogW(L"[pick] confirm: window %p is gone", (void*)it.hwnd);
            r.ok = false;
        }
    }
    LogW(L"[pick] confirm ok=%d hwnd=%p mon=%p rect=%ld,%ld,%ld,%ld title=%ls",
         r.ok ? 1 : 0, (void*)r.hwnd, (void*)r.monitor, r.rect.left, r.rect.top,
         r.rect.right, r.rect.bottom, it.title.c_str());
    result_ = r;
    if (hwnd_) DestroyWindow(hwnd_);
}

void Picker::AskSettings() {
    LogW(L"[pick] settings");
    PickResult r;
    r.settings = true;   // ok 保持 false：這次沒有選任何來源
    result_ = r;
    if (hwnd_) DestroyWindow(hwnd_);
}

void Picker::Cancel() {
    LogW(L"[pick] cancel");
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT Picker::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        // 拖曳視窗時不要刷新：佈局一直在變，重畫會讓拖曳頓。
        if (suspend_ > 0) return 0;
        if (w == kTimerRefresh) {
            Refresh();
        } else if (w == kTimerThumbs) {
            if (initialDone_) PumpThumbs(kThumbBatch);
        }
        if (dirty_) {
            dirty_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        ++suspend_;
        return 0;

    case WM_EXITSIZEMOVE:
        if (suspend_ > 0) --suspend_;
        return 0;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (p.y >= 0 && p.y < S(52)) return HTCAPTION;   // 標題區可以拖
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!tracking_) {
            // 沒有 TrackMouseEvent 就收不到 WM_MOUSELEAVE，滑出去之後
            // 卡片會一直停在「懸停」的樣子。
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        // 拖拇指：先吃掉，不要讓下面的 hover / 游標邏輯把拖曳打斷。
        // SetCapture 之後就算游標跑到軌道外也還收得到這裡的座標，
        // 所以往上／往下拖出視窗邊界不會「卡住」。
        if (sbDrag_) {
            DragThumbTo(p.y);
            return 0;
        }
        const bool sbHover = HitScrollBar(p);
        const int hc = sbHover ? -1 : HitCard(p);
        const int ht = sbHover ? -1 : HitTab(p);
        const int hb = sbHover ? 0 : HitButton(p);
        if (hc != hover_ || ht != hoverTab_ || hb != hoverBtn_ || sbHover != sbHover_) {
            hover_ = hc;
            hoverTab_ = ht;
            hoverBtn_ = hb;
            sbHover_ = sbHover;
            InvalidateRect(h, nullptr, FALSE);
        }
        const bool clickable = (hc >= 0) || (ht >= 0) || (hb != 0);
        // IDC_HAND / IDC_ARROW 是 MAKEINTRESOURCE 的 LPSTR，要轉成 W 版才過得了
        // LoadCursorW 的 LPCWSTR（直接寫 (LPCWSTR)IDC_HAND 就是這個原因）。
        // 捲軸用箭頭，不用手型：Windows 的捲軸本來就不是手型。
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)(clickable ? IDC_HAND : IDC_ARROW)));
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_ = hoverTab_ = -1;
        hoverBtn_ = 0;
        sbHover_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        // 捲軸先判：軌道的可點區域有放大（HitScrollBar），不能讓底下的卡片搶走。
        if (HitScrollBar(p)) {
            if (PtInRect(&sbThumb_, p)) {
                sbDrag_ = true;
                sbGrab_ = p.y - sbThumb_.top;
                SetCapture(h);
            } else {
                // 點軌道 = 翻一頁，方向看點在拇指上方還是下方（和系統捲軸一致）。
                ScrollBy(p.y < sbThumb_.top ? -PageHeight() : PageHeight());
            }
            return 0;
        }
        const int hb = HitButton(p);
        if (hb == 1) { Confirm(); return 0; }
        if (hb == 2) { Cancel(); return 0; }
        if (hb == 3) { AskSettings(); return 0; }
        const int ht = HitTab(p);
        if (ht >= 0) { SetTab(ht); return 0; }
        const int hc = HitCard(p);
        if (hc >= 0 && hc != sel_[tab_]) {
            sel_[tab_] = hc;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const int hc = HitCard(p);
        if (hc >= 0) {
            sel_[tab_] = hc;
            Confirm();
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (sbDrag_) {
            sbDrag_ = false;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        // 拖到一半被別人搶走（Alt+Tab、系統對話框）：不要留著 sbDrag_=true，
        // 否則回來之後滑鼠一動就繼續捲，看起來像「視窗自己在跳」。
        sbDrag_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(w);
        ScrollBy(-(delta / WHEEL_DELTA) * S(120));
        return 0;
    }

    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: Cancel(); return 0;
        case VK_RETURN:
        case VK_SPACE: Confirm(); return 0;
        case VK_TAB: SetTab((tab_ + 1) % 3); return 0;
        case VK_LEFT: MoveSel(-1); return 0;
        case VK_RIGHT: MoveSel(1); return 0;
        case VK_UP: MoveSel(-kCols); return 0;
        case VK_DOWN: MoveSel(kCols); return 0;
        case VK_HOME: MoveSel(-100000); return 0;
        case VK_END: MoveSel(100000); return 0;
        case VK_PRIOR: MoveSel(-kCols * 3); return 0;
        case VK_NEXT: MoveSel(kCols * 3); return 0;
        default: return 0;
        }

    case WM_CLOSE:
        Cancel();
        return 0;

    case WM_DESTROY:
        KillTimer(h, kTimerRefresh);
        KillTimer(h, kTimerThumbs);
        hwnd_ = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Picker::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Picker*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->Handle(h, m, w, l);
}

PickResult Picker::Run() {
    LogReset();
    LogW(L"[pick] begin live=%d", live_ ? 1 : 0);

    Enumerate();
    LogW(L"[pick] windows=%zu monitors=%zu", wins_.size(), mons_.size());
    CaptureAll();

    sel_[0] = wins_.empty() ? -1 : 0;
    sel_[1] = mons_.empty() ? -1 : 0;
    sel_[2] = region_.empty() ? -1 : 0;   // 區域永遠只有一張卡
    tab_ = wins_.empty() ? 1 : 0;   // 沒有視窗就直接開螢幕頁

    if (!CreateAndShow()) {
        LogW(L"[pick] CreateWindow FAILED");
        ReleaseThumbs();
        return PickResult{};
    }

    // 這裡刻意**不用** GetMessage 為主迴圈，而是 PeekMessage + WaitMessage：
    // 全域熱鍵的 WM_HOTKEY 是送給主訊息窗的，兩種迴圈都抽得到它，但我們要在
    // 「使用者還開著選擇器」的當下就能決定要不要把它攔下來（見 onSysMsg_）。
    //
    // 攔到之後的順序是「關掉 picker → 結束迴圈 → 由呼叫端處理訊息」。
    // 反過來（在這裡直接處理）等於在選擇器的訊息迴圈裡再開一個 modal，
    // 而且螢幕上還蓋著選擇器 —— 截圖會截到它、框選會框到它。
    MSG msg{};
    while (hwnd_) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;   // 跟原本的 GetMessage 迴圈一致
            if (onSysMsg_ && onSysMsg_(msg)) {
                LogW(L"[pick] intercepted msg=0x%04X -> closing picker", msg.message);
                sysMsgHit_ = true;
                sysMsg_ = msg;
                // 不要留腳印：這個訊息已經被我們消費掉了，外面不該再收到一次。
                if (hwnd_) DestroyWindow(hwnd_);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            WaitMessage();
        }
    }

    const PickResult r = result_;
    ReleaseThumbs();
    ReleaseResources();
    LogW(L"[pick] end ok=%d region=%d settings=%d hwnd=%p mon=%p",
         r.ok ? 1 : 0, r.region ? 1 : 0, r.settings ? 1 : 0, (void*)r.hwnd, (void*)r.monitor);

    // 攔到熱鍵這條路：PickResult 直接帶著那個訊息出去，三個旗標保持 false。
    // 記在 result_ 之後才回傳，是為了讓上面那行 log 反映「使用者其實沒選東西」。
    if (sysMsgHit_) {
        PickResult s;
        s.sysMsg = true;
        s.msg = sysMsg_;
        return s;
    }
    return r;
}

} // namespace

PickResult PickSource(HWND owner, bool live, SysMsgFn onSysMsg) {
    (void)owner;   // 置中基準是「滑鼠所在螢幕」，托盤的 0×0 owner 沒有意義
    Picker p(live, std::move(onSysMsg));
    return p.Run();
}

} // namespace app
