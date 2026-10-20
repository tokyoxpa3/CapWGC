// ============================================================================
// snip.cpp — 截圖的實作
// ============================================================================

#include "snip.h"

#include "i18n.h"

// DROPFILES（CF_HDROP 的內容格式）住在 shlobj_core.h，光靠 windows.h 拿不到。
// 這個標頭是必要的，不是為了 Shell API —— 只是為了那個結構定義。
#include <shlobj_core.h>
#include <wincodec.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace app {
namespace {

// 32bpp top-down DIB 的配置。負高度 = top-down：像素在記憶體裡的順序跟
// 螢幕一致（左上開始），做裁切與之後送 OCR 時不必到處記「這張是不是反的」。
void FillBmi(BITMAPINFO* bi, int w, int h) {
    std::memset(bi, 0, sizeof(BITMAPINFO));
    bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth = w;
    bi->bmiHeader.biHeight = -h;
    bi->bmiHeader.biPlanes = 1;
    bi->bmiHeader.biBitCount = 32;
    bi->bmiHeader.biCompression = BI_RGB;
}

} // namespace

ScreenBitmap::~ScreenBitmap() { Free(); }

void ScreenBitmap::Free() {
    if (dc_) {
        if (old_) SelectObject(dc_, old_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    old_ = nullptr;
    bits_ = nullptr;
    w_ = h_ = 0;
}

void ScreenBitmap::MoveFrom(ScreenBitmap& o) {
    dc_ = o.dc_;         bmp_ = o.bmp_;      old_ = o.old_;
    bits_ = o.bits_;     w_ = o.w_;          h_ = o.h_;
    screen_ = o.screen_;
    o.dc_ = nullptr;     o.bmp_ = nullptr;   o.old_ = nullptr;
    o.bits_ = nullptr;   o.w_ = o.h_ = 0;
    o.screen_ = RECT{};
}

bool ScreenBitmap::Alloc(int w, int h) {
    Free();
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bi{};
    FillBmi(&bi, w, h);

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    dc_ = CreateCompatibleDC(screen);
    if (dc_) {
        bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    }
    ReleaseDC(nullptr, screen);

    if (!dc_ || !bmp_ || !bits_) {
        Free();
        return false;
    }
    old_ = SelectObject(dc_, bmp_);
    w_ = w;
    h_ = h;
    return true;
}

bool ScreenBitmap::CaptureVirtualScreen(std::wstring* err) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        if (err) *err = i18n::T(L"讀不到虛擬桌面大小");
        return false;
    }
    if (!Alloc(vw, vh)) {
        if (err) *err = i18n::T(L"配置截圖記憶體失敗");
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (!screen) {
        if (err) *err = i18n::T(L"取得桌面 DC 失敗");
        Free();
        return false;
    }
    // CAPTUREBLT：把分層視窗（含我們自己的遮罩）也一起抓進來。這正是我們
    // 要的 —— 按下熱鍵時先把「變暗前的原圖」抓一份，之後覆蓋層才疊上去。
    const BOOL ok = BitBlt(dc_, 0, 0, vw, vh, screen, vx, vy, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        if (err) *err = i18n::T(L"BitBlt 抓取桌面失敗");
        Free();
        return false;
    }
    screen_ = RECT{vx, vy, vx + vw, vy + vh};
    return true;
}

bool ScreenBitmap::CropTo(const RECT& rect, ScreenBitmap* out) const {
    if (!out || !valid()) return false;

    // 夾到這張圖的範圍。使用者框選時可能拖到螢幕外（多螢幕的交界），
    // 不夾的話 memcpy 會讀到別人的記憶體。
    RECT r = rect;
    r.left = (int)__max(r.left, screen_.left);
    r.top = (int)__max(r.top, screen_.top);
    r.right = (int)__min(r.right, screen_.right);
    r.bottom = (int)__min(r.bottom, screen_.bottom);
    const int cw = r.right - r.left;
    const int ch = r.bottom - r.top;
    if (cw <= 0 || ch <= 0) return false;
    if (!out->Alloc(cw, ch)) return false;

    const int sx = r.left - screen_.left;
    const int sy = r.top - screen_.top;
    const auto* src = static_cast<const BYTE*>(bits_);
    auto* dst = static_cast<BYTE*>(out->bits_);
    const size_t rowBytes = (size_t)cw * 4;

    for (int y = 0; y < ch; ++y) {
        const BYTE* s = src + ((size_t)(sy + y) * w_ + sx) * 4;
        BYTE* d = dst + (size_t)y * rowBytes;
        std::memcpy(d, s, rowBytes);
        // GDI 抓下來的像素 alpha 通道是 0（BitBlt 不填 alpha）。留著的話，
        // 貼進支援 alpha 的程式會變成一張全透明的圖。統一補成不透明。
        for (int x = 0; x < cw; ++x) d[x * 4 + 3] = 0xFF;
    }
    out->screen_ = r;
    return true;
}

bool ScreenBitmap::SavePng(const std::wstring& path) const {
    if (!valid()) return false;

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w_, (UINT)h_);

    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        // PNG 支援 BGRA，正常不會走到這裡；保險起見還是記著。
        if (SUCCEEDED(hr) && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) {
        const UINT stride = (UINT)w_ * 4;
        const UINT size = stride * (UINT)h_;
        hr = frame->WritePixels((UINT)h_, stride, size, static_cast<BYTE*>(bits_));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (SUCCEEDED(hr)) hr = stream->Commit(STGC_DEFAULT);
    ok = SUCCEEDED(hr);

    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

// ============================================================================
// 剪貼簿
// ============================================================================

bool CopyToClipboard(HWND owner, const ScreenBitmap& bmp,
                     const std::wstring& filePath, std::wstring* err) {
    if (!bmp.valid()) {
        if (err) *err = i18n::T(L"沒有可複製的圖");
        return false;
    }

    const int w = bmp.width();
    const int h = bmp.height();
    const size_t stride = (size_t)w * 4;
    const size_t pixBytes = stride * (size_t)h;
    const size_t total = sizeof(BITMAPINFOHEADER) + pixBytes;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem) {
        if (err) *err = i18n::T(L"配置剪貼簿記憶體失敗");
        return false;
    }
    auto* p = static_cast<BYTE*>(GlobalLock(mem));
    if (!p) {
        GlobalFree(mem);
        if (err) *err = i18n::T(L"鎖定剪貼簿記憶體失敗");
        return false;
    }

    // CF_DIB 是「BITMAPINFOHEADER + 像素」。這裡用**正高度**（bottom-up）：
    // top-down 是 Windows 後期才普遍支援的寫法，寫成 bottom-up 才能連老程式
    // （小畫家、部分安裝精靈）都貼得上。所以逐列反向拷貝。
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = w;
    bih.biHeight = h;          // 正 = bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)pixBytes;
    std::memcpy(p, &bih, sizeof(bih));

    const auto* src = static_cast<const BYTE*>(bmp.bits());
    BYTE* dst = p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; ++y) {
        const BYTE* s = src + (size_t)y * stride;
        BYTE* d = dst + (size_t)(h - 1 - y) * stride;
        std::memcpy(d, s, stride);
    }
    GlobalUnlock(mem);

    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        if (err) *err = i18n::T(L"開啟剪貼簿失敗（可能有其他程式正在使用）");
        return false;
    }
    EmptyClipboard();

    // CF_DIB：主要格式，多數程式都吃這個。
    if (!SetClipboardData(CF_DIB, mem)) {
        // 失敗時所有權還在我們手上，要自己釋放 —— SetClipboardData 成功時
        // 系統接手，失敗時系統不管，忘了 Free 就是每次失敗漏一塊記憶體。
        GlobalFree(mem);
        CloseClipboard();
        if (err) *err = i18n::T(L"寫入剪貼簿失敗");
        return false;
    }
    mem = nullptr;   // 所有權已轉移

    // CF_BITMAP：相容用。有些老程式只認這個。失敗不致命（CF_DIB 已經在了）。
    BITMAPINFO bi{};
    FillBmi(&bi, w, h);
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP hbmp = screen ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)
                          : nullptr;
    if (screen) ReleaseDC(nullptr, screen);
    if (hbmp && bits) {
        std::memcpy(bits, bmp.bits(), pixBytes);
        if (!SetClipboardData(CF_BITMAP, hbmp)) {
            DeleteObject(hbmp);
        }
    } else if (hbmp) {
        DeleteObject(hbmp);
    }

    // CF_HDROP：讓檔案總管按 Ctrl+V 貼出這個檔案。
    //
    // 這裡要放的是**檔名**，不是圖片資料 —— 檔案總管貼上做的是「複製那個
    // 檔案」，它不會去讀 CF_DIB。所以圖片格式與檔案格式必須同時存在，才能
    // 一邊貼到小畫家、一邊貼到資料夾。
    //
    // DROPFILES 的結構是：標頭 + 一串以 '\0' 分隔的檔名 + 最後再一個 '\0'；
    // 若 fWide 為 0 就是 ANSI。這裡一律用 fWide = TRUE（UTF-16），因為路徑
    // 可能含有中文 —— ANSI 版在非系統語系的機器上會變亂碼。
    if (!filePath.empty()) {
        // 單一檔案的位元組數：標頭 + 路徑 + 結尾的兩個 '\0'
        const size_t chars = filePath.size() + 2;   // 路徑本身 + '\0' + 清單結束的 '\0'
        const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (drop) {
            auto* df = static_cast<DROPFILES*>(GlobalLock(drop));
            if (df) {
                df->pFiles = sizeof(DROPFILES);   // 檔名清單從標頭之後開始
                df->pt = POINT{0, 0};
                df->fNC = FALSE;
                df->fWide = TRUE;                 // UTF-16，中文路徑才不會亂碼
                wchar_t* names = reinterpret_cast<wchar_t*>(
                    reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
                std::memcpy(names, filePath.c_str(),
                            filePath.size() * sizeof(wchar_t));
                names[filePath.size()] = L'\0';      // 這一項結束
                names[filePath.size() + 1] = L'\0';  // 整份清單結束
                GlobalUnlock(drop);
                // 失敗不致命：圖片格式已經在了，只是檔案總管貼不上。
                if (!SetClipboardData(CF_HDROP, drop)) {
                    GlobalFree(drop);
                }
            } else {
                GlobalFree(drop);
            }
        }
    }

    CloseClipboard();
    return true;
}

} // namespace app
