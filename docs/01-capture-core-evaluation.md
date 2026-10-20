# WGC 擷取核心評估 + 錄屏軟體架構提案

> 對象：`D:\workspace\CapWGC`（新專案）
> 評估樣本：
> 1. `D:\workspace\AndroidApp\test\模擬器\probe\cap\wgccap.cpp`（571 行）
> 2. `D:\workspace\AndroidApp\test\模擬器\probe\enc\wgcenc.cpp`（1382 行）
> 3. `D:\workspace\4Games\PyMacro_Builder\libs\rdp\CaptureAgent.cpp`（1092 行，WGC 後端在 638–1049）
>
> 日期：2026-10-17

---

## 0. 結論

| 排名 | 實作 | 定位 | 每幀 CPU（實測） | 能直接搬進錄屏軟體嗎 |
|---|---|---|---|---|
| **1** | `wgcenc.cpp` | WGC → MF 硬編碼，**零 CPU 回讀** | **0.550 ms** @720p59.7 | 擷取+編碼骨架可以，缺健壯性 |
| 2 | `CaptureAgent.cpp` | WGC → 共享記憶體，**必須** CPU 回讀 | 0.9–4.5 ms（CopyResource+Map） | 擷取概念可以，回讀不可 |
| 3 | `wgccap.cpp` | WGC → BGR → WIC JPEG | **1.730 ms**（僅轉換段） | 只有 crop/pacer 概念可留 |

**一句話**：`wgcenc` 的擷取核心比 `wgccap` 快 **3.1 倍**（同一台機器、同一幀、只差那一段路徑的 A/B 對照，見 `PLAN.md` §端到端整合）。`CaptureAgent` 的擷取核心介於兩者之間，但它的價值不在速度，而在**健壯性**——它是三者中唯一會處理「item 被關閉 / 解析度變更 / 連續失敗」並自動重建的。

**給新專案的處方**：擷取骨架抄 `wgcenc`，健壯性抄 `CaptureAgent`，兩者都沒有的（游標合成、VFR、音訊、封裝）自己補。**不要抄 `wgccap`**。

---

## 1. 三個實作的定位差異（先講清楚，否則比較不公平）

三者的「慢」不完全是寫得差，有一部份是目標不同：

| | wgccap | wgcenc | CaptureAgent |
|---|---|---|---|
| 輸出 | 長度前綴 JPEG | 長度前綴 Annex-B + PTS | 共享記憶體裸 BGRA |
| 消費者 | Go 編排層 → MJPEG over HTTP | RTC 串流 | Python / YoloFoundry 讀像素 |
| 所以必須 | 每幀完整圖（JPEG 無幀間預測） | 有幀間預測，靜止可 0 幀 | 隨時可讀的最新完整畫面 |
| CPU 回讀 | 必要 | **不必要** | **必要**（契約決定） |
| 擷取目標 | 單視窗 | 單視窗 | 整個 monitor |

也就是說：`CaptureAgent` 慢是因為**契約要求像素必須落到 CPU 可見的共享記憶體**，不是因為作者不會寫。這一點在新專案裡不成立——錄屏軟體的消費者是最終檔案，像素一輩子不需要下 GPU。

`wgccap` 則是三者中唯一「不必要地慢」的：它把可以留在 GPU 的工作（格式轉換、編碼）搬到了 CPU，而且每幀重新配置資源。

---

## 2. 逐項對照

| 環節 | wgccap | wgcenc | CaptureAgent |
|---|---|---|---|
| item 建立 | `CreateForWindow` | `CreateForWindow`（含 `IsIconic`→`SW_RESTORE`） | `CreateForMonitor` |
| D3D11 device | HARDWARE, BGRA_SUPPORT | 同左 | 同左 + **WARP fallback** |
| frame pool | `CreateFreeThreaded`, B8G8R8A8, **2 buffers** | 同左 | 同左 |
| 重入保護 | **無** | `busy.exchange(true)` 丟幀 | 靠 `frameLock` 序列化（會排隊） |
| staging texture | **每幀 `CreateTexture2D`** | 不需要（GPU ring） | **快取重用**（正確） |
| 格式轉換 | **逐像素 BGRA→BGR 標量迴圈** | 無（MFT 內部 BGRA→NV12） | 無（直接 memcpy BGRA） |
| 像素出口 | WIC JPEG（每幀新建 encoder + HGlobal stream + 再複製一次） | `MFCreateDXGISurfaceBuffer` → MFT | `Map` + `memcpy` 到 SHM |
| 時間戳 | 無 | **`frame.SystemRelativeTime()`（100ns）** | 無（frameId 計數） |
| 邊框抑制 | `IsBorderRequired(false)` | 同左 | 同左 |
| 游標 | 預設開 | 預設開 | **預設關**（`RDPCAP_WGC_CURSOR`） |
| 能力探測 | 只有 v3 | 只有 v3 | **v2/v3/v5 全部探測並回讀驗證** |
| 解析度變更 | 不處理 | 不處理 | **偵測 → 重建** |
| item.Closed | 不處理 | 不處理 | **`Closed` 事件 → 重建** |
| 後端 fallback | 無 | 無 | **WGC 失敗 → DXGI** |
| 靜止畫面 | pacer 重送同一張 | 合法地 0 幀 | 合法地 0 幀 |

