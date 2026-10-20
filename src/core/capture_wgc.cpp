// ============================================================================
// capture_wgc.cpp
// ============================================================================

#include "capture_wgc.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace capwgc {

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// 回呼共用的狀態。用 shared_ptr 而不是讓 lambda 捕獲 Impl 的 this：
// Stop() 之後可能還有一個回呼在飛，shared_ptr 保證它拿到的是活的物件。
struct SharedState {
    std::atomic<bool> busy{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> closed{false};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> dropped{0};

    // 幀率閘門。
    //
    // MinUpdateInterval 只是「最短間隔」，WGC 不保證遵守；而且它擋不住
    // 「合成器比目標幀率快」的情況 —— 實測 1920x1080@239Hz 的桌面，
    // MinUpdateInterval(1ms) 之下 WGC 真的給到 234 fps，4 秒錄出 13 MB。
    // 那不是「更流暢」，那是 4 倍的 GPU 與 4 倍的檔案，而播放端還是 60。
    //
    // 所以這裡再過一道軟體閘門。丟掉的是重複的內容，時間軸仍然正確 ——
    // 交出去的每一幀帶的都是它自己的擷取時間。判斷邏輯見 FrameGate。
    std::chrono::steady_clock::time_point lastSeen{};
    FrameGate gate;
    std::atomic<uint64_t> throttled{0};

    uint32_t lastW = 0, lastH = 0;   // 只在 busy 區內存取，不需 atomic
    WgcCapture::FrameCallback cb;
};

struct WgcCapture::Impl {
    std::shared_ptr<SharedState> st = std::make_shared<SharedState>();

    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    event_token frameToken{};
    event_token closedToken{};
    bool haveFrameToken = false;
    bool haveClosedToken = false;

    std::string caps;
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}

WgcCapture::~WgcCapture() { Stop(); }

bool WgcCapture::Start(const CaptureConfig& cfg, ID3D11Device* device,
                       FrameCallback onFrame, std::string* err) {
    auto fail = [&](const char* what, HRESULT hr) {
        if (err) {
            char b[256];
            snprintf(b, sizeof(b), "%s failed 0x%08lX", what, (unsigned long)hr);
            *err = b;
        }
        return false;
    };

    if (!device) return fail("null ID3D11Device", E_INVALIDARG);
    if (!cfg.hwnd && !cfg.monitor) return fail("neither hwnd nor monitor given", E_INVALIDARG);

    // multi_threaded：pool 的 FrameArrived 可以在任何執行緒上跑。
    // 已經初始化過（RPC_E_CHANGED_MODE）不致命，WinRT 會自己 marshal。
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (hresult_error const&) {
    }

    impl_->st->cb = std::move(onFrame);
    impl_->st->stop.store(false);
    impl_->st->closed.store(false);
    impl_->st->frames.store(0);
    impl_->st->dropped.store(0);
    impl_->st->throttled.store(0);
    impl_->st->lastW = impl_->st->lastH = 0;
    impl_->st->gate.Init(cfg.maxFps);
    impl_->st->lastSeen = std::chrono::steady_clock::time_point{};

    // ---- capture item
    try {
        auto factory = get_activation_factory<GraphicsCaptureItem>();
        auto interop = factory.as<IGraphicsCaptureItemInterop>();

        HRESULT hr;
        if (cfg.hwnd) {
            // 最小化的視窗會回一個 154x25 的 item，而且 SetWindowPos 被靜默忽略。
            // 先還原，否則後面每一幀都會被尺寸檢查擋掉。
            if (IsIconic(cfg.hwnd)) {
                ShowWindow(cfg.hwnd, SW_RESTORE);
                Sleep(120);
            }
            hr = interop->CreateForWindow(cfg.hwnd, guid_of<GraphicsCaptureItem>(),
                                          put_abi(impl_->item));
        } else {
            hr = interop->CreateForMonitor(cfg.monitor, guid_of<GraphicsCaptureItem>(),
                                           put_abi(impl_->item));
        }
        if (FAILED(hr) || !impl_->item) return fail("CreateForWindow/Monitor", hr);
    } catch (hresult_error const& e) {
        return fail("CreateForWindow/Monitor (threw)", e.code().value);
    }

    const auto sz = impl_->item.Size();
    if (sz.Width == 0 || sz.Height == 0) return fail("item has zero size", E_FAIL);

    // ---- frame pool + session
    try {
        com_ptr<IDXGIDevice> dxgiDev;
        check_hresult(device->QueryInterface(guid_of<IDXGIDevice>(), dxgiDev.put_void()));
        com_ptr<IInspectable> insp;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
        IDirect3DDevice rtDevice = insp.as<IDirect3DDevice>();

        // 3 個緩衝：回呼裡同步 copy 走，2 個容易在慢幀時等到 pool 空。
        impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, sz);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
    } catch (hresult_error const& e) {
        return fail("pool/session", e.code().value);
    }
    if (!impl_->pool || !impl_->session) return fail("pool or session is null", E_FAIL);

