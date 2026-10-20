# CapWGC 任務交接

> 把這份文件整份貼給新的對話窗口即可接手。內容自包含，不需要讀其他檔案也能開始。

---

## 你的角色與專案目標

我們在做 **CapWGC**：一個 MIT 授權的 Windows 錄屏軟體。目標是「比 Captura 更好用、更輕量、壓縮率更高」，集各家之長：

- Captura 的功能廣度（區域/視窗/螢幕、GIF）
- LICEcap 的啟動速度（< 1MB、秒開）
- OBS 的編碼質量（硬體編碼、VFR、音訊多軌）
- ScreenToGif 的錄後編輯

**硬約束：不能用 FFmpeg**（LGPL/GPL 會污染 MIT）。編碼走 Media Foundation 或 NVENC SDK 動態載入。

**溝通約定**：繁體中文，不要用驚嘆號。

工作目錄：**4070 Ti 那台是 `E:\workspace\CapWGC`，3060 Ti（`tokyo`）這台是
`D:\workspace\CapWGC`**。下面所有路徑都相對於專案根目錄，不要寫死磁碟機。

---

## 目前進度

**M1（SinkWriter 版）與 M2（自驅動 MFT + 自寫 muxer + fMP4）都已完成，驗收標準全部通過。
M3 的 AV1 也已完成**（4070 Ti 上端到端跑通，含 av1C + OBU 容器寫入）。
**音軌也已打通**（WASAPI loopback → AAC → muxer 第二條 trak/traf），
**GUI 的多個音源會混成一條軌**（系統聲 + 麥克風相加，避免播放器只播第一條
而聽不到麥克風），CLI 則維持每個來源一條軌以便逐條量測，
**容器能把音軌延遲寫進 `edts/elst` / `tfdt` 並且真的生效**。
延遲的值 = `firstPacketQpc − videoOrigin`（音軌比影像零點晚起跑多少），
現在**預設自動補**。這條在 2026-10 之前是反的（預設不補償），原因是一個
「素材源延遲抵掉管線延遲」的量測假象 —— 詳見 `docs/08` §6.6.1。

**GUI 也已經有了**（2026-10-18）：托盤 + 全域熱鍵（**使用者可自訂並持久化**）
+ 拖曳框選（或單擊選視窗）+ **Discord 式縮圖牆來源選擇器** + **錄製中漂浮控件**
（兩顆圖示：停止 / 麥克風靜音，可拖曳且記住位置，全螢幕錄影也不會入鏡）
+ 設定視窗。這是最短閉環，沒有時間軸與編輯器。
為了不讓錄製迴圈出現第二個實作，先把引擎那一段抽成 `src/core/recorder.h/.cpp`，
GUI 只負責給參數與啟停；CLI 保留自己那一份（所有量測腳本依賴它的輸出行為）。
細節、設計取捨與踩過的坑見 `docs/09-gui.md`。

**2026-10-20 第八輪（六項調整）**：

1. 托盤「選擇視窗」移到選單第一位（最常用、且不會立刻開始做任何事）。
2. **選擇器開著時截圖 / OCR 仍然可用**。根因是 picker 自己的 modal 迴圈把
   `WM_HOTKEY` 抽出來丟掉；新增 `SysMsgFn` 攔截（`PickSource` 的第三個參數），
   攔到就先關自己、再由呼叫端 `PostMessage` 重播訊息。托盤的截圖項也改走
   `WM_APP_SNIP`，兩條入口共用同一道門。
3. 熱鍵文件全面對齊現況（預設只綁兩個、其餘「未綁定」）。
4. **錄影模式的框選修好**：`Overlay::Submit` 原本有 `if (!snip_ || !owner_) return;`
   守衛，把錄影模式的四條出口（放開 / 單擊 / Enter / 雙擊）全部靜默吃掉。
5. 設定視窗改成三分頁（一般 / OCR / 熱鍵），476×720 → **476×364**。切頁只做
   `ShowWindow`，不重建控制項（見 `docs/09` §4.11）。
6. **PrtScn 接管預設開啟**：`Hotkeys::Default().snip = {0, VK_SNAPSHOT}` +
   `AutoEngagePrtScn` 啟動時自動接管，用 `[prtsc] chosen` 記號區分「沒問過」
   與「明確選過別的鍵」（見 `docs/09` §4.8.5）。
7. 分頁上線後的三個修正：**分頁標題亂碼**（專案沒定義 `/DUNICODE`，
   `TabCtrl_InsertItem` 展開成 `TCM_INSERTITEMA`，寬字串被當 `char*` 讀 →
   第一格空白、第二格只剩 `O`、第三格亂碼）、**OCR 頁最後一行被切一半**
   （`STATIC` 高度按 `\r\n` 個數給，但長的那行自己折行 → 少算一行）、
   **熱鍵頁把「視窗清單」排到第一行**（`kHkOrder[]` 只管顯示順序，寫回
   settings 的對應不變）。驗收腳本 `out/gui_settings_tabs.ps1` 三頁各拍一張，
   見 `docs/09` §4.11.4 / §4.11.5。

