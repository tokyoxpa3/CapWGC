// ============================================================================
// settings.cpp — 設定持久化與設定視窗的實作
// ============================================================================

#include "settings.h"

#include <commctrl.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace app {
namespace {

constexpr int IDC_TAB = 1000;
constexpr int IDC_DIR_EDIT = 1001;
constexpr int IDC_DIR_BROWSE = 1002;
constexpr int IDC_CODEC = 1003;
constexpr int IDC_FPS = 1004;
constexpr int IDC_QUALITY = 1005;
constexpr int IDC_QUALITY_VAL = 1006;
constexpr int IDC_AUDIO_SYS = 1007;
constexpr int IDC_AUDIO_MIC = 1008;
constexpr int IDC_CURSOR = 1009;
constexpr int IDC_HINT = 1010;
constexpr int IDC_AUDIO_SEP = 1011;
constexpr int IDC_HK_REGION = 1020;
constexpr int IDC_HK_PICKER = 1021;
constexpr int IDC_HK_SCREEN = 1022;
constexpr int IDC_HK_STOP = 1023;
constexpr int IDC_HK_ERROR = 1024;
constexpr int IDC_HK_SNIP = 1025;
constexpr int IDC_SNIP_SAVE = 1031;
constexpr int IDC_SNIP_DIR = 1032;
constexpr int IDC_SNIP_DIR_BROWSE = 1033;
constexpr int IDC_PRTSN_TAKE = 1034;
constexpr int IDC_PRTSN_NOTE = 1035;

const wchar_t* kCodecItems[] = {L"自動（AV1 > HEVC > H.264）", L"AV1", L"HEVC", L"H.264"};
// 幀率是「上限」不是「保證」：WGC 的送幀速度跟螢幕刷新率綁在一起，60Hz 的
// 螢幕就算選 240 也只會出 60。144 / 165 / 240 只有在高刷螢幕上才有意義，
// 而且檔案大小是線性成長的（240 大約是 60 的四倍）。
const int kFpsItems[] = {30, 60, 120, 144, 165, 240};
constexpr int kFpsCount = (int)(sizeof(kFpsItems) / sizeof(kFpsItems[0]));
constexpr int kFpsDefaultSel = 1;      // 60 fps

// ---- 虛擬鍵碼 ↔ 文字 ------------------------------------------------------
//
// 只列「使用者真的會拿來當熱鍵」的那些。其餘（滑鼠鍵、IME 鍵、媒體鍵）用
// VK%02X 顯示與解析 —— 罕見但不會靜默吃掉，手改 INI 也還原得回來。
struct KeyName {
    UINT vk;
    const wchar_t* name;
};

const KeyName kKeyNames[] = {
    {VK_BACK, L"Backspace"},   {VK_TAB, L"Tab"},
    {VK_RETURN, L"Enter"},     {VK_PAUSE, L"Pause"},
    {VK_CAPITAL, L"CapsLock"}, {VK_ESCAPE, L"Esc"},
    {VK_SPACE, L"Space"},      {VK_PRIOR, L"PageUp"},
    {VK_NEXT, L"PageDown"},    {VK_END, L"End"},
    {VK_HOME, L"Home"},        {VK_LEFT, L"Left"},
    {VK_UP, L"Up"},            {VK_RIGHT, L"Right"},
    {VK_DOWN, L"Down"},        {VK_INSERT, L"Insert"},
    {VK_DELETE, L"Delete"},    {VK_SNAPSHOT, L"PrintScreen"},
    {VK_NUMLOCK, L"NumLock"},  {VK_SCROLL, L"ScrollLock"},
    {VK_OEM_1, L";"},          {VK_OEM_PLUS, L"="},
    {VK_OEM_COMMA, L","},      {VK_OEM_MINUS, L"-"},
    {VK_OEM_PERIOD, L"."},     {VK_OEM_2, L"/"},
    {VK_OEM_3, L"`"},          {VK_OEM_4, L"["},
    {VK_OEM_5, L"\\"},         {VK_OEM_6, L"]"},
    {VK_OEM_7, L"'"},
};

std::wstring VkName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    for (const auto& k : kKeyNames) {
        if (k.vk == vk) return k.name;
    }
    wchar_t b[16];
    swprintf_s(b, L"VK%02X", vk);
    return b;
}

bool VkFromName(const std::wstring& in, UINT* out) {
    if (in.empty() || !out) return false;
    if (in.size() == 1) {
        const wchar_t c = towupper(in[0]);
        if (c >= L'A' && c <= L'Z') { *out = (UINT)c; return true; }
        if (c >= L'0' && c <= L'9') { *out = (UINT)c; return true; }
    }
    // 常見別名（不然「手改 INI 打 Escape 就失敗」這種事很惱人）
    struct Alias {
        const wchar_t* text;
        UINT vk;
    };
    static const Alias kAliases[] = {
        {L"escape", VK_ESCAPE},   {L"return", VK_RETURN},
        {L"del", VK_DELETE},      {L"ins", VK_INSERT},
        {L"pgup", VK_PRIOR},      {L"pgdn", VK_NEXT},
        {L"prtsc", VK_SNAPSHOT},  {L"print", VK_SNAPSHOT},
        {L"spacebar", VK_SPACE},
    };
    for (const auto& a : kAliases) {
        if (!_wcsicmp(in.c_str(), a.text)) { *out = a.vk; return true; }
    }
    for (const auto& k : kKeyNames) {
        if (!_wcsicmp(in.c_str(), k.name)) { *out = k.vk; return true; }
    }
    for (UINT vk = VK_F1; vk <= VK_F24; ++vk) {
        if (!_wcsicmp(in.c_str(), VkName(vk).c_str())) { *out = vk; return true; }
    }
    // 數字鍵盤：接受 Num7 與 Numpad7 兩種寫法（VkName 產生前者，鍵盤上的字
    // 是後者，手改 INI 很容易打成後者）。
    for (UINT vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) {
        const std::wstring n = VkName(vk);
        if (!_wcsicmp(in.c_str(), n.c_str())) { *out = vk; return true; }
        if (!_wcsicmp(in.c_str(), (L"numpad" + n.substr(3)).c_str())) { *out = vk; return true; }
    }
    if (in.size() > 2 && towupper(in[0]) == L'V' && towupper(in[1]) == L'K') {
        const unsigned long v = wcstoul(in.c_str() + 2, nullptr, 16);
        if (v > 0 && v < 0x100) { *out = (UINT)v; return true; }
    }
    return false;
}

// 對話框執行期間的暫存。用靜態是因為視窗訊息是 C 介面，沒有 this。
struct DlgState {
    Settings* s = nullptr;
    bool ok = false;
    HFONT font = nullptr;
    HWND hwnd = nullptr;
    Hotkey hk[5];          // 對應 IDC_HK_* 的編輯順序
    std::wstring hkError;  // 非空 → 顯示在 IDC_HK_ERROR
    // 「用 PrtScn 取代 Windows 剪取工具」勾了沒。真正的登錄檔改動在呼叫端
    // （app::EngagePrtScn），這裡只是把使用者的意圖帶出去。
    bool wantPrtsc = false;

    // ---- 分頁 ----
    //
    // 三個分頁是「同一批控制項 + 一個 Tab 控制項」：每個控制項建立時就掛到
    // 所屬頁面，切頁時只做 ShowWindow，**不重建**。重建會把使用者的輸入、
    // 熱鍵子類化、捲動位置全部弄掉，而且每次切頁都要重跑一次 SetWindowSubclass
    // —— 那是這類重構最常見的坑。
    HWND tab = nullptr;
    std::vector<HWND> page[3];
    int curPage = 0;
    // 頁面內容的原點（client 座標）。由 TCM_ADJUSTRECT 算出來，不是寫死的：
    // 分頁籤的高度跟字型、DPI 有關，寫死一個 y 在某些縮放比例下會壓到籤上。
    int pageX = 0;
    int pageY = 0;
};

DlgState* g_dlg = nullptr;

