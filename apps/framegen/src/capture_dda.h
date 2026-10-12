#pragma once

#include "common.h"

#include <vector>

// Captures one rectangle of a monitor through DXGI Desktop Duplication.
class DdaCapture {
public:
    // Creates the D3D11 device on the adapter that owns `monitor`.
    explicit DdaCapture(HMONITOR monitor);

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    // Monitor rectangle in virtual-desktop coordinates.
    RECT MonitorRect() const { return monitorRect_; }

    enum class Result { NewFrame, NoChange, Lost };

    // Waits up to `timeoutMs` for the desktop to change inside `region`
    // (virtual-desktop coordinates). On NewFrame the region is copied into
    // `dst` (B8G8R8A8, region-sized) and `presentQpc` holds the frame's
    // present time. Frames that only move the pointer or only touch other
    // parts of the desktop return NoChange.
    Result Acquire(const RECT& region, ID3D11Texture2D* dst, UINT timeoutMs, LONGLONG& presentQpc);

    // Recreates the duplication after Lost (mode change, secure desktop, ...).
    void Reset();

private:
    bool RegionChanged(const RECT& local, UINT metadataSize);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    RECT monitorRect_{};
    std::vector<BYTE> metadata_;
};
