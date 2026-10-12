#include "presenter.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr wchar_t kClassName[] = L"framegen.overlay";
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

Presenter::Presenter(ID3D11Device* device, bool vsync) : device_(device), vsync_(vsync) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = nullptr;  // keep whatever cursor the game sets
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                            kClassName, L"framegen", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            wc.hInstance, nullptr);
    if (!hwnd_) Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(overlay)");
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE))
        throw std::runtime_error(
            "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed; Windows 10 2004 or later is required");

    ComPtr<IDXGIDevice> dxgiDevice;
    Check(device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    Check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 8;
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Check(factory->CreateSwapChainForComposition(device_, &desc, nullptr, &swapChain_),
          "CreateSwapChainForComposition");

    Check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_)), "DCompositionCreateDevice");
    Check(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_), "CreateTargetForHwnd");
    Check(dcomp_->CreateVisual(&visual_), "CreateVisual");
    Check(visual_->SetContent(swapChain_.Get()), "SetContent");
    Check(target_->SetRoot(visual_.Get()), "SetRoot");
    Check(dcomp_->Commit(), "Commit");
}

Presenter::~Presenter() {
    swapChain_.Reset();
    visual_.Reset();
    target_.Reset();
    dcomp_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
}

void Presenter::Place(const RECT& rect) {
    Size size = SizeOf(rect);
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, static_cast<int>(size.width),
                 static_cast<int>(size.height), SWP_NOACTIVATE);
    if (size == size_) return;
    size_ = size;
    Check(swapChain_->ResizeBuffers(0, size.width, size.height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
}

void Presenter::Show(bool visible) {
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

ID3D11Texture2D* Presenter::BackBuffer() {
    // Flip-model buffer 0 is always the current back buffer. The swap chain
    // keeps the buffer alive, so the reference can be dropped immediately.
    ComPtr<ID3D11Texture2D> buffer;
    Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
    return buffer.Get();
}

void Presenter::Present() {
    // Pacing is done by the caller's timer; vsync additionally aligns each
    // present to a refresh at the cost of up to one refresh of latency.
    HRESULT hr = swapChain_->Present(vsync_ ? 1 : 0, 0);
    if (hr != DXGI_STATUS_OCCLUDED) Check(hr, "Present");
}