// 建立控制項時「這一頁」是哪一頁。-1 = 不分頁（Tab 本身、確定/取消）。
// 用全域而不是多傳一個參數：Child() 有三十幾個呼叫點，逐一改只會讓版面
// 程式更難讀。
int g_curPage = -1;

// 分頁順序（使用者指定：一般 → OCR → 熱鍵）。
const wchar_t* kPageNames[3] = {L"一般", L"OCR", L"熱鍵"};
constexpr int kPageCount = 3;

constexpr int kHotkeyCount = 5;
const int kHotkeyIds[kHotkeyCount] = {IDC_HK_REGION, IDC_HK_PICKER, IDC_HK_SCREEN,
                                      IDC_HK_STOP, IDC_HK_SNIP};

HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
           int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (c && g_dlg && g_dlg->font) SendMessageW(c, WM_SETFONT, (WPARAM)g_dlg->font, TRUE);
    // 掛到目前這一頁（見 DlgState::page 的說明）。
    if (c && g_dlg && g_curPage >= 0 && g_curPage < kPageCount) {
        g_dlg->page[g_curPage].push_back(c);
    }
    return c;
}

void SetText(HWND parent, int id, const std::wstring& t) {
    SetDlgItemTextW(parent, id, t.c_str());
}

std::wstring GetText(HWND parent, int id) {
    const int n = GetWindowTextLengthW(GetDlgItem(parent, id));
    std::wstring s((size_t)n, L'\0');
    if (n > 0) GetDlgItemTextW(parent, id, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}

int ComboSel(HWND parent, int id) {
    return (int)SendDlgItemMessageW(parent, id, CB_GETCURSEL, 0, 0);
}

// 定義在下面（版面那一節）。這裡先宣告，是因為錯誤訊息要能跳回熱鍵頁。
void ShowPage(HWND h, int page);

void ShowHotkeyError(HWND h, const std::wstring& msg) {
    if (!g_dlg) return;
    g_dlg->hkError = msg;
    // 錯誤訊息住在「熱鍵」那一頁。使用者可能正停在「一般」頁按確定，那時
    // 這行字是隱藏的 —— 視窗不會關、又沒有任何看得見的理由，看起來像
    // 「按確定沒反應」。所以有錯就跳回熱鍵頁。
    if (!msg.empty()) ShowPage(h, 2);
    if (HWND e = GetDlgItem(h, IDC_HK_ERROR)) SetWindowTextW(e, msg.c_str());
}

// ---- 熱鍵擷取控制項 --------------------------------------------------------
//
// 用「唯讀 EDIT + 子類化」而不是自繪控制項：
//   - 唯讀 EDIT 仍然收得到 WM_KEYDOWN / WM_SYSKEYDOWN（自繪視窗要先搶焦點）
//   - 它天生有邊框、有 caret 位置、能進 Tab 順序，看起來就像一個欄位
//   - 關鍵是 WM_GETDLGCODE 回 DLGC_WANTALLKEYS：不這樣做的話，Enter 會被
//     對話框吃掉（直接按「確定」關窗），Esc 會關窗，Tab 會跳走 ——
//     使用者會覺得「這欄位根本抓不到鍵」。
void RefreshHotkeyBox(int idx) {
    if (!g_dlg || idx < 0 || idx >= kHotkeyCount) return;
    HWND e = GetDlgItem(g_dlg->hwnd, kHotkeyIds[idx]);
    if (e) SetWindowTextW(e, HotkeyText(g_dlg->hk[idx]).c_str());
}

LRESULT CALLBACK HotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                            DWORD_PTR ref) {
    const int idx = (int)ref;

    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = (UINT)w;

        if (vk == VK_TAB) {
            // 自己把 Tab 轉出去：回 DLGC_WANTALLKEYS 之後對話框不會代勞了
            HWND next = GetNextDlgTabItem(GetParent(h), h,
                                          (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next) SetFocus(next);
            return 0;
        }
        if (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_DELETE) {
            g_dlg->hk[idx] = Hotkey{};
            ShowHotkeyError(GetParent(h), L"");
            RefreshHotkeyBox(idx);
            return 0;
        }

        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;

        // 只按了修飾鍵：先顯示「Ctrl+…」，等真正的鍵
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN ||
            vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
            vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            // Win 當修飾鍵在這裡是「無聲死路」，所以要在**這裡**擋，不是在後面
            // 檢查 mods 的地方 —— shell 會在 Win 的 keydown 之後就把整個組合
            // 接走（連帶彈出開始功能表），後面那個真正的鍵永遠不會送進欄位，
            // 流程走不到一般的 mods 判斷。實測 Win+Shift+P 永遠停在「Shift+Win+…」，
            // 使用者只會覺得「按了沒反應」。已記在 docs/09 的踩坑表。
            if (mods & MOD_WIN) {
                ShowHotkeyError(GetParent(h),
                                L"Win 組合會被系統接走，抓不到；請改用 Ctrl / Alt / Shift");
                return 0;
            }
            std::wstring t;
            if (mods & MOD_CONTROL) t += L"Ctrl+";
            if (mods & MOD_ALT) t += L"Alt+";
            if (mods & MOD_SHIFT) t += L"Shift+";
            SetWindowTextW(h, (t + L"…").c_str());
            return 0;
        }

        if (vk == VK_RETURN && mods == 0) {
            // 焦點在欄位上時按 Enter = 按「確定」（不然會變成「這欄抓不到鍵」）
            HWND parent = GetParent(h);
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                         (LPARAM)GetDlgItem(parent, IDOK));
            return 0;
        }
        if (vk == VK_F12) {
            // F12 被核心偵錯器保留，RegisterHotKey 一定失敗
            ShowHotkeyError(GetParent(h), L"F12 是系統保留鍵，請換一個");
            return 0;
        }
        if (mods == 0 && vk != VK_SNAPSHOT) {
            ShowHotkeyError(GetParent(h),
                            L"熱鍵至少要有一個修飾鍵（Ctrl / Alt / Shift）");
            return 0;
        }
        // Win 當修飾鍵在這裡是「無聲死路」：shell 會在按鍵送到前景視窗前就接走
        // Win+<key>（順便彈出開始功能表），所以我們只會收到 Win 與 Shift 這兩個
        // 修飾鍵的 keydown，真正那個鍵永遠不會到 —— 欄位就停在「Shift+Win+…」，
        // 使用者只會覺得「按了沒反應」。與其留一個半截狀態，不如當場說清楚。
        // 實測 Win+Shift+P 停在 Shift+Win+…，已記在 docs/09 的踩坑表。
        if (mods & MOD_WIN) {
            ShowHotkeyError(GetParent(h),
                            L"Win 組合會被系統接走，抓不到；請改用 Ctrl / Alt / Shift");
            return 0;
        }

        g_dlg->hk[idx] = Hotkey{mods, vk};
        ShowHotkeyError(GetParent(h), L"");
        RefreshHotkeyBox(idx);
        return 0;
    }

    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_CONTEXTMENU:
        return 0;   // 不要讓輸入法／貼上改到這個欄位，也不要嗶嗶叫

    case WM_LBUTTONDOWN:
        SetFocus(h);
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, TRUE);
        break;

    default:
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

void ReadBack(HWND h, Settings* s) {
    s->outputDir = GetText(h, IDC_DIR_EDIT);
    const int c = ComboSel(h, IDC_CODEC);
    if (c >= 0) s->codec = c;
    const int f = ComboSel(h, IDC_FPS);
    if (f >= 0 && f < kFpsCount) s->fps = kFpsItems[f];
    s->quality = (int)SendDlgItemMessageW(h, IDC_QUALITY, TBM_GETPOS, 0, 0);
    s->captureCursor = IsDlgButtonChecked(h, IDC_CURSOR) == BST_CHECKED;
    s->audioSystem = IsDlgButtonChecked(h, IDC_AUDIO_SYS) == BST_CHECKED;
    s->audioMic = IsDlgButtonChecked(h, IDC_AUDIO_MIC) == BST_CHECKED;
    s->separateTracks = IsDlgButtonChecked(h, IDC_AUDIO_SEP) == BST_CHECKED;
    s->snipSaveFile = IsDlgButtonChecked(h, IDC_SNIP_SAVE) == BST_CHECKED;
    s->snipDir = GetText(h, IDC_SNIP_DIR);

    // PrtScn 接管：勾選＝要 PrtScn 給截圖用。這裡只把「意圖」記進 hotkeys
    // （snip 綁上 PrtScn），**真正的系統設定在呼叫端才改** —— 設定視窗不該
    // 在按下確定之前就動到登錄檔，使用者可能按取消。
    g_dlg->wantPrtsc = IsDlgButtonChecked(h, IDC_PRTSN_TAKE) == BST_CHECKED;
    if (g_dlg) {
        s->hotkeys.region = g_dlg->hk[0];
        s->hotkeys.picker = g_dlg->hk[1];
        s->hotkeys.screen = g_dlg->hk[2];
        s->hotkeys.stop = g_dlg->hk[3];
        s->hotkeys.snip = g_dlg->hk[4];
    }
}

