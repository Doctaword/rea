// framegen: window-capture frame generation and upscaling prototype.
// See docs/design.md for the pipeline and pacing model.

#include "capture_dda.h"
#include "pipeline.h"
#include "presenter.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace {

struct Options {
    std::wstring windowTitle;  // substring; empty = foreground window after a countdown
    int multiplier = 2;
    bool scale = false;
    bool vsync = false;
    double maxIntervalMs = 100.0;
    std::wstring shaderPath;
    PipelineSettings pipeline;
};

void PrintUsage() {
    std::wprintf(
        L"usage: framegen [options]\n"
        L"  --window <text>        target the visible window whose title contains <text>\n"
        L"                         (default: the foreground window after a 5 second countdown)\n"
        L"  --multiplier <2|3|4>   output frames per source frame (default 2)\n"
        L"  --scale                cover the whole monitor, scaling the game with Lanczos\n"
        L"  --sharpen <0..1>       sharpening strength (default 0)\n"
        L"  --search <1..16>       coarse motion search radius in pixels (default 4)\n"
        L"  --occlusion <0..1>     disagreement that stops blending (default 0.12)\n"
        L"  --max-interval-ms <n>  no generation when source frames are further apart (default 100)\n"
        L"  --vsync                align presents to the display refresh\n"
        L"  --shaders <path>       shader file (default: shaders\\framegen.hlsl next to the exe)\n"
        L"hotkeys: Ctrl+Alt+F toggles generation, Ctrl+Alt+Q quits\n");
}

bool ParseOptions(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* {
            if (i + 1 >= argc) throw std::runtime_error("missing value after an option");
            return argv[++i];
        };
        if (a == L"--window") o.windowTitle = next();
        else if (a == L"--multiplier") o.multiplier = std::clamp(_wtoi(next()), 2, 4);
        else if (a == L"--scale") o.scale = true;
        else if (a == L"--sharpen") o.pipeline.sharpness = std::clamp(static_cast<float>(_wtof(next())), 0.f, 1.f);
        else if (a == L"--search") o.pipeline.searchRadius = std::clamp(_wtoi(next()), 1, 16);
        else if (a == L"--occlusion")
            o.pipeline.occlusionThreshold = std::clamp(static_cast<float>(_wtof(next())), 0.f, 1.f);
        else if (a == L"--max-interval-ms") o.maxIntervalMs = std::max(1.0, _wtof(next()));
        else if (a == L"--vsync") o.vsync = true;
        else if (a == L"--shaders") o.shaderPath = next();
        else return false;
    }
    if (o.shaderPath.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir.resize(dir.find_last_of(L"\\/") + 1);
        o.shaderPath = dir + L"shaders\\framegen.hlsl";
    }
    return true;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

HWND FindTarget(const std::wstring& titlePart) {
    if (titlePart.empty()) {
        for (int s = 5; s > 0; --s) {
            std::wprintf(L"Focus the game window... %d\n", s);
            Sleep(1000);
        }
        return GetForegroundWindow();
    }
    struct Search {
        std::wstring needle;
        HWND found = nullptr;
    } search{Lower(titlePart)};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t title[512];
            if (GetWindowTextW(hwnd, title, 512) == 0) return TRUE;
            if (Lower(title).find(s->needle) == std::wstring::npos) return TRUE;
            s->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Where the game is, where the overlay goes, and where the image sits in it.
struct Geometry {
    RECT capture{};  // game client area clipped to the monitor, desktop coordinates
    RECT overlay{};  // overlay window, desktop coordinates
    RECT image{};    // image inside the overlay, overlay coordinates
    bool operator==(const Geometry& g) const {
        return EqualRect(&capture, &g.capture) && EqualRect(&overlay, &g.overlay) && EqualRect(&image, &g.image);
    }
};

bool ComputeGeometry(HWND target, const RECT& monitor, bool scale, Geometry& g) {
    RECT client;
    if (!GetClientRect(target, &client)) return false;
    POINT tl{client.left, client.top}, br{client.right, client.bottom};
    ClientToScreen(target, &tl);
    ClientToScreen(target, &br);
    RECT game{tl.x, tl.y, br.x, br.y};
    if (!IntersectRect(&g.capture, &game, &monitor)) return false;
    Size src = SizeOf(g.capture);
    if (src.width < 64 || src.height < 64) return false;

    if (!scale) {
        g.overlay = g.capture;
        g.image = {0, 0, static_cast<LONG>(src.width), static_cast<LONG>(src.height)};
        return true;
    }
    g.overlay = monitor;
    Size dst = SizeOf(monitor);
    double k = std::min(static_cast<double>(dst.width) / src.width, static_cast<double>(dst.height) / src.height);
    LONG w = static_cast<LONG>(src.width * k), h = static_cast<LONG>(src.height * k);
    LONG x = (static_cast<LONG>(dst.width) - w) / 2, y = (static_cast<LONG>(dst.height) - h) / 2;
    g.image = {x, y, x + w, y + h};
    return true;
}

class Pacer {
public:
    Pacer() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }
    ~Pacer() { if (timer_) CloseHandle(timer_); }

    void WaitUntil(LONGLONG qpc) {
        const double remaining = QpcSeconds(qpc - QpcNow());
        if (remaining > 0.0015 && timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((remaining - 0.001) * 1e7);  // 100 ns units, relative
            SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer_, INFINITE);
        }
        while (QpcNow() < qpc) YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
};

