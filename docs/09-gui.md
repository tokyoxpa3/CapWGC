# 09 — GUI：托盤、熱鍵、區域框選、錄製指示

> 這一層的目標不是「做一個好看的介面」，而是讓引擎第一次能被日常使用。
> 範圍刻意壓到最短閉環：**選區域 → 錄 → 停 → 拿到檔案**。

---

## 1. 為什麼是現在

引擎在 M1/M2/AV1/音訊/多軌/A-V 偏移之後已經收尾（`docs/03`~`docs/08`），
20 項驗收 19 項通過。但 `src/` 只有 `cli`、`core`、`probe` 三個目錄，
所有能力都要靠命令列參數驅動 —— 沒有任何一個普通人能日常用它錄一次螢幕。

專案目標寫的是「比 Captura 更好用」，而 Captura 的價值八成在介面：托盤、
全域熱鍵、拖一個框就開始錄、錄製中的紅點。所以下一個最大的落差不是編碼，
是這一層。

---

## 2. 架構：先抽 Recorder，再寫 UI

### 2.1 為什麼不能直接在 GUI 裡再寫一次錄製迴圈

`src/cli/main.cpp` 的錄製迴圈裡有三個「看起來可以隨便寫、但寫錯就要重測
整個 A/V 對齊」的語意：

1. **第一幀只拿來開編碼器，不寫進檔案** —— 那時還不知道 duration。
2. **檔案時間的零點是「第一筆寫入的幀」**，音訊延到那一刻才 `Start()`。
   零點若定在第一幀，音軌會多補一幀（16.7ms），變成過度補償。
3. **時長由前後 pts 相減得到**（VFR），不是 `1/fps`。

這三條一旦有第二個實作，之後修一處就要改兩邊、測兩次，而驗證它們需要
`anim_window --sync` + `mp4_check --sync` 那一整套（見 `docs/08` §6.5）。

所以先抽 `src/core/recorder.h/.cpp`，GUI 只負責「給參數、按開始、按停止」。

### 2.2 Recorder 的邊界

```cpp
struct RecorderConfig {
    // 目標三選一：region > hwnd > monitor
    bool useRegion; RECT regionScreen; HWND hwnd; HMONITOR monitor;
    // 編碼 / 音訊 / 輸出 ...
    int64_t avOffset100ns;   // 額外校正值；gap 是自動補的，不用設這裡
};
class Recorder {
    bool Start(const RecorderConfig&, std::string* err);
    void Stop();                       // 阻塞收尾
    RecorderStats stats() const;       // 跨執行緒可讀
};
```

刻意**沒有**做的兩件事：

- **`--ab` / `--ref`（同批幀多路輸出）**。那是量測工具，不進 GUI。CLI 保留
  自己那一份實作不動 —— 所有既有腳本（`sync_ab.sh`、`calibrate.sh`、
  `ui_sweep`、`hard_sweep`）都依賴它的輸出行為，重構它等於把量測基線弄掉。
- **`--av-offset auto` 的讀檔**。GUI 不讀校準檔：音軌的 gap 由 `Recorder`
  自己量、自己補（§4.7），剩下的微調用 `avOffset100ns` 明確指定就好，
  不需要一個「開機時偷偷套用某個數字」的隱含行為。

`regionScreen` 是**虛擬桌面的螢幕座標**，Recorder 內部負責換算成「中心點所在
那顆螢幕的 item 裁切矩形」。這件事不能丟給呼叫端：GUI 拿到的是滑鼠座標，
CLI 拿到的是 `--crop` 的 item 座標，兩者混用會出現「錄到隔壁螢幕」這種
難以察覺的錯。換算時寬高向下取偶（4:2:0 的編碼器對奇數寬高不一定吃得下）。

### 2.3 檔案結構

```
src/core/recorder.h/.cpp   可重用的錄製器（GUI 用；CLI 保留自己的迴圈）
src/app/ui.h/.cpp          MakeDotIcon / DimPanel / BorderWindow / TextBadge
src/app/overlay.h/.cpp     區域框選覆蓋層
src/app/hud.h/.cpp         錄製中指示（外框 + 計時徽章）
src/app/picker.h/.cpp      縮圖牆來源選擇器（Discord 式）
src/app/settings.h/.cpp    INI 設定 + 設定視窗（含熱鍵擷取）
src/app/main.cpp           WinMain、托盤、熱鍵、狀態機、自我測試
```

### 2.4 狀態機

```
     ┌────── 熱鍵 A / 托盤（區域） ──────┐
     │                                   │
     │   ┌── 熱鍵 W / 托盤（縮圖牆）──┐  │
     │   ▼                           │  │
   Picking ──(選好來源)──┐           │  │
     │                   │           │  │
     └──(Esc)──> Idle <──┴───────────┴──┤
     ▼                                   │
  Selecting ──(選好 / Esc / 右鍵)──> Idle ──(熱鍵 R)──> Recording
                                                          │
                                      (熱鍵 S / 視窗關閉 / 4 秒無畫面)
                                                          ▼
                                                       Stopping
                                                          │
                                                (worker 收尾完成 → post)
                                                          ▼
                                                         Idle
```

`Picking`（縮圖牆）與 `Selecting`（拖曳框選）是兩條平行入口，都收斂到
「產生一個目標 → 交給 Recorder」。`Picking` 選到的可能是一顆螢幕
（`HMONITOR`）而不是視窗，所以它與 `Selecting` 不能共用同一種回傳值。

`Stop()` 一定要在 worker thread：它會 join WGC 的擷取執行緒、沖完 AAC、
寫 moov，實測 100~300ms。在 UI 執行緒做，托盤會卡住不回應 —— 使用者會以為
程式當了，然後去工作管理員殺掉它，正好毀掉剛錄好的檔案。

---

## 3. 操作

熱鍵**全部可在設定視窗自訂**（§4.8），下表是預設值。

| 動作 | 預設熱鍵 | 說明 |
|---|---|---|
| 選區域錄影 | `Ctrl+Alt+A` | 拖曳出矩形 |
| 選視窗錄影 | `Ctrl+Alt+A` | 單擊目標視窗（拖曳 < 4px 視為單擊） |
| 視窗清單（縮圖牆） | `Ctrl+Alt+W` | 開 Discord 式縮圖牆挑來源（§4.9） |
| 錄主螢幕 | `Ctrl+Alt+R` | 直接錄游標所在螢幕 |
| 截圖 / OCR | `Ctrl+Alt+X` | 凍結畫面 → 頂端工具列選「截圖」或「OCR」→ 框選（§4.10） |
| 停止 | `Ctrl+Alt+S` | |
| 設定 / 開啟資料夾 / 結束 | 托盤右鍵選單 | 選單上的熱鍵文字是**動態**的，跟著自訂值變 |
| 取消選取 | `Esc` 或右鍵 | |

錄製中畫面上只有一顆 66×36 的膠囊（§4.2）：

| 動作 | 操作 | 說明 |
|---|---|---|
| 停止 | 點左邊的紅色方塊 | 與 `Ctrl+Alt+S` 等價 |
| 麥克風靜音 / 恢復 | 點右邊的麥克風 | 圖示變灰加斜線；只影響麥克風那一路，系統聲不動 |
| 移動控件 | 左鍵按住膠囊拖 | 位置會記住，下次錄影沿用（§4.2.2） |
| 歸位 | 托盤右鍵 → 重設控件位置 | 錄影中也有效（§4.2.3） |

計時不在畫面上（那會擋住內容），改成托盤圖示的提示文字。

**「錄影中開關麥克風」的準確語意**（常被誤解，先講清楚）：

| 情況 | 行為 |
|---|---|
| 這次錄影**有**麥克風（設定勾了「麥克風」，且端點開得起來） | 按鈕＝即時開關。關掉時把麥克風那一路換成**零樣本**：時間軸不動、影片照錄；再按一次就接著錄。同一支檔案可以反覆切 |
| 這次錄影**沒有**麥克風（設定沒勾） | 按鈕是灰的，點下去只會說明原因。錄影中**不能**新增音源（Recorder 的拓撲在 `Start()` 就固定了） |

檔案裡**只有一條音軌**：系統聲與麥克風混在一起（§4.7.1）。所以「關麥克風」
的實際效果是「這一段時間只剩系統聲」，不是「少一條軌」。想要單獨的麥克風軌
請勾設定裡的「保留分軌音源」（§4.7.2）。

**幀率選項**：`30 / 60 / 120 / 144 / 165 / 240`。這個值是**上限不是保證** ——
WGC 的送幀節奏跟螢幕刷新率綁在一起，60Hz 的螢幕選 240 也只會出 60。高於
120 的三個選項在下拉裡標了「（高刷螢幕）」，因為它們只有在高刷螢幕上才有意義，
而且檔案大小是線性成長的（240 大約是 60 的四倍）。

實測一支 5.2 秒的檔案（`out/gui_bar_mic_cycle.ps1`，逐 100ms 量 RMS）：

```
0.00~0.55s   -42 dB   ← 正常收麥克風
0.60~2.05s  -999 dB   ← 按鈕關閉（完全無聲）
2.10~3.65s   -42 dB   ← 按回來，同一支檔案繼續錄
3.75~5.18s  -999 dB   ← 再關一次
```

麥克風軌長度 5.184s 對影像 5.217s —— 中途關兩次，時間軸一個 tick 都沒少。

單擊與拖曳共用一個入口（跟 Snipaste 一樣），不需要額外的模式切換；
要一次攤開所有可錄來源時走縮圖牆那條路。

---

## 4. 幾個關鍵設計

### 4.1 遮罩用「四塊面板」而不是「一整片挖洞」

要讓選取框內透出桌面，就得用 per-pixel alpha（`UpdateLayeredWindow` +
32bpp DIB）。但「均勻變暗」用 `LWA_ALPHA` 一個 API 就夠。

所以遮罩是四塊普通矩形（上/下/左/右），跟著選取框移動。沒有 alpha 合成、
不需要每次重算 region、也不會因為 DIB 的預乘 alpha 搞錯而整片變黑。

#### 4.1.1 面板是「每次框選重建」，所以 Destroy 必須把狀態歸零

`DimPanel` 的生命週期是「`Create` 一次 → 用完 `Destroy` 一次」。這種反覆
建立／拆掉的元件最容易壞在狀態沒清：`Destroy` 只清了 `hwnd_`，`visible_`
還是 `true`、`last_` 還是上一次的矩形。而 `SetRect` 的早期返回是「矩形沒變
就什麼都不做」，於是新建的視窗永遠不會被 `ShowWindow`。

症狀是「遮罩開起來了，但畫面沒有變暗」—— 而且**間歇性**：還沒拖曳時整片
變暗的矩形就是整個虛擬桌面，每次完全一樣，所以第二、三次開框選一定中。
`BorderWindow::Destroy` 有一模一樣的問題（框會不見）。

用一個不需要人互動的測試釘住它：

```
build.bat dimtest && out\dim_panel_test.exe
```

它在同一個矩形上連續做「Create → SetRect → Destroy」三輪，直接問
`IsWindowVisible`。沒有修的時候第二輪就是 `SetRect moved=0 visible=0`。

這個 bug 值得單獨記一筆的理由是：它**繞過了所有既有的檢查**。
`--sniptest` 會印 `BeginSnip -> 1`、log 會印 `overlay.BeginSnip -> 1`，
兩者都是真的（覆蓋層確實建立了），錯的是那一塊遮罩的顯示狀態。
所以驗遮罩只能驗「螢幕像素有沒有變暗」，不能驗 API 回傳值。

### 4.2 錄製中的 UI：一顆可以拖的膠囊，沒有外框

原本是「外框 + 計時徽章」。2026-10-18 改成單一顆漂浮控件
（`src/app/controlbar.h/.cpp`），原因有三個：

1. 外框會讓被錄的畫面看起來像被裁掉一塊，使用者會以為錄出來就是那樣。
2. 錄整個螢幕時外框根本畫不到外面去，只能靠 display affinity 排除 ——
   那條路失效時會在成品邊緣留一圈紅邊。
3. 錄製中真正需要的動作只有兩個：停、靜音。計時不是動作，放在畫面上
   只是擋路。

現在畫面上只有 66×36 的膠囊：左邊紅色方塊（停止）、右邊麥克風（靜音切換）。
計時改成托盤提示文字（`TrayText` 帶秒數，整秒變了才 `NIM_MODIFY` 一次）。
沒有麥克風軌時麥克風鈕畫成灰的，點下去用氣泡說明原因 —— 一顆按了沒反應的
鈕比一顆灰色的鈕更難懂。

#### 4.2.1 全螢幕錄影時它不會入鏡

靠 `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`。WGC 合成擷取
畫面時把這個視窗整個拿掉，**不是變黑** —— 膠囊後面原本的畫面會正常出現在
成品裡。

實測（2026-10-18，`out/gui_bar_exclude2.ps1`，全螢幕 1600×900，膠囊在
1534,816）：

| 執行方式 | 幀數 | 膠囊區域的紅色像素（停止鈕簽名） |
|---|---|---|
| 預設（排除開啟） | 181 | **0 / 181 幀** |
| `--allow-capture`（排除關閉，對照組） | 116 | 106 像素/幀，**116 / 116 幀都有** |

「紅色像素」是很好的簽名：UI 上其他地方沒有那種紅。單看亮度不夠——桌面
桌布本身就有暗塊，第一版就是被桌布騙過一次。

這個 API 同時會讓**一般截圖**看不到控件（QA 時 `CopyFromScreen` 抓到的是
背後的内容）。所以 QA 入口有 `--allow-capture` 可以關掉它。