    // ---- 能力探測（有就用，沒有就算了，但每一項都回讀驗證）
    std::string caps;

    if (auto s3 = impl_->session.try_as<IGraphicsCaptureSession3>()) {
        try {
            s3.IsBorderRequired(false);
            caps += "border=off ";
        } catch (hresult_error const& e) {
            caps += "border=rejected(" + std::to_string((unsigned long)e.code().value) + ") ";
        }
    } else {
        caps += "border=n/a ";
    }

    if (auto s2 = impl_->session.try_as<IGraphicsCaptureSession2>()) {
        try {
            s2.IsCursorCaptureEnabled(cfg.captureCursor);
            caps += std::string("cursor=") + (cfg.captureCursor ? "on " : "off ");
        } catch (hresult_error const& e) {
            caps += "cursor=rejected(" + std::to_string((unsigned long)e.code().value) + ") ";
        }
    } else {
        caps += "cursor=n/a ";
    }

    if (auto s5 = impl_->session.try_as<IGraphicsCaptureSession5>()) {
        try {
            // 24H2 起不設就是 60fps 上限。這裡設成「目標幀率的週期」而不是
            // 固定 1ms —— 1ms 會讓 239Hz 的桌面以 239fps 送幀（實測 4 秒 13MB）。
            //
            // ★ 用 floor 再減 1ms，不要用 ceil。ceil(1000/60)=17ms 比真實週期
            // 16.67ms 還長，剛好 60fps 的來源會被擋掉一半（實測 60fps 的視窗
            // 只收到 48fps）。留 1ms 餘裕讓「等速」通過，真正的上限交給下面的
            // 軟體閘門（那裡有 10% 容差）。
            const int fps = cfg.maxFps > 0 ? cfg.maxFps : 60;
            const int ms = std::max(0, (int)(1000.0 / fps - 1.0));
            s5.MinUpdateInterval(std::chrono::milliseconds(ms));
            const auto rb = s5.MinUpdateInterval();
            char b[96];
            snprintf(b, sizeof(b), "minInterval=%.2fms target=%dfps ",
                     std::chrono::duration<double, std::milli>(rb).count(), fps);
            caps += b;
        } catch (hresult_error const& e) {
            caps += "minInterval=rejected(" + std::to_string((unsigned long)e.code().value) + ") ";
        }
    } else {
        caps += "minInterval=n/a ";
    }

    impl_->caps = caps;

