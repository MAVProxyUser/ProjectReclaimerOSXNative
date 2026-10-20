# Reach Renaissance on the loader

Reach Renaissance (github.com/NETF1ix/reach-renaissance-releases) is a fan client for Halo: Reach built on MCC's
`haloreach.dll`, much as Project Reclaimer is built on `halo3.dll`. It offers multiplayer, Firefight and Campaign with the
Xbox 360 menus. This page covers running it on the Mac under our loader. Checked with Reach Renaissance 0.1.17 and MCC's
Steam build of Halo: Reach (`haloreach.dll` SHA-256 `738dd2d2...0894`, the one build 0.1.17 accepts).

## Status (2026-10-20)

- **A local match renders and runs.** Tested: Slayer on Sword Base, first person with HUD, through the D3D11 -> GL path and
  the native GL helper. It ran ~2,500 draws a frame at the engine's 60 fps cap and lasted full 60 s / 150 s sessions.
- Sound: XAudio2 on CoreAudio, FMOD banks from MCC, and the Waves DLLs as guests (the same path as Halo 3).
- Not yet: the menus (WebView2), the public server list and updates (HTTPS through Windows' Schannel), the launcher
  (`Reach Renaissance.exe`), keyboard / mouse / controller in a match (untested), and host-probe's own HLSL (D3DCompile:
  gamma pass, emblems, loading dots).

## Setup

1. MCC's Halo: Reach files: `DEPOTS="1064225 1064226" ./reclaimer-mac.sh download` (13.2 GB with your Steam login;
   1064225 is Campaign + Firefight, 1064226 is Multiplayer, 3.2 GB shared). They land in the prefix's MCC folder next to
   Halo 3.
2. Reach Renaissance: unzip `ReachRenaissance-v<version>.zip` into
   `~/Library/Application Support/ReclaimerMac/prefix/drive_c`, which makes `C:\ReachRenaissance`.
3. `bash shim/build.sh`, then `bash shim/reach.sh` (Slayer on Sword Base), or
   `bash shim/reach.sh <map> <game variant> <minutes>`. The log goes to `/tmp/reach_play.log`.

## How it is built (static look at 0.1.17)

| Program | Role |
|---|---|
| `Reach Renaissance.exe` | Launcher and updater. It starts `bin/menu-overlay.exe` with the page, game and master arguments. |
| `bin/menu-overlay.exe` | The menus: `themes/classic/*.html/js` in WebView2, as a transparent window over the game. It starts host-probe for each match from an args template (below). |
| `bin/host-probe.exe` | The game. It runs `LoadLibraryW(haloreach.dll)` (after `SetDllDirectoryW`), `D3D11CreateDevice`, `CreateGameEngine` and the host callbacks. It applies ~85 in-memory engine patches (16 -> 32+ players, Elite armor ...). |
| `bin/rr-browser.exe`, `bin/rr-master.exe` | Server list, LAN discovery, rendezvous, social / chat; a master server. |

- All five are Rust built for `x86_64-pc-windows-gnu`, so their CRT is `msvcrt.dll`. That is the loader's msvcrt layer
  (FIXES.md section 1).
- host-probe's first argument is its run folder.
- The launcher's match template is
  `--launch-block launch_block_1.bin --map {map} --mode {mode} --time-limit {time} --exit-after-game`.
- `{mode}` is a stock game variant file name without `.bin` (`slayer_054` ...). A plain `slayer` panics with "cannot read
  game variant ...\slayer.bin".
- The menu runtime uses `--launch-block lb_m_c_1.bin --menu-scene all ...`.

## Loader fixes it needed (all in FIXES.md, "reach-port")

- **Section 1:** msvcrt.dll for windows-gnu programs; `__C_specific_handler` from kernel32; LoadLibraryW + SetDllDirectoryW.
- **Section 2:**
  - Guest thread stacks of the requested size: an overflow past the 512 KB default corrupted a loader thread.
  - ExitThread / FreeLibraryAndExitThread.
- **Section 3:** VirtualAlloc honours PAGE_EXECUTE_* (host-probe's jump stubs).
- **Section 7:** the game window from a PeekMessageW pump; CallWindowProcW.
- **Section 9:** LoadLibrary("xaudio2_9redist.dll") returns our XAudio2. The genuine DLL from MCC\Binaries\Win64 recursed
  until the stack ran out.
- **Section 10:** OMSetRenderTargetsAndUnorderedAccessViews. Without it the frame never reached the back buffer and the
  window stayed black.

Unlike halo3.dll, haloreach.dll needed no crash guards so far. Its imports match halo3.dll's closely: 336 of 406 handled,
and the rest are logging stubs it does not call in a match. The Halo 3 patches and hooks are keyed on a module named
"halo3" and do not touch haloreach.dll, which loads at the same base (0x180000000).

## Next

1. Input in a match: our Raw Input / XInput go to host-probe's window.
2. A real `D3DCompile`, either Microsoft's `d3dcompiler_47.dll` from MCC as a guest (115 msvcrt imports, 14 not in ucrtbase,
   plus CryptoAPI hashes) or the few host-probe shaders by hand.
3. The menus: WebView2 on WKWebView. The page side is only `chrome.webview.postMessage`, `message` events and shared buffers.
4. HTTPS: Schannel (secur32) and the certificate store (crypt32) on Security.framework.
5. The launcher: Toolhelp32 process listing, `reg query`, and PowerShell (firewall rule) child processes.
