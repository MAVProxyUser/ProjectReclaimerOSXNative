# Project Reclaimer on Apple Silicon — natively, without Wine

Runs [Project Reclaimer](https://projectreclaimer.dev) (the community Halo 3 client built on Halo: The
Master Chief Collection's `halo3.dll`) on an Apple Silicon Mac **without Wine, CrossOver or a VM**.

`shim/build/peload` is a from-scratch Win64 PE loader that runs under Rosetta 2. It loads the Reclaimer
client and MCC's `halo3.dll` directly and implements the Windows APIs they use, including:

- **Graphics:** a Direct3D 11 → OpenGL 4.1 (OpenGL-on-Metal) renderer with a DXBC → GLSL shader
  translator, and a CPU interpreter for the GPU particle compute shaders.
- **Audio:** XAudio2 on CoreAudio.
- **Input:** XInput on GameController.framework, and Raw Input from macOS keyboard/mouse events.

## Status

**Playable:** a Guardian Slayer match runs with:
- correct rendering, lighting and shadows
- the first-person weapon, particles, muzzle flash and explosions
- the HUD, the loading screen and game audio
- controller input (Xbox, DualSense, …) and keyboard/mouse

On an M4, it reaches the match about 12 s after launch and averages ~116 fps (roughly 50–200 fps
depending on the scene). The window title shows the elapsed time, the load time and the live frame rate.

Reclaimer's own frontend menu (Halo 3 menu scene with Server Browser, Campaign, Host Game, Customize,
Settings, Credits) also renders and responds to the mouse: `RECLAIMER_MENU=1 bash shim/play.sh`.

**Online play:** joining a public internet server works, from the menu's Server Browser or directly with
`RECLAIMER_JOIN=host:port bash shim/play.sh`. The client joins the lobby (players, map vote), prepares
the next map when the server rotates, and loads into the match. Only servers running the same client
version (0.9.13) can be joined; the browser shows other versions in red.

Known gaps:
- **Starting a match from the menu** (Host Game → Start Game) hangs on the loading screen. Use the
  default `bash shim/play.sh`, which goes straight into a Guardian match.
- **Scoreboard text.** Holding Back/Tab in a match shows the scoreboard bars without names, service
  tags or scores. In the default direct match the player has no name at all; Reclaimer draws the
  scoreboard text with its own UI, which only runs under its menu (being worked on).
- **No MCC pause menu** (by design in standalone mode); F1 releases the mouse.
- **Frame-rate ceiling.** Performance is bound by OpenGL-on-Metal's per-draw-call cost.

## Requirements

- An Apple Silicon Mac with **Rosetta 2** (`softwareupdate --install-rosetta --agree-to-license`).
  Developed and tested on an M4 running macOS 26.
- Your own copy of **Halo: The Master Chief Collection** on Steam, with the Halo 3 Multiplayer files.
- **Project Reclaimer v0.9.13** — `project-reclaimer-v0.9.13.exe` from https://projectreclaimer.dev.
  It must be this exact build; the loader patches it at fixed offsets. SHA-256:
  `a393504dc32a8fd35dabb5b26ea1427fbaedabdac76b00133d49ebc44fb8efd1`
- About 25 GB of disk space for the MCC files.

No game files are included in this repository or its releases.

## Setup

1. **Get the loader.** Either:
   - download the release bundle and unpack it:
     ```bash
     tar xzf ProjectReclaimerOSXNative-*-macos.tar.gz
     ```
     Then clear the download quarantine so macOS lets the unsigned binary run:
     ```bash
     xattr -dr com.apple.quarantine ProjectReclaimerOSXNative-*-macos
     ```
   - or build from source (needs the Xcode command-line tools): `bash shim/build.sh`. This also needs
     the three VC++ runtime DLLs (`ucrtbase.dll`, `vcruntime140.dll`, `msvcp140.dll`) in `shim/crt/`;
     the release bundle contains them.
2. **Download the MCC files** natively with DepotDownloader. You log in with your own Steam account
   (QR code, or `STEAM_USER=<name>` for password + Steam Guard). The files land in
   `~/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection`.
   ```bash
   ./reclaimer-mac.sh download
   ```
   (Only `download` is needed; that script's other commands are for an older Wine-based setup.)
3. **Add the client exe.** Put `project-reclaimer-v0.9.13.exe` in `client/`, or point
   `RECLAIMER_EXE=/path/to/exe` at it.

## Launch

```bash
bash shim/play.sh
```

From a source checkout you can build and launch in one go:

```bash
bash shim/build.sh && bash shim/play.sh
```

Startup sequence:
- The window title counts up from launch (`starting... 0:07`).
- The Guardian loading screen appears.
- The match starts about 12 s in, and the title then shows `match loaded in N s` and the frame rate.
- The engine log goes to `/tmp/reclaimer_play.log`.

Quit by closing the window, or Ctrl-C in the terminal. If a launch is interrupted, clear leftovers:

```bash
pkill -9 -f build/peload
```

The first launch builds and caches the game's shaders as effects first appear (brief hitches). Later
launches pre-build them during the loading screen.

## Controls and options

- **Controller:** any controller macOS supports. Look pitch is inverted by default.
- **Keyboard + mouse:** click the window to capture them; F1 or switching windows releases them.

Options go in front of the command, e.g. `PELOAD_INVERT_PITCH=0 bash shim/play.sh`:

| Variable | Effect |
|---|---|
| `PELOAD_INVERT_PITCH=0` | normal (non-inverted) look |
| `PELOAD_AUDIO=0` | no sound |
| `PELOAD_VSYNC=1` | vsync the window (off by default) |
| `RECLAIMER_EXE=<path>` | use the client exe at this path |
| `RECLAIMER_MENU=1` | start in Reclaimer's frontend menu instead of a match |
| `RECLAIMER_JOIN=host:port` | start in the menu and join that server, e.g. `RECLAIMER_JOIN=203.0.113.7:11774` |

The window can be resized and made full screen. Developer and diagnostic switches are documented in
`CLAUDE.md` and in the source.

## Layout

| Path | Contents |
|---|---|
| `shim/src/peload.c` | PE loader, Win32/DXGI/D3D11 implementation, GL renderer, present path |
| `shim/src/dxbc2glsl.c` | DXBC (Shader Model 5) → GLSL 4.10 translator |
| `shim/src/dxbc_cpu.c` | CPU interpreter for the compute shaders (particles), run on a worker thread |
| `shim/src/xaudio.c` | XAudio2 / X3DAudio on CoreAudio |
| `shim/src/wininput.c` | keyboard/mouse → Raw Input and window messages |
| `shim/play.sh` / `shim/build.sh` | launcher / build script |
| `re-tools/` | reverse-engineering helpers (angr decompile, xrefs) |
| `shim/docs/` | research notes |