**OCR 也已經有了**（2026-10-20）：截圖後可直接取字並放進剪貼簿。推論在**行程內**
（`src/core/ocr_onnx.cpp`，ONNX Runtime CPU provider），不經 Python、使用者不必
裝任何東西；出貨包就是 `CapWGC.exe` + `onnxruntime.dll` + `models\` 三樣。
少了 `models\` 或 dll 只會讓 OCR 那一格變灰，錄影截圖照常（連結時
`/DELAYLOAD` + 啟動時先探測 dll 與 API 版本，見 `docs/11` §11.5）。
前後處理與 paddlex 的 Python 實作一比一對齊，同一張圖 10 行文字完全相同；
360×180 的截圖 8~21 行約 391~656 ms。部署與打包見 `docs/11-ocr-deployment.md`。
刻意**沒有**「辨識中…」的氣球：推論不到一秒，而 Shell 對氣球有 5 秒最短顯示
時間且 `uTimeout` 已失效，會變成「字都貼好了還在說辨識中」（見 `docs/09` §4.5.2）。

**提示訊息已經不用 Shell 的托盤氣球了**（2026-10-20）：改成自己畫的
`ui::Toast`。原因是「彈窗沒關掉就再截圖，後面的彈窗會有延遲感」這個回報 ——
先試過 `NIF_REALTIME`（文件說會取代正在顯示的那一則），**在 Win10/11 上沒有
這個語意**。`build.bat balloontest` 對真正的托盤圖示連送三則，三則全部
`accepted=1`，但同時只有一則讀得到：托盤氣球現在就是 Action Center 的 toast，
走的是**通知佇列**，單則預設 5 秒，連按三次就落後 15 秒。
`Toast` 是一個 `WS_EX_LAYERED` 的無邊框視窗（per-pixel alpha，
`UpdateLayeredWindow`），`Show()` 永遠立刻換掉畫面上那一則，實測 22/47/52 ms。
停留時間照字數估（2.2 秒 + 每字 28 ms，錯誤再加 1.2 秒，上限 7 秒）；錄影中
會讓開右下角的漂浮控件（`SetBottomInset(52)`），否則「停止」會被提示蓋住。
見 `docs/09` §4.5.2，驗證用 `tools\toast_queue_check.py`。順帶一個陷阱：
`Shell_NotifyIconW` 回 `TRUE` 只代表**已收下**，不代表**已顯示**。

**換掉氣球時漏掉了一段，補回來了**：「已儲存（點一下開啟）」的開啟動作原本靠
托盤回呼的 `NIN_BALLOONUSERCLICK`，改用自家 `Toast` 之後那則通知永遠不會來 ——
標題還寫著「點一下開啟」，點下去只把提示關掉。`Toast::Show` 現在收一個
`ui::Toast::Action`：左鍵才執行（右鍵／中鍵只關掉）、`Hide()` 就作廢（不然會
執行到上一則的動作）、可點擊時多留 2.5 秒。呼叫端也有規則 —— 標題寫「點一下」
就必須真的能點：`snipSaveFile` 沒開或存檔失敗時標題只寫「已截圖」，因為那時
`lastPath` 還是上一次的值，接上回呼會**開錯檔案**。驗證兩層：
`build.bat toastclicktest`（接線，做過反向驗證）與 `tools\toast_click_check.py`
（端到端，比對開到的檔名是不是剛存的那個）。見 `docs/09` §4.5.3.1。

**截圖可以同時貼到小畫家與檔案總管，但前提是那個檔案真的存在**（2026-10-20）：
剪貼簿本來就同時放多種格式，所以「兩個都能貼」不是取捨問題，是**要不要多放一個
`CF_HDROP`** 的問題。但 `CF_HDROP` 帶的是**檔名**，不是像素 —— 檔案總管按 Ctrl+V
做的是「複製那個檔案」，它從不去讀 `CF_DIB`。所以存檔必須發生在寫剪貼簿**之前**
（`DeliverSnip` 的順序就是這麼排的），否則貼出來是一次空操作。

這個依賴關係直接改變了預設值：`[snip] save_file` 從**關**改成**開**。原本關著的
理由是「只是想貼到別的地方，硬存檔會積一堆檔案」，但既然「貼到資料夾」這個功能
本身就靠存檔，預設關掉等於預設讓那個選項不存在。改預設值有一個必須確認的地方：
**已經選過「不要存」的人不能被影響** —— 他們的 ini 裡有 `save_file=0` 這一行，
照舊；只有從來沒碰過這個設定（ini 裡沒有這個鍵）的人才會拿到新的預設值。這就是
`GetPrivateProfileIntW(section, key, default, ini)` 的語意，也是它值得信任的原因。

圖片另外收在一個專用資料夾（`[snip] dir`，留空＝`輸出資料夾\Snip`），檔名沿用
時間戳。`SnipDir()` 寫成**函式**而不是在載入時算好：`outputDir` 會被使用者在設定
視窗改掉，若在載入時就固定，改完輸出資料夾之後截圖還是存到舊位置 —— 而且因為
路徑已經有值，看起來完全正常，是那種「沒有錯誤訊息的錯」。見 `docs/09` §4.5.1，
驗證用 `tools\clipboard_hdrop_check.py`（檢查兩種格式都在、PNG 是真 PNG）。

**遮罩面板的生命週期也修過一個間歇性 bug**（2026-10-20）：`DimPanel` /
`BorderWindow` 每次框選都重建，但 `Destroy` 沒把 `visible_` / `last_` 歸零，
下次拿到同一個矩形時會被「矩形沒變就什麼都不做」的早期返回吃掉，症狀是
「覆蓋層開起來了但畫面不變暗」。現在有 `build.bat dimtest` 釘住它。

細節見 `docs/05-m2-status.md`、`docs/06-av1-status.md`、`docs/07-audio.md`、
`docs/08-multi-audio.md`，這裡只放結論：

| 驗收標準 | 結果 |
|---|---|
| HEVC 檔案 < H.264 的 80%（同等畫質） | 通過，63~75% |
| CPU < 1 ms/幀 @720p | 通過，HEVC 0.86 / H.264 0.54 |
| 1080p60 錄 10 分鐘不掉幀 | 通過 |
| 強制 kill 後檔案仍可播 | 通過（fMP4，960 幀完整解出） |
| `--quality` 改動看得到大小變化 | 通過，2.6→5.6→10.8→15.2 MB |
| **AV1 產出的檔案可解** | **通過**，30 秒 949 幀 decodes OK |
| **AV1 < H.264（同等畫質）** | **高熵 52~59%；UI 63~66%**（2026-10-17 第二輪，同批錨點重測） |
| **AV1 vs HEVC（同等畫質）** | **高熵 91~100%（中位 98%）；UI 反而 101~121%**（八點各一組，3060 Ti 量） |
| **音軌可解且對齊影像** | **通過**，放測試音 `peak=-13.0 dBFS`；6 秒錄影影像 5.968s / 音訊 5.968s（差 0 ms） |
| **有音軌時 fMP4 仍抗 kill** | **通過**，每個 moof 雙 traf（trk1 影像 + trk2 音訊） |
| **多音源（系統聲 + 麥克風）各一條軌** | **通過**，44.1k 與 48k 兩軌同時解開（rms −9.0 / −36.5） |
| **三條音源 + 閒置裝置** | **通過**，3 條 trak 時長 6.246/6.251/6.251s，閒置那條 `decodes OK (silent)` |
| **多軌 fMP4 抗 kill** | **通過**，每個 moof 三個 traf（120 幀 + 85~95 AU ×2），kill 後兩條音軌都解得出 6s |
| **A/V 偏移量測**（QPC 記帳，不是音畫差） | 單路 56.2~59.4ms、`--ab` 125.8ms、`--ab --ref` 163.8ms |
| **延遲能寫進容器** | **通過**，progressive 寫 `elst` empty edit、fMP4 寫第一個 `tfdt`；宣告 60ms → 實測呈現差 +57.5 / +65.0ms |
| **補償能抵掉真實延遲** | **通過**，`--audio-delay 500 --av-offset 500` → 殘差 +15.5ms |
| **推導的偏移值可信** | **通過（2026-10 改寫）**，推導值 = `firstPacketQpc − videoOrigin` = 音軌比影像零點晚起跑多少，就是該補的量。舊結論「不通過、預設不補償」是兩個誤差互相抵掉的假象（見下方「已經踩過的坑」最後一條） |
| **GUI 與 CLI 兩條路一致** | **通過**，`verify_sync.sh`：CLI −2.7~+3.6ms、GUI −6.0~+3.9ms（各四次），兩者差 1.3~9.6ms（修正前 +5.8 / +67.5，差 62ms） |
| **負偏移（音訊偏晚）能裁** | **通過**，fMP4 −150ms → 量到 −136.6（丟 6 AU）；progressive `elst media_time=6615/44100` → 量到 −151.0 |
| **換硬體不用改程式** | **通過**，偏移由 `out\calibrate.sh` 量測後存校準檔，`--av-offset auto` 讀它；本機量到 +6.3ms 落在門檻內寫 0 |
| **GUI：拖曳框選 → 錄 → 停 → 拿到檔案** | **通過**，拖 260,180→980,620 得到 720×440，62 幀 / 1.38s，`decodes OK` |
| **GUI 錄出的音軌** | **通過**，44100 Hz 2ch 1.370s 對影像 1.400s，差 30ms（AAC 一幀量化） |
| **GUI 的指示不會入鏡** | **通過**，全螢幕 181 幀中控件區域紅色像素 0（對照組 116/116 幀都有）；控件靠 `WDA_EXCLUDEFROMCAPTURE` |
| **控件可拖且位置會記住** | **通過**，拖到 387,242 → INI `bar_pos=-1213,-658` → 下次錄影回到同一個點 |
| **控件不會跑到螢幕外** | **通過**，`bar_pos=5000,-5000` / `-99999,99999` / 未設定，三種都夾回 `rcWork` |
| **麥克風靜音（錄製中）** | **通過**，靜音後麥克風軌 `peak=-999.0 dBFS`，長度 2.731s 對影像 2.759s |
| **錄影中反覆開關麥克風** | **通過**，同一支 5.2s 檔案關→開→關，逐 100ms RMS `-42 / -999 / -42 / -999 dB`，音軌 5.184s 對影像 5.217s |
| **GUI 多音源混成單軌** | **通過**，檔案只有 1 條音軌；麥克風關掉只放 440Hz 時同一條軌出現 `-11.7 dB`（證明 loopback 有混進去） |
| **停止鈕可用** | **通過**，點紅色方塊 → 控件立刻消失、檔案產生（600×400 與選取一致） |
| **GUI 設定持久化** | **通過**，`%LOCALAPPDATA%\CapWGC\settings.ini`，壞值會被夾回合法範圍 |
| **GUI 熱鍵可自訂** | **通過**，五組（區域 / 縮圖牆 / 主螢幕 / 停止 / 截圖-OCR）可在設定視窗擷取，`--hotkeytest` 28/28 PASS |
| **熱鍵擷取即時回饋** | **通過**，逐鍵顯示 `Ctrl+…` → `Ctrl+Shift+…` → `Ctrl+Shift+F7`；Esc 清除、無修飾鍵被擋、Win 被擋 |
| **熱鍵衝突偵測** | **通過**，重複綁定按確定被擋並提示；修好後關窗並寫 INI |
| **熱鍵註冊失敗可回滾** | **通過**，`ApplyHotkeys` 註冊失敗時把舊的整套註冊回去，不會出現「按什麼都沒反應」 |
| **縮圖牆（視窗/螢幕分頁）** | **通過**，視窗分頁 25 個來源（4 `printwindow` + 1 前景 `screen` + 20 最小化）、螢幕分頁顯示「螢幕 1 1600×900 主螢幕」 |
| **縮圖牆清單完整性** | **通過**，修前只有 7 個（最小化的與 UWP 標題為空的被靜默丟掉）。現在 25 個全列，`picker.log` 的 `windows=` 與 `gui_picker_diag.ps1` 的 ACCEPT 一致 |
| **縮圖牆鍵鼠操作** | **通過**，方向鍵移動、Tab 切分頁、Enter/雙擊確認、Esc 關閉；`End` 選到最小化項 → `Enter` 會先 `SW_RESTORE` 再回傳矩形（`restored minimized window ... iconic=0`） |
| **縮圖牆即時更新** | **通過**，`out/gui_picker_live.ps1` **12/12 PASS**：開新視窗 2.6s 內 `refresh: + added hwnd=…`（hwnd 精確比對）、關掉 → `refresh: - removed`、既有項不被重排、即時刷新期間 `how=screen` **0 次**（不自拍）；`--picker-static` 下 `live=0` 且零 refresh 行 |

```
WGC → 軟體幀率閘門 → GPU ring[4](crop) → VideoProcessorBlt(BGRA→NV12, BT.709)
    → IMFTransform::ProcessInput → pump 排空 → 自寫 MP4 muxer（fMP4 預設）

loopback(render 端點) → 降混 2ch/16-bit → MF AAC-LC #0 ┐
mic     (capture 端點) → 降混 2ch/16-bit → MF AAC-LC #1 ├→ 同一個 muxer 的 trak 2 / 3 / 4
loopback(其他裝置)     → 降混 2ch/16-bit → MF AAC-LC #2 ┘   （每條 trak 各自一個 traf）

時間軸：影像 t=0 = 第一筆「寫入」的幀（第一幀只拿來開編碼器），
        音訊延到那一刻才 Start()，所以兩邊的內容起點其實一樣。
        要寫延遲就寫進 edts/elst（progressive）或第一個 tfdt（fMP4），
        預設不寫：QPC 記帳算出來的 60ms 是管線延遲差，不是音畫差
        （實測真實偏移 −1.3~+10.9ms，見 docs/08 §6.6）。