#### 4.2.2 可以拖，而且位置會記住

* 拖曳：左鍵按住膠囊本體拖動。位移 < 4px 視為點擊（不然點圖示會變成拖曳）。
  拖曳一律用 `GetCursorPos` 算位移 —— 視窗自己在移動，client 座標會跟著跳。
* 位置存成**相對擷取範圍右下角的位移**（`[ui] bar_pos=dx,dy`），不是絕對座標。
  絕對座標在 1600×900 上拖到角落，換到 1920×1080 就會離邊界很遠。
* 預設位置：擷取範圍右下角**外側** 8px。外側放不下就換上方，上下都沒有
  （錄整個螢幕就是這種）才疊進右下角。

#### 4.2.3 它不可能跑到螢幕外

三道防線，任何一道單獨都足以讓它回到可見範圍：

1. **拖曳中**：每一幀都把新位置夾回「游標所在螢幕」的 `rcWork`。
2. **顯示時**：讀到的位移一律夾回工作區；若位移指向別的螢幕（多螢幕時
   把控件放別的螢幕是合理偏好）就夾到**那顆**螢幕，螢幕不在了就夾到最近的
   那顆。`rcWork` 不含工作列，所以連被工作列蓋住都不會。
3. **逃生門**：托盤選單「重設控件位置」，清掉 `bar_pos` 並立刻歸位
   （錄影中也有效）。使用者不必知道 INI 在哪。

實測 `out/gui_bar_offscreen.ps1`（把 `bar_pos` 手改成荒謬值）：

| `bar_pos` | 實際落點 | 在工作區內 |
|---|---|---|
| `5000,-5000` | 1534,0 | 是（貼右上） |
| `-99999,99999` | 0,816 | 是（貼左下） |
| 未設定 | 1534,816 | 是（預設右下） |

### 4.3 遮罩與外框用 GDI，文字用 D2D

遮罩與外框只是純色填滿，GDI 一個 `FillRect` 就結束；文字才是 D2D 有價值的
地方（DirectWrite 的字距與抗鋸齒）。全部都自己畫、不拉框架 —— 維持「小、
秒開」這個相對 OBS 的賣點。目前 `CapWGC.exe` 580 KB。

圓角**不能**靠視窗 region（`CreateRoundRectRgn`）做：region 是 1-bit 遮罩，
沒有抗鋸齒，圓角在 100% DPI 下是一格一格的階梯（使用者原話：「像素感太強」）。
錄製控件改成 per-pixel alpha：32bpp 的 DIB（premultiplied）→ D2D **DC**
render target 畫 → `UpdateLayeredWindow`。形狀由 alpha 通道決定，圓角與 1px
描邊都是平滑的（實測邊緣出現 217 / 167 / 151 / 209 這種部分覆蓋值，
共 352 個中間值像素；改之前是 0）。

代價有兩個，都要處理：

| 代價 | 處理 |
|---|---|
| 不能用 `SetLayeredWindowAttributes` | 它和 `UpdateLayeredWindow` 互斥，混用會整個視窗不更新 |
| 視窗變成矩形，圓角外的透明區也會收滑鼠 | `WM_NCHITTEST` 用 DIB 的 alpha 判斷，`< 8` 回 `HTTRANSPARENT` 讓點擊穿過去 |

```cpp
case WM_NCHITTEST: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    if (self->AlphaAt(p.x, p.y) < 8) return HTTRANSPARENT;
    return HTCLIENT;
}
```

DIB 一定要 **top-down**（`biHeight = -h`），`AlphaAt` 才能直接用
`bits[y * w + x]`；D2D 的 pixel format 要用 `D2D1_ALPHA_MODE_PREMULTIPLIED`，
不然 `UpdateLayeredWindow` 貼上去會出現黑邊。

遮罩與外框仍然走 GDI（純色填滿），只有控件這顆膠囊需要 per-pixel alpha。

### 4.4 設定視窗不能「以 owner 置中」

托盤程式的 owner 是一個 0×0 的隱藏訊息窗（`a.msgwnd`，位置 (0,0)），
它不是給人看的。`GetWindowRect` 對它回 `{0,0,0,0}`，於是

```
x = 0 + (0 - 460) / 2 = -230      // 當時的視窗尺寸 460×369（現已 460×480）
y = 0 + (0 - 369) / 2 = -184
```

標題列整個跑到螢幕左上角外面。而這個視窗是 `WS_POPUP` 且沒有工作列按鈕，
使用者**沒有辦法把它拖回來** —— 只能砍掉行程。

現在的分工是：

| 情況 | 基準 |
|---|---|
| owner 有實際大小（≥ 64×64） | 以 owner 置中（多螢幕下跟著母視窗） |
| owner 是 0×0 / 無 owner | **滑鼠所在螢幕的工作區**（托盤程式該有的行為） |

最後不論走哪條路，都把矩形夾回 `rcWork`：塞得下就往內推，塞不下就貼左上
（至少標題列與按鈕看得到）。`y` 另外強制 `>= rcWork.top`，因為標題列是唯一
能拖動它的地方。

### 4.5 挑資料夾要另開一條 STA 執行緒

主執行緒是 **MTA**（`CoInitializeEx(nullptr, COINIT_MULTITHREADED)`），這是刻意的：
`AudioCapture::Open()` 在呼叫端執行緒建立 `IMMDeviceEnumerator` / `IAudioClient`，
再交給擷取執行緒使用。改成 STA 的話那些介面跨 apartment 就得 marshal，音訊會壞掉。

但 `SHBrowseForFolderW` 帶 `BIF_NEWDIALOGSTYLE` 需要 **STA**。在 MTA 呼叫的實際
症狀不是回錯誤，而是**整個行程卡死**：對話框永遠不出現、UI 沒有回應、CPU 0%，
只能砍行程。實測 `Responding = False`、連 `#32770` 都沒被建立。

所以挑資料夾丟到自己的 STA 執行緒，主執行緒只負責等：

```cpp
HANDLE th = CreateThread(nullptr, 0, BrowseThreadProc, &job, 0, nullptr);
for (;;) {
    DWORD w = MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT);
    if (w == WAIT_OBJECT_0) break;          // 執行緒結束
    if (w != WAIT_OBJECT_0 + 1) break;
    // 抽訊息：不抽的話設定視窗會變「沒有回應」，而且 shell 對 owner 呼叫
    // EnableWindow 是 SendMessage 過來的，不抽就是互相等死。
    ...
}
```

三個必要的細節：

| 細節 | 不做的後果 |
|---|---|
| 用 `MsgWaitForMultipleObjects` + `PeekMessage` 而不是 `WaitForSingleObject` | 挑選期間設定視窗變「沒有回應」，且可能與 shell 的 `EnableWindow` 互鎖 |
| 把 `WM_QUIT` 重新 `PostQuitMessage` | 被這裡吃掉的話，外層 `GetMessage` 永遠等不到結束信號 |
| 寫回前檢查 `IsWindow(h)` | 抽訊息期間視窗可能已被關掉（owner 被 disable，但 `WM_CLOSE` 仍可送達） |

失敗是無聲的（選了資料夾、欄位沒變），所以這條路徑會寫
`%LOCALAPPDATA%\CapWGC\browse.log`，一行 `browse done [路徑]` + 一行
`write back [路徑 ok]`。

### 4.6 設定用 INI 不用 JSON

只有十來個純量。INI 一行一個、可以手改、Windows 原生
`GetPrivateProfileStringW`，不用寫一個能正確處理跳脫與編碼的 JSON parser
（150 行起跳，換來零好處）。位置：`%LOCALAPPDATA%\CapWGC\settings.ini`。

`Load()` 會把壞掉的值夾回合法範圍 —— `quality = 0` 會被 MFT 當成「未設定」
而回退預設值，那是 HANDOFF 裡記過的坑。

### 4.7 音軌要補「它比影像晚起跑」的那一段

**症狀**：錄出來的檔案，聲音比畫面早（嘴型對不上）。CLI 錄同一個畫面卻是好的。

**根因**：兩條軌的時間軸零點不是同一個瞬間。

| | 零點 |
|---|---|
| 影像 `t = 0` | 第一筆**寫入**的幀的擷取時刻（`videoOriginQpc`） |
| 音訊 `t = 0` | 第一個 WASAPI packet 的內容時刻（`firstPacketQpc`） |

音訊是等第一筆影像寫入之後才 `Start()` 的，所以
`gap = firstPacketQpc − videoOriginQpc > 0`：檔案時間 0 的那一刻，音軌還沒有
資料。但 muxer 收到第一筆 AU 一定放在 `dts = 0`，於是整條音軌早了 `gap`。

`gap` 有多大取決於「第一筆寫入的幀」離「音訊串流真的跑起來」多遠，也就是
`AudioCapture::Start()` 的耗時 —— 這台機器 1~6ms，慢的機器上百毫秒。它與
硬體、驅動、codec 初始化時間有關，**不能寫死**。

**修法**：`applyAudioOffset()` 在第一個 AU 產生時（容器還沒收任何樣本，
`SetAudioStartOffset` 才不會拒絕）把 `+gap` 寫進去。`gap > 0` 走 empty
edit／tfdt 後推，正是「音訊本來偏早」那一格。

要點：

- **逐條軌算**，不要共用一個值。多條音源不是同時 `Start()` 的，共用會讓第二條
  軌偏掉。所以狀態放在 `Audio` 裡（`offsetApplied`），不是 `Impl` 上。
- `cfg.avOffset100ns` 變成**額外校正值**，與 `gap` 相加而不是取代。
- 這個 bug 在 CLI 上被藏了很久：CLI 的 `gap` 是 86~89ms，而驗證素材源的
  `PlaySound` 剛好也慢 85~90ms，兩個誤差互相抵掉 → 量成「對齊」。
  詳見 `docs/08` §6.6.1。

**驗證**：`bash out/verify_sync.sh`，判準是「絕對值 ≤ 10ms」加「CLI 與 GUI
兩條路一致」。修正後 CLI −2.7~+3.6ms、GUI −6.0~+3.9ms（各四次），兩者差
1.3~9.6ms；修正前同一個素材是 +5.8 / +67.5。

### 4.7.1 多個音源要混成一條軌，不是各寫一條

**症狀**：設定裡勾了麥克風，錄完播放卻**完全沒有聲音**。

**診斷**：檔案裡其實有兩條 `trak` —— 一條 loopback（當時系統很安靜，整條是
靜音）、一條 mic（`peak=-0.0 dBFS`，聲音清清楚楚）。問題是**大部分播放器只播
第一條音軌**（Windows 內建播放器、手機播放器都是），而第一條剛好是 loopback。
聲音在檔案裡，播放器沒去播它。

實測那支 26 秒的檔案：

```
trak 0  vide hvc1  26.153s  1600x900
trak 1  soun mp4a  26.703s  44100 Hz  loopback   ← 播放器播這條（全靜音）
trak 2  soun mp4a  26.688s  48000 Hz  mic        ← 真正有聲音的在這裡
```

**修法**：多個來源**混成同一條軌**再編碼（`RecorderConfig::mixAudio`，預設開）。
第一個開成功的來源當 master（通常是 loopback），其餘當 slave：

```
slave：擷取 → 轉成 master 聲道數 → 靜音就換 0 → 重取樣到 master 取樣率 → ring
master：擷取 → 靜音就換 0 → memcpy 進 mixBuf → 逐一 MixInto（相加 + clamp）→ AAC
```

三個實作要點：

| 要點 | 為什麼 |
|---|---|
| slave 的靜音在**重取樣之前**換 0 | 相位照樣前進，取消靜音時時間軸不會突然對不上 |
| ring 容量 1 秒，滿了就丟最舊的 | 兩個裝置的時鐘不會一樣快，漂移一定要有地方吸收 |
| 相加用 `int` 再 clamp 到 int16 | 直接 `int16 + int16` 會溢位繞回，聽起來是爆音 |

代價講清楚：

- 混音後**只剩一條軌**，想要「分軌後製」的人拿不到單獨的麥克風軌了。
- 麥克風不再走 §4.7 的逐軌 `gap` 補償，改成以樣本數對齊 master。兩次 `Start()`
  相差只有毫秒級，人耳分不出來；但這確實比逐軌補償粗略。
- **CLI 仍然分軌**（`src/cli/main.cpp` 有自己的一套），它是量測工具，
  `mp4_check --audio` 要能單獨量每一條來源。這是兩條路刻意的不一致。

**驗證**（`out/gui_bar_mic_cycle.ps1` + `out/mic_window_rms.py`）：一支 5.2 秒
的檔案，麥克風關→開→關，逐 100ms 的 RMS 是 `-42 / -999 / -42 / -999 dB`，
檔案裡只有一條音軌（44100 Hz），長度 5.201s 對影像 5.234s。

> **2026-10-18 補充（量測更正）**：原本這裡寫「`gui_bar_mix_sys.ps1` 把麥克風
> 關掉、只放 440Hz，同一條軌出現 `-11.7 dB`，證明 loopback 混進去了」。後來
> 用 `--selftest --audio`（只有 loopback 一個源）重測，得到的是整條 `-999 dB` ——
> **這台機器上 loopback 端點本身就是靜音的**，那次 `-11.7 dB` 是麥克風收到
> 喇叭的聲音（腳本點靜音鈕沒點中）。混音邏輯本身沒問題（麥克風的內容確實
> 進了混音軌），但「loopback 有沒有混進去」在這種環境下量不出來，要換一台
> 真的在放音的機器才有結論。