    // ---- 事件
    auto st = impl_->st;
    impl_->frameToken = impl_->pool.FrameArrived(
        [st](Direct3D11CaptureFramePool const& sender,
             winrt::Windows::Foundation::IInspectable const&) {
            if (st->stop.load(std::memory_order_relaxed)) return;

            // free-threaded pool 可能重入。即時錄製裡排隊只增加延遲，
            // 不增加資訊 —— 直接丟。
            if (st->busy.exchange(true, std::memory_order_acq_rel)) {
                st->dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            struct Guard {
                std::atomic<bool>* f;
                ~Guard() { f->store(false, std::memory_order_release); }
            } guard{&st->busy};

            try {
                // ★ 一定要先 TryGetNextFrame，再判斷要不要這幀。
                //
                // TryGetNextFrame 同時是「把 pool 裡的幀取走」的動作。先判斷再取，
                // 被節流丟掉的那一幀會留在 pool 裡；累積到 maxBufferSize 之後
                // WGC 就不再產生新幀，也**不再回呼 FrameArrived** —— 整個擷取永久
                // 停住，而且 dropped 是 0、沒有任何錯誤訊息。
                //
                // 實測症狀：每輪錄到 3~8 秒、throttled 剛好跳到 3 的時候畫面停止
                // 更新，captured 凍結在 177/445，行程還活著、CPU 接近 0。
                // 看起來像「WGC 掛了」或「編碼器卡死」，其實是我們自己把 pool 塞爆。
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                // 幀率閘門：目標是「平均最多 maxFps 幀」。判斷邏輯與為什麼不用
                // 硬下界，見 capture_wgc.h 的 FrameGate。
                const auto now = std::chrono::steady_clock::now();
                const bool firstFrame =
                    st->lastSeen.time_since_epoch().count() == 0;
                const int64_t sinceUs = firstFrame
                    ? 0
                    : std::chrono::duration_cast<std::chrono::microseconds>(
                          now - st->lastSeen).count();
                st->lastSeen = now;
                if (!firstFrame && !st->gate.Admit(sinceUs)) {
                    st->throttled.fetch_add(1, std::memory_order_relaxed);
                    return;      // 幀已經取走了，這裡丟掉是安全的
                }

                auto access = frame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                com_ptr<ID3D11Texture2D> tex;
                check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), tex.put_void()));

                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                if (d.Width == 0 || d.Height == 0) return;

                CapturedFrame cf;
                cf.texture = tex.get();
                cf.pts100ns = frame.SystemRelativeTime().count();
                cf.width = d.Width;
                cf.height = d.Height;
                cf.formatChanged = (st->lastW != 0) &&
                                   (st->lastW != d.Width || st->lastH != d.Height);
                st->lastW = d.Width;
                st->lastH = d.Height;

                st->frames.fetch_add(1, std::memory_order_relaxed);
                if (st->cb) st->cb(cf);
            } catch (hresult_error const& e) {
                // 絕不讓例外逃進 WGC 的回呼；但也不靜默吞掉。
                fprintf(stderr, "[wgc] frame handler hr=0x%08lX\n",
                        (unsigned long)e.code().value);
            } catch (...) {
                fprintf(stderr, "[wgc] frame handler: unknown exception\n");
            }
        });
    impl_->haveFrameToken = true;

    impl_->closedToken = impl_->item.Closed(
        [st](GraphicsCaptureItem const&, winrt::Windows::Foundation::IInspectable const&) {
            st->closed.store(true, std::memory_order_release);
        });
    impl_->haveClosedToken = true;

    // ---- go
    try {
        impl_->session.StartCapture();
    } catch (hresult_error const& e) {
        return fail("StartCapture", e.code().value);
    }

    return true;
}

void WgcCapture::Stop() {
    if (!impl_) return;
    impl_->st->stop.store(true, std::memory_order_release);

    // 先撤回呼再關。對已經關掉的 pool 動 FrameArrived 會炸。
    try {
        if (impl_->haveFrameToken && impl_->pool) {
            impl_->pool.FrameArrived(impl_->frameToken);
        }
    } catch (...) {}
    impl_->haveFrameToken = false;

    try {
        if (impl_->haveClosedToken && impl_->item) {
            impl_->item.Closed(impl_->closedToken);
        }
    } catch (...) {}
    impl_->haveClosedToken = false;

    try { if (impl_->session) impl_->session.Close(); } catch (...) {}
    try { if (impl_->pool) impl_->pool.Close(); } catch (...) {}

    impl_->session = nullptr;
    impl_->pool = nullptr;
    impl_->item = nullptr;

    // 等最後一個回呼退出（busy 可能在飛）。最多等 200ms，避免關程式時卡住。
    for (int i = 0; i < 200 && impl_->st->busy.load(); ++i) {
        Sleep(1);
    }
}

bool WgcCapture::running() const {
    return impl_ && impl_->session && !impl_->st->stop.load();
}

bool WgcCapture::closed() const {
    return impl_ && impl_->st->closed.load(std::memory_order_acquire);
}

uint64_t WgcCapture::frames() const {
    return impl_ ? impl_->st->frames.load() : 0;
}

uint64_t WgcCapture::dropped() const {
    return impl_ ? impl_->st->dropped.load() : 0;
}

uint64_t WgcCapture::throttled() const {
    return impl_ ? impl_->st->throttled.load() : 0;
}

std::string WgcCapture::caps() const {
    return impl_ ? impl_->caps : std::string();
}

} // namespace capwgc
