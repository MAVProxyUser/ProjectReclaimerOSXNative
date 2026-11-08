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

## Inside the app (0.10.0 addresses; 0.10.1 and later: napp_builds.h, "When the app updates")

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

## When the app updates

The app updates itself ("Update available ... Updated to 0.10.1; starting it."). nhook's app-specific addresses live in a table
per app build, `shim/native/napp_builds.h`, keyed by the app binary's LC_UUID: the guest image, the code table, the Opus decode
and the voice mixer's call / sender-id load (+ the id's offset in the mixer's entry), the per-frame function, the chat_log gate
and byte, the mute list and set_muted. halo3's own functions are reached by their halo3 address through the code table, so they
move only with MCC's halo3.dll, not with the app; the app's `[capacity]` lines give its moved halo3 fields at run time.

- **Porting** = `python3 re-tools/machoport.py port re-tools/machoport-sigs.json <new app binary> --apply`: every point found again
  by its code's shape and checked by what it must be (the mixer's bl reaches the decode, set_muted reaches the mute list, the
  chat gate's ldrb reads the chat_log byte, ...). It writes the row into napp_builds.h and into ReclaimerMac/native/app-builds.txt
  (which a prebuilt dylib reads). **shim/native.sh does this by itself** when the installed app has no row (then rebuilds the
  dylib). A point it cannot find is 0 in the row and that feature stays off; the rest work. A point that moved in a way the
  finder cannot follow: re-find it with `re-tools/machoref.py` (strref / calls / dataref / dis) and add a finder for it in
  machoport.py's POINTS (kinds code / data / call / infunc / imm).
- **Checking** = the hooks check (below) and `bash shim/tools/native-hooks-test.sh` (a local test match: PASS / FAIL per item),
  then the feature tests (native-fire-test, native-aim-test, native-host-test.sh walls|tpnext, native-voice-test, native-spar-test);
  `bash shim/tools/native-test-all.sh` runs all of them in a row (~20 min).
- 0.10.0 -> 0.10.1 (2026-11-08): all 12 points found; the code table still 22894 entries; the voice mixer's sender entry grew by
  0x28 bytes (the peer id at x26 - 0x2a0, was 0x278). The kept release zips (ReclaimerMac/native/v<version>/) hold both binaries.
  0.10.1's own notes: "one host connection per client in both networking models: players' traffic goes through the host" (no
  client-to-client connections, "Classic" networking the default) -- so a client sees only the host's address: the players table's
  IP column and the Packets window have per-player addresses only when THIS app hosts (as on a dedicated server before).

## The soundboard (nmic.c)

The app opens a microphone only in an online session, by asking CoreAudio for the default input device; on a Mac without one
(this Mac Studio) it stays "[voice] no capture (no microphone): listening only". nhook then hands it a virtual device ("Reclaimer
soundboard", 48 kHz mono float, VMIC_DEV 'RCLM'), answers that device's property queries, emulates the AUHAL unit the app points
at it (a 10 ms clock calling the app's input callback) and fills its AudioUnitRender(bus 1) from the soundboard (silence when
idle); with a real microphone the clip is mixed over it instead. The app's Talk mode decides when it is sent (Sensitivity: by
itself while a clip plays; Hold / Toggle to talk: with the talk key). The page's Soundboard panel: the files in
ReclaimerMac/soundboard (Play, Stop, volume, "add" = upload), POST /control?do=sb&play=<name>|stop=1|vol=0..2. Not the app's
--voice-wav: it reads its file to the end at launch (a named pipe too) and transmits all the time (shim/tools/voice-fifo-probe.sh).
Test: shim/tools/native-soundboard-test.sh. Diagnostics: NHOOK_AUDIO_TRACE=1, NHOOK_MUTE_OUTPUT=1; NHOOK_VMIC=0 = off.

0.10.1's server browser downloads the selected server's mods by itself (and goes on "out of sight" when you join another one);
cancel it in the app's mods window -- nhook does not touch it (backlog N23).

## Walkers that fight, learned from real players (2026-11-08 night)

