# 編碼器選型：MF MFT vs NVENC 直通，以及 RTX 40 的 AV1

> 日期：2026-10-17
> 觸發：使用者有兩台機器——本機 RTX 3060 Ti、另一台 RTX 4070 Ti，問 RTX 40 有沒有更好的技術。

---

## 1. 本機實測（RTX 3060 Ti / Ampere / 第 7 代 NVENC）

用 `out/probe_encoders.exe` 掃出來的結果：

```
D3D11 hardware device: OK (feature level 0xB000)

encoder                                        d3d11  hw     outputs
NVIDIA H.264 Encoder MFT                       yes    yes    H.264
NVIDIA HEVC Encoder MFT                        yes    yes    HEVC
H264 Encoder MFT                               no     no     H.264      (MS 軟編)
HEVCVideoExtensionEncoder                      no     no     HEVC       (商店擴展)
VP9VideoExtensionEncoder                       no     no     VP9        (軟編)
Microsoft AVC DX12 Encoder                     yes    yes    (none)     (DX12 專用，不吃 D3D11)
Microsoft HEVC DX12 Encoder                    yes    yes    (none)     (同上)

verdict: AV1 = no, HEVC = yes (d3d11-aware), H.264 = yes (d3d11-aware)
```

**結論：這台沒有 AV1 硬編，預設用 HEVC。** 兩個 NVIDIA MFT 都是 d3d11-aware，
可以直接吃 D3D11 紋理，零拷貝路徑成立。

NVIDIA H.264 MFT 的 CODECAPI 實測：

```
RateControlMode    yes  cur=0 (CBR)       ← 可改成 3 (Quality)
Quality            yes  cur=65             ← 0..100，錄屏建議 70-80
MeanBitRate        yes  cur=7331282
MaxBitRate         yes  cur=21993846
GOPSize            yes  cur=60             ← 改成按時間控制
EncodeQP           yes  cur=26
MinQP/MaxQP        yes  0..51
BPictureCount      err 0x80004001          ← 這個 MFT 不暴露 B 幀
ForceKeyFrame      yes
```

兩個值得注意的點：
- **`BPictureCount` 回 E_NOTIMPL**。NVIDIA 的 MF 包裝層沒暴露 B 幀，但 NVENC 本身支援。
  這是 MF 的資訊損失，不是硬體限制。
- **`RateControlMode` 預設是 CBR**。不改就是 Captura 那種「靜態畫面照燒碼率」的行為。

---

## 2. RTX 4070 Ti（Ada / 第 8 代 NVENC）多了什麼

| 能力 | Ampere (3060 Ti) | Ada (4070 Ti) |
|---|---|---|
| H.264 硬編 | 有 | 有 |
| HEVC 硬編 | 有 | 有 |
| **AV1 硬編** | **無** | **有** |
| HEVC 4:4:4 | 有（NVENC 層） | 有 |
| 單路吞吐 | 8K30 | 8K60 |

### AV1 對「錄屏」的意義比對一般影片更大

一般影片的 AV1 vs HEVC 差距是 20–30%。但桌面錄製的內容特性完全不同：
大面積同色（視窗背景、工具列）、重複紋理（文字筆畫、圖標）、少量高頻邊緣。

AV1 規範裡有兩個專門為此設計的工具：

- **Palette mode**：區塊內顏色數少時，用調色板索引代替逐像素值。桌面 UI 正好是「少量顏色」。
- **Intra block copy (IBC)**：在幀內複製區塊，對重複的文字/圖標極有效。

HEVC 也有對應的 SCC（Screen Content Coding）擴展，但**消費級硬體編碼器一律不支援**。
AV1 把這些工具放進了基礎規範，硬體實作的成本低得多。

⚠️ **但不要過度承諾**：NVENC 的 AV1 實際啟用哪些工具，NVIDIA 沒有公開清單。
保守估計「同質量省 20–30%」，樂觀情況（palette/IBC 真的開了）在純 UI 內容上可能更多。
這需要實測——用同一段桌面錄製，AV1 和 HEVC 各跑一次，比檔案大小。
**實測結果比這個保守估計還要保守得多，見 §6。**

### 還有一個更實際的優勢：4070 Ti 的 NVENC 有餘裕

錄屏時 GPU 同時在跑遊戲或渲染。3060 Ti 的 NVENC 在 4K60 HEVC 下會吃掉可觀的編碼單元時間；
4070 Ti 的雙 NVENC 餘裕大得多，高負載下不容易掉幀。這是「錄得穩」而非「錄得小」。

---

## 3. MF MFT 的天花板

MF 是跨廠商的統一介面，代價是**只暴露交集**。以下能力 MF 拿不到：

| 能力 | 對壓縮率的影響 | MF | NVENC SDK |
|---|---|---|---|
| Adaptive Quantization (AQ) | 5–15%（平坦區域省碼率，細節區保質量） | ✗ | ✓ |
| Lookahead | 5–10%（提前幾幀決定碼率分配） | ✗ | ✓（H.264/HEVC） |
| Multi-pass | 3–8% | ✗ | ✓ |
| B 幀控制 | 15–25% | 部分（本機 MFT 直接不給） | ✓ |
| 4:4:4 色彩 | 桌面彩色文字明顯更銳利 | ✗（只吃 NV12） | ✓ |
| AV1 | 20–30% | 看驅動是否註冊 | ✓ |