### 4.7.2 分軌：給剪輯用，但預設不播

混成一條軌解決了「播放器只播第一條」，代價是想後製的人拿不到單獨的麥克風。
所以設定裡多一個勾選：**保留分軌音源（給剪輯用，檔案較大）**，預設關。

打開之後，`N` 個音源會寫成 `1 + N` 條軌：

| 軌 | 內容 | `tkhd` flags |
|---|---|---|
| 1 | 混音（系統 + 麥克風） | `7` = enabled / in_movie / in_preview |
| 2 | 系統聲（未混音） | `6` = 拿掉 enabled |
| 3 | 麥克風（未混音） | `6` |

三條都要的理由：只有「混音 + 麥克風」是還原不出系統聲的。`A = sys + mic`、
`B = mic`，`A − B` 不是乾淨的 `sys`（相位、clamp、重取樣誤差都會留下殘響）。
要真的能剪，就得三條都有。

實作上有兩個地方特別容易寫錯：

| 要點 | 為什麼 |
|---|---|
| master 的**分軌先編碼、再混音** | `mixBuf` 一旦蓋掉 `pcm`，原始的系統聲就沒了 |
| master 的分軌排在混音軌**後面** | 播放器挑第一條，第一條永遠要是混音軌 |

`tkhd` 的 `Track_enabled` 只是**請求**：MP4 沒有 MKV 那種 `FlagDefault`，
播放器不認這個 bit 就還是會照軌道順序挑第一條 —— 這也是為什麼順序比 flag 重要。
`mp4_check --tracks` 會把 `[disabled]` 印出來。

順帶一提，分軌的兩條都是**原生取樣率**、不經過重取樣，剪輯要的是沒被動過的素材；
混音軌才是 master 的取樣率。`applyAudioOffset` 因此要對同一個來源的兩條軌
都套一次同樣的 `gap`，只套一條的症狀很隱蔽：播放器聽起來是準的，剪輯軟體
把分軌拉出來才發現差了 A 毫秒。

**驗證**：`--selftest 4 --audio --mic --split` →

```
trak 0  id=1  vide hvc1  ts=90000   1600x900
trak 1  id=2  soun mp4a  ts=44100   loopback: Realtek HD Audio 2nd output
trak 2  id=3  soun mp4a  ts=44100   loopback: …（分軌）  [disabled]
trak 3  id=4  soun mp4a  ts=48000   mic: 麥克風 (Realtek(R) Audio)（分軌）  [disabled]
```

三條軌長度一致（3.67 / 3.67 / 3.69 s，對影像 3.70 s），`selftest.log` 的
`audioTracks=3`。同一支檔案逐軌量 RMS：混音軌 `-39.1 dBFS`、mic 分軌
`-40.0 dBFS`、系統分軌 `-999 dBFS` —— 系統那條剛好對應「這台機器 loopback
本來就沒聲音」，也就是分軌忠實反映了它的來源，沒有互相污染。

### 4.8 熱鍵：可自訂、可失敗、可回報

原本 `Ctrl+Alt+A/R/S` 三組寫死在 `RegisterHotKey` 呼叫裡。問題有兩個：

1. 撞到別的程式（實測 Anki / 某些輸入法）就是**無聲失效** —— 熱鍵按了沒反應，
   使用者只會覺得「程式壞了」。
2. 使用者無從改，因為沒有入口。

#### 4.8.1 註冊要「能失敗」而不是「一定成功」

`RegisterHotKey` 是**可以被別人佔用而失敗**的。所以啟動時逐一註冊、
**失敗者跳過、其餘照用**，而不是全部成功才啟動：

```cpp
// 回傳第一個被佔用的動作名稱；空字串 = 全部成功
std::wstring RegisterAll(HWND w, const Hotkeys& hk);
```

`RegisterAll` 先 `UnregisterAll()` 再逐個註冊 —— 改熱鍵時必經這條路，
不先清掉舊的會把「舊值」也算成佔用者，導致改完反而失敗。

啟動時若有失敗，托盤氣泡用警告色列出是哪幾組（不是靜默）。

#### 4.8.2 改熱鍵是「全有或全無」

設定視窗按確定時走 `ApplyHotkeys`：

```cpp
bool ApplyHotkeys(App& a, const Hotkeys& next, std::wstring* failed) {
    const Hotkeys prev = a.settings.hotkeys;
    const std::wstring bad = RegisterAll(a.msgwnd, next);
    if (bad.empty()) { a.settings.hotkeys = next; /* 更新托盤提示 */ return true; }
    RegisterAll(a.msgwnd, prev);          // 回滾：否則舊的被 Unregister、新的沒註冊
    if (failed) *failed = bad;
    return false;
}
```

**沒有回滾就會出現最糟的狀態**：舊的已經 `Unregister`、新的註冊失敗，
使用者從此按什麼都沒反應，而且 UI 上顯示的是新值 —— 完全對不上。
設定視窗失敗時**只回滾熱鍵**，其餘設定（輸出資料夾、品質…）照收，
不要讓一個撞鍵把整份設定擋掉。

#### 4.8.3 擷取控制項用「唯讀 EDIT + 子類化」

不用自繪控制項，用唯讀 `EDIT` 加 `SetWindowSubclass`：

| 做法 | 為什麼 |
|---|---|
| 唯讀 `EDIT` 而不是自繪 | 天生有邊框、caret、進得了 Tab 順序，看起來就是一個欄位；且仍收得到 `WM_KEYDOWN` / `WM_SYSKEYDOWN` |
| `WM_GETDLGCODE` 回 `DLGC_WANTALLKEYS \| DLGC_WANTCHARS` | **不這樣做欄位根本抓不到鍵**：Enter 被對話框吃掉（直接關窗）、Esc 關窗、Tab 跳走。使用者會覺得「這欄位壞了」 |
| 回 `DLGC_WANTALLKEYS` 之後自己轉 Tab | 對話框不再代勞，`GetNextDlgTabItem` + `SetFocus` 要自己寫，否則 Tab 卡在欄位裡 |
| 修飾鍵用 `GetKeyState` 讀 | 讀的是**真實鍵盤狀態**。副作用是 `PostMessage` 測不出來（沒有真的按鍵），測試必須用 `keybd_event` |
| Esc / Backspace / Delete 清除 | 統一語意為「＝不綁熱鍵」，顯示「（未設定）」 |

擷取時的行為：

- 只按修飾鍵 → 先顯示 `Ctrl+…`，等真正的鍵（有即時回饋，不然像沒反應）
- 無修飾鍵 → 擋，紅字說明
- `F12` → 擋（核心偵錯器保留，`RegisterHotKey` 一定失敗）
- `Win` 組合 → **擋**，見下

`Win` 這條特別記一筆：Win 的 keydown 會被 shell 先接走並彈出開始功能表，
**應用層只收得到 `Shift+Win` 這種半截組合，真正那個鍵永遠不會到**。
實測 `Win+Shift+P` 的欄位永遠停在 `Shift+Win+…`，使用者只會覺得「按了沒反應」。
所以與其留一個半截死路，不如當場擋掉並說明原因。

注意 `MOD_WIN` 本身是**合法**的（`RegisterHotKey` 支援，INI 手寫
`Win+Shift+P` 能正常註冊）——擋的只是「擷取 UI 抓不到」，不是「不能用」。
這兩件事的差別寫在 `HotkeyUsable` 的註解裡。

#### 4.8.4 持久化成人可讀的字串

INI 存的是 `Ctrl+Alt+A` 這種字串，不是 `MOD_CONTROL|MOD_ALT` + `0x41`
的數字。理由：手改得動、看得出是什麼、跨版本不會因為常數值改變而錯亂。

| 函式 | 職責 |
|---|---|
| `HotkeyText` | 固定 `Ctrl` → `Alt` → `Shift` → `Win` 順序輸出 |
| `HotkeyFromText` | 吃大小寫 / 空白 / `Control`/`Ctrl` / `Win`/`Windows`；別名表含 `escape`/`return`/`del`/`pgup`/`prtsc`/`Numpad7`… |
| `Hotkeys::Duplicate` | 回傳如 `「選區域」與「停止」`，供衝突訊息用 |
| `HotkeyUsable` | 排除 F12、純修飾鍵、滑鼠鍵 |

**解析失敗整條退回預設**，不做部分修補：INI 被改壞時「一半舊值一半預設」
比「全部預設」更難理解。

```ini
[hotkeys]
region=Ctrl+Alt+A
picker=Ctrl+Alt+W
screen=Ctrl+Alt+R
stop=Ctrl+Alt+S
```

### 4.9 縮圖牆：像 Discord 那樣挑來源

原本「單擊選視窗」要求使用者**憑記憶**去點一個已經被別的視窗蓋住的目標，
而且看不到它到底長什麼樣。Discord 的分享來源選擇器把「所有可錄的東西」
一次攤開成縮圖，一眼就知道要錄哪個。

#### 4.9.1 列舉與過濾

```cpp
EnumWindows(...)                                   // z-order，最上層的排前面
DwmGetWindowAttribute(DWMWA_CLOAKED)               // 濾掉 UWP 的隱形殼
```

`DWMWA_CLOAKED` 那一步不能省：UWP 程式在 `EnumWindows` 眼裡是「可見的」，
但實際是 cloaked 的空殼，縮圖會是一片黑 —— 使用者會看到一排黑格子。

**但「濾掉」要有節制**：濾得太兇就是「我明明開著，清單裡卻沒有」（見 §4.9.4）。
現在只濾掉這幾類，而且每一條都有理由：

| 濾掉的 | 為什麼 |
|---|---|
| `Progman` / `WorkerW` | 桌面本身，錄它等於錄整個螢幕 |
| `Shell_TrayWnd` / `Shell_SecondaryTrayWnd` | 工作列 |
| `IME` / `MSCTFIME UI` | 輸入法視窗 |
| `Windows.UI.Core.CoreWindow` | UWP 的內層殼（外層 `ApplicationFrameWindow` 才是要錄的） |
| 還原後矩形 < 120×80 | 真正的小工具/浮窗 |

螢幕分頁則走 `EnumDisplayMonitors`，主螢幕排第一。

列舉完做一次排序：**沒最小化的排前面，最小化的排最後**，兩組內部保持 z-order。

```cpp
std::stable_sort(wins_.begin(), wins_.end(),
                 [](const Item& a, const Item& b) { return !a.minimized && b.minimized; });
```

不排的話，最小化的視窗常常擠在 z-order 的前段，首頁會變成一整片灰格子，
使用者第一眼就覺得「東西變少了」——清單其實是齊的，只是排壞了。

#### 4.9.2 抓縮圖：先抓完才顯示自己

順序是**先把所有縮圖抓完，再顯示選擇器**。

反過來（先顯示再逐張抓）的症狀是縮圖一張張「跳」出來，而且抓的時候
選擇器自己在最上層，抓前景視窗會抓到「我們自己」。

抓法有優先序：

| 情況 | 做法 |
|---|---|
| 一般視窗 | `PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)` |
| 抓回來幾乎全黑 | 標記為 placeholder（有些 GPU 加速視窗不支援） |
| 前景視窗 | `BitBlt(SRCCOPY \| CAPTUREBLT)` 當 fallback |
| 最小化的視窗 | 不抓畫面（`PrintWindow` 只給黑圖），直接畫程式圖示 |
| 縮放 | `StretchBlt(HALFTONE)` 到 480×270 |

`MostlyBlack` 取樣檢查是必要的：`PrintWindow` **會成功回傳**卻給出一片黑，
不作內容檢查就會把黑圖當成有效縮圖。逐項記在 `picker.log`：

```
[pick] item hwnd=... how=printwindow thumb=ok 1600x900 title=...
[pick] item hwnd=... how=screen      thumb=ok 1600x900 title=...
[pick] item hwnd=... how=minimized   thumb=placeholder 1280x800 title=... 最小化
[pick] capture 25 windows + 1 monitors in 140 ms
```

拍不到畫面時**不要只留一個「無法預覽」灰塊**——一整排灰塊看不出誰是誰。
退回去抓圖示，順序是：

```
WM_GETICON(ICON_BIG/SMALL2/SMALL)  →  子視窗的同一組  →  類別圖示  →  exe 圖示
```

`WM_GETICON` 一律 `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 120ms)`：卡住的視窗
會把整個列舉一起拖住。拿到的 handle 都 `CopyIcon` 成自己的，`ReleaseThumbs`
裡 `DestroyIcon`，否則每開一次選擇器漏一批 GDI 物件。

UWP 的 `ApplicationFrameWindow` 自己回 null，圖示在子視窗上（實測「小算盤」
要走到第二層才拿到）；連子視窗都沒有就 `SHGetFileInfoW(exe)` 拿 process 的圖示。

#### 4.9.3 鍵鼠與外觀

| 操作 | 行為 |
|---|---|
| ←→↑↓ / Home / End / PgUp / PgDn | 移動選取 |
| Tab | 切「視窗 / 螢幕」分頁 |
| 單擊 / 雙擊 / Enter | 單擊選取，雙擊或 Enter 確認 |
| Esc / 右鍵 | 取消 |
| 滾輪 / 捲軸 | 捲動 |

Discord 深色配色（`#1e1f22` 底、`#2b2d31` 卡片、`#5865f2` 強調色），
自繪 `RoundRect` + `StretchBlt`。視窗是 `WS_POPUP | WS_VSCROLL`，
自訂 `WM_VSCROLL` / `WM_MOUSEWHEEL`，hover 用 `TrackMouseEvent`。