LONGLONG SecondsToQpc(double s) {
    return static_cast<LONGLONG>(s / QpcSeconds(1));
}

enum Hotkey { kToggle = 1, kQuit = 2 };

int Run(const Options& opt) {
    HWND target = FindTarget(opt.windowTitle);
    if (!target) throw std::runtime_error("target window not found");
    wchar_t title[256];
    GetWindowTextW(target, title, 256);
    std::wprintf(L"target: \"%ls\"\n", title);

    DdaCapture capture(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST));
    Pipeline pipeline(capture.Device(), capture.Context(), opt.shaderPath);
    pipeline.settings = opt.pipeline;
    Presenter presenter(capture.Device(), opt.vsync);
    Pacer pacer;

    RegisterHotKey(nullptr, kToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(nullptr, kQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    Geometry geometry{};
    bool configured = false;
    bool enabled = true;
    LONGLONG lastGeometryCheck = 0, lastSourceQpc = 0, statsStart = QpcNow();
    double interval = 0;  // smoothed source frame interval, seconds
    int sourceFrames = 0, presents = 0;
    const double maxInterval = opt.maxIntervalMs / 1000.0;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == kQuit) return 0;
            if (msg.message == WM_HOTKEY && msg.wParam == kToggle) {
                enabled = !enabled;
                std::wprintf(L"frame generation %ls\n", enabled ? L"on" : L"off");
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(target)) return 0;

        if (QpcSeconds(QpcNow() - lastGeometryCheck) > 0.25) {
            lastGeometryCheck = QpcNow();
            Geometry g{};
            bool visible = !IsIconic(target) && ComputeGeometry(target, capture.MonitorRect(), opt.scale, g);
            if (!visible) {
                presenter.Show(false);
                configured = false;
                Sleep(100);
                continue;
            }
            if (!configured || !(g == geometry)) {
                geometry = g;
                pipeline.Resize(SizeOf(g.capture), SizeOf(g.overlay), g.image);
                presenter.Place(g.overlay);
                presenter.Show(true);
                configured = true;
            }
        }
        if (!configured) continue;

        LONGLONG sourceQpc = 0;
        switch (capture.Acquire(geometry.capture, pipeline.CaptureTarget(), 8, sourceQpc)) {
        case DdaCapture::Result::Lost:
            capture.Reset();
            pipeline.ForgetHistory();
            continue;
        case DdaCapture::Result::NoChange:
            continue;
        case DdaCapture::Result::NewFrame:
            break;
        }

        const double dt = lastSourceQpc ? QpcSeconds(sourceQpc - lastSourceQpc) : 0;
        lastSourceQpc = sourceQpc;
        if (dt > 0 && dt < maxInterval) interval = interval == 0 ? dt : 0.8 * interval + 0.2 * dt;
        ++sourceFrames;

        const bool haveMotion = pipeline.CommitCapture();
        const bool generate = enabled && haveMotion && dt > 0 && dt < maxInterval && interval > 0;
        const LONGLONG start = QpcNow();
        if (generate) {
            const LONGLONG step = SecondsToQpc(interval / opt.multiplier);
            for (int k = 1; k < opt.multiplier; ++k) {
                pacer.WaitUntil(start + (k - 1) * step);
                pipeline.RenderGenerated(static_cast<float>(k) / opt.multiplier, presenter.BackBuffer());
                presenter.Present();
                ++presents;
            }
            pacer.WaitUntil(start + (opt.multiplier - 1) * step);
        }
        pipeline.RenderCurrent(presenter.BackBuffer());
        presenter.Present();
        ++presents;

        const double elapsed = QpcSeconds(QpcNow() - statsStart);
        if (elapsed >= 1.0) {
            std::wprintf(L"source %5.1f fps  output %5.1f fps  generation %ls\n", sourceFrames / elapsed,
                         presents / elapsed, enabled ? L"on" : L"off");
            sourceFrames = presents = 0;
            statsStart = QpcNow();
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Options opt;
    try {
        if (!ParseOptions(argc, argv, opt)) {
            PrintUsage();
            return 2;
        }
        return Run(opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "framegen: %s\n", e.what());
        return 1;
    }
}