```

AV1 只差在最後一段：AU 是「帶 obu_size 的 OBU 串」，原樣進 mdat，參數集是
sequence header OBU，解析後放進 `av1C`。

---

## 檔案結構

```
CapWGC\                        ← 專案根（4070 Ti 在 E:\workspace，3060 Ti 在 D:\workspace）
  build\build.bat              probe|cli|gui|all，自動找 vcvars64（含手動 fallback）
  src\core\recorder.h/.cpp     可重用的錄製器（GUI 用；時序語意與 main.cpp 一致）
  src\app\main.cpp             WinMain、托盤、全域熱鍵、狀態機、自我測試
  src\app\ui.h/.cpp            托盤圖示 / 半透明遮罩 / 中空外框 / 文字徽章
  src\app\overlay.h/.cpp       區域框選（拖曳選矩形、單擊選視窗）
  src\app\controlbar.h/.cpp    錄製中的漂浮控件：停止 + 麥克風靜音兩顆圖示，
                               可拖曳、位置存進 INI、全螢幕時靠
                               WDA_EXCLUDEFROMCAPTURE 不入鏡（取代舊的
                               外框 + 計時徽章；計時改放托盤提示）
  src\app\picker.h/.cpp        縮圖牆來源選擇器（Discord 式；視窗/螢幕兩分頁；即時更新）
  src\app\settings.h/.cpp      INI 設定 + 設定視窗（含熱鍵擷取控制項）
  src\core\capture_wgc.h/.cpp  WGC 擷取層（pimpl，header 不含 winrt）
  src\core\encoder.h           抽象介面 + EncoderConfig
  src\core\mft_encoder.h/.cpp  M2：自驅動 IMFTransform（預設引擎）
  src\core\encoder_mft.h/.cpp  M1：MF SinkWriter 包裝（對照組）
  src\core\mp4_muxer.h/.cpp    自寫 MP4 muxer，progressive 與 fMP4 兩種佈局
                               （H.264/HEVC 走 Annex-B→AVCC；AV1 走 OBU + av1C；
                                音軌 0..N 條，每條一個 trak / 一個 traf；
                                起始延遲 → progressive 的 edts/elst、fMP4 的 tfdt）
  src\core\audio_capture.h/.cpp WASAPI 擷取：loopback（含靜音 keep-alive）與麥克風，
                               端點列舉、裝置解析（索引 / 名稱 / endpoint ID）
  src\core\aac_encoder.h/.cpp  MF AAC encoder MFT 包裝（raw AU + ASC 從 MF_MT_USER_DATA）
  src\core\codec_select.h/.cpp 執行期探測編碼器 + 自動選擇 + 降級鏈
  src\cli\main.cpp             CapCli（--audio / --mic / --audio-src / --audio-device /
                               --mic-device / --list-audio-devices / --audio-bitrate）
  src\probe\probe_encoders.cpp 編碼器/解碼器能力探測工具
  src\probe\anim_window.cpp    確定性動畫測試源（--hard 高熵、--borderless、
                               --sync 音畫同步素材源：整屏反轉 + 3kHz 純音）
  src\probe\mp4_check.cpp      解回檔案驗證容器 + PSNR（--audio 逐條量每聲道峰值/RMS、
                               --tracks 讀 moov 並印 edts/elst 與 tkhd−mdhd 的延遲、
                               --sync 量音畫事件對齊並與容器宣告的延遲對比、
                               --streams 讀 MF 的 stream 索引）
  out\                         全部產物（exe、測試 mp4）
  out\iso_psnr.py              同畫質換算（PSNR 軸對檔案大小分段線性插值）
  out\mp4_frags.py             列 fMP4 每個 moof 的 traf、樣本數與 tfdt（抗 kill + 延遲驗證）
  out\sync_ab.sh               A/V 同步 A/B 對照一鍵腳本（含清場、素材源、錄製、量測）
  out\calibrate.sh             量這台機器的 A/V 偏移 → 寫校準檔 → 立刻驗證
                               （兩輪解混疊：FLIP=2000 定向 + FLIP=400 精修）
  out\gui_smoke.ps1            GUI 全流程煙霧測試（熱鍵→框選→錄→停，逐步截圖）
  out\gui_hud.ps1              只顯示錄製指示（外框 + 徽章 + 一塊遮罩）
  out\gui_bar.ps1              漂浮控件三張截圖（正常 / 靜音 / 恢復）
  out\gui_bar_exclude2.ps1     全螢幕錄影：控件不入鏡（-AllowCapture 是對照組）
  out\gui_bar_drag.ps1         拖曳 → 位置寫進 INI → 下次沿用
  out\gui_bar_offscreen.ps1    荒謬座標也會被夾回工作區
  out\gui_bar_region.ps1       區域錄影時控件在範圍外 + 點停止鈕
  out\gui_bar_mute.ps1         靜音後麥克風軌必須是 -999 dBFS
  out\gui_bar_mic_cycle.ps1    錄影中關→開→關，逐 100ms 量 RMS（配 out\mic_window_rms.py）
  out\gui_bar_mix_sys.ps1      麥克風關掉只放 440Hz，驗系統聲有混進同一條軌
  out\gui_bar_aa.ps1           控件邊緣特寫（8× 放大看抗鋸齒）
  out\gui_settings.ps1         只開設定視窗
  out\gui_settings_fit.ps1     量設定視窗有沒有出界，印 RESULT PASS/FAIL
  out\gui_picker.ps1           縮圖牆：三張截圖（視窗/螢幕/選取）+ Esc 關閉
  out\gui_picker_live.ps1      即時更新：開/關視窗清單跟上、不重排、不自拍（12 項斷言）
  out\gui_picker_diag.ps1      逐個頂層視窗列出「為什麼被丟掉」（對帳用）
  out\gui_picker_min.ps1       最小化視窗：列舉 + End/Enter 還原
  out\gui_hotkey_capture.ps1   熱鍵擷取全流程（往返/清除/拒絕/衝突/套用）
  out\gui_hotkey_probe.ps1    逐鍵觀察熱鍵擷取（驗前景時序）
  out\gui_hotkey_win.ps1       只驗 Win 組合被擋（必須獨立行程，見 docs/09 §5）
  out\ui_sweep\                UI 組 dense sweep：8 個 quality × (AV1+HEVC+ref)
                               + compute.py（自己量完並算同畫質比，超出範圍標 OUT）
  out\hard_sweep\              高熵組 dense sweep：q40~q75 八點 × (AV1+HEVC+ref)
                               + compute.py（同上，另外印雙向錨點與斜率）
  out\h264_recheck\            H.264 旋鈕異常的證據與同批錨點
  docs\01-capture-core-evaluation.md   三份 WGC 實作的效能對比與架構提案
  docs\02-encoder-selection.md         MF vs NVENC、RTX 40 的 AV1
  docs\03-m1-status.md                 M1 實測與限制
  docs\04-codec-autoselect.md          自動選碼的實作與驗證
  docs\05-m2-status.md                 M2 實測、RD 數據、fMP4 與四個容器 bug
  docs\06-av1-status.md                AV1 實作（av1C/OBU）與 4070 Ti 實測
  docs\07-audio.md                     音軌：loopback + AAC + 第二條 trak/traf，與三個坑
  docs\08-multi-audio.md               多音源：麥克風與系統聲各一條軌、裝置選擇，與兩個坑
  docs\09-gui.md                       GUI：Recorder 抽層、托盤/熱鍵/框選/指示、
                                       熱鍵自訂、縮圖牆，與踩過的坑
```

---

## 建置與測試

```bat
cd <專案根>\build
build.bat cli        :: 只編 CapCli
build.bat probe      :: probe_encoders + anim_window + mp4_check
build.bat gui        :: 只編 CapWGC（GUI）
build.bat            :: 全部
```

**建置前先確認沒有 CapCli.exe / CapWGC.exe / anim_window.exe 在跑。** 否則會看到
`LNK1104: 無法開啟檔案 ...`（早期版本因為訊息被吞掉，看起來像 `Access is denied.`，
很容易誤判成編譯失敗或權限問題）。

執行：

```bat
out\CapCli.exe --out rec.mp4 --seconds 10
out\CapCli.exe --list-windows
out\CapCli.exe --out a.mp4 --hwnd 0x1234 --seconds 5 --codec h264 --quality 70
out\CapCli.exe --hwnd 0x1234 --progressive --out b.mp4   :: 退回 moov 在檔尾

:: 錄系統聲音（WASAPI loopback → AAC-LC，預設 128 kbps）
out\CapCli.exe --hwnd 0x1234 --out c.mp4 --seconds 6 --audio
out\CapCli.exe --hwnd 0x1234 --out c.mp4 --seconds 6 --audio --audio-bitrate 192000

:: 系統聲 + 麥克風（兩條獨立音軌），或任意多條
out\CapCli.exe --hwnd 0x1234 --out c.mp4 --seconds 6 --audio --mic
out\CapCli.exe --hwnd 0x1234 --out c.mp4 --seconds 6 --audio --mic --audio-src loopback:0
out\CapCli.exe --out c.mp4 --seconds 6 --mic --mic-device 麥克風
out\CapCli.exe --list-audio-devices

:: 驗音軌（量每聲道峰值/RMS，確認不是全靜音；--tracks 看 trak 名稱與時長）
out\mp4_check.exe --audio a.mp4
out\mp4_check.exe --tracks a.mp4
out\mp4_check.exe --streams a.mp4
python out\mp4_frags.py out\m_kill.mp4

:: 驗容器有沒有把延遲寫進去（預設 fMP4 走 tfdt；--av-offset 走 elst / tfdt）
out\CapCli.exe --out g1.mp4 --seconds 6 --audio --progressive --av-offset 60
out\CapCli.exe --out f1.mp4 --seconds 6 --audio --mic --av-offset 60
out\mp4_check.exe --tracks g1.mp4      :: 音軌有 elst 0.060s，影像那條沒有
python out\mp4_frags.py f1.mp4         :: 音軌第一片 tfdt = 2646 @44100

:: 量真正的音畫差（需要動到聲音與畫面，一鍵跑一組）
:: 判讀：progressive 呈現差 = D + E；fMP4 = D；|呈現差| <= 10ms 算對齊
:: 注意：素材源週期 400ms 時 D 只能決定到 mod 400ms，大偏移要拉長週期
out\sync_ab.sh P0 --progressive        :: 純原始時間軸（--no-av-offset，A/B 對照用）
out\sync_ab.sh P1 --progressive --av-offset 60
out\sync_ab.sh C2 --audio-delay 500 --av-offset 500   :: 證明能抵掉真實延遲