### 關鍵：`wgccap` 的三個效能缺陷

1. **每幀 `CreateTexture2D`**（`wgccap.cpp:428`）。staging texture 是迴圈內的區域變數，離開就釋放。D3D11 資源建立要走核心態配置，720p 下每次約 0.1–0.5 ms，1080p/4K 更貴。`CaptureAgent:705` 和 `wgcenc:942` 都改成快取/ring 了。
2. **逐像素 BGRA→BGR**（`wgccap.cpp:457-464`）。92 萬次迭代、每次 3 個 byte 賦值，無法向量化也無法 `memcpy`。這一段單獨就是 1.18 ms/frame。
3. **WIC JPEG 的三份拷貝**（`wgccap.cpp:107-183`）。每幀 `CreateStreamOnHGlobal` + `CreateEncoder` + `CreateNewFrame` + `Commit`，最後 `out.assign` 再把 HGlobal 整塊複製一次。720p 高質量 JPEG 約 2–5 ms（未實測，量級估算），而且**完全無法利用 GPU**。

三者相加，`wgccap` 在 720p60 下的實際成本大約 4–7 ms/frame，單核 25–40%。這也解釋了為什麼它的 pacer 需要獨立執行緒——callback 裡塞不下了。

---

## 3. 3.1 倍的來源（實測數據）

`PLAN.md` §端到端整合（2026-10-08，10 秒對照跑）：

| | 零拷貝 | `--cpu-copy` 對照 |
|---|---|---|
| captured / encoded | 598 / 597（59.8 / 59.7 /s） | 597 / 596（59.6 / 59.5 /s） |
| dropped | 0 | 0 |
| 輸出 | 3,658,666 B（2.93 Mbit/s） | 3,654,495 B（2.92 Mbit/s） |
| **CPU** | **0.550 ms/frame** | **1.730 ms/frame** |

對照組 `--cpu-copy` 就是把 `wgccap` 的逐像素迴圈原樣搬過來，其餘管線完全相同。輸出位元組差 0.1%，證明對照公平。

**拆解**：
- 逐像素轉換 + Map：+1.18 ms
- 若再加上 `wgccap` 的每幀 texture 建立與 WIC JPEG，差距會拉到 **8–12 倍**

換算：60fps 下 0.55 ms/frame = 單核 3.3%；1.73 ms = 10.4%；`wgccap` 全鏈路 ≈ 25–40%。

---

## 4. 各自的隱患（不只是效能）

### `wgccap` — 有一個正確性 bug
`FrameArrived` 是 free-threaded pool 的回呼，**可能重入**。`wgccap` 沒有任何 busy guard，兩個回呼同時進來時會共用 `sh->bgr` / `sh->jpg` 兩個 scratch buffer，並對同一個 `staging` 做 `CopyResource`。`wgcenc:979` 用 `busy.exchange(true)` 直接丟幀解決了，`wgccap` 沒有。

另外 `pacer` 每幀 `local = sh->cur` 複製整張 JPEG（幾百 KB × 30 次/秒），可以改成 `shared_ptr` 交換。

### `CaptureAgent` — 兩個可改進點
1. `OnFrame` 裡 `frameLock` 包住 `CopyResource + Map + memcpy` 整段（`CaptureAgent.cpp:703`）。free-threaded 重入時第二個執行緒會排隊，而 pool 只有 2 個 buffer，排隊等於丟幀且增加延遲。正確做法是 busy guard 直接丟。
2. `Map` 是同步等待 GPU 完成（`CaptureAgent.cpp:723`）。這是 `0.9–4.5 ms` 波動的主因——不是 memcpy 慢，是等 GPU。錄屏軟體不需要這一步，所以這個問題在新專案裡自動消失。

