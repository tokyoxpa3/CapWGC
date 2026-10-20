// ============================================================================
// settings.h — 設定持久化與設定視窗
// ============================================================================
//
// 用 INI 而不是 JSON：這裡只有十來個純量，INI 一行一個、可以手改、不用寫
// 解析器（寫一個能正確處理跳脫與編碼的 JSON parser 是 150 行起跳，換來的是
// 零好處）。Windows 原生 API 就是 GetPrivateProfileStringW。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX

#endif

#include <windows.h>
#include <string>

namespace app {

// PrtScn 目前是誰的。`Absent` 很重要：**「值不存在」和「等於 1」在系統上看起來
// 一樣（都是開剪取工具），但還原時不能混為一談** —— 使用者原本沒這個值，我們
// 就不該留一個 1 在他機器上。
enum class PrtScnState { Absent, Disabled, Enabled, Error };

// ---- 熱鍵 ------------------------------------------------------------------
//
// 存成「修飾鍵 + 虛擬鍵碼」，寫進 INI 時轉成人看的字串（`Ctrl+Alt+A`）：
// 手改得動、改壞了也看得出哪裡錯。`vk == 0` 代表這個動作不綁熱鍵。
struct Hotkey {
    UINT mods = 0;   // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    UINT vk = 0;     // 0 = 未設定

    bool set() const { return vk != 0; }
    bool same(const Hotkey& o) const { return mods == o.mods && vk == o.vk; }
};

// 至少要有一個修飾鍵（純字母鍵會被 RegisterHotKey 接受，但之後打字就廢了）。
// **唯一例外是 PrtScn**：它天生不帶修飾鍵，而且不參與打字（見 .cpp 的說明）。
bool HotkeyUsable(const Hotkey& hk);

// "Ctrl+Alt+A" / "（未設定）"
std::wstring HotkeyText(const Hotkey& hk);

// 解析 "Ctrl+Alt+A"。大小寫、空白、`Control`/`Ctrl`、`Win`/`Windows` 都吃。
// 解析不出來就回 false（呼叫端保留原值，不要留半套狀態）。
bool HotkeyFromText(const std::wstring& s, Hotkey* out);

// 關閉 PrtScn 接管時，截圖要退回的那一組（Ctrl+Alt+X）。
//
// 不能直接用 Hotkeys::Default().snip —— 預設值本身就是 PrtScn，那樣「取消
// 接管」會把熱鍵指回同一個鍵，等於沒放掉。
Hotkey SnipFallbackHotkey();

struct Hotkeys {
    Hotkey region;   // 拖曳框選 / 單擊選視窗（錄影）
    Hotkey picker;   // 視窗清單（縮圖牆）
    Hotkey screen;   // 錄主螢幕
    Hotkey stop;     // 停止
    // 截圖 / OCR。跟 region 是兩條不同的路：region 選完就開始錄影，snip 選完
    // 是「進剪貼簿」或「送 OCR」—— 使用者的意圖完全不同（看東西看到一半想截
    // 一張，跟想錄一段是兩件事），所以刻意分成兩個熱鍵而不是一個帶模式的。
    Hotkey snip;

    static Hotkeys Default();
    // 兩個動作綁同一組 → 第二個一定註冊失敗，在設定視窗就先擋掉。
    bool Duplicate(std::wstring* which) const;
};

struct Settings {
    std::wstring outputDir;

    // 0 = 自動（AV1 > HEVC > H.264），1 = AV1，2 = HEVC，3 = H.264
    int codec = 0;
    int quality = 70;
    int fps = 60;
    int gop = 2;
    bool captureCursor = false;
    bool audioSystem = true;
    bool audioMic = false;
    // 除了混音軌之外，每個音源再各留一條（給剪輯用）。那些分軌會標成
    // 「不要預設播」，所以一般播放器仍然只播混音軌，不會出現重疊的人聲。
    bool separateTracks = false;

    // ---- 截圖 / OCR ----
    //
    // 進入截圖覆蓋層時的預設動作**不放在這裡**：它由 BeginSnip(a, wantOcr)
    // 的參數明確傳入，一般熱鍵一律「截圖」。
    // 曾經這裡有一個 snipDefaultAction 並寫回 ini，結果是「用過一次 OCR 之後
    // 每次進來都預設 OCR」，而且使用者看不到那個值 —— 已移除。
    //
    // 截圖要不要順便存一份到 SnipDir()（除了進剪貼簿之外）。
    //
    // **預設開**（2026-10-20 改）。原本是關，理由是「只是要貼到別的地方，
    // 硬存檔會積一堆檔案」。改的主因是檔案總管貼上：CF_HDROP 帶的是檔名，
    // 磁碟上沒有那個檔案就貼不出東西 —— 也就是說「能貼到資料夾」這個功能
    // 本身就依賴存檔。既然存了，就該讓使用者找得到，不再是一個隱形副作用。
    bool snipSaveFile = true;

    // 圖片專用資料夾（截圖 PNG）。
    //
    // 刻意跟影片分開：outputDir 裡會混著 .mp4 與 .png，兩者的用途與保留期
    // 完全不同（截圖是一時的、影片可能留著）。分開之後使用者可以直接把
    // 整個 Snip 資料夾清掉，不會誤刪影片。
    //
    // 空字串 = 用 DefaultSnipDir()（outputDir 底下的 Snip\）。
    std::wstring snipDir;

    // OCR 模型目錄**不再開放設定**。這裡刻意不留欄位：模型固定放在 exe
    // 旁邊的 models\（見 ocr.cpp 的 FindModelsDir），換版本是換檔案、不是
    // 改路徑。舊 ini 裡的 model_dir 會被忽略。