:: 音畫同步回歸（會自己判 PASS/FAIL，並檢查 CLI 與 GUI 一致）
bash out\verify_sync.sh
FLIP=2000 out\sync_ab.sh D2 --audio-delay 500         :: 量超過 ±P/2 的偏移要拉長週期

:: 負偏移（音訊偏晚）= 裁掉開頭。progressive 走 elst media_time（精確），
:: fMP4 真的丟 AU（量化到 23.2ms）。FLIP=2000 是為了排除混疊歧義。
FLIP=2000 out\sync_ab.sh N1 --av-offset -150
FLIP=2000 out\sync_ab.sh N2 --progressive --av-offset -150

:: 校準這台機器（約 30 秒；換 GPU／音效驅動／Windows 版本要重跑）
out\calibrate.sh                 :: 量兩輪 → 解混疊 → 寫 %LOCALAPPDATA%\CapWGC\av-offset.txt → 驗證
out\calibrate.sh --dry-run       :: 只量不寫
out\CapCli.exe --out a.mp4 --seconds 6 --audio --av-offset auto   :: 讀校準檔

:: 確定性內容源（開另一個視窗，會印出 hwnd）
out\anim_window.exe --size 1280x720 --seconds 900 --hard --fps 60
out\anim_window.exe --size 1920x1080 --seconds 900 --hard --fps 60 --borderless
:: 音畫同步素材源（每次反轉同時播 3kHz 純音，配 mp4_check --sync）
out\anim_window.exe --sync --seconds 40 --interval 400
out\mp4_check.exe --sync out\sync_P0.mp4 --freq 3000 --period 400 -v
```

**GUI（`out\CapWGC.exe`，雙擊即用，沒有主視窗，只有托盤）**

| 動作 | 預設熱鍵 |
|---|---|
| 截圖 / OCR | `PrtScn` |
| 視窗清單（縮圖牆，像 Discord 的來源選擇） | `Ctrl+Alt+W` |
| 選區域（拖曳）或選視窗（單擊） | 未綁定，走托盤選單 |
| 錄主螢幕 | 未綁定，走托盤選單 |
| 停止 | 未綁定，點漂浮控件的紅方塊 |

**五組熱鍵都可以在設定視窗自訂**（點欄位後直接按組合鍵；Esc / Backspace
清除＝不綁）。設定存在 `%LOCALAPPDATA%\CapWGC\settings.ini` 的 `[hotkeys]`
段落，存的是 `Ctrl+Alt+A` 這種人可讀字串。熱鍵被別的程式佔用時，啟動會用
托盤氣泡警告、設定按確定時會被擋下並提示是哪一組；改熱鍵是「全有或全無」
（新的註冊失敗會把舊的整套註冊回去）。`Win` 組合會被 shell 先接走，
擷取 UI 直接擋掉並說明。

托盤右鍵的順序是：**選擇視窗…** / 錄製區域 / 錄製主螢幕 / 停止 ── 截圖 / OCR
/ 截圖後直接 OCR ── 設定… / 重設控件位置 / 開啟輸出資料夾 ── 結束。
「選擇視窗」排第一位，因為它最常用、而且不會立刻開始做任何事。
選單上的熱鍵文字是動態的，跟著自訂值變。日誌在 `%LOCALAPPDATA%\CapWGC\gui.log`。

**錄製中的漂浮控件**（`src/app/controlbar.*`）：畫面上只有一顆 66×36 的膠囊。

| 動作 | 操作 |
|---|---|
| 停止 | 點左邊紅色方塊（與「停止」熱鍵等價，該鍵預設未綁定） |
| 麥克風靜音 / 恢復 | 點右邊麥克風（變灰加斜線；只影響麥克風軌） |
| 移動 | 左鍵按住膠囊拖；位置存 `[ui] bar_pos=dx,dy`（相對擷取範圍右下角） |
| 歸位 | 托盤右鍵 → 重設控件位置（錄影中也有效） |

麥克風鈕是**錄影中的即時開關**：關掉時把麥克風那一路換成零樣本，時間軸不動，
同一支檔案可以關→開→關反覆切（實測 5.2s 檔案逐 100ms RMS：-42 / -999 / -42 /
-999 dB）。前提是**這次錄影有麥克風**——設定裡的「麥克風」是總開關，在 `Start()`
那一刻決定；沒勾就沒有這一路，錄影中無法新增。

檔案裡只有**一條音軌**：系統聲與麥克風相加後再編碼（`mixAudio`，預設開）。
理由見 docs/09 §4.7.1 —— MP4 有多條音軌時大部分播放器只播第一條，而第一條
通常是安靜的 loopback，使用者會以為「麥克風沒錄到」。

它不會入鏡（`WDA_EXCLUDEFROMCAPTURE`，實測全螢幕 181 幀中紅色像素 0），
也**不可能跑到螢幕外**（拖曳中與顯示時都夾回 `rcWork`，加上選單逃生門）。
沒有麥克風軌時那顆鈕是灰的，點下去會說明原因。

外觀走 per-pixel alpha（32bpp DIB → D2D **DC** render target →
`UpdateLayeredWindow`），不是 `SetWindowRgn` 的圓角 region —— region 是 1-bit，
圓角會有一格一格的階梯（使用者：「像素感太強」）。連帶兩件事：不能再用
`SetLayeredWindowAttributes`（與 ULW 互斥），而且透明區會吃滑鼠，所以
`WM_NCHITTEST` 要讀 alpha 回 `HTTRANSPARENT`。詳見 docs/09 §4.3。

```bat
:: 不用互動就能驗證（GUI 的失敗是無聲的，這幾個入口很重要）
out\CapWGC.exe --selftest 3 --audio     :: 只跑 Recorder，統計寫到 out\selftest.log
out\CapWGC.exe --hudtest 8 --allow-capture   :: 只看外框 + 徽章 + 一塊遮罩
out\CapWGC.exe --settings               :: 直接開設定視窗
out\CapWGC.exe --hotkeytest             :: 熱鍵文字往返/拒絕/衝突，28 案例，寫 out\hotkeytest.log
out\CapWGC.exe --picker                 :: 直接開縮圖牆
out\CapWGC.exe --picker-static          :: 開縮圖牆但關掉即時刷新（拍可重現畫面用）
out\CapWGC.exe --allow-capture          :: 關掉 SetWindowDisplayAffinity，截圖才看得到自己的 UI
```

**驗證改動的標準流程**（編碼器相關改動必做）：

1. 開 `anim_window.exe --hard`，取得 hwnd
2. 三路同時輸出（同一批幀才可比）：
   ```bat
   CapCli.exe --hwnd <hwnd> --codec hevc --quality 60 --ab h264 --q2 60 ^
              --ref ref.mp4 --out hevc.mp4 --out2 h264.mp4 --seconds 6
   ```
3. `mp4_check.exe ref.mp4 hevc.mp4` 與 `mp4_check.exe ref.mp4 h264.mp4`
4. 看「同等 PSNR 下的檔案大小比」，**不要**直接比同一個 `--quality` 的大小

---

## 已驗證的環境事實

**這台（2026-10-17 之後的工作機）**

- GPU：**RTX 4070 Ti**（Ada，第 8 代 NVENC，**有 AV1 硬編**），驅動 616.92。
  `probe_encoders.exe` 掃到 NVIDIA H.264 / HEVC / **AV1** 三個硬體 MFT，
  都是 d3d11-aware。
- **session 是 RDP**（不是 console），桌面只有 **1280×720**。後果有兩個：
  擷取上限約 31 fps；1080p 以上的測試在這台做不了。
- **沒有 HEVC 解碼器**（商店的 HEVC 擴展要付費，`winget` 回 `0x803f8001`）。
  所以 `mp4_check` 量不到 HEVC 的 PSNR。AV1 解碼擴展免費，已安裝，可以量。
- 這台原本**沒有任何 C++ 工具鏈**（VS2019 BuildTools 只剩 MSBuild）。
  已補裝 VS BuildTools 2022 + VCTools 工作負載（MSVC 14.44 + SDK 10.0.26100）。

**前一台（3060 Ti，docs/01~05 的數據來源；docs/06 的 HEVC PSNR 與 docs/07 的音軌實測也在這台）**

- console session。靜止的記事本 8 秒只收到 35 幀（4.4 fps）→「靜止近乎 0 碼率」
  這個賣點在 console 上成立。RDP 下不成立（遠端桌面本身在重繪）。
- 顯示器 1920×1080@239Hz。WGC 沒設 `MinUpdateInterval` 時會給 234 fps
  （4 秒 13 MB），一定要節流。
- GPU 是 **RTX 3060 Ti**（Ampere，第 7 代 NVENC，無 AV1 硬編）——
  `docs/02` 一度把交接文件寫的 4070 Ti 當成筆誤，其實兩台都在，別再搞混。
- **這台有 HEVC 解碼器**（4070 Ti 那台沒有），所以所有 HEVC 的 PSNR 都要在這台量。
  `docs/06` §3.4 的 AV1-vs-HEVC 同畫質數字就是 2026-10-17 在這台補的。
  hostname `tokyo`，工作目錄 `D:\workspace\CapWGC`（兩台路徑不同，別直接複製指令）。
- 音訊端點的 mix format 是 **44100 Hz 2 ch**（AAC 的 ASC 2 bytes）。
- `BPictureCount` 被 MF 包裝層回 `REJECTED`（硬體其實支援），AV1 MFT 也一樣。
  `LowLatency` 同樣被拒。
- `QualityVsSpeed` 預設 33/100。推到 100 對壓縮率有明顯增益，已在程式中設定。

---

## 已經踩過的坑（別重踩）

### 容器 / muxer

| 坑 | 症狀 | 解法 |
|---|---|---|
| `stsz` 空表少寫 `sample_size` | MF 回 `0xC00D36C4`，像「格式不支援」 | stsz 的版面是 `sample_size(4) + sample_count(4)`，不是只有 entry_count |
| 把 `BuildStsd()` 的回傳再包一層 | 同上，且原生 subtype 變成 `73747364`（'stsd' 的 ASCII） | `BuildStsd()` 回傳的已是完整 box |
| `trun` 的 `data_offset` 回填位置算錯 | 檔案打得開，但 `SetCurrentMediaType(NV12)` 回 `0xC00D5212` | 偏移是 `moof(8) + mfhd + traf(8) + tfhd + tfdt + trun(8) + ver/flags(4) + sample_count(4)` |
| 分片累加用「時間軸結束位置」 | 5 秒錄影播出來 7 秒，越長越誇張 | 用 `dts.back() + delta.back() - dts.front()` 當分片長度 |
| HEVC 的 NAL header 是 2 bytes | hvcC 的 profile/level 是垃圾值 | profile_tier_level 從 `sps->p + 3` 取（H.264 是 `p[1]`） |
| AV1 sequence header 少讀/多讀一個 bit | av1C 解出 `level_idx=18` + `high tier`（720p 不存在的組合），**但檔案照樣能播** | `decoder_model_info_present_flag` 只在 `timing_info_present_flag=1` 時才存在；解析完拿 max_frame_width/height 跟設定值對一次 |
| **`esds` 少包最外層 `ES_Descriptor(0x03)`** | box 結構看起來完全合法、影像正常，但 MF 只認得影像軌：`mp4_check --audio` 回 `no audio stream (0xC00D36B3)` | descriptor 是三層嵌套 `0x03 → 0x04 → 0x05`；`0x04/0x05/0x06` 看起來像完整樹，最外殼很容易漏。`Desc(esAll, 0x03, esP)` 才是完整 |
| 音軌沿用影像的 timescale（90000） | AAC 的 1024 取樣點換算不整數，`stts` 得逐筆描述或容忍誤差 | 音軌 timescale = 取樣率，每個 AU 剛好 1024 tick，`stts` 只有一個 run |
| 音軌時間軸用 `pts100ns` 累加 | 每個 AU 都有捨入誤差，長錄影會漂 | 用累計取樣點累加；`pts100ns` 只決定起點 |

### 擷取

| 坑 | 症狀 | 解法 |
|---|---|---|
| 幀率閘門寫在 `TryGetNextFrame()` 之前 | 錄 3~8 秒後擷取永久停住，`captured` 凍結、`dropped=0`、無錯誤、行程還活著 | 先取幀再判斷。不取走的幀會塞爆 pool，WGC 就不再回呼 |
| 閘門用嚴格下界 | 精準 60 fps 的來源被錄成 48 fps | 留 10% 容差 |
| `MinUpdateInterval` 用 `ceil(1000/fps)` | 17 ms > 16.67 ms，60 fps 被擋掉一半 | `floor(1000/fps - 1)` |
| `MinUpdateInterval` 設 1 ms | 239 Hz 桌面以 239 fps 送幀 | 見上，配合軟體閘門 |
| WGC 對最小化視窗 | 回 154×25 的 item | `IsIconic` → `SW_RESTORE` 再 `CreateForWindow` |
| free-threaded pool 重入 | 兩個 callback 共用 scratch buffer | `busy.exchange(true)` 直接丟幀 |
| 視窗被遮住 | 只給 10 fps | `WS_EX_TOPMOST` + `SetForegroundWindow` |
| `anim_window` 用了 `WS_EX_TOOLWINDOW` | `CreateForWindow` 回 `0x80070057`，看起來像 hwnd 抓錯或 WGC 壞了 | Windows 11 24H2 的 WGC 不接受 tool window，拿掉這個 ex-style |

### Media Foundation

| 坑 | 症狀 | 解法 |
|---|---|---|
| 忘記 `MFStartup` | `MFCreateSinkWriterFromURL` 回 `0xC00D3E85`（像格式不支援） | main 開頭 `MFStartup(MF_VERSION, MFSTARTUP_FULL)` |
| 碼控在 `SetOutputType` **之後**才設 | `SetValue` 回 `S_OK` 但值被型別協商重置 | 碼控一定要在 `SetOutputType` 之前 |
| CODECAPI 用 `VT_I4` | 值被靜默丟棄 | 用 `VT_UI4` |
| QP 設 0 | 被當成「未設定」，回退預設（quality=100 的檔案比 85 還小） | 上界停在 1 |
| `MF_SA_D3D11_AWARE` 讀錯來源 | 永遠拿到 0，硬體 MFT 被當軟體 | 從 `IMFTransform::GetAttributes()` 讀，不是 `IMFActivate` |
| WRL ComPtr 用 `.put()` | 編譯錯 C2039 | WRL 是 `.GetAddressOf()`，`.put()` 是 winrt::com_ptr |
| `std::max` / `std::clamp` 被巨集干擾 | 編譯錯 C2589 | 每個 header 開頭 `#define NOMINMAX` |
| `GetProcessTimes` 量化 | 3 秒短跑量出 2.28 ms/幀的假象 | 粒度 15.625 ms，至少跑 6 秒 |
| MF 物件沒 `Reset()` 就 `MFShutdown()` | 輸出全對但結束時 segfault | 先釋放再 shutdown |

