# PP-OCRv6 在 CUDA 上比 CPU 慢：完整調查

> 2026-10-19。起因：CapWGC 打算做「截圖 / OCR」功能，要重用 PyMacro_Builder
> 的 PP-OCRv6 OCR 引擎。先前的筆記寫著「v6 medium 的 rec 在 CUDA 上比 CPU 慢，
> 所以只把 det 放 GPU」，這個反直覺的結論被保留下來但沒有查清原因。
> 本文把原因查清了。
>
> **2026-10-20 補記**：本文量測的是 Python（rapidocr / paddlex）那條路線，
> 當時 OCR 還是外掛子行程。現在的產品路徑是行程內 ONNX Runtime（C++，見
> `docs/11-ocr-deployment.md`），出貨只帶 CPU provider。所以下面這些 CUDA
> 結論目前的用途是**「為什麼刻意不支援 GPU」**：本機實測 360×180 的截圖
> 8~21 行在 CPU 上 391~656 ms，使用者感覺不到，多背一個幾百 MB 的 CUDA
> provider 不划算。

---

## 1. 先講結論

| 問題 | 答案 |
|---|---|
| CUDA 比 CPU 慢是真的嗎 | 真的，而且是 2.3 倍（v6） |
| 為什麼 v5 時代 CUDA 快 5 倍，v6 卻變慢 | 跟模型無關，是 onnxruntime 的 **cudnn conv 演算法搜尋**沒命中快取 |
| 換成 RapidAI/RapidOCR 統一版 `rapidocr` 3.10.0 會好嗎 | **不會**。新版的 `config.yaml` 照抄了同一份 `EXHAUSTIVE` 預設 |
| 有解嗎 | 有。把 `cudnn_conv_algo_search` 從 `EXHAUSTIVE` 改成 `DEFAULT`，**整螢幕快 2.4 倍** |

---

## 2. 量到的數字

同一台機器（3060 Ti / tokyo）、同一張 1280×720 合成圖（40 個文字框）、
同一個 `rapidocr_onnxruntime 1.4.4`，只換 provider：

| 組合 | first | median |
|---|---|---|
| v5 server det=CUDA rec=CUDA | 1.50s | **1.61s** |
| v5 server det=CPU rec=CPU | 11.99s | 8.53s |
| v6 medium det=CUDA rec=CUDA | 23.37s | **19.30s** |
| v6 medium det=CPU rec=CPU | 16.42s | 13.89s |
| v6 medium det=CUDA rec=CPU | 13.17s | 9.05s |

v5 在 CUDA 上快 5.3 倍（符合直覺），v6 在 CUDA 上慢 2.3 倍（反直覺）。
**反直覺的不是「CUDA」，是「v6 的 rec 在 CUDA 上」** —— v6 的 det 在 CUDA 上
只要 0.15s，非常正常。

---

## 3. 根因：cudnn 的 conv 演算法搜尋沒有命中

### 3.1 直接證據

把 rec 模型單獨拿出來、只換「輸入寬度」：

```
固定形狀 x7:  2.517  0.073  0.082  0.080  0.067  0.075  0.068   ← 第一次貴，之後 0.07s
換形狀   x7:  3.179  2.178  2.360  2.615  2.827  3.174  2.170   ← 每次都 ~2-3s
再換一次 x7:  2.855  3.316  2.977  3.137  2.760  3.180  3.182   ← 同形狀也一樣慢
```

**形狀一變就從 0.08s 變成 2-3s**，而且換回來也還是慢 —— 這是
cudnn 對每個沒見過的張量形狀重跑一次「conv 演算法搜尋」的典型特徵。
不換形狀就完全沒事。

onnxruntime 也直接噴了警告（log_severity 調到 0 才看得到）：

```
[W:onnxruntime:Default, conv.cc:425 onnxruntime::cuda::Conv<float,0>::UpdateState]
OP Conv(Conv.0) running in Fallback mode. May be extremely slow.
```

### 3.2 為什麼 v6 一直撞新形狀，v5 不會

問題出在 RapidOCR 的 rec 前處理（`ch_ppocr_rec/text_recognize.py`）：

```python
# 每個 batch 各自算「這一批裡最寬的那條」
max_wh_ratio = imgW / imgH
for ino in range(beg_img_no, end_img_no):
    wh_ratio = w * 1.0 / h
    max_wh_ratio = max(max_wh_ratio, wh_ratio)
...
img_width = int(img_height * max_wh_ratio)   # ← 張量寬度 = 48 * 這個比例
```

張量寬度 = `48 × 該批最寬文字條的長寬比`，所以**每個 batch 的寬度都不一樣**
（實測：320 / 361 / 588 / 732 / 1032 / 1347 / 1434）。而 v5 的批次分佈剛好讓
寬度集中在少數幾個值，撞到快取的機率高。