// ---- 版面 ------------------------------------------------------------------
//
// 每個分頁的內容都用「相對於頁面原點」的座標寫，建立時才加上 pageX / pageY。
// 這樣三個頁面可以各自從 (0,0) 排起，不必為了分頁籤的高度重算每一行 y。
//
// 頁面高度取三者最大（熱鍵頁最高，約 250px）。改版面時要確認最大的那一頁
// 還塞得進 kTabH 扣掉籤頭的高度 —— 塞不進去不會有任何錯誤訊息，控制項只會
// 被切掉，所以留了餘裕。
constexpr int kTabX = 12;
constexpr int kTabY = 12;
constexpr int kTabW = 452;
constexpr int kTabH = 300;
constexpr int kPad = 4;    // 內容與分頁內緣的距離

void BuildControls(HWND h, const Settings& s) {
    const int dpi = GetDpiForWindow(h);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, dpi, 72);
    lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_dlg->font = CreateFontIndirectW(&lf);

    g_dlg->hk[0] = s.hotkeys.region;
    g_dlg->hk[1] = s.hotkeys.picker;
    g_dlg->hk[2] = s.hotkeys.screen;
    g_dlg->hk[3] = s.hotkeys.stop;
    g_dlg->hk[4] = s.hotkeys.snip;
    // 初始勾選狀態看「截圖熱鍵是不是 PrtScn」，而不是看 ini 的 owned 旗標。
    // 兩者可能不同步（例如使用者手改了 ini，或 PrtScn 被設定視窗清掉），
    // 而使用者看到的是熱鍵欄位 —— 它必須跟勾選框說同一件事。
    g_dlg->wantPrtsc = (s.hotkeys.snip.vk == VK_SNAPSHOT && s.hotkeys.snip.mods == 0);

    // ---- 分頁籤 ----
    g_curPage = -1;   // 分頁籤本身不屬於任何一頁
    HWND tab = Child(h, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, kTabX, kTabY,
                     kTabW, kTabH, IDC_TAB);
    g_dlg->tab = tab;
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<LPWSTR>(kPageNames[i]);
        // **一定要用 TCM_INSERTITEMW**，不能用 TabCtrl_InsertItem 巨集。
        //
        // 這個專案沒有定義 UNICODE（所有 API 都直接呼叫 ...W 版本），所以巨集
        // 會展開成 TCM_INSERTITEMA（TCM_FIRST+7，W 版是 +62）。把 TCITEMW 的
        // 寬字串指標交給 A 版處理，comctl32 會把它當 char* 讀：
        //   「一般」→ 第一個 byte 就是 0x00 → 空字串
        //   「OCR」 → 讀到 'O' 後面接 0x00 → 只剩 "O"
        //   「熱鍵」→ 讀到 4 個 byte 才碰到 0 → 變成亂碼
        // 症狀是「分頁名稱顯示異常」，但不會有任何錯誤碼 —— 訊息本身送得成功。
        SendMessageW(tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&it);
    }
    // 內容原點用 TCM_ADJUSTRECT 算，不寫死：籤頭的高度跟字型／DPI 有關，
    // 寫死一個 y 在某些縮放比例下會壓到籤上，或留下太大的空白。
    RECT dr{};
    GetClientRect(tab, &dr);
    TabCtrl_AdjustRect(tab, FALSE, &dr);
    MapWindowPoints(tab, h, reinterpret_cast<POINT*>(&dr), 2);
    g_dlg->pageX = dr.left + kPad;
    g_dlg->pageY = dr.top + kPad;

    // 頁面內的座標 → 視窗座標，並掛到指定的那一頁（見 Child）。
    auto P = [h](int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                 int y, int w, int hh, int id) {
        g_curPage = page;
        HWND c = Child(h, cls, text, style, g_dlg->pageX + x, g_dlg->pageY + y, w, hh, id);
        g_curPage = -1;
        return c;
    };

    // ========================================================================
    // 第一頁：一般（錄影）
    // ========================================================================
    P(0, L"STATIC", L"輸出資料夾", SS_LEFT, 0, 0, 200, 18, 0);
    // 文字要在建立時就給：用 SetDlgItemTextW 事後補會有一瞬間的空欄位，
    // 而且很容易忘（第一版就忘了，欄位一直是空的）。
    P(0, L"EDIT", s.outputDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 0, 20, 340, 24,
      IDC_DIR_EDIT);
    P(0, L"BUTTON", L"瀏覽…", BS_PUSHBUTTON, 348, 20, 80, 24, IDC_DIR_BROWSE);

    P(0, L"STATIC", L"編碼器", SS_LEFT, 0, 58, 100, 18, 0);
    HWND cb = P(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 78, 214, 200,
                IDC_CODEC);
    for (const wchar_t* it : kCodecItems) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)it);
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)(s.codec < 4 ? s.codec : 0), 0);

    P(0, L"STATIC", L"幀率", SS_LEFT, 232, 58, 100, 18, 0);
    HWND fb = P(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 232, 78, 196, 200,
                IDC_FPS);
    for (int f : kFpsItems) {
        wchar_t b[32];
        // 高刷選項直接標在字串上，不另外擠一行說明 —— 對話框沒有多餘的垂直空間，
        // 而且選單展開時就是最需要看到這句話的時候。
        if (f >= 144)
            swprintf_s(b, L"%d fps（高刷螢幕）", f);
        else
            swprintf_s(b, L"%d fps", f);
        SendMessageW(fb, CB_ADDSTRING, 0, (LPARAM)b);
    }
    int fsel = kFpsDefaultSel;
    for (int i = 0; i < kFpsCount; ++i) if (kFpsItems[i] == s.fps) fsel = i;
    SendMessageW(fb, CB_SETCURSEL, (WPARAM)fsel, 0);

    P(0, L"STATIC", L"品質", SS_LEFT, 0, 118, 60, 18, 0);
    wchar_t qv[16];
    swprintf_s(qv, L"%d", s.quality);
    P(0, L"STATIC", qv, SS_LEFT, 48, 118, 60, 18, IDC_QUALITY_VAL);

    HWND tb = P(0, L"msctls_trackbar32", L"", TBS_HORZ | TBS_AUTOTICKS, 0, 138, 428, 32,
                IDC_QUALITY);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(30, 100));
    SendMessageW(tb, TBM_SETTICFREQ, 10, 0);
    SendMessageW(tb, TBM_SETPOS, TRUE, s.quality);

    P(0, L"BUTTON", L"系統聲音", BS_AUTOCHECKBOX, 0, 184, 110, 22, IDC_AUDIO_SYS);
    P(0, L"BUTTON", L"麥克風", BS_AUTOCHECKBOX, 120, 184, 90, 22, IDC_AUDIO_MIC);
    P(0, L"BUTTON", L"錄下游標", BS_AUTOCHECKBOX, 220, 184, 110, 22, IDC_CURSOR);
    CheckDlgButton(h, IDC_AUDIO_SYS, s.audioSystem ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_AUDIO_MIC, s.audioMic ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_CURSOR, s.captureCursor ? BST_CHECKED : BST_UNCHECKED);

    // 分軌是給剪輯用的：檔案裡除了混音軌之外，每個音源再各留一條（標成
    // 「不要預設播」，所以一般播放器仍然只播混音軌）。預設關 —— 開了檔案
    // 會大一截，而且多數人只是錄完直接播。
    P(0, L"BUTTON", L"保留分軌音源（給剪輯用，檔案較大）", BS_AUTOCHECKBOX, 0, 210, 420,
      22, IDC_AUDIO_SEP);
    CheckDlgButton(h, IDC_AUDIO_SEP, s.separateTracks ? BST_CHECKED : BST_UNCHECKED);

    // ========================================================================
    // 第二頁：OCR（截圖 / OCR 相關）
    // ========================================================================
    //
    // 這一頁刻意很短：真正需要設定的東西只有「截圖要不要順便存檔」與「存到
    // 哪裡」。預設動作（截圖 or OCR）不放這裡，而是記住使用者上一次在工具列上
    // 選了什麼 —— 那才是使用者調這個值的地方，進來一次就懂了。
    P(1, L"STATIC", L"截圖 / OCR（熱鍵進入後，上方工具列可切換）", SS_LEFT, 0, 0, 430,
      18, 0);
    P(1, L"BUTTON", L"截圖時也存一份（檔案總管可直接貼上）", BS_AUTOCHECKBOX, 0, 24, 420,
      22, IDC_SNIP_SAVE);
    CheckDlgButton(h, IDC_SNIP_SAVE, s.snipSaveFile ? BST_CHECKED : BST_UNCHECKED);

    // 圖片專用資料夾。留空 = 輸出資料夾底下的 Snip\（見 Settings::SnipDir）。
    // 之所以獨立一欄而不是「跟影片放一起」：截圖通常只是過渡產物，分開之後
    // 整個資料夾可以一次清掉，不會連影片一起誤刪。
    P(1, L"STATIC", L"圖片資料夾（留空 = 輸出資料夾\\Snip）", SS_LEFT, 0, 54, 400, 18, 0);
    P(1, L"EDIT", s.snipDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 0, 74, 340, 24,
      IDC_SNIP_DIR);
    P(1, L"BUTTON", L"瀏覽…", BS_PUSHBUTTON, 348, 74, 80, 24, IDC_SNIP_DIR_BROWSE);

    // ---- PrtScn 接管 ----
    //
    // 這是一個「會改到系統設定」的選項，所以必須在畫面上講清楚，不能藏在
    // 熱鍵欄位裡讓使用者自己填 PrtScn 就默默生效 —— 那樣他不會知道
    // Win+Shift+S 之類的行為被動過。
    //
    // 為什麼不讓使用者直接把 snip 填成 PrtScn 就好：技術上填得進去
    // （HotkeyUsable 對 PrtScn 放行），但**按了不會有反應**，因為 Windows
    // 11 會先一步把 PrtScn 交給剪取工具。實測：登錄值不存在或為 1 時
    // RegisterHotKey 回成功但收不到 WM_HOTKEY；設成 0 才收得到。
    // 所以「接管」這件事必然包含改系統設定，那就必須是一個看得見的開關。
    const bool taken = app::PrtScnOwned();
    P(1, L"BUTTON", L"用 PrtScn 取代 Windows 剪取工具（截圖 / OCR）",
      BS_AUTOCHECKBOX | WS_TABSTOP, 0, 112, 430, 20, IDC_PRTSN_TAKE);
    if (g_dlg->wantPrtsc) {
        CheckDlgButton(h, IDC_PRTSN_TAKE, BST_CHECKED);
    }
    P(1, L"STATIC",
      taken ? L"目前由 CapWGC 接管。取消勾選會還原 Windows 的 PrtScn。"
            : L"預設開啟。會關閉 Windows 11 的「使用 PrtScn 開啟剪取工具」"
              L"（Win+Shift+S 不受影響），並把 PrtScn 綁給截圖 / OCR。",
      SS_LEFT, 16, 132, 414, 34, IDC_PRTSN_NOTE);

    // OCR 引擎本身沒有可調的東西（模型固定在 exe 旁邊的 models\），所以這一頁
    // 只說明「它在那裡、壞了看哪裡」。留白比多一個不能用的欄位好。
    //
    // 高度要留給「實際折行後」的行數，不是 `\r\n` 的個數：這裡的三行字裡，中間
    // 那行在 430px 寬下會自己折成兩行，所以總共是 4 行。原本給 56px（＝3 行）
    // 會把最後一行切成一半 —— 而且 STATIC 不會回報「字被切掉」，看起來就像
    // 排版壞了。要嘛加高、要嘛把那行縮短，這裡兩件都做（見下面那句）。
    P(1, L"STATIC",
      L"OCR 引擎：模型固定放在程式旁邊的 models\\，不需要設定。\r\n"
      L"工具列的「OCR」是灰色＝引擎沒載入（原因見托盤提示，只提示一次）。\r\n"
      L"辨識結果直接進剪貼簿，不開面板、不落地成檔案。",
      SS_LEFT, 0, 176, 444, 74, 0);

    // ========================================================================
    // 第三頁：熱鍵
    // ========================================================================
    //
    // 這裡本來還有一塊「OCR 模型」路徑。**已移除**：那只是覆寫模型目錄的
    // 逃生門（給「模型放在網路磁碟」這種情況），但一般使用者的模型就在
    // exe 旁邊的 models\，這個欄位對他們只有兩個效果 —— 看起來像在選版本
    // （其實不是，換版本是換檔案、不是改路徑），以及手動改壞之後 OCR 默默
    // 失效。真的需要換模型目錄的情境太罕見，不值得占用設定畫面的版面。
    P(2, L"STATIC", L"熱鍵（點欄位後直接按組合鍵）", SS_LEFT, 0, 0, 300, 18, 0);
    // 顯示順序和 hk[] / kHotkeyIds[] 的索引是**兩件事**，別混在一起。
    // 最常用的排最上面（「視窗清單」連托盤單擊都會走，改動頻率最高）。
    // kHkOrder 存的是「原始索引」：HotkeyProc 的 dwRefData 拿到的也是原始索引，
    // 所以排序只影響畫面，寫回 settings 的對應關係完全不受影響。
    static const int kHkOrder[kHotkeyCount] = {1, 0, 2, 3, 4};
    static const wchar_t* kHkLabels[kHotkeyCount] = {
        L"視窗清單（縮圖）", L"選區域錄影 / 拖曳框選", L"錄主螢幕", L"停止",
        L"截圖 / OCR"};
    for (int row = 0; row < kHotkeyCount; ++row) {
        const int i = kHkOrder[row];
        const int y = 24 + row * 28;
        P(2, L"STATIC", kHkLabels[row], SS_LEFT, 0, y + 3, 140, 18, 0);
        HWND e = P(2, L"EDIT", HotkeyText(g_dlg->hk[i]).c_str(),
                   WS_BORDER | ES_READONLY | ES_CENTER | WS_TABSTOP, 148, y, 180, 24,
                   kHotkeyIds[i]);
        if (e) SetWindowSubclass(e, HotkeyProc, (UINT_PTR)kHotkeyIds[i], (DWORD_PTR)i);
    }

    // 這行提示是必要的，不是裝飾：Win 當修飾鍵時，Win 的 keydown 會被 shell
    // 先吃掉（連帶彈出開始功能表），應用層只收得到「Shift+Win」這種半截組合，
    // 後面那個真正的鍵永遠不會到 —— 實測 Win+Shift+P 停在 Shift+Win+…。
    P(2, L"STATIC",
      L"Esc 或 Backspace 清除（＝不綁熱鍵）。除了 PrtScn，至少需要一個修飾鍵；"
      L"建議用 Ctrl / Alt / Shift，Win 組合會被系統先接走。\r\n"
      L"被其他程式佔用的組合，會在按確定後提示。",
      SS_LEFT, 0, 172, 430, 54, IDC_HINT);
    P(2, L"STATIC", L"", SS_LEFT, 0, 232, 430, 18, IDC_HK_ERROR);

    // ---- 共用按鈕 ----
    // 放在分頁外面：三頁共用同一組確定／取消，切頁時按鈕不會跳位置。
    g_curPage = -1;
    Child(h, L"BUTTON", L"確定", BS_DEFPUSHBUTTON, 288, kTabY + kTabH + 12, 84, 28, IDOK);
    Child(h, L"BUTTON", L"取消", BS_PUSHBUTTON, 380, kTabY + kTabH + 12, 84, 28, IDCANCEL);

    ShowPage(h, 0);
}