回傳值刻意用一個小結構而不是 `HWND`：

```cpp
struct PickResult {
    bool ok = false;
    HWND hwnd = nullptr;         // 非空 = 選了視窗
    HMONITOR monitor = nullptr;  // 非空 = 選了整個螢幕
    RECT rect{};
};
```

因為「選一顆螢幕」與「選一個視窗」是兩種不同的目標，用 `HWND` 表達不了後者。

#### 4.9.4 清單為什麼會「變少」：兩個無聲的過濾器

使用者回報「可選窗口變少了，不齊全」。查下來不是程式被改壞，而是列舉階段
有兩個**靜默丟棄**的規則，把它們算進來才知道少了什麼：

| 丟掉的原因 | 實測數量 | 後果 |
|---|---|---|
| `IsIconic` → 直接 `return TRUE` | 9 | 最小化的視窗整個消失（VS Code、Discord、Windows Terminal…） |
| `GetWindowTextW` 空字串 → 丟 | 1+ | UWP 的 `ApplicationFrameWindow` 標題在子視窗上，「設定」「小畫家」全消失 |

第一條的修法不是「放寬就好」，因為最小化視窗的 `GetWindowRect` 回的是
**圖示大小**（實測 160×28），照抄會得到一張 160×28 的縮圖與錯誤的錄製矩形：

```cpp
const bool iconic = IsIconic(h) != FALSE;
RECT r{};
if (iconic) {
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp)) return TRUE;
    r = wp.rcNormalPosition;          // 還原後的位置與大小
} else if (!GetWindowRect(h, &r)) {
    return TRUE;
}
```

第二條改用 `EnumChildWindows` 撈第一個非空標題（`ChildTitle`），
連子視窗都沒標題才退回 exe 名——**至少給個名字，不要讓它消失**。

另外上限從 24 提到 40，而且截斷時要留痕，否則又是一次靜默丟失：

```cpp
if ((int)v->size() >= kMaxWindows) { /* 只記一次 */ 
    LogW(L"[pick] window list truncated at %d (more windows exist)", kMaxWindows);
}
```

最後一塊是**確認時要先還原**：WGC 的 `CreateForWindow` 對最小化視窗只回一個
154×25 的 item（見 HANDOFF 的坑），所以 `Confirm()` 裡：

```cpp
if (it.minimized || IsIconic(it.hwnd)) {
    ShowWindow(it.hwnd, SW_RESTORE);
    SetForegroundWindow(it.hwnd);
    for (int i = 0; i < 40 && IsIconic(it.hwnd); ++i) Sleep(15);   // 最多等 600ms
    GetWindowRect(it.hwnd, &r.rect);   // 還原後重讀
    LogW(L"[pick] restored minimized window %p iconic=%d", ...);
}
```

驗證方式：`out/gui_picker_diag.ps1` 把每個頂層視窗與「被丟掉的理由」逐行列出來
（`ACCEPT` 數量要和 `picker.log` 的 `windows=` 對得上），再用
`out/gui_picker_min.ps1` 開一個最小化視窗、`End` 跳到最後、`Enter` 確認，
日誌要出現 `restored minimized window` 與 `end ok=1`。

#### 4.9.5 即時更新：清單會自己跟上，但**不重排**

原本縮圖牆開啟時只抓一次快照，之後視窗開了、關了、還原了都要**重開**才看得到。
現在掛了兩個 `SetTimer`：

| 計時器 | 週期 | 做什麼 |
|---|---|---|
| `kTimerRefresh` | 400ms | 重跑 `EnumWindows`，比對清單（增 / 刪 / 改標題 / 改尺寸） |
| `kTimerThumbs` | 120ms | 一次補抓最多 2 張縮圖 |

分成兩條是刻意的：`EnumWindows` 一趟 10~30ms 沒必要更勤，而 `PrintWindow`
一張可能 5~30ms，所以縮圖要**分批**抓。合在一起做就變成「每 100ms 掃清單 +
抓 N 張圖」，視窗一多起來畫面就會卡。

**核心取捨：用 `hwnd` 當身分原地更新，不重建整個 vector。**

```
既有的（hwnd 還在）→ 留在原位，更新 rect / minimized，縮圖留著
新出現的           → 插到尾巴（不插前面），標記 needThumb
消失的             → 移除
```

不重建的理由是體感：使用者可能正在看第二頁、或游標已經停在某張卡片上。
整份重建會讓所有卡片跳位、選取跑掉 —— 那正是「即時更新」最常見的失敗
（東西一直亂跳，比不更新還糟）。只有**尺寸變了**或**最小化狀態變了**才作廢
縮圖重抓，否則每 400ms 重抓 40 張等於一直在做白工。

這也連帶修掉一個 hover 的坑：`hover_` 是**索引**，清單刪一項之後同一個索引
就指到別的卡片。症狀是「滑鼠在 A 上，B 卻亮著」。清單一變就用游標位置
重新 `HitCard` 一次。

**兩個「不做」，各有物理理由：**

1. **即時刷新絕不從螢幕抓圖。** 首次快照時選擇器還沒顯示，前景視窗可以
   `BitBlt` 抄一份；但即時刷新時選擇器自己是 topmost，正蓋在目標上 ——
   抄下來的就是選擇器自己那一塊。所以 `CaptureOne(it, live=true)` 直接跳過
   `BitBlt` 回退（記 `how=live-skip`），這一輪留空、下一輪再試 `PrintWindow`。
   `PrintWindow` 不受遮擋影響，所以視窗縮圖可以安全地即時更新。
2. **監視器縮圖不即時。** 整個螢幕的縮圖**只能**靠螢幕擷取拿，而選擇器正蓋在
   螢幕正中央 —— 即時重抓必然拍到自己。所以螢幕分頁只在開窗時抓一次
   （`CaptureOne` 的 monitor 分支在 `live=false` 才做 BitBlt）。

順帶一個查證結果：**`WDA_EXCLUDEFROMCAPTURE` 對 GDI 截圖也生效**（實測
控制組 100% 可見、排除組 0%）。所以將來若要讓螢幕縮圖也能即時重抓，
把選擇器標上排除是可行的一條路 —— 但現在不需要，因為視窗縮圖用
`PrintWindow` 就夠了，監視器縮圖本來就不需要一直更新。

拖曳視窗時（`WM_ENTERSIZEMOVE`）暫停刷新，否則佈局一直在變、重畫會讓拖曳頓。

`--picker-static` 關掉即時刷新（`live=0`），給 QA 拍可重現的畫面用 ——
即時刷新會在截圖中途改動清單，讓「有幾張卡片」這類斷言不穩定。

### 4.10 截圖 / OCR：工具列是「模式選擇器」，不是「按鈕」

這一段記錄一個**看起來能用、實際會炸**的設計錯誤，因為它很容易再犯一次。

#### 4.10.1 錯在哪：點下去就執行

第一版的工具列呼叫回呼是「點了就立案執行」：

```cpp
// ❌ 第一版：點到 OCR 立刻拿「目前的選取範圍」去送 OCR
toolbar_.Create(owner_, [this](ui::Toolbar::Action a) {
    FinishSnip(a);          // 直接提交
});
```

而 `FinishSnip` 裡的範圍在「還沒有拖曳」時是**整個桌面**（那是 hover 命中的
結果）。於是使用者的操作 ——「按熱鍵 → 游標剛好在工具列上 → 點 OCR」——
產生的不是「切到 OCR 模式」，而是「立刻對整張桌面做 OCR」：

```
[18:50:00.582] snip done ok=1 action=2 rect=-8,-8,1608,860   ← 整屏
[18:50:00.629] ocr request id=1 1600x860
[18:50:05.258] ocr FAILED: UnicodeEncodeError: 'cp950' ...
```

（第二行那個 cp950 崩潰是另一個 bug，已修；但它掩蓋了真正的設計問題 ——
就算不崩，使用者要的也從來不是「整屏取字」。）

**根本原因**：把「選模式」和「執行動作」壓在同一個點擊上。使用者點工具列的
心理模型是**切換**（跟微軟內建剪取工具、ShareX 一樣），不是**送出**。

#### 4.10.2 現在的做法：先選，後框，最後才執行

```cpp
// ✅ 現在：點工具列只改「等一下要用哪個動作」
toolbar_.Create(owner_, [this](ui::Toolbar::Action a) {
    if (a == ui::Toolbar::Action::None) return;   // 點在空隙
    defaultAction_ = a;                           // 只切模式
    toolbar_.SetActive(a);                        // 只換高亮
});
```

三種「結束框選」的入口（單擊選視窗 / 拖曳 / Enter）全部收斂到同一個
`Submit()`，它才把 `defaultAction_` 拿出來當這次的動作：

```cpp
void Overlay::Submit(const RECT& rect, HWND win) {
    pendingAction_ = defaultAction_;   // ← 這裡才決定要截圖還是 OCR
    PostMessageW(owner_, WM_OVERLAY_DONE, 0, 0);
}
```

於是順序變成：**熱鍵 → 選模式（可省略，預設截圖）→ 框選 → 執行**。

#### 4.10.3 預設值不能被「使用歷史」污染

第一版還有一個更陰的問題：它把「使用者這次選了什麼」寫回 ini
（`[snip] default_action`）。結果是「用過一次 OCR 之後，之後**每次**進來都
預設 OCR」—— 跟「預設是截圖」完全相反，而且那個鍵在使用者介面上根本看不到，
連要改回來都不知道去哪改。

修法不是「記得別寫回」，而是**把這條路拆掉**：

| | 舊 | 新 |
|---|---|---|
| 預設動作從哪來 | `settings.snipDefaultAction`（會被寫回） | `BeginSnip(a, wantOcr)` 的**參數** |
| 熱鍵 / 托盤「截圖」 | — | `wantOcr = false` |
| 托盤「截圖並取字」 | 先改 settings 再呼叫 | 直接 `wantOcr = true` |
| ini 是否參與 | 是 | **否**（`default_action` 已不再讀寫） |

差別在於：參數是**每次呼叫都重新給**的，沒有任何狀態可以累積；ini 那種
「先寫進去、下次讀回來」的設計，只要有一行程式碼忘了，行為就開始漂移。

同時 `wantOcr` 還會被 `a.ocrAvailable` 否決：引擎沒就緒時一律退回截圖
（OCR 那一格本來就是灰的，點不到）。這樣「預設截圖」在最壞情況下也成立。

#### 4.10.4 驗證方式

需要模擬「滑鼠停在工具列上點一下」這種真實順序，`--sniptest` 不夠
（它不模擬滑鼠），所以用兩支小工具接力：

```bash
out/key.exe hotkey            # 送 Ctrl+Alt+X
out/drag.exe click 830 37     # 點工具列 OCR 段
out/drag.exe drag 180 150 820 420   # 再框選
```

判準是**日誌的缺席**，不是日誌的內容 —— 點完工具列之後 `gui.log` 必須
**完全沒有**新行（沒有 `snip done`）。實測：

```
[19:19:08.342] hotkey id=5 mode=0
[19:19:08.399] overlay.BeginSnip -> 1 ocrReady=1 def=1   ← def=1 是 Snip
    ← 中間那一下點擊，日誌全空
[19:19:12.870] snip done ok=1 action=2 rect=1065,215,1345,520
[19:19:12.903] ocr request id=2 280x305
[19:19:13.276] ocr ok lines=11 ms=346
```

`rect` 是 280x305（不是 1600x860）證明框選生效；`action=2` 證明模式切換生效；
`def=1` 證明預設是截圖。三個訴求一次驗完。

#### 4.10.5 結果不畫在螢幕上（原本會彈一個面板）

第一版辨識完會在畫面正中央（`wa.top + 22%`）彈一個深色文字面板，按 Esc
或點一下關掉。**已移除。**

理由：使用者按 OCR 的唯一目的就是「把字拿去別的地方用」，而文字早在結果
回來的那一刻就進剪貼簿了。這個面板因此是多餘的一層 —— 而且不是無害的多餘：

1. **它逼使用者先處理它。** 不點掉就一直在那，滑鼠過去點別的東西也可能
   被它吃掉（它是 topmost 徽章）。
2. **它蓋住剛框選的內容。** 想對照「原文 vs 辨識結果」時，原文正好被面板
   壓在下面。
3. **它讓「貼上」這個動作多一個岔路。** 使用者其實只要 Ctrl+V 就好。

現在的行為：**只進剪貼簿**，托盤氣球講一句「已複製 N 行／M 字，直接貼上即可」
就結束。氣球幾秒後自己消失，不需要理它。

#### 4.5.1 截圖同時能貼到小畫家與檔案總管

「截圖之後可以一邊貼到小畫家、一邊貼到資料夾嗎」——**可以，但前提是那個檔案
真的存在**。這不是實作偏好，是 Windows 剪貼簿的性質：

| 格式 | 帶的是什麼 | 誰會用 |
|---|---|---|
| `CF_DIB` / `CF_BITMAP` | **像素** | 小畫家、Word、多數圖片編輯器 |
| `CF_HDROP` | **檔名**（`DROPFILES` 結構）| 檔案總管、拖放到別的程式 |

關鍵在 `CF_HDROP`：它裡面只有路徑字串。檔案總管按 Ctrl+V 做的是「複製那個
檔案」，**它從不去讀剪貼簿裡的像素**。所以想兩邊都能貼，唯一的辦法是先存檔，
然後把三種格式一起放進剪貼簿 —— 而且順序不能顛倒：**先寫檔、再進剪貼簿**。