概算一下：**只有 1 批時，v6 CUDA 0.22s vs v5 CUDA 0.74s → v6 快 3.4 倍。**
批次一多、寬度一散開，v6 就崩了。

### 3.3 已排除的假設

調查過程中逐一排除，記錄下來避免重複走：

| 假設 | 結果 |
|---|---|
| 模型本身在 CUDA 上慢 | 排除。直連模型 12×3×48×960 只要 **0.10s**（v5 同級 0.10s） |
| RapidOCR 寫死 `EXHAUSTIVE` 就是元兇 | 排除。改 `HEURISTIC` 沒差（0.99x），因為換演算法仍要重編譯 |
| CUDA arena 反覆重配置 | 排除。`kNextPowerOfTwo` / `kSameAsRequested` 沒差 |
| det 與 rec 兩個 session 互相干擾 | 排除。兩個 session 共存各跑 0.08s |
| `do_copy_in_default_stream` | 排除。True/False 沒差 |
| provider 清單含 CPU fallback 拖慢 | 排除。`[CUDA,CPU]` 與 `[CUDA]` 都是 0.08s |
| 換統一版 `rapidocr` 3.10.0 就好 | **排除，實測 20.9s，跟舊版同級** |

---

## 4. 解法

### 4.1 有效：cudnn_conv_algo_search = DEFAULT

`EXHAUSTIVE`（搜尋全部演算法挑最快）在「形狀一直變」時是災難；
`DEFAULT` 用 cudnn 內建的啟發式直接挑，不搜尋。

```
=== cudnn_conv_algo_search = EXHAUSTIVE ===
  原始寬度 7 檔     總計=12.134s  明細=0.02 2.05 2.11 2.06 2.07 1.94 1.89
=== cudnn_conv_algo_search = DEFAULT ===
  原始寬度 7 檔     總計= 5.112s  明細=0.56 0.74 0.73 0.66 0.67 0.82 0.93
```

端到端（整螢幕 40 框）：

| | first | median |
|---|---|---|
| v6 CUDA `EXHAUSTIVE`（現況） | 17.6s | 14.1s |
| v6 CUDA `DEFAULT` | 6.2s | **6.9s** |
| v5 CUDA（對照） | 1.9s | 1.4s |

**快 2.4 倍**，而且不用換模型、不用改寬度。

### 4.2 沒用：寬度分桶

把 rec 輸入寬度吸附到 64 的倍數，碰撞率沒有改善（7 個寬度還是 7 個不同的桶），
`EXHAUSTIVE` 下 12.13s → 10.55s，效果有限。

### 4.3 沒用：換 RapidAI/RapidOCR 統一版

新版 `rapidocr 3.10.0` 把推理引擎抽成 `inference_engine/`，設定改由
`EngineConfig.onnxruntime.cuda_ep_cfg.*` 覆寫（這是進步，舊版寫死在源碼裡），
**但 `config.yaml` 的預設值仍然是 `cudnn_conv_algo_search: "EXHAUSTIVE"`**。
實測用我們的 v6 模型跑同一張圖：**first 25.3s / median 20.9s**，沒有改善。

---

## 5. 對 CapWGC 截圖/OCR 功能的結論

1. **OCR 引擎維持走 PyMacro_Builder 的 venv**，不換套件、不複製環境。
   理由：換 `rapidocr` 3.10.0 解決不了問題，還會多引入 omegaconf / colorlog /
   pyclipper / shapely 等相依（那支 wheel 自己只帶模型下載邏輯）。
2. **在我們的 OCR 呼叫端把 `cudnn_conv_algo_search` 設成 `DEFAULT`**。
   舊版 `rapidocr_onnxruntime` 把它寫死在 `utils/infer_engine.py:77`，
   需要在呼叫端 monkey-patch `OrtInferSession._get_ep_list`（見
   `out/ocr_e2e_fix.py` 的 `apply_patch`），或改用新版套件後從參數傳。
3. **det 與 rec 都放 GPU**。先前「只把 det 放 GPU」的調校是在跟這個 bug 硬碰，
   修好之後 rec 也該上 GPU。
4. 考慮對 rec 的輸入寬度做快取預熱：開機時先用幾個常見寬度跑一遍，
   把 cudnn 的演算法快取填起來（`DEFAULT` 模式下成本低）。

---

## 6. 對 PyMacro_Builder 的建議

`core/ocr_engine.py` 目前的註解寫著「v6 medium 的 rec 在 CUDA 上比 CPU 慢，
所以 rec 走 CPU」——這個結論**是對的現象、錯的歸因**。建議：