// 切換分頁：只做 Show / Hide，不重建控制項（見 DlgState::page 的說明）。
void ShowPage(HWND h, int page) {
    if (!g_dlg || page < 0 || page >= kPageCount) return;
    g_dlg->curPage = page;
    for (int p = 0; p < kPageCount; ++p) {
        const int cmd = (p == page) ? SW_SHOW : SW_HIDE;
        for (HWND c : g_dlg->page[p]) ShowWindow(c, cmd);
    }
    if (g_dlg->tab) TabCtrl_SetCurSel(g_dlg->tab, page);
    InvalidateRect(h, nullptr, TRUE);
}

// ---- 選資料夾 --------------------------------------------------------------
//
// 為什麼要另開一條 STA 執行緒：
//
// 主執行緒是 MTA（`CoInitializeEx(nullptr, COINIT_MULTITHREADED)`），這是刻意的
// —— `AudioCapture::Open` 在呼叫端執行緒建立 WASAPI 介面，再交給擷取執行緒
// 使用；改成 STA 的話那些介面跨 apartment 就得 marshal，音訊會直接壞掉。
//
// 但 `SHBrowseForFolderW` 帶 `BIF_NEWDIALOGSTYLE` 時需要 STA。在 MTA 呼叫的
// 實際症狀不是回錯誤，而是**整個行程卡死**：對話框永遠不出現、UI 沒有回應、
// CPU 0%，只能砍行程（實測 `Responding = False`，連 #32770 都沒建立）。
//
// 所以：挑資料夾這件事丟到自己的 STA 執行緒，主執行緒只負責等與抽訊息。
struct BrowseJob {
    HWND owner = nullptr;
    std::wstring path;
    bool canceled = true;
};

