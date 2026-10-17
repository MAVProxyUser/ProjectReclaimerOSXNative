# Project Reclaimer on Apple Silicon — natively, without Wine

Runs [Project Reclaimer](https://projectreclaimer.dev) (the community Halo 3 client built on Halo: The
Master Chief Collection's `halo3.dll`) on an Apple Silicon Mac **without Wine, CrossOver or a VM**.

`shim/build/peload` is a from-scratch Win64 PE loader that runs under Rosetta 2. It loads the Reclaimer
client and MCC's `halo3.dll` directly and implements the Windows APIs they use, including:

- **Graphics:** a Direct3D 11 → OpenGL 4.1 (OpenGL-on-Metal) renderer with a DXBC → GLSL shader
  translator, and a CPU interpreter for the GPU particle compute shaders.
- **Audio:** XAudio2 on CoreAudio for the game, and a WASAPI output device on CoreAudio for the client's own
  menu music and voice chat.
- **Input:** XInput on GameController.framework, and Raw Input from macOS keyboard/mouse events.

## Status

**Playable**, starting from Reclaimer's own menu (Halo 3 menu scene with Server Browser, Campaign, Host
Game, Customize, Settings, Credits):
- **Host Game → Start Game** hosts a local Guardian Slayer match.
- **Server Browser** lists the public servers; double-click one (or select it and JOIN GAME) to join.
  The client joins the lobby (players, map vote), prepares the next map when the server rotates, and
  loads into the match. Only servers running the same client version can be joined; the
  browser shows other versions in red.
- In a match: correct rendering, lighting and shadows; the first-person weapon, particles, muzzle flash
  and explosions; the HUD and scoreboard (hold Tab or Back); the loading screen and game audio;
  controller (Xbox, DualSense, …) and keyboard/mouse.

On an M4 the menu is up about 17 s after launch, a hosted match loads in ~6 s, and gameplay averages
~85–120 fps depending on the map and scene. The window title shows the elapsed time, the load time and
the live frame rate (never more than the screen's refresh rate; when the engine runs faster, e.g. on
loading screens, its own rate is shown beside it).

Known gaps:
- **No MCC pause menu** (by design in standalone mode); F1 releases the mouse.
- **Frame-rate ceiling.** Performance is bound by OpenGL-on-Metal's per-draw-call cost.

## Requirements

- An Apple Silicon Mac with **Rosetta 2** (`softwareupdate --install-rosetta --agree-to-license`).
  Developed and tested on an M4 running macOS 26.
- Your own copy of **Halo: The Master Chief Collection** on Steam, with the Halo 3 Multiplayer files.
- **Project Reclaimer client v0.9.18** — `project-reclaimer-client-v0.9.18.exe` from
  https://github.com/ProjectReclaimer/project-reclaimer-releases/releases (SHA-256
  `a8d3a37ad27e31abc09d009a94210470976ecefb34908a9fb8e1a245f4924910`). The loader finds the few client
  code paths it patches by byte signature, so other builds usually work too; v0.9.13 is also tested.
  Servers and players are almost all on the newest release.
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
3. **Add the client exe.** Put `project-reclaimer-client-v0.9.18.exe` in `client/`, or point
   `RECLAIMER_EXE=/path/to/exe` at it. The first launch of a new client version builds its Forge asset
   library (about 2.5 minutes, "BUILDING ASSET LIBRARY"); later launches reach the menu in ~20 s.

## Copy to another Mac with a USB stick

If one Mac is already set up, you can move everything to another Apple Silicon Mac on a USB stick: no Steam login
or DepotDownloader download on the second Mac. The stick needs about 31 GB free (25 GB with `--minimal`); format it
APFS or Mac OS Extended so it can hold files over 4 GB.

On the Mac that already plays (from the repository folder):

```bash
bash shim/tools/make-usb.sh /Volumes/RECLAIMER
```

That copies, into `ReclaimerMac/` on the stick, the loader and launch scripts, the VC++ runtime DLLs, your Reclaimer
client exe, your MCC files (the whole MCC folder; `--minimal` copies only what Project Reclaimer reads) and the
client's caches and fonts (the Forge asset cache, so the second Mac skips the 2.5-minute first-launch build). Add
`--with-profile` to bring your Reclaimer profile (nickname and identity) along -- then don't play on both Macs at once.

