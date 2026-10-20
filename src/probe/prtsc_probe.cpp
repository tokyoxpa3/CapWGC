// ============================================================================
// prtsc_probe.cpp — PrtScn 這個鍵到底被誰接走了？
// ============================================================================
//
// 問題：使用者想把「截圖 / OCR」綁在 PrtScn 上，取代 Windows 11 的
// 「使用 PrtScn 開啟剪取工具」。能不能成功，取決於一個很具體的事實：
//
//   **PrtScn 被 Windows 的剪取工具吃掉時，RegisterHotKey(VK_SNAPSHOT) 會失敗。**
//
// 這是猜不得的 —— 兩種結果的因應方式完全不同：
//   - 註冊得到 → 直接讓使用者選這個鍵，什麼都不用做
//   - 註冊不到 → 必須先關掉 Win11 的那個設定（HKCU\Control Panel\Keyboard
//                 的 PrintScreenKeyForSnippingEnabled = 0），而且要具體告訴
//                 使用者去哪裡關，不能只說「已被佔用」
//
// 所以先量。這個工具只做三件事，全部輸出成一行一行的 KEY=VALUE：
//   1. 讀那個登錄值（不存在 / 0 / 1）
//   2. 試註冊 VK_SNAPSHOT（修飾鍵 0，就是真正的 PrtScn）
//   3. 註冊成功就等 8 秒，按鍵真的會送來嗎（WM_HOTKEY 有沒有到）
//
// 第 3 點是關鍵：登錄值說「啟用」但實際沒搶到、或登錄值不存在（某些版本
// 預設就是開）的情況下，光看第 1、2 點的組合會判斷錯。真的按一次最準。
//
// --inject：由程式自己送一次 PrtScn（keybd_event），1.5 秒後送。
// 為什麼要自己做這件事：這個探測需要「有人在 8 秒內按一下」，自動化時沒有
// 那個人。**但注入的按鍵不一定等於真按** —— Shell 處理 PrtScn 走的是鍵盤
// 訊息路徑，注入通常會走同一條，可是若要下結論必須知道這是注入的結果。
// 所以輸出多一行 hotkey.injected=1，讓看 log 的人知道差別在哪。真的硬體
// 按鍵永遠是更強的證據，注入只是拿來做可重複的對照。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace {

constexpr int kWaitMs = 8000;

