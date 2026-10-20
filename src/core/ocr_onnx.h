// ============================================================================
// ocr_onnx.h — 原生 OCR 推論（ONNX Runtime，det + rec，不經 Python）
// ============================================================================
//
// 這是 tools/ocr_cli.py 那條 Python 路線的替代品：同樣的兩個模型
// （PP-OCRv6_small_det / _rec 的 ONNX 匯出版），同樣的前後處理語意，
// 但整條路都在行程內完成。
//
// 為什麼要重寫成 C++：
//   - Python 版本讓「一個錄屏軟體」多出一個幾百 MB 的執行期依賴，
//     而且使用者得自己建 venv、自己填 python.exe 路徑。對「下載解壓就能用」
//     這件事來說，這是唯一一個說不通的地方。
//   - 我們本來就有現成的像素（ScreenBitmap 是 BGRA），Python 那條卻要先
//     寫成暫存 PNG、再讓 Pillow 解回來。C++ 這條直接把記憶體餵進去。
//
// 契約：
//   - Load() 會把兩個 ONNX session 建好並做一次暖機（見 .cpp 的說明）。
//     這一步在 CPU 上要 0.5~2 秒，所以呼叫端必須在背景執行緒做。
//   - Run() 不是執行緒安全的：一個 OcrOnnx 實例同一時間只能有一個 Run()。
//     要平行就跑多個實例（每個都要自己的 session）。
//   - 輸入是 BGRA、top-down、stride 可自訂（D3D11 的 staging texture 拿到的
//     通常有 padding，所以不能假設 stride == w*4）。
//
// 前置資產（放在 exe 旁邊的 models\ 底下）：
//   ppocr_det.onnx   PP-OCRv6_small_det 的 ONNX 匯出
//   ppocr_rec.onnx   PP-OCRv6_small_rec 的 ONNX 匯出
//   ppocr_dict.txt   rec 的字元表，一行一個字（index 0 是 blank，最後補一個空白）
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace capwgc {

// 一行辨識結果。box 是**原圖座標**的軸對齊矩形（左上含、右下不含）。
struct OcrLine {
    std::wstring text;
    float score = 0.f;
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};

class OcrOnnx {
public:
    OcrOnnx();
    ~OcrOnnx();
    OcrOnnx(const OcrOnnx&) = delete;
    OcrOnnx& operator=(const OcrOnnx&) = delete;

    // 失敗時 err 是給人看的一行英文/中文說明（會原樣進 log）。
    bool Load(const std::wstring& detPath, const std::wstring& recPath,
              const std::wstring& dictPath, std::string* err);

    // bgra：w*h 的 BGRA8888，top-down。stride 單位是位元組。
    // 結果按「先上後下、同列先左後右」排序（人眼的閱讀順序）。
    bool Run(const uint8_t* bgra, int w, int h, int stride,
             std::vector<OcrLine>* out, int* elapsedMs, std::string* err);

    bool loaded() const { return impl_ != nullptr; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// 檢查行程內那份 onnxruntime 能不能用（有沒有編譯時要的 API 版本）。
// 回傳 nullptr = 可用；否則是一行給人看的原因。
//
// 為什麼需要這個：Windows 11 的 System32\ 裡有微軟自己的 onnxruntime.dll，
// 版本比 PaddleOCR 的匯出工具鏈用的舊。我們的 DLL 不在時載入器會抓到那一份，
// 它「載得起來」但沒有 ORT_API_VERSION 對應的 API 表；直接拿去建 Ort::Env 是
// 空指標解參考（access violation），不是可以 catch 的 C++ 例外，整個行程會被
// 帶走。所以在碰任何 Ort:: 物件之前先用這個函式擋一次。
const char* OcrRuntimeError();

} // namespace capwgc