### 音訊

| 坑 | 症狀 | 解法 |
|---|---|---|
| **WASAPI loopback 在系統安靜時一個 packet 都不送** | 錄 12.98s，AU 數看起來正常，但整條軌全是靜音（`peak=-999.0`）、`loop.frames()=0`；影像完全正常 | 音訊引擎 idle。另外開一條 `IAudioRenderClient` 播放串流持續餵零當 keep-alive，引擎就不會停 |
| 沒檢查 `AUDCLNT_BUFFERFLAGS_SILENT` | 安靜段變成雜音 | loopback 在靜音時仍送資料，且緩衝區內容未定義（不是零），flag 一定要看 |
| 音軌比影像短 58 ms | 6 秒錄影 audio 5.875s vs video 5.988s | AAC 是 1024 取樣點一幀，尾端不足一幀留在編碼器裡。`Flush(targetFrames)` 先補靜音到影像長度再收尾 |
| float32 → int16 用 32768 | +1.0 溢出成爆音 | 用 32767 |
| `AAC` 輸出帶 ADTS 標頭 | 播放器當成音訊資料解，結果是雜音 | `MF_MT_AAC_PAYLOAD_TYPE = 0`（raw） |
| 自己拼 AudioSpecificConfig | 跟編碼器實際參數不一致（尤其 SBR / 非 48k） | 從輸出型別的 `MF_MT_USER_DATA` 取（前 12 bytes 之後那段） |
| 音訊執行緒碰到 `bytes()` 的 `_ftelli64` | 在別人 `fwrite` 中間取值，數字錯亂 | `mu` 改 `mutable`，`bytes()` 也加同一把鎖 |
| **MF 的 stream 索引不照 moov 的 trak 順序** | 多軌檔的驗證數字張冠李戴（把麥克風的 RMS 當成系統聲），但檔案本身完全正常 | 音軌在前、影像在後，多條音軌還是反序。不要用序號對應：`mp4_check --tracks`（讀 box）與 `--streams`（讀 MF）交叉看 |
| **`main` 的 argv 是 ANSI（CP_ACP）** | `--list-audio-devices` 印得出「麥克風」，`--mic-device 麥克風` 卻說找不到裝置 | `GetCommandLineW` + `CommandLineToArgvW` 自己取命令列，統一轉 UTF-8（順帶修好中文輸出路徑） |
| 多條軌共用一個取樣點累加器 | 44.1k 與 48k 混用時時間軸漂掉 | 每條軌自己的 `ts` / `nextDts`；`mdhd.timescale` 也各自取自己的取樣率 |
| 多條音軌的 `alternate_group` 設成同一個非零值 | 播放器只播其中一條（聽不到系統聲或麥克風） | 全部留 0 |
| 多條音軌的 `tkhd.duration` 都填 `movieDur` | 短的那條被拉長，尾巴一段靜音 | 每條軌填自己的時長 |
| `Flush(targetFrames)` 共用一個換算 | 48k 那條少掉 8% 的尾巴 | 用該軌自己的取樣率換算 |
| **以為「QPC 相減」就是音畫差** | 推導出 56~72ms，寫進容器後音訊反而晚 60ms。於是 2026-10 之前結論是「檔案本來對齊、不要補」 | **那個結論是被量測工具騙的**：素材源的 `PlaySound` 從呼叫到出聲有 85~90ms，而 CLI 的 gap 剛好 86~89ms，兩個誤差互相抵掉 → 量成「對齊」。實際上整條音軌早了 gap。修法：預設自動補 `firstPacketQpc − videoOrigin`（docs/08 §6.6.1） |
| 用「第一幀」當影像時間軸的零點去算偏移 | 音軌會多補一幀（16.7ms），變成過度補償 | 零點是「第一筆**寫入**」的幀。第一幀只拿來讀尺寸、開編碼器，不進檔案 |
| 音訊在第一幀回呼裡就 `Start()` | 量延遲時沒有基準可減（零點還沒定），而且容器也來不及收到延遲 | 音訊延到第一筆寫入的幀才起跑。代價是開頭多丟一幀 |
| 用「第一次 `Drain()` 拿到多少 frame」當 loopback pre-roll | 量到 10.2ms，真實值 60ms 上下 —— 這個錯值就是那 60ms 偽影的來源 | 改用 `IAudioCaptureClient::GetBuffer` 的 `pu64QPCPosition`（`firstPacketQpc100ns()`），並把它跟影像零點相減寫進容器 |
| **素材源用 `PlaySound(SND_ASYNC)` 當同步事件** | 呼叫只花 1~3ms，但出聲要 85~90ms。整條 A/V 驗證鏈的讀數全部偏 +85ms，還剛好把 recorder 的 gap bug 抵掉 | `anim_window --sync` 改成「持續播的 waveOut 串流 + `waveOutGetPosition` 驅動畫面反轉」：輸出延遲同時作用在兩邊，自動抵消。殘差 +32ms 用 `--flip-lead` 補（docs/08 §6.5.1） |
| 只看一條路（CLI）的同步數字 | CLI 的 gap 剛好抵掉素材源誤差，看起來永遠「對齊」；GUI 的 gap 小，露出來變成 70ms | 回歸測試必須同時量 CLI 與 GUI，並斷言**兩者一致**（`out/verify_sync.sh`） |
| `--ab` 時 `au` 在 target 迴圈裡累加 | `au=428` 對上 `pcm=5.02s`（實際 214 個 AU） | AU 數在來源端計一次；`errors` 才逐 target 累加 |