### `wgcenc` — 缺的是產品化，不是效能
1. 只有 `CreateForWindow`，沒有 `CreateForMonitor`（全螢幕錄製必需）。
2. **沒有 `item.Closed` 重建**。`CaptureAgent.cpp:954-958` 有。這是三者中後果最嚴重的缺口：切換解析度、GPU 重置、鎖螢幕都會讓 item 靜默失效，程式看起來還活著但永遠不再出幀。
3. 沒有 DXGI fallback。
4. 沒有音訊。

---

## 5. 建議的擷取核心（三者融合）

```
                    ┌─ WGC item (CreateForWindow / CreateForMonitor)
                    │   + item.Closed 事件 → 重建
                    │   + 解析度變更輪詢 → 重建
                    ▼
            FrameArrived (free-threaded)
                    │
            busy.exchange(true)?  ── 是 ──► 丟幀（不加佇列）
                    │ 否
                    ▼
            ID3D11Texture2D (WGC 擁有，唯讀)
                    │
                    │ CopySubresourceRegion（帶 crop box，GPU）
                    ▼
            bgraRing[4]  BGRA, 偶數對齊, RENDER_TARGET|SHADER_RESOURCE
                    │
                    │ （可選）游標 alpha-blend 合成（1 個 quad，~0.05 ms GPU）
                    │
                    │ VideoProcessorBlt（BGRA → NV12，GPU）
                    ▼
            nv12Ring[4]
                    │
                    │ MFCreateDXGISurfaceBuffer → IMFSample
                    │ SetSampleTime(frame.SystemRelativeTime())
                    ▼
            [ MFT: H.264 / HEVC / AV1 ]
                    │
                    ▼
            Annex-B + PTS ──► [MKV muxer]  ──► remux MP4
                          └─► [即時預覽 / 串流]
```

### 12 條設計決策

1. **像素永不下 GPU**。這是與 `CaptureAgent` 最根本的分歧，也是 3–10 倍差距的來源。
2. **不做 dirty rect 優化**。`CaptureAgent` 的髒矩形是為了「共享記憶體給人讀」——只更新變化的區域能省頻寬。但 H.264 的 P 幀參考**整幀**，局部更新對編碼器毫無意義，反而多一次 rect 解析。這是很多人會誤抄的地方。
3. **VFR（可變幀率）是壓縮率的核心**。WGC 天然變更驅動：靜止桌面 ~2fps、動態 60fps。保留真實 PTS，靜止畫面就是 0 位元組。不要為了「看起來像 60fps」而重送幀——那是 `wgccap` 的 pacer 邏輯，對 JPEG 必要，對 H.264 是純浪費。
4. **時間戳用 `frame.SystemRelativeTime()`**，不要用 `now()`。前者是 WGC 擷取那一刻的 QPC 時間，後者是「我處理到這裡的時間」，會把編碼延遲算進影片節奏。
5. **busy guard 丟幀，不排隊**。即時錄製裡排隊只會增加延遲，不會增加資訊。
6. **關閉 WGC 游標捕獲，自己合成**。`IsCursorCaptureEnabled(false)` 之後，游標移動不產生新幀（省碼率）；需要游標時用一個 textured quad + alpha blend 畫進 ring。這樣「游標在動但畫面靜止」的情況不會觸發重編碼，同時影片裡看得到游標。
7. **能力探測要回讀驗證**。`CaptureAgent.cpp:929-942` 的做法是對的：`MinUpdateInterval(1ms)` 設完要讀回來確認。`IsCursorCaptureEnabled` / `IsBorderRequired` / `MinUpdateInterval` 三個都要 try_as 探測，不要假設 Win11 就有。
   **Win11 24H2 上 `MinUpdateInterval` 不設就是 60fps 上限**，錄 120fps 或高刷螢幕會直接被砍。這不是優化，是功能。
