# framegen design

framegen is an original, open implementation of window-capture frame
generation and upscaling for Windows games, in the same family of tools as
Lossless Scaling. It does not contain, derive from, or load any Lossless
Scaling code, shaders, or models. Everything here is built from public
techniques (block-matching motion estimation, motion-compensated frame
interpolation, Lanczos resampling) and public Windows APIs.

## Goal

Make a game that renders at a low or unstable frame rate look smoother by
showing synthesized in-between frames, and optionally display a low-resolution
game window scaled up to the full monitor. It works on any game that runs in
a window or borderless window, with no game-side integration.

## Pipeline

```
 game window ──► capture ──► motion estimation ──► frame synthesis ──► output pass ──► overlay present
                (DXGI DDA)   (prev↔cur, pyramid)   (t = k/M warps)     (resample,      (topmost,
                                                                         sharpen)       click-through)
```

All stages run on one D3D11 device created on the adapter that drives the
monitor showing the game, so no frame ever leaves the GPU.

### 1. Capture

The prototype uses DXGI Desktop Duplication (DDA):

- Finds the DXGI output for the monitor that contains the target window.
- Each `AcquireNextFrame` returns the desktop image; frames whose dirty
  rectangles do not intersect the game's client area, and pointer-only
  updates (`LastPresentTime == 0`), are ignored, so the frame cadence we see
  is the game's present cadence.
- The game's client rectangle is copied out with `CopySubresourceRegion`
  into a two-slot ring (previous, current).
- `LastPresentTime` (a QPC timestamp) feeds the source frame interval
  estimate used for pacing.

The overlay window is excluded from capture with
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` so the tool never captures
its own output. This needs Windows 10 2004 or later.

Planned: a Windows.Graphics.Capture (WGC) backend. WGC captures one window
even when it is partly covered, reports per-window frames directly, and is
the better default for most games; DDA stays as the low-overhead fallback.

### 2. Motion estimation

Hierarchical, candidate-based block matching, run on luma:

1. Each new frame is reduced once to a luma pyramid (half resolution, then
   halving down to about 1/32). The current frame's pyramid becomes the
   previous frame's pyramid on the next frame, so each frame is reduced once.
2. At the coarsest level an exhaustive ±4 pixel search finds one vector per
   8×8 block.
3. Each finer level tests a small candidate set: the upsampled parent vector,
   the parent vectors of the four neighbouring blocks, and zero motion. The
   best candidate is refined with a ±1 pixel search. The cost is SAD plus a
   smoothness penalty against the parent vector and a small bias towards zero
   motion, which keeps static HUD elements stable.
4. Vectors are stored in normalized UV units so every level and the warp pass
   share one convention.

This runs twice per frame, previous→current and current→previous, so the
synthesis pass has a forward and a backward field to choose from.

Planned tuning: a temporal candidate (last frame's field), per-block
confidence, and finer block grids for fast camera pans.

### 3. Frame synthesis

For each generated frame at time `t = k/M` between the previous and current
frame (`M` is the multiplier, 2 to 4):

- The forward field is sampled at the in-between position with one
  fixed-point iteration (`v = F(p - t·v)`) to approximate flow at time `t`.
  This gives samples `prev(p - t·v)` and `cur(p + (1-t)·v)`.
- The backward field gives a second pair the same way.
- Each pair's photometric disagreement measures how plausible it is. The two
  blends are mixed by inverse error.
- Where both pairs disagree strongly (occlusion, disocclusion, transparency),
  the pass stops blending and takes the single sample from the nearer source
  frame, which trades a small judder for no ghosting.

### 4. Output pass

One compute pass resamples the frame (real or generated) to the overlay size:

- Lanczos-2 resampling when scaling, aspect-preserving with black bars.
- Optional sharpening: an unsharp mask clamped to the local 3×3 min/max so it
  cannot ring.

The pass writes an RGBA8 texture that is copied to the swap chain back buffer.

### 5. Present and pacing

The overlay is a borderless, topmost, non-activating, click-through window
(`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
WS_EX_NOREDIRECTIONBITMAP`) placed over the game's client area, or over the
whole monitor in scale mode. It presents through a DirectComposition flip
swap chain, which keeps the window click-through and the game focused.

Interpolation needs the next real frame, so output runs one source frame
behind. When real frame `N` arrives at time `T_N`, with a smoothed source
interval `Δ`:

| present | content            | at                |
| ------- | ------------------ | ----------------- |
| 1       | synth(N-1, N, 1/M) | now               |
| …       | synth(N-1, N, k/M) | `T_N + (k-1)·Δ/M` |
| M       | real N             | `T_N + (M-1)·Δ/M` |

Waits use a high-resolution waitable timer plus a short spin. Generation is
skipped when `Δ` is above 100 ms (static scenes, menus, loading screens), so
those show real frames only.

Added latency is roughly `(M-1)/M` of a source frame plus GPU time, the same
trade every interpolation-based frame generator makes.

## Known limits of the prototype

- Mouse input in scale mode is not remapped. The overlay is click-through,
  so clicks land on the real, unscaled window underneath. Controller games
  and frame generation without scaling are unaffected.
- Exclusive-fullscreen games cannot be captured; use borderless or windowed.
- HDR outputs are captured as 8-bit; the HDR path needs a 16-bit float
  capture format and an scRGB swap chain.
- Only one monitor and one target window at a time.

## Tuning per game

Settings that matter most per game, exposed as command-line options now and
as saved profiles later:

- `--multiplier`: 2× for 30–45 fps sources, 3× or 4× only when the base frame
  rate is stable.
- `--search`: coarse search radius; raise it for fast first-person camera
  turns, lower it for slow third-person or strategy games to reduce artifacts.
- `--sharpen`: helps when scaling up from low render resolutions.
- `--occlusion`: how much disagreement stops blending; lower values ghost
  less but judder more around moving edges.
- `--max-interval-ms`: stops generation when the source frame rate drops into
  slideshow territory, where artifacts dominate.

## Next steps

1. Build and verify on the target machine: capture cadence, overlay
   click-through, capture exclusion, and pacing with PresentMon.
2. WGC capture backend.
3. Temporal candidate, confidence-weighted vector smoothing, and scene-cut
   detection that skips synthesis across hard cuts.
4. Per-game profiles keyed by executable name.
5. Evaluate integrating AMD FidelityFX FSR 3 frame interpolation (MIT
   licensed) as an alternative synthesis backend, and FSR 1 (MIT) for
   spatial upscaling, with attribution under `licenses/`.