### GUI

| 坑 | 症狀 | 解法 |
|---|---|---|
| **`WDA_EXCLUDEFROMCAPTURE` 讓截圖看不到自己的 UI** | 視窗 `IsWindowVisible` 真、矩形正確、D2D 零錯誤，截圖卻一片空白 → 誤判成「元件沒畫出來」 | 遮罩與外框改成不設（本來就在擷取範圍外）；徽章保留，另開 `--allow-capture` 關掉 |
| 均勻變暗在截圖上看不出來 | 整片都暗、沒有對照組，看起來跟正常一樣 | 量像素（132 vs 233），別憑眼睛 |
| 沒有訊息迴圈就沒有 `WM_PAINT` | 測試模式裡視窗存在但空白 | 測試迴圈自己 `PeekMessage` |
| `Begin()` 裡先 `SetCapture` | 框選一打開就自己結束，還選到沒碰過的視窗 | 只在 `WM_LBUTTONDOWN` 時 capture |
| `Stop()` 之後 `stats().audioAU` 變 0 | 檔案音軌正常，統計說 0 AU | `Stop()` 會清 `audio`；AU 數用獨立原子計數 |
| `frames == 0` 被當成成功 | 完全靜止的視窗只送「開編碼器那一幀」，成品是空檔卻報「已儲存」 | 也算失敗，訊息寫「沒有寫入任何畫面」 |
| 音源開不起來被靜默跳過 | 勾了麥克風卻沒聲音 | `RecorderStats::audioNote` 帶原因，氣泡改警告色 |
| 設定視窗的資料夾欄位是空的 | 開了才發現要重填 | EDIT 的文字在 `CreateWindowExW` 時就給 |
| **熱鍵欄位「抓不到鍵」** | 點進去按什麼都沒反應，像控制項壞了 | `WM_GETDLGCODE` 回 `DLGC_WANTALLKEYS`，否則 Enter 被對話框吃掉、Esc 關窗、Tab 跳走；回了之後 Tab 要自己轉 |
| **改熱鍵後整套失效** | 新組合註冊失敗、舊的已 `Unregister` → 按什麼都沒反應，UI 卻顯示新值 | `ApplyHotkeys` 失敗要把舊的整套註冊回去；只回滾熱鍵，其餘設定照收 |
| **`Win` 組合鍵是無聲死路** | 欄位停在 `Shift+Win+…`，後續那個鍵永不來 | shell 先接走 Win+<key>。在「只按修飾鍵」那個分支就擋掉並說明 |
| **`RegisterAll` 不先清舊值** | 改熱鍵時「舊值」被算成佔用者，改完反而失敗 | 每次註冊前先 `UnregisterAll` |
| **縮圖牆一排黑格子** | UWP 程式縮圖全黑 | `EnumWindows` 把 cloaked 的 UWP 空殼算成可見，要過濾 `DWMWA_CLOAKED` |
| **`PrintWindow` 成功卻給黑圖** | 回傳成功、縮圖一片黑，被當有效縮圖 | `MostlyBlack` 取樣檢查 → placeholder；前景視窗 fallback `BitBlt(SRCCOPY\|CAPTUREBLT)` |
| **縮圖一張張跳出來** | 且前景那張抓到「選擇器自己」 | 先抓完所有縮圖再顯示自己 |
| **每個音源各寫一條軌** | 使用者「勾了麥克風，播放卻沒聲音」：檔案裡 mic 是 `peak=-0.0 dBFS`，但播放器只播第一條（安靜的 loopback） | 多來源混成**一條**軌（`mixAudio`）。判準不是「檔案有沒有聲音」，是「播放器會播哪一條」 |
| **`wantMix && openAac(master)` 的短路** | 單一音源時 `wantMix` 是 false，`openAac(master)` 整段被跳過 → `aac` 空指標 → `--selftest --audio` 一啟動就 segfault | 迴圈裡的條件改成 `if (!s->aac && !openAac(s))`，不要假設 master 一定開好了 |
| **列舉時丟掉最小化的視窗** | 使用者：「可選窗口變少了，不齊全」。VS Code / Discord / Terminal 開著卻只有 7 個 | `IsIconic` 不是丟棄理由。矩形用 `GetWindowPlacement().rcNormalPosition`（`GetWindowRect` 對最小化只回 160×28），`it.minimized` 標記，確認時先 `SW_RESTORE` |
| **`GetWindowTextW` 空字串就丟** | UWP 的「設定」「小算盤」整個消失（標題在子視窗上） | `EnumChildWindows` 撈第一個非空標題，都沒有才退回 exe 名 |
| **清單齊了但排壞了** | 首頁 8~9 格全是「無法預覽」，看起來還是「東西變少」 | `stable_sort` 讓沒最小化的排前面；拍不到畫面時畫程式圖示（`WM_GETICON` → 子視窗 → 類別 → exe），不要留灰塊 |
| **`WM_GETICON` 用 `SendMessage`** | 一個卡住的視窗就讓整個選擇器卡住 | `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 120ms)`；圖示 `CopyIcon` 成自己的，釋放時 `DestroyIcon` |
| **上限截斷無聲** | 超過上限的視窗默默不見，症狀與「不齊全」一樣 | 截斷時記一行 `[pick] window list truncated at N` |
| **啟動氣泡是 topmost，蓋住錄製控件** | 全螢幕成品裡看不到控件，誤判成「排除失效」 | 測試腳本先用 `Win+N` → `Esc` 收掉氣泡再錄 |
| **用亮度判斷控件有沒有入鏡** | 桌布本身有暗塊 → 假陽性 | 用停止鈕的紅色當簽名（`r>150,g<110,b<110`） |
| **靜音用「跳過不編碼」** | 累計取樣點停住，之後的 AU dts 全往前擠，聽起來整段加速 | 送零樣本進編碼器，時間軸不動 |
| **D2D 弧的掃掠方向** | 麥克風圖示變成雨傘（弧從上面繞） | y 軸向下，180°→360° 要 `COUNTER_CLOCKWISE` |
| **控件位置存絕對座標** | 換解析度就跑掉 | 存相對擷取範圍右下角的位移 |
| **用 region 裁圓角** | 邊緣一格一格的階梯（「像素感太強」） | region 是 1-bit，沒有抗鋸齒。改 per-pixel alpha + `UpdateLayeredWindow`；連帶不能用 `SetLayeredWindowAttributes`、`WM_NCHITTEST` 要回 `HTTRANSPARENT` |
| **用 `PostMessage` 的 client 座標測拖曳** | 視窗不動（拖曳讀 `GetCursorPos`） | 測試要 `SetCursorPos` + `mouse_event` |
| **以為「同時貼兩處」要二選一** | 想著「要保像素還是保檔名」，其實剪貼簿本來就能同時放多種格式 | 一次 `OpenClipboard` 裡 `SetClipboardData(CF_DIB)` + `SetClipboardData(CF_HDROP)`。真正的限制是別的：`CF_HDROP` 帶的是**檔名**，檔案總管只做「複製那個檔案」，磁碟上沒有就是空操作 → 存檔必須排在寫剪貼簿**之前** |
| **`DROPFILES` 未宣告** | `error C2065: 'DROPFILES': 未宣告的識別項`，連帶五個錯誤 | 它在 `shlobj_core.h`，`windows.h` 不含。同時要 `#pragma comment(lib, "shell32.lib")` |
| **`fWide` 沒設成 `TRUE`** | 英文路徑正常，中文路徑貼出來是亂碼 | 用 UTF-16：`fWide = TRUE` + `memcpy` 寬字元，而且緩衝區要留**兩個**結尾 `\0`（一項結束 + 清單結束） |
| **空字串也照放 `CF_HDROP`** | 「已截圖」說有存，貼到資料夾卻沒東西 | 只有 `savedPath` 非空才放；標題也只在真的存檔時才寫「點一下開啟」 |

### 量測方法