**這就是為什麼 OBS 的 nvenc 不走 MF，而是直接載入 `nvEncodeAPI64.dll`。**

### 授權上仍然乾淨

`nvEncodeAPI64.dll` 是 NVIDIA 驅動自帶的系統元件，**不需要分發**。
MIT 專案只要用 `LoadLibrary` + `GetProcAddress` 動態載入，執行時偵測不到就退回 MF，
原始碼裡沒有任何 NVIDIA 的二進位或標頭（`nvEncodeAPI.h` 是 MIT 相容的宣告式標頭，
但更保守的做法是自己宣告用到的函式指標型別）。

這與「靜態連結 FFmpeg」是兩種完全不同的性質。

---

## 4. 決策：三層 encoder backend

```
IEncoder (抽象)
  ├─ NvencEncoder    動態載入 nvEncodeAPI64.dll；NVIDIA 機器首選，最高壓縮
  ├─ MftEncoder      Media Foundation；跨廠商 fallback（Intel QSV / AMD AMF / 軟編）
  └─ (未來) AmfEncoder / QsvEncoder  只有在 MF 明顯不夠時才做
```

執行期選擇順序：

1. 探測 NVENC（`LoadLibrary("nvEncodeAPI64.dll")` + `NvEncodeAPIGetMaxSupportedVersion`）
2. 探測 MF 的硬體 MFT
3. 都沒有 → MF 軟體 MFT（`CLSID_CMSH264EncoderMFT`），並在 UI 明示「軟體編碼，CPU 佔用高」

### 編碼格式的預設策略

| 機器 | 預設 | 理由 |
|---|---|---|
| 有 AV1 硬編（RTX 40 / Arc） | **AV1** | 壓縮率最高 |
| 有 HEVC 硬編 | **HEVC** | 兼容性與壓縮率平衡點 |
| 只有 H.264 | H.264 | 相容性優先 |

**不要預設 AV1**：Windows 需要 AV1 Video Extension 才能播放，舊版 Premiere / 剪映 / 部分手機不支援。
做成「最高壓縮」選項，並在 UI 上標註兼容性。

### 三檔預設（給一般使用者）

| 檔位 | 編碼 | 碼控 | 典型 1080p60 大小 |
|---|---|---|---|
| 極致壓縮 | AV1 / HEVC | Quality 75 + AQ + lookahead | 3–8 MB/分 |
| 平衡（預設） | HEVC / H.264 | Quality 65 | 8–15 MB/分 |
| 相容優先 | H.264 High | Quality 60 | 12–25 MB/分 |

（以上是桌面內容量級，遊戲畫面會高 3–5 倍。）

---

## 5. 下一步

1. **M1 先用 MftEncoder 跑通整條鏈路**（WGC → ring → NV12 → MFT → MP4）。
   理由：MF 是跨廠商保底路徑，且能驗證擷取/封裝層是否正確。換 backend 只換一個類。
2. **M2 加 NvencEncoder**，在同一條管線上 A/B 比檔案大小與 CPU。
3. 在 4070 Ti 上跑 `probe_encoders.exe`，確認 MF 是否註冊了 AV1 encoder MFT。
   如果沒有，NvencEncoder 就是唯一能拿到 AV1 的路徑——這會把 M2 的優先級提前。

---

## 6. 後記（2026-10-17，4070 Ti 實測完）

上面第 5 節的第 3 點有答案了：**MF 在這台 4070 Ti 上確實註冊了 AV1 encoder MFT**
（`NVIDIA AV1 Encoder MFT`，d3d11-aware），不需要 NVENC 直通就能拿到 AV1。

實測結果推翻了本文一個過度樂觀的假設：AV1 對 HEVC 的優勢**不是** 20~30%，
甚至連「高熵 0~18%」都高估了。

2026-10-17 在 3060 Ti 上補完同畫質比較（4070 Ti 那台沒有 HEVC 解碼器，只能編不能量）：
**高熵內容 AV1 是 HEVC 的 91~100%（中位 98%）；UI 內容反過來，101~121%（中位約 107%）。**

§2 猜的「palette / IBC 在 UI 內容上更有效」在 NVENC 上不只沒有兌現，方向還是反的。
原本「UI 省 17~47%」是拿同一個 quality 旋鈕比的，那時 HEVC 的畫質高了 2.5~2.8 dB；
補成八個 quality 點、換成同畫質之後，AV1 在 UI 上要多花 1~21%。

順帶一個方法論教訓：一開始只用 q60/q75 兩個點內插，算出「UI 是 92%、兩條曲線會
交叉」，補到八個點之後交叉完全消失 —— rate-PSNR 曲線是凹的，長弦會把中段算貴。
同一個坑在高熵組又踩了一次（那次方向相反，長弦讓 AV1 看起來太好，差 2 個百分點）。

結論：**AV1 在錄屏場景的賣點不是壓縮率**（對 HEVC 高熵只省 0~9%、UI 還更差），
而是它免授權、且 40 系列以後的硬體都有。細節與數據見 `docs/06-av1-status.md`
§3.4.1 與 §3.4.3。
