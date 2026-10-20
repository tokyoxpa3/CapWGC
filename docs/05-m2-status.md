# M2 狀態：自驅動 MFT + 自寫 muxer（含 fMP4）

> 日期：2026-10-17
> 產出：`out/CapCli.exe`（`--engine mft` 預設）、`out/mp4_check.exe`
> 原始碼：`src/core/mft_encoder.*`、`src/core/mp4_muxer.*`、`src/core/encoder.h`

---

## 1. 結論先講

| 驗收標準 | 結果 | 數據 |
|---|---|---|
| HEVC 檔案 < H.264 的 80% | **通過（同等畫質下 63~75%）** | 見 §3 |
| CPU < 1 ms/幀 @720p | **通過** | HEVC 0.86、H.264 0.54 ms/幀 |
| 1080p60 錄 10 分鐘不掉幀 | **通過** | 35 935 幀 / 59.9 fps / busy 丟幀 0，見 §5.4 |
| 強制 kill 後檔案仍可播 | **通過** | 見 §4，960 幀完整可解 |
| `--quality` 改動看得到大小變化 | **通過** | 2.6 → 5.6 → 10.8 → 15.2 MB |

四個「通過」裡有三個是修 bug 之後才成立的，而且那三個 bug 的症狀全部**不指向**
真正的原因。這也是這份文件主要的價值：把「看起來像 A、其實是 B」的對應關係記下來。

---

## 2. 架構

```
WGC (free-threaded pool)
  → 軟體幀率閘門（10% 容差）
  → GPU ring[4] (BGRA, crop)
  → VideoProcessorBlt → NV12 (BT.709 limited)
  → MFCreateDXGISurfaceBuffer → IMFSample
  → IMFTransform::ProcessInput
  → ProcessOutput（pump 執行緒每 5ms 排空一次）
  → Mp4Muxer（progressive 或 fMP4）
```

`--engine sink` 仍保留 M1 的 SinkWriter 路徑當對照組。

---

## 3. HEVC vs H.264：先確定你量的是什麼

### 3.1 同一個 QP 不等於同一種畫質

第一輪量測（1280×720、高熵內容、同一批幀、`--quality` 相同）得到：

| quality | HEVC | H.264 | HEVC 大小 |
|---|---|---|---|
| 30 | 2.74 MB / 36.56 dB | 2.47 MB / 34.49 dB | 111% |
| 50 | 5.92 MB / 45.27 dB | 5.57 MB / 42.18 dB | 106% |
| 70 | 11.66 MB / 54.32 dB | 10.85 MB / 46.69 dB | 107% |
| 85 | 16.98 MB / 60.47 dB | 15.21 MB / 60.56 dB | 112% |

看起來「HEVC 比較差」。但同一列裡 HEVC 的 PSNR 高 2~8 dB —— 它只是**多花了
位元去買畫質**。用同一條 quality→QP 直線餵兩個 codec，等於拿兩把刻度不同的尺
去量同一件事。

### 3.2 修掉映射之後

HEVC 的 quality→QP 斜率改成 0.44（H.264 維持 0.50），並把
`CODECAPI_AVEncCommonQualityVsSpeed` 從預設的 33 推到 100：

| quality | HEVC | H.264 | HEVC 大小 | 畫質差 |
|---|---|---|---|---|
| 40 | 2.57 MB / 40.14 dB | 3.41 MB / 38.60 dB | **75.3%** | +1.5 dB |
| 60 | 4.68 MB / 47.60 dB | 7.35 MB / 45.27 dB | **63.7%** | +2.3 dB |
| 75 | 7.33 MB / 53.90 dB | 11.21 MB / 56.05 dB | **65.4%** | −2.2 dB |

q40 與 q60 是**兩個軸都贏**。q75 那列 HEVC 畫質較低但仍小 35%，換算成同等
PSNR 大約是 74%，仍在 80% 之內。

`QualityVsSpeed` 是這次最大的單一增益：它預設 33/100，也就是「偏快」那一側。
推到 100 之後兩個 codec 都變好，HEVC 受益更多（q60 從 79.7% 降到 63.7%）。

### 3.3 量測方法本身有兩個陷阱

- **跨執行比較沒有意義**：兩次獨立錄製的動畫相位不同，逐幀 PSNR 會低到 21 dB
  （拿「方塊在左邊」比「方塊在右邊」）。必須用 `--ab` 讓同一批擷取幀同時餵給
  兩個編碼器，再加 `--ref` 產一份 q100 的近無損參考。