| 坑 | 症狀 | 解法 |
|---|---|---|
| 跨執行比 PSNR | 只有 21 dB，像編碼器壞了 | 動畫相位不同。用 `--ab` 同一批幀多路輸出 |
| 為了補 PSNR 而在有解碼器的那台「重錄」一次 | 同上，21 dB | 不能重錄。要把 4070 Ti 產出的 mp4 **連同參考檔**一起帶過去量 |
| 直接比 coded size | H.264 816×640 vs HEVC 832×640 無法比 | 用 `MF_MT_MINIMUM_DISPLAY_APERTURE` 取可見區域 |
| 比同一個 `--quality` 的檔案大小 | 得到「HEVC 比較大」的錯誤結論 | quality 不是跨 codec 可比的刻度，要比同等 PSNR |
| 只用兩個 quality 點內插算同畫質比 | UI 組算出 91.7%、還得出「曲線交叉」，補到八點後是 104%；高熵組三點 92.7%、八點 90.6% | rate-PSNR 曲線是凹的，長弦會把中段算貴（也可能算便宜，方向看弦切在哪）。至少 5~6 個點、相鄰間隔 ≤5，超出範圍標 OUT 不外插 |
| 測試源用 `Sleep(16)` | 來源只有 39 fps，卻拿來驗「60fps 不掉幀」 | 用絕對截止時刻 + `timeBeginPeriod(1)` |
| `mp4_check` 用 `GetCurrentLength()` 當緩衝區上界 | AV1 解碼器的輸出是 DXGI surface，長度回 0，每一幀都被判成壞帧（`len=0`） | 回 0 時改用 `GetMaxLength()` |
| 用寬頻峰值找哔聲起點 | 「靜音」樣本也量到 11 個哔聲（環境 RMS −35 dBFS、峰值 −10.6 dBFS），配對結果全是雜訊 | 素材源播 3kHz 純音，量測端用 Goertzel 只取那個窄帶（`--sync --freq 3000`）。純音 max/quiet 要 ≥ 4 倍，否則工具會自己印警告 |
| 哔聲週期設成反轉週期的整數倍來「避免混疊」 | 500ms 反轉 + 1500ms 哔聲，`--audio-delay 500` 竟然顯示對齊 —— 1500 是 500 的倍數，哔聲仍在反轉格點上 | 去混疊靠**限制掃描範圍**到 ±P/2，不是靠改週期。超過 ±P/2 的偏移會混疊（−500ms 在 ±200ms 窗裡顯示 −96.7ms），要量就把 `FLIP` 拉長 |
| 40ms 哔聲 | 一個哔跨兩個 AAC AU，能量中途掉回門檻，被數成兩次（16 個哔數成 27 個） | 哔聲拉長到 150ms（橫跨 6~7 個 AU），再加 120ms 防抖 |
| `--sync file -v`（`-v` 放第 4 個位置） | 舊版把第 4 個參數當門檻，`atoi("-v")=0`，門檻變 0 → 整段音訊只算一個 onset，**看起來像完全對齊** | `--sync` 改用 `--freq/--period/-v` 具名參數 |
| 上一輪的 `anim_window` 沒殺掉就錄下一組 | 兩個素材源同時發聲，9 秒檔案量到 15 個哔聲（應該 6 個），數字看起來照樣合理 | `out/sync_ab.sh` 每次開頭 `taskkill`，收工再殺一次 |
| 錄整個螢幕來量亮度跳變 | 800×600 的視窗在 1600×900 螢幕上只佔中間一塊，亮度差被稀釋（151..204） | 用 `--hwnd` 只錄那個視窗（39..233） |
| 用「回傳 −1」表示「讀不出容器宣告的位移」 | `elst` 的負位移（裁掉開頭）是**合法值**，被 −1 哨兵吃掉。症狀是量測端印 `E = ?`，看起來像解析器沒實作，其實是值被吞了 | `declaredAudioDelayMs` 改成回傳 `bool` + `int64_t* out`。任何「合法值域包含哨兵」的 API 都有這個問題 |
| fMP4 的負位移裁 AU 用 `ceil` | 真實偏移只有 9ms 時，ceil 丟 23ms，補償後是 −14ms —— 比不補償（+9ms）離對齊**更遠** | 用 `floor`（只丟整塊落在裁切區內的 AU）。多補的方向是危險的，寧可少補 |
| 假設「fMP4 的 E 一律已含在媒體時間裡」 | 負位移是「直接丟 AU」，丟完 `tfdt` 還是 0，往前拉這件事已經全反映在 D 裡；再加一次 E 就變成減兩次 | 只有 **E > 0** 的 fMP4 才算「已含在媒體時間」（`tfdt` 往後推）；E < 0 要當成 progressive 那樣另外加 |
| 把「量到 14.6ms」直接寫成 `--av-offset -14.6` | 校準檔看起來有效，但 fMP4 量化成 0 AU，實際什麼都沒做 —— 靜默失效 | 校準腳本先過 ±10ms 對齊門檻（寫 0）；若負補償小於一個 AU 就明講 fMP4 無效 |

### GUI 測試腳本（模擬鍵鼠時的假失敗）

這幾條的共通點是：**症狀看起來像程式壞了，其實是測試端不成立**。全部實際踩過。

| 坑 | 症狀 | 解法 |
|---|---|---|
| 只做 `AttachThreadInput` + `SetFocus` | `focus[1020]=True` 但欄位完全沒反應，誤判成「首個和弦抓不到」，繞了一輪 | `keybd_event` 的鍵只送**前景視窗**。每步都要 `BringWindowToTop` + `SetForegroundWindow`，並斷言 `GetForegroundWindow() == h` |
| 用 `PostMessage(WM_KEYDOWN)` 測熱鍵 | 欄位永遠抓不到 | 修飾鍵是 `GetKeyState` 讀**真實鍵盤狀態**，`PostMessage` 不會改變它。必須 `keybd_event` |
| `Win` 案例與其他案例同一個行程 | 跑完 Win 那步後全部失效（`fg=False`、`<no ctl>`），像連環壞掉 | Win 的 keydown 彈出開始功能表搶走前景。Win 案例拆到獨立行程 |
| 測試腳本並行執行 | 看到殘留的 `Shift+F7`、`<no ctl>`，誤判成競態 | 搶同一個 exe 與前景，一律串行 |
| PowerShell 變數不分大小寫 | `Chord($WIN,$SHIFT,$P)` 說「找不到超載」 | `$p` 與 `$P` 同一個變數（`-PassThru` 的 Process 物件）。鍵碼改用 `$VKP` |
| 跨行程讀 EDIT 用 `GetWindowTextW` | 欄位已寫回新值，腳本讀到舊值 | 用 `SendMessage(WM_GETTEXT)`；P/Invoke 要 `EntryPoint="SendMessageW"` |
| 測試腳本存成無 BOM 的 UTF-8 | 錯誤訊息像「字串遺漏結尾字元」 | PowerShell 5.1 會當 ANSI 讀。一律存 **UTF-8 with BOM** |
| **ctypes 沒宣告 `restype` 就碰 64 位元 handle** | `GetClipboardData` 傳回的值被截成 32 位元，下一個 `DragQueryFileW` 就去讀垃圾位址：`OSError: access violation reading 0x…D2E0`，看起來像「這段程式碼有記憶體錯誤」 | `GetClipboardData` / `GlobalLock` / `DragQueryFileW` 都要 `restype = c_void_p` 或 `c_size_t`、`argtypes` 一起宣告。同一個坑會連續咬兩層（`GlobalLock` 接著報 `OverflowError: int too long to convert`） |
| **`MAKELPARAM` 的高低字寫反** | 拖出 200×360 的矩形卻斷言 360×200 —— 因為它**不會失敗**，只是把矩形轉置了 | x 在**低**字、y 在**高**字：`((y & 0xFFFF) << 16) \| (x & 0xFFFF)`。寫成 helper 並留註解，順手用尺寸斷言把它釘住 |
| **交易日誌逐行比對多行訊息** | `toast 已截圖: <body>` 的 body 自己含 `\n`，檔名落在**下一行**，腳本永遠找不到 → 誤判成程式沒做事 | 先把那段 log 切片 `join("\n")` 再比對 |

---

## 下一步

按優先級：

0. ~~**GUI / 產品化**~~ **已完成（2026-10-18，最短閉環 + 三項 UX 強化）**。
   托盤 + 全域熱鍵 + 拖曳框選（單擊選視窗）+ **錄製中漂浮控件**（停止 /
   麥克風靜音，可拖曳、記住位置、全螢幕也不入鏡）+ 設定視窗，
   `build.bat gui` → `out\CapWGC.exe`（600 KB）。錄製迴圈先抽成
   `src/core/recorder.h/.cpp`，避免出現第二個實作；CLI 保留自己那一份不動
   （所有量測腳本依賴它的輸出行為）。
   三項 UX 強化：**熱鍵可自訂並持久化**（五組，設定視窗擷取，衝突偵測，
   註冊失敗可整套回滾）、**Discord 式縮圖牆來源選擇**（`Ctrl+Alt+W`，
   視窗/螢幕兩分頁、即時縮圖、鍵鼠導航）、**漂浮控件**（2026-10-18 第二輪，
   取代原本的外框 + 計時徽章）。
   縮圖牆**即時更新**也已完成（2026-10-18 第七輪）：清單 400ms 重比對、
   縮圖 120ms 分批補抓，用 `hwnd` 為身分原地更新（不重排、保留選取），
   即時路徑不做螢幕擷取回退以免拍到自己。`--picker-static` 可關掉。
   細節與踩過的坑見 `docs/09-gui.md`（§4.9.5）。
   剩下的是錄後編輯 / GIF 匯出（第 8 項）、托盤記住上次來源、多螢幕排版、
   首次啟動引導。
