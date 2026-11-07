# The native macOS client (Project Reclaimer 0.10.0+)

Since 0.10.0 ("rcl-engine", 2026-11-06) Project Reclaimer ships native builds for Windows, macOS and Linux. The macOS build
(`Project Reclaimer.app`, bundle id `dev.projectreclaimer.game`, arm64, Metal through MoltenVK, ad hoc signed, no hardened
runtime) recompiles MCC's `halo3.dll` (and `bink2w64.dll`, Reach / Halo 2 when installed) with its own translator at first
start ("[translate] 22894 functions, 29.5 MB of code ... done in 3.9 s", cached) and links it into the process ("linked
recompiled engine, image at 0x...."). No Rosetta, no loader, no Wine: the loader (peload) is only needed for 0.9.x clients and
for the Windows build of 0.10 (it runs, see CLAUDE.md "Client 0.10.0").

## Install on this Mac (what was done 2026-11-06)

1. App: the release zip's `Project Reclaimer.app` -> `/Applications/Project Reclaimer.app` (`ditto`, signature intact;
   `lsregister -f` so `reclaimer://` join links open it). Our loader's `Reclaimer.app` (~/Applications) registers no URL
   scheme, so there is no conflict.
2. Player folder: `~/Library/Application Support/ProjectReclaimer` (the app's default; `RECLAIMER_PROFILE_DIR` overrides it,
   and a profile named by that variable is a TEST profile: stats / social services and Discord are left alone). The app's own
   migration only looks in `~/Documents/My Games/Project Reclaimer` (the 0.9 Windows folder), so the loader's profile
   (`prefix/drive_c/ReclaimerProfile`) was copied by hand: Campaign, Documents, Films, Mods, Native, appearance.json,
   settings.json and the `*-checked` markers; not Logs.
3. `mcc-folder.txt` = the macOS path of the MCC folder (no newline): the loader's
   `prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection`. A `C:\...` path there makes the
   first page ask for the folder.
4. `profile.json` converted with `shim/tools/profile-native.py convert <loader profile.json> <new profile.json>` (below). The
   loader's profile is untouched (the 0.9 client under the loader keeps using it); immutable backups in
   `ReclaimerMac/backups/` (`chflags uchg`).

Check: the app's log line `[host] community profile <name> (<id>)` with the same id as before, and no "is locked here".

## The community identity (profile.json)

The identity is an Ed25519 key: `identity.public_key` (32 bytes) is the community profile id; the private seed must come
along or the player id is lost. The app parses (field names compared as immediates in the parser at 0x102dcf438 of 0.10.0):

| version | fields | where |
|---|---|---|
| 1 | `identity`, `protection: "windows-dpapi-user-v1"`, `protected_seed` (hex DPAPI blob) | 0.9 / 0.10 Windows |
| 2 | `identity`, `seed` (hex, 32 bytes), no protection | native macOS / Linux |

Anything else: "Unsupported community profile format; not replaced". The app never replaces a key it cannot read ("... not
replaced", "locked here"), but a locked profile can't sign in.

Under the loader `CryptProtectData` is a passthrough, so a loader profile's `protected_seed` is the RAW seed (64 hex chars).
The native app unlocks version 1 only for blobs made by Wine's crypt32 (it tries the users WINEUSERNAME, LOGNAME, then
"steamuser") and calls a raw seed "not made by Wine". `profile-native.py` checks that the seed derives `identity.public_key`,
then writes version 2 (default) or, with `--wine <user>`, a Wine blob; both verified in the app (same id, unlocked).

Wine's CryptProtectData as the app expects it (dlls/crypt32/protectdata.c + the app's unlock at 0x102dcd0a4):
key = 3DES-168 from SHA1(user + NUL + "I'm hunting wabbits" + salt16 + entropy) with entropy =
"MCC-community/profile/Ed25519/v1" (the client's pOptionalEntropy), expanded the CryptDeriveKey way (SHA1(0x36-pad ^ h) ||
SHA1(0x5c-pad ^ h), first 24 bytes); 3DES-CBC, zero IV, PKCS#5; fingerprint SHA1(plaintext). Layout: count0 = 1,
info0 = "Wine Crypt32 ok\0" (16 raw bytes, NO length prefix), count1 = 1, info1 (same), null0, then length-prefixed
description (UTF-16 ""), cipher_alg 0x6603, key_len 168, data0 (the magic), null1, hash_alg 0x8004, hash_len 160, salt,
cipher, fingerprint.

