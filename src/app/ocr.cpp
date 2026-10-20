// ============================================================================
// ocr.cpp — OCR 引擎的執行緒與訊息膠水（推論本身在 core/ocr_onnx.cpp）
// ============================================================================
//
// 這個檔案只負責三件事，刻意不碰任何模型細節：
//   1. 找模型檔在哪（exe 旁邊的 models\）
//   2. 一條工作執行緒：載入模型 → 排隊 → 推論 → PostMessage 回 UI
//   3. 請求的生命週期（id、取消、停止）
//
// 為什麼模型載入要放在工作執行緒而不是 Start() 裡：載入 + 暖機要 0.6~1.1 秒，
// 放在 UI 執行緒就是「開機時畫面卡一秒」。而使用者可能只是想錄影，一輩子
// 都不會按 OCR。
// ============================================================================

#include "ocr.h"

#include "../core/ocr_onnx.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace app {
namespace {

// 從 exe 所在目錄往上找 models\。開發時 exe 在 out\、模型在專案根目錄；
// 發佈時兩者在同一層。找不到就回空字串（呼叫端會回報「找不到模型」）。
std::wstring FindModelsDir() {
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash);

    const wchar_t* rel[] = {L"\\models", L"\\..\\models", L"\\..\\..\\models"};
    for (const wchar_t* r : rel) {
        const std::wstring p = dir + r;
        if (GetFileAttributesW((p + L"\\ppocr_det.onnx").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return p;
        }
    }
    return {};
}

bool FileExists(const std::wstring& p) {
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

OcrEngine::~OcrEngine() { Stop(); }

bool OcrEngine::Start(HWND notifyWnd, UINT msgId, DoneFn onDone, ReadyFn onReady,
                      const std::wstring& modelsDir) {
    if (worker_) return false;      // 已經啟動
    wnd_ = notifyWnd;
    msg_ = msgId;
    done_ = std::move(onDone);
    onReady_ = std::move(onReady);
    models_ = modelsDir.empty() ? FindModelsDir() : modelsDir;

    if (!lockReady_) {
        InitializeCriticalSection(&lock_);
        lockReady_ = true;
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return false;

    stop_ = 0;
    ready_ = 0;
    pendingCount_ = 0;

    worker_ = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            static_cast<OcrEngine*>(p)->WorkerMain();
            return 0;
        },
        this, 0, nullptr);
    if (!worker_) {
        CloseHandle(wake_);
        wake_ = nullptr;
        return false;
    }
    return true;
}

void OcrEngine::WorkerMain() {
    // onnxruntime.dll 是延遲載入（build.bat 的 /DELAYLOAD:onnxruntime.dll），
    // 所以「DLL 不在」不會讓整個程式起不來 —— 但延遲載入失敗是在**第一次呼叫**
    // 才炸，那時已經在推論執行緒裡了。所以在碰任何 Ort:: 東西之前先自己載一次：
    // 失敗就只是 OCR 變灰色，錄影截圖照常。
    //
    // 為什麼用 LoadLibrary 而不是 FileExists：Windows 11 的 System32 裡有一份
    // 微軟自己的 onnxruntime.dll（API 版本比我們的舊），只看檔案在不在會誤判成
    // 「有」。載進來之後由 OcrOnnx::Load 檢查版本，不相容會回一句錯誤而不是崩潰。
    //
    // 載到就刻意不 FreeLibrary：延遲載入的 stub 會沿用這個已載入的模組，
    // 免得多一次載入/卸載，也避免 DLL_PROCESS_DETACH 期間還在跑推論。
    if (!LoadLibraryW(L"onnxruntime.dll")) {
        PostReady(false, L"找不到 onnxruntime.dll（OCR 推論引擎）。"
                         L"OCR 會保持灰色，錄影與截圖不受影響");
        return;
    }

    // 光「載得到」還不夠：System32 那份舊版會讓後面的 Ort::Env 建構變成空指標
    // 解參考，那是 access violation 不是例外，會直接把行程收掉（連 log 都還來
    // 不及寫）。所以先驗 API 版本，失敗就只讓 OCR 變灰色。
    if (const char* why = capwgc::OcrRuntimeError()) {
        PostReady(false, L"OCR 推論引擎版本不相容（" +
                         std::wstring(why, why + std::strlen(why)) +
                         L"）。OCR 會保持灰色，其他功能不受影響");
        return;
    }

    // 模型檔先檢查一次，錯誤訊息才能講得具體（「找不到 ppocr_det.onnx」
    // 比「載入失敗」有用得多）。
    if (models_.empty()) {
        PostReady(false, L"找不到 models 目錄（ppocr_det.onnx / ppocr_rec.onnx）");
        return;
    }
    const std::wstring det = models_ + L"\\ppocr_det.onnx";
    const std::wstring rec = models_ + L"\\ppocr_rec.onnx";
    const std::wstring dict = models_ + L"\\ppocr_dict.txt";
    if (!FileExists(det) || !FileExists(rec) || !FileExists(dict)) {
        PostReady(false, L"models 目錄不完整（需要 ppocr_det.onnx、ppocr_rec.onnx、"
                         L"ppocr_dict.txt）");
        return;
    }

    auto engine = std::make_unique<capwgc::OcrOnnx>();
    std::string err;
    if (!engine->Load(det, rec, dict, &err)) {
        PostReady(false, L"OCR 模型載入失敗：" + std::wstring(err.begin(), err.end()));
        return;
    }
    ready_ = 1;
    PostReady(true, L"");

    for (;;) {
        WaitForSingleObject(wake_, 200);
        if (stop_) break;

        Job job;
        bool have = false;
        EnterCriticalSection(&lock_);
        if (!queue_.empty()) {
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
            have = true;
        }
        LeaveCriticalSection(&lock_);
        if (!have) continue;

        OcrResult r;
        r.id = job.id;
        std::vector<capwgc::OcrLine> lines;
        int ms = 0;
        std::string e;
        if (engine->Run(job.bgra.data(), job.w, job.h, job.w * 4, &lines, &ms, &e)) {
            r.ok = true;
            r.ms = ms;
            r.lines.reserve(lines.size());
            r.scores.reserve(lines.size());
            for (auto& l : lines) {
                r.lines.push_back(std::move(l.text));
                r.scores.push_back(l.score);
            }
        } else {
            r.ok = false;
            r.error = std::wstring(e.begin(), e.end());
        }
        InterlockedDecrement(&pendingCount_);
        PostDone(r);
    }
    ready_ = 0;
}

int OcrEngine::Request(const uint8_t* bgra, int w, int h, int stride) {
    if (!worker_ || !bgra || w <= 0 || h <= 0 || stride < w * 4) return 0;

    Job job;
    job.w = w;
    job.h = h;
    job.bgra.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        memcpy(job.bgra.data() + (size_t)y * w * 4, bgra + (size_t)y * stride,
               (size_t)w * 4);
    }