- **coded size ≠ 畫面尺寸**：H.264 對齊 16、HEVC 對齊 64。同一段內容
  H.264 是 816×640、HEVC 是 832×640，直接逐像素比會拿 padding 比 padding。
  要用 `MF_MT_MINIMUM_DISPLAY_APERTURE` 取可見區域。

---

## 4. fMP4：被 kill 也還能播

### 4.1 為什麼非做不可

progressive（moov 在檔尾）在行程被 kill 時，索引根本沒寫，整段錄影變成不可播的
二進位垃圾。實測對照：

| 容器 | kill 後檔案 | 能不能播 |
|---|---|---|
| progressive | 28.8 MB | **不能**，`MF_E_UNSUPPORTED_BYTESTREAM_TYPE` |
| fMP4 | 28.7 MB | **可以**，960 幀 / 16.085 秒全部解出 |

同樣大小的資料，一個是錄影、一個是垃圾。差別只在「索引放在前面還是後面」。

### 4.2 實作要點

- `moov` 在**第一個 sample** 之後才寫（那時才有 SPS/PPS），`mvex`/`trex` 告訴
  播放器這是分片檔
- 分片邊界對齊 GOP：遇到關鍵幀就把上一片收掉。損失上限是一個 GOP（預設 2 秒）
- `tfhd` 用 `default-base-is-moof`（0x020000），`trun` 的 `data_offset` 相對於
  moof 開頭 —— 每片自包含，不需要知道檔案絕對位置
- 每片寫完立刻 `fflush`。少這一步，被 kill 時最後一片會停在 CRT 緩衝區裡
- `Close()` 回填 `mvhd`/`tkhd`/`mdhd` 的時長（大小不變，原地覆寫）。被 kill 的
  檔案時長停在 0，播放器用分片累加，仍然能播

### 4.3 四個連續的容器 bug（每一個的症狀都不指向自己）

| 症狀 | 真正的原因 |
|---|---|
| `0xC00D36C4`，像「格式不支援」 | `stsz` 空表少寫 `sample_size` 欄位（要 8 bytes，只寫了 4），後面每個 box 邊界錯開 |
| 同上 | `BuildStsd()` 回傳的已是完整 `stsd` box，又被包了一層。MF 把內層 box header 當 sample entry，subtype 變成 `73747364`（'stsd' 的 ASCII） |
| 開得起來但 `SetCurrentMediaType(NV12)` 回 `0xC00D5212` | `trun` 的 `data_offset` 回填位置少算 4 bytes（漏掉 ver/flags），把 `sample_count` 覆寫成 moof 大小 |
| 5 秒錄影播出來 7 秒，越長越誇張 | 分片累加用了「時間軸結束位置」而不是「分片長度」 |

四個都修完之後 MF 才願意解。**教訓**：MP4 的解析器對「大小差 4 bytes」完全不
報錯，只會回一個看起來像「格式不支援」的錯誤碼。遇到 `0xC00D36C4` 時，先把
box 樹印出來對大小，比讀文件快。

---

## 5. 長錄影穩定性

### 5.1 一個把整個擷取永久殺死的 bug

症狀：錄到 3~8 秒時畫面停止更新，`captured` 凍結（177 / 445），行程還活著、
CPU 接近 0，`dropped=0`、沒有任何錯誤訊息。重開一段又正常。

原因：幀率閘門寫在 `TryGetNextFrame()` **之前**，被節流丟掉的那一幀留在 pool 裡
沒被取走。累積到 `maxBufferSize`（3）之後，WGC 不再產生新幀，也**不再回呼
FrameArrived**。`throttled` 從 2 跳到 3 的那一刻正好是死亡瞬間。

修法：先 `TryGetNextFrame()` 再判斷要不要這幀。順序反了不是效能問題，是會讓
整個擷取永久停住。

### 5.2 幀率閘門的三個數值陷阱

- **不能用嚴格下界**：來源剛好 60.0 fps 時每幀間隔 16.67 ms，一點 jitter 就落到
  16660 µs，嚴格比較「< 16666」會判成太快而丟掉，60 fps 被砍成 48 fps。
- **`MinUpdateInterval` 不能用 ceil**：`ceil(1000/60)=17 ms` 比真實週期 16.67 ms
  還長，剛好 60 fps 的來源會被擋掉一半。用 `floor(1000/fps - 1)`。
