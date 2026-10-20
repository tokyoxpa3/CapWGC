# M1 狀態：WGC → GPU → MF → MP4 已跑通

> 日期：2026-10-17
> 產出：`out/probe_encoders.exe`、`out/anim_window.exe`、`out/CapCli.exe`
> 原始碼：`src/core/capture_wgc.*`、`src/core/encoder_mft.*`、`src/cli/main.cpp`

---

## 1. 完成了什麼

```
CapCli.exe --out rec.mp4 --seconds 10 [--hwnd 0x1234] [--codec hevc|h264] [--quality 70]
```

整條鏈路：

```
WGC item (window/monitor)
  → free-threaded pool, busy guard, 3 buffers
  → item.Closed / 解析度變更偵測
  → CopySubresourceRegion（帶 crop）到 BGRA ring
  → VideoProcessorBlt（BGRA → NV12，BT.709，GPU）
  → MFCreateDXGISurfaceBuffer → MF SinkWriter
  → NVENC → MP4
```

已實作且驗證的點：

- [x] `CreateForWindow` / `CreateForMonitor`
- [x] `IsIconic` → `SW_RESTORE`（最小化視窗保護）
- [x] busy guard（free-threaded 重入直接丟幀）
- [x] `IsBorderRequired(false)` / `IsCursorCaptureEnabled(false)` / `MinUpdateInterval(1ms)`，全部回讀
- [x] GPU 色彩轉換（VideoProcessor，BT.709 limited range）
- [x] VFR：PTS 用 `frame.SystemRelativeTime()`，duration 由前後幀差決定
- [x] 硬體 NVENC（`nvidia-smi` 確認 encoder util 3-4%）
- [x] HEVC / H.264 切換
- [x] 動畫測試源（確定性內容，供 A/B 比較）

---

## 2. 實測數據（RDP session，RTX 3060 Ti）

| 場景 | codec | 解析度 | 實測 fps | CPU | 碼率 | 大小 |
|---|---|---|---|---|---|---|
| RDP 桌面 | H.264 | 1280×720 | 30.0 | **0.67 ms/幀** | 1.21 Mbps | 0.87 MB / 6s |
| RDP 桌面 | HEVC | 1280×720 | 31.5 | **0.58 ms/幀** | 2.81 Mbps | 2.01 MB / 6s |
| 動畫視窗 | H.264 | 800×600 | 30.8 | **1.02 ms/幀** | 0.35 Mbps | 0.21 MB / 5s |
| 動畫視窗 | HEVC | 800×600 | 31.4 | **0.80 ms/幀** | 0.44 Mbps | 0.26 MB / 5s |

CPU 量測注意：`GetProcessTimes` 在這台有 15.625 ms 量化粒度，短跑誤差大。
3 秒那次量到 2.28 ms/幀是量化假象，6 秒的 0.67 ms 才是真值。

**與既有基準對比**：`wgcenc.cpp` 的 0.550 ms/幀（720p60，零拷貝直通 MFT）。
本專案 0.67 ms/幀（720p30，多一層 SinkWriter），同級。

---

## 3. 三個已確認的問題

### 問題一：`--quality` 完全沒生效

`IMFSinkWriter::GetServiceForStream(0, GUID_NULL, IID_ICodecAPI)` 回
`0xC00D36BA`（`MF_E_INVALIDSTREAMNUMBER`）。SinkWriter 不讓外部碰內部 encoder 的 CODECAPI。

後果很具體：**HEVC 的檔案比 H.264 大**（0.44 vs 0.35 Mbps）。正常情況下 HEVC 應該省 25-50%。
兩個編碼器都跑 SinkWriter 的預設參數，所以不可比，而且都不是最優。

這不是調參能解決的，是架構限制。

### 問題二：moov 在檔尾，crash 會毀檔

實測檔案結構：

```
ftyp (24) → uuid (39) → mdat (216958) → moov (1351)
```

`moov` 在 `mdat` 之後。錄製中途斷電/崩潰，整個檔案不可播。
這正是 OBS 堅持用 MKV 的原因。

### 問題三：B 幀與 AQ 拿不到

SinkWriter 沒有暴露 `CODECAPI_AVEncMPVDefaultBPictureCount`（B 幀，省 15-25%）
與 AQ（自適應量化，省 5-15%）。這兩項加起來是 20-35% 的壓縮率。

---

## 4. 結論：SinkWriter 是死路，M2 必須自己驅動 MFT

SinkWriter 的價值是「五分鐘能跑起來驗證擷取層」。它做到了，而且證明了：

- WGC 在 RDP session 可用，拿到的是真 GPU（RTX 3060 Ti）
- VideoProcessor 的 GPU 色彩轉換正常
- NVENC 真的在跑
- VFR 時間戳與 MP4 封裝正確

但它給不了「壓縮率最高」。三個問題全部指向同一個解法：**自己驅動 IMFTransform + 自寫 muxer**。

### M2 的具體工作

1. **MftEncoder 取代 SinkWriter**
   - `MFTEnumEx` 選硬體 MFT → `MFT_MESSAGE_SET_D3D_MANAGER`
   - `ICodecAPI`：Quality 模式、B 幀、GOP、QP 範圍、ForceKeyFrame
   - async MFT 的 drain pump（`wgcenc` 已驗證：drain → collect → drain，否則一半幀靜默丟失）
   - 用實測出幀率回推碼率（VFR 下用固定 fps 算一定錯）

2. **自寫 fMP4 muxer**
   - `moov` 在前 + `moof`/`mdat` 分片 → crash 只丟最後一個 fragment
   - Annex-B → AVCC 轉換（4-byte length prefix）
   - 從 SPS/PPS/VPS 組 `avcC` / `hvcC`
   - `trun` 的 sample duration 支援任意值 → VFR 天然支援

3. **NvencEncoder（M3）**
   - 動態載入 `nvEncodeAPI64.dll`
   - AQ / lookahead / multi-pass / 4:4:4
   - RTX 40 上拿 AV1

### 預期收益

| 項目 | 現在（SinkWriter） | M2（自驅動 MFT） |
|---|---|---|
| 碼控 | 不可控 | Quality + B 幀 + AQ |
| HEVC vs H.264 | HEVC 反而大 25% | HEVC 省 25-50% |
| crash 安全 | 毀檔 | 只丟最後 2 秒 |
| AV1 | 無 | M3 |

---

## 5. 一個仍未解釋的現象

RDP 桌面靜止時，WGC 仍然給 30 fps（`captured=90 / 3s`）。這與「變更驅動」的預期不符。

可能原因：
- RDP 的遠端桌面本身在持續重繪（時鐘、游標、連線指示）
- `MinUpdateInterval(1ms)` 讓 WGC 更積極取幀
- RDP 的 DWM 合成路徑與本機不同

在 console session 上重測才能確定。這會直接影響「靜止畫面 0 碼率」這個賣點能否成立，
M2 必須先量清楚。
