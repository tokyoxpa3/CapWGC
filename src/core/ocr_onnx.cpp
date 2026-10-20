// ============================================================================
// ocr_onnx.cpp — PP-OCRv6 的 C++ 推論實作
// ============================================================================
//
// 這份程式碼是照著 paddlex 的 Python 實作一比一還原的，不是「大致上像」。
// 下面每一個常數都能在 paddlex 的原始碼裡找到對應的那一行，改動前先確認：
//
//   偵測（det）
//     paddlex/inference/models/text_detection/processors.py
//       resize_image_type0()      縮放比例、32 對齊、max_side_limit
//       NormalizeImage            BGR + mean/std
//       DBPostProcess             thresh / box_thresh / unclip_ratio
//   辨識（rec）
//     paddlex/inference/models/text_recognition/processors.py
//       resize_norm_img()         48 高、右側補 0（補的是**歸一化後**的 0）
//       CTCLabelDecode            blank 在 index 0、字典從 1 開始、最後補空白
//
// 為什麼要這麼較真：OCR 的錯字有九成來自前後處理與訓練時不一致（通道順序、
// 補值、縮放取整），而不是模型本身。只要有一處不同，輸出就會變成「大概對
// 但總有幾個字怪怪的」，而且極難 debug —— 因為模型永遠不會報錯。
//
// 兩個容易踩的細節：
//   1. Python 的 round() 是「四捨六入五成雙」，C 的 round() 是「五入」。
//      det 的 32 對齊用 round(h/32)*32，邊界上會差一格，所以這裡用
//      std::nearbyint（預設捨入模式就是 half-to-even）。
//   2. rec 的右側補白是在**歸一化之後**補 0.0，換算回像素是灰色 127.5，
//      不是黑色。補 -1.0（黑）會讓模型看到一條黑邊，短字串的準確率明顯下降。
// ============================================================================

#include "ocr_onnx.h"