On the other Mac, plug in the stick and run:

```bash
bash /Volumes/RECLAIMER/ReclaimerMac/install.sh
```

It checks for Rosetta 2, then copies the game files to the same place `./reclaimer-mac.sh download` would
(`~/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master
Chief Collection`) and the loader to `~/ReclaimerMac` (pass another folder as an argument to change that). Then:

```bash
cd ~/ReclaimerMac && bash shim/play.sh
```

Both scripts are resumable -- run them again if a copy was interrupted. The files are your own game copy, for your
own Macs; don't share the stick.

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
- After a black screen, the Halo 3 menu scene fades in; the menu is ready about 17 s in (`menu loaded
  in N s` in the title).
- Host Game → Start Game for a local match, or Server Browser to join a server.
- The engine log goes to `/tmp/reclaimer_play.log`.

To jump straight into a server, give its address: `RECLAIMER_JOIN=host:port bash shim/play.sh`.
`bash shim/tools/servers.sh` lists the busiest servers (64-127 players) with their addresses and the mods
each one needs; [shim/docs/mods.md](shim/docs/mods.md) explains mods, including downloading Workshop-only
ones with DepotDownloader.

`RECLAIMER_DIRECT=1 bash shim/play.sh` skips the menu and goes straight into a local Guardian match
(about 12 s). In that mode Reclaimer's in-game UI doesn't run, so the scoreboard has no names or scores.

Quit by closing the window, or Ctrl-C in the terminal. If a launch is interrupted, clear leftovers:

```bash
pkill -9 -f build/peload
```

The first launch builds and caches the game's shaders as effects first appear (brief hitches). Later
launches pre-build them during the loading screen.

### A second player to shoot at (testing)

`shim/target.sh` starts a second copy of the client as a stationary target: it hosts a hidden Online game
on Valhalla by itself (scripted clicks in its own window) and stands in the match. It has its own profile
("Target"), no controller, keyboard, mouse or audio, no window and no Dock icon (it runs in the background,
capped at 30 fps since nobody sees its frames), so it doesn't touch your input. Then join it:

```bash
bash shim/target.sh
RECLAIMER_JOIN=127.0.0.1:11774 bash shim/play.sh
```

The target is in its match about 50 s after starting; `TARGET_MAP=<name>` picks another map, and
`bash shim/target.sh stop` stops it (`play.sh` leaves a running target alone). `TARGET_SHOW=1` gives it a
window (minimized in the Dock; add `TARGET_MINIMIZE=0` to keep it on screen).

Any client can run that way: `PELOAD_HIDE_WINDOW=1` (no window, no Dock icon) and `PELOAD_MAX_FPS=<n>` (frame
cap; 30 by default with a hidden window, `0` = uncapped).

### A dedicated server on this Mac

Project Reclaimer's headless dedicated server runs too (no window, in the background):

```bash
bash shim/dedicated.sh                                  # start it
RECLAIMER_JOIN=127.0.0.1:49190 bash shim/play.sh        # join it
bash shim/dedicated.sh status                           # what it is playing, players, ready
bash shim/dedicated.sh stop                             # stop it
```