// 這條路徑的失敗是無聲的（選了資料夾、欄位沒變），留一行日誌。
void LogBrowse(const wchar_t* what, const std::wstring& v) {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring p = std::wstring(dir) + L"\\CapWGC\\browse.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    fwprintf(f, L"%s [%s]\n", what, v.c_str());
    fclose(f);
}

DWORD WINAPI BrowseThreadProc(void* p) {
    BrowseJob* j = static_cast<BrowseJob*>(p);
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        LogBrowse(L"CoInitializeEx failed", std::to_wstring((long)hr));
        return 0;
    }

    BROWSEINFOW bi{};
    bi.hwndOwner = j->owner;
    bi.lpszTitle = L"選擇錄影輸出資料夾";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path)) j->path = path;
        CoTaskMemFree(pidl);
        j->canceled = false;
    }
    LogBrowse(L"browse done", j->path);
    CoUninitialize();
    return 0;
}

void BrowseFolder(HWND h, int editId) {
    BrowseJob job;
    job.owner = h;

    HANDLE th = CreateThread(nullptr, 0, BrowseThreadProc, &job, 0, nullptr);
    if (!th) return;

    // 一邊等一邊抽訊息。直接 WaitForSingleObject 會讓設定視窗在挑選期間變成
    // 「沒有回應」；而且 shell 對 owner 呼叫 EnableWindow 是 SendMessage 過來
    // 的，主執行緒不抽訊息就會互相等死。
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) break;   // 不該發生，但別無限迴圈
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            // 這裡把 WM_QUIT 吃掉的話，外層的 GetMessage 會永遠等不到結束信號。
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                continue;
            }
            if (!IsDialogMessageW(h, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);

    // 抽訊息期間視窗可能已經被關掉（例如使用者按了 Esc），別寫進死掉的 HWND。
    if (!job.path.empty() && IsWindow(h)) {
        const BOOL set = SetDlgItemTextW(h, editId, job.path.c_str());
        LogBrowse(L"write back", job.path + (set ? L" ok" : L" FAILED"));
    }
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        BuildControls(h, *g_dlg->s);
        return 0;
    case WM_NOTIFY: {
        // 分頁切換。注意這是**一般視窗**（不是 dialog），所以 WM_NOTIFY 直接
        // 送到這裡；IsDialogMessageW 不會替我們處理它。
        const NMHDR* nh = reinterpret_cast<const NMHDR*>(l);
        if (nh && nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
            const int sel = TabCtrl_GetCurSel(g_dlg->tab);
            if (sel >= 0) {
                ShowPage(h, sel);
                // 焦點原本在舊頁面的控制項上（例如熱鍵欄位）→ 那個控制項剛被
                // 藏起來，焦點會掉到視窗本身。移到分頁籤上，鍵盤才不會「消失」。
                HWND focus = GetFocus();
                if (focus && !IsWindowVisible(focus)) SetFocus(g_dlg->tab);
            }
        }
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)l == GetDlgItem(h, IDC_QUALITY)) {
            wchar_t b[16];
            swprintf_s(b, L"%d",
                       (int)SendDlgItemMessageW(h, IDC_QUALITY, TBM_GETPOS, 0, 0));
            SetDlgItemTextW(h, IDC_QUALITY_VAL, b);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == GetDlgItem(h, IDC_HK_ERROR)) {
            HDC dc = (HDC)w;
            SetTextColor(dc, RGB(176, 32, 32));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK: {
            ReadBack(h, g_dlg->s);
            // 同一組熱鍵綁兩個動作：第二個一定註冊失敗，而且是在「按了確定
            // 之後」才失敗，使用者會覺得設定沒生效。在這裡先擋。
            std::wstring dup;
            if (g_dlg->s->hotkeys.Duplicate(&dup)) {
                ShowHotkeyError(h, dup + L"綁了同一組熱鍵");
                return 0;
            }
            g_dlg->ok = true;
            DestroyWindow(h);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(h);
            return 0;
        case IDC_DIR_BROWSE:
            BrowseFolder(h, IDC_DIR_EDIT);
            return 0;
        case IDC_SNIP_DIR_BROWSE:
            BrowseFolder(h, IDC_SNIP_DIR);
            return 0;
        case IDC_PRTSN_TAKE: {
            // 勾選框與「截圖 / OCR」那個熱鍵欄位是同一件事的兩種說法，
            // 必須同步 —— 不然使用者會勾了接管、欄位卻還顯示 Ctrl+Alt+X，
            // 然後不知道哪一個才是對的。
            const bool on = IsDlgButtonChecked(h, IDC_PRTSN_TAKE) == BST_CHECKED;
            g_dlg->hk[4] = on ? Hotkey{0, VK_SNAPSHOT} : SnipFallbackHotkey();
            RefreshHotkeyBox(4);
            g_dlg->wantPrtsc = on;

            // 說明文字跟著換：這一段是「我改了你的系統」的告知，寫死一句
            // 會讓取消勾選之後還留著「目前由 CapWGC 接管」。
            HWND note = GetDlgItem(h, IDC_PRTSN_NOTE);
            if (note) {
                SetWindowTextW(note,
                               on ? L"確定後會關閉 Windows 11 的「使用 PrtScn 開啟剪取工具」"
                                    L"（Win+Shift+S 不受影響），並把 PrtScn 綁給截圖 / OCR。"
                                  : L"取消勾選會把 PrtScn 還原給 Windows（原本沒設定過就"
                                    L"維持沒設定），截圖熱鍵回到 Ctrl+Alt+X。");
            }
            return 0;
        }
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_dlg->hwnd = nullptr;
        if (g_dlg->font) { DeleteObject(g_dlg->font); g_dlg->font = nullptr; }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

// ============================================================================
// Hotkey
// ============================================================================

// 注意 MOD_WIN 是**合法**的：RegisterHotKey 支援它，INI 手寫 `Win+Shift+P`
// 也能正常註冊。這裡不擋，是因為擋的是「這個鍵能不能當主鍵」，不是「能不能
// 用 Win 當修飾鍵」。真正的限制在擷取 UI（見 HotkeyProc）：Win 的 keydown
// 會被 shell 先接走，欄位根本收不到後續那個鍵。
bool HotkeyUsable(const Hotkey& hk) {
    if (hk.vk == 0) return false;
    // PrtScn 是「沒有修飾鍵也可以」的唯一例外。理由不是特例優惠，而是它
    // 本來就不是打字鍵：使用者的直覺就是「按一下 PrtScn 截圖」，要求他按
    // Ctrl+PrtScn 反而違背他換掉微軟剪取工具的目的。
    // 其他單鍵仍然拒絕 —— 那會讓整台機器的打字變成災難。
    if (hk.mods == 0 && hk.vk != VK_SNAPSHOT) return false;
    if (hk.vk == VK_F12) return false;
    switch (hk.vk) {
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1:
    case VK_XBUTTON2: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        return false;
    default:
        break;
    }
    return true;
}

std::wstring HotkeyText(const Hotkey& hk) {
    if (hk.vk == 0) return L"（未設定）";
    std::wstring s;
    if (hk.mods & MOD_CONTROL) s += L"Ctrl+";
    if (hk.mods & MOD_ALT) s += L"Alt+";
    if (hk.mods & MOD_SHIFT) s += L"Shift+";
    if (hk.mods & MOD_WIN) s += L"Win+";
    s += VkName(hk.vk);
    return s;
}

bool HotkeyFromText(const std::wstring& text, Hotkey* out) {
    if (!out) return false;
    if (text.empty()) return false;

    std::wstring cur;
    std::vector<std::wstring> parts;
    for (wchar_t c : text) {
        if (c == L'+') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);

    Hotkey hk{};
    bool haveKey = false;
    for (auto& p : parts) {
        while (!p.empty() && iswspace(p.front())) p.erase(p.begin());
        while (!p.empty() && iswspace(p.back())) p.pop_back();
        if (p.empty()) continue;
        if (!_wcsicmp(p.c_str(), L"ctrl") || !_wcsicmp(p.c_str(), L"control")) {
            hk.mods |= MOD_CONTROL;
        } else if (!_wcsicmp(p.c_str(), L"alt")) {
            hk.mods |= MOD_ALT;
        } else if (!_wcsicmp(p.c_str(), L"shift")) {
            hk.mods |= MOD_SHIFT;
        } else if (!_wcsicmp(p.c_str(), L"win") || !_wcsicmp(p.c_str(), L"windows")) {
            hk.mods |= MOD_WIN;
        } else {
            if (haveKey) return false;   // 兩個非修飾鍵 → 不是合法組合
            UINT vk = 0;
            if (!VkFromName(p, &vk)) return false;
            hk.vk = vk;
            haveKey = true;
        }
    }
    if (!haveKey) return false;
    *out = hk;
    return HotkeyUsable(hk);
}

// 預設值刻意只給**兩個**（2026-10-20 改，同日再把截圖換成 PrtScn）：
//   截圖 / OCR、選擇視窗
//
// 理由：錄影是「謹慎行為」—— 佔空間、而且通常是錄給別人看，所以錄影本身
// 不該靠一組記不住的熱鍵觸發。真正天天用、值得佔用一個全域熱鍵的是
// 「看到什麼截什麼」，以及「把主視窗叫出來」。其餘三個留空，需要的人自己
// 在設定裡指定。
//
// 截圖預設＝**PrtScn**（2026-10-20 改，原本 Ctrl+Alt+X）。這是「用 PrtScn 取代
// Windows 剪取工具」預設開啟的落地方式：
//   * 預設值就是 PrtScn → 啟動時看到 snip 是 PrtScn 且還沒接管，就自動接管
//     （見 main.cpp 的 AutoEngagePrtScn）。
//   * **已經自己改過熱鍵的使用者完全不受影響**：ini 裡有 [hotkeys] snip 那一行，
//     Load() 逐鍵覆蓋，值還是他的；接管旗標也還在原本的狀態。
//   * 曾經在設定視窗取消勾選的人，ini 裡是 `snip=Ctrl+Alt+X`，同樣不會被翻回來。
//
// 留空（vk == 0）不是「壞掉」：托盤選單、主視窗、錄影中的漂浮控件都還是
// 完整的入口，只是沒有鍵盤捷徑。
//
// 舊使用者不受影響：Load() 是**逐鍵**覆蓋的，ini 裡寫過的那幾行照舊，
// 只有從來沒寫過（＝沒碰過這個設定）的鍵才會拿到新的預設值。
Hotkeys Hotkeys::Default() {
    Hotkeys k;
    k.picker = Hotkey{MOD_CONTROL | MOD_ALT, 'W'};
    // PrtScn 是唯一允許「沒有修飾鍵」的鍵（見 HotkeyUsable 的說明）：它不參與
    // 打字，而且使用者的直覺就是「按一下 PrtScn 截圖」。
    k.snip = Hotkey{0, VK_SNAPSHOT};
    // region / screen / stop 留空。
    return k;
}

// 關閉 PrtScn 接管之後，截圖要退回哪一組。
//
// 不能寫成 `Hotkeys::Default().snip` —— 預設值現在就是 PrtScn，那樣「取消接管」
// 會把熱鍵指回同一個鍵，等於沒放掉，而且按下去不會有反應（Windows 又拿回去了）。
Hotkey SnipFallbackHotkey() { return Hotkey{MOD_CONTROL | MOD_ALT, 'X'}; }

bool Hotkeys::Duplicate(std::wstring* which) const {
    const Hotkey all[5] = {region, picker, screen, stop, snip};
    const wchar_t* names[5] = {L"選區域", L"視窗清單", L"錄主螢幕", L"停止",
                               L"截圖 / OCR"};
    for (int i = 0; i < 5; ++i) {
        if (!all[i].set()) continue;
        for (int j = i + 1; j < 5; ++j) {
            if (!all[j].set()) continue;
            if (all[i].same(all[j])) {
                if (which) *which = std::wstring(L"「") + names[i] + L"」與「" + names[j] + L"」";
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Settings
// ============================================================================

std::wstring Settings::DefaultOutputDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, 0, buf))) {
        std::wstring p = buf;
        p += L"\\CapWGC";
        return p;
    }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"CapWGC";
}

std::wstring Settings::SnipDir() const {
    // 明確設定過就照用（使用者可能想丟到雲端同步資料夾之類的）。
    if (!snipDir.empty()) return snipDir;
    // 沒設定就跟著輸出資料夾，但不直接混在裡面 —— 見 settings.h 的說明。
    std::wstring base = outputDir;
    if (base.empty()) base = DefaultOutputDir();
    // 去掉結尾的斜線，否則會變成 D:\tmp\\Snip
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    return base + L"\\Snip";
}

std::wstring Settings::IniPath() {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return L"";
    std::wstring p = dir;
    p += L"\\CapWGC";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\settings.ini";
    return p;
}

void Settings::Load() {
    outputDir = DefaultOutputDir();
    hotkeys = Hotkeys::Default();
    const std::wstring ini = IniPath();
    if (ini.empty()) return;

    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"record", L"output_dir", outputDir.c_str(), buf, MAX_PATH,
                             ini.c_str());
    if (buf[0]) outputDir = buf;

    codec = GetPrivateProfileIntW(L"record", L"codec", codec, ini.c_str());
    quality = GetPrivateProfileIntW(L"record", L"quality", quality, ini.c_str());
    fps = GetPrivateProfileIntW(L"record", L"fps", fps, ini.c_str());
    gop = GetPrivateProfileIntW(L"record", L"gop", gop, ini.c_str());
    captureCursor = GetPrivateProfileIntW(L"record", L"cursor", 0, ini.c_str()) != 0;
    audioSystem = GetPrivateProfileIntW(L"record", L"audio_system", 1, ini.c_str()) != 0;
    audioMic = GetPrivateProfileIntW(L"record", L"audio_mic", 0, ini.c_str()) != 0;
    separateTracks =
        GetPrivateProfileIntW(L"record", L"separate_tracks", 0, ini.c_str()) != 0;

    // 截圖 / OCR
    //
    // 注意：這裡**故意不讀** [snip] default_action。舊版會把「上次選了 OCR」
    // 寫進這個鍵，導致之後每次叫出截圖都預設在 OCR（跟「預設截圖」相反），
    // 而使用者完全看不到這個值。現在預設模式由呼叫端明確傳遞，ini 不再參與，
    // 舊 ini 裡殘留的 default_action=1 也就自然失效了。
    // 預設值 = 1（開）。改預設值有個必須注意的地方：**已經有 save_file=0 的
    // 使用者不會被影響** —— 他們真的選過「不要存」，ini 裡有那一行，照舊。
    // 只有從來沒碰過這個設定（ini 裡沒這個鍵）的人才會拿到新的預設 1，而那
    // 正是要改的對象。這也是用 GetPrivateProfileIntW 帶預設值的好處。
    snipSaveFile = GetPrivateProfileIntW(L"snip", L"save_file", 1, ini.c_str()) != 0;

    {
        wchar_t b[MAX_PATH] = {};
        GetPrivateProfileStringW(L"snip", L"dir", L"", b, MAX_PATH, ini.c_str());
        snipDir = b;
    }

    // OCR 只剩「有沒有警告過」要記。模型目錄那個欄位已從設定畫面移除，
    // ini 裡的 model_dir 也不再讀 —— 舊 ini 留著那一行無害，只是被忽略。
    ocrWarned = GetPrivateProfileIntW(L"ocr", L"warned", 0, ini.c_str()) != 0;

    // 手改壞的值不要讓它一路傳到編碼器（quality=0 會被 MFT 當成「未設定」，
    // 反而回退預設值 —— 見 HANDOFF 的坑）。
    if (codec < 0 || codec > 3) codec = 0;
    if (quality < 1) quality = 70;
    if (quality > 100) quality = 100;
    if (fps < 1 || fps > 240) fps = 60;
    if (gop < 1) gop = 2;

    // 控件位置：空字串 = 沒拖過。格式 "dx,dy"，手改得動也看得懂。
    wchar_t pos[64] = {};
    GetPrivateProfileStringW(L"ui", L"bar_pos", L"", pos, 64, ini.c_str());
    if (pos[0]) {
        int dx = 0, dy = 0;
        if (swscanf_s(pos, L"%d,%d", &dx, &dy) == 2) {
            hasBarPos = true;
            barDx = dx;
            barDy = dy;
        }
    }

    // 熱鍵：解析不出來就整條退回預設（保留半套值等於偷偷換掉使用者的設定）。
    struct HkSlot {
        const wchar_t* key;
        Hotkey* dst;
        Hotkey dflt;
    };
    // 熱鍵的讀取要分得出三種狀態，這是「預設有鍵是空的」之後才浮出來的問題：
    //
    //   1. ini 裡**沒有這個鍵**  → 從來沒碰過 → 套用新的預設值
    //   2. ini 裡是**空字串**    → 使用者明確清空（或在設定視窗清掉）→ 保持不綁
    //   3. ini 裡有字            → 解析，失敗才退回預設
    //
    // 只有 GetPrivateProfileStringW 的預設值是分不出 1 和 2 的（兩者都回空的
    // 內容），所以傳一個哨兵字串當「沒這個鍵」的記號 —— 真實值不可能是它。
    constexpr const wchar_t* kAbsent = L"\x0001";
    const Hotkeys d = Hotkeys::Default();
    const HkSlot slots[5] = {
        {L"region", &hotkeys.region, d.region},
        {L"picker", &hotkeys.picker, d.picker},
        {L"screen", &hotkeys.screen, d.screen},
        {L"stop", &hotkeys.stop, d.stop},
        {L"snip", &hotkeys.snip, d.snip},
    };
    for (const auto& s : slots) {
        wchar_t text[128] = {};
        GetPrivateProfileStringW(L"hotkeys", s.key, kAbsent, text, 128, ini.c_str());
        if (!wcscmp(text, kAbsent)) continue;   // 狀態 1：沒這個鍵 → 用預設
        if (!text[0]) {                         // 狀態 2：明確留空
            *s.dst = Hotkey{};
            continue;
        }
        Hotkey hk;
        if (HotkeyFromText(text, &hk)) *s.dst = hk;
        else *s.dst = s.dflt;                   // 狀態 3 但解析失敗
    }
}

void Settings::Save() const {
    const std::wstring ini = IniPath();
    if (ini.empty()) return;
    wchar_t buf[32];

    WritePrivateProfileStringW(L"record", L"output_dir", outputDir.c_str(), ini.c_str());
    swprintf_s(buf, L"%d", codec);
    WritePrivateProfileStringW(L"record", L"codec", buf, ini.c_str());
    swprintf_s(buf, L"%d", quality);
    WritePrivateProfileStringW(L"record", L"quality", buf, ini.c_str());
    swprintf_s(buf, L"%d", fps);
    WritePrivateProfileStringW(L"record", L"fps", buf, ini.c_str());
    swprintf_s(buf, L"%d", gop);
    WritePrivateProfileStringW(L"record", L"gop", buf, ini.c_str());
    WritePrivateProfileStringW(L"record", L"cursor", captureCursor ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"record", L"audio_system", audioSystem ? L"1" : L"0",
                               ini.c_str());
    WritePrivateProfileStringW(L"record", L"audio_mic", audioMic ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"record", L"separate_tracks",
                               separateTracks ? L"1" : L"0", ini.c_str());

    WritePrivateProfileStringW(L"snip", L"save_file", snipSaveFile ? L"1" : L"0",
                               ini.c_str());
    // 空字串要寫掉（＝跟著 outputDir 自動決定），否則清空路徑之後還會留著舊值。
    WritePrivateProfileStringW(L"snip", L"dir", snipDir.empty() ? nullptr : snipDir.c_str(),
                               ini.c_str());
    WritePrivateProfileStringW(L"ocr", L"warned", ocrWarned ? L"1" : nullptr,
                               ini.c_str());

    if (hasBarPos) {
        wchar_t pos[64];
        swprintf_s(pos, L"%d,%d", barDx, barDy);
        WritePrivateProfileStringW(L"ui", L"bar_pos", pos, ini.c_str());
    } else {
        WritePrivateProfileStringW(L"ui", L"bar_pos", nullptr, ini.c_str());
    }

    // 熱鍵寫成「Ctrl+Alt+A」：手改得動，改壞了下一次啟動看得出來是哪裡壞。
    //
    // **未設定要寫「空字串」，不能寫 HotkeyText()**。HotkeyText 對 vk==0 回
    // 「（未設定）」—— 那是給人看的，寫進 ini 之後 Load() 會把它當成「有值
    // 但解析失敗」，於是退回預設值。症狀是「我把熱鍵清空了，重開程式它又
    // 回來了」。現在預設有三個是空的，這條路會變成常態，所以必須修。
    // 寫 nullptr（＝刪掉這個鍵）也一樣達不到「明確留空」的語意，因為 Load()
    // 對「沒有這個鍵」的處理就是套預設值 —— 所以我們寫空字串，並在 Load()
    // 把「空字串」明確讀成「不要這個熱鍵」。
    auto HkOut = [&](const wchar_t* key, const Hotkey& hk) {
        if (!hk.set()) {
            WritePrivateProfileStringW(L"hotkeys", key, L"", ini.c_str());
        } else {
            WritePrivateProfileStringW(L"hotkeys", key, HotkeyText(hk).c_str(),
                                       ini.c_str());
        }
    };
    HkOut(L"region", hotkeys.region);
    HkOut(L"picker", hotkeys.picker);
    HkOut(L"screen", hotkeys.screen);
    HkOut(L"stop", hotkeys.stop);
    HkOut(L"snip", hotkeys.snip);
    // 立刻落盤，避免程式被 kill 時設定不見
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

// ============================================================================
// PrtScn 接管
// ============================================================================
//
// 這裡的每一個判斷都來自實測，不是文件。實測方法見
// `build.bat prtsctest` → `out\prtsc_probe.exe --inject / --control`：
//
//   reg.state=absent   hotkey.register=1  hotkey.received=0   ← 值不存在＝開
//   reg.state=disabled hotkey.register=1  hotkey.received=1   ← 設 0 才拿得到
//   （對照組 Ctrl+Alt+W 同樣流程 received=1，證明「收不到」不是量測方法錯）
//
// 兩個反直覺的點，都寫進程式碼的理由：
//
// 1. **註冊成功不代表拿得到**。RegisterHotKey 回 TRUE，但 WM_HOTKEY 永遠不來。
//    所以「熱鍵註冊失敗就回報使用者」這條路在這裡完全不會觸發 —— 必須另外
//    判斷「註冊了卻沒反應」。這也是為什麼接管開關不能做成「默默失敗」。
//
// 2. **「值不存在」≠「值等於 1」**。對系統來說兩者都開剪取工具，但還原時
//    不能留一個我們寫進去的 1 在使用者機器上。

namespace {

constexpr const wchar_t* kPrtscKey = L"Control Panel\\Keyboard";
constexpr const wchar_t* kPrtscValue = L"PrintScreenKeyForSnippingEnabled";

// 改登錄值。absent = 刪掉這個值（還原成「使用者從來沒設過」）。
bool WritePrtsc(DWORD value, bool absent) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPrtscKey, 0, KEY_SET_VALUE, &k) !=
        ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st;
    if (absent) {
        st = RegDeleteValueW(k, kPrtscValue);
        // 本來就沒有 → 視為成功（要達成的狀態已經成立）
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    } else {
        st = RegSetValueExW(k, kPrtscValue, 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

bool ReadPrtsc(DWORD* out, bool* absent) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPrtscKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD val = 0, cb = sizeof(val), type = 0;
    const LSTATUS st = RegQueryValueExW(k, kPrtscValue, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&val), &cb);
    RegCloseKey(k);
    if (st == ERROR_FILE_NOT_FOUND) {
        *absent = true;
        return true;
    }
    if (st != ERROR_SUCCESS) return false;
    // 型別不是 DWORD 就不要自作聰明改它 —— 那是別人的資料。
    if (type != REG_DWORD) return false;
    *absent = false;
    *out = val;
    return true;
}

} // namespace

PrtScnState PrtScnQuery() {
    DWORD v = 0;
    bool absent = false;
    if (!ReadPrtsc(&v, &absent)) return PrtScnState::Error;
    if (absent) return PrtScnState::Absent;
    return v ? PrtScnState::Enabled : PrtScnState::Disabled;
}

bool PrtScnTake() {
    // 記下「原本是什麼」再改。記在 ini 裡而不是記憶體，因為使用者可能在
    // 接管期間把程式關掉（當機、登出），下次啟動要能還原。
    const PrtScnState before = PrtScnQuery();
    if (before == PrtScnState::Error) return false;

    if (!WritePrtsc(0, false)) return false;

    // 只有「原本不存在」需要特別記 —— 這是還原時唯一會做錯的狀態。
    PrtScnSetOwned(true);
    PrtScnRememberAbsent(before == PrtScnState::Absent);
    return true;
}

bool PrtScnGiveBack() {
    // 還原優先看「我們改之前它是什麼」，而不是現在它是什麼。現在一定是 0
    // （我們改的），照現在的值還原等於什麼都沒做。
    const bool wasAbsent = PrtScnWasAbsent();
    PrtScnSetOwned(false);
    return WritePrtsc(1, wasAbsent);
}

// ---- ini 裡的兩個記號 ------------------------------------------------------
//
// 放在 [prtsc] 而不是放進 Settings struct：這兩個值描述的是「這台機器的
// 系統設定現在被誰動過」，不是使用者的偏好。把它們混進 Settings 會讓
// 設定視窗的複本／套用流程有機會把它們一起回滾掉 —— 那會讓還原資訊消失，
// 使用者的 PrtScn 就永遠回不去。
namespace {
bool ReadFlag(const wchar_t* key) {
    const std::wstring ini = Settings::IniPath();
    if (ini.empty()) return false;
    return GetPrivateProfileIntW(L"prtsc", key, 0, ini.c_str()) != 0;
}
void WriteFlag(const wchar_t* key, bool v) {
    const std::wstring ini = Settings::IniPath();
    if (ini.empty()) return;
    WritePrivateProfileStringW(L"prtsc", key, v ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}
} // namespace

bool PrtScnOwned() { return ReadFlag(L"owned"); }
void PrtScnSetOwned(bool owned) { WriteFlag(L"owned", owned); }
bool PrtScnWasAbsent() { return ReadFlag(L"was_absent"); }
void PrtScnRememberAbsent(bool absent) { WriteFlag(L"was_absent", absent); }
bool PrtScnChosen() { return ReadFlag(L"chosen"); }
void PrtScnSetChosen(bool chosen) { WriteFlag(L"chosen", chosen); }

bool ShowSettingsDialog(HWND owner, Settings* s, bool* wantPrtsc) {
    if (!s) return false;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    // ICC_TAB_CLASSES 是分頁控制項（SysTabControl32）需要的 —— 少了它
    // CreateWindowExW 會回 null，而且錯誤碼只會是「找不到視窗類別」。
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CapWGCSettings";
    RegisterClassExW(&wc);

    DlgState st;
    st.s = s;
    g_dlg = &st;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    // 客戶區大小：分頁籤 12,12 起、452×300，按鈕在 y=324 高 28（→352），
    // 下緣留 12。改成三分頁之後高度從 720 掉到 364 —— 這是分頁唯一的實際好處：
    // 使用者不必再把視窗拖高才看得到下面的欄位。
    RECT rc{0, 0, 476, 364};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // ---- 定位 ----
    //
    // 「以 owner 為中心」在這裡是陷阱：本程式的 owner 是托盤用的隱藏訊息窗，
    // 大小是 0×0、位置在 (0,0)。置中算出來就是 (-w/2, -h/2)，標題列整個跑到
    // 螢幕左上角外面，而且因為沒有工作列按鈕，使用者根本拖不回來。
    //
    // 所以：owner 有實際大小才用它置中，否則退到「滑鼠所在螢幕的工作區」
    // （托盤程式該有的行為），最後不論走哪條路都再夾一次工作區。
    RECT wa{};
    HMONITOR mon = nullptr;
    if (owner && IsWindow(owner)) {
        RECT orc{};
        GetWindowRect(owner, &orc);
        const int ow = orc.right - orc.left;
        const int oh = orc.bottom - orc.top;
        if (ow >= 64 && oh >= 64) {
            wa = orc;
            mon = MonitorFromRect(&orc, MONITOR_DEFAULTTONEAREST);
        }
    }
    if (!mon) {
        POINT cur{};
        GetCursorPos(&cur);
        mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    if (IsRectEmpty(&work)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    RECT target = IsRectEmpty(&wa) ? work : wa;
    int x = target.left + ((target.right - target.left) - w) / 2;
    int y = target.top + ((target.bottom - target.top) - h) / 2;

    // 夾回工作區：寬高塞得下就整個推進畫面內，塞不下就貼左上（至少標題列
    // 與按鈕看得到）。這一步同時保護「owner 只露出半個」的多螢幕情況。
    if (w <= (work.right - work.left)) {
        if (x + w > work.right) x = work.right - w;
        if (x < work.left) x = work.left;
    } else {
        x = work.left;
    }
    if (h <= (work.bottom - work.top)) {
        if (y + h > work.bottom) y = work.bottom - h;
    }
    if (y < work.top) y = work.top;   // 標題列一定要在工作區內，否則拖不動

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOPMOST, L"CapWGCSettings",
                                L"CapWGC 設定", style, x, y, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        g_dlg = nullptr;
        return false;
    }
    st.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    const bool ok = st.ok;
    if (ok && wantPrtsc) *wantPrtsc = st.wantPrtsc;
    g_dlg = nullptr;
    return ok;
}

} // namespace app