const wchar_t* RegStateText(DWORD* out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", 0, KEY_READ, &k) !=
        ERROR_SUCCESS) {
        *out = 0xFFFFFFFF;
        return L"open-failed";
    }
    DWORD val = 0;
    DWORD cb = sizeof(val);
    DWORD type = 0;
    const LSTATUS st =
        RegQueryValueExW(k, L"PrintScreenKeyForSnippingEnabled", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&val), &cb);
    RegCloseKey(k);
    if (st == ERROR_FILE_NOT_FOUND) {
        *out = 0xFFFFFFFF;
        return L"absent";
    }
    if (st != ERROR_SUCCESS) {
        *out = 0xFFFFFFFF;
        return L"query-failed";
    }
    *out = val;
    return val ? L"enabled" : L"disabled";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool inject = (argc > 1 && !_wcsicmp(argv[1], L"--inject"));
    // --control：對照組。用 Ctrl+Alt+W（一個「普通」的熱鍵）跑同樣的流程。
    //
    // 為什麼一定要有這一組：如果 PrtScn 收不到，有兩種可能 ——
    //   (a) PrtScn 被 Shell 吃掉（結論）
    //   (b) 我的注入方式根本觸發不了 WM_HOTKEY（測試方法錯）
    // 這兩者的輸出一模一樣。跑一次對照組就能分辨：對照組收得到，才證明
    // 「收不到」是 PrtScn 的問題而不是量測的問題。
    const bool control = (argc > 1 && !_wcsicmp(argv[1], L"--control"));
    const bool autoSend = inject || control;
    const UINT mods = control ? (MOD_CONTROL | MOD_ALT) : 0;
    const UINT vk = control ? 'W' : VK_SNAPSHOT;

    wprintf(L"== PrtScn probe ==\n");
    wprintf(L"mode=%ls\n", control ? L"control(Ctrl+Alt+W)" : L"prtsc");

    // 1. 登錄值。注意「absent」不等於「關閉」—— 沒寫過就是系統預設，
    //    而 Win11 的預設是**開**（PrtScn 開剪取工具）。
    DWORD regVal = 0;
    const wchar_t* regText = RegStateText(&regVal);
    wprintf(L"reg.state=%ls\n", regText);
    if (regVal != 0xFFFFFFFF) wprintf(L"reg.value=%lu\n", regVal);
    else wprintf(L"reg.value=none\n");

    // 2. 試註冊（PrtScn 用 mods = 0；對照組用 Ctrl+Alt+W）。
    const BOOL ok = RegisterHotKey(nullptr, 1, mods, vk);
    const DWORD err = ok ? 0 : GetLastError();
    wprintf(L"hotkey.register=%d\n", ok ? 1 : 0);
    wprintf(L"hotkey.error=%lu\n", err);

    if (!ok) {
        // 1409 = ERROR_HOTKEY_ALREADY_REGISTERED。這個錯誤碼就是「別人拿走了」，
        // 而 PrtScn 的情況幾乎都是 Win11 的剪取工具。其他錯誤碼要照實印，
        // 因為那可能是完全不同的問題（例如在某些 UIPI 情境下的權限問題）。
        wprintf(L"verdict=blocked\n");
        wprintf(L"hint=%ls\n",
                err == ERROR_HOTKEY_ALREADY_REGISTERED
                    ? L"PrtScn 已被其他程式（多半是 Windows 剪取工具）佔用"
                    : L"註冊失敗，錯誤碼見上");
        return 2;
    }

    // 3. 真的按一次最準。使用者要在這 8 秒內按 PrtScn（或 --inject 自己送）。
    wprintf(L"press=waiting\n");
    wprintf(L"hotkey.injected=%d\n", autoSend ? 1 : 0);
    if (!autoSend) wprintf(L"hint=%ls\n", L"8 秒內按一下 PrtScn");
    fflush(stdout);

    MSG msg{};
    const DWORD start = GetTickCount();
    const DWORD until = start + kWaitMs;
    bool got = false;
    bool sent = false;
    while (!got) {
        const DWORD left = until - GetTickCount();
        if ((int)left <= 0) break;

        if (autoSend && !sent && GetTickCount() - start >= 1500) {
            if (control) {
                keybd_event(VK_CONTROL, 0, 0, 0);
                keybd_event(VK_MENU, 0, 0, 0);
                keybd_event((BYTE)vk, 0, 0, 0);
                keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
                keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
                keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
            } else {
                keybd_event((BYTE)vk, 0, 0, 0);
                keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
            }
            sent = true;
        }

        // 15ms 一格：注入要準時，不能一路睡到逾時才發現該送鍵了。
        const DWORD r = MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                                  std::min<DWORD>(left, 15), QS_HOTKEY);
        if (r == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_HOTKEY && (int)msg.wParam == 1) got = true;
            }
        }
    }

    UnregisterHotKey(nullptr, 1);
    wprintf(L"hotkey.received=%d\n", got ? 1 : 0);
    wprintf(L"verdict=%ls\n", got ? L"works" : L"registered-but-silent");
    if (!got) {
        // 註冊成功但按鍵不來：Win11 的 PrtScn 有自己的處理路徑，Shell 會在
        // 熱鍵之前把它吃掉。這種「註冊成功」比「註冊失敗」更難查 ——
        // 所以第 3 步不是多餘的。
        wprintf(L"note=%ls\n",
                L"註冊成功但收不到 WM_HOTKEY，代表鍵被 Shell 搶先處理");
    }
    return got ? 0 : 3;
}