因此 `CopyToClipboard` 多一個 `filePath` 參數：

```cpp
// snip.h
bool CopyToClipboard(HWND owner, const ScreenBitmap& bmp,
                     const std::wstring& filePath, std::wstring* err);
```

傳空字串就是舊行為（純圖片）。`DeliverSnip` 先把 PNG 寫出來再呼叫它：

```cpp
if (bmp.SavePng(path)) { savedPath = path; ... }
app::CopyToClipboard(a.msgwnd, bmp, savedPath, &err);
```

實作上有兩個必須注意的點：

1. **`DROPFILES` 不在 `windows.h` 裡**，要 `#include <shlobj_core.h>`。
   這跟「用了什麼 Shell API」無關，純粹是那個結構的定義住在那裡。
2. **`fWide = TRUE`**。`DROPFILES` 有 ANSI 與 UTF-16 兩種寫法，ANSI 版在非
   系統語系的機器上遇到中文路徑就是亂碼。緩衝區要多留**兩個** `'\0'`：
   一個結束檔名、一個結束整份清單。

##### 4.5.1.1 存檔預設改成「開」

原本 `snipSaveFile` 預設 `false`，理由是「多數人只是要貼到別的地方，硬存會
積檔案」。改成預設 `true` 的直接原因就是上面那條：**能貼到資料夾這件事本身
依賴存檔**。既然存了，就該讓使用者找得到，而不是留一個隱形副作用。

改預設值有一個必須確認的地方：**已經選過「不要存」的人不能被影響**。他們
的 `ini` 裡有 `save_file=0` 這一行，照舊；只有從來沒碰過這個設定（`ini` 裡
沒有這個鍵）的人才會拿到新的預設值。`GetPrivateProfileIntW(..., 1, ...)` 的
第三個參數就是這個語意。

##### 4.5.1.2 圖片專用資料夾

截圖預設落到 `SnipDir()`：

```cpp
std::wstring Settings::SnipDir() const {
    if (!snipDir.empty()) return snipDir;          // 使用者明確指定
    std::wstring base = outputDir.empty() ? DefaultOutputDir() : outputDir;
    // 去掉結尾斜線，否則會變成 D:\tmp\\Snip
    ...
    return base + L"\\Snip";
}
```

刻意跟影片分開：`outputDir` 裡混著 `.mp4` 與 `.png`，兩者用途與保留期完全
不同（截圖是一時的、影片可能留著）。分開之後整個資料夾可以一次清掉，不會
連影片一起誤刪。

**做成函式而不是載入時就把值填好**：`outputDir` 會被使用者在設定視窗改掉，
若 `SnipDir` 的結果在載入時就固定，改完輸出資料夾之後截圖還是存到舊位置 ——
而且因為路徑「已經有值」，看起來完全正常，查都不知道從哪裡查。

驗證（`tools/clipboard_hdrop_check.py`，兩種設定各跑一輪）：

```
== snip.save_file=1 ==
  ok   bitmap format present (DIB=True, BITMAP=True)
  ok   CF_DIB measures 360x200
  ok   CF_HDROP present (Explorer can paste)
  ok   exactly one file in CF_HDROP (got 1)
  ok   the file exists: D:\tmp\Snip\CapWGC_20261020_124936.png
  ok   lives in the Snip folder (dir=Snip)
  ok   timestamp name (got CapWGC_20261020_124936.png)
  ok   real PNG with 360x200

== snip.save_file=0 ==
  ok   bitmap format present (DIB=True, BITMAP=True)
  ok   CF_DIB measures 360x200
  ok   CF_HDROP absent (nothing to paste into a folder)
```

它不只看「格式存在」，還把 PNG 的 IHDR 解出來比對尺寸 —— 否則一個 0 byte
的檔案也能讓「檔案存在」通過。`CF_HDROP` 用 `DragQueryFileW` 讀，不是自己
解 `DROPFILES`：那個 blob 有 ANSI / UTF-16 兩種寫法與優先序，`DragQueryFileW`
正是檔案總管自己呼叫的 API。

做過反向驗證：拿掉寫 `CF_HDROP` 那一段 → `CF_HDROP present` 立刻變 FAIL，
其餘仍綠。順帶證明了一件容易誤會的事：日誌那行 `snip clipboard ok ... hdrop=1`
看的是「有沒有 savedPath」，**不是**剪貼簿實際內容 —— 所以它照樣印 1，
真正判準只能是去讀剪貼簿。

一個測試自己的坑：`MAKELPARAM(x, y)` 是 **x 在低位、y 在高位**。第一版寫成
`(420 << 16) | 260` 等於拖出一個 200×360 的矩形，然後斷言 360×200 ——
程式是對的，錯的是測試。

#### 4.5.2 為什麼「辨識中…」不見了，以及為什麼連氣球都不要了

第一版有：送出請求時彈一則「辨識中…」，完成時再彈一則「辨識完成」。問題是
推論只要 0.4~0.7 秒，而 Shell 對氣球有**最短顯示時間**（預設 5 秒，可在
「協助工具 → 顯示通知的時間」調整），`NOTIFYICONDATAW.uTimeout` 在 Vista
之後就被忽略 —— 程式沒有任何辦法把已顯示的氣球提早收掉。於是使用者看到的
是「字都貼好了，右下角還在說辨識中」。

第一層處理很簡單：**拿掉「辨識中…」**。不到一秒的動作不需要進度指示 ——
工具列上點的就是 OCR 那一格，按下去就知道接下來會發生什麼。

第二層處理是「讓新氣球取代舊的」。這裡先試過 `NIF_REALTIME`（0x40），
照文件的說法是「立刻取代正在顯示的那一則、不排隊」。**在 Win10/11 上這個
語意已經沒有了**，實測推翻（見下方 4.5.1.1），所以最後改成**完全不用 Shell
的通知機制**，自己開一個視窗畫提示（`ui::Toast`，見 4.5.2）。

附帶一個容易誤判的陷阱：`Shell_NotifyIconW(NIM_MODIFY, NIF_INFO)` 回傳
`TRUE` 只代表**已收下**，不代表**已顯示**。程式於是「以為自己講過了」，
而使用者什麼都沒看到 —— 這種失敗沒有任何日誌可以指出來，只能靠「畫面上
到底有沒有那則訊息」去驗。

##### 4.5.2.1 證據：氣球在 Win10/11 是走通知佇列

`src/probe/balloon_queue_test.cpp`（`build.bat balloontest`）對著一個真正的
托盤圖示連送三則 `NIF_INFO | NIF_REALTIME`，間隔 300 ms：

```
A) 連送三則 NIF_INFO|NIF_REALTIME 氣球
  post #1 accepted=1
  post #2 accepted=1
  post #3 accepted=1
  讀得到內容的氣球數: 1
B) 對照組：自己的視窗連秀三次
  show #1 visible after 0 ms
  show #2 visible after 0 ms
  show #3 visible after 0 ms

結論：三則都 accepted=1，但同時只有一則讀得到 → 排隊，不是取代。
```

三則全部「成功」，畫面上只有一則。這就是使用者回報的「彈窗沒關掉就再截圖，
後面的彈窗會有延遲感」—— 不是 CapWGC 慢，是那則訊息在**排隊等前一則講完**，
單則預設 5 秒，連按三次就會累積 15 秒的落後。

另一個不會寫在文件裡的事實：Win10 之後托盤氣球其實就是 **Action Center 的
toast**，由 `ShellExperienceHost` 繪製。所以它長什麼樣、停多久、要不要進
通知中心，全部由系統決定，程式一點都碰不到。

##### 4.5.2.2 錯誤與警告的取捨

既然不再用佇列，就沒有「新訊息會不會把舊訊息擠掉」的問題了 —— 這是自己畫
的附帶好處。但反之要自己決定「這則該停多久」，見 4.5.3 的停留時間公式：
錯誤與警告多加 1.2 秒，因為要讀完。

#### 4.5.3 `ui::Toast`：自己畫的提示

一個 `WS_EX_LAYERED` 的無邊框視窗，貼在 `SPI_GETWORKAREA` 的右下角。

| 決定 | 為什麼 |
|---|---|
| **per-pixel alpha**（32bpp DIB + `ID2D1DCRenderTarget` + `UpdateLayeredWindow(ULW_ALPHA)`）| 圓角要抗鋸齒。`SetWindowRgn` 是 1-bit 遮罩，邊緣會有階梯感；`LWA_ALPHA` 是整窗單一透明度，蓋不掉四角的直角 |
| `WS_EX_TOPMOST \| TOOLWINDOW \| NOACTIVATE \| LAYERED` | 要浮在最上層、不進 Alt+Tab、不搶焦點（搶了焦點會把使用者正在貼上的目標視窗踢掉）|
| **不加** `WS_EX_TRANSPARENT` | 點一下就能關掉。`WM_MOUSEACTIVATE` 回 `MA_NOACTIVATE`，所以點得到但不會被啟用 |
| **不設** `SetWindowDisplayAffinity` | 故意讓使用者截得到 —— 提示本身就是內容（例如「已複製 12 行」），遮掉它沒道理。對照：`ControlBar` 有遮，因為那是我們自己的操作介面 |
| 上限寬 380 DIP | 超過就折行。不折的話錯誤訊息會長成一條壓過半個螢幕的橫幅 |

**停留時間不是固定的 5 秒，而是照字數估**（`main.cpp` 的 `TrayBalloon`）：

```cpp
const size_t chars = text.size() + (title ? wcslen(title) : 0);
int ms = 2200 + (int)chars * 28;
if (kind != ui::Toast::Kind::Info) ms += 1200;   // 錯誤要多留一點時間讀
if (ms > 7000) ms = 7000;
```

「已截圖」這種四個字的訊息停留 2.3 秒就夠（再多只是擋畫面），而一則三行的
錯誤大約 4 秒。固定值兩邊都不對。

**錄影中要讓開漂浮控件**。控件在右下角、提示也在右下角：

```cpp
a.bar.Show(captureRect, a.rec->hasMic(), ...);
// 控件高 36 DIP、離邊界 8 DIP，讓開 36 + 8 + 8 = 52
a.toast.SetBottomInset(52);
```

`StopRecording` / `FinishRecording` 收掉控件時把 inset 歸零。不讓開的話，
錄影當下最需要按的「停止」會被「已開始錄影」那則提示蓋住。

##### 4.5.3.1 「點一下開啟」的動作要自己接回來

換掉 Shell 氣球時漏了一段，而且是**靜默**漏掉的：舊做法靠托盤回呼裡的
`NIN_BALLOONUSERCLICK` 呼叫 `OpenLastFile()`。氣球一換成自家視窗，那則
通知就永遠不會來 —— 但標題還寫著「已儲存（點一下開啟）」，於是點下去只把
提示關掉。**訊息在承諾一個不存在的動作**，比完全不寫還糟。

所以 `Toast::Show` 多一個可選的 `Action`：

```cpp
// ui.h
using Action = std::function<void()>;
void Show(const std::wstring& title, const std::wstring& body, Kind kind,
          int afterMs = 2600, Action onClick = {});
```

三條規則是刻意這樣定的：

| 行為 | 為什麼 |
|---|---|
| **只有左鍵**觸發動作 | 可點擊的提示旁邊常常就是別的東西。右鍵／中鍵只關掉，不該變成「用預設程式開了一個檔」 |
| 關掉就作廢（`Hide()` 清 `onClick_`）| 不清的話它會活到下一則 `Show()` 為止，中間任何一次「殘留視窗被點到」都會執行上一則的動作 |
| 可點擊時多留 2.5 秒 | 使用者得先看到「點一下開啟」才來得及點，而這種訊息常常出現在他還在忙別的事的時候 |

`Dismiss(leftClick)` 會**先抄一份回呼再 `Hide()`**：`Hide()` 負責清掉它，而
動作本身可能又要 `Show()` 一則新的（例如開啟失敗）。順序顛倒就會自己把自己的
回呼清掉。

呼叫端也有一條規則：**標題寫「點一下」就必須真的能點**。所以

- `DeliverSnip`：`snipSaveFile` 沒開、或 `SavePng` 失敗 → 標題只寫「已截圖」
  （存檔失敗時 `lastPath` 還是上一次的值，接上回呼會**開錯檔案**，比不能點更糟）；
- `FinishRecording`：成功才給回呼。

驗證分兩層，因為這是兩種不同的錯：

```
build.bat toastclicktest && out\toast_click_test.exe
  [2] actionable toast, left click
    action ran exactly once                            ok
  [3] actionable toast, right click
    action did NOT run                                 ok
  [4] action must not survive Hide()                   ok
  [5] a new plain toast must not inherit the old action ok
  PASS
```

`toast_click_test` 證明**接線**對（左鍵跑、右鍵不跑、關掉就作廢）。它證明不了
`main.cpp` 有沒有接到**對的**回呼 —— 那是一行的事，單元測試看不到。所以還有
端到端：

```
tools\toast_click_check.py
  toast: [12:11:17.052] toast 已截圖（點一下開啟）: 200 × 360 像素，已複製到剪貼簿
  saved: CapWGC_20261020_121117.png
  clicking the toast at (1443, 800)
   [12:11:17.079] open.last: D:\tmp\CapWGC_20261020_121117.png
  PASS: clicking the toast opened the file it named
```

它把 INI 的 `snip.save_file` 暫時設成 1、觸發一次框選、**讀出提示內文裡的檔名**、
點下去、再比對日誌的 `open.last:` 是不是同一個檔 —— 開錯檔也算失敗。跑完會把
INI 還原。

