// ============================================================================
// i18n.h — 介面文字的多語系
// ============================================================================
//
// 規則刻意跟 4Games\NetRedirector 一致：
//
//   * **以「繁體中文原文」當 key**，不是 menu.settings 這種符號 key。
//     好處是程式碼裡看得到實際顯示的字，少一層對照；缺翻譯時直接顯示原文，
//     不會出現空白或 "menu.settings"。語系檔是平的 JSON 物件：
//
//         { "設定…": "Settings…", "取消": "Cancel" }
//
//   * 語言清單與**順序**比照 NetRedirector（見 .cpp 的 kLangs），選單直接
//     照這個順序排。名稱用該語言自己的寫法（繁體中文 / English / 日本語…），
//     這樣使用者不必先看得懂目前語言才找得到自己的語言。
//
// 為什麼不做資源檔（.rc / MUI）：建置流程確實有 rc.exe 了（圖示要走 PE 資源，
// 見 build\app.rc），但字串表放進 .rc 的話，改一個字要重編，翻譯的人也看不
// 懂 —— 這支程式的語系檔是可以直接丟給人改的。多一個字串表格式換不到什麼。
//
// 語系檔位置：exe 旁邊的 locale\<code>.json（打包時一起帶）。找不到檔案就
// 全部顯示原文 —— 這是刻意的降級，不是錯誤。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdarg>
#include <string>

namespace app {
namespace i18n {

// 一個支援的語言。name 是「用該語言自稱」的名字。
struct Lang {
    const wchar_t* code;
    const wchar_t* name;
};

// 全部語言，順序＝設定視窗下拉選單的順序（比照 NetRedirector）。
const Lang* Languages();
int LanguageCount();

// 找不到就回 nullptr / -1。
const Lang* Find(const wchar_t* code);
int IndexOf(const wchar_t* code);
const wchar_t* NameOf(const wchar_t* code);

// 系統介面語言 → 支援的代碼。認不出來回 en_US（不是 zh_TW：這支程式會
// 流傳到別人的機器上，英文是比較安全的預設）。
const wchar_t* Detect();

// 目前生效的代碼。載入前是 zh_TW（＝原文）。
const wchar_t* Current();

// 載入語系。code 為空或不在清單裡 → 用 Detect()。
// 回傳 false 只代表「檔案沒讀到／格式壞掉」，介面仍然是可用的原文。
bool Load(const wchar_t* code);

// 讀 ini 的 [ui] lang；沒有這個鍵就用系統語言。程式啟動時呼叫一次。
// （ini 路徑由 settings.cpp 決定，這裡直接問它，避免兩邊各寫一份。）
void LoadFromIni();

// 翻譯一條字串。查不到就回傳 src 本身（指標有效期限＝程式壽命）。
const wchar_t* T(const wchar_t* src);
// 給 std::wstring 用的版本（例如要接在別的 wstring 後面）。
std::wstring Ts(const std::wstring& src);

// 帶參數的介面字串。**整句**進語系檔（"%s 選區域錄影"），不要在程式裡把句子
// 切成片段再拼 —— 語序與格位每種語言都不一樣。
//
// 為什麼不直接用 swprintf_s + 固定緩衝區：緩衝區是照**中文**長度給的，而德文／
// 西班牙文／葡萄牙文的同一句話可以長到兩倍。swprintf_s 塞不下時會叫用 invalid
// parameter handler，那是 0xC0000409 直接收掉行程 —— 使用者看到的是「換了語言
// 之後程式一開就閃退」。實例：es_ES 的「%d fps (pantalla de alta frecuencia)」
// 塞不進 wchar_t[32]。所以翻譯字串一律走這裡，長度由字串自己決定。
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring FmtV(const wchar_t* fmt, va_list ap);

// 上一次 Load 的結果，給 log 用（"locale\ja_JP.json: 212 entries" 之類）。
const std::wstring& LoadNote();

// 語系檔目錄（exe\locale）。檔案不存在時也回傳路徑，方便寫進錯誤訊息。
const std::wstring& LocaleDir();

} // namespace i18n
} // namespace app
