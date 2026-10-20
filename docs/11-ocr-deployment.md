# 11 — OCR 的部署方式（行程內 ONNX，沒有 Python）

> 2026-10-20 改寫。這一頁之前回答的是「venv 會不會被上傳到 GitHub」；
> 那個問題隨著 Python 後端一起消失了，現在要回答的是：
> **「解壓縮之後，OCR 到底靠什麼跑起來，少一個檔會怎樣。」**

## 11.1 結論先講

出貨包就是這五個檔案，放在同一個目錄：

```
CapWGC.exe           739 KB     主程式
onnxruntime.dll     16.5 MB     推論引擎（微軟 ONNX Runtime，MIT）
models\ppocr_det.onnx   9.4 MB  文字偵測（PP-OCRv6_small_det）
models\ppocr_rec.onnx  20.2 MB  文字辨識（PP-OCRv6_small_rec）
models\ppocr_dict.txt    73 KB  字元表（18708 行）
```

- **沒有 Python、沒有 venv、沒有 pip、沒有首次啟動下載。**
- **沒有 VC++ Redistributable**：build.bat 沒指定 `/MD`，cl 預設走靜態 CRT，
  `dumpbin -dependents` 查不到 `VCRUNTIME140.dll` / `MSVCP140.dll`。
- 少了 `models\` 或 `onnxruntime.dll` **只會讓 OCR 那一格變灰**，錄影、截圖、
  框選、托盤、熱鍵全部照常。細節見 §11.5。

## 11.2 這條路是怎麼來的

舊版是「spawn 一個 Python 子行程跑 `tools/ocr_cli.py`，用 stdin/stdout 的
JSON Lines 溝通」。那樣的優點是換模型/改前處理不必重編，代價是：

| 問題 | 具體長相 |
|---|---|
| 使用者要自己建 venv | `py -3.11 -m venv` + `pip install -r tools\requirements-ocr.txt`（paddlepaddle 一族，數百 MB） |
| 要在設定裡填 python.exe 路徑 | 每個人的 Python 位置都不同，等於沒有預設值 |
| 首次啟動要下載模型 | paddlex 自己抓 133 MB，離線機器直接死 |
| 每次辨識都要寫一張暫存 PNG | 我們手上本來就是 BGRA，卻要 encode 再讓 Pillow 解回來 |

對一個「錄屏小工具」來說，這是唯一說不通的地方。所以 2026-10-20 把整條路
搬進行程內：`src/core/ocr_onnx.cpp` 直接呼叫 ONNX Runtime C++ API，
`src/app/ocr.cpp` 只剩執行緒與訊息的膠水（見該檔開頭的說明）。

**為什麼不是 Paddle Inference**：官方 C++ 推論庫本身加上一票依賴，出貨體積
是「幾百 MB」等級；而 paddlex 匯出的 ONNX 用 ORT 跑，一份 16.5 MB 的 dll 就夠。
模型是同一個模型，差的只是執行引擎。

**數值一致性**：`src/core/ocr_onnx.cpp` 是照 paddlex 的 Python 前後處理一比一
還原的（常數、通道順序、補值、取整方式都對齊，理由寫在檔案開頭）。實測同一張
1200×620 合成圖，C++ 與 Python 的 **10 行文字完全相同**，分數只在小數第三位
有差（det 框的邊界差一格）。同一張圖 C++ 約 1.0~1.1 秒、Python 約 1.75 秒。

## 11.3 模型與字表從哪裡來

模型不進版控（det 9.4 MB + rec 20.2 MB，對只想看錄影程式碼的人是永久成本），
由 `build\prepare-models.bat` 從 paddlex 快取複製過來：

```bat
build\prepare-models.bat
rem 來源：%USERPROFILE%\.paddlex\official_models\PP-OCRv6_small_{det,rec}_onnx\inference.onnx
rem 目的：models\ppocr_{det,rec}.onnx
```

沒有 paddlex 快取的話，去 PaddlePaddle model zoo 抓 ONNX 匯出版，用同樣的
檔名丟進 `models\` 即可。

**`ppocr_dict.txt` 例外，它進版控。** 只有 73 KB，而缺了它的症狀是
「每一行都認得出來、但全部是亂碼」——那種 bug 看 log 完全看不出來，不值得
讓每個人重踩一次。內容是 `inference.yml` 的 `PostProcess.character_dict`，
一行一個字（index 0 是 CTC 的 blank，最後補一個空白，共 18708 行）。

## 11.4 設定

`%LOCALAPPDATA%\CapWGC\settings.ini` 只剩一個 OCR 相關的鍵：

```ini
[ocr]
warned=1          ← 「OCR 缺少檔案」的提示已經講過一次（見 §11.6）
```

模型目錄的搜尋順序是 `exe目錄\models`、`..\models`、`..\..\models`
（`src/app/ocr.cpp` 的 `FindModelsDir`），所以開發時 `out\CapWGC.exe` 配專案
根目錄的 `models\` 會自動命中。

**`model_dir` 這個鍵已經移除**（2026-10-20）。原本設定視窗有一個「模型」欄位可以
覆寫上面的搜尋結果，但它只能改**路徑**，不能選**版本** —— 使用者看到的「模型」
很容易被讀成「以後出新版可以在這裡挑」，實際上換版本要換的是 `models\` 裡的
`inference.onnx` 本身。功能與期待不符、版面又要為它讓路，所以整個欄位連同 ini
的寫入一起刪掉，`Settings` 也不再讀這個鍵。舊 ini 留著那一行無害，只是被忽略。

舊版留下的 `python=` / `script=` 兩個鍵同理，現在沒人讀，留著也無害
（`Save()` 不會再寫它們）。

## 11.5 三種「少了東西」的失敗，與降級行為

OCR 是這個專案唯一的外部資產依賴，所以每一種缺失都必須「只讓 OCR 變灰」，
不能讓程式起不來。實際做法有兩層：

1. **連結時用 `/DELAYLOAD:onnxruntime.dll`**（`build\build.bat`）。沒有它，
   DLL 不在就是載入器在 `main()` 之前把行程殺掉 —— 連托盤都不會出現。
2. **執行時先自己探測**：`OcrEngine::WorkerMain` 先 `LoadLibraryW` 一次，
   再用 `capwgc::OcrRuntimeError()` 確認那份 DLL 真的提供我們編譯時要的
   `ORT_API_VERSION`。兩者都過才去建 `Ort::Env`。

| 情況 | 症狀 | 程式行為 |
|---|---|---|
| `models\` 不在或不完整 | `ocr ready=0 detail=models 目錄不完整…` | OCR 灰，提示**一次** |
| `onnxruntime.dll` 不在 | `ocr ready=0 detail=找不到 onnxruntime.dll…` | OCR 灰，提示**一次** |
| DLL 在但版本不符 | `ocr ready=0 detail=OCR 推論引擎版本不相容…` | OCR 灰，提示**一次** |
| 模型損壞、推論失敗 | `ocr FAILED: …` | OCR 灰，**每次都**提示 |

「提示一次」由 `settings.ini` 的 `warned=1` 記住：這些是「這個安裝少了東西」，
不會因為多開幾次就自己好，每次開機被同一個氣泡打擾比沒有 OCR 更煩。抑制時
仍會留一行 `ocr warn suppressed` 在 log 裡，方便分辨「早就講過」與「這次才壞」。

### 為什麼「DLL 在」還需要版本檢查

Windows 11 的 `C:\Windows\System32\` 裡有**微軟自己的** `onnxruntime.dll`
（Windows ML 帶的，版本 1.17.1，API 版本 17）。我們的模型鏈用的是 API 版本
31。少了我們的 dll 時，載入器會照搜尋順序抓到那一份：**它載得起來**，但
`OrtGetApiBase()->GetApi(31)` 回 `nullptr`，接著 `Ort::Env` 建構就是空指標
解參考 —— 那是 access violation，不是可以 `catch` 的 C++ 例外，整個行程會被
帶走（實測：連一行 log 都來不及寫）。這就是 §11.5 第 2 層存在的理由。

驗證指令：

```bat
dumpbin -dependents out\CapWGC.exe | findstr /i onnx
rem 沒有輸出 = 已經是延遲載入（正確）