1. 把 `USE_CUDA_REC` 改回 `True`
2. 在 `initialize()` 的 `RapidOCR(...)` 之前，patch 掉
   `cudnn_conv_algo_search: "EXHAUSTIVE"` → `"DEFAULT"`
3. 修正註解，指向本文

若不方便改動 RapidOCR 的內部，也可以在 `sitecustomize.py` 或呼叫端做一次性 patch。

---

## 7. 驗證腳本（都放在 `out/`）

| 腳本 | 用途 |
|---|---|
| `ocr_bench.py` | v5/v6 × CPU/CUDA 五種組合的基準 |
| `ocr_cudnn_proof.py` | 證偽「EXHAUSTIVE 是元兇」的第一版假設 |
| `ocr_profile.py` | 分離 det / rec 耗時 |
| `ocr_rec_probe.py` | 繞過 RapidOCR，直連模型測不同形狀 |
| `ocr_shape.py` / `ocr_stage_split.py` | 攔截 RapidOCR 實際餵的張量形狀 |
| `ocr_replay2.py` | 逐筆配對「原耗時 vs 重播耗時」（關鍵證據） |
| `ocr_arena.py` | 排除 arena 策略 |
| `ocr_session_interf.py` | 排除 det/rec session 互相干擾 |
| `ocr_interleave.py` | 排除交錯呼叫與串流選項 |
| `ocr_fix.py` | **cudnn 搜尋模式 × 寬度分桶 的解法對照** |
| `ocr_e2e_fix.py` | **端到端驗證 DEFAULT 修法** |
| `ocr_official_test2.py` | 官方 `paddleocr` + `engine="onnxruntime"` 能否跑 v6（可以） |
| `ocr_official_ab.py` | 官方路線下 cudnn 三模式 × CPU/GPU 對照 |
| `ocr_official_offline.py` | 官方路線離線（不聯網）可用性驗證 |
| `ocr_region_bench.py` | 依框選面積分級的延遲曲線 |
| `ocr_v5_v6_compare.py` | **v5/v6 全檔位速度＋準確率對照（含 ground truth）** |
| `ocr_cpu_small.py` / `ocr_cpu_verify.py` | **v6 tiny/small 純 CPU 延遲（關鍵發現）** |

---

## 8. 後續補充：官方路線、v5 vs v6、以及「v6 在 CPU 上更快」

### 8.1 官方 `paddleocr` 路線可用，而且不必裝 paddlepaddle

官方 `paddleocr==3.7.0`（= PP-OCRv6 發布版）+ `paddlex==3.7.2` 支援
`engine="onnxruntime"`，**核心依賴不含 paddlepaddle**，可以純 ORT 跑。

- 模型用官方 `{model}_onnx` 發佈包（HF / ModelScope / BOS / AIStudio 自動下載）
- `engine_config` 能正確傳入 `providers` / `provider_options`，
  所以 **cudnn 設定是「官方支援的參數」而不是 monkey-patch**
- 離線可用：把下載好的模型目錄設給 `model_dir`，關掉聯網即可

**關鍵事實**：官方 `PP-OCRv6_medium_det_onnx/inference.onnx` 與
PyMacro_Builder 的 `PP-OCRv6_medium_det_infer.onnx` **MD5 完全相同**
（det `97b77881…`、rec `2e23144e…`）——即兩邊用的是同一份模型，
差別只在「誰來餵 onnxruntime、cudnn 參數怎麼設」。

### 8.2 v5 vs v6（同機、同 cudnn=DEFAULT、3060 Ti）

合成圖含 ground truth，計分為整區文字的字元相似度：

| 模型 | 全屏 1920×1080 min | 全屏 acc | 框選 700×240 min | 框選 acc |
|---|---|---|---|---|
| PP-OCRv6_medium | 5.169s | 1.000 | 2.276s | 1.000 |
| **PP-OCRv6_small** | **1.451s** | 1.000 | **1.008s** | 1.000 |
| **PP-OCRv6_tiny** | **0.838s** | 1.000 | **0.357s** | 0.991 |
| PP-OCRv5_server | 35.996s | 1.000 | 6.063s | 1.000 |
| PP-OCRv5_mobile | 2.712s | 1.000 | 0.856s | 1.000 |

**v5 不是答案**：v5_server 在 GPU 上慢到 36s（比 v6_medium 還慢 7 倍），
v5_mobile 跟 v6_small 同級但更慢。**要快要往 v6 的小檔位走，不是退回 v5。**

### 8.3 CPU vs GPU：真正跑起來的數字（2026-10-19 修訂）