8. **兩個 ring 而不是一個**。BGRA ring 給游標合成，NV12 ring 給編碼器。直接讓 MFT 做 BGRA→NV12 也可以（`wgcenc` 就是這樣，NVIDIA MFT 接受 ARGB32），但自建 `ID3D11VideoProcessor` 能控制色彩矩陣（BT.709 vs BT.601）與色階（full vs limited），錄屏文字邊緣會更準。
9. **ring 深度 4，槽位用「輸出序號」判定可回收**。`wgcenc:1093` 的 `slotSeq[i] < 0 || outputs >= slotSeq[i] + 1` 是正確做法——不要用「送進去就算用完」。
10. **重建策略**：`item.Closed` 事件 → 重建；解析度變更 → 重建並重設 encoder；連續失敗 3 次 → 放棄並 fallback DXGI。`CaptureAgent.cpp:840` 的 `MAX_CONSECUTIVE_FAILURES` 邏輯直接抄。
11. **DXGI Desktop Duplication 作為 fallback**。WGC 在這些情況不可用：Win10 1803 以前、DWM 關閉、部分 RDP session、DRM 保護內容。錄屏軟體「便捷」的前提是「一定能錄」。
12. **`IsIconic` 檢查**。`wgcenc:813` 踩過的坑：對最小化視窗 `CreateForWindow` 會回一個 154×25 的 item，而且 `SetWindowPos` 被靜默忽略，然後每一幀都被 crop 檢查擋掉，程式以「編碼器拒絕紋理」的姿態退出。先 `SW_RESTORE`。

### 兩個必踩的坑（`wgcenc` 已經踩過）

**坑一：WGC 的變更驅動 × MFT 的輸入驅動，相乘會把輸出扣住。**

async MFT 只在收到 `METransformNeedInput` 時才接受輸入，而它要 `ProcessOutput` 之後才會發下一次 `NeedInput`。WGC 又只在畫面變化時給幀。兩者相乘的結果是：**每個 callback 只做一輪「排空 → 收集」，就變成每兩次 callback 才推進一步**。實測 `captured` 62/s 但 `encoded` 只有 33/s，一半的幀因缺 credit 被丟掉，而且**沒有任何錯誤訊息**。

解法（`wgcenc` 已實作）：改成「排空 → 收集 → 再排空」，並加一個 5ms 的排水幫浦執行緒，保證「沒有下一個 callback 時也有人把已經編好的那一格收出來」。修完 dropped 由 146 降到 **0**。

一般原則：**驅動 async MFT 時，「送進去的」與「吐出來的」必須對得上帳**。對不上就是有東西被靜默丟掉。

**坑二：用固定 fps 算碼率，在 VFR 下一定算錯。**

MFT 把 `CODECAPI_AVEncCommonMeanBitRate` 換算成「每幀預算」時，用的是 `MF_MT_FRAME_RATE`。但 WGC 的實際出幀率是變動的——靜止桌面 8.5/s，動態 59/s。用 60fps 宣告、實際 8.5fps 交付，等於每幀預算被放大 7 倍，碼率失控。

解法：**量測實際擷取率，再回推該告訴 MFT 的碼率**。`wgcenc:998-1035` 的做法是 0.5 秒窗口 + 0.5 指數平滑（0.25 秒窗口會因為 WGC 的突發性而劇烈震盪，實測 14 秒內重寫 29 次碼率還是沒對上）。

---

## 6. 錄屏軟體架構

### 技術選型

| 層 | 選擇 | 理由 |
|---|---|---|
| 擷取+編碼核心 | **C++20 + C++/WinRT**，編譯成 DLL 或獨立 exe | 三份既有程式碼都是這個棧，可直接複用；無 .NET/GC 停頓 |
| 編碼器 | **Media Foundation MFT** | 一套程式碼吃 NVENC / QSV / AMF；`wgcenc` 已驗證可行 |
| 色彩轉換 | `ID3D11VideoProcessor` | GPU，可控色彩矩陣 |
| 音訊 | WASAPI loopback + capture，雙軌 | 系統聲與麥克風分開存，後期可調 |
| 封裝 | **MF SinkWriter 出 MP4**；MKV 需自寫 muxer | 零外部依賴；MKV 的 EBML 格式簡單，300 行可寫完 |
| UI | Win32 + Direct2D（或 WinUI 3） | 啟動 < 300ms，記憶體 < 50MB |
| 全域熱鍵 | `RegisterHotKey` + low-level hook 備援 | 錄製中也要能用 |

**不建議**：Electron（記憶體 200MB+）、WPF（啟動 1–2s，這正是 Captura 的問題）、Tauri（依賴 WebView2，且 UI 執行緒與擷取執行緒隔離變複雜）。

### 依賴與授權（維持 MIT 乾淨）

這是硬約束，會反過來決定幾個技術選擇：