dumpbin -imports out\CapWGC.exe | findstr /i delay
rem 應該看到 onnxruntime.dll 在 delay import 表裡
```

## 11.6 打包

```bat
build\package.bat
rem -> dist\CapWGC-0.1.0\   +   dist\CapWGC-0.1.0.zip（31 MB）
```

腳本會先跑 `build.bat gui`，再把 exe、`onnxruntime.dll`、`models\` 三樣東西
複製進 `dist\CapWGC-0.1.0\`，並用 PowerShell 寫一份 UTF-8（含 BOM）的
`README.txt`。**用 `echo` 寫中文會被 OEM 碼頁吃掉，所以那段是 PowerShell。**

驗收方式不是「看資料夾裡有東西」，而是從乾淨目錄真的跑一次：

```bat
python tools\ocr_snip_check.py D:\some\clean\dir\CapWGC.exe
```

它會啟動 `CapWGC.exe --snipocr`、對 `CapWGCSelect` 視窗 PostMessage 餵一次
假拖曳、等 `gui.log` 出現 `ocr ok lines=`，最後**讀回剪貼簿**比對（只看 log
不夠：log 可以說 ok 而剪貼簿是空的）。實測（乾淨目錄、打包版）：

```
overlay=0x… toolbar=0x… 177x50
  log: overlay.BeginSnip -> 1 ocrReady=1 def=2
  log: snip done ok=1 action=2 rect=60,60,420,240
  log: ocr request id=1 360x180
  log: ocr ok lines=8 ms=406
  log: ocr clip ok n_lines=8 chars=32
