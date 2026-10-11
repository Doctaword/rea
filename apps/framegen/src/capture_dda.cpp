#include "capture_dda.h"

namespace {

bool Intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

}  // namespace

DdaCapture::DdaCapture(HMONITOR monitor) {
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !output_ && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor == monitor) {
                Check(output.As(&output_), "IDXGIOutput1");
                monitorRect_ = desc.DesktopCoordinates;
                break;
            }
        }
        if (!output_) adapter.Reset();
    }
    if (!output_) throw std::runtime_error("no DXGI output drives the target window's monitor");

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef FRAMEGEN_DEBUG_LAYER
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                            ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_),
          "D3D11CreateDevice");
    Reset();
}

void DdaCapture::Reset() {
    dupl_.Reset();
    Check(output_->DuplicateOutput(device_.Get(), &dupl_), "DuplicateOutput");
}

bool DdaCapture::RegionChanged(const RECT& local, UINT metadataSize) {
    if (metadataSize == 0) return false;
    if (metadata_.size() < metadataSize) metadata_.resize(metadataSize);

    UINT used = 0;
    auto* moves = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data());
    if (FAILED(dupl_->GetFrameMoveRects(metadataSize, moves, &used))) return true;
    for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i)
        if (Intersects(moves[i].DestinationRect, local)) return true;

    auto* dirty = reinterpret_cast<RECT*>(metadata_.data());
    if (FAILED(dupl_->GetFrameDirtyRects(metadataSize, dirty, &used))) return true;
    for (UINT i = 0; i < used / sizeof(RECT); ++i)
        if (Intersects(dirty[i], local)) return true;
    return false;
}

DdaCapture::Result DdaCapture::Acquire(const RECT& region, ID3D11Texture2D* dst, UINT timeoutMs,
                                       LONGLONG& presentQpc) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return Result::Lost;
    Check(hr, "AcquireNextFrame");

    Result result = Result::NoChange;
    RECT local = region;
    OffsetRect(&local, -monitorRect_.left, -monitorRect_.top);

    // LastPresentTime == 0 means only the pointer changed.
    if (info.LastPresentTime.QuadPart != 0 && RegionChanged(local, info.TotalMetadataBufferSize)) {
        ComPtr<ID3D11Texture2D> desktop;
        Check(resource.As(&desktop), "desktop texture");
        D3D11_BOX box{static_cast<UINT>(local.left), static_cast<UINT>(local.top), 0,
                      static_cast<UINT>(local.right), static_cast<UINT>(local.bottom), 1};
        context_->CopySubresourceRegion(dst, 0, 0, 0, 0, desktop.Get(), 0, &box);
        presentQpc = info.LastPresentTime.QuadPart;
        result = Result::NewFrame;
    }
    dupl_->ReleaseFrame();
    return result;
}