## Running it

- Double-click, or `open -a "Project Reclaimer"`; join links: `open "reclaimer://join?server=host:port"`.
- The binary directly (logs to stdout): `"/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer"`.
- Test profile (no services): `RECLAIMER_PROFILE_DIR=<folder> RECLAIMER_CACHE_DIR=<folder> RECLAIMER_TEST_SILENT=1 <binary>`
  (`ReclaimerMac/native/test-v2` is a converted copy). Killing it before its menu is up is remembered ("the last start was
  closed before its menu was up"; after two, mods downloaded from servers are left out of the next start).
- 215 `RECLAIMER_*` variables (`python3 re-tools/machoref.py` / strings): `_PROFILE_DIR`, `_CACHE_DIR`, `_MCC_FILES`,
  `_STATS_SERVER`, `_STATS_KEY`, `_JOIN_LINK`, `_DISCOVERY_DIR`, `_CONTENT_DIR`, ~190 `RECLAIMER_TEST_*` diagnostics.

## Reverse engineering the app

`re-tools/machoref.py` (arm64 Mach-O, universal binaries too): `strref` (adrp+add/ldr references to a text, also into
concatenated literals), `dis` (a function by LC_FUNCTION_STARTS, string targets annotated), `func`, `calls`. `strref` needs
numpy (system python3), `dis` capstone (`re-tools/angrenv/bin/python`). Rust compares short field names and enum strings as
mov/movk immediates: grep the disassembly's immediates, not `strings`.

## Command line (0.10.0 `--help`)

`project-reclaimer [ENGINE.dll] [MCC_FOLDER] [OPTIONS]`; `dedicated COMMAND | --server FILE.toml`; `--rcon ADDRESS:PORT PASSWORD
COMMAND...`; `--rcon-app`. Options: `--map NAME` (a multiplayer game straight away, e.g. `--map guardian`), `--forge`,
`--campaign ID`, `--seconds N` (end after N s), `--background` (tests: a small window below the others that never takes the
focus -- use it for every unattended run), `--headless` (no window, null renderer), `--local-players N`, `--data-dir`,
`--settings FILE.json`, `--renderer vulkan`, `--verify-map FILE`, `--kit` / `--kit-tool` (the Reclaimer Kit).

## Inside the app (0.10.0; for our hooks)

- **Guest image**: halo3.dll is mapped at its RVAs (headers too) in a range reserved INSIDE the app binary: app vmaddr
  0x103f94000 + the app's slide (the log line "[host] linked recompiled engine, image at 0x..." prints it). Relocated, so
  guest pointers are host pointers. .text is unchanged; TLS index (halo3+0xa39f9c) = 0. Import slots point at stand-in
  addresses just past the image (calls there trap into the runtime).
- **Code table**: `{ptr, count}` at app 0x103f44418 (+slide): sorted 16-byte `{preferred-base guest address, host code}` pairs,
  22894 of them (the lookup at app 0x103296384; a 4096-entry cache at 0x103f6c188). A host pointer into the app's own text
  = a function the app replaced with native code (a "stand-in").
- **Lifted function entry** (its guest-ABI wrapper; lifted code calls lifted code through these): `stp x29,x30,[sp,#-16]!`,
  `mov x29,sp`, `stp x19,x20,[sp,#-16]!`, `ldr w<n>,[x0,#0x290]`, with **x0 = the guest thread's context**: rax +0, rcx +8,
  rdx +0x10, rbx +0x18, rsp +0x20, rbp +0x28, rsi +0x30, rdi +0x38, r8..r15 +0x40..+0x78; +0x290 bit 31 = a pending-event
  flag; **+0x2d8 = the thread's TLS pointer (gs:[0x58])**, cached by the runtime helper (app 0x1033f6d7c) once read. The
  wrapper calls the body with (x2 = ctx, x3 = rsp, x4.. = the function's live-in guest registers) -- per-function signatures,
  so hooks go on wrappers.
- **The code is MAP_JIT** (reported RWX): a thread writes it after `pthread_jit_write_protect_np(0)`. A hook = the first
  instruction replaced by one `b` to a stub within +-128 MB (fixed-address allocations next to the code region; the region's
  own neighbourhood is reserved, VM_FLAGS_ANYWHERE lands ~1 GB away).
- **What still runs as halo3 code** (`NHOOK_CENSUS=1`, a 60 s solo Guardian match): 1007 of 21977 lifted functions. The app
  replaced main_loop_body (0xb2250), the sim tick (0xef120), object updates, render prep, local-user lookups (0xfbb94 /
  0xfbc04), display_event (0x11b0ec: no kills in that run, unknown), object_delete (0x342ed8: its entry is a branch, not a
  wrapper), chud_messaging_post (0x2d3f2c) and the teleport / vehicle functions: the loader's halo3 hooks don't carry over
  one-to-one. Still lifted and running: the game-state lock pair 0x12c948 / 0x12c9a0, the millisecond clock 0x9d22c
  (~7000 calls/s on the engine thread), ... (the census file lists them).
- **Engine thread TLS**: hook 0x9d22c, keep its ctx; TLS block = `[[ctx+0x2d8] + 0]`. The players table (TLS +0x10, data
  array "players", element 0x2f8, names at +0x58) reads exactly as under the loader (verified: name and unit handle).
- **Capacities**: the app enlarges halo3's tables and moves fields; it logs each change as `[capacity] <what>: old -> new`
  (players.count 0x10 -> 0x80, objects.count 0x800 -> 0x3fc0, game_globals.bytes 0x159c0 -> 0xed900 with statborg /
  player_state / ... moved, mapping.output_users 0xd8 -> 0x4e8, ...). Readers of moved fields must apply them.

## Our features on the native app (status)

The loader's stats page, overlay, forensics, chat/voice/kill feed and recording hooked halo3.dll or the 0.9 exe inside the
loader; none of that runs in the native app by itself. `shim/native/` brings them back: `nhook.dylib` (DYLD_INSERT_LIBRARIES;
build `bash shim/native/build.sh`, run `bash shim/native.sh [--map guardian --seconds 60 --background]`, test profile unless
NATIVE_REAL=1, log /tmp/reclaimer_native.log; it refuses to start while the app already runs).

**The stats page** (2026-11-06, shim/native/nstats.c): the loader's page (shim/src/stats_page.html, unchanged) on
http://127.0.0.1:8765/ (`NHOOK_STATS_HTTP=<port>`, 0 off; `NHOOK_STATS_PAGE=<file>` serves it from disk). Works: the map
(.map files the app opens: interposed open/openat -> shim/src/h3maps.c, now shared with the loader's hitbox.c), players
(name, team, health, shield, camo, weapons by tag name, grenades, vehicle and seat, position, speed, facing; "you" = the
profile's nickname), deaths (alive -> dead) with the heatmap (the loader's heatmap files: both clients add to them), map
pictures (made with re-tools/h3bsp.py when missing), the game variant and traits ("Game rules"), vehicles, projectiles,
the feed: match start, joins / leaves (the players table), chat / server / notice lines and ballots (the app's own
`[chat] line [...]` / `[playlist]` log lines). Verified in a background Guardian match (page with no console errors).

Since 2026-11-07 also: **kills, deaths, assists, betrayals, suicides, score** from halo3's statborg (see "Kills" below), a
kill feed paired from its counts (estimated weapon), the **Connected list** (halo3's network session; host badge, session
state, slots = the session's maximum), the **Packets window** (wire layer: every datagram's type, TCP connections; the
game layer of halo3 message names is not in yet). display_event is hooked and logs its first 200 events raw
(`[nhook] [event]`) to learn the app's enlarged event layout.

Since 2026-11-07 also (shim/native/nrender.m: the app's presentDrawable: swizzled): the **overlay** (hitboxes, vehicles,
info, items, tracers; the page's buttons and panel; halo3's observer camera; no visible-only, no zones), **recording**
(the page's Record row, rec.m), **Capture frame**, page **controls** (overlay, rec, recstop, capture; NHOOK_STATS_READONLY=1
off), the **fps line** (perf). Recording / capture / overlay checked in background Guardian matches.

Live check (2026-11-07, the user's profile, a 72-player Infection game on Sandtrap): hooks placed, the page in the match
with 72 connected, kills from the statborg (57 players with kills), overlay hitboxes and tracers drawing, 145-179 fps at
3440x1440 (frames 5-7 ms median). Fixed on the way: empty snapshots (wrong engine thread), the in-game menu ending the
match, stub pages not found in some launches, the projection on ultrawide screens (Hor+).

Since the same night (2026-11-07): the overlay uses the frame's own view constants (shader parameter 0x27 via a filtered text
hook on halo3+0x2af478) and the scene's depth texture: zones are hidden behind the world and every other feature has
"visible only" again. Hooks on the app's OWN code work (text_hook: the app is ad hoc signed, no hardened runtime); 917 halo3
functions are the app's own code (NHOOK_LIST_BRANCH_ENTRIES=1). Bullets come from a text hook on the app's object_delete
(needs a real game to show).

Since the same day also: weapon / vehicle / power-up spawns and pickups (nspawns.c), shots / hits / accuracy (the object_delete
text hook), page **mutes** (the app's own set_muted and mute list, nnet.c voice_init), **who talks** (the page's dot and the
forensic archive's "who talked when", from the app's `[voice] active ... heard 0x<id>: N frames ... rms` report every ~2 s;
timing only), /forensics (forensic.c unchanged otherwise).

Test runs can drive the app: `--ui-script <file>` (menu automation: `wait 12`, `press SERVER BROWSER`, `trigger right 1`
(menus only), `stick`, `burst`, ...; a wrong row name prints the rows) and `--request-at SECONDS JSON` (the engine's test
requests: top level `gameplay` / `networking` / `appearance` / `forge` / `campaign` / `hud` / `theater` / `capacity` / `maps` /
`editor` / `at_tick`; a wrong name lists the variants, a missing field is named -- e.g. `{"campaign":{"hold_trigger":{"seconds":6}}}`
fires the local player's weapon in a multiplayer test match too, `{"campaign":{"fire_pulses":{"count":N,...}}}`). The test
profile refuses public servers itself ("[net] test profile: game connection at ... left alone"). More requests:
`{"campaign":"kill_player"}` (the feed showed "<name> killed themselves" from the statborg), `{"campaign":{"target_practice":
{"tag":"objects\\characters\\masterchief\\mp_masterchief\\mp_masterchief","player":0,"distance":5,"span":0,"speed":0,
"seconds":15}}}` (a biped 5 units ahead; fields tag player distance bearing span speed seconds weapon dual team clear lift;
the overlay drew its hitboxes, our estimated hits counted 2 of 10 shots while the app's own counter said 0 hits / 0 damage for
the team-less target), `place_bipeds` {tag, at [x,y,z], spacing, side}. An unknown field makes serde list all of them. Live bullet check:
`bash shim/tools/native-fire-test.sh` (object_delete bullets, shots, tracer pixels in overlay captures: PASS 2026-11-07).

The 0.10 server list: shim/tools/servers10.py (server-status.md "Client 0.10+": one UDP datagram per server, no QUIC).

Not yet: voice AUDIO in the forensic archive (the native voice packets are not seen: their receive function is the app's own
Rust, not found yet), the packet log's game layer (halo3 message names).

### Kills (static RE of halo3.dll)

- halo3+0x10057c returns the statborg = game engine globals (TLS +0x20, the app's `game_globals` in its [capacity] lines)
  + 0xfeb8; the app moves it to +0x159c0 (`[capacity] game_globals.statborg: 0xfeb8 -> 0x159c0`), the player blocks grow
  to 128 x 0x40 (`statborg_teams_shift 0x1c00`).
- halo3+0x1da7e0 `adjust_player_stat(statborg, player, stat, delta, results_stat, ...)`: i16 at
  statborg + (player*32 + stat)*2, clamped to +-30000, marks the player's network bit (replicated), then sets the game
  results statistic `results_stat`. Its callers name the stats: 0x1db33c kills = stat 2 (results 9 "kills"),
  0x1db58f deaths = 3 (11), 0x1db5ca suicides = 4 (13), 0x1db635 betrayals = 5 (12), 0x1db171 assists = 6 (10),
  0xfc76f rounds won = 7 (6), 0xfca27 score = 0 (7). Team blocks: +0x400 + team*0x18 (12 i16) in halo3.dll.
- Game results (halo3+0x2190e40 in halo3.dll; the app moves it: `[c1] game results: 128 slots, ... at`): statistics at
  +0x1358 (`results.statistics`), 0x263 u16 per player (low 15 bits), the first 0x34 named by the table at
  halo3+0x792c80 (24-byte rows, name pointer first: re-tools/h3results.py), then medals, then 63 damage types x 6 at
  index 0xea (halo3+0x124064). Not read by the page (the statborg is the live, replicated one).

Online use: the hooks only read (one entry hook on halo3+0x9d22c records the engine thread's context; nothing in the game
changes). The app's anti-cheat is server-side and behavioural ("sent game data that a normal game never sends", unusual
physics); no client-memory integrity check was found in the binary's strings. Still the user's call before a real match.
