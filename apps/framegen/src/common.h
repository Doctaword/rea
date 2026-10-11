#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

struct Size {
    UINT width = 0;
    UINT height = 0;
    bool operator==(const Size&) const = default;
};

inline Size SizeOf(const RECT& r) {
    return {static_cast<UINT>(r.right - r.left), static_cast<UINT>(r.bottom - r.top)};
}

inline void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s failed (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

inline double QpcSeconds(LONGLONG ticks) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) / freq;
}

inline LONGLONG QpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