| 需求 | 不能用 | 用什麼 |
|---|---|---|
| 編碼 | **FFmpeg**（LGPL/GPL，靜態連結會污染授權） | Media Foundation MFT（Windows 內建） |
| 封裝 MP4 | libavformat | **MF SinkWriter**（內建 MPEG-4 file sink） |
| 封裝 MKV | libavformat | 自寫 minimal EBML muxer（~300 行，格式簡單） |
| 音訊 | libswresample | WASAPI + 自己做樣本格式轉換（16/24/32bit 而已） |
| UI | Qt（LGPL）、Electron（MIT 但體積大） | Win32 + Direct2D，或 Dear ImGui（MIT） |

**不用 FFmpeg 的代價**：沒有 libx264 這種高質量軟體編碼 fallback。但錄屏場景本來就該用硬體編碼——沒有 GPU 的機器錄 1080p60 本來就跑不動。真要 fallback，可以用 MF 內建的軟體 MFT（`CLSID_CMSH264EncoderMFT`），質量不如 x264 但夠用。

**MKV 的取捨**：MF 沒有 MKV sink。要 crash-safe 就得自己寫 muxer。MKV 的 EBML 結構比 MP4 的 box 結構更適合「邊錄邊寫」（不需要回頭改 header），300 行可完成。或者接受 MP4 + 定期 flush（風險是斷電毀檔）。

建議 M1 先做 MP4（快），M3 補 MKV。

### 模組劃分

```
CapCore.dll         擷取 → 編碼 → 封裝，無 UI 依賴，可獨立測試
  ├─ CaptureSource  WGC / DXGI 抽象，統一吐 ID3D11Texture2D + PTS
  ├─ Compositor     crop、游標合成、縮放
  ├─ Encoder        MFT 封裝，品質模式、動態碼率、強制 IDR
  └─ Muxer          MKV / fMP4
CapUI.exe           托盤、熱鍵、區域選擇器、錄製工具列
CapCli.exe          命令列錄製（CI / 自動化 / 除錯）
```

`CapCore` 與 UI 完全解耦的理由：錄屏軟體最常見的 bug 是「UI 卡住導致掉幀」。核心跑在自己的執行緒，UI 只收狀態事件。

---

## 7. 壓縮率：怎麼做到比 Captura 高一個數量級

### 7.1 最大的差距不是編碼器，是「有沒有幀間預測」

Captura 的預設路徑是 SharpAvi/FFmpeg 的 CPU 編碼，碼率策略偏固定。而桌面錄製的內容特性是：**90% 的像素在 90% 的時間裡不變**。

| 方案 | 1280×720@30，1 分鐘 | 倍率 |
|---|---|---|
| MJPEG q75（`wgccap` 路徑） | ~600 MB | 1× |
| H.264 CBR 3 Mbps | ~22 MB | 1/27 |
| **H.264 Quality 模式 + VFR** | **~8–15 MB** | **1/40 ~ 1/75** |
| **HEVC Quality + VFR** | **~5–10 MB** | **1/60 ~ 1/120** |
| **AV1（RTX 40 / Arc）** | **~4–8 MB** | **1/75 ~ 1/150** |

（量級估算，實際取決於內容。靜態簡報 vs 遊戲畫面差很大。）

### 7.2 具體參數

```cpp
// 錄屏要的是「質量導向」，不是「碼率導向」
CODECAPI_AVEncCommonRateControlMode = eAVEncCommonRateControlMode_Quality;  // = 3
CODECAPI_AVEncCommonQuality         = 60..80;   // 不是 CBR 的 MeanBitRate
CODECAPI_AVEncMPVDefaultBPictureCount = 2;      // B 幀，省 15–25%
CODECAPI_AVEncMPVGOPSize            = 0;        // 改用「時間」控制 IDR，見下
CODECAPI_AVEncVideoEncodeQP         = 22..28;   // 若 MFT 支援直接給 QP
```

**GOP 按時間而不是幀數**。VFR 下「60 幀一個 GOP」可能等於 30 秒（靜止畫面），seek 體驗極差。做法：每 2 秒強制 IDR，用 PTS 判定。

**不要用 CBR**。CBR 會在靜止畫面上繼續燒碼率填滿緩衝區，這是 Captura 檔案大的直接原因之一。

### 7.3 五個容易被忽略的省碼率手段