#include <onnxruntime_cxx_api.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace capwgc {
namespace {

// ---- 與 paddlex 對齊的常數 ------------------------------------------------
// det：來自 PP-OCRv6_small_det 的 inference.yml（PostProcess 段）與 OCR pipeline
// 的 TextDetection 段（limit_side_len / limit_type / max_side_limit）。
constexpr float kDetThresh = 0.2f;
constexpr float kDetBoxThresh = 0.45f;
constexpr float kDetUnclipRatio = 1.4f;
constexpr int kDetMaxCandidates = 3000;
constexpr int kDetLimitSideLen = 64;
constexpr int kDetMaxSideLimit = 4000;

// det 的歸一化。注意順序是 BGR：paddlex 用 cv2 讀圖（BGR），再套這組
// 「看起來像 RGB」的 mean/std —— 這是 PaddleOCR 一貫的做法，模型就是這樣
// 訓練的。照 RGB 順序餵會掉準確率，而且不會有任何錯誤訊息。
constexpr float kDetMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kDetStd[3] = {0.229f, 0.224f, 0.225f};

// rec：48 高；寬度下限 320（= 320/48 的長寬比下限），上限 3200。
constexpr int kRecHeight = 48;
constexpr int kRecBaseWidth = 320;
constexpr int kRecMaxWidth = 3200;

// 太小的連通塊直接丟掉：DB 的輸出經過 0.2 門檻後，雜訊通常只剩幾個像素。
constexpr int kMinBlobArea = 8;
// 太扁的框（單邊 < 3px）rec 讀不出東西，paddlex 也是這樣濾的。
constexpr int kMinBoxSide = 3;

// ---- 小工具 ---------------------------------------------------------------

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// 把 BGRA 的一行（或一塊）轉成「歸一化後的 float」，寫進 dst 的某個平面。
inline void NormalizeRow(const uint8_t* src, int count, int bpp, const float* mean,
                         const float* invStd, float* dstB, float* dstG, float* dstR) {
    for (int i = 0; i < count; ++i) {
        const uint8_t* p = src + (size_t)i * bpp;
        dstB[i] = ((float)p[0] / 255.0f - mean[0]) * invStd[0];
        dstG[i] = ((float)p[1] / 255.0f - mean[1]) * invStd[1];
        dstR[i] = ((float)p[2] / 255.0f - mean[2]) * invStd[2];
    }
}

// 雙線性縮放（BGRA → BGRA）。與 cv2.resize(INTER_LINEAR) 的取樣位置一致：
// 目標像素中心 = (i + 0.5) * srcSize / dstSize - 0.5。
void ResizeBgra(const uint8_t* src, int sw, int sh, int sstride, int dw, int dh,
                std::vector<uint8_t>* dst) {
    dst->resize((size_t)dw * dh * 4);
    uint8_t* out = dst->data();
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    const double rx = (double)sw / dw;
    const double ry = (double)sh / dh;
    std::vector<int> x0(dw), x1(dw);
    std::vector<float> fx(dw);
    for (int x = 0; x < dw; ++x) {
        double sx = (x + 0.5) * rx - 0.5;
        if (sx < 0) sx = 0;
        int ix = (int)sx;
        if (ix >= sw - 1) { ix = sw - 1; sx = (double)ix; }
        x0[x] = ix;
        x1[x] = (ix + 1 < sw) ? ix + 1 : ix;
        fx[x] = (float)(sx - ix);
    }
    for (int y = 0; y < dh; ++y) {
        double sy = (y + 0.5) * ry - 0.5;
        if (sy < 0) sy = 0;
        int iy = (int)sy;
        if (iy >= sh - 1) { iy = sh - 1; sy = (double)iy; }
        const int iy2 = (iy + 1 < sh) ? iy + 1 : iy;
        const float fy = (float)(sy - iy);
        const uint8_t* r0 = src + (size_t)iy * sstride;
        const uint8_t* r1 = src + (size_t)iy2 * sstride;
        uint8_t* o = out + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const uint8_t* p00 = r0 + (size_t)x0[x] * 4;
            const uint8_t* p01 = r0 + (size_t)x1[x] * 4;
            const uint8_t* p10 = r1 + (size_t)x0[x] * 4;
            const uint8_t* p11 = r1 + (size_t)x1[x] * 4;
            const float w00 = (1 - fx[x]) * (1 - fy);
            const float w01 = fx[x] * (1 - fy);
            const float w10 = (1 - fx[x]) * fy;
            const float w11 = fx[x] * fy;
            for (int c = 0; c < 4; ++c) {
                const float v = p00[c] * w00 + p01[c] * w01 + p10[c] * w10 + p11[c] * w11;
                o[(size_t)x * 4 + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

// 連通塊（8 連通）。回傳每塊的外接矩形。
struct Blob {
    int x0, y0, x1, y1;   // 右下不含
};

std::vector<Blob> FindBlobs(const std::vector<uint8_t>& mask, int w, int h) {
    std::vector<Blob> out;
    std::vector<uint8_t> seen((size_t)w * h, 0);
    std::vector<int> stack;

    for (int sy = 0; sy < h; ++sy) {
        for (int sx = 0; sx < w; ++sx) {
            const size_t idx = (size_t)sy * w + sx;
            if (!mask[idx] || seen[idx]) continue;

            Blob b{sx, sy, sx + 1, sy + 1};
            int area = 0;
            stack.clear();
            stack.push_back((int)idx);
            seen[idx] = 1;

            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                ++area;
                const int cx = cur % w;
                const int cy = cur / w;
                if (cx < b.x0) b.x0 = cx;
                if (cy < b.y0) b.y0 = cy;
                if (cx + 1 > b.x1) b.x1 = cx + 1;
                if (cy + 1 > b.y1) b.y1 = cy + 1;

                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = cy + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int nx = cx + dx;
                        if (nx < 0 || nx >= w) continue;
                        const size_t ni = (size_t)ny * w + nx;
                        if (mask[ni] && !seen[ni]) {
                            seen[ni] = 1;
                            stack.push_back((int)ni);
                        }
                    }
                }
            }
            if (area >= kMinBlobArea) out.push_back(b);
        }
    }
    return out;
}

// 一個已載入的 ONNX session（名稱、形狀都在建構時抓好，避免每次推論都問一次）。
class Session {
public:
    bool Load(Ort::Env& env, const std::wstring& path, std::string* err) {
        try {
            Ort::SessionOptions so;
            // 0 = 交給 ORT 依機器決定（它會用實體核心數）。
            so.SetIntraOpNumThreads(0);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // 連續推論同一組形狀時，開啟記憶體 pattern 快取可以省掉重配置。
            so.EnableCpuMemArena();
            session_ = std::make_unique<Ort::Session>(env, path.c_str(), so);

            Ort::AllocatorWithDefaultOptions alloc;
            auto inName = session_->GetInputNameAllocated(0, alloc);
            auto outName = session_->GetOutputNameAllocated(0, alloc);
            in_ = inName.get();
            out_ = outName.get();
            return true;
        } catch (const Ort::Exception& e) {
            if (err) *err = std::string("onnxruntime: ") + e.what();
            return false;
        }
    }

    // 回傳輸出的第一個 tensor 的資料指標與形狀；失敗回 nullptr。
    const float* Run(const std::vector<float>& input, const std::vector<int64_t>& shape,
                     std::vector<int64_t>* outShape, std::string* err) {
        try {
            auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto tensor = Ort::Value::CreateTensor<float>(
                mem, const_cast<float*>(input.data()), input.size(), shape.data(),
                shape.size());
            const char* inNames[] = {in_.c_str()};
            const char* outNames[] = {out_.c_str()};
            auto outs = session_->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1,
                                      outNames, 1);
            if (outs.empty()) {
                if (err) *err = "onnxruntime: no output";
                return nullptr;
            }
            *outShape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
            return outs[0].GetTensorData<float>();
        } catch (const Ort::Exception& e) {
            if (err) *err = std::string("onnxruntime: ") + e.what();
            return nullptr;
        }
    }

    bool ok() const { return session_ != nullptr; }

private:
    std::unique_ptr<Ort::Session> session_;
    std::string in_, out_;
};

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct OcrOnnx::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "capwgc-ocr"};
    Session det, rec;
    std::vector<std::string> dict;   // UTF-8，一行一個字（不含 blank）
    std::string err;

    bool LoadDict(const std::wstring& path);
    bool RunDet(const uint8_t* bgra, int w, int h, int stride,
                std::vector<OcrLine>* boxes, std::string* err);
    bool RunRecBatch(const uint8_t* bgra, int w, int h, int stride,
                     std::vector<OcrLine>* lines, size_t from, size_t count,
                     std::string* err);
    bool RunRecOne(const uint8_t* bgra, int w, int h, int stride, OcrLine* line,
                   std::string* err);
};