`toast_click_test` 做過反向驗證：把 `Dismiss(true)` 改回 `Hide()`，[2] 立刻變
`action ran exactly once  FAIL`，其餘仍綠 —— 它抓的正是那一個缺陷，不是隨便
都會過。

一個實際踩到的坑：`Log("toast %s: %s", title, body)` 的 `body` 本身含 `\n`，
所以「已存檔：xxx.png」落在**下一行**。腳本原本逐行比對，於是永遠找不到檔名、
報「nothing to open」—— 是腳本錯，不是程式錯。改成把整段拼起來再 grep。

兩個實作上的細節：

1. **`Show()` 裡先 `Paint()` 再 `ShowWindow()`**。`UpdateLayeredWindow` 是在
   `Paint()` 裡做的，順序顛倒會有一帧「空視窗」閃過去。
2. **單一 timer 編號**。連續 `Show()` 時 `SetTimer(hwnd_, 1, ...)` 用的是同一個
   id，語意就是「重設」；換 id 的話會累積一堆 timer 一起到期，把最新那一則
   提早關掉。

驗證方式與氣球不同 —— 氣球看不到內部狀態，自己的視窗可以量：

```
tools\toast_queue_check.py
  snip 1: toast visible after 22 ms
  snip 2: toast visible after 47 ms
  snip 3: toast visible after 52 ms
  3/3 notifications appeared; loop took 3.3 s
  log: [11:46:40.791] toast 已截圖: 200 × 360 像素，已複製到剪貼簿
  log: [11:46:41.878] toast 已截圖: 180 × 340 像素，已複製到剪貼簿
  log: [11:46:43.007] toast 已截圖: 160 × 320 像素，已複製到剪貼簿
  PASS: every notification replaced the previous one immediately
```

腳本連送三次框選（間隔 900 ms），輪詢 `IsWindowVisible(CapWGCToast)`，並斷言
日誌裡有三行 `toast `。50 ms 是 `ShowWindow` + 一次 `UpdateLayeredWindow` 的
成本，跟畫面複雜度無關，也不會隨次數累積 —— 這就是跟佇列的差別。

`TrayBalloon` 這個名字與 `NIIF_*` 旗標都**保留**（約 30 個呼叫點不用改），
只是內部改成轉呼叫 `toast.Show()`，同時補一行 `Log("toast ...")`：提示會自己
消失，事後要追「到底有沒有提示過」只能靠日誌。

三個連帶處理（拆面板不是只刪一行）：

| 情況 | 處理 |
|---|---|
| 辨識到 0 行 | **不動剪貼簿**（以前會塞「（沒有辨識到文字）」佔位字串，貼下去才發現是假的），改成 `ui::Toast` 警告 |
| 剪貼簿被別的程式佔住 | `OpenClipboard` 失敗 → 退回 `ui::Toast` 顯示前 120 字，文字不會憑空消失 |
| 暫存 PNG 誰刪 | 以前是「關面板時刪」，現在沒有面板 → 改在**回呼拿到結果時**刪（成功失敗都刪），另外請求送不出去時也要自己刪 |

`TextBadge` 這個類別**沒有刪** —— overlay 還用它畫選取範圍的尺寸標籤，
只是 OCR 不再用它。

驗證：

```
[22:17:48.903] ocr ok lines=1 ms=72
[22:17:48.904] ocr clip ok n_lines=1 chars=35      ← 是 clip，不是 panel shown
```

```python
# EnumWindows 掃「可見且類別/標題含 capwgc」的視窗
visible CapWGC windows: NONE
```

另外確認 `%TEMP%\CapWGC\` 在每次辨識後都回到 0 個檔案（連跑好幾次都不累積），
以及框到空白區時剪貼簿仍是空的（`CF_UNICODETEXT present: False`）。

---

## 5. 開發 / 驗證旗標

GUI 的失敗幾乎都是無聲的（覆蓋層沒開、熱鍵沒收到、編碼器初始化失敗），
所以留了一批不用互動就能驗證的入口（數量不必數，看下面的清單）：

```bat
:: 只跑 Recorder 這條路徑，輸出與 CLI 同格式的統計（寫到 out\selftest.log）
out\CapWGC.exe --selftest 3 --audio

:: 音源可以自由組合：--audio=loopback、--mic=麥克風，兩個一起就是混音
out\CapWGC.exe --selftest 4 --audio --mic

:: 再加 --split 就是「混音軌 + 每個來源各一條分軌」（tkhd 標 disabled）
out\CapWGC.exe --selftest 4 --audio --mic --split

:: 幀率（selftest 預設 60）；高刷螢幕上才有意義
out\CapWGC.exe --selftest 4 --fps 144

:: 純數學驗證幀率閘門，把「來源 × 目標」48 組全跑一遍（寫到 out\gate.log）
out\CapWGC.exe --gatestest

:: 只顯示錄製中的漂浮控件（＋左上角一塊遮罩），用來單獨看 UI 有沒有畫出來
:: 前兩秒不動，之後每半秒切一次靜音，順便驗圖示有沒有跟著換
out\CapWGC.exe --hudtest 8 --allow-capture

:: 直接開設定視窗（刻意用 0×0 的 owner，等同托盤的真實呼叫路徑）
out\CapWGC.exe --settings

:: 熱鍵的文字往返 / 拒絕 / 衝突偵測，28 個案例，寫到 out\hotkeytest.log
out\CapWGC.exe --hotkeytest

:: 直接開縮圖牆（等同熱鍵 W 的呼叫路徑）
out\CapWGC.exe --picker

:: 開縮圖牆但關掉即時刷新 —— 要拍「可重現」的畫面時用這個
out\CapWGC.exe --picker-static

:: 關掉 SetWindowDisplayAffinity，讓截圖看得到我們自己的 UI
out\CapWGC.exe --allow-capture
```

不在 GUI 本體裡的幾個（因為它們要跨行程觀察，或要能把元件單獨隔離）：

```bat
:: 遮罩面板「Destroy 後重建、同一個矩形」還畫不畫得出來（§4.1.1）
build.bat dimtest && out\dim_panel_test.exe

:: 為什麼托盤氣球被自己的 Toast 取代：三則 NIF_INFO 全都 accepted=1，
:: 但同時只有一則讀得到（§4.5.1.1）
build.bat balloontest && out\balloon_queue_test.exe