- **「間隔下限」會把目標幀率量化成刷新率的因數**（2026-10-18 補）。這是加入
  144 / 165 / 240 三個選項時才暴露出來的：

  | 螢幕 | 目標 | 硬下界（6.94 ms） | 結果 |
  |---|---|---|---|
  | 239 Hz | 144 | 每幀 4.18 ms < 6.94 ms | 每兩幀收一幀 → **119 fps** |
  | 165 Hz | 144 | 每幀 6.06 ms < 6.94 ms | 每兩幀收一幀 → **82 fps** |

  使用者只會覺得「選了 144 卻卡」，看不出是閘門自己在丟。改成「信用」制
  （`FrameGate`，見 `capture_wgc.h`）：每過 dt 就存下 dt，真的出幀時才扣掉
  一個目標週期。信用會累積，所以 239 Hz 底下收幀節奏是「收收放」，長期平均
  剛好 144。容差改成 5%（不是 10%），因為信用本身會自我修正。

  `--gatestest` 把「來源 × 目標」48 組全部跑一遍（純數學，不開擷取），
  含 239→144 = 144.0、165→144 = 144.0，全部 PASS。

### 5.3 測試源本身也要校準

`anim_window` 原本用 `Sleep(16)` 節奏，實際延遲是 15.6 ms 的倍數，一輪 ~25 ms，
只有 39 fps。來源跑不到 60，就沒辦法拿來驗「60fps 不掉幀」。改成
「算下一個截止時刻、只睡到那裡」＋ `timeBeginPeriod(1)` 之後是精準的 60.0 fps。

### 5.4 1080p60 十分鐘實測

```
CapCli.exe --hwnd <hwnd> --codec hevc --quality 70 --seconds 600 --fps 60 --gop 2
```

| 項目 | 數值 |
|---|---|
| 擷取幀數 | 35 935（59.9 fps） |
| busy 丟幀 | **0** |
| 閘門節流 | 50（內容重複，非損失） |
| CPU | 0.595 ms/幀 |
| 輸出 | 963.63 MB / 13.472 Mbps / 35 930 samples |
| 解碼驗證 | 1920×1080，599.985 秒，decodes OK |

擷取 35 935 與寫入 35 930 差 5 幀（0.014%），是收尾 flush 時還在管線裡的那幾幀。

過程中有一段約 15 秒碼率掉到 0.01 MB/s（測試源暫時靜止），
順便印證了「靜止近乎 0 碼率」—— 同一段時間 `captured` 仍是 60 fps，
代表擷取沒有停，只是內容不變。

`--borderless` 是必要的：帶標題列的 1920×1080 視窗會被螢幕裁掉，客戶區只剩
1606×913，量到的就不是 1080p。

---

## 6. 量測指令

```bat
:: 公平比較（同一批幀、三路同時輸出）
CapCli.exe --hwnd <hwnd> --codec hevc --quality 60 --ab h264 --q2 60 ^
           --ref ref.mp4 --out hevc.mp4 --out2 h264.mp4 --seconds 6

:: PSNR
mp4_check.exe ref.mp4 hevc.mp4
mp4_check.exe ref.mp4 h264.mp4

:: crash-safe 驗證
CapCli.exe --hwnd <hwnd> --seconds 120 --out killed.mp4
:: （12 秒後從工作管理員強制結束）
mp4_check.exe killed.mp4      :: 應該要 decodes OK

:: 對照組
CapCli.exe --hwnd <hwnd> --seconds 120 --progressive --out dead.mp4
:: （同樣強制結束）
mp4_check.exe dead.mp4        :: 預期 0xC00D36C4
```

---

## 7. 還沒做的

- **B 幀**：`BPictureCount` 被 MF 包裝層回 `REJECTED`（硬體其實支援）。
  省 15~25% 的空間，但要處理 ctts 與分片內的重排序。muxer 的 rank 法已經寫好，
  缺的是讓 MFT 真的吃下這個設定。
- **AV1**：本機是 Ampere，沒有 AV1 硬編，MF 也沒註冊 AV1 encoder MFT。
  要等 Ada 機器或走 NVENC 直通（M3）。
  → **2026-10-17 補：4070 Ti 上已實作並驗證完，見 `docs/06-av1-status.md`。**
- **音訊**：目前只有視訊軌。
- **`hvcC` 的 `general_level_idc` 是 0**：編碼器自己寫的，不是我們的 bug。
  MF 不在意，但嚴格的解析器可能挑。要的話可以在組 hvcC 時從解析度回推一個合法值。
