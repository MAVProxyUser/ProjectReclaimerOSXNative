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

## Our features on the native app (status)

The loader's stats page, overlay, forensics, chat/voice/kill feed and recording all hook halo3.dll or the 0.9 exe inside
the loader. With the native app none of that runs. The way back: the app has no hardened runtime, so a dylib can be loaded
with `DYLD_INSERT_LIBRARIES` (arm64, our own process: reading engine memory at known addresses, never sweeping), plus the
app's own interfaces (`RECLAIMER_STATS_SERVER`, its join-link socket). Engine data lives in the recompiled image; halo3's
TLS is the app runtime's emulation.
