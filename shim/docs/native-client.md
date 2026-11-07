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
loader; none of that runs in the native app. `shim/native/` is the way back: `nhook.dylib` (DYLD_INSERT_LIBRARIES; build
`bash shim/native/build.sh`, run `bash shim/native.sh [--map guardian --seconds 60 --background]`, test profile unless
NATIVE_REAL=1, log /tmp/reclaimer_native.log). Done: the log tap, the image check, the code table, entry hooks, the census,
the engine thread's TLS and the players table. Next: the stats HTTP server and page on top of these readers (stats.c's
readers with the capacity remaps), then events (the app's own log lines, or hooks on what still runs lifted), then the
overlay (drawn by the app's Metal renderer: needs its own path).