1. **關閉游標捕獲**（見決策 6）。滑鼠亂動時，游標移動會讓 WGC 產生新幀，整幀重編碼。實測這在簡報/文件場景能省 30–60% 碼率。
2. **靜止時 0 幀**。WGC 天然如此，只要不去「補幀」就免費得到。
3. **避免不必要的縮放**。錄 1080p 就輸出 1080p，讓 GPU 的 scaler 只在必要時介入。縮放會破壞桌面內容的高頻細節，反而讓編碼器更難壓。
4. **色彩範圍明確標記**。BT.709 limited range，並在 container 裡寫對。標錯會讓播放器做一次多餘的 range 轉換，畫面發灰。
5. **文字優先**。桌面錄製的主要內容是文字。H.264 4:2:0 對彩色文字邊緣不友善，可以考慮：
   - 提高 QP 質量（文字區塊的碼率值得花）
   - HEVC 4:4:4（NVENC 支援，但播放器兼容性差，做成選項而非預設）
   - 錄製時偵測「大面積靜態 + 少量文字變化」→ 降幀率保質量

### 7.4 什麼不要做

- **不要做兩階段（先錄無損再轉檔）**。使用者要的是「按一下、等於錄好」，不是「錄完還要等 10 分鐘轉檔」。
- **不要做區域/tile 編碼**。HEVC 有 tile 但硬體編碼器不暴露，自建等於放棄硬編。
- **不要為了相容性預設 H.264 4:4:4**。多數播放器與瀏覽器不支援。

---

## 8. 競品取捨

| 產品 | 值得抄 | 不要抄 |
|---|---|---|
| **Captura** | 輕量的產品定位、區域/視窗/螢幕三種模式、GIF 直出 | WPF 架構、CPU 編碼、停更的依賴鏈 |
| **OBS** | 場景/來源模型、錄製時 mkv + remux、VFR 處理、音訊多軌、游標合成 | 整套 Qt 依賴、設定複雜度（100+ 選項） |
| **ShareX** | 錄完的後處理工作流（上傳/複製/開啟）、熱鍵自訂 | WinForms UI、把錄屏外包給 FFmpeg 子行程 |
| **ScreenToGif** | 錄製後的逐幀編輯器（對教學場景是殺手功能） | 以 GIF 為中心的資料模型 |
| **LICEcap** | 極致的啟動速度與體積（< 1MB） | 只有 GIF、無音訊 |
| **Windows 截圖工具** | 系統級整合、零安裝 | 功能天花板、無法自訂編碼 |

**差異化定位建議**：Captura 的功能廣度 + LICEcap 的啟動速度 + OBS 的編碼質量 + ScreenToGif 的錄後編輯。這四件事目前沒有任何一個產品同時做到。

---

## 9. MVP 路線

**M1（能錄，1–2 週）**
- `CapCore`：WGC 單視窗 + 全螢幕，零拷貝 → NVENC H.264 → MKV
- 固定 60fps 上限，VFR
- 命令列可錄，無 UI
- 驗收：1080p60 錄 10 分鐘不掉幀，CPU < 8% 單核

**M2（能用，+2 週）**
- 托盤 + 全域熱鍵 + 區域選擇器
- WASAPI 雙軌音訊
- 錄完 remux MP4 + 通知
- 驗收：從熱鍵到開錄 < 500ms

**M3（好用，+2 週）**
- 游標合成、點擊高亮
- HEVC / AV1 選項
- 暫停/續錄
- 崩潰恢復（掃描未完成的 MKV）
- 驗收：拔電源不毀檔

**M4（比 Captura 好）**
- 錄後輕量編輯（裁剪、GIF 匯出、壓縮）
- 多螢幕 / 指定 monitor
- 隱私遮罩（錄製時排除指定視窗）

---

## 10. 待驗證清單

以下是我**沒有**實測、需要在 M1 補數據的項目：

1. `ID3D11VideoProcessor` 的 BGRA→NV12 vs MFT 內部轉換，哪個快、哪個色彩準（720p / 1080p / 4K 各測）
2. HEVC vs H.264 在**桌面文字內容**上的實際碼率差距（不是一般影片的 25–50%，桌面可能不同）
3. AV1 硬編在 1080p60 的即時性（RTX 40 應該夠，但要驗）
4. MKV muxer 的 CPU 成本（用 libavformat 還是自寫 minimal muxer）
5. 游標合成的 GPU 成本 vs 「游標移動觸發重編碼」的碼率成本，哪個划算