:: 「點一下開啟」真的會開檔：左鍵跑回呼、右鍵只關掉、關掉就作廢（§4.5.3.1）
build.bat toastclicktest && out\toast_click_test.exe
```

```
tools\dim_check.py    :: 量「畫面實際有沒有變暗」，含「先用微軟剪取工具」的對照
tools\toast_queue_check.py  :: 連三次截圖，量提示出現延遲（證明不再排隊）
tools\toast_click_check.py  :: 端到端：截圖存檔 → 點提示 → 開到的就是那個檔
tools\clipboard_hdrop_check.py :: 一次截圖後剪貼簿同時有 CF_DIB 與 CF_HDROP（§4.5.1）
tools\ocr_snip_check.py out\CapWGC.exe  :: 框選 → OCR → 剪貼簿，讀回剪貼簿斷言
```

`tools\clipboard_hdrop_check.py` 另外會驗那顆 PNG 是真的（讀 IHDR 取尺寸、比對
拖出去框的長寬），因為「剪貼簿說有 `CF_HDROP`」跟「那個檔案存在而且是張圖」是
兩件事。它不需要啟動程式 —— 直接 `WM_HOTKEY` 觸發截圖，再從剪貼簿讀。

`tools\dim_check.py` 直接用 `WM_HOTKEY` 驅動托盤程式，不模擬鍵盤 —— 熱鍵
注入在遠端桌面 / 非前景的情況下根本送不到目標程式，那條路測不出東西。

`--hotkeytest` 是純函式層的測試（`HotkeyText` / `HotkeyFromText` /
`HotkeyUsable` / `Duplicate`），不碰視窗，所以它可以完全自動判 PASS/FAIL。
輸出中日文要能正確顯示有兩個前提（都在 `main.cpp` 的 `PW()` 裡處理）：

| 前提 | 不做會怎樣 |
|---|---|
| `_setmode(_fileno(stdout), _O_U8TEXT)` + `_wfopen(..., L"w, ccs=UTF-8")` | `printf("%ls", 中文)` 在 "C" locale 下會截斷或變成 `?` |
| 同時寫日誌檔 | 程式是 `WINDOWS` 子系統，**沒有 stdout**；`AttachConsole` 只在從主控台啟動時有效 |

`--allow-capture` 是必要的：`WDA_EXCLUDEFROMCAPTURE` 連「使用者截圖」都會
排除，QA 截圖會看不到任何東西，很容易誤判成「元件沒畫出來」（實際踩過）。

自動化煙霧測試（會模擬熱鍵與拖曳，逐步截圖）：

```bat
powershell -ExecutionPolicy Bypass -File out\gui_smoke.ps1     :: 全流程
powershell -ExecutionPolicy Bypass -File out\gui_hud.ps1       :: 只看指示
powershell -ExecutionPolicy Bypass -File out\gui_settings.ps1  :: 只看設定視窗
powershell -ExecutionPolicy Bypass -File out\gui_settings_pos.ps1 :: 量設定視窗有沒有出界
powershell -ExecutionPolicy Bypass -File out\gui_settings_fit.ps1 :: 同上，另判 RESULT PASS/FAIL
powershell -ExecutionPolicy Bypass -File out\gui_settings_fps.ps1 :: 展開幀率下拉，數項目 + 截圖
powershell -ExecutionPolicy Bypass -File out\gui_browse.ps1    :: 挑資料夾（確定 + 取消兩條路）
powershell -ExecutionPolicy Bypass -File out\gui_picker.ps1    :: 縮圖牆三張截圖 + Esc 關閉
powershell -ExecutionPolicy Bypass -File out\gui_picker_live.ps1  :: 即時刷新（開/關視窗、不重排、不自拍）
powershell -ExecutionPolicy Bypass -File out\gui_picker_diag.ps1 :: 逐視窗列出「為什麼被丟掉」
powershell -ExecutionPolicy Bypass -File out\gui_picker_min.ps1  :: 最小化視窗：列舉 + End/Enter 還原
powershell -ExecutionPolicy Bypass -File out\gui_hotkey_capture.ps1 :: 熱鍵擷取全流程
powershell -ExecutionPolicy Bypass -File out\gui_hotkey_probe.ps1  :: 逐鍵觀察擷取
powershell -ExecutionPolicy Bypass -File out\gui_hotkey_win.ps1     :: Win 組合被擋（獨立行程）
powershell -ExecutionPolicy Bypass -File out\gui_bar.ps1        :: 控件外觀三張截圖（正常 / 靜音 / 恢復）
powershell -ExecutionPolicy Bypass -File out\gui_bar_exclude2.ps1 :: 全螢幕錄影，控件不入鏡（預設）
powershell -ExecutionPolicy Bypass -File out\gui_bar_exclude2.ps1 -AllowCapture :: 對照組，控件應該入鏡
powershell -ExecutionPolicy Bypass -File out\gui_bar_drag.ps1   :: 拖曳 → 位置寫進 INI → 下次沿用
powershell -ExecutionPolicy Bypass -File out\gui_bar_offscreen.ps1 :: 荒謬座標也會被夾回工作區
powershell -ExecutionPolicy Bypass -File out\gui_bar_region.ps1 :: 區域錄影時控件在範圍外 + 點停止鈕
powershell -ExecutionPolicy Bypass -File out\gui_bar_mute.ps1   :: 靜音後麥克風軌必須是 -999 dBFS
powershell -ExecutionPolicy Bypass -File out\gui_bar_mic_cycle.ps1 :: 錄影中關→開→關，逐段量 RMS
powershell -ExecutionPolicy Bypass -File out\gui_bar_mix_sys.ps1 :: 麥克風關掉只放 440Hz，驗系統聲有混進去
powershell -ExecutionPolicy Bypass -File out\gui_bar_aa.ps1     :: 控件邊緣特寫（8× 放大看抗鋸齒）
bash out\verify_sync.sh                                        :: 音畫同步（CLI + GUI 一起量）
```

**控件測試的兩個前提**（不做的話會得到「看起來通過」的假結果）：

1. **啟動氣泡會蓋住控件**。氣泡在右下角、而且是 topmost，比我們的控件還上層。
   第一輪測試就是被它蓋住，錄影裡看不到控件，一度以為「排除失效」。腳本開頭
   用 `Win+N` → `Esc` 把氣泡收掉。
2. **桌面要會動**。WGC 是 VFR：靜止桌面只送 2~3 幀，樣本太少。先開
   `anim_window.exe --hard` 當素材源再錄。
3. 判斷「有沒有入鏡」要看**停止鈕的紅色**（`r>150, g<110, b<110`），不要看
   亮度 —— 桌布本身就有暗塊，第一版就是這樣被騙。

`gui_settings_pos.ps1` 會把視窗矩形與 `rcWork` 一起印出來並判定 `RESULT PASS/FAIL`，
是這條坑的回歸測試。腳本存成 **UTF-8 with BOM**：PowerShell 5.1 對無 BOM 的
UTF-8 會當 ANSI 讀，中文字串的位元組會把結尾引號吃掉，錯誤訊息長得像
「字串遺漏結尾字元」而不是編碼問題。

**熱鍵 / picker 的測試腳本有兩條紀律**，違反會得到看起來像「程式壞了」的假失敗：

1. **每個步驟都要先把前景搶回來**。`keybd_event` 注入的鍵只會送到**前景視窗**，
   光是 `AttachThreadInput` + `SetFocus` 不夠。腳本裡的 `ForceForeground`
   會 `AttachThreadInput` → `BringWindowToTop` → `SetForegroundWindow`，
   再回報 `GetForegroundWindow() == h`。實測 `fg=False` 的那幾步，
   欄位會**完全沒反應**（連 `Ctrl+…` 預覽都沒有）—— 那正是最初誤判成
   「首個和弦抓不到」的原因。
2. **`Win` 組合要單獨一個行程測**。它的 keydown 會彈出開始功能表、搶走前景，
   同一行程裡後面所有注入都失效（症狀正是 `fg=False` 加 `<no ctl>`）。
   所以 `Win+Shift+P` 放在 `gui_hotkey_win.ps1`，不放在 `gui_hotkey_probe.ps1`。

另外，這些腳本**不能並行執行**：它們搶同一個 `CapWGC.exe` 與同一個前景，
會互相污染（實測並行時看到殘留的 `Shift+F7` 與 `<no ctl>`）。一律串行。

`verify_sync.sh` 一定要**同時**跑 CLI 與 GUI：這個 bug 在 CLI 上會被素材源的
誤差抵掉，只量一條路會得到「一直是對齊」的假結論（§4.7）。它會自己判
`PASS/FAIL`，判準是「|呈現差| ≤ 10ms」加「兩條路差 ≤ 10ms」。

要進一步分辨「是影像軌還是音軌偏掉」時用 `out\sync_report.py`：它把
`anim_sync.log`（flip 的真實 QPC）與錄影端記的 `videoOrigin` / `firstPacketQpc`
對起來，分別算出 `videoError` 與 `audioError`。用法見腳本開頭的說明。

---

## 6. 驗證結果（2026-10-18，3060 Ti / `tokyo`）

| 項目 | 結果 |
|---|---|
| 建置 | 五個 exe 全出，`CapWGC.exe` 539 KB |
| `--selftest 3 --audio` | inited=1 frames=82，HEVC（本機無 AV1 硬編），音訊 130 AU |
| 區域錄影（GUI 全流程） | 720×440 / 62 幀 / 1.38s，`decodes OK` |
| 音軌 | 44100 Hz 2ch，1.370s 對影像 1.400s，`decodes OK` |
| 區域尺寸 | 拖曳 260,180→980,620 得到 720×440，與選取完全一致 |
| 遮罩 | 截圖像素 132（變暗）vs 233（未變暗） |
| 錄製控件（外觀） | 66×36 膠囊，紅方塊 + 麥克風；靜音時圖示轉灰加斜線（`out/bar_on_crop.png` / `bar_muted_crop.png`） |
| **全螢幕錄影時控件不入鏡** | 181 幀中膠囊區域紅色像素 **0**；對照組（`--allow-capture`）116/116 幀都有（106 像素/幀） |
| 控件邊緣抗鋸齒 | 圓角邊緣出現部分覆蓋值（217 / 167 / 151 / 209），全圖 352 個中間值像素；改前（region 裁切）是 0（`out/bar_aa.png` 8× 放大看得到平滑弧線） |
| 控件拖曳 | 1534,816 → 拖到 387,242；INI 寫入 `bar_pos=-1213,-658`；下一次錄影回到 387,242 |
| 控件不跑出畫面 | `bar_pos=5000,-5000` → 1534,0；`-99999,99999` → 0,816；未設定 → 1534,816，三次都在 `rcWork` 內 |
| 區域錄影的控件位置 | 區域 300,200→900,600，控件落在 834,608（範圍外側、右對齊），`overlaps_region=False` |
| 停止鈕 | 點下去控件立刻消失、檔案產生（600×400，與選取一致） |
| 麥克風靜音 | 靜音後麥克風軌 `peak=-999.0 dBFS`，長度 2.731s 對影像 2.759s（時間軸沒被截斷） |
| 錄影中反覆開關麥克風 | 同一支 5.2s 檔案：關 → 開 → 關，逐 100ms RMS 是 `-42 / -999 / -42 / -999 dB`，麥克風軌 5.184s 對影像 5.217s（`out/gui_bar_mic_cycle.ps1`） |
| **系統聲 + 麥克風混成單軌** | 檔案只有 1 條音軌（44100 Hz），長度 5.201s 對影像 5.234s；麥克風關掉只放 440Hz 時，同一條軌出現 `-11.7 dB` 持續音（`out/gui_bar_mix_sys.ps1`）→ 註：本機 loopback 端點本身靜音，見 §4.7.1 的更正 |
| **保留分軌音源** | `--selftest 4 --audio --mic --split` → 3 條音軌：混音（`tkhd` flags 7）、系統（6 + `[disabled]`）、麥克風（6 + `[disabled]`）；長度 3.67 / 3.67 / 3.69s 對影像 3.70s，`audioTracks=3` |
| 分軌內容 | 逐軌 RMS：混音 `-39.1 dBFS`、mic 分軌 `-40.0 dBFS`、系統分軌 `-999 dBFS`（＝這台機器 loopback 本來就沒聲，分軌忠實反映來源） |
| **幀率選項** | 下拉 6 項：30 / 60 / 120 / 144 / 165 / 240，144 以上標「（高刷螢幕）」（`out/gui_settings_fps.ps1`，`fps items = 6`） |
| **幀率閘門（數學）** | `--gatestest` 48 組全 PASS：239Hz→144 = 144.0、165Hz→144 = 144.0（舊的硬下界會是 119 / 82）、60→60 = 60.0 |
| 幀率（實機，239Hz 螢幕 + 60fps 內容） | 目標 60 → 58.3 fps；144 → 90.5 fps；240 → 115.4 fps，`dropped=0`、`throttled≤2` |
| 設定視窗 | 輸出資料夾、編碼器、幀率、品質、三個音源/游標開關、分軌勾選、**截圖存檔 + 圖片資料夾**、四個熱鍵欄位（OCR 模型欄位已移除，見 §11.4） |
| 設定視窗落點 | 562,147 **476×652**（客戶區 460×613），`rcWork` 0,0 1600×852 → `RESULT PASS` 完全在工作區內 |
| 熱鍵文字往返 | `--hotkeytest` **28/28 PASS**（含 `Numpad7` / `VK%02X` / 別名 / 大小寫 / 空白） |
| 熱鍵擷取（逐鍵） | `Ctrl+…` → `Ctrl+Shift+…` → `Ctrl+Shift+F7`，立即回饋正常 |
| 熱鍵擷取（清除） | `Esc` → `（未設定）` |
| 熱鍵擷取（拒絕） | 無修飾鍵 → `熱鍵至少要有一個修飾鍵（Ctrl / Alt / Shift）` |
| 熱鍵擷取（Win） | `Win+Shift+P` 被擋，欄位維持 `Ctrl+Alt+A` 不被污染，紅字說明原因 |
| 熱鍵衝突 | 「停止」設成 `Ctrl+Alt+A` → 按確定被擋：`「選區域」與「停止」綁了同一組熱鍵` |
| 熱鍵套用 | 修好後按確定 → 視窗關閉，INI `[hotkeys]` 四項正確 |
| 縮圖牆（視窗分頁） | 820×700，z-order 排出記事本/小算盤/Chrome/WorkBuddy 等 12 張縮圖，全部有內容 |
| 縮圖牆（螢幕分頁） | Tab 切換正常，顯示「螢幕 1 1600×900 主螢幕」 |
| 縮圖牆（鍵鼠） | 方向鍵移動選取框、Enter / 雙擊確認、`Esc` 關閉（`after-esc picker=0`） |
| **縮圖牆（完整性）** | 25 個視窗全數列出（修前只有 7 個）：4 個 `printwindow` + 1 個前景 `screen` + 20 個最小化；`picker.log` 的 `windows=25` 與 `gui_picker_diag.ps1` 的 ACCEPT 對得上 |
| 縮圖牆（排序） | 沒最小化的排前面、最小化的排最後；首頁 9 格都是有內容的縮圖，不再是一整片灰塊 |
| 縮圖牆（佔位圖） | 拍不到畫面時畫程式圖示：資料夾、Discord、VS Code、Windows Terminal 一眼認得出 |
| 縮圖牆（還原） | `End` 選到最小化項 → `Enter`：日誌 `restored minimized window ... iconic=0`、`end ok=1 hwnd=...` |
| **縮圖牆（即時更新）** | `gui_picker_live.ps1` **12/12 PASS**：開新視窗 → 2.6s 內 `refresh: + added hwnd=...`（hwnd 精確比對）；關掉 → `refresh: - removed hwnd=...`；首項未被重排；即時刷新期間 `how=screen` 出現 **0** 次（不自拍）；`Esc` 正常關閉 |
| 縮圖牆（`--picker-static`） | `begin live=0`，等待 1.5s 期間**零** `refresh` 行 → 即時刷新確實關掉 |
| 挑資料夾（確定） | 樹狀檢視選 root → `SHBrowseForFolderW` 回 `C:\Users\Administrator\Desktop`，欄位寫回，`Responding = True` |
| 挑資料夾（取消） | 欄位維持原值，行程正常，`browse.log` 兩行 `browse done` / `write back ... ok` |
| 音畫同步（CLI） | `gap=+99.5ms` → 呈現差 **−2.7 ~ +3.6ms**（四次），PASS |
| 音畫同步（GUI） | `gap=+10.2ms` → 呈現差 **−6.0 ~ +3.9ms**（四次），PASS |
| 兩條路一致性 | 差 **1.3 ~ 9.6ms**（修正前是 62ms） |
| 托盤 | 啟動氣泡、錄製中圖示轉紅、停止後氣泡（點一下開檔）、選單熱鍵文字跟著自訂值變 |

音訊量到 `peak=-999.0`（全靜音）是正常的：這台當時沒有播放任何聲音，
loopback 的 keep-alive 仍在送靜音封包 —— 那正是 `docs/07` 那個坑的正確行為。

---

## 7. 踩過的坑

| 坑 | 症狀 | 解法 |
|---|---|---|
| **`WDA_EXCLUDEFROMCAPTURE` 讓 QA 截圖看不到自己的 UI** | 視窗 `IsWindowVisible` 為真、`GetWindowRect` 正確、D2D 零錯誤，但截圖上一片空白 → 誤判成「元件沒畫出來」，繞了兩輪 | 遮罩與外框改成不設（它們本來就不在擷取範圍內）；徽章保留但加 `--allow-capture` 旗標關掉 |
| **啟動氣泡是 topmost，會蓋住控件** | 全螢幕錄影的成品裡看不到控件，第一輪因此誤判「排除失效」（其實是氣泡擋住） | 測試腳本先用 `Win+N` → `Esc` 收掉氣泡再錄 |
| **用亮度判斷「控件有沒有入鏡」** | 桌布本身就有暗塊，`dark_px` 統計看起來像控件 → 假陽性 | 用停止鈕的紅色當簽名（`r>150, g<110, b<110`），UI 上沒有第二個地方是那種紅 |
| **靜止桌面只有 2~3 幀** | 樣本太少，排除測試等於沒測 | WGC 是 VFR。先開 `anim_window --hard` 讓畫面動，再錄 |
| **用 region 裁圓角** | 邊緣一格一格的階梯，使用者說「像素感太強」 | region 是 1-bit 遮罩，天生沒有抗鋸齒。改 per-pixel alpha（DIB + D2D DC target + `UpdateLayeredWindow`），形狀交給 alpha 通道 |
| **`UpdateLayeredWindow` 之後又呼叫 `SetLayeredWindowAttributes`** | 視窗完全不更新，看起來像「控件沒畫出來」 | 兩者互斥。用了 ULW 就不要再設 `LWA_ALPHA`，整體透明度改用 `BLENDFUNCTION::SourceConstantAlpha` |
| **per-pixel alpha 讓透明區也吃滑鼠** | 圓角外面那幾塊看不見的地方點得到，會擋住底下的視窗 | `WM_NCHITTEST` 讀 DIB 的 alpha，`< 8` 回 `HTTRANSPARENT` |
| **DIB 用 bottom-up** | `AlphaAt` 的列索引要反過來算，很容易寫錯 | `biHeight = -h` 存成 top-down，索引就是 `y * w + x` |
| **D2D 的弧掃掠方向畫反** | 麥克風圖示變成一把雨傘（弧從上面繞過去） | D2D 角度是「從 +x 軸順時針量」而 y 軸向下，180°→360° 順時針會走**上面**。要 `D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE` |
| **靜音用「跳過不編碼」實作** | 音軌的累計取樣點停住，之後每個 AU 的 dts 往前擠，聽起來整段被加速；尾巴補靜音也補錯長度 | 送**零樣本**進編碼器：時間軸一個 tick 都不動，AU 數也照常 |
| **每個音源各寫一條軌** | 使用者「勾了麥克風，播放卻沒聲音」——檔案裡有聲音（`peak=-0.0 dBFS`），但播放器只播第一條，而那條是安靜的 loopback | 多來源混成**一條**軌（§4.7.1）。判斷依據不能只看「檔案有沒有聲音」，要看「播放器會播哪一條」 |
| **混音直接 `int16 + int16`** | 相加溢位繞回，聽起來是爆音 | 用 `int` 加完再 clamp 到 int16 |
| **重取樣每次 packet 相位歸零** | 每個 packet 接縫會有一聲「喀」 | 保留小數相位（`pos_`）與最後一個輸入 frame，跨 packet 連續內插 |
| **控件位置存絕對座標** | 在 1600×900 拖到角落，換 1920×1080 就離邊界很遠 | 存相對擷取範圍右下角的位移（`[ui] bar_pos=dx,dy`） |
| **用 `PostMessage` 的 client 座標測拖曳** | 視窗根本沒動 —— 拖曳讀的是 `GetCursorPos`，不是訊息裡的座標 | 測試要用 `SetCursorPos` + `mouse_event`（真的移動游標） |
| **沒有訊息迴圈就沒有 `WM_PAINT`** | 測試模式裡視窗存在但空白，跟「元件壞了」一模一樣 | 測試迴圈自己 `PeekMessage` 抽訊息 |
| **`Begin()` 裡先 `SetCapture`** | 框選一打開就自己結束，還選到一個沒碰過的視窗 | 只在 `WM_LBUTTONDOWN` 時 capture。按下之前抓滑鼠會把第一次點擊吃掉 |
| **均勻變暗在截圖上肉眼看不出來** | 整片都變暗，沒有對照組，看起來跟正常一樣 | 量像素（132 vs 233），不要憑眼睛 |
| **`Stop()` 之後 `stats().audioAU` 變 0** | 檔案本身音軌正常，但統計說 0 個 AU | `Stop()` 會清掉 `audio` 容器；AU 數改用獨立的原子計數，不從逐源加總 |
| **`frames == 0` 被當成成功** | 完全靜止的視窗只送「開編碼器那一幀」，成品是空檔，卻報「已儲存」 | 把 `frames == 0` 也算失敗，訊息寫「沒有寫入任何畫面（目標可能完全靜止）」 |
| **音源開不起來被靜默跳過** | 勾了麥克風卻沒聲音，使用者只會以為程式壞了 | `RecorderStats::audioNote` 帶出原因，氣泡改成警告色 |
| **點「瀏覽…」整個行程卡死** | 對話框不出現、UI 沒有回應、CPU 0%，只能砍行程。看起來像「當機」 | `SHBrowseForFolderW` + `BIF_NEWDIALOGSTYLE` 需要 STA，主執行緒是 MTA（音訊要求）。改到自己的 STA 執行緒，主執行緒 `MsgWaitForMultipleObjects` + 抽訊息 |
| **測試腳本用 `GetWindowText` 讀別人的 EDIT** | 欄位明明已經寫回 `...\Desktop`，腳本讀到的還是舊值 → 誤判成「功能壞了」 | 跨行程讀控制項文字要用 `SendMessage(WM_GETTEXT)` |
| 設定視窗開在螢幕外 | 標題列在 `y=-184`，視窗是 `WS_POPUP`、沒有工作列按鈕，**拖不回來** | 見 4.4：owner 是 0×0 時改用滑鼠所在螢幕的工作區，最後一律夾回 `rcWork` |
| **QA 入口傳 `nullptr`，把 bug 遮掉了** | `--settings` 走的是「無 owner」那條路，置中正常；托盤走的是 0×0 owner 那條，出界。截圖驗過卻還是壞的 | QA 入口要模擬**真實**呼叫：`--settings` 改成建一個 0×0 owner 再開 |
| **錄出來聲音比畫面早（嘴型對不上）** | 使用者用 PotPlayer 播，聲音早幾百毫秒；CLI 錄同一畫面卻是好的 | 音軌第一筆樣本的內容時刻是 `videoOrigin + gap`，但 muxer 放在 `dts=0` → 整條早 `gap`。逐條軌補 `+gap`（§4.7） |
| **只量 CLI 的同步數字** | CLI 的 `gap` 剛好抵掉素材源的 `PlaySound` 延遲，永遠顯示「對齊」，bug 藏了好幾輪 | 回歸測試必須同時量 CLI 與 GUI 並斷言一致（`verify_sync.sh`） |
| 設定視窗的資料夾欄位是空的 | 開了才發現要重填 | EDIT 的文字在 `CreateWindowExW` 時就給，不要事後補 |
| `LNK1104: 無法開啟 CapWGC.exe` | 看起來像編譯失敗 | exe 還在跑。GUI 也一樣，建置前先 `taskkill`（HANDOFF 已記） |
| **即時刷新時從螢幕抓縮圖** | 卡片裡出現縮圖牆**自己**（topmost 蓋在目標上） | 即時路徑不做 `BitBlt` 回退（`how=live-skip`），只信 `PrintWindow`；監視器縮圖只在開窗時抓一次 |
| **即時刷新重建整個清單** | 使用者正在看第二頁，清單每個週期重排一次，選取亂跳 | 用 `hwnd` 當身分原地對齊：既有留原位、新的插尾巴、只在尺寸/最小化狀態變時才重抓縮圖 |
| **`hover_` 是索引** | 刪掉一項之後，游標在 A 上但 B 亮著 | 清單一變就用游標位置重新 `HitCard` 一次 |
| **刷新與首次抓圖搶同一批 Item** | 首次那批是唯一能 `BitBlt` 到前景視窗的機會，被輪替插隊就沒了 | `initialDone_` 旗標：首次縮圖抓完才啟動計時器 |
| **改小視窗後縮圖還是舊尺寸** | 卡片裡的畫面被拉扁 | 記 `thumbSrcW/H` 與 `thumbWasMinimized`，變了才作廢重抓 |
| **拿 `notepad` 當「視窗出現」的測試標記** | 測試永遠失敗：Win11 的記事本是單實例，再開也只是換標題、不產生新視窗 | 用 `out\anim_window.exe`（每次啟動都是新 hwnd）；斷言用 **hwnd 精確比對**，不要用標題（會受單實例與本地化影響） |
| 測試腳本的 `FindWindow` 回 0 | 視窗明明存在 | P/Invoke 的 `DllImport` 預設 ANSI，`FindWindowW` 的字串被截斷。宣告 `CharSet = CharSet.Unicode`，或直接用 `EnumWindows` 依 class 找 |
| **熱鍵欄位「抓不到鍵」** | 點進去按什麼都沒反應，看起來像控制項壞了 | `WM_GETDLGCODE` 沒回 `DLGC_WANTALLKEYS` 的話，Enter 被對話框吃掉（直接關窗）、Esc 關窗、Tab 跳走。回了之後 Tab 要自己轉（`GetNextDlgTabItem`） |
| **測試端只做 `SetFocus` 不夠** | `focus[1020]=True` 但欄位完全沒反應，誤判成「首個和弦抓不到」，繞了一輪 | `keybd_event` 注入的鍵只送**前景視窗**。`AttachThreadInput` + `SetFocus` 不會讓視窗變前景。每個步驟都要 `BringWindowToTop` + `SetForegroundWindow` 並斷言 `GetForegroundWindow() == h` |
| **`PostMessage` 送不出熱鍵** | 用 `PostMessage(WM_KEYDOWN)` 測，欄位永遠抓不到 | 修飾鍵是用 `GetKeyState` 讀**真實鍵盤狀態**的（不是事件裡的旗標），`PostMessage` 不會改變它。測試必須用 `keybd_event` |
| **`Win` 組合鍵是無聲死路** | 欄位停在 `Shift+Win+…`，後續那個鍵永遠不來，使用者只覺得「按了沒反應」 | shell 先接走 Win+<key>（還彈出開始功能表）。在「只按了修飾鍵」那個分支就擋掉並說明，不要留半截狀態 |
| **`Win` 測試污染整條測試序列** | 跑完 Win 那步之後所有注入都失效（`fg=False`、`<no ctl>`），看起來像程式連環壞掉 | Win 的 keydown 彈出開始功能表、搶走前景。Win 案例拆到獨立行程（`gui_hotkey_win.ps1`） |
| **測試腳本並行執行互相污染** | 看到殘留的 `Shift+F7`、`<no ctl>`，誤判成競態 bug | 它們搶同一個 exe 與同一個前景。一律串行 |
| **PowerShell 變數不分大小寫** | `Chord($WIN,$SHIFT,$P)` 說「找不到超載」，因為 `$P` 是前面 `-PassThru` 的 Process 物件 | `$p` 與 `$P` 是同一個變數。鍵碼變數改名（`$VKP`） |
| **改熱鍵後整套失效** | 新組合註冊失敗，但舊的已經 `Unregister` → 按什麼都沒反應，UI 還顯示新值 | `ApplyHotkeys` 註冊失敗要把**舊的整套註冊回去**（§4.8.2）；且只回滾熱鍵，其餘設定照收 |
| **`RegisterAll` 不先清舊值** | 改熱鍵時「舊值」被算成佔用者，改完反而失敗 | 每次 `RegisterAll` 先 `UnregisterAll` |
| **`printf("%ls", 中文)` 顯示 `?`** | `--hotkeytest` 的中文全變成 `?` 或截斷 | 預設 "C" locale。要 `_setmode(_fileno(stdout), _O_U8TEXT)`，日誌檔要 `_wfopen(..., L"w, ccs=UTF-8")` |
| **`WINDOWS` 子系統程式沒有 stdout** | 測試輸出看不到 | 同時寫日誌檔；`AttachConsole` 只在從主控台啟動時有效 |
| **縮圖牆一排黑格子** | UWP 程式的縮圖全是黑的 | `EnumWindows` 把 cloaked 的 UWP 空殼也算成可見。要過濾 `DwmGetWindowAttribute(DWMWA_CLOAKED)` |
| **列舉時把最小化的視窗直接丟掉** | 使用者說「可選窗口變少了，不齊全」：VS Code / Discord / Windows Terminal 明明開著，清單裡卻只有 7 個 | `IsIconic` 不是丟棄理由。用 `GetWindowPlacement().rcNormalPosition` 當矩形（`GetWindowRect` 對最小化視窗只回 160×28 的圖示大小），`it.minimized` 標記起來，確認時先 `SW_RESTORE` |
| **`GetWindowTextW` 空字串就丟** | UWP 的「設定」「小算盤」整個從清單消失 —— 它們的標題在子視窗上 | `EnumChildWindows` 撈第一個非空標題；連子視窗都沒標題才退回 exe 名，不要讓它消失 |
| **清單齊了但排壞了** | 首頁 8~9 格全是「無法預覽」，看起來還是「東西變少了」 | z-order 會把最小化的擠到前面。`stable_sort` 讓沒最小化的排前面，最小化的畫程式圖示而不是灰塊 |
| **`WM_GETICON` 用 `SendMessage`** | 清單裡只要有一個卡住的視窗，整個選擇器就卡在那裡 | `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 120ms)`；拿到的圖示 `CopyIcon` 成自己的，`ReleaseThumbs` 裡 `DestroyIcon`，不然每開一次漏一批 GDI 物件 |
| **上限截斷是無聲的** | 超過 `kMaxWindows` 的視窗默默不見，症狀與上面的「不齊全」一模一樣 | 截斷時留一行 `[pick] window list truncated at 40`，讓「少了」有痕跡可查 |
| **`PrintWindow` 成功卻給黑圖** | 回傳成功，縮圖卻是一片黑，被當成有效縮圖 | 要 `MostlyBlack` 取樣檢查，黑的標記為 placeholder；前景視窗再試 `BitBlt(SRCCOPY \| CAPTUREBLT)` |
| **縮圖一張張跳出來** | 而且前景視窗那張抓到「選擇器自己」 | 順序要**先抓完所有縮圖再顯示自己**，不要先顯示再逐張抓 |
| **跨行程 `GetFocus` 拿不到焦點** | 以為焦點設定失敗 | 跨行程 `SetFocus` 一定要先 `AttachThreadInput` 把兩條輸入佇列接起來 |

---

## 8. 還沒做

按優先級：

1. **錄後編輯 / GIF 匯出**。差異化最大的一塊，現在有了 UI 才有依附的地方
   （時間軸、預覽）。
2. **托盤上的目標記憶**。縮圖牆解決了「怎麼挑」，但每次 `Ctrl+Alt+W` 還是要
   重挑；應該記住上次的來源直接重錄。
3. **多螢幕的目標選擇**。縮圖牆的「螢幕」分頁已經列得出每一顆螢幕，
   但只有一顆時沒測到多螢幕的排版；`Ctrl+Alt+R` 也還是「游標所在那顆」。
4. ~~**縮圖牆的即時更新**~~ **已完成（2026-10-18 第七輪）**。清單 400ms
   重新比對（`hwnd` 為身分、不重排、保留選取），縮圖 120ms 分批補抓（每次
   上限 2 張、新出現的優先）。即時路徑不做螢幕擷取回退（會拍到自己），
   監視器縮圖維持開窗時抓一次。`--picker-static` 可關掉即時刷新。
   驗收 `out/gui_picker_live.ps1` **12/12 PASS**。見 §4.9.5。
   剩餘可做的：縮圖牆的縮圖**內容**本身不會自動更新（只有新視窗/改尺寸才
   重抓），要「畫面內容也跟著動」得改成定時重抓同一批 —— 但那個成本高、
   且會讓 PrintWindow 一直打擾目標視窗，先不做。
5. **熱鍵的 Win 支援**。需要用 low-level keyboard hook（`WH_KEYBOARD_LL`）
   才攔得到 Win 組合，成本比現行 `RegisterHotKey` 高，先不做。
6. **B 幀**。`Recorder` 目前寫死 `ec.bFrames = 0`，等 MF 那邊的
   `BPictureCount` 問題解決（HANDOFF 待辦 4）再開。
7. **AAC priming（23~46ms）**。與 CLI 同一個缺口，等 `elst` 的
   `media_time` 補上。
8. **首次啟動的引導**。現在只有一個氣泡寫熱鍵，使用者不一定看得到。
