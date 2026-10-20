#!/usr/bin/env python3
# ============================================================================
# ocr_cli.py — CapWGC 的 OCR 後端（常駐程序，stdin 餵圖、stdout 吐 JSON）
# ============================================================================
#
# 為什麼是「常駐 + stdin」而不是「每次叫一次 python」：
#
#   PP-OCRv6 的模型載入（onnxruntime 建 session、讀 30MB 權重）在 CPU 上要
#   1~3 秒。截圖是「按下熱鍵、框一下、馬上要看字」的互動，每次重載模型的
#   代價完全吃掉辨識本身的 0.06 秒。所以這支程式啟動時載一次模型，之後每張
#   圖只做推論。
#
# 協定（一行一個 JSON，UTF-8，雙向）：
#
#   程式 → 我們（啟動完成時，一行）：
#     {"ready": true, "det": "...", "rec": "...", "engine": "cpu"}
#
#   我們 → 程式（每張圖）：
#     {"image": "<絕對路徑>", "id": 7}
#     （用路徑傳圖而不是把 base64 塞進 stdin：1920×1080 的 PNG base64 是
#       2~5MB 一行，pipe 的緩衝與解析都會拖慢；檔案路徑是幾十 bytes。
#       檔案由呼叫端在 temp 目錄建立，用完自己刪。）
#
#   程式 → 我們（結果，一行）：
#     {"id": 7, "ok": true, "texts": ["...", "..."],
#      "scores": [0.99, 0.97], "ms": 61}
#     失敗時 {"id": 7, "ok": false, "error": "..."}
#
#   我們 → 程式：{"cmd": "quit"}   或直接關掉 stdin（EOF）
#
# 用 JSON Lines 而不是自訂協定：出錯時人眼看得懂、cat 得動、不必寫 parser。
#
# 裝置：預設 CPU。實測（2026-10，見 docs/10-ocr-cuda-investigation.md §8）：
#   PP-OCRv6_small，760×280 的框選、每張圖都不同（真實截圖情境）
#     CPU 0.43~1.36s    GPU 0.82~1.98s
#   CPU 仍然較快，且純 CPU 的 onnxruntime 比 GPU 版（含 CUDA/cuDNN DLL）
#   小約 350MB。但領先幅度不像先前以為的那麼大 —— 先前「CPU 只要 0.057s」
#   是「同一張圖重複跑」量到的快取命中值，不是真實數字。
#   --device gpu 保留給後續換大型模型（medium）時使用，那時 GPU 才會明顯贏。
# ============================================================================

import argparse
import json
import os
import sys
import time