1. ~~**補齊 AV1 vs HEVC 的同畫質比較**~~ **已完成（2026-10-17，第三輪把高熵也補到八點）**。
   結論：**高熵 AV1 是 HEVC 的 91~100%（中位 98%，省 0~9%），UI 反而 101~121%
   （中位約 107%）** —— 不是規格書講的 20~30%，UI 上方向還是相反的。
   原本「高熵 82~101%、UI 53~83%」是同一個 quality 旋鈕下的大小比，不可比 ——
   q75 時 AV1 少了 2.2 dB 畫質，檔案當然小。真正的落差在 AV1 對 H.264（52~59%），
   而 HEVC 對 H.264 已有 59~71%。細節見 `docs/06` §3.4、§3.4.1、§3.4.3。
   中途一度算出「UI 91.7%、兩條曲線交叉」，那是**兩點內插的假象** —— 補到八個點後
   交叉消失，AV1 從頭到尾沒有低於 100%。教訓：同畫質比至少要 5~6 個點。
   高熵也補到八點（q40~q75，`out/hard_sweep/`，3060 Ti 量完 16 個檔）：
   **比值不是常數** —— 41.75 dB 省 9.4%、46 dB 歸零、52.5 dB 回到 93%。
   AV1 那條接近直線，**HEVC 那條不是**（相鄰間距 1.67 / 2.03 / 1.93 / 1.90 /
   2.78 / 1.65 / 1.96 dB），所以三點版在低碼率端被長弦壓了 2 個百分點
   （92.7% → 90.6%）。同一個坑第三次踩到，方向還是反的。
2. ~~**UI 組的 H.264 只有 q60/q75 兩點**~~ **已補，結論翻轉（2026-10-17 第二輪）**。
   補點時發現這張卡的 H.264 quality 旋鈕**既不單調也不連續**：同批幀下
   QP 16 → QP 15 只多 5.2% 位元卻跳 +7.39 dB；q80 → q85 多花 20% 位元反而掉
   0.18 dB。三種下法（原生 Quality / Quality+釘死 QP / UnconstrainedVBR+釘死 QP）
   都重現，而且重疊處互相吻合到 0.3%，所以不是量測問題，是旋鈕本身分段。
   因此「掃曲線再內插」全部作廢，改成**同批匹配 PSNR 直接量比值**：
   **UI 63~66%、高熵 52~59%**。舊的 55% 是高熵組的數字被套到 UI 上。
   細節與證據見 `docs/06` §3.4.2，原始檔在 `out/h264_recheck/`。
   剩下：旋鈕的根因未定（要 NVENC SDK 層的 log 或另一張卡對照）；H.264 在
   UI 的 49~54 dB 之間沒有可用操作點。
3. **AV1 的 quality→QP 斜率**。原本 0.62 是拿別台量到的 HEVC 47.60 dB 對齊的，
   同一批幀重測後實際偏差是 **q40 +0.94 / q60 -0.83 / q75 -2.22 dB** —— 三個點都不齊，
   q75 比原本估計的 1.1 dB 嚴重一倍。反推要對齊 HEVC 所需的斜率是
   0.57 / 0.645 / 0.675，一路往上，一條直線對不齊。要嘛分段，要嘛換成查表。
4. **B 幀**（省 15~25% 空間）。`BPictureCount` 在 HEVC 與 AV1 都被 MF 拒絕，但硬體
   支援。muxer 的 rank 法（`BuildTimeline`）已經寫好也測過，缺的是讓 MFT 真的吃下
   設定。要注意分片內的重排序不能跨分片邊界。
5. ~~**音訊軌**。目前只有視訊。AAC 走 MF 的 AAC encoder MFT，muxer 要加第二條
   `trak` 與 `traf`。~~ **已完成（2026-10-17）**。WASAPI loopback → MF AAC-LC
   （raw AU + ASC 從 `MF_MT_USER_DATA`）→ 第二條 trak/traf。驗收：放測試音
   `peak=-13.0 dBFS`；6 秒錄影影像 5.968s / 音訊 5.968s（差 0 ms）；有音軌時
   fMP4 每個 moof 雙 traf，強殺後仍可解。**過程中兩個坑的症狀都不指向自己**：
   `esds` 少包最外層 `ES_Descriptor(0x03)`（MF 只認得影像軌，回
   `no audio stream 0xC00D36B3`），以及系統安靜時 loopback 一個 packet 都不送
   （要自建靜音渲染串流 keep-alive）。細節見 `docs/07-audio.md`。
   剩下的是多軌（麥克風分開）、裝置選擇、Opus 選項 —— 都不是缺口。
   **多軌與裝置選擇已完成（2026-10-17 第二輪）**：`--audio` / `--mic` /
   `--audio-src` 各成一條 trak，`--audio-device` / `--mic-device` 可指定端點。
   驗收：系統聲 + 麥克風同時錄、三條來源（含閒置裝置）、兩條 loopback 抓同一端點、
   fMP4 三 traf 抗 kill、progressive 雙軌，全部通過。**兩個坑**：MF 的 stream
   索引不照 trak 順序（音軌在前、影像在後、多音軌還是反序），以及 `main` 的 argv
   是 ANSI（中文裝置名比對不到）。細節見 `docs/08-multi-audio.md`。
   剩下的是 Opus、A/V 偏移補償、每軌獨立碼率、熱插拔。
   **A/V 偏移（2026-10-17 第三、四輪）—— 結論被推翻了，要照新的看**：
   - 容器這一半是對的：影像零點改成「第一筆寫入的幀」，音訊延到那一刻才 `Start()`，
     延遲寫進 `edts/elst` 的 empty edit（progressive）或第一個 `tfdt`（fMP4）。
     實測宣告 60ms → 呈現差 +57.5ms（`elst`）/ +65.0ms（`tfdt`）；宣告 500ms
     能把真實的 500ms 延遲抵到 +15.5ms。**機制可用**。
   - 但「偏移是多少」這一半量錯了。QPC 記帳得到的 56~72ms 是「WGC 幀時間戳」與
     「loopback packet QPC」各自的管線延遲差，不是音畫差。用內容事件
     （`anim_window --sync` 的 3kHz 純音對整屏反轉 + `mp4_check --sync`）量出來，
     真實偏移是 **−1.3 ~ +10.9ms** —— 檔案本來就對齊。
   - 所以 `CapCli` 的預設改成**不補償**，並提供 `--av-offset <ms>` / `--av-offset auto`。
     先前預設補 60ms 的版本會讓音訊晚 54~82ms，比不補更糟。
   - 工具：`out/sync_ab.sh <label> [CapCli args]` 一鍵跑一組；判讀見 `docs/08` §6.5，
     推翻過程見 §6.6。
   **負偏移與校準（2026-10-17 第五輪，接在上一輪之後）**：
   - 位移的符號定死：`N > 0` = 往後推（音訊本來偏早）走 `elst` empty edit / `tfdt`；
     `N < 0` = 往前拉 = **裁掉開頭**（音訊本來偏晚）。「要寫的 N」= −「量到的 D」。
   - 負偏移的兩條路都實作了，也都量過：progressive 寫 `elst` 的 `media_time`
     （單位是 mdhd timescale，44.1k，不是 movie 的 1000）精確到 tick；
     fMP4 沒有 `edts`，真的丟開頭幾個 AU，量化到 23.2ms，取整用 **floor**。
   - `out/calibrate.sh`：兩輪量測解混疊（FLIP=2000 定向 + FLIP=400 精修）→
     寫 `%LOCALAPPDATA%\CapWGC\av-offset.txt` → 立刻用 `--av-offset auto` 驗一次。
     本機量到 +6.3ms，落在 ±10ms 門檻內 → 寫 0（= 不補償，與 §6.6 一致）。
   - **換硬體怎麼辦**：這個偏移是 per-machine 的管線延遲差，推導不出來、只能量，
     所以換 GPU／音效驅動／Windows 版本就重跑一次 `calibrate.sh`（約 30 秒）。
     程式與容器格式都不用改 —— 這就是「換不同等級的硬體不會有問題」的答案。
   - 細節與四組實測見 `docs/08` §6.7。
   剩下的：AAC priming（23~46ms）還沒補、fMP4 的小幅負位移（< 一個 AU）表達不了。
6. **1080p 以上的 AV1 實測**。這台桌面只有 1280×720（RDP），解析度上不去。
   NVENC 的 AV1 標稱 8K60，實際要等有高解析度桌面的機器。
7. **`hvcC` 的 `general_level_idc` 是 0**。編碼器自己寫的，不是我們的 bug。
   MF 不在意，但嚴格的解析器可能挑。可以在組 hvcC 時從解析度回推一個合法值。
8. **錄後編輯 / GIF 匯出**。這是產品差異化的部分，目前完全沒動。
9. **`--codec auto` 現在真的會選 AV1**（優先序 AV1 > HEVC > H.264）。
   要不要因為相容性把 HEVC 擺前面，是產品決策。

---

## 環境資訊

**現在這台（4070 Ti）**

- Windows 11 Build 26100，**RDP session**，桌面 1280×720
- GPU：**RTX 4070 Ti**（Ada，有 AV1 硬編），驅動 616.92
- VS2022 BuildTools（MSVC 14.44）+ Windows SDK 10.0.26100.0
- 解碼器：只有 H.264 與 AV1（AV1 Video Extension 已安裝）。**沒有 HEVC 解碼器**。
- `build.bat` 會自動找 `vcvars64.bat`；沙箱擋掉 `reg.exe` 時會走手動 fallback
  設定 INCLUDE/LIB/PATH，這是正常的

**前一台（3060 Ti，docs/01~05 的數據來源；docs/06 的 HEVC PSNR 與 docs/07 的音軌實測也在這台）**

- Windows 11 Build 26100，**console session**（非 RDP），hostname `tokyo`
- 顯示器 1920×1080@239Hz
- GPU：**RTX 3060 Ti**（Ampere，無 AV1 硬編），驅動 610.88
- **有 HEVC 解碼器**（4070 Ti 那台沒有）
- VS2022 Enterprise + BuildTools
- 工作目錄 `D:\workspace\CapWGC`（兩台路徑不同）
- 音訊端點：預設播放裝置的 mix format 是 **44100 Hz 2 ch**（AAC 的 ASC 2 bytes）。
  render 端點 3 個（預設 `Realtek HD Audio 2nd output`，44.1k）、capture 端點 2 個
  （預設 `麥克風` 48k、另有 `立體聲混音`）。多音源的實測都在這台（`docs/08`）。