bool OcrOnnx::Impl::LoadDict(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open dict: " + WideToUtf8(path);
        return false;
    }
    std::string line;
    dict.clear();
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;   // 尾端多一個換行不算一個字元
        dict.push_back(line);
    }
    if (dict.size() < 1000) {
        err = "dict looks truncated: " + std::to_string(dict.size()) + " entries";
        return false;
    }
    return true;
}

// ---- 偵測 -----------------------------------------------------------------

bool OcrOnnx::Impl::RunDet(const uint8_t* bgra, int w, int h, int stride,
                           std::vector<OcrLine>* boxes, std::string* e) {
    // 1) 縮放比例：與 paddlex 的 resize_image_type0 完全一致。
    double ratio = 1.0;
    if (std::min(h, w) < kDetLimitSideLen) {
        ratio = (double)kDetLimitSideLen / std::min(h, w);
    }
    int rh = (int)(h * ratio);
    int rw = (int)(w * ratio);
    if (std::max(rh, rw) > kDetMaxSideLimit) {
        const double r2 = (double)kDetMaxSideLimit / std::max(rh, rw);
        rh = (int)(rh * r2);
        rw = (int)(rw * r2);
        ratio *= r2;
    }
    // 32 對齊。用 nearbyint（half-to-even）對齊 Python 的 round()。
    rh = std::max((int)std::nearbyint((double)rh / 32.0) * 32, 32);
    rw = std::max((int)std::nearbyint((double)rw / 32.0) * 32, 32);

    // 2) 縮放 + 歸一化 + CHW。
    std::vector<uint8_t> resized;
    const uint8_t* src = bgra;
    int sstride = stride;
    if (rw != w || rh != h) {
        ResizeBgra(bgra, w, h, stride, rw, rh, &resized);
        src = resized.data();
        sstride = rw * 4;
    }
    std::vector<float> input((size_t)3 * rh * rw);
    const size_t plane = (size_t)rh * rw;
    float invStd[3] = {1.0f / kDetStd[0], 1.0f / kDetStd[1], 1.0f / kDetStd[2]};
    for (int y = 0; y < rh; ++y) {
        NormalizeRow(src + (size_t)y * sstride, rw, 4, kDetMean, invStd,
                     input.data() + y * rw,
                     input.data() + plane + y * rw,
                     input.data() + plane * 2 + y * rw);
    }

    // 3) 推論。
    std::vector<int64_t> shape{1, 3, rh, rw};
    std::vector<int64_t> outShape;
    const float* pred = det.Run(input, shape, &outShape, e);
    if (!pred) return false;
    if (outShape.size() != 4 || outShape[2] != rh || outShape[3] != rw) {
        if (e) *e = "det output shape mismatch";
        return false;
    }

    // 4) DB 後處理：門檻 → 連通塊 → 分數 → unclip。
    std::vector<uint8_t> mask((size_t)rh * rw);
    for (size_t i = 0, n = mask.size(); i < n; ++i) {
        mask[i] = pred[i] > kDetThresh ? 1 : 0;
    }
    std::vector<Blob> blobs = FindBlobs(mask, rw, rh);
    if ((int)blobs.size() > kDetMaxCandidates) blobs.resize(kDetMaxCandidates);

    const double sx = (double)w / rw;   // 回到原圖的倍率
    const double sy = (double)h / rh;

    for (const Blob& b : blobs) {
        const int bw = b.x1 - b.x0;
        const int bh = b.y1 - b.y0;
        if (bw < kMinBoxSide || bh < kMinBoxSide) continue;

        // box score：這個框內預測值的平均。paddlex 用多邊形內的均值，
        // 軸對齊的矩形框下兩者等價。
        double sum = 0;
        for (int y = b.y0; y < b.y1; ++y) {
            const float* row = pred + (size_t)y * rw;
            for (int x = b.x0; x < b.x1; ++x) sum += row[x];
        }
        const float score = (float)(sum / (double)(bw * bh));
        if (score < kDetBoxThresh) continue;

        // unclip：DB 的輸出是「縮小後」的文字區域，要依 unclip_ratio 撐回去。
        // pyclipper 對矩形做 offset 的結果就是四邊各外擴 delta。
        const double area = (double)bw * bh;
        const double perim = 2.0 * (bw + bh);
        const double delta = area * kDetUnclipRatio / perim;

        OcrLine line;
        line.score = score;
        line.x0 = (float)std::max(0.0, (b.x0 - delta) * sx);
        line.y0 = (float)std::max(0.0, (b.y0 - delta) * sy);
        line.x1 = (float)std::min((double)w, (b.x1 + delta) * sx);
        line.y1 = (float)std::min((double)h, (b.y1 + delta) * sy);
        if (line.x1 - line.x0 < kMinBoxSide || line.y1 - line.y0 < kMinBoxSide) continue;
        boxes->push_back(line);
    }
    return true;
}