- **How a joined client's walker reaches the server**: the controls packet "A" is built by halo3+0x50fb8 (the app's own code)
  from a control record (+0x10 yaw, +0x14 pitch, +0x18 / +0x1c throttle, +8 button bits); a text hook before it writes the
  walker's values (nbot.c bot_on_abuild). Fire = control record bit 19 at the reader (+0xC8): the local weapon fires and the
  packet's trigger bytes (halo3+0x5452c, from the weapon's barrels) carry it. The app reads controllers through IOHID, not
  GameController (nvpad.m's virtual pad is never polled).
- **The fighter** (NHOOK_BOT_FIGHT=1, nbot.c): line of sight by halo3's collision ray 0x1fd748 from the engine thread, reaction
  delay, aim noise, the real players' distance band / strafe timing / back-off, grenades, melee, floor-safe steps, no tracking
  through walls; A* prefers the lanes players use (the model's heat).
- **Battles**: `bash shim/tools/walker-battle.sh [N] [secs]` (our local server, Guardian free-for-all, test identities test-v2 /
  test-second / test-third / test-fourth -- `init` makes the last two), `python3 shim/tools/walker-train.py [gens] [secs]`
  (evolves the tactics), `python3 shim/tools/walker-compare.py` (walkers vs players on the same measurements).
- **Real players**: `bash shim/tools/observe-public.sh [total] [session]` (the user's profile, background window, mod-free public
  servers, nothing pressed) + match-recorder.py (no names) -> ReclaimerMac/tracks; `python3 shim/tools/walker-model.py` ->
  tracks/walker-model.json (NHOOK_BOT_MODEL), merged with the stats page's footprint heatmaps. Infection recordings count as
  lanes only (heat / flow): zombies and survivors do not fight like slayer players.
- **How human**: `python3 shim/tools/walker-compare.py [walkers' folder]` prints players vs walkers per measure and a
  human-likeness score = 100 x exp(-mean |log(walker / player)|); `python3 shim/tools/walker-report.py [out.html]` = the night's
  page (no names: Walker 1-4). `WB_TRACKS=<folder>` keeps one battle's recordings apart (one set of settings measured alone).
  The night's numbers: training run 1 55%, run 2 73%, the trained champion's showcase battles 70 -> 81% as the reaction was fixed
  (walker-model's reaction = 30 -> 5 deg of aim: NHOOK_BOT_TURN0, the turn rate before reacting, 0.73 rad/s; NHOOK_BOT_REACT the
  delay factor, 1.6). `python3 shim/tools/walker-train.py --champion` = the tactics set with the best mean over all its battles.
- **How players fight** (2026-11-08 midday): `python3 shim/tools/engage-extract.py` (duels + multi-player records per recording ->
  tracks/engagements), `python3 shim/tools/engage-model.py [--json report.json]` (the report by game class + tracks/engage-model.json:
  weapon distance bands, target-choice weights, second-attacker odds, a movement library). The fighter reads it (nbot_engage.c,
  NHOOK_BOT_ENGAGE=<file>, =0 = the old nearest-enemy fighter): awareness (in view / attacked / close), the players' choice rule,
  turning to a second attacker with their odds and delay, the weapon's distance band, real movement pieces.
- **Weapon pickup is not a button bit**: shim/tools/native-pickup-scan.sh held each of the control record's 64 bits (+0xC8) on a
  Mauler lying on the floor: none swapped it (FIXES.md "The pickup scan"). Walkers keep their loadout (NHOOK_BOT_PICKBIT unset).

## Learning from real players: the pipeline and the tome (2026-11-08)

The walkers learn how real players move and fight from public matches, per map and in aggregate, and the archive grows with
every match the hook sees.

1. **Recording.** Every public match a hooked game is in is recorded by nhook itself (ntracks.c: tracks/<date>_<map>_<tag>
   .jsonl.gz, 10 samples a second per player: position, aim, shield / health, weapon, vehicle, kills, deaths, shots, hits,
   grenades; events: kills, deaths, medals, spawns, pickups -- slots only, never names; NHOOK_TRACKS=0 off). The observer
   (`shim/tools/observe-public.sh`) samples on purpose: mod-free public servers, whole matches, Slayer / CTF (Infection left out
   for now: OBS_INFECTION=1), OBS_MATCHES / OBS_MIN_PLAYERS / OBS_MAX_PLAYERS / OBS_STRICT for "10 large then 4 smaller games",
   and it favours the map x game type the tome has least of (coverage +25 / +12 players in its pick). It runs its own
   match-recorder.py (the same format; the header also names the game engine).
2. **The engagement library.** `python3 shim/tools/engage-extract.py` cuts every recording into duels (who attacks first,
   awareness, distances, movement across / toward the other, aim, fire, shields, outcome), target choices, second attackers,
   outnumbered stretches, teammate joins, skirmish sizes, radar contacts (turning to unseen moving enemies), respawns (revenge,
   the death spot), power-item spawns (who goes for them), weapon use (held time, kills, deaths) and one meta record per
   recording -> tracks/engagements/*.eng.jsonl.gz. Game type: the engine first, the variant's name, the listing last.
3. **The models.** `python3 shim/tools/engage-model.py [--json report.json]` -> tracks/engage-model.json (the AGGREGATE: every
   Slayer / CTF / other match), tracks/maps/<map>.engage.json (PER MAP, 150+ duels, with "by_class" per game type on that map),
   tracks/tome.json (THE TOME: per map x game type the recordings, minutes, players, duels, kills, first / last sampled, the
   Halo 3 maps never sampled) and the report (who wins and why, distances, movement, target choice, radar, revenge, items,
   weapon effectiveness).
4. **The walkers** (nbot_engage.c / nbot_goals.c) load the aggregate and overlay the map's model and its game type's section
   wherever there are enough samples, reloaded on a map or game change; the map's own movement pieces come first; lanes and
   its own failed-death zones are per map. What they use: awareness (view, radar, attackers), the fitted target choice, second
   attackers with the players' odds and delay, weapon distance bands, real movement pieces, grenades, the radar look, the
   respawn draw (killer / death spot / the item's lane), power items, the goal draw (never idle); in team games teamwork from the
   players' team games: the spacing to a teammate, joining a teammate's fight, coming to a teammate's aid, then moving as a unit.
5. **Checking them.** `walker-battle.sh` (WB_IDS, WB_WATCH, DED_GAME / DED_MAP on the server) + `engage-compare.py`
   (fight-likeness against the players on the same map) + `walker-compare.py` (movement); `native-firefight.sh` for Firefight.
6. **Watching it all.** `python3 shim/tools/workboard.py` (http://127.0.0.1:8790/): what runs, the direction, the task list, "What the
   game is learning" (`python3 shim/tools/learnings.py`: findings with their sample sizes), the tome's coverage grid, the observer's log; `workboard.py task "..." doing|next|done` / `workboard.py direction "..."`.

## Lanes (2026-11-08, nlanes.c)

The stats page's map: heatmap **deaths / lanes / lanes + deaths / neither**. Lanes = how many times players crossed each 0.5-unit
cell (moves between snapshots; standing still adds nothing), on foot and in vehicles apart, carved as ruts relative to the map's
own traffic and deeper as more is recorded; hover gives the count and the usual direction. Recorded live by nhook on every map
(<heatmaps>/<map>.lanes, NHOOK_LANES=0 off) and added from the recordings with `python3 shim/tools/lanes-import.py`
(`--show <map>`; safe while a game runs: both merge under a lock). Offline: `bash shim/tools/lanes-test.sh`,
`python3 shim/tools/lanes-page-mock.py --map <map>` (the page with that map's data on http://127.0.0.1:8798/).

## The motion tracker (halo3, found 2026-11-08; static RE of halo3.dll -- the app lifts the same code)

- halo3+0x2d289c = the HUD's per-user draw (ecx = local user 0..3). It calls halo3+0x2d349c "is the tracker shown" (the user's
  player, the HUD flag at [TLS block +0x220] +0x144.., the player's radar mode != 1) and then the tracker's draw halo3+0x2f3904
  with S = [TLS block + 0xF0] + user * 0x28c8 + 0x244 (the HUD's per-user block; halo3+0x2cf7f8 packs the radar mode into its
  +0xca8 bits).
- S+0xcbc = the range in world units (the draw divides by 0.3281 for metres; its scale switches at 10, 75, 150 m). S+0xcb4 = the
  newest of 10 history frames (the draw goes (cb4 + 1 + k) % 10, k = 0..9: oldest first). Frame f at S + 0xf0 + f * 0x104:
  int32 count, then up to 16 blips x 16 bytes: float x, float y (the sensor plane), float ?, u8 type (+0xc; 7 drawn 3x larger,
  6 its own branch), u8 size class (+0xd: x a scale table at [halo3 globals +0x80]), u8 colour (+0xe). halo3+0x2f36b8 draws one
  frame, halo3+0x2f329c one blip. The list at S+0xb18 (16 bytes each, count S+0xcac, fades S+0xc58) is drawn on top.
- Player traits: resolved at player +0x1FC; radar mode +0x214, range +0x216 (u16). halo3+0x202de4 is the player's network
  update (it replicates them; not the tracker).
- nbot_goals.c rd_blips reads user 0's newest frame; NHOOK_RADAR_LOG=1 logs it beside the true enemy positions in the walker's
  frame every 2 s, to fit x / y to the world. The walker's radar uses halo3's rules (rd_contact) until that is done.

## The hooks check

Everything nhook depends on in the app registers in `shim/native/nhchk.h`: per item its feature, its kind (address, entry /
body / text hook, interpose, swizzle, app code / data, log line, reader), when it should fire, whether it installed (or was found)
and why not, and how often it fired. `GET /hooks.json`, the stats page's **Hooks check** window (FAILED red, silent orange, ok
green, "problems only"), and `[hooks]` lines in the log (90 s after the engine started, 60 s into each match). Verdicts: ok,
FAILED (not installed / not found in this build: the feature is off), silent (installed, what it waits for happened, no call yet),
waiting (a match / a shot / a talker / a page switch has not happened yet), off (a switch), not expected (display_event: the app
never calls it). Hooks that install on first use are preflighted at start (their shape checks, nothing written).
While someone plays: `python3 shim/tools/hooks-watch.py [port]` prints each item whose verdict changes. The user's first 0.10.1
session (5 maps on a public server): 33 ok, 0 failed; voice (ids at the new offset, channels), chat, the kill feed and the overlay's
hitboxes (a capture: on the players' armour) all checked live.

## What was removed (2026-11-08, client 0.10.1)

**From nhook, with the 0.10.1 port** (each replaced, not lost; the old way stays behind its switch where it is a behaviour):
- The 0.10.0 constants: `APP_IMAGE` (0x103f94000), `APP_CODETAB` (0x103f44418), `APP_MUTED` / `APP_SETMUTE`, `HT_FRAME_FN`
  and the Opus decode / voice mixer / chat gate literals -- now `APP(field)` from napp_builds.h, per build.
- The run-time checks that compared exact 0.10.0 instruction words (the mixer's `bl` encoding 0x978a7e41, `#0x278`, the chat
  gate's adrp / ldrb words): they now decode the instructions and compare against the build's own row.
- The fixed sender-id offset 0x278 in the voice hook (now the row's `mixer_id_off`: 0x2a0 in 0.10.1).
- `is_muted` (0x10214db98 in 0.10.0): only ever named in a comment, never used at run time, so it is not a table point.
- One 16 KB stub page per entry / body hook: stubs now share near pages (NHOOK_STUB_PAGE_EACH=1 = the old way).
- "Hooks off" = no stats page at all: with hooks off the page now starts and its Hooks check window says why.

**From the app itself in 0.10.1** (its release notes; what it changes for our page):
- Client-to-client connections and the standby connections to a possible replacement host: every player's traffic now goes
  through the host ("one host connection per client in both networking models"). A client sees only the host's address, so the
  players table's IP column and the Packets window have per-player addresses only when this app hosts.
- "Enhanced" networking as the default ("Classic" is the default now; Enhanced is still selectable).

**Loader features not brought to the native app** (the user's call, 2026-11-07; BACKLOG): auto-vote (#50), graphics quality
from the page (#68; the app's own Settings menu has it), the late camera update (mouse only; the user plays with a controller).

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
kill feed paired from its counts (estimated weapon; since the same night EXACT from halo3's game results when the app keeps
them: killer x victim matrix + per-damage-type kills, FIXES.md "exact kills"), the **Connected list** (halo3's network session; host badge, session
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
(seen live 2026-11-07 in a 50-player public game: tracers and hitboxes drawn, 200-260 fps).

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

Voice packets (forensic audio): a text hook on the app's Opus decode (opus-rs, 0.10.0 0x1003f1e7c) called by the voice mixer
(0x102152578; sender id at x26 - 0x278). Live 2026-11-07 in a 50-player public game: 88 utterances with audio archived, one
decoded by ffmpeg to clear speech (FIXES.md). Mute was checked there too (the app's list took and dropped a player).

Chat: the app prints its chat lines only with its chat_log byte set (a dedicated-server option); nhook sets it
(nstats.c chat_log_tick), so chat / [SERVER] / notice lines reach the feed and the archive (FIXES.md, 2026-11-07).
The kill feed ignores the history a client receives after joining a game in progress (FIXES.md "history is not a kill").
Aim vs hits: the page's Aim column (FIXES.md "aim vs hits"): each straight shot's miss against the nearest enemy's posed
hitboxes and whether the server then registered damage; the Aimbot? column flags lobby outliers from it.
Hook cost: the page's Hook cost panel (nhcost.h; FIXES.md "the hook cost meter"): every hook and overlay part times itself,
and "measure" switches one off and on in turns to give its real fps cost.

Host tools (2026-11-07, shim/native/nhost.c; FIXES.md "the host tools on the native app"): teleport (next to a player, to a
clicked point, by a delta, to the map's spots), boundaries off, invisible walls off, speed 200%, on the game's host or our
native dedicated server (requests take &slot=<player> there). halo3's own network layer does not exist natively: halo3 code
that marks objects for a network update (0x1ec3c) faults on its NULL table unless halo3 is told it runs a local game for
the call. Lifted code calls lifted code directly at the BODY its wrapper calls (x2 = context, x3 = guest rsp, x4..x7 = rcx,
rdx, r8, r9, result x2): body_hook_after hooks there; code-table entry hooks see only calls through the table. The app's
per-frame function runs from halo3's render loop (0xb19bc, a client only); a dedicated server runs halo3's main loop 0xb2538
-> game tick runner 0xeefa8 -> 0xef120. Crash frames: NHOOK_CODETAB_DUMP + shim/tools/native-frames.py.

Two clients (2026-11-07): our native dedicated server plus the test profile and a SECOND test profile with its own identity,
ReclaimerMac/native/test-second (made by the app on its first start; renamed "Second"): shim/tools/native-host-test.sh tpnext,
shim/tools/native-voice-test.sh. NATIVE_ALLOW_SECOND=1 lets a test client start beside the server. The app's own test options
for that: `--voice-wav FILE` + `--voice-talk all|team|nearby|open` (talk from a WAV), `--chat-say SECONDS:all|team|nearby:TEXT`,
`--voice-mute <peer>`, `--ballot-choose 0..3`; test requests that work on a joined client: `{"campaign":{"aim_at_nearest_player":
{"height":0.5,"approach":0}}}` and `{"campaign":{"fire_pulses":{"count":50,"every":0.1,"hold":0.05}}}` (hold_trigger fires only a
few shots there). NHOOK_TAP_LOG=<file> copies every line the hooks read (any process that loads the dylib: a server's worker).
Voice channel: the mixer entry's byte at end - 7 (1 global, 2 team, 3 proximity; FIXES.md "the channel"). Host diagnostic:
do=floorprobe&x=&y=&z= (halo3's collision ray down + the open / solid test at 9 depths, in host_msg).

The sparring partner, the walker and the page's mic switch (2026-11-07, shim/native/nbot.c; FIXES.md): halo3's per-player
control record (TLS block +0xC0, 4 x 0x110 bytes: +0xC0 unit, +0xC8 buttons (bit 0 crouch, bit 1 jump), +0xD0 flags, +0xD4 yaw,
+0xD8 pitch, +0xDC / +0xE0 throttle forward / left) is filled by halo3+0xf66ac and copied into the unit's control by 0xf7178 --
both are the app's own code, so a text hook at the app's 0xf7178 writes record 0. The aim reaches the server; a joined client's
MOVEMENT does not (the app sends it from its own input layer), so the walker runs only in a local game / one this client hosts.
Test clients only (a test profile, a local game or a loopback / private server). shim/tools/native-spar.sh puts the second test
identity on our server with the aim on; shim/tools/native-spar-test.sh checks both. NHOOK_BOT_THROTTLE=x,y / NHOOK_BOT_BTNSCAN=1
re-find the record's throttle axes / button bits after an app update. Mic: AudioUnitRender on input bus 1 is interposed and
zeroed while muted (do=mic&on=, NHOOK_MIC_MUTED=1).

Not yet: the packet log's game layer (halo3 message names; 0.10's wire protocol is the app's own).

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
  +0x1358 (`results.statistics`; app +0x6c98), 0x263 u16 per player (low 15 bits; 0x4c6 bytes a player), the first 0x34 named
  by the table at halo3+0x792c80 (24-byte rows, name pointer first: re-tools/h3results.py), then medals, then 63 damage types
  x 6 from index 0xea = 234 (damage-kills, -deaths, -betrayals, -suicides, -headshots, ...: halo3+0x793160); then the versus
  matrix (`results.versus`, app +0x2cf98, rows of 1 << versus_row_log2 = 128 entries of two u16). Live in the app on the host
  (a suicide moved 11 / 13 / 235 / 237 and versus[0][0]); read since 2026-11-07 for exact kills (rs_tick).

Online use: the hooks only read (one entry hook on halo3+0x9d22c records the engine thread's context; nothing in the game
changes). The app's anti-cheat is server-side and behavioural ("sent game data that a normal game never sends", unusual
physics); no client-memory integrity check was found in the binary's strings. Still the user's call before a real match.
