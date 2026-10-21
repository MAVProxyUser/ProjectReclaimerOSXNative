# Three games, one loader

The loader runs three fan clients that each load one of MCC's engine DLLs into their own process:

| game | client exe | engine | launcher |
|---|---|---|---|
| `h3` Project Reclaimer | `project-reclaimer-client-v*.exe` | `halo3.dll` | `shim/play.sh` (+ `dedicated.sh`, `target.sh`) |
| `reach` Reach Renaissance | `bin/host-probe.exe`, its launcher / `menu-overlay.exe` / `rr-browser.exe` | `haloreach.dll` | `shim/reach.sh` |
| `h2` Project Gravemind | `h2gravemind.exe` | `halo2.dll` | `shim/gravemind.sh` |

`shim/src/games.c` decides which one is running (the exe's name; else the first engine DLL loaded; `PELOAD_GAME=h3|reach|h2|other`
overrides) and prints `[game] ...` at start.

## The rule

- **Shared:** anything that makes a Windows function behave the way Windows does -- a missing API implemented, a wrong answer
  corrected (a mutex that wasn't recursive, FindFirstFileW's arguments, case-insensitive HLSL semantics). Every game benefits,
  and none can depend on the wrong behaviour, because none ran on it on Windows.
- **Per game:** anything only one game needs -- patches of its exe or engine, hooks into its code, special cases, defaults
  that differ. These ask `game_is(GAME_x)` first, so they can't fire in another game.

## What is per game today

| what | game | where |
|---|---|---|
| exe signature patches (`g_sigpatches`: ui-reentrancy, xaudio, watchdog) | h3 | peload.c `reentrancy_fix` -- searched for by byte pattern, so a chance match in another exe would patch it |
| stats page exe hooks (voice, chat, player addresses, slots, ballot) | h3 | stats.c `st_voice_install` |
| halo3.dll hooks (skinguard, vtguard, origin guard, object_delete, invisible walls, Waves prefill ...) | h3 (or an unplaced exe that loads halo3.dll) | peload.c `load_guest` |
| stats server, overlay, forensic archive, walker | h3 (they bind / start only when halo3.dll loads) | stats.c, hitbox.c, forensic.c, nav.c |
| menu grading shader recognised in D3DCompile | h3 | peload.c `i_D3DCompile` (rtgrade) |
| MCC's real HLSL compiler for D3DCompile | h2 by default (`PELOAD_REAL_D3DCOMPILE=1` anywhere, `=0` off) | peload.c `i_D3DCompile` |
| waveOutGetNumDevs reports one device | h2 (h3 / reach: 0, so they keep XAudio2 only) | peload.c `i_waveOutGetNumDevs` |

Reach Renaissance's additions (WebView2 on WKWebView, windows across loader processes, Schannel on LibreSSL) are Windows
components implemented in full, used only when a game asks for them -- shared by the rule above.

## Checking that no game broke another

`bash shim/tools/game-matrix.sh` runs every installed game briefly with the checkout's loader (input-less, no audio, test
profiles, one at a time) and prints PASS / FAIL / SKIP per game:

- h3: our local dedicated server + one client joining it: the match starts and the chat hook answers a typed line.
- reach: a local Sword Base match: frames are presented.
- h2: an offline Lockout match: Gravemind reports "game state 1" and a window frame isn't black.

Run it after any loader change and before merging.
