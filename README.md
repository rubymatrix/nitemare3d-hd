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

You can build on Linux and Windows too. See [docs/TECHNICAL.md](docs/TECHNICAL.md) for how.

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
little. When you get hurt, the edges of the screen flash red. The HUD art
is in `assets/hud/`.

## Lighting

The castle is lit by its own lamps, chandeliers, candles and fires: their
light falls on the walls, floor, ceiling and monsters around them, walls
cast shadows, and candle and fire light flickers. Away from any light it
gets dark, with a faint glow around you.

## Play together (co-op)

Two players can play the campaign together over a network. One player
hosts, the other joins:

```bash
./build/nitemare3d --data original/data --host                # player 1
./build/nitemare3d --data original/data --join 192.168.1.20   # player 2: the host's address
```

The host starts a game from the menu as usual, and player 2's game loads
the same level and follows along from level to level. Both need the same
game files. To play over the internet, the host forwards TCP port 5333 on
their router to their computer, and player 2 joins with the host's public
address (`--host 6000` and `--join ADDRESS:6000` use another port).

Each player has their own health, ammo and powers and picks up what they
walk over; keys and weapons are shared, so what one finds, both have.
Monsters go after whoever is nearer. A player who dies comes back at the
start of the level and the game goes on. You see the other player as the
man in the green lab coat. A panel in the top corner shows the connection,
the ping, and whether you're waiting for the host. Co-op is for the
desktop versions only, and the other player and the panel need the new
renderer (not `--classic`).

## Useful options

| Option | What it does |
| --- | --- |
| `--fullscreen` | start in full screen |
| `--classic` | the original 320×200 look |
| `--soundfont FILE.sf2` | better music, using any General MIDI SoundFont |
| `--fm` | the 1994 Sound Blaster music sound |
| `--hud classic` | the original status bar instead of the new gothic HUD |
| `--bob 0` | turn off the view and weapon bob while walking |
| `--roll 0` | turn off the view leaning when you strafe |
| `--light off` | turn off the lighting (everything evenly lit, as in 1994) |
| `--ambient 0.5` | make unlit areas brighter (0 to 1, default 0.35) |
| `--hit-flash off` | no red flash around the screen edges when you get hurt |

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
