// framegen: window-capture frame generation and upscaling prototype.
// See docs/design.md for the pipeline and pacing model.

#include "capture_dda.h"
#include "pipeline.h"
#include "presenter.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::wstring windowTitle;  // substring; empty = foreground window after a countdown
    std::wstring shaderPath;
    std::wstring profilePath;
    // Tuning, from the profile file then the command line.
    int multiplier = 2;
    bool scale = false;
    bool vsync = false;
    double maxIntervalMs = 100.0;
    PipelineSettings pipeline;
};

// Tuning settings shared by profiles.ini keys and --<key> options.
// Flags (scale, vsync) take "1"/"0" in profiles and no value on the command line.
bool IsFlag(const std::wstring& key) { return key == L"scale" || key == L"vsync"; }

bool ApplySetting(Options& o, const std::wstring& key, const wchar_t* value) {
    if (key == L"multiplier") o.multiplier = std::clamp(_wtoi(value), 2, 4);
    else if (key == L"scale") o.scale = _wtoi(value) != 0;
    else if (key == L"vsync") o.vsync = _wtoi(value) != 0;
    else if (key == L"sharpen") o.pipeline.sharpness = std::clamp(static_cast<float>(_wtof(value)), 0.f, 1.f);
    else if (key == L"search") o.pipeline.searchRadius = std::clamp(_wtoi(value), 1, 16);
    else if (key == L"occlusion")
        o.pipeline.occlusionThreshold = std::clamp(static_cast<float>(_wtof(value)), 0.f, 1.f);
    else if (key == L"max-interval-ms") o.maxIntervalMs = std::max(1.0, _wtof(value));
    else return false;
    return true;
}

constexpr const wchar_t* kSettingKeys[] = {L"multiplier", L"scale",     L"vsync",          L"sharpen",
                                           L"search",     L"occlusion", L"max-interval-ms"};

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
        L"  --profiles <path>      per-game profiles (default: profiles.ini next to the exe)\n"
        L"  --shaders <path>       shader file (default: shaders\\framegen.hlsl next to the exe)\n"
        L"Settings resolve as: built-in defaults, then [default] and [<game>.exe] in the\n"
        L"profile file, then command-line options.\n"
        L"hotkeys: Ctrl+Alt+F toggles generation, Ctrl+Alt+Q quits\n");
}

std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    return dir;
}

using CliSettings = std::vector<std::pair<std::wstring, std::wstring>>;

// Paths and the target are read now; tuning is kept aside so it can be
// applied after the target's profile.
bool ParseOptions(int argc, wchar_t** argv, Options& o, CliSettings& tuning) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* {
            if (i + 1 >= argc) throw std::runtime_error("missing value after an option");
            return argv[++i];
        };
        if (a.rfind(L"--", 0) != 0) return false;
        std::wstring key = a.substr(2);
        if (key == L"window") o.windowTitle = next();
        else if (key == L"shaders") o.shaderPath = next();
        else if (key == L"profiles") o.profilePath = next();
        else if (IsFlag(key)) tuning.emplace_back(key, L"1");
        else if (std::find(std::begin(kSettingKeys), std::end(kSettingKeys), key) != std::end(kSettingKeys))
            tuning.emplace_back(key, next());
        else return false;
    }
    if (o.shaderPath.empty()) o.shaderPath = ExeDir() + L"shaders\\framegen.hlsl";
    if (o.profilePath.empty()) o.profilePath = ExeDir() + L"profiles.ini";
    return true;
}

std::wstring ProcessExeName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name.assign(path, size);
        name = name.substr(name.find_last_of(L"\\/") + 1);
    }
    CloseHandle(process);
    return name;
}

// Applies [section] of the profile file. Returns true when the section exists.
bool ApplyProfileSection(Options& o, const std::wstring& section) {
    wchar_t probe[2];
    if (GetPrivateProfileSectionW(section.c_str(), probe, 2, o.profilePath.c_str()) == 0) return false;
    for (const wchar_t* key : kSettingKeys) {
        wchar_t value[64];
        if (GetPrivateProfileStringW(section.c_str(), key, L"", value, 64, o.profilePath.c_str()) > 0)
            ApplySetting(o, key, value);
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

int Run(Options opt, const CliSettings& cli) {
    HWND target = FindTarget(opt.windowTitle);
    if (!target) throw std::runtime_error("target window not found");
    wchar_t title[256];
    GetWindowTextW(target, title, 256);
    const std::wstring exe = ProcessExeName(target);
    std::wprintf(L"target: \"%ls\" (%ls)\n", title, exe.empty() ? L"unknown process" : exe.c_str());

    ApplyProfileSection(opt, L"default");
    if (!exe.empty() && ApplyProfileSection(opt, Lower(exe)))
        std::wprintf(L"profile: [%ls] from %ls\n", Lower(exe).c_str(), opt.profilePath.c_str());
    for (const auto& [key, value] : cli) ApplySetting(opt, key, value.c_str());
    std::wprintf(L"settings: multiplier %d, search %d, occlusion %.2f, sharpen %.2f, scale %ls, vsync %ls\n",
                 opt.multiplier, opt.pipeline.searchRadius, opt.pipeline.occlusionThreshold,
                 opt.pipeline.sharpness, opt.scale ? L"on" : L"off", opt.vsync ? L"on" : L"off");

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
    CliSettings cli;
    try {
        if (!ParseOptions(argc, argv, opt, cli)) {
            PrintUsage();
            return 2;
        }
        return Run(opt, cli);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "framegen: %s\n", e.what());
        return 1;
    }
}
