# AGENT.md — how to work on this repository

Project Reclaimer (a fan Halo 3 multiplayer client built on Halo: The Master Chief Collection's game files) on Apple Silicon,
without Wine. Two halves:

- **The native app** (what the user plays since 2026-11-06). Reclaimer 0.10+ ships its own `Project Reclaimer.app` (arm64, Metal;
  it recompiles MCC's `halo3.dll` itself). Ours is `nhook.dylib` (`shim/native/`), injected by `shim/native.sh`: the live stats page
  (http://127.0.0.1:8765), overlay, clip recording, forensic archive, host tools.
- **The loader** (0.9.x Windows clients). A from-scratch Win64 PE loader under Rosetta (`shim/src/peload.c` and friends): Win32,
  D3D11 -> OpenGL, XAudio2, input. It also runs Reach Renaissance (`shim/reach.sh`) and Project Gravemind (`shim/gravemind.sh`).

## Read first (kept current)

| file | what it holds |
|---|---|
| `CLAUDE.md` | the working notes: every subsystem, env switch, test and pitfall. Long; search it rather than read it whole |
| `shim/docs/BACKLOG.md` | what is open and what is done; the user's requests in their own words |
| `shim/docs/FIXES.md` | every fix: function, why, logic, the switch that restores the old behaviour. Section 0 = install / usage |
| `shim/docs/native-client.md` | the native app: install, identity, running, its insides (addresses), our features, tests, app updates |
| `README.md` | the user-facing guide (native app first, then the loader) |
| `shim/docs/porting.md`, `patch-addresses.md` | a new 0.9.x client release: signatures, `re-tools/patchscan.py` / `sigport.py` |

Topic docs in `shim/docs/` (each says which client it covers): forensics, server-status, client-transport, game-options, mods,
recording, teleport, map-boundaries, net-authority, halo3-objects, halo3-event-feed, halo3-variant-traits, halo3-engine-globals,
h3-map-geometry, reclaimer-voice-mute, workshop-helper, PERF-PLAN, games, reach-renaissance, gravemind, megalo, mod-files-security,
security-rcon, rcon-capabilities, easyanticheat-eos, native-transport-gate. Kept as written (history): `shim/STATUS.md`,
`shim/docs/menu-render-status.md`, `shim/docs/RE-findings-menu-reentrancy.md`.

## The user's rules

1. **Every fix** gets an entry in `shim/docs/FIXES.md` in the same commit. **Every behaviour change** gets an env switch that brings
   the old behaviour back, named in that entry. New knowledge goes into `CLAUDE.md` or the topic doc in the same commit; keep
   `BACKLOG.md` current.
2. **Nothing personal in git or in a release**: no macOS user name, Steam account, player nickname or community id. Never print or
   copy the identity seed (`profile.json`). Never type a password: Steam sign-ins are the user's, in their own terminal. Grep the
   staged diff before committing; `make-release.sh` refuses home paths.
3. **Never sweep process memory**: scanning a game process's VM regions kernel-panicked the Mac. Find engine structures
   statically (`re-tools/`), then read the known addresses.
4. **Never disturb the user's game**:
   - Don't kill their processes or capture their mouse.
   - Match processes with anchored patterns (`pgrep -f '^/Applications/Project Reclaimer.app'`, `'^[^ ]*build/peload'`).
   - One test client at a time unless the user allows a second. Check `ps` before and after every launch.
   - Tests use test profiles: native `ReclaimerMac/native/test-v2` (`shim/native.sh` without `NATIVE_REAL=1`) and
     `test-second`, with `NATIVE_LOG=<your own file>`.
   - Tests use our local-only server (`shim/tools/native-dedicated.sh`, `shim/dedicated.sh`), never public servers.
   - Runs with an old-behaviour switch get a hidden window.
5. **Helpers go in the repo**: `re-tools/` for RE, `shim/tools/` for runs and tests, committed. Look for an existing one first.
6. **Downloads need the user's OK**, client updates included (ask before `client-update.sh`). Commit freely. Push, merge into
   `main` and release only when asked.
7. **Don't edit a shell script while it runs**: bash reads it as it goes.
8. **What the user cares about**:
   - They play with a controller and judge by feel: input-to-screen delay, window fps, stutter.
   - Explain jargon in anything written for them.
   - No bounding-sphere or ring stand-ins in the overlay.

## Build, run, test

```bash
bash shim/native/build.sh                                  # nhook.dylib (atomic: a running app keeps its copy)
bash shim/native.sh [--join-server host:port] [--background]   # the app + the dylib, test profile; NATIVE_REAL=1 = the user's folder
bash shim/build.sh && bash shim/play.sh                    # the loader
```

- **Offline tests** (`shim/tools/`): nhook-selftest, results-feed-test, spawn-test, overlay-feat-test, forensic-oggtest,
  clipboard-test, d2d-test, rec-test, callcost.
- **Live tests** (local server, test profiles), native: native-fire-test, native-aim-test, `native-host-test.sh walls|tpnext`,
  native-voice-test.
- **Live tests**, loader: quiet-run, port-verify, spots-verify.

## Reverse engineering

- **PE**: `re-tools/refs.py` (func / strref / globref), `decomp.py` (angr, `setup.sh` first), `slotdis.py`, `ctxcalls.py`,
  `memwrites.py`.
- **The native app** (arm64 Mach-O): `re-tools/machoref.py` (strref / dis / func / calls / dataref).
- **Map files**: `re-tools/h3bsp.py` and the other `h3*.py`.
- **Checking a hit**: a unique signature hit can still be the wrong function. Validate it by what it does, and mark findings
  CONFIRMED or guessed in the docs.

## Release

```bash
bash shim/tools/make-release.sh <client version> v<N>      # from a clean, committed main
gh release create reclaimer-<version>-v<N> --target main --title "..." --notes-file <notes> dist/<name>.tar.gz dist/<name>.tar.gz.sha256
```

Write the notes like the previous release's (`gh release view`): what is new for players, bundle contents, quick start, SHA-256.