> **⚠️ 本節先前寫的「框選 30–60ms、CPU 快一個量級、快 15 倍」是量測假象，已作廢。**
>
> 原因：當時量的是**同一張圖重複跑**。paddlex 會依輸入形狀快取推論計劃，
> 同一張圖第二次開始就命中快取，量到的是「快取命中值」而不是推論成本。
> 真實的截圖情境每張圖都不同 —— 每次都是新的輸入形狀，都要重新規劃。

**修正後的量測方法**：每輪餵**不同**的圖（模擬真實截圖），並保留同圖重複當對照組。

| 情境 | CPU（PP-OCRv6_small） | GPU（PP-OCRv6_small） |
|---|---|---|
| 同一張圖重複跑（快取命中） | 0.05–0.06s | — |
| **每張圖都不同（真實情境）** | **0.43–1.36s** | **0.82–1.98s** |

逐尺寸對照（cleanenv，同一批圖）：

| 框選尺寸 | CPU median | GPU median |
|---|---|---|
| 500×160 | 0.430s | 0.818s |
| 760×280 | 0.761s | 0.915s |
| 1400×600 | 1.364s | 1.982s |

**結論修正**：CPU 仍然較快，但領先幅度是 **約 1.2–1.9×**，**不是**先前寫的 15×。
另外，單獨跑 det 或 rec 模型都很快（det 0.05–0.15s、rec 0.01–0.07s）；
成本主要來自 paddlex/ORT 對**新輸入形狀**的重新規劃（新形狀的第一張圖
可能要 2.5–5.8s，之後同形狀才降下來）。

### 8.3.1 關鍵實作細節：CPU 必須明確指定 provider

安裝的是 `onnxruntime-gpu` 時，只寫 `engine_config={"device_type":"cpu"}` 會**自動
前置 TensorRT/CUDA provider**，導致「明明指定 CPU 卻跑了 GPU 而且更慢」：

| engine_config | 760×280 實測 |
|---|---|
| `{"device_type":"cpu"}` | 2.410s |
| `{"device_type":"cpu","providers":["CPUExecutionProvider"],"provider_options":[{}]}` | **0.515s** |

差距 **4.7×**。`ocr_cli.py` 因此硬性寫入 `providers`，不依賴預設推斷。
這個坑很隱蔽 —— 設定看起來正確、程式不會報錯，只是慢 5 倍。

### 8.3.2 CPU 路線的其他好處仍然成立

- 純 CPU 的 `onnxruntime` 比 GPU 版小約 **350MB**（見 8.4）。
- 不需要 CUDA/cuDNN runtime，部署到沒有 NVIDIA 顯示卡的機器上不會失敗。
- 對「螢幕截圖、框選區域通常不大」的場景，0.4–1.4s 在互動上可接受。

**所以選 CPU 的結論不變，但理由從「快一個量級」改成「快 1.2–1.9× + 小 350MB +
部署單純」。**「20 倍」這種數字不要再引用。

### 8.4 打包體積

| 路線 | 安裝後 | 說明 |
|---|---|---|
| CPU（`onnxruntime`） | **541MB** | ORT 只有 48MB（`onnxruntime.dll` 18.4MB） |
| GPU（`onnxruntime-gpu`） | **977MB** | ORT 393MB，其中 `onnxruntime_providers_cuda.dll` **349MB** |
| 扣掉 dist-info / pycache / 測試後 | 約 **735MB**（GPU 路線） | 這是 PyInstaller 實際會碰的量級 |
| 模型 det+rec | **133MB**（59+74） | 若選 small/tiny 更小 |

**選 CPU 路線可直接省下約 350MB**，而且速度更快 —— 這是本文最划算的結論。

### 8.5 對官方 benchmark 的取用建議

官方公布的硬數字只有：參數 1.5M/7.7M/34.5M、medium 相對 v5_server
偵測 +4.6~4.9%／識別 +5.1%、medium 在 Xeon+OpenVINO 1.40s、
tiny 在 Xeon 0.2s、tiny 在 M4 瀏覽器 97ms、A100 0.13s、CPU 加速 5.2×。

可直接引用：**檔位／參數量／相對精度提升／「CPU 可用」這個方向**，
以及三檔的「大小－精度－速度」排序。這些拿來選型夠用。

不可直接引用：**絕對延遲**。官方沒有 CPU 型號細節、沒有 batch/解析度/精度
（FP32/FP16/INT8）說明、沒有後端對照、完全沒有 CUDA 數據；而且官方是用
**OpenVINO** 跑 CPU，我們實際用的是 **onnxruntime** —— 後端不同，數字不可移植。
我們的場景（螢幕截圖、可能含介面文字）也跟官方評測集不同。

結論：**官方數據用來「決定試哪一檔」，我們的實測用來「決定最終用哪一檔」。**
以本案為例，只看官方會直覺選 GPU + medium；實測卻發現 CPU + small/tiny
才是正解，這個反轉官方數據給不出來。