    EnterCriticalSection(&lock_);
    job.id = nextId_++;
    const int id = job.id;
    queue_.push_back(std::move(job));
    InterlockedIncrement(&pendingCount_);
    LeaveCriticalSection(&lock_);

    SetEvent(wake_);
    return id;
}

int OcrEngine::pending() const { return (int)pendingCount_; }

void OcrEngine::CancelPending() {
    if (!lockReady_) return;
    EnterCriticalSection(&lock_);
    const int n = (int)queue_.size();
    queue_.clear();
    LeaveCriticalSection(&lock_);
    if (n > 0) InterlockedExchangeAdd(&pendingCount_, -n);
}

void OcrEngine::Stop() {
    if (!worker_) {
        if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
        return;
    }
    InterlockedExchange(&stop_, 1);
    SetEvent(wake_);
    WaitForSingleObject(worker_, 5000);
    CloseHandle(worker_);
    worker_ = nullptr;
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    ready_ = 0;
}

void OcrEngine::PostDone(const OcrResult& r) {
    if (!wnd_) return;
    auto* heap = new OcrResult(r);
    if (!PostMessageW(wnd_, msg_, 1, (LPARAM)heap)) delete heap;
}

void OcrEngine::PostReady(bool ready, const std::wstring& detail) {
    if (!wnd_) return;
    auto* heap = new std::wstring(detail);
    if (!PostMessageW(wnd_, msg_, ready ? 2 : 3, (LPARAM)heap)) delete heap;
}

void OcrEngine::HandleMessage(WPARAM wParam, LPARAM lParam) {
    switch (wParam) {
    case 1: {
        std::unique_ptr<OcrResult> r(reinterpret_cast<OcrResult*>(lParam));
        if (r && done_) done_(*r);
        break;
    }
    case 2:
    case 3: {
        std::unique_ptr<std::wstring> d(reinterpret_cast<std::wstring*>(lParam));
        if (onReady_) onReady_(wParam == 2, d ? *d : std::wstring());
        break;
    }
    default:
        break;
    }
}

} // namespace app
