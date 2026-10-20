# 編碼器自動選擇（AV1 優先）

> 日期：2026-10-17
> 模組：`src/core/codec_select.{h,cpp}`
> 接入：`CapCli --codec auto`（預設）

---

## 1. 怎麼判斷

不猜 GPU 型號，直接問 Media Foundation 有哪些 encoder MFT 能接受目標 subtype：

```
MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL)
  → 逐個 ActivateObject
  → 讀 MF_SA_D3D11_AWARE（從 MFT attributes，不是 IMFActivate）
  → 對 AV1 / HEVC / H.264 各試一次 SetOutputType(TEST_ONLY)
  → 排序：AV1 > HEVC > H.264，同 codec 內硬體優先
  → 去重：每個 codec 只留最好的一個
```

GPU 型號只用來「顯示」與「提示」，不用來決定能力。這樣即使 MF 的行為隨驅動版本改變，
程式也不需要更新。

### 為什麼要有降級鏈

`ProbeCodecs` 說「這個 MFT 接受 AV1」，不等於 SinkWriter 真的能建出這條管線。
實測就遇到：AV1 的 `AddStream` 過了，但 `SetInputMediaType` 回 `0xC00D5212`（`MF_E_TOPO_CODEC_NOT_FOUND`）。

所以 `Open()` 是按候選順序逐個試，第一個成功為止。指定 `--codec av1` 而機器不支援時，
不會整個錄製失敗，而是自動降到下一個可用格式並在 stderr 說明原因。

---

## 2. 輸出長什麼樣

本機（RTX 3060 Ti）：

```
GPU     : NVIDIA GeForce RTX 3060 Ti [Ampere (RTX 30)] 8024 MB
session : RDP (remote)
encoders:
  HEVC   hardware  NVIDIA HEVC Encoder MFT
  H.264  hardware  NVIDIA H.264 Encoder MFT
prefer  : HEVC (auto)
capture caps: border=off cursor=off minInterval=1.00ms
capturing item 1280x720 -> 1280x720 @(0,0)
using   : HEVC
encoder : NVIDIA HEVC Encoder MFT
```

4070 Ti 上預期會是：

```
GPU     : NVIDIA GeForce RTX 4070 Ti [Ada (RTX 40)] 12288 MB
encoders:
  AV1    hardware  NVIDIA AV1 Encoder MFT      ← 若驅動有註冊
  HEVC   hardware  NVIDIA HEVC Encoder MFT
  H.264  hardware  NVIDIA H.264 Encoder MFT
prefer  : AV1 (auto)
using   : AV1
```

**如果 4070 Ti 上沒有出現 AV1 那一行**，程式會印：

```
note    : Ada (RTX 40) 的硬體支援 AV1，但 Media Foundation 沒有暴露 AV1 encoder MFT
          → 要拿到 AV1 需要 NVENC 直通（M3），目前先降一級
```

這正是需要實測的地方 —— 把 `out/probe_encoders.exe` 或 `CapCli.exe` 複製到那台跑一次就知道。

---

## 3. 驗證結果

| 測試 | 結果 |
|---|---|
| `--codec auto`（本機） | 探測到 HEVC + H.264 硬體，選 HEVC |
| `--codec av1`（本機） | AV1 初始化失敗 → 自動降級 HEVC，stderr 有原因 |
| `--codec h264` | 正常 |
| GPU 世代推斷 | Ampere (RTX 30) 正確 |

---

## 4. 一個必須正視的結果：HEVC 比 H.264 大 40%

同樣的動畫內容、同樣 5 秒：

| codec | 大小 | 碼率 |
|---|---|---|
| H.264 | 0.20 MB | 0.33 Mbps |
| HEVC | 0.28 MB | 0.47 Mbps |

這不正常。HEVC 在同等質量下應該省 25–50%。

根因還是同一個：**`--quality` 沒有傳進 encoder**。

- `GetServiceForStream(0, GUID_NULL, IID_ICodecAPI)` → `MF_E_INVALIDSTREAMNUMBER`
- `MF_SINK_WRITER_ENCODER_CONFIG` 塞 CODECAPI 的 GUID → 被忽略，無效

兩條路都不通，所以兩個 codec 都跑各自的預設值。而 SinkWriter 對 HEVC 的預設
（可能是較高質量的 profile / QP）恰好比 H.264 更耗碼率。

**這不是 HEVC 的問題，是我們給不了它正確參數的問題。**

換句話說：自動選了「理論上更省」的格式，實際得到更大的檔案。這個矛盾不解決，
「壓縮率最高」就是空話。

---

## 5. 解法只有一條：M2 自驅動 MFT

SinkWriter 是封閉的，拿不到 `ICodecAPI` 就永遠只能用它給的預設。要真正控制：

1. 自己 `MFTEnumEx` → `ActivateObject` → `MFT_MESSAGE_SET_D3D_MANAGER`
2. 自己 `SetOutputType` / `SetInputType`
3. 自己拿 `ICodecAPI` 設 Quality / B 幀 / GOP / QP 範圍
4. 自己 `ProcessInput` / `ProcessOutput`
5. 自己寫 muxer（SinkWriter 只負責 mux，但沒辦法只借它的 mux）

這也順便解決另外兩個問題：moov 在檔尾（crash 毀檔）、拿不到 B 幀與 AQ。

M2 完成後，同一段內容的預期結果：

| codec | 現在（SinkWriter 預設） | M2（Quality 70 + B 幀） |
|---|---|---|
| H.264 | 0.20 MB | ~0.16 MB |
| HEVC | 0.28 MB | ~0.11 MB |
| AV1 | 不可用 | ~0.09 MB |

（量級估算，實際要在 M2 完成後量。）
