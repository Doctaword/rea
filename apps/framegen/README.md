# framegen

An original, open prototype of window-capture frame generation and upscaling
for Windows games. It captures a game window, synthesizes in-between frames
with GPU motion estimation, and shows the result in a click-through overlay
on top of the game. See [docs/design.md](docs/design.md) for the pipeline.

framegen is independent of Lossless Scaling and contains none of its code,
shaders, or models.

Status: prototype. It compiles; it has not yet been run on real hardware.

## Requirements

- Windows 10 2004 or later (for capture exclusion of the overlay)
- A D3D11 feature level 11.0 GPU
- The game in windowed or borderless windowed mode

## Build

With Visual Studio 2022 (Desktop development with C++) and CMake:

```powershell
cmake -S apps/framegen -B apps/framegen/build -G "Visual Studio 17 2022" -A x64
cmake --build apps/framegen/build --config Release
```

The executable lands in `apps/framegen/build/Release/` with a `shaders/`
folder next to it. Shaders are compiled at startup by `d3dcompiler_47.dll`,
which ships with Windows.

## Run

Start the game, then:

```powershell
.\framegen.exe --window "Elden Ring" --multiplier 2
```

Without `--window`, framegen waits five seconds and takes whichever window is
in the foreground. It prints source and output frame rates once a second.

| option                  | effect                                                |
| ----------------------- | ----------------------------------------------------- |
| `--multiplier <2-4>`    | output frames per source frame                        |
| `--scale`               | fill the monitor, scaling the game with Lanczos       |
| `--sharpen <0-1>`       | sharpening strength                                   |
| `--search <1-16>`       | coarse motion search radius; raise for fast cameras   |
| `--occlusion <0-1>`     | disagreement that stops blending (default 0.12)       |
| `--max-interval-ms <n>` | skip generation below 1000/n source fps (default 100) |
| `--vsync`               | align presents to display refresh                     |

Hotkeys: Ctrl+Alt+F toggles generation, Ctrl+Alt+Q quits.

## Per-game profiles

framegen works on any game with the `[default]` settings. To tune a game, add
a section named after its executable to `profiles.ini` next to `framegen.exe`:

```ini
[eldenring.exe]
search=8
occlusion=0.10
```

framegen detects the target's executable and prints which profile it applied.
Command-line options override the profile. See [profiles.ini](profiles.ini)
for every key.