    // 「已經跟使用者講過這台機器沒裝 OCR 後端」。
    //
    // 為什麼要記這個：OCR 起不來時程式會在啟動時彈托盤氣泡。但絕大多數人
    // 只是要錄影／截圖，根本不用 OCR —— 每次開機都被同一個警告打擾，比
    // 沒有 OCR 這件事本身更煩。所以「後端不存在」這種**不會自己好轉**的
    // 失敗只講一次；其他失敗（程序 crash、模型載入失敗）照樣每次都講。
    bool ocrWarned = false;

    // 錄製中漂浮控件的位置：**相對擷取範圍右下角**的位移（像素）。
    // 存相對值而不是絕對座標，換螢幕或改解析度後才會落在同一個角落 ——
    // 絕對座標在 1600×900 上拖到角落，換到 1920×1080 就會離邊界很遠。
    // hasBarPos == false = 使用者還沒拖過，用預設位置（右下角外側）。
    bool hasBarPos = false;
    int barDx = 0;
    int barDy = 0;

    Hotkeys hotkeys = Hotkeys::Default();

    // ---- 介面語言 ----
    //
    // 空字串 = 還沒選過，啟動時用系統語言（見 i18n::Detect）。第一次存檔
    // 之後 ini 就會有明確的值 —— 使用者換了系統語言但不換這個欄位時，
    // 介面不該跟著跳。
    std::wstring lang;

    static std::wstring DefaultOutputDir();

    // 截圖 PNG 的落地目錄。snipDir 有設就用它，否則 outputDir 底下的 Snip\。
    //
    // 做成函式而不是在載入時就把值填好：outputDir 會被使用者在設定視窗裡
    // 改掉，若 DefaultSnipDir 的結果在載入時就固定下來，改完輸出資料夾之後
    // 截圖還是存到舊位置 —— 而且因為路徑已經有值，看起來完全正常。
    std::wstring SnipDir() const;

    static std::wstring IniPath();

    void Load();
    void Save() const;
};

// ---- PrtScn 接管 -----------------------------------------------------------
//
// Windows 11 預設讓 PrtScn 開啟「剪取工具」。那條路徑會**搶在 RegisterHotKey
// 前面**處理按鍵，所以行為很反直覺：
//
//   RegisterHotKey(VK_SNAPSHOT) 回 **成功**，但 WM_HOTKEY 永遠不會來。
//
// （實測：對照組 Ctrl+Alt+W 註冊後收得到 WM_HOTKEY，PrtScn 註冊成功卻收不到；
// 把下面那個登錄值改成 0 之後同一個程式立刻收得到，改回去又收不到。見
// `build.bat prtsctest` → `out\prtsc_probe.exe --inject`。）
//
// 所以「把截圖綁到 PrtScn」在這台機器上不是改一個熱鍵就結束的事，它必然
// 牽涉到改使用者的系統設定。因此：
//   - 不靜默改。改之前要使用者明確同意。
//   - 改之前記下原值，關掉接管時**還原成原本的樣子**（原本「不存在」就刪掉
//     那個值，不要硬寫 1 —— 那兩種狀態對系統來說不等價）。
PrtScnState PrtScnQuery();

// 要 PrtScn 只給 CapWGC 用：登錄值設 0。回傳 false 表示寫入失敗（權限／
// 群組原則），呼叫端要把原因講出來，不能只說「失敗」。
bool PrtScnTake();
// 還原成「PrtScn 開剪取工具」。還原我們改過的那個值。
bool PrtScnGiveBack();
// 這台機器上一次是不是我們改的（記在 ini 裡）。程式被關掉再打開時要靠它
// 決定「關閉接管」要還原到什麼狀態。
bool PrtScnOwned();
void PrtScnSetOwned(bool owned);

// 我們動手之前，那個機碼原本是不是**不存在**。這兩種狀態對系統來說看起來
// 一樣，但還原時不能混：原本沒有就不該留一個 1 在使用者機器上。
// 只在 PrtScnOwned() 為真時有意義。
bool PrtScnWasAbsent();
void PrtScnRememberAbsent(bool absent);

// 使用者是否已經對「PrtScn 接管」做過**明確的決定**（勾選或取消勾選）。
//
// 為什麼需要這個旗標：「預設開啟」聽起來只是一個預設值，但實際上它要動系統
// 設定，而舊版的預設值是 Ctrl+Alt+X。只看 ini 有沒有值分不出兩件事：
//   * 使用者從來沒碰過（ini 是舊版寫的 Ctrl+Alt+X）→ 應該遷移到 PrtScn
//   * 使用者刻意把截圖鍵設成 Ctrl+Alt+X → 不該被翻掉
// 所以另外記一個「問過了」的記號：沒有這個記號＝還沒問過，可以遷移。
bool PrtScnChosen();
void PrtScnSetChosen(bool chosen);

// 原生控制項的設定視窗，modal。回傳 true = 使用者按了「確定」（並已寫回 *s）。
//
// wantPrtsc（可空）：使用者勾了「用 PrtScn 取代 Windows 剪取工具」。
// **這裡不回傳「已接管」，只回傳「他要什麼」** —— 真正的登錄檔改動由呼叫端
// 執行，因為那需要先註冊熱鍵、失敗要能回滾，不是一個 UI 函式該做的事。
bool ShowSettingsDialog(HWND owner, Settings* s, bool* wantPrtsc = nullptr);

} // namespace app