--- clipboard ---
（8 行真實文字）
PASS
```

## 11.7 版控與 repo 體積

```
models/*.onnx            不進版控（30 MB 權重）
models/ppocr_dict.txt    進版控（73 KB，缺了輸出全是亂碼）
third_party/onnxruntime/ 進版控（include 1.2 MB + lib 17 MB，含 dll）
dist/                    不進版控（產物）
```

`third_party/onnxruntime` 進版控是刻意的：它是**編譯期依賴**（標頭 + import
library），沒有它就編不過，而 ORT 的發行包動輒上百 MB（含 CUDA provider）。
這裡只留 CPU 版的最小集合。

## 11.8 快速自我檢查

```bash
# 1. 產品路徑不該再啟動任何子行程（舊版是 spawn python.exe 跑 ocr_cli.py）
grep -rniE "CreateProcess|ShellExecute|_popen" src/app/ocr.cpp src/core/ocr_onnx.cpp
# 沒有輸出 = 純行程內

# 2. exe 不該直接依賴 onnxruntime.dll（應該是延遲載入）
dumpbin -dependents out/CapWGC.exe | grep -i onnx        # 沒有輸出
dumpbin -imports   out/CapWGC.exe | grep -i -A2 "delay"  # 看得到 onnxruntime.dll

# 3. 打包內容齊全
unzip -l dist/CapWGC-0.1.0.zip

# 4. 端到端（含剪貼簿）
python tools/ocr_snip_check.py
```

注意 `src/core/ocr_onnx.cpp` 的註解裡仍然大量出現 `paddlex` —— 那些是「這個常數
對應 Python 實作的哪一行」的引用，是刻意的，不是殘留依賴。

`tools/ocr_cli.py` 與 `tools/requirements-ocr.txt` 仍然留著，用途只剩一個：
**當 C++ 實作走鐘時，拿它當數值參考**（跑同一張圖比對輸出）。產品路徑不再碰它。