The first start creates the server folder (`C:\ReclaimerTestServer` in the loader's prefix; `DED_FOLDER='C:\<name>'`
for another) with a local-only `dedicated.toml`: Valhalla Slayer on UDP 49190, hidden, not listed on any master, no
UPnP. The server waits in its lobby until a player joins, then starts the match. If the very first join after
starting it times out, join again. To host for other people, edit `dedicated.toml` (its options are documented in
`dedicated.toml.example` next to it) -- that makes the server public, so only do it on purpose. `play.sh` leaves a
running server alone. Logs: `/tmp/reclaimer_dedicated.log` and `/tmp/reclaimer_dedicated_<pid>.log`.

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
| `RECLAIMER_JOIN=host:port` | join that server as soon as the menu is up, e.g. `RECLAIMER_JOIN=203.0.113.7:11774` |
| `RECLAIMER_DIRECT=1` | skip the menu: straight into a local Guardian match (no scoreboard text) |
| `PELOAD_HITBOXES=1` | draw the real hitboxes (each player's and vehicle's collision model, which is what shots hit) posed on their live skeletons, through walls: players red with the head yellow, vehicles blue. `=spheres` draws bounding spheres instead; `=all` adds spheres for weapons, equipment and projectiles. **F2** toggles the overlay during play (any launch) |
| `PELOAD_HB_TRACERS=1` | projectile tracers: grenades/rockets/plasma as trails, bullets as lines from muzzle to impact; **F3** toggles |
| `PELOAD_HB_INFO=1` | player info above each player: shield and health %, held weapon, grenades, active camo, in a vehicle; **F4** toggles |
| `PELOAD_HB_ITEMS=1` | weapons and equipment lying on the map, by name; **F5** toggles |
| `PELOAD_HB_TEAMS=enemies` | show only enemies (`friends`: only teammates). Hitboxes are red/yellow for enemies, blue for teammates; **F6** cycles all / enemies / friends |
| `PELOAD_STATS_HTTP=<port>` | live game stats page; on by default at http://127.0.0.1:8765/, `=0` turns it off (details below) |
| **F8** | capture: saves the frame, its depth buffer and the camera position to `~/reclaimer_cap_*` (for reporting rendering glitches at a spot; the overlay is not in it) |
| `PELOAD_HB_VISIBLE=1` | visible only: hitboxes and labels only for objects you can actually see, not behind walls (a small copy of the game's depth buffer, read back every frame); **F9** toggles |
| `PELOAD_HB_TEXT_PX=7` | overlay label height in window pixels (Text − / + on the stats page) |
| `PELOAD_D3DGL_INVARIANT=1` | experimental: vertex positions computed bit-identically in every pass; try it if you see seams/speckles along polygon edges (e.g. Valhalla's rock walls) |
| `PELOAD_ALLOW_TELEPORT=1` | teleport, **in games you host** (Host Game from the menu): the stats page's player details get **Teleport next to them** (you land beside that player, facing them, at a spot inside the map), F7 jumps to the opposite side of the map. It uses the game's own teleport on the host, between frames, so every player sees the move. A client can't move itself (the host puts it straight back -- tested); in a private game where the host also runs this loader, set `PELOAD_TELEPORT_HOST=<host ip>:<its stats port>` on the client and its button asks the host to move you |
| `PELOAD_H3="name=value,..."` | set Halo 3 engine settings, e.g. `render_debug_depth_render=1`, `camera_fov_scale=1.2` — all 72 are listed in `shim/docs/halo3-engine-globals.md` |

### Live game stats over HTTP

`play.sh` serves what the game knows about the match on http://127.0.0.1:8765/, updated 10 times a second
(`PELOAD_STATS_HTTP=<port>` for another port, `=0` off; `PELOAD_STATS_ADDR=<ip>` to listen on another address, e.g. to
watch from another device). From top to bottom the page shows:

- **In-game overlay** buttons -- the same switches as the function keys: Hitboxes (F2), Tracers (F3), Player info (F4),
  Items (F5), Text − / + (label size), Show all / enemies / friends (F6), Visible only (F9), Teleport (F7, only with
  `PELOAD_ALLOW_TELEPORT`) and Capture frame (F8).
- **Connected** -- everyone in the game's network session, also in the lobby before a match starts, in alphabetical
  order. Players who are talking light up green. The speaker beside each name mutes or unmutes their voice (a red cross
  = muted); **Mute all** / **Unmute all** do everyone. This is the client's own voice mute (the same list as its
  scoreboard Mute buttons): it only affects what you hear and lasts until the game closes.
- **Game rules** -- the running game's options (rounds, respawn, map, powerups, score and points) and every trait set
  with what it changes: everyone, after spawning, each powerup, and the mode's roles (leader, flag carrier, infected,
  last survivor, ...).
- **Players** -- team, shield and health, weapon, deaths, shots and accuracy, grenades, vehicle, speed and active
  traits (where they differ from the defaults, with powerup and respawn-trait countdowns). 🔊 marks a talker, 🔇 a
  muted player, "(you)" the local player (from Halo 3's own player mapping, so also right in third person and vehicles).
  Shots count every projectile (each shotgun pellet is one); a hit is a shot that ended inside another player's body
  bounds -- an estimate.
- **Death heatmap** -- where players died (blue = few, red = many) over a top-down picture of the map and a grey
  "where players walk" layer, with everyone's live position. Deaths for this match or all matches on that map (kept in
  `~/Library/Application Support/ReclaimerMac/heatmaps/`). The map pictures are made once from the game's own `.map`
  files (`bash shim/tools/maptop.sh`; `play.sh` runs it in the background when they are missing; needs python3 with
  numpy and Pillow).
