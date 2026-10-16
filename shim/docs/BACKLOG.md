# Backlog — requests from the user, and their status

Kept current as work lands (newest status first). "Done" items name the commit that delivered them.

## Open

| # | Request | Status / notes |
|---|---|---|
| 1 | **Game audio silent after joining a match from the menu (v0.9.18)** | Direct mode (`RECLAIMER_DIRECT=1`) has audio on v0.9.18 (user heard the "Slayer" announcer). A muted dump of a menu-*hosted* Valhalla match also had audio. The silent case was a menu-runtime **join of a match in progress** (FKK BTB). To reproduce: target.sh host + a muted joiner with `PELOAD_AUDIO_DUMP`. |
| 2 | **Water doesn't render** | Seen on Valhalla. Not investigated yet. |
| 3 | **Frame rate** — "any way to increase fps?" | Draw-call bound in GL-on-Metal (~3.6 µs per draw, 1200–2300 draws/frame). Ideas: GL submission thread, draw batching/instancing, fewer state changes. Not started. |
| 5 | **45 s startup watchdog — root cause** | We skip the deadline check (signature patch `watchdog`). On Windows something clears that deadline in time; find what, so the patch can go. |
| 6 | **Audio desync after a teleporter sound** (reported on v0.9.13) | Possibly the XAudio2 stopped-voice flush bug fixed in 36c3c98; not re-tested. |
| 7 | **Seams at polygon joins on outer map geometry** (e.g. Valhalla rock walls) | User: low priority. |
| 8 | **Headless `dedicated` server** | Configures, binds, sets up Online, loads the map, then its worker idles at "waiting for the native game client" (halo3 main loop never runs). target.sh covers the "second player to shoot at" need. |
| 9 | **Menu music / voice chat output** | Client logs "Menu music output unavailable: No music playback device" and "Voice audio unavailable" — the client's own audio output (not XAudio2) has no device under the shim. |
| 10 | Controller in menu-joined matches | Reported "input was nil" once; likely the user was focused on the input-less Target window (now labeled and minimized). Re-check. |
| 11 | v0.9.13 regression check | Loader changes for v0.9.18 (module handles, TLS, CreateDirectoryW, pipes) not re-verified on v0.9.13; it is only a fallback now. |
| 12 | Release bundle | GitHub release v0.1.0 predates everything below; a new bundle (loader + scripts) would need the user's go-ahead. |

## Done (2026-10-16)

- Join public servers (remote join proven, then real BTB on v0.9.18) — 36c3c98, a574219
- Server Browser list, lobby names, map vote render (GL thunk xmm6-15) — f125b76
- Scoreboard text (foreground/activation rules); play.sh defaults to the menu — 5d425e5
- Second player to shoot at: shim/target.sh (input-less client hosting hidden Valhalla) — f4eb1e6, d858a8b
- Bigger window (largest 16:9 that fits the screen) — 1367a68
- Reclaimer client v0.9.18 support (signature patches, Bink/Waves DLL support) — a574219
- Hitbox overlay `PELOAD_HITBOXES=1|all` (halo3's own debug_objects overlays are compiled out; reads the object table, draws bounding spheres with the world camera); verified in a 60-player BTB — HEAD
- Title fps no longer claims more than the screen can show (loading screens spin at ~1000 presents/s) — HEAD