// ---- 辨識 -----------------------------------------------------------------

bool OcrOnnx::Impl::RunRecBatch(const uint8_t* bgra, int w, int h, int stride,
                                std::vector<OcrLine>* lines, size_t from, size_t count,
                                std::string* e) {
    // 逐行處理，不是「一批一起跑」。這一點特別查過 paddlex 的原始碼：
    //   text_recognition/predictor.py 的 process() 雖然算了一個跨批的
    //   max_wh_ratio，但那只拿去給 post_op 換算 word box 座標；真正餵給模型的
    //   張量是 processors.py 的 __call__ → [self.resize(img) for img in imgs]，
    //   每一張圖各自補到 max(320/48, 自己長寬比)。
    // 所以「把同批補到最寬」是錯的：實測會讓短行（例如「設定視窗 → 熱鍵」）
    // 右側多出一大片灰，空白字元的判斷跟著跑掉，結果反而比逐行更不像 Python。
    (void)count;
    for (size_t i = 0; i < lines->size() - from; ++i) {
        OcrLine& line = (*lines)[from + i];
        if (!RunRecOne(bgra, w, h, stride, &line, e)) return false;
    }
    return true;
}

bool OcrOnnx::Impl::RunRecOne(const uint8_t* bgra, int w, int h, int stride,
                              OcrLine* line, std::string* e) {
    // 裁切（軸對齊、夾在圖內）。
    const int cx0 = std::max(0, (int)std::floor(line->x0));
    const int cy0 = std::max(0, (int)std::floor(line->y0));
    const int cx1 = std::min(w, (int)std::ceil(line->x1));
    const int cy1 = std::min(h, (int)std::ceil(line->y1));
    if (cx1 - cx0 < 1 || cy1 - cy0 < 1) {
        line->text.clear();
        line->score = 0.f;
        return true;
    }
    const int cw = cx1 - cx0;
    const int ch = cy1 - cy0;

    // 寬度：max(320/48 的長寬比, 實際長寬比)，上限 3200。
    const double ratio = (double)cw / ch;
    const double maxRatio = std::max((double)kRecBaseWidth / kRecHeight, ratio);
    int imgW = (int)(kRecHeight * maxRatio);
    int resizedW = (int)std::ceil(kRecHeight * ratio);
    if (resizedW > imgW) resizedW = imgW;
    if (imgW > kRecMaxWidth) { imgW = kRecMaxWidth; resizedW = kRecMaxWidth; }
    if (imgW < kRecHeight) imgW = kRecHeight;

    std::vector<uint8_t> crop((size_t)cw * ch * 4);
    for (int y = 0; y < ch; ++y) {
        memcpy(crop.data() + (size_t)y * cw * 4,
               bgra + (size_t)(cy0 + y) * stride + (size_t)cx0 * 4, (size_t)cw * 4);
    }
    std::vector<uint8_t> rs;
    ResizeBgra(crop.data(), cw, ch, cw * 4, resizedW, kRecHeight, &rs);

    // 歸一化。右側沒寫到的部分維持 0.0f —— 那正是 paddlex 的 padding 值
    // （它在**歸一化之後**才補 0，換算回像素是灰色 127.5，不是黑色）。
    std::vector<float> input((size_t)3 * kRecHeight * imgW, 0.0f);
    const size_t plane = (size_t)kRecHeight * imgW;
    for (int y = 0; y < kRecHeight; ++y) {
        const uint8_t* row = rs.data() + (size_t)y * resizedW * 4;
        float* pb = input.data() + (size_t)y * imgW;
        float* pg = input.data() + plane + (size_t)y * imgW;
        float* pr = input.data() + plane * 2 + (size_t)y * imgW;
        for (int x = 0; x < resizedW; ++x) {
            const uint8_t* p = row + (size_t)x * 4;
            pb[x] = ((float)p[0] / 255.0f - 0.5f) / 0.5f;
            pg[x] = ((float)p[1] / 255.0f - 0.5f) / 0.5f;
            pr[x] = ((float)p[2] / 255.0f - 0.5f) / 0.5f;
        }
    }

    std::vector<int64_t> shape{1, 3, kRecHeight, imgW};
    std::vector<int64_t> outShape;
    const float* probs = rec.Run(input, shape, &outShape, e);
    if (!probs) return false;
    if (outShape.size() != 3) {
        if (e) *e = "rec output rank != 3";
        return false;
    }
    const int T = (int)outShape[1];
    const int C = (int)outShape[2];

    // CTC greedy decode：
    //   index 0 = blank；1..dict.size() = 字典；dict.size()+1 = 空白字元。
    //   連續重複的類別只算一次（CTC 的 collapse），blank 直接跳過。
    const int blank = 0;
    const int spaceIdx = (int)dict.size() + 1;
    std::wstring text;
    double probSum = 0;
    int probCount = 0;
    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = probs + (size_t)t * C;
        int best = 0;
        float bestV = row[0];
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestV) { bestV = row[c]; best = c; }
        }
        if (best == blank) { prev = -1; continue; }
        if (best != prev) {
            // 這裡**不能**再算一次 softmax：PP-OCR 匯出的 rec 模型最後一層
            // 已經內建 softmax，輸出就是機率（PaddleOCR 的 CTCLabelDecode 也是
            // 直接 preds.max(axis=2) 當信心值）。再套一次會得到
            // 1/sum(exp(x-max)) ≈ 1/18710 ≈ 5e-5，四捨五入後全部變成 0.000，
            // 看起來像「信心度壞了」，其實是重複歸一化。
            probSum += bestV;
            ++probCount;
            if (best == spaceIdx) {
                text.push_back(L' ');
            } else {
                const int di = best - 1;
                if (di >= 0 && di < (int)dict.size()) {
                    text += Utf8ToWide(dict[(size_t)di]);
                }
            }
        }
        prev = best;
    }
    line->text = text;
    line->score = probCount ? (float)(probSum / probCount) : 0.0f;
    return true;
}

