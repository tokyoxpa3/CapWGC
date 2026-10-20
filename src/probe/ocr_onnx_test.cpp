// ============================================================================
// ocr_onnx_test.cpp — 原生 OCR 的比對工具
// ============================================================================
//
// 用途只有一個：把同一張圖同時餵給 C++ 這條與 tools/ocr_cli.py，逐行比對輸出。
// 移植 OCR 最怕的不是「跑不起來」，而是「跑得起來但字錯了幾個」—— 那種錯
// 不會有任何錯誤訊息，只能靠並排比對抓出來。
//
// 用法：
//   ocr_onnx_test.exe <models 目錄> <圖檔.bmp> [--raw]
//
// --raw：只印純文字（一行一行），方便跟 Python 的輸出做 diff。
//
// 為什麼吃 BMP 而不是 PNG：PNG 要解壓縮（zlib）或拉 WIC，BMP 只要讀檔頭。
// 這支只是開發期工具，不必為了「支援更多格式」把依賴帶進來。
// ============================================================================

#include "../core/ocr_onnx.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 讀 24/32 bpp 的未壓縮 BMP，轉成 BGRA、top-down。
bool LoadBmp(const wchar_t* path, std::vector<uint8_t>* bgra, int* w, int* h) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;

    uint8_t hdr[54] = {};
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return false; }

    const int dataOff = *(int*)(hdr + 10);
    const int dibSize = *(int*)(hdr + 14);
    if (dibSize < 40) { fclose(f); return false; }
    const int width = *(int*)(hdr + 18);
    const int heightRaw = *(int*)(hdr + 22);
    const int bpp = *(short*)(hdr + 28);
    const int compression = *(int*)(hdr + 30);
    if (compression != 0 || (bpp != 24 && bpp != 32)) { fclose(f); return false; }

    const bool topDown = heightRaw < 0;
    const int height = topDown ? -heightRaw : heightRaw;
    const int bytesPP = bpp / 8;
    const int srcStride = ((width * bpp + 31) / 32) * 4;

    std::vector<uint8_t> src((size_t)srcStride * height);
    fseek(f, dataOff, SEEK_SET);
    if (fread(src.data(), 1, src.size(), f) != src.size()) { fclose(f); return false; }
    fclose(f);

    bgra->assign((size_t)width * height * 4, 255);
    for (int y = 0; y < height; ++y) {
        const int sy = topDown ? y : (height - 1 - y);
        const uint8_t* row = src.data() + (size_t)sy * srcStride;
        uint8_t* dst = bgra->data() + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = row + (size_t)x * bytesPP;
            dst[x * 4 + 0] = p[0];
            dst[x * 4 + 1] = p[1];
            dst[x * 4 + 2] = p[2];
            dst[x * 4 + 3] = (bytesPP == 4) ? p[3] : 255;
        }
    }
    *w = width;
    *h = height;
    return true;
}

} // namespace

namespace {

// 一律以 UTF-8 輸出。直接 printf("%ls") 會走 CRT 的 ANSI 轉換（繁中機器是
// cp950），遇到字典裡沒有的字就整個字串斷在那裡 —— 看起來像「只認出半行」，
// 其實是輸出層的問題，很容易誤導除錯方向。
std::string U8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0,
                                      nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr,
                        nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: ocr_onnx_test.exe <models dir> <image.bmp> [--raw]\n");
        return 2;
    }
    const bool raw = (argc >= 4 && wcscmp(argv[3], L"--raw") == 0);

    const std::wstring dir = argv[1];
    const std::wstring det = dir + L"\\ppocr_det.onnx";
    const std::wstring rec = dir + L"\\ppocr_rec.onnx";
    const std::wstring dict = dir + L"\\ppocr_dict.txt";

    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    if (!LoadBmp(argv[2], &bgra, &w, &h)) {
        printf("FAIL: cannot read bmp\n");
        return 1;
    }

    capwgc::OcrOnnx ocr;
    std::string err;
    const ULONGLONG t0 = GetTickCount64();
    if (!ocr.Load(det, rec, dict, &err)) {
        printf("FAIL: load: %s\n", err.c_str());
        return 1;
    }
    const int loadMs = (int)(GetTickCount64() - t0);

    std::vector<capwgc::OcrLine> lines;
    int ms = 0;
    if (!ocr.Run(bgra.data(), w, h, w * 4, &lines, &ms, &err)) {
        printf("FAIL: run: %s\n", err.c_str());
        return 1;
    }

    if (raw) {
        for (const auto& l : lines) {
            printf("%s\n", U8(l.text).c_str());
        }
    } else {
        printf("image %dx%d  load=%dms  run=%dms  lines=%zu\n", w, h, loadMs, ms,
               lines.size());
        for (const auto& l : lines) {
            printf("[%.3f] (%4.0f,%4.0f)-(%4.0f,%4.0f) len=%zu |%s|\n", l.score, l.x0,
                   l.y0, l.x1, l.y1, l.text.size(), U8(l.text).c_str());
        }
    }
    return 0;
}
