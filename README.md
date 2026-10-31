# Nitemare-3D, rebuilt for modern computers

![Nitemare-3D title screen, redrawn at high resolution](docs/images/splash.jpg)

Nitemare-3D is a 1994 first-person horror game for Windows 3.1 by David P.
Gray. This project makes the original game run on today's Mac, Linux and
Windows computers, and in a web browser.

It is still the original game. A program reads the old Windows 3.1
executable and converts it into modern code, so the levels, enemies,
weapons, menus and save games all behave exactly as they did in 1994. On top
of that you get:

- any screen size, including widescreen and ultrawide
- smooth 60 fps movement (the original ran at 25)
- sharper textures
- WASD and mouse look
- gamepad support
- full-screen play in a browser

**Play it in your browser:** https://nitemare3d.everydaypreview.com

## You need your own copy of the game

This repository has none of the game's files. You need the original Windows
installer, `win3_Nite3d3x.zip`. The game files and the converted game code
are made from that file on your own computer and never go into this
repository.

## Build and play (Mac)

1. Install the tools:

   ```bash
   brew install sdl2 cmake ghidra openjdk@21
   ```

2. Unpack the game files from your installer:

   ```bash
   python3 tools/extract_installer.py /path/to/win3_Nite3d3x.zip original
   ```

3. Convert the game:

   ```bash
   GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec JAVA_HOME=/opt/homebrew/opt/openjdk@21 tools/build_gen.sh
   ```

4. Build it:

   ```bash
   cmake -S . -B build && cmake --build build -j
   ```

5. Play:

   ```bash
   ./build/nitemare3d --data original/data
   ```

## Build and play (Windows)

1. Install Visual Studio 2022 or later with "Desktop development with C++",
   plus Python 3, a JDK 21 and Ghidra 11.3 or later.

2. In a Developer PowerShell, unpack the game files and convert the game:

   ```powershell
   python tools\extract_installer.py C:\path\to\win3_Nite3d3x.zip original
   $env:GHIDRA_INSTALL_DIR = 'C:\path\to\ghidra_11.3.2_PUBLIC'
   $env:JAVA_HOME = 'C:\path\to\jdk-21'
   powershell -ExecutionPolicy Bypass -File tools\build_gen.ps1
   ```

3. Build it. SDL2 is downloaded for you:

   ```powershell
   cmake --preset windows
   cmake --build --preset windows
   ```

4. Play:

   ```powershell
   build\Release\nitemare3d.exe --data original\data
   ```

On Windows the game draws with Direct3D 12 and starts in borderless full
screen at your desktop resolution, like the Mac app (`--windowed` for a
window). For the redrawn title screen and main menu, put an `hdpack` folder
made by `re/upscale/pack.py` next to `nitemare3d.exe`.

You can build on Linux too. See [docs/TECHNICAL.md](docs/TECHNICAL.md) for how.

## Controls

| Key | Action |
| --- | --- |
| W A S D or arrow keys | move and turn |
| Mouse | look around |
| Left click or Ctrl | fire |
| Right click or Space | open doors, use things |
| 1–4 | pick a weapon |
| Esc | menu |
| Tab | status |
| F9 / F10 | maps |
| F5 / F4 | quick save / quick load |
| Alt+Enter | full screen |

Gamepads work as well: left stick moves, right stick turns, right trigger
fires, A uses.

## New HUD, bob and lean

With the view at full size, the 3D view fills the whole screen and the old
status bar is replaced by a gothic HUD: your face in a pointed frame, a
health bar, the ammo for your weapon, and your keys and items. Walking
bobs the view and weapon like in Quake, and strafing leans the view a
little. The HUD art is in `assets/hud/`.

## Lighting

The castle is lit by its own lamps, chandeliers, candles and fires: their
light falls on the walls, floor, ceiling and monsters around them, walls
cast shadows, and candle and fire light flickers. Away from any light it
gets dark, with a faint glow around you.

## Useful options

| Option | What it does |
| --- | --- |
| `--fullscreen` | start in full screen |
| `--windowed` | start in a window (the Windows build starts in full screen) |
| `--classic` | the original 320×200 look |
| `--soundfont FILE.sf2` | better music, using any General MIDI SoundFont |
| `--fm` | the 1994 Sound Blaster music sound |
| `--hud classic` | the original status bar instead of the new gothic HUD |
| `--bob 0` | turn off the view and weapon bob while walking |
| `--roll 0` | turn off the view leaning when you strafe |
| `--no-vsync` | don't wait for the screen refresh (good with G-SYNC / FreeSync screens) |
| `--light off` | turn off the lighting (everything evenly lit, as in 1994) |
| `--ambient 0.5` | make unlit areas brighter (0 to 1, default 0.35) |

Put an option at the end of the play command, for example
`./build/nitemare3d --data original/data --classic`. Every other option is
listed in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## How it works

[docs/TECHNICAL.md](docs/TECHNICAL.md) explains how the conversion works,
along with the web build, music rendering, the renderer and the debugging
tools.

## Legal

Nitemare-3D is © David P. Gray / Gray Design Associates. This repository
contains only new code and some open-source libraries (Nuked OPL3,
TinySoundFont, stb_vorbis). It does not include the game itself. The picture at the top is an AI
redraw of the game's original title screen. Don't
commit game files or converted game code to it.