// ============================================================================
// 對外介面
// ============================================================================

OcrOnnx::OcrOnnx() = default;

OcrOnnx::~OcrOnnx() { delete impl_; }

const char* OcrRuntimeError() {
    // OrtGetApiBase 本身一定拿得到（它是 C 介面的入口，不會回 nullptr）；
    // 真正的問題在 GetApi(版本)：版本不支援時它回 nullptr，並且自己往 stderr
    // 印一行 "The requested API version [31] is not available..."。
    const OrtApiBase* base = OrtGetApiBase();
    if (!base) return "onnxruntime: OrtGetApiBase() returned null";

    if (base->GetApi(ORT_API_VERSION) != nullptr) return nullptr;

    const char* ver = base->GetVersionString();
    static std::string msg;
    msg = std::string("onnxruntime ") + (ver ? ver : "?") +
          " does not provide API version " + std::to_string(ORT_API_VERSION);
    return msg.c_str();
}

bool OcrOnnx::Load(const std::wstring& detPath, const std::wstring& recPath,
                   const std::wstring& dictPath, std::string* err) {
    delete impl_;
    impl_ = nullptr;

    // Impl 的成員包含 Ort::Env，光建構就會向 onnxruntime 要 API 表。DLL 版本
    // 不相容時（例如 exe 旁邊沒有我們的 DLL，載入器改抓到別處的舊版）這裡會丟
    // Ort::Exception。例外若穿出這個函式就沒人接 —— 它跑在 OCR 工作執行緒上，
    // 下場是 std::terminate 直接把程式收掉。所以必須在這裡就攔下來，變成一句
    // 看得懂的錯誤字串，讓 OCR 只是變灰色。
    try {
        impl_ = new Impl();
    } catch (const std::exception& e) {
        if (err) *err = std::string("onnxruntime init: ") + e.what();
        return false;
    }

    if (!impl_->LoadDict(dictPath)) {
        if (err) *err = impl_->err;
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    if (!impl_->det.Load(impl_->env, detPath, &impl_->err)) {
        if (err) *err = impl_->err;
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    if (!impl_->rec.Load(impl_->env, recPath, &impl_->err)) {
        if (err) *err = impl_->err;
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    // 暖機：det 與 rec 的第一次推論各自要做 graph 最佳化與記憶體配置，
    // 實測加起來 0.3~1.5 秒。不先跑一次的話，使用者第一次按 OCR 就要等
    // 這段時間，看起來像「卡住」。用合成圖而不是真圖，避免依賴外部檔案。
    {
        std::vector<uint8_t> warm((size_t)640 * 240 * 4, 250);
        for (int y = 20; y < 60; ++y) {
            for (int x = 20; x < 620; ++x) {
                uint8_t* p = warm.data() + ((size_t)y * 640 + x) * 4;
                if (((x / 7) % 3) != 0) { p[0] = p[1] = p[2] = 20; }
            }
        }
        std::vector<OcrLine> lines;
        int ms = 0;
        Run(warm.data(), 640, 240, 640 * 4, &lines, &ms, nullptr);
    }
    return true;
}

bool OcrOnnx::Run(const uint8_t* bgra, int w, int h, int stride,
                  std::vector<OcrLine>* out, int* elapsedMs, std::string* err) {
    if (!impl_) {
        if (err) *err = "ocr engine not loaded";
        return false;
    }
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) {
        if (err) *err = "bad image";
        return false;
    }
    const ULONGLONG t0 = GetTickCount64();

    out->clear();
    if (!impl_->RunDet(bgra, w, h, stride, out, err)) return false;
    if (out->empty()) {
        if (elapsedMs) *elapsedMs = (int)(GetTickCount64() - t0);
        return true;
    }

    // 閱讀順序：先按「行」分組（同一行的 y 中心落在同一個桶），桶內再按 x。
    // 桶高取所有框高度的中位數 —— 用固定像素值會在字級不同的畫面上錯行。
    std::vector<int> heights;
    heights.reserve(out->size());
    for (const auto& l : *out) heights.push_back((int)(l.y1 - l.y0));
    std::sort(heights.begin(), heights.end());
    const int medH = std::max(1, heights[heights.size() / 2]);
    const double bucketH = std::max(4.0, medH * 0.6);
    std::sort(out->begin(), out->end(), [bucketH](const OcrLine& a, const OcrLine& b) {
        const int ba = (int)std::floor(((a.y0 + a.y1) * 0.5) / bucketH);
        const int bb = (int)std::floor(((b.y0 + b.y1) * 0.5) / bucketH);
        if (ba != bb) return ba < bb;
        return a.x0 < b.x0;
    });

    // 逐行辨識（見 RunRecBatch 開頭的說明：paddlex 其實也是逐圖 resize）。
    {
        std::string e1;
        if (!impl_->RunRecBatch(bgra, w, h, stride, out, 0, out->size(), &e1)) {
            if (err && err->empty()) *err = e1;
            for (auto& l : *out) { l.text.clear(); l.score = 0.f; }
        }
    }
    // 空白結果沒有意義（rec 對純線條/圖示會吐空字串），直接濾掉。
    out->erase(std::remove_if(out->begin(), out->end(),
                              [](const OcrLine& l) { return l.text.empty(); }),
               out->end());

    if (elapsedMs) *elapsedMs = (int)(GetTickCount64() - t0);
    return true;
}

} // namespace capwgc