def _force_utf8_stdio():
    """把 stdin/stdout 釘死在 UTF-8。

    沒有這一步，Windows 上會出事 —— 而且症狀很嚇人：

        UnicodeEncodeError: 'cp950' codec can't encode character '\\u4e2a'
        in position 121: illegal multibyte sequence

    原因是 Python 在「管線（pipe）」上不會用 UTF-8，而是用系統的 ANSI
    代碼頁（繁中 Windows 就是 cp950，簡中就是 cp936）。OCR 認到的字是
    任意的 —— 只要截圖裡有一個簡體字、一個日文假名、或一個特殊符號不在
    cp950 裡，print() 就會拋例外，整個常駐程序死掉，使用者看到的就是
    「OCR 失敗」的氣球。

    呼叫端（ocr.cpp）全程用 UTF-8 收發，所以這裡兩邊都對齊 UTF-8：
      * stdout/stderr → UTF-8（吐 JSON 給 C++）
      * stdin → UTF-8（收 C++ 送來的請求）
    另外 errors="replace" 是刻意的：真的遇到無法編碼的字元時，寧可輸出
    一個替代符號讓那一行結果有點瑕疵，也不要讓整個引擎掛掉。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    try:
        sys.stdin.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def log(*a):
    """一律寫 stderr。stdout 是協定通道，多一個字元都會讓解析端錯位。"""
    print("[ocr_cli]", *a, file=sys.stderr, flush=True)


# 這兩個環境變數必須在 import paddleocr 之前設好：
#   PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK  關掉「連伺服器問模型來源」的檢查，
#       離線機器上沒關它會卡在網路逾時，看起來像「OCR 掛掉」。
#   PADDLE_PDX_MODEL_SOURCE=BOS            從 ModelScope 下載而不是 HuggingFace
#       （只在需要下載時有差；本地已有模型時不會走這條）
os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
os.environ.pop("HF_ENDPOINT", None)


def build_engine(args):
    import logging

    logging.disable(logging.WARNING)

    from paddleocr import PaddleOCR

    kwargs = dict(
        text_detection_model_name=args.det,
        text_recognition_model_name=args.rec,
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=False,
        engine="onnxruntime",
    )

    # 本地模型目錄（可選）。指了就用本地的，沒指就讓 paddlex 走它自己的快取
    # （%USERPROFILE%\.paddlex\official_models）。
    if args.det_dir:
        kwargs["text_detection_model_dir"] = args.det_dir
    if args.rec_dir:
        kwargs["text_recognition_model_dir"] = args.rec_dir

    if args.device == "gpu":
        providers = ["CUDAExecutionProvider"]
        if args.allow_cpu_fallback:
            providers.append("CPUExecutionProvider")
        # cudnn_conv_algo_search=DEFAULT 是關鍵：onnxruntime 的預設是
        # EXHAUSTIVE（每個 shape 都跑一次演算法搜尋）。OCR 的輸入寬度每張圖
        # 都不一樣，EXHAUSTIVE 會一而再地重搜，實測慢 2~6 倍。
        kwargs["engine_config"] = {
            "device_type": "gpu",
            "device_id": args.device_id,
            "providers": providers,
            "provider_options": [
                {
                    "device_id": args.device_id,
                    "cudnn_conv_algo_search": "DEFAULT",
                    "do_copy_in_default_stream": True,
                }
            ] + ([{}] if args.allow_cpu_fallback else []),
            "enable_cpu_mem_arena": False,
        }
    else:
        # CPU 這條路一定要**明寫 providers**，不能只給 device_type。
        #
        # 實測（同一台機器、同一批圖，760×280 選取範圍）：
        #     {"device_type": "cpu"}                                  2.410s
        #     {"device_type":"cpu","providers":["CPUExecutionProvider"]} 0.515s
        # 差 4.7 倍。原因是 device_type 只決定「偏好」，真正的 provider 清單
        # 由 onnxruntime 自己排 —— 如果裝的是 onnxruntime-gpu 那個 build，它會
        # 把 TensorRT/CUDA 也排進去，即使 device_type=cpu 也先做一輪可用性
        # 探測與 fallback。明寫就完全繞過這件事。
        #
        # 這正是「同一支程式換一台機器就慢 5 倍」的典型來源，所以寫死在這裡，
        # 不靠環境。
        kwargs["engine_config"] = {
            "device_type": "cpu",
            "providers": ["CPUExecutionProvider"],
            "provider_options": [{}],
        }

    log("loading", args.det, args.rec, "device=", args.device)
    return PaddleOCR(**kwargs)


def run_once(ocr, image_path):
    """回傳 (texts, scores)。找不到檔或推論失敗會丟例外，由呼叫端包成 JSON。"""
    import numpy as np
    from PIL import Image

    img = Image.open(image_path).convert("RGB")
    arr = np.array(img)
    res = ocr.predict(arr)

    texts, scores = [], []
    try:
        r0 = res[0]
        texts = [str(t) for t in (r0.get("rec_texts") or [])]
        scores = [float(s) for s in (r0.get("rec_scores") or [])]
    except Exception as e:          # 結果結構變了也要回報，不要整個死掉
        log("result parse warn:", repr(e))
    return texts, scores


def main():
    ap = argparse.ArgumentParser(description="CapWGC OCR backend (resident, JSON lines)")
    ap.add_argument("--det", default="PP-OCRv6_small_det",
                    help="文字偵測模型名（預設 PP-OCRv6_small_det）")
    ap.add_argument("--rec", default="PP-OCRv6_small_rec",
                    help="文字辨識模型名（預設 PP-OCRv6_small_rec）")
    ap.add_argument("--det-dir", default="", help="偵測模型的本地目錄（可選）")
    ap.add_argument("--rec-dir", default="", help="辨識模型的本地目錄（可選）")
    ap.add_argument("--device", default="cpu", choices=["cpu", "gpu"])
    ap.add_argument("--device-id", type=int, default=0)
    ap.add_argument("--allow-cpu-fallback", action="store_true",
                    help="gpu 模式下 CUDA 不可用時退回 CPU")
    ap.add_argument("--warmup", action="store_true", default=True,
                    help="啟動後先跑一張小圖，把 ORT 的首次配置成本挪出互動路徑")
    ap.add_argument("--no-warmup", dest="warmup", action="store_false")
    args = ap.parse_args()

    # 必須在「任何輸出之前」把編碼釘死（見 _force_utf8_stdio 的說明）。
    _force_utf8_stdio()

    t0 = time.time()
    try:
        ocr = build_engine(args)
    except Exception as e:
        # 載入失敗要讓呼叫端「一眼看懂」：直接吐 JSON 到 stdout，而不是只寫
        # stderr 然後默默結束 —— 呼叫端的 readline 會拿到 EOF，分不清是
        # 「還沒好」還是「永遠不會好」。
        print(json.dumps({"ready": False, "error": f"{type(e).__name__}: {e}"}),
              flush=True)
        return 2

    if args.warmup:
        # 暖機是這支程式最重要的一段，理由用數字說話（cleanenv / ORT 1.23.2，
        # 700×180 含三行字的圖）：
        #     call0  3.77s     ← 沒有暖機，使用者第一次截圖要等這麼久
        #     call1  0.041s
        #     call2  0.040s
        # 3.7 秒全部花在「辨識模型的 ORT session 第一次被建立」（graph 優化 +
        # 權重配置）。它之所以拖到第一次真圖才做，是因為**只有真的偵測到文字**
        # 才會呼叫辨識模型 —— 空白圖與隨機雜訊都偵測不到文字框，等於沒暖到。
        #
        # 所以暖機圖必須「長得像文字」：在白底上畫一堆黑色小方塊（暗塊的
        # 密度與大小接近一行字），det 就會產生候選框，rec 也跟著被初始化。
        # 不依賴字型檔（Pillow 的 truetype 在精簡環境可能找不到），也不需要
        # 把 numpy 以外的东西帶進來。
        try:
            import numpy as np

            warm = np.full((240, 720, 3), 252, dtype=np.uint8)
            # 五行「假文字」：每行由寬度不一的黑塊組成，塊間留白。
            rng = np.random.default_rng(1234)
            for row in range(5):
                y0 = 20 + row * 44
                x = 24
                while x < 690:
                    bw = int(rng.integers(6, 22))
                    if rng.random() < 0.82:      # 多數位置是「字」
                        warm[y0:y0 + 22, x:x + bw] = 18
                    x += bw + int(rng.integers(3, 10))
            # 跑兩次：第一次建立 session，第二次把 ORT 的其餘延遲初始化清掉。
            for _ in range(2):
                ocr.predict(warm)
        except Exception as e:
            log("warmup warn:", repr(e))

    print(json.dumps({
        "ready": True,
        "det": args.det, "rec": args.rec, "device": args.device,
        "load_ms": int((time.time() - t0) * 1000),
    }), flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            print(json.dumps({"ok": False, "error": f"bad json: {e}"}), flush=True)
            continue

        if req.get("cmd") == "quit":
            break

        rid = req.get("id")
        path = req.get("image", "")
        if not path:
            print(json.dumps({"id": rid, "ok": False, "error": "no image path"}),
                  flush=True)
            continue

        t = time.time()
        try:
            texts, scores = run_once(ocr, path)
            # 刻意不加 ensure_ascii=False：預設（True）會把中文轉成
            # \uXXXX 的純 ASCII，這樣輸出「完全不經過編碼器」，cp950 想
            # 找麻煩也找不到地方下手。（_force_utf8_stdio 是另一層保險：
            # 即使有人改回 ensure_ascii=False，UTF-8 也還撐得住。）
            #
            # 前提是 ocr.cpp 真的解得回來。曾經這裡的註解寫「解析器本來就
            # 支援 \uXXXX」，但當時 texts **陣列**走的是另一份手抄的解析
            # 程式，那份漏了 case 'u' —— 剪貼簿裡出現的是字面 "u6587"。
            # 現在 ocr.cpp 兩條路共用同一個 ParseJsonStringAt，才有這個前提。
            print(json.dumps({
                "id": rid, "ok": True, "texts": texts, "scores": scores,
                "ms": int((time.time() - t) * 1000),
            }), flush=True)
        except Exception as e:
            print(json.dumps({"id": rid, "ok": False,
                              "error": f"{type(e).__name__}: {e}"}), flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
