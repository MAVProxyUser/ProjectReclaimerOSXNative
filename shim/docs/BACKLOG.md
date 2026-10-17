# Backlog — requests from the user, and their status

Kept current as work lands. "Done" items name the commit that delivered them. Open items are in the
order they will be worked; frame rate is deliberately last (user, 2026-10-16).

## Open

| # | Item | Status / notes |
|---|---|---|
| 1 | **Unexplained crashes** | (a) v0.9.18 SIGTRAP during a direct Guardian match the user played (earlier session; MiniDumpWriteDump now logs the exception, module+offset and return addresses). (b) A SIGSEGV at startup: guest code (Rust thread-local, exe+0x1d6455d) ran on a thread with no TLS (`gs:[0x58]`=0), seen once. Loader threads are now named, so the next [CRASH] line says which thread. Both possibly the same root cause as the thread-id bug fixed in 55811fa; watch for recurrence. |
| 2 | **Minimized window stalls the game** | A client minimized during loading stops presenting ("presentation callback stalled") and never reaches gameplay. Hit by test runs with PELOAD_MINIMIZE; would also hit a player who minimizes during a load. |
| 3 | **Class-selection screen input** (servers with abilities, e.g. "UNSC vs Covenant") | The user reported the class picker while input seemed unavailable. Re-check that controller and keyboard drive that screen. |
| 4 | **Controller in menu-joined matches** | "Input was nil" reported once, probably focus on the input-less Target window. Re-check. |
| 5 | **Audio desync after a teleporter sound** (reported on v0.9.13) | Not re-tested. Candidate fixes since then: XAudio2 stopped-voice flush (36c3c98) and the thread-id/std::mutex fix (55811fa). |
| 6 | **Menu music / voice chat output** | The client logs "Menu music output unavailable: No music playback device" and "Voice audio unavailable": the client's own audio output (not XAudio2) finds no device under the loader. |
| 7 | **Hitbox overlay polish** | Elite heads use the default helmet's collision shape (permutation 0), not each player's helmet; optional: teammates vs enemies in different colours (needs the team field in the biped data). |
| 8 | **45 s startup watchdog — root cause** | The signature patch `watchdog` skips the deadline check. On Windows something clears that deadline in time; find what, so the patch can go. |
| 9 | **v0.9.13 regression check** | Loader changes for v0.9.18 (module handles, TLS, CreateDirectoryW, pipes) and today's fixes are not verified on v0.9.13; it is only a fallback now. |
| 10 | **Seams at polygon joins on outer map geometry** (e.g. Valhalla rock walls) | User: low priority. |
| 11 | **Headless `dedicated` server** | Configures, binds, sets up Online, loads the map, then its worker idles at "waiting for the native game client" (halo3 main loop never runs). target.sh covers the "second player to shoot at" need. |
| 12 | **Release bundle** | GitHub release v0.1.0 predates all of this; a new bundle (loader + scripts, no game files) needs the user's go-ahead. |
| 13 | **Frame rate** (last) | Measured and planned in shim/docs/PERF-PLAN.md: ~77% of each frame is the GL driver path (Apple GL-on-Metal, translated under Rosetta, 1.5–4.7× native per call). Done: redundant glUseProgram cache (4afd0a9). Small wins left (state-change counts, PrimitiveRestartIndex cache); big step proposed: native arm64 GL helper process (up to ~2×). |

## Done (2026-10-16)

- Join public servers (remote join proven, then real BTB on v0.9.18) — 36c3c98, a574219
- Server Browser list, lobby names, map vote render (GL thunk xmm6-15) — f125b76
- Scoreboard text (foreground/activation rules); play.sh defaults to the menu — 5d425e5
- Second player to shoot at: shim/target.sh (input-less client hosting hidden Valhalla) — f4eb1e6, d858a8b
- Bigger window (largest 16:9 that fits the screen) — 1367a68
- Reclaimer client v0.9.18 support (signature patches, Bink/Waves DLL support) — a574219
- Game audio after joining a match in progress (user heard full audio in FKK BTB)
- Hitbox overlay: bounding spheres (f73252e), then the real hitboxes — each object's collision model posed on
  its live node matrices (76a80c8); F2 toggles it in-game (c1f12f8). Verified on Valhalla vehicles, a Guardian
  Spartan and a 60-player BTB.
- Title fps no longer claims more than the screen can show (loading screens spin at ~1000 presents/s) — f73252e
- Intermittent hang at match start: GetCurrentThreadId returned 0x1000 on every thread, which disabled every
  msvcp140 std::mutex — 55811fa
- Water renders: the water shader pair never linked (varying locations >= 14 on Apple GL) — 434db9b (user confirmed)
