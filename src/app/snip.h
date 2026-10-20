// ============================================================================
// snip.h — 截圖：抓螢幕、裁切、進剪貼簿、存檔
// ============================================================================
//
// 用 GDI 的 BitBlt 而不是 WGC，理由：
//
//   WGC 是為了「連續串流」設計的（FramePool + 非同步送幀），抓一張圖要等
//   好幾幀才能確保拿到的是「現在」的畫面，程式碼量與延遲都遠大於需求。
//   BitBlt(CAPTUREBLT) 是一次同步呼叫，抓完就是那一刻的畫面 —— 截圖要的
//   正是「按下那一刻」，不是「最近的某一幀」。
//
// 這裡不排斥 DWM 的合成結果：BitBlt 抓的是桌面 DC，拿到的是合成後畫面，
// 所以分層視窗、圓角、陰影都會正常出現。
//
// 一個已知限制：受 DRM 保護的內容（某些播放器的畫面）會抓到黑塊。這是
// 系統層級的保護，不是這裡寫錯。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>

namespace app {

// 一個 32bpp top-down 的 DIB。拷貝語意（有拷貝建構），因為要跨執行緒傳。
class ScreenBitmap {
public:
    ScreenBitmap() = default;
    ~ScreenBitmap();

    ScreenBitmap(const ScreenBitmap&) = delete;
    ScreenBitmap& operator=(const ScreenBitmap&) = delete;

    ScreenBitmap(ScreenBitmap&& o) noexcept { MoveFrom(o); }
    ScreenBitmap& operator=(ScreenBitmap&& o) noexcept {
        if (this != &o) { Free(); MoveFrom(o); }
        return *this;
    }

    // 抓整個虛擬桌面。失敗回 false（err 有原因）。
    bool CaptureVirtualScreen(std::wstring* err);

    // 從這張圖裁出 rect（虛擬桌面座標）。超出邊界會自動夾到圖內；
    // 夾完為空回 false。裁出來的座標原點在 (0,0)。
    bool CropTo(const RECT& rect, ScreenBitmap* out) const;

    bool valid() const { return bits_ != nullptr; }
    int width() const { return w_; }
    int height() const { return h_; }
    void* bits() const { return bits_; }
    HDC dc() const { return dc_; }

    // 這張圖的左上角在虛擬桌面上的座標（CaptureVirtualScreen 之後才有意義）。
    const RECT& screenRect() const { return screen_; }

    // 把這張圖寫成 PNG 檔。失敗回 false。
    bool SavePng(const std::wstring& path) const;

    void Free();

private:
    void MoveFrom(ScreenBitmap& o);
    bool Alloc(int w, int h);

    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    RECT screen_{};
};

// 把點陣圖放進剪貼簿。
//
// 同時放 CF_BITMAP 與 CF_DIB 兩個格式：只放 CF_BITMAP 的話，某些程式（含
// 部分瀏覽器與 Office 版本）貼出來是糊的或直接貼不上；只放 CF_DIB 的話，
// 老程式（小畫家 6.1）找不到。兩個都放是最保險的組合。
//
// filePath 非空時**再多放一個 CF_HDROP**，讓檔案總管按 Ctrl+V 直接貼出這個
// 檔案。為什麼一定要真的有檔案：CF_HDROP 帶的是一串**檔名**，不是圖片內容
// —— 檔案總管貼上做的是「複製那個檔案」，它從不去讀剪貼簿裡的像素。所以
// 想「同時」能貼到小畫家與檔案總管，唯一的辦法是先存檔、再讓兩種格式一起
// 指過去。這也是為什麼呼叫端必須在呼叫這裡之前就寫好 PNG。
//
// 傳空字串就是純圖片（舊行為）：貼到小畫家可以，貼到檔案總管不會有反應。
//
// 注意：剪貼簿一次只能有一個擁有者，而 CF_DIB 的記憶體必須是 GMEM_MOVEABLE
// 且由系統接手 —— 這裡用 SetClipboardData 之後就不能再碰那個 handle。
bool CopyToClipboard(HWND owner, const ScreenBitmap& bmp,
                     const std::wstring& filePath, std::wstring* err);

} // namespace app
