// ============================================================================
// autostart.cpp — 開機自動啟動的實作
// ============================================================================
//
// 實作刻意短：這裡真正的難點不是 API，而是**哪個狀態該怎麼解讀**（見 .h）。
// 每次改動前先看一次那張表，尤其是 Blocked —— 它是「預設開啟」唯一會讓使用者
// 覺得「這程式不聽話」的地方。
// ============================================================================

#include "autostart.h"

#include "settings.h"   // Settings::IniPath（記號存在同一個 ini 裡）

#include <windows.h>

#include <vector>

namespace app {
namespace autostart {
namespace {

constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Windows 的「啟動應用程式」開關（工作管理員 / 設定 → 應用程式 → 啟動）。
//
// 這個機碼才是 Explorer 真正看的地方：Run 裡有值、但這裡標成停用，登入時
// 仍然不會啟動。它是一個 REG_BINARY，第一個 byte 是狀態：
//   0x02 / 0x06 = 啟用（06 是使用者自己在設定裡打開的）
//   0x03        = 停用（使用者關掉的）
// 沒有這個值 = 從來沒被關過 = 啟用。
constexpr const wchar_t* kApprovedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// 從命令列字串取出執行檔路徑。值可能是 `"C:\a b\x.exe" --flag`、也可能是
// 沒有引號的 `C:\x.exe`（早期或其他工具寫的），兩種都要吃。
std::wstring CommandTarget(const std::wstring& cmd) {
    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i])) ++i;
    if (i >= cmd.size()) return std::wstring();
    if (cmd[i] == L'"') {
        const size_t end = cmd.find(L'"', i + 1);
        if (end == std::wstring::npos) return cmd.substr(i + 1);
        return cmd.substr(i + 1, end - i - 1);
    }
    return cmd.substr(i);
}

// 展開 REG_EXPAND_SZ 的 %VAR%（有些工具會寫 `%LOCALAPPDATA%\...`）。
// REG_SZ 也可能含 %，一併展開沒有副作用。
std::wstring Expand(const std::wstring& s) {
    if (s.empty() || s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n > sizeof(buf) / sizeof(buf[0])) return s;
    return std::wstring(buf, n - 1);
}

// 正規化成可以直接比對的完整路徑。
//
// **不能只做字串比較**：註冊表裡可能是 8.3 短名（PROGRA~1）、可能用相對路徑、
// 也可能是 / 分隔。GetFullPathNameW 解掉後面兩種，短名要靠 GetLongPathNameW
// —— 那需要檔案存在，取不到就原樣留著（下面還能用大小寫不敏感比對擋掉大半）。
std::wstring Normalize(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD n = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full,
                                      nullptr);
    std::wstring p = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? std::wstring(full) : path;
    wchar_t longPath[MAX_PATH * 2] = {};
    const DWORD m = GetLongPathNameW(p.c_str(), longPath,
                                     (DWORD)(sizeof(longPath) / sizeof(longPath[0])));
    if (m > 0 && m < sizeof(longPath) / sizeof(longPath[0])) p.assign(longPath, m);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(Normalize(a).c_str(), Normalize(b).c_str()) == 0;
}

// 讀 Run 的值。回傳 false = 沒有這個值（或機碼讀不到）。
bool ReadRunValue(std::wstring* out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, cb = 0;
    LSTATUS st = RegQueryValueExW(k, kValueName, nullptr, &type, nullptr, &cb);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb == 0) {
        RegCloseKey(k);
        return false;
    }
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, L'\0');
    DWORD cb2 = cb;
    st = RegQueryValueExW(k, kValueName, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &cb2);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS) return false;
    // 值不一定有結尾 0（有些程式寫的沒有）—— 自己收斂。
    size_t len = 0;
    while (len < buf.size() && buf[len]) ++len;
    *out = std::wstring(buf.data(), len);
    return true;
}

// 「啟動應用程式」那個開關被關了嗎。
bool IsBlockedInTaskManager() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;   // 沒有這個機碼 = 沒被關過
    BYTE data[16] = {};
    DWORD cb = sizeof(data), type = 0;
    const LSTATUS st =
        RegQueryValueExW(k, kValueName, nullptr, &type, data, &cb);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || type != REG_BINARY || cb == 0) return false;
    // 第一個 byte：2 / 6 = 啟用，3 = 停用。其他值（含 0）一律當成啟用 ——
    // 猜錯成「停用」會讓我們把使用者要的東西關掉，代價更大。
    return data[0] == 0x03;
}

// 把「啟動應用程式」那個開關打開（使用者在我們的設定裡勾了 → 他要它開）。
//
// 不做的話會出現最惡毒的失敗：Run 值寫進去了、勾選框也顯示已啟用，但登入後
// 什麼都沒發生 —— 因為 Windows 還記著「這個項目停用」。而且從我們的介面取消
// 再勾選，完全看不出為什麼沒用。
void ClearBlock() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    BYTE on[12] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RegSetValueExW(k, kValueName, 0, REG_BINARY, on, sizeof(on));
    RegCloseKey(k);
}

