#pragma once

#include "common.h"

#include <dcomp.h>

// Topmost, click-through, non-activating overlay presenting through a
// DirectComposition flip-model swap chain. Excluded from screen capture so
// Desktop Duplication never sees its own output.
class Presenter {
public:
    Presenter(ID3D11Device* device, bool vsync);
    ~Presenter();

    // Moves the overlay to `rect` (virtual-desktop coordinates) and resizes
    // the swap chain to match.
    void Place(const RECT& rect);
    void Show(bool visible);

    ID3D11Texture2D* BackBuffer();
    void Present();

    HWND Window() const { return hwnd_; }

private:
    ID3D11Device* device_;
    bool vsync_;
    HWND hwnd_ = nullptr;
    Size size_{};
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
};