- **Player details** (click a player in the table) -- under the heatmap: a big Mute / Unmute button, **Teleport next to
  them** (when you host the game and started it with `PELOAD_ALLOW_TELEPORT=1`), camo, all weapons,
  grenades, vehicle and seat, deaths, current and longest life, distance, top speed, shots, hits, accuracy, active
  traits, facing and position; their deaths are ringed on the heatmap.

Machine-readable endpoints:

- `/stats.json` -- the latest snapshot: `players` (name, slot, team, you, alive, health, shield, camo, weapon(s),
  grenades, vehicle, seat, pos, speed, facing, deaths, life, longest_life, distance, top_speed, shots, hits, accuracy,
  traits, effects, voice, muted, talking), `session` (the connected list), `variant` (options and trait sets),
  `overlay` (the switches), `vehicles`, `projectiles` in flight, `map`, `in_match`, match `time`
- `/events` -- the same JSON as a stream (Server-Sent Events): `curl -N http://127.0.0.1:8765/events`
- `/heat.json` -- the heatmap data (visited cells, deaths); `/maptop/<map>.png|json` -- the map picture and its world
  bounds
- `POST /control?do=<hitboxes|tracers|info|items|visible|teams|textup|textdown|teleport|capture|mute|unmute|muteall|
  unmuteall>[&voice=<id>]` with an `X-Reclaimer-Control: 1` header (other web pages can't send it);
  `PELOAD_STATS_READONLY=1` turns the controls off

It reads the game's own memory (object and player tables, the network session, the game variant) -- the information
Halo 3 sends to every client -- and works with the overlay off. Health and shield are fractions (1.0 = full; shields
can go above 1 when overcharged). On public servers this is information other players don't have; it is meant for
your own and your dedicated server's matches.

### Frame rate in big games

Big Team Battle with 60 players runs at about 15-25 fps: the game issues 3,000-4,700 draw calls a frame and each one
costs about 9 µs in the loader plus Apple's OpenGL driver (both run translated under Rosetta). The client's own
**Settings → Graphics → Advanced graphics** cuts draw calls: **Shadows** (each shadow caster draws again into the
shadow maps), Level of detail, Draw distance, Effects and Decals. The window title shows draws per frame next to the
frame rate. A 60 Hz display caps what reaches the screen; the title shows the engine's own rate beside it.

The window opens at the largest 16:9 size that fits your screen (`PELOAD_WIN_SIZE=1280x720` picks a size), can be
resized, and can go full screen (green button or Ctrl-Cmd-F). Developer and diagnostic switches are documented in
`CLAUDE.md` and in the source.

## Layout

| Path | Contents |
|---|---|
| `shim/src/peload.c` | PE loader, Win32/DXGI/D3D11 implementation, GL renderer, present path |
| `shim/src/dxbc2glsl.c` | DXBC (Shader Model 5) → GLSL 4.10 translator |
| `shim/src/dxbc_cpu.c` | CPU interpreter for the compute shaders (particles), run on a worker thread |
| `shim/src/xaudio.c` | XAudio2 / X3DAudio on CoreAudio |
| `shim/src/wininput.c` | keyboard/mouse → Raw Input and window messages |
| `shim/src/wasapi.c` | WASAPI render endpoint (the client's menu music and voice chat) on AudioQueue |
| `shim/src/hitbox.c` / `shim/src/stats.c` | overlay (hitboxes, tracers, labels, visible only, F8 capture) / live stats over HTTP |
| `shim/src/stats_page.html` | the stats page (embedded at build time; served from the file in a source checkout) |
| `shim/play.sh` / `shim/build.sh` | launcher / build script |
| `shim/target.sh` / `shim/dedicated.sh` | windowless second player / local dedicated server |
| `shim/tools/servers.sh` / `shim/tools/maptop.sh` | busiest public servers and their mods / top-down map pictures for the heatmap |
| `re-tools/` | reverse-engineering helpers (angr decompile, xrefs, `.map` reader `h3map.py`, map geometry `h3bsp.py`) |
| `shim/docs/` | research notes (index in `CLAUDE.md`); user guides: `mods.md`, `game-options.md` |