// ---- ini 記號 --------------------------------------------------------------
//
// 放 [startup] 而不是塞進 Settings struct：這個記號描述的是「使用者對開機啟動
// 做過決定」，不是錄影偏好；混進 Settings 會跟著設定視窗的複本／取消流程一起
// 回滾，變成「按了取消，但記號已經寫了」。
bool ReadFlag(const wchar_t* key) {
    const std::wstring ini = Settings::IniPath();
    if (ini.empty()) return false;
    return GetPrivateProfileIntW(L"startup", key, 0, ini.c_str()) != 0;
}

void WriteFlag(const wchar_t* key, bool v) {
    const std::wstring ini = Settings::IniPath();
    if (ini.empty()) return;
    WritePrivateProfileStringW(L"startup", key, v ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

} // namespace

const wchar_t* const kValueName = L"CapWGC";

std::wstring ExePath() {
    // 唯一來源：GetModuleFileNameW。
    //
    // 不要換成 GetCommandLineW 或 argv[0]（見 .h 的說明）：那是「別人怎麼啟動
    // 我」，不是「我在哪裡」。從 8.3 短名、相對路徑、被別的程式帶參數啟動時，
    // 寫進註冊表的就是一個登入後解不開的路徑 —— 而且失敗是無聲的。
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring Command() {
    const std::wstring exe = ExePath();
    if (exe.empty()) return std::wstring();
    // 一定要引號：Program Files、含空格的資料夾、中文路徑。
    return L"\"" + exe + L"\"";
}

State Query() {
    std::wstring value;
    if (!ReadRunValue(&value)) return State::NotSet;

    const std::wstring target = Expand(CommandTarget(value));
    if (target.empty()) return State::Stale;

    // 指向別的檔案 → Stale。這裡不看「那個檔案在不在」：舊版本目錄被清掉的
    // 情況與換了目錄但舊檔還在的情況，對我們來說都是「開機不會啟動這支」。
    if (!SamePath(target, ExePath())) return State::Stale;

    if (IsBlockedInTaskManager()) return State::Blocked;
    return State::Enabled;
}

bool Enable() {
    const std::wstring cmd = Command();
    if (cmd.empty()) return false;

    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS st = RegSetValueExW(
        k, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (st != ERROR_SUCCESS) return false;

    // 寫完再解掉 Windows 那邊的停用記號。順序無所謂，但要**兩件都做**。
    ClearBlock();
    return true;
}

bool Disable() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kValueName);   // 本來就沒有 → 忽略
        RegCloseKey(k);
    }
    // 連「啟動應用程式」那筆一起刪。留著的話，使用者下次重新勾選時只會看到
    // 一個停用的殘留項；而且 Stale 的判斷會多一種無關的來源。
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &k) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(k, kValueName);
        RegCloseKey(k);
    }
    return true;
}

bool Chosen() { return ReadFlag(L"chosen"); }
void SetChosen(bool chosen) { WriteFlag(L"chosen", chosen); }

bool IsEnabled() { return Query() == State::Enabled; }

bool Apply(bool want) {
    // 「意圖」和「現況」一樣就什麼都不做。
    //
    // 不這樣做的話，每次按確定都會重寫一次 Run 值 —— 使用者在登錄檔裡手動
    // 加的參數（例如 --minimized）會被無聲蓋掉。而且寫入次數變多也更容易
    // 撞到防毒軟體的登錄監控。
    //
    // 注意比的是 IsEnabled() 而不是「有值」：Stale（指向舊目錄）在使用者眼裡
    // 就是「沒有啟用」，勾了就該修成正確路徑。
    if (want == IsEnabled()) return true;

    if (!want) {
        Disable();
        SetChosen(true);
        return true;
    }
    if (!Enable()) return false;
    SetChosen(true);
    return true;
}

void EnsureDefaultOn() {
    const State st = Query();
    const bool asked = Chosen();

    if (st == State::Blocked) {
        // 使用者在 Windows 設定裡明確停用了。就算還沒「問過」也不要翻回來 ——
        // 「預設開啟」講的是預設值，不是「每次開機都搶一次」。
        SetChosen(true);
        return;
    }
    if (st == State::Enabled) {
        SetChosen(true);
        return;
    }
    if (st == State::Stale) {
        // 換了目錄（開發版 out\ ↔ 打包版 dist\、或解壓到新資料夾）。不論問過
        // 沒都要修：那是路徑過期，不是使用者的選擇。
        if (Enable()) SetChosen(true);
        return;
    }

    // NotSet。只有「沒問過」才套預設（見 .h 的說明）。
    if (asked) return;
    if (Enable()) SetChosen(true);
}

} // namespace autostart
} // namespace app
