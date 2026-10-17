# Patch and hook addresses across every Project Reclaimer release

Static survey, 2026-10-17. Every release exe published at
https://github.com/ProjectReclaimer/project-reclaimer-releases (34 tags, v0.8.1 .. v0.9.18; there is no v0.9.14) was
downloaded to `~/Library/Application Support/ReclaimerMac/releases/<tag>/` and read as data only: pefile + capstone +
byte regexes over the mapped image, with the same wildcard semantics as `sig_parse`/`sig_find` in `shim/src/peload.c`
(unique match over the executable sections). Nothing was run. Every file matched its release's `SHA256SUMS.txt`; the
v0.9.13 client and dedicated exes are byte-identical to `analysis/`, the v0.9.18 client to `client/`.

Addresses are exe RVAs (ImageBase 0x140000000) unless written `halo3+`.

- **CONFIRMED** = read in the disassembly, or the signature hit agrees with an independent anchor (below).
- **GUESSED** = found by shape only, or the meaning is inferred.

How each location was checked: our signatures were run first; then every point was located a second time **without**
them, from anchors that do not depend on the bytes we match: string references (`lea` to the text), the call graph
(callers/callees of anchored functions), shared globals (the lock byte of a `Mutex`), and halo3.dll constants
embedded in the exe. A table cell is "confirmed" when both agree.

## 1. Versions

| Tag | Published (UTC) | Client exe | Size (bytes) | SizeOfImage | .text size | PE timestamp (UTC) | Dedicated exe size | RCON exe size |
|---|---|---|---|---|---|---|---|---|
| v0.8.1 | 2026-09-26 22:43 | `project-reclaimer.exe` | 25156096 | 0x1811000 | 0x10a0768 | 2026-09-26 22:01 | -- | -- |
| v0.8.2 | 2026-09-26 23:19 | `project-reclaimer.exe` | 25219072 | 0x1821000 | 0x10ae0f8 | 2026-09-26 23:19 | -- | -- |
| v0.8.3 | 2026-09-26 23:53 | `project-reclaimer.exe` | 25223680 | 0x1822000 | 0x10ae6b8 | 2026-09-26 23:53 | -- | -- |
| v0.8.4 | 2026-09-27 01:00 | `project-reclaimer.exe` | 25197568 | 0x181b000 | 0x10a5838 | 2026-09-27 01:00 | -- | -- |
| v0.8.5 | 2026-09-27 01:26 | `project-reclaimer.exe` | 25197568 | 0x181b000 | 0x10a5838 | 2026-09-27 01:26 | -- | -- |
| v0.8.6 | 2026-09-27 13:46 | `project-reclaimer-v0.8.6.exe` | 25254912 | 0x1829000 | 0x10afc08 | 2026-09-27 13:46 | -- | -- |
| v0.8.7 | 2026-09-28 11:13 | `project-reclaimer-v0.8.7.exe` | 25890304 | 0x18c4000 | 0x112e9b8 | 2026-09-28 11:13 | -- | -- |
| v0.8.8 | 2026-09-28 15:45 | `project-reclaimer-v0.8.8.exe` | 27258368 | 0x1a12000 | 0x123fa68 | 2026-09-28 15:44 | -- | -- |
| v0.8.9 | 2026-09-29 14:42 | `project-reclaimer-v0.8.9.exe` | 28005376 | 0x1ac7000 | 0x12d1e58 | 2026-09-29 14:42 | -- | -- |
| v0.8.10 | 2026-09-29 20:02 | `project-reclaimer-v0.8.10.exe` | 28542464 | 0x1b4c000 | 0x1338c48 | 2026-09-29 20:02 | -- | 6478848 |
| v0.8.11 | 2026-09-29 23:45 | `project-reclaimer-v0.8.11.exe` | 28919296 | 0x1ba9000 | 0x1381178 | 2026-09-29 23:34 | -- | 6488064 |
| v0.8.12 | 2026-09-30 11:22 | `project-reclaimer-v0.8.12.exe` | 29281792 | 0x1c01000 | 0x13c8bb8 | 2026-09-30 11:10 | -- | 6488064 |
| v0.8.13 | 2026-09-30 17:37 | `project-reclaimer-v0.8.13.exe` | 29542912 | 0x1c41000 | 0x13f7948 | 2026-09-30 17:27 | -- | 6488064 |
| v0.8.14 | 2026-10-01 17:22 | `project-reclaimer-v0.8.14.exe` | 32249856 | 0x1ed6000 | 0x16150f8 | 2026-10-01 17:03 | 21143040 | 6532608 |
| v0.8.15 | 2026-10-01 22:11 | `project-reclaimer-v0.8.15.exe` | 32325632 | 0x1ee7000 | 0x1625ae8 | 2026-10-01 21:53 | 21367808 | 6532096 |
| v0.8.16 | 2026-10-03 12:45 | `project-reclaimer-v0.8.16.exe` | 32523776 | 0x1f18000 | 0x1649d28 | 2026-10-03 12:31 | 21522944 | 6532096 |
| v0.9.0 | 2026-10-03 16:22 | `project-reclaimer-v0.9.0.exe` | 33625088 | 0x2026000 | 0x16c82b8 | 2026-10-03 16:03 | 22493184 | 6532096 |
| v0.9.1 | 2026-10-04 02:49 | `project-reclaimer-v0.9.1.exe` | 33745920 | 0x2043000 | 0x16df158 | 2026-10-04 02:28 | 22645248 | 6532096 |
| v0.9.2 | 2026-10-05 12:43 | `project-reclaimer-v0.9.2.exe` | 32410624 | 0x1efd000 | 0x16e7138 | 2026-10-05 12:29 | 21262336 | 5494784 |
| v0.9.3 | 2026-10-05 19:12 | `project-reclaimer-v0.9.3.exe` | 32713728 | 0x1f48000 | 0x1724a28 | 2026-10-05 19:04 | 21380096 | 5494784 |
| v0.9.4 | 2026-10-05 22:41 | `project-reclaimer-v0.9.4.exe` | 32715776 | 0x1f49000 | 0x17251a8 | 2026-10-05 22:32 | 21382144 | 5494784 |
| v0.9.5 | 2026-10-06 20:27 | `project-reclaimer-v0.9.5.exe` | 33678336 | 0x2033000 | 0x17e9cf8 | 2026-10-06 20:11 | 22015488 | 5495808 |
| v0.9.6 | 2026-10-07 14:40 | `project-reclaimer-v0.9.6.exe` | 34856960 | 0x215a000 | 0x18bdbf8 | 2026-10-07 14:24 | 23114752 | 5496320 |
| v0.9.7 | 2026-10-07 20:18 | `project-reclaimer-v0.9.7.exe` | 35136512 | 0x21a0000 | 0x18f7578 | 2026-10-07 20:01 | 23344640 | 5496320 |
| v0.9.8 | 2026-10-08 13:47 | `project-reclaimer-v0.9.8.exe` | 35760128 | 0x2238000 | 0x196f848 | 2026-10-08 13:35 | 23753728 | 5496320 |
| v0.9.9 | 2026-10-08 22:55 | `project-reclaimer-v0.9.9.exe` | 36213248 | 0x22a6000 | 0x19ca498 | 2026-10-08 22:46 | 24113664 | 5496320 |
| v0.9.10 | 2026-10-09 13:52 | `project-reclaimer-v0.9.10.exe` | 36392960 | 0x22d3000 | 0x19ef858 | 2026-10-09 13:42 | 24174592 | 5496320 |
| v0.9.11 | 2026-10-09 22:35 | `project-reclaimer-v0.9.11.exe` | 36830720 | 0x233d000 | 0x1a47a78 | 2026-10-09 22:35 | 24606720 | 5496320 |
| v0.9.12 | 2026-10-10 13:20 | `project-reclaimer-v0.9.12.exe` | 36848128 | 0x2342000 | 0x1a4a8f8 | 2026-10-10 13:20 | 24625664 | 5496320 |
| v0.9.13 | 2026-10-10 22:17 | `project-reclaimer-v0.9.13.exe` | 37313536 | 0x23b4000 | 0x1aad2f8 | 2026-10-10 22:12 | 24817152 | 5496320 |
| v0.9.15 | 2026-10-12 19:36 | `project-reclaimer-v0.9.15.exe` | 38508032 | 0x24d6000 | 0x1b9c986 | 2026-10-12 19:17 | 25672704 | 5492224 |
| v0.9.16 | 2026-10-13 14:46 | `project-reclaimer-v0.9.16.exe` | 39233536 | 0x2589000 | 0x1c2e9a6 | 2026-10-13 14:30 | 26091520 | 5493760 |
| v0.9.17 | 2026-10-14 19:18 | `project-reclaimer-v0.9.17.exe` | 39667200 | 0x25f3000 | 0x1c81c16 | 2026-10-14 19:11 | 26502656 | 5493760 |
| v0.9.18 | 2026-10-15 19:01 | `project-reclaimer-client-v0.9.18.exe` | 43051520 | 0x2930000 | 0x1f3b706 | 2026-10-15 18:53 | 29191168 | 5493760 |

- v0.8.1 .. v0.8.5 ship as `project-reclaimer.exe`; v0.8.6 .. v0.9.17 as `project-reclaimer-v<ver>.exe`; v0.9.18 as
  `project-reclaimer-client-v0.9.18.exe`. A separate `project-reclaimer-dedicated-v<ver>.exe` exists from v0.8.14 and
  `reclaimer-rcon-v<ver>.exe` from v0.8.10. Our scripts only load the client exe (`dedicated.sh` runs the client's
  `dedicated` subcommand), so the dedicated exe is surveyed for comparison only; the rcon exe has none of these points.
- v0.8.4 and v0.8.5 have the same file size, image size and .text size, but every point is at a different RVA.

## 2. The patch and hook points

| Point | What the loader does | Purpose | How it is found | v0.9.18 |
|---|---|---|---|---|
| ui-reentrancy | NOP `jne rel32` at sig+8 and `jne rel8` at sig+0x1c | UI composition's once-per-second cache cleanup panics "RefCell already borrowed" on our first-call path | signature, `g_sigpatches` (on by default) | sig 0x657035, sites 0x65703d / 0x657051 |
| xaudio #1..#4 | NOP the 4 `jne` that compare halo3's XAudio2 import slots with the retail values | the client's headless-audio setup checks 4 halo3 XAudio2 import slots against the retail values before replacing them ("Headless server audio off: XAudio2 and Waves are never loaded."); with our shim in the slots it refuses ("XAudio2 import slots differ from the supported build; refused") | 4 signatures, `PELOAD_RECLAIMER_PATCHES` | sites 0x53d134 / 0x53d16b / 0x53d1a2 / 0x53d1d5 in FUNC 0x53d0f0 |
| watchdog | `je` -> `jmp` at sig+0xa | the worker's engine hang watchdog ("still running after N seconds") gives up on the engine thread; only the direct path (`RECLAIMER_DIRECT=1`) arms it | signature, `PELOAD_RECLAIMER_PATCHES` | sig 0x7963ec, site 0x7963f6 in FUNC 0x793f40 |
| set_muted | called (Win64 `rcx` id, `dl` muted) from the halo3 frame hook | stats page voice mute; MUTED = set_muted+0x24+disp32@+0x20 | signature, `stats.c st_voice_install` | 0x66bea0, MUTED 0x27d7848 |
| voice_recv | 19-byte entry hook (`st_on_voice`) | stats page "talking" indicator | signature (its first 19 bytes are also the hook's expected prologue) | 0x684080 |
| push_history | 19-byte entry hook (`st_on_chat`) | stats page chat feed | signature (first 19 bytes = hook prologue) | 0xb89930 |
| expanded-session flag | read (byte) | which halo3 session layout (16 or 128 players) `st_session` reads | **fixed RVA** exe+0x27f7210, used only when set_muted sits at exe+0x66bea0 (stats.c `st_voice_init`) | 0x27f7210, written once at 0x9e4a55 |
| guardian-refusal | `PELOAD_PATCH=exe+4e066d:909090909090` (NOP `jno`) | old v0.9.13 probe path: force the Guardian map-resolution callback to succeed (RE log session notes, CLAUDE.md) | **fixed RVA**, v0.9.13 only | jno at 0x5cc021 |
| xaudio (old form) | `PELOAD_PATCH=exe+460294,4602cb,460302,460335` | same as xaudio above | **fixed RVAs**, v0.9.13 only (CLAUDE.md) | = the xaudio sites |
| client's own halo3 detour | none (we must not hook the same function) | Reclaimer detours `game_engine_player_killed` (halo3+0x118a1c) itself; its installer checks a 15-byte prologue (`48 8B C4 48 89 58 08 44 88 48 20 44 89 40 18`) stored in .rdata | .rdata bytes -> `lea r8` -> next call = installer, `lea r9` = detour | data 0x1fec270, detour 0x601a90, installer 0x6ddb00 (93 call sites) |

The fixed RVAs `exe+0x4e066d`, `0x460294`, `0x4602cb`, `0x460302`, `0x460335` hold their expected original bytes in
v0.9.13 and in no other build; `0x66bea0` / `0x684080` / `0xb89930` (set_muted / voice_recv / push_history) only in
v0.9.18. CONFIRMED for all 34 builds.

Correction to `halo3-event-feed.md` 3.4: in v0.9.18 the 15 prologue bytes the client checks are at **exe+0x1fec270**
(the `lea r8` at 0x5aa2d1). exe+0x1fec2c0 is inside the string "Custom mode native contract mismatch".

The loader's own halo3.dll hooks (main_loop_body halo3+0xb2250, object_delete +0x342ed8, display_event +0x11b0ec,
chud post +0x2d3f2c) are not affected by Reclaimer releases (they depend on MCC's halo3.dll). No client build carries
their prologue bytes in .rdata, so the client does not appear to detour them the way it detours player_killed
(GUESSED: a byte search of .rdata, not a full list of the client's hooks).

## 3. Where each point is, per release

Cells are RVAs (hex) of the byte the loader changes (patches), the function entry (calls/hooks) or the variable.

| Cell | Meaning |
|---|---|
| `65703d` | our signature matches once and the independent anchor search finds the same place: CONFIRMED |
| `(58a775)` | our signature does **not** match; the place exists and was found by anchors (section 5 says how it differs) |
| `**414520 !** (real 14aa00)` | our signature matches once but in the **wrong function** |
| `1fbe90 ?` | signature hit not cross-checked (dedicated exe: no mute UI to anchor on) |
| `--` | the code does not exist in that build |
| `=fixed` | the fixed `PELOAD_PATCH` RVA is right only here |

Rows "MUTED", "expanded-session flag" and the three "client:" rows are derived values: MUTED from the anchor-found
set_muted, the flag from the writer pattern in section 6, the client rows from the .rdata bytes.

### Client exe, v0.8.1 .. v0.8.12

| point | v0.8.1 | v0.8.2 | v0.8.3 | v0.8.4 | v0.8.5 | v0.8.6 | v0.8.7 | v0.8.8 | v0.8.9 | v0.8.10 | v0.8.11 | v0.8.12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ui-reentrancy jne (sig+8) | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- |
| xaudio slot 1 jne | (69f963) | (3e8d23) | (4b7ce3) | (5b8323) | (62abc3) | (48a023) | (5c2273) | (50ebc3) | (4f9593) | (859103) | (1700a3) | (81f193) |
| xaudio slot 2 jne | (69f9c4) | (3e8d84) | (4b7d44) | (5b8384) | (62ac24) | (48a084) | (5c22d4) | (50ec24) | (4f95f4) | (859164) | (170104) | (81f1f4) |
| xaudio slot 3 jne | (69fa25) | (3e8de5) | (4b7da5) | (5b83e5) | (62ac85) | (48a0e5) | (5c2335) | (50ec85) | (4f9655) | (8591c5) | (170165) | (81f255) |
| xaudio slot 4 jne | (69fa5f) | (3e8e1f) | (4b7ddf) | (5b841f) | (62acbf) | (48a11f) | (5c236f) | (50ecbf) | (4f968f) | (8591ff) | (17019f) | (81f28f) |
| watchdog je (sig+0xa) | 66ea7d | 4c10bd | 1815ad | 5844bd | 3b666d | 6ee90d | 233a5d | 82203d | 7a454 | 2a41a | 890cba | a811a |
| set_muted (entry) | (412e50) | (580e0) | (456720) | (5e52f0) | (657b90) | (641c00) | (500f20) | (830d00) | (277710) | (181880) | (80fb70) | (85b8a0) |
| MUTED list (.data) | 170f180 | 171d718 | 17204b0 | 17184a8 | 1718da8 | 1727068 | 17bc4b0 | 1902db8 | 19af970 | 1a2d878 | 1a8aed0 | 1ae0918 |
| voice_recv (entry) | (411650) | (56870) | (455160) | (5e3a10) | (6562b0) | (640640) | (4ff6b0) | (82f500) | (275f10) | (180010) | (80e390) | (85a2d0) |
| push_history (entry) | (480f00) | (351ce0) | (547670) | (25bfc0) | (25bf50) | (256c80) | (423ed0) | (1a27d0) | (27e6b0) | (3f9170) | (5ad220) | (1d95c0) |
| guardian-refusal jno | 3eb4f5 | 5c6b75 | 3e9725 | 5bd0b5 | 62f955 | 22513a | 46b435 | 78e075 | 5b0435 | 74919a | 6c8615 | 5def65 |
| expanded-session flag (.data) | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- |
| client: player_killed prologue bytes (.rdata) | -- | -- | -- | -- | -- | -- | -- | -- | -- | 13e49e7 | 13cb43f | 1420fd7 |
| client: its detour | -- | -- | -- | -- | -- | -- | -- | -- | -- | 3d3d20 | 6cc790 | 5e5d40 |
| client: hook installer (call sites) | -- | -- | -- | -- | -- | -- | -- | -- | -- | 58b2a0 (22) | 5883c0 (22) | 233120 (23) |

### Client exe, v0.8.13 .. v0.9.6

| point | v0.8.13 | v0.8.14 | v0.8.15 | v0.8.16 | v0.9.0 | v0.9.1 | v0.9.2 | v0.9.3 | v0.9.4 | v0.9.5 | v0.9.6 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ui-reentrancy jne (sig+8) | -- | -- | -- | -- | -- | -- | 46168c | 485fa2 | 486f82 | 4da7f2 | 4e9cf2 |
| xaudio slot 1 jne | (58a775) | (5aa1d5) | (9d9945) | (6f18a5) | (257a5) | (41a7e5) | 3a5f94 | 3cd494 | 3ce474 | 406604 | 40e594 |
| xaudio slot 2 jne | (58a7d6) | (5aa236) | (9d99a6) | (6f1906) | (25806) | (41a846) | 3a5fcb | 3cd4cb | 3ce4ab | 40663b | 40e5cb |
| xaudio slot 3 jne | (58a837) | (5aa297) | (9d9a07) | (6f1967) | (25867) | (41a8a7) | 3a6002 | 3cd502 | 3ce4e2 | 406672 | 40e602 |
| xaudio slot 4 jne | (58a871) | (5aa2d1) | (9d9a63) | (6f19a1) | (258a1) | (41a903) | 3a6035 | 3cd535 | 3ce515 | 4066a5 | 40e635 |
| watchdog je (sig+0xa) | (88488) | (97d698) | (8db2f9) | (4a54ca) | (4e72ca) | (a191ef) | 5438e3 | 56ba4d | 56cb9d | 5bd6fd | 5e7a2c |
| set_muted (entry) | (7fe520) | (d81d0) | (774900) | **414520 !** (real 14aa00) | **951490 !** (real 8be0e0) | **1b4f00 !** (real 3d4770) | 46efa0 | 4955f0 | 496740 | 4eb0d0 | 4fc160 |
| MUTED list (.data) | 1b1dfd0 | 1d977d8 | 1db0d28 | 1dd7cd0 | 1ee7268 | 1ef51d8 | 1df4320 | 1e3d538 | 1e3e538 | 1f239f0 | 203e5b8 |
| voice_recv (entry) | (7fcc30) | (d69c0) | (773330) | (1491f0) | (8bcb10) | (3d3110) | (47a2e0) | (4a0930) | (4a1a80) | (4f6d80) | (5123c0) |
| push_history (entry) | (793f30) | (16d810) | (34d620) | (165940) | (67b960) | (adf3c0) | (78f5a0) | (7c4aa0) | (7c6000) | (858dd0) | (89b950) |
| guardian-refusal jno | 750df9 | 6bc4e1 | 2600c1 | 99569 | 4c5091 | a0b911 | 4068ff | 42951f | 42a4ff | 47cf77 | 48c4e7 |
| expanded-session flag (.data) | -- | -- | -- | -- | -- | -- | 1e08350 | 1e515e8 | 1e525e8 | 1f37eb8 | 205aa70 |
| client: player_killed prologue bytes (.rdata) | 14ce277 | 16f2b80 | 16f3e8f | 169c87f | 17ad2c9 | 17cdbb9 | 1789cae | 17c90be | 17ca0ee | 1890dae | 1965c1e |
| client: its detour | 630b00 | 76bf80 | 7634d0 | 528060 | 6fa090 | 5c51a0 | 41baa0 | 43e6c0 | 43f6a0 | 4926f0 | 49ff00 |
| client: hook installer (call sites) | fc590 (27) | 6c4320 (36) | 767fd0 (37) | 52c680 (40) | 6ffd70 (45) | 5cab50 (48) | 4d3f30 (51) | 4fb800 (50) | 4fc950 (50) | 54d4e0 (56) | 56a9a0 (60) |

### Client exe, v0.9.7 .. v0.9.18

| point | v0.9.7 | v0.9.8 | v0.9.9 | v0.9.10 | v0.9.11 | v0.9.12 | v0.9.13 | v0.9.15 | v0.9.16 | v0.9.17 | v0.9.18 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ui-reentrancy jne (sig+8) | 5014c5 | 51aa63 | 541813 | 5495b3 | 54d933 | 54ed53 | 553add | 59006d | 5ce33d | 5dcb7d | 65703d |
| xaudio slot 1 jne | 4215c4 | 43b064 | 456044 | 45d464 | 45cff4 | 45d3e4 | 460294 | 4916f4 | 4cdb44 | 4d3924 | 53d134 |
| xaudio slot 2 jne | 4215fb | 43b09b | 45607b | 45d49b | 45d02b | 45d41b | 4602cb | 49172b | 4cdb7b | 4d395b | 53d16b |
| xaudio slot 3 jne | 421632 | 43b0d2 | 4560b2 | 45d4d2 | 45d062 | 45d452 | 460302 | 491762 | 4cdbb2 | 4d3992 | 53d1a2 |
| xaudio slot 4 jne | 421665 | 43b105 | 4560e5 | 45d505 | 45d095 | 45d485 | 460335 | 491795 | 4cdbe5 | 4d39c5 | 53d1d5 |
| watchdog je (sig+0xa) | 601cf4 | 61b9d7 | 64569a | 649b09 | 655a69 | 6574c9 | 65ddf9 | 6a33ac | 6e21d4 | 6f2f23 | 7963f6 |
| set_muted (entry) | 512220 | 52cd50 | 5544f0 | 55c2d0 | 561740 | 562b60 | 568380 | 5a6390 | 5e4fe0 | 5f3820 | 66bea0 |
| MUTED list (.data) | 2082170 | 2114f00 | 21814f8 | 21ac518 | 2213c28 | 2218c38 | 2287dc8 | 239e558 | 244ab58 | 24b2758 | 27d7848 |
| voice_recv (entry) | (5284a0) | (542de0) | (56ade0) | (572bd0) | (578690) | (579ab0) | (57f2f0) | 5bc4c0 | 5fac10 | 609420 | 684080 |
| push_history (entry) | (8ce140) | (9007b0) | (930c10) | (93f9d0) | 9596e0 | 95c2f0 | 96d2b0 | 9e9880 | a4ae10 | a88dd0 | b89930 |
| guardian-refusal jno | 49fb87 | 4b5a97 | 4d1ae7 | 4d9687 | 4db522 | 4dc7ed | 4e066d =fixed | 517c55 | 5547b5 | 55b6f5 | 5cc021 |
| expanded-session flag (.data) | 209e848 | 2131740 | 219de18 | 21c8e78 | 22305f8 | 2235608 | 22a47f8 | 23bb118 | 24678b8 | 24cf9c8 | 27f7210 |
| client: player_killed prologue bytes (.rdata) | 199fc96 | 1a192fe | 1a74d9a | 1a9a122 | 1af266a | 1af5702 | 1b58f6a | 1c48242 | 1cdb3d2 | 1d2e832 | 1fec270 |
| client: its detour | 4b7350 | 4cd8d0 | 4f15b0 | 4f95b0 | 4fd620 | 4fea40 | 502d10 | 53edf0 | 57f970 | 58a200 | 601a90 |
| client: hook installer (call sites) | 580dd0 (58) | 59a1a0 (62) | 5c1970 (68) | 5ca0b0 (70) | 5cf9b0 (72) | 5d0e80 (73) | 5d6830 (74) | 60f700 (76) | 64d8d0 (79) | 65c5e0 (88) | 6ddb00 (93) |

### Dedicated exe (not loaded by our scripts), v0.8.14 .. v0.9.7

| point | v0.8.14 | v0.8.15 | v0.8.16 | v0.9.0 | v0.9.1 | v0.9.2 | v0.9.3 | v0.9.4 | v0.9.5 | v0.9.6 | v0.9.7 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| xaudio slot 1 jne | (118ee5) | (47ecc5) | (c72c5) | (5496d5) | (1f6c85) | d08a4 | d40b4 | d4614 | dcde4 | ddc34 | de3f4 |
| xaudio slot 2 jne | (118f46) | (47ed26) | (c7326) | (549736) | (1f6ce6) | d08db | d40eb | d464b | dce1b | ddc6b | de42b |
| xaudio slot 3 jne | (118fa7) | (47ed87) | (c7387) | (549797) | (1f6d47) | d0912 | d4122 | d4682 | dce52 | ddca2 | de462 |
| xaudio slot 4 jne | (118fe1) | (47edc1) | (c73c1) | (5497d1) | (1f6d81) | d0945 | d4155 | d46b5 | dce85 | ddcd5 | de495 |
| watchdog je (sig+0xa) | (77c5b) | (e3edb) | (19d80c) | (7cb4c) | (2dee9c) | (232a90) | (23421b) | (2348eb) | (2549cb) | (271b31) | (27b1c9) |
| voice_recv (entry) | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- |
| push_history (entry) | (1517a0) | (4b5880) | (30890) | (51f680) | (d5520) | (43d5b0) | (44b830) | (44c510) | (4ad5c0) | (4e25a0) | (4fd530) |
| guardian-refusal jno | 6f085 | db305 | 43fcf9 | 3bf4f5 | 2d18d1 | 12f36f | 12df3f | 12e49f | 152587 | 15b737 | 15c557 |
| expanded-session flag (.data) | -- | -- | -- | -- | -- | 1399e79 | 13b7011 | 13b7011 | 144f821 | 155e3e1 | 1596069 |
| client: player_killed prologue bytes (.rdata) | f7115f | f76147 | fae316 | ffdf68 | 1027829 | fba3f6 | fd34f6 | fd34f6 | 1056a3e | 11192c6 | 11485ae |
| client: its detour | 50f3a0 | 208bc0 | 449720 | 19d670 | 26eef0 | 13f1c0 | 13dd90 | 13e2f0 | 162970 | 169dc0 | 16e940 |
| client: hook installer (call sites) | 5143e0 (36) | 20c520 (37) | 44efc0 (40) | 1be570 (45) | 280820 (48) | 1c5920 (51) | 1c5100 (50) | 1c5540 (50) | 1e6f40 (56) | 1f5fd0 (60) | 1fb4a0 (58) |

### Dedicated exe, v0.9.8 .. v0.9.18

| point | v0.9.8 | v0.9.9 | v0.9.10 | v0.9.11 | v0.9.12 | v0.9.13 | v0.9.15 | v0.9.16 | v0.9.17 | v0.9.18 |
|---|---|---|---|---|---|---|---|---|---|---|
| xaudio slot 1 jne | e52c4 | ea504 | eac24 | e6ea4 | e7564 | e8184 | f3c24 | f46f4 | f6734 | ff484 |
| xaudio slot 2 jne | e52fb | ea53b | eac5b | e6edb | e759b | e81bb | f3c5b | f472b | f676b | ff4bb |
| xaudio slot 3 jne | e5332 | ea572 | eac92 | e6f12 | e75d2 | e81f2 | f3c92 | f4762 | f67a2 | ff4f2 |
| xaudio slot 4 jne | e5365 | ea5a5 | eacc5 | e6f45 | e7605 | e8225 | f3cc5 | f4795 | f67d5 | ff525 |
| watchdog je (sig+0xa) | (27ef18) | (2929f1) | (28f890) | (2984f0) | (29a220) | (29f760) | 2bee05 | 2c8fa4 | 2d5eb3 | (312f10) |
| voice_recv (entry) | -- | -- | -- | -- | -- | -- | 1fbe90 ? | 2058e0 ? | 2105d0 ? | 229a30 ? |
| push_history (entry) | (518520) | (5329c0) | (53a960) | 551730 | 554660 | 562f60 | 5aa9d0 | 5d2b90 | 6086f0 | 69f740 |
| guardian-refusal jno | 15ef27 | 1651a7 | 166087 | 164602 | 165b9d | 16a5ad | 17c335 | 17f675 | 1827a5 | 18eae1 |
| expanded-session flag (.data) | 15f4e71 | 164b509 | 16595a1 | 16c0cd9 | 16c5ce9 | 16f2e71 | 17bd711 | 1820ee1 | 1882fb1 | 1b0561b |
| client: player_killed prologue bytes (.rdata) | 119491e | 11dcd9a | 11e8e2a | 12410f2 | 1244132 | 126b2b2 | 131403a | 1368592 | 13b693a | 15e6770 |
| client: its detour | 171950 | 17f830 | 180b30 | 1814b0 | 182ba0 | 1879e0 | 19e310 | 1a5680 | 1ac110 | 1bf3a0 |
| client: hook installer (call sites) | 1fe740 (62) | 210680 (68) | 2120f0 (70) | 213540 (72) | 215b20 (73) | 21a880 (74) | 22d350 (76) | 237320 (79) | 241cb0 (88) | 25c160 (93) |

Coverage of the current signatures (client): ui-reentrancy 16/16 builds that have the code (v0.9.2+); xaudio 16/34
(v0.9.2+); watchdog 28/34 (all but v0.8.13 .. v0.9.1); set_muted 16/34 correct (v0.9.2+), **3 wrong** (v0.8.16, v0.9.0,
v0.9.1); voice_recv 4/34 (v0.9.15+); push_history 7/34 (v0.9.11+). All 34 builds have every point except
ui-reentrancy (absent before v0.9.2), the expanded-session flag (absent before v0.9.2) and the player_killed detour
(absent before v0.8.10); voice_recv before v0.9.2 and push_history before v0.9.6 are GUESSED (section 5).

## 4. Stable vs moving

**Every RVA moves in every release.** No point, code or data, has the same RVA in two consecutive client releases (the
only repeat anywhere is the dedicated exe's flag and .rdata bytes between v0.9.3 and v0.9.4). Even v0.8.4 -> v0.8.5,
with identical section sizes, moved everything. A fixed RVA is valid for exactly one build. CONFIRMED.

**Never changed** (byte-identical wherever the code exists):

- halo3.dll constants compiled into the exe. The XAudio2 check's slot RVAs halo3+0xa39e10/18/20/28 and expected values
  halo3+0x60c416 / 0x60c422 / 0x60c40a / 0x60c385 are the same in all 34 client and 21 dedicated exes, and each
  constant-compare-`jne` sequence occurs once per exe. The player_killed prologue bytes are the same from v0.8.10 on.
  They change only if MCC's halo3.dll changes, not with Reclaimer releases, which makes them the best anchors.
- Error strings used as anchors: "still running after .. seconds", "Guardian configuration refused",
  "Headless server audio off: XAudio2 and Waves are never loaded." / "XAudio2 import slots differ from the supported
  build; refused", "You cannot mute yourself", "details-mute", "Standalone MCC engine reached its native swap-chain
  present path" are in every build ("engine hang watchdog" only from v0.9.6).
- Rust/std code shapes: the `Option<Instant>` deadline test `cmp dword [..], 1000000000 ; je ; call Instant::now ; nop`
  (the 1e9 nanosecond niche), the RefCell cleanup `mov byte [rbx+0x68],1 ; cmp qword [rbx],0 ; jne ; mov qword [rbx],-1 ;
  cmp dword [rbx+0x10],1000000000` (v0.9.2+), the global `Mutex` lock `mov cl,1 ; xor eax,eax ; lock cmpxchg [rip+X],cl`.
- push_history's first 35 bytes (pushes, `sub rsp,0xa8`, `lea rbp,[rsp+0x80]`, `mov qword [rbp+0x20],-2`) in all 34.

**Moved, and the signature still finds it:** ui-reentrancy (v0.9.2+), xaudio (v0.9.2+), watchdog (v0.8.1 .. v0.8.12,
v0.9.2+), set_muted (v0.9.2+), push_history (v0.9.11+), voice_recv (v0.9.15+). The wildcards on branch displacements,
`rbp` stack displacements and rip-relative displacements are what make this work: those bytes differ in nearly every
build.

**Signatures that break:** see section 5. In short: the compiler re-allocates registers and stack slots, merges or
splits compare chains, picks rel8 vs rel32 branches, inlines helpers or not, and functions gain locals (frame size)
as features are added. Fields the developers add to structs move later fields (push_history's `+0xd8 -> +0xf8`).

## 5. Why each signature fails where it fails (CONFIRMED from the disassembly)

**ui-reentrancy, v0.8.1 .. v0.9.1: the code is not there.** No `cmp dword [reg+disp8],1000000000 ; jne ; xor eax,eax ;
jmp` cleanup exists anywhere in .text (searched with any register and offset). "Not found" is the right answer.
From v0.9.2 the same function also has a second, identical cleanup on `[rbx+0x58]` (v0.9.18 0x657461): the
`C6 43 68 01` field byte is what keeps the signature unique; wildcarding it gives 2 matches.

**xaudio, v0.8.1 .. v0.9.1: older implementation.** Each slot was read with `GetCurrentProcess` + `ReadProcessMemory`
into `[rsp+0x30]` instead of a helper returning a Result, so the signature's leading
`48 8B 44 24 20 48 89 C1 48 F7 D9 0F 81` (`mov rax,[rsp+0x20] ; mov rcx,rax ; neg rcx ; jno`) is absent. The compare
itself has four encodings:

| Builds | slots 1-3 | slot 4 |
|---|---|---|
| v0.9.2 .. v0.9.18 | `48 8D 83 <c> 48 39 44 24 28 0F 85 rel32` | `48 81 C3 85 C3 60 00 48 39 5C 24 28 75 rel8` |
| v0.8.1 .. v0.8.12 | `48 8D 83 <c> 48 39 44 24 30 0F 85` | `48 81 C3 85 C3 60 00 48 39 D8 0F 85` |
| v0.8.15, v0.9.1 | `48 8D 83 <c> 48 39 44 24 30 0F 85` | `48 81 C3 85 C3 60 00 48 39 5C 24 30 0F 85` |
| v0.8.13, v0.8.14, v0.8.16, v0.9.0 (+ dedicated v0.8.14 .. v0.9.1) | `49 8D 86 <c> 48 39 44 24 30 0F 85` (base r14) | `49 81 C6 85 C3 60 00 4C 39 F0 0F 85` |

**watchdog, v0.8.13 .. v0.9.1: the compiler merged the tail.** The patch site is identical (`81 BD d32 00 CA 9A 3B 74 xx
E8 rel32 90 48 8B 8D d32 48 39 C8 75`), but after it:

| Builds | bytes after `cmp rax,rcx` |
|---|---|
| v0.8.1 .. v0.8.12, v0.9.2+ | `75 0D 3B 95 d32 72 0E E9 ..` (`jne ; cmp edx,[rbp+X] ; jb ; jmp`) |
| v0.8.13 .. v0.9.1 (dedicated v0.8.14 .. v0.9.13) | `75 08 3B 95 d32 EB 03 48 39 C8 48 8B 8D d32 0F 83` (one shared `jae`) |
| dedicated v0.9.18 | the nanoseconds live in `ebx`: `81 FB 00 CA 9A 3B 74 24 E8 .. 90 48 8B 8D d32 48 39 C8 75 09 39 DA 72 0E` |

Before v0.9.6 the function has no "engine hang watchdog" text, only "still running after .. seconds"; it is the same
deadline (the worker runtime's engine thread wait). The places found in v0.8.1 .. v0.9.5 are in that function:
CONFIRMED by the string reference.

**set_muted, v0.8.1 .. v0.9.1: different prologue, and a false match.** Old shape (all builds up to v0.9.1):
`55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D` (extra
`push r14`, frame 0x30, unwind slot `[rbp-8]`; MUTED = +0x26 + disp32 at +0x22). The current signature is the v0.9.2+
shape `55 56 57 53 48 83 EC 38 .. 48 C7 45 00 FE ..`. In v0.8.16, v0.9.0 and v0.9.1 that shape also exists in a
**Forge** function (a `Mutex<Vec>` setter called by the placement code that logs "Frozen copies were created", "The new
size was placed, but the old object was not removed"): 0x414520, 0x951490, 0x1b4f00. With the stats page on, the
loader would call that function for a mute and read the Forge list as the mute list. Conversely, the old-shape
signature matches unrelated functions in v0.9.2+ (twice in v0.9.13+). Real set_muted was identified in every build as
the callee shared by the mute UI functions ("You cannot mute yourself", "details-mute", "scoreboard-mute") that locks a
global with `cl` and shares that global with exactly one `dl`-locked function (is_muted).

**voice_recv, v0.8.1 .. v0.9.13: the frame grows and registers move.** Found in every build as the non-UI caller of
is_muted that uses its 4th argument:

| Builds | `sub rsp,` | then |
|---|---|---|
| v0.8.1 .. v0.9.1 | 0x98 | `48 C7 45 10 FE FF FF FF 49 81 F9 89 02 00 00 75 14` (`cmp r9,0x289`: a fixed-size packet; GUESSED to be the older voice handler) |
| v0.9.2 .. v0.9.5 | 0xa8 | `48 C7 45 20 FE.. 4D 89 CF 4C 89 C3 49 89 D6 48 89 CF` |
| v0.9.6, v0.9.7 | 0xb8 | `48 C7 45 30 FE.. 4D 89 CE 4C 89 C3 49 89 D7 48 89 CF` |
| v0.9.8 | 0xc8 | `48 C7 45 40 FE..` (same registers) |
| v0.9.9, v0.9.10 | 0xe8 | `48 C7 45 60 FE..` |
| v0.9.11 .. v0.9.13 | 0x108 | `0F 29 75 70` (save xmm6, disp8) `48 C7 45 68 FE.. 4D 89 CE 4C 89 C3 ..` |
| v0.9.15 .. v0.9.18 | 0x118 | `0F 29 B5 80 00 00 00` (disp32) `48 C7 45 78 FE.. 4D 89 CE 4C 89 C7 49 89 D7 48 89 CB` |

The first 19 bytes (8 pushes + `sub rsp,imm32`) are position-independent in every build; only the imm32 differs.

**push_history, v0.8.1 .. v0.9.10: new arguments, then a new field.** The first 35 bytes never change; the next bytes:

| Builds | after `mov qword [rbp+0x20],-2` |
|---|---|
| v0.8.1 .. v0.9.1 | `49 89 D4 4C 8B 79 10` (`mov r12,rdx ; mov r15,[rcx+0x10]`) |
| v0.9.2 .. v0.9.5 | `48 89 CE 44 8B A5 90 00 00 00` (`mov rsi,rcx ; mov r12d,[rbp+0x90]`: a stack argument appears, GUESSED the timestamp) |
| v0.9.6 .. v0.9.10 | `80 B9 D8 00 00 00 00 0F 84` (history-enabled byte at chat+0xd8) |
| v0.9.11 .. v0.9.18 | `80 B9 F8 00 00 00 00 0F 84` (moved to chat+0xf8) |

The v0.8.1 .. v0.9.5 functions were found by shape (a function called 4-5 times that caps a deque at 100 entries,
`cmp r,0x64`, of 0x58-byte entries, `imul r,r,0x58`): GUESSED. `st_on_chat`'s `Line` layout was read in v0.9.18 only;
whether older builds use the same layout is not checked.

**guardian-refusal** (no signature in the loader): the result hub `xor eax,eax ; cmp rax,[rbp+X] ; jno <refusal>` exists
in every build (the `jno` target reaches the "Guardian configuration refused" log within 0x100 bytes), in five shapes:
`cmp rax,[rbp-0x58] ; jno rel8` + `test rsi,rsi` (most v0.8.x), `[rbp+disp32] ; jno rel32` + `test rsi,rsi` (v0.8.6,
v0.8.10), `jno rel32` + `mov rcx,rsi ; call` (v0.8.13, v0.8.16, v0.9.2 .. v0.9.4), `[rbp-0x48] ; jno rel8` + `mov rcx,rsi`
(v0.8.14, v0.8.15, v0.9.0, v0.9.1), `jno rel32` + `mov rcx,[rbp+X] ; call` (v0.9.5+). Where the hub sits in a small helper
the `jno` is 2 bytes (`71 xx`), so the patch is `90 90`, not 6 NOPs.

**expanded-session flag:** written once, `mov qword [rip+P],rdi ; mov byte [rip+F],1`, right after a
`xor eax,eax ; cmp rax,[rbp+X] ; jno` result check (v0.9.18: in FUNC 0x9e41b0, called from the custom-mode installer
FUNC 0x5a7b80, which also logs "Player tables moved" and installs the player_killed detour). Present from v0.9.2
(client and dedicated); no such writer before. The halo3 session offsets stats.c uses for the extended layout appear
as constants in the exe from different builds: `sess+0x1381c0` (peers) at 20+ sites from v0.9.2 (0-4 stray byte
matches before), `sess+0x1493c0` (players) at 13+ sites from v0.9.6 and nowhere before. So `st_session`'s extended
layout matches v0.9.6+; for v0.9.2 .. v0.9.5 the 128-player records are elsewhere (GUESSED from the absence of the
constant).

## 6. Advice for future patch runs

1. **Treat every RVA as single-build.** Look things up by signature at load time (as `g_sigpatches` does) and keep
   fixed RVAs only as a check. stats.c's `g_st_setmuted==g_base+0x66bea0` -> read `exe+0x27f7210` works for v0.9.18
   only; locate the flag with the writer signature below instead.
2. **Wildcard:** rel8/rel32 branch and call targets, rip-relative disp32, `rbp`/`rsp` stack displacements, the frame
   size (`sub rsp,imm32`) and the unwind slot byte in `48 C7 45 xx FE FF FF FF`, and struct field offsets of fields the
   client is still adding (push_history's `80 B9 ?? ?? 00 00 00`).
3. **Do not wildcard:** halo3 constants (they pin the exact compare), the `00 CA 9A 3B` Duration niche, `C6 43 68 01`
   in ui-reentrancy (the sibling cleanup on +0x58 would match too).
4. **Expect both branch lengths.** The same `jne`/`jno` is `75 xx` in one build and `0F 85 rel32` in another. A patch
   should NOP the instruction it finds, with its real length, after checking the opcode, not assume 6 bytes.
5. **Validate a match by meaning, not only by uniqueness.** A unique hit can be the wrong function (set_muted in
   v0.8.16 .. v0.9.1). Cheap checks that work in all 34 builds: set_muted's lock global must also be locked by an
   is_muted-shaped function (`56 48 83 EC 20 B2 01 31 C0 F0 0F B0 15 <disp>`, or `56 57 48 83 EC 28 B2 01 ..` up to
   v0.9.1); a watchdog/guardian/xaudio hit must sit in the function that references its log string.
6. **Entry hooks:** `st_voice_install`/`st_chat_install` pass the parsed pattern as the expected prologue to
   `entry_hook` (19 bytes). A `??` inside those 19 bytes becomes 0 and the `memcmp` fails silently. If the frame size is
   wildcarded (needed for voice_recv across builds), copy the real bytes from the image after `sig_find`.
7. **Good anchors, in order of reliability:** halo3.dll constants in the exe; log strings (`lea` of the text, then the
   function); call graph from a string-anchored function (set_muted, is_muted, voice_recv = is_muted's only caller
   outside the UI); Rust/std idioms (Mutex lock, Duration niche). Prologue shapes are the least stable: they changed for
   set_muted, voice_recv and push_history.

Replacement signatures, tested on all 34 client and 21 dedicated exes (unique, and at the anchor-confirmed place):

| Point | Signature | Patch / value | Finds |
|---|---|---|---|
| watchdog | `00 CA 9A 3B 74 ?? E8 ?? ?? ?? ?? 90 48 8B 8D ?? ?? ?? ?? 48 39 C8 75` | +4: `74` -> `EB` | all 34 client + all 21 dedicated (incl. the `cmp ebx` form) |
| xaudio #1 | `16 C4 60 00 48 39 44 24 ?? 0F 85` | +9: `0F 85` -> 6x `90` | all 55 exes |
| xaudio #2 | `22 C4 60 00 48 39 44 24 ?? 0F 85` | +9 | all 55 |
| xaudio #3 | `0A C4 60 00 48 39 44 24 ?? 0F 85` | +9 | all 55 |
| xaudio #4 | `85 C3 60 00 48 39 5C 24 ?? 75` / `85 C3 60 00 48 39 5C 24 ?? 0F 85` / `85 C3 60 00 48 39 D8 0F 85` / `85 C3 60 00 4C 39 F0 0F 85` | +9 (2 or 6 bytes) / +7 (6) | v0.9.2+ / v0.8.15, v0.9.1 / v0.8.1 .. v0.8.12 / v0.8.13, v0.8.14, v0.8.16, v0.9.0 (one of the four in every exe) |
| set_muted (up to v0.9.1) | `55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 0F 85` | MUTED = +0x26 + disp32@+0x22 | correct in v0.8.1 .. v0.9.1 with the is_muted pairing check (rule 5); together with the current signature: 34/34, no false match |
| push_history | `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 20 FE FF FF FF 80 B9 ?? ?? 00 00 00 0F 84` | entry hook, first 19 bytes concrete | v0.9.6+ (client and dedicated) |
| guardian-refusal | `31 C0 48 3B 85 ?? ?? ?? ?? 0F 81 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? E8` | +9: `0F 81` -> 6x `90` | v0.9.5+ (older: string anchor) |
| expanded-session flag | `31 C0 48 3B 45 ?? 0F 81 ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01` | flag = +0x1a + disp32@+0x15 | v0.9.2+ (client and dedicated) |

Not fixable with one fixed-length signature: voice_recv before v0.9.15 (frame size, `movaps` disp8 vs disp32 and
register choices all vary; wildcarding the frame alone still only finds v0.9.15+). Use the call graph (rule 7), or keep
the v0.9.15+ signature with the imm32 at +15 wildcarded and rule 6 applied. ui-reentrancy needs no change (stable in
every build that has the code).

## 7. Reproducing

The survey scripts are not in the repo. The method: map each exe's sections at their RVAs; run each signature as a
regex (`??` = any byte) over the executable sections, counting overlapping matches; index every `E8`/`E9` rel32 and
every rip-relative disp32 in .text once, so string `lea`s, callers and data references are dictionary lookups; take
function bounds from .pdata (following chained unwind info). Read the releases as data only: they are untrusted
binaries and are kept outside the repo.

## 8. Everything the loader touches

Static, 2026-10-17, same rules as above. Every byte the loader patches, every function it detours or calls, every
return address it compares and every variable it reads or writes, in **both** binaries, from `shim/src/*.c`.

**halo3.dll**: the MCC copy in the prefix (`.../Halo The Master Chief Collection/halo3/halo3.dll`), 11,127,768 bytes,
SHA-256 `b209d8454b12dc77e54ccd2c9924ec8d44b8619d21cf98e36ffaf601e67efb63`, PE timestamp 2025-08-16 20:45 UTC,
ImageBase 0x180000000, .text 0x6f811c bytes. **Only this one version is available locally**, so the halo3 signatures
below are verified unique in this file only; whether they survive an MCC update is untested. **exe**: client v0.9.18
(RVAs) and all 34 releases (coverage).

Signatures follow `sig_parse`/`sig_find`: `??` = any byte; every rel8/rel32 branch or call target and every
rip-relative disp32 is `??`. Each one matches **exactly once** in the executable section(s) and at the RVA given
(CONFIRMED by scanning). "Site" = the offset of the touched byte inside the signature. For data, the anchor is a unique
instruction sequence that references the variable: address = hit + (end of that instruction) + the disp32 stored in it.

### 8.1 halo3.dll: code we patch or detour

| RVA | What | How we touch it | Source | Found today | Proposed signature | Site | Original bytes at site |
|---|---|---|---|---|---|---|---|
| 0x342ed8 | object_delete | entry hook: 14 bytes compared, then relocated | peload.c `hbdel_install` -> hitbox.c `hb_on_delete` (off with `PELOAD_NO_DELHOOK`) | fixed RVA + prologue compare | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 50` | +0 | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54` |
| 0xb2250 | main_loop_body (frame safe point) | entry hook: 20 bytes compared + relocated | peload.c `hbdel_install` -> `hb_frame_hook` (teleport, bounds, stats voice mutes) | fixed + compare | `48 8B C4 57 48 81 EC 60 02 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 18` | +0 | `48 8B C4 57 48 81 EC 60 02 00 00 48 C7 44 24 30 FE FF FF FF` |
| 0x11b0ec | game_engine_display_event | entry hook: 17 bytes compared + relocated | stats.c `st_events_install` -> `st_on_display_event` (off with `PELOAD_NO_EVHOOK`) | fixed + compare | `40 53 48 83 EC 40 41 83 C8 FF 48 8B D9 44 39 41 0C 75 ??` | +0 | `40 53 48 83 EC 40 41 83 C8 FF 48 8B D9 44 39 41 0C` |
| 0x2d3f2c | chud_messaging_post | `entry_hook_tail`: 16 bytes compared; the replay tail re-runs `sub rsp,0x28 ; cmp ecx,4 ; jge`, loads the TLS index from the absolute address halo3+0xa39f9c and jumps to +0x10 (0x2d3f3c); r9 = the return address | stats.c `st_events_install` -> `st_on_chud` | fixed + compare. The 16 compared bytes contain a rip-relative disp32, so any rebuild breaks the compare even when the code is the same | `48 83 EC 28 83 F9 04 7D ?? 44 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00` | +0 (resume +0x10) | `48 83 EC 28 83 F9 04 7D 36 44 8B 0D 60 60 76 00` |
| 0x2ac24c | armor-skin key registration (sub_1802ac24c) | entry detour, 14 bytes relocated, **not compared**: `test r8,r8 ; jz -> xor eax,eax ; ret` | peload.c `skinguard_install` / `skinguard_one` (`PELOAD_SKINGUARD`) | fixed RVA, no check | `48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 C7 FE FF FF FF` | +0 | `48 8B C4 55 56 57 41 54 41 55 41 56 41 57` |
| 0x28e8e0 | per-section decompression cb builder (sub_18028e8e0) | trace detour `hook_one` id 0xdec0, 14 bytes relocated, not compared | peload.c `capcb1_install` (`PELOAD_D3DGL_CAPCB1`) | fixed, no check | `40 55 48 8B EC 48 83 EC 50 F3 0F 10 51 04 48 8D 55 D0` | +0 | `40 55 48 8B EC 48 83 EC 50 F3 0F 10 51 04` |
| 0x172bce | NULL-vtable release in the resource sweep (FUNC 0x172ad0) | mid-function detour, 17 bytes relocated, not compared; NULL vtable -> jump to 0x172bee | peload.c `vtguard_install` / `vtguard_one` (`PELOAD_VTGUARD`) | fixed RVA and fixed skip RVA | `75 ?? 49 8B 06 49 8B CE FF 10 83 C8 FF F0 41 0F C1 46 0C 83 F8 01 75 ?? 49 8B 06` | +2. Skip = the target of the leading `jne`: site + (int8)sig[1] | `49 8B 06 49 8B CE FF 10 83 C8 FF F0 41 0F C1 46 0C` |
| 0x172f9a | NULL-vtable release, shared_ptr release (FUNC 0x172e88) | same, 15 bytes; skip 0x172fb7 | same | fixed | `75 ?? 48 8B 03 48 8B CB FF 10 8B C7 F0 0F C1 43 0C 03 C7 75 ??` | +2. Skip = site + (int8)sig[1] | `48 8B 03 48 8B CB FF 10 8B C7 F0 0F C1 43 0C` |
| 0x37b816 | map boundary test (FUNC 0x37b6c8) | patch `0F 85 54 01 00 00` -> `E9 55 01 00 00 90` (same target; new rel32 = old rel32 + 1) | hitbox.c `g_hb_bnd` / `hb_bounds_set` (stats page `do=bounds`, written at the frame safe point) | fixed RVA; original or patched bytes compared before writing | `0F 85 ?? ?? ?? ?? 48 8D 95 C8 00 00 00 8B CF E8 ?? ?? ?? ??` | +0 | `0F 85 54 01 00 00` |
| 0x37b734 | second boundary test (same function) | `74` -> `EB` | same | same | `74 ?? 8B CF E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ??` | +0 | `74 41` |
| 0x219c1f | biped ground test (FUNC 0x2199c4) | `75 06` -> `EB 46`: jump to the target of the `je` at +6 (0x219c67) | same | same | `75 ?? F6 43 63 02 74 ?? 83 38 FF F2 0F 10 40 24` | +0. New rel8 = (int8)sig[7] + 6 | `75 06` |

Not patched, listed so nobody does: 0x1b8e00 (a boundary function whose entry the client checks; patching it is
refused, map-boundaries.md) and 0x118a1c game_engine_player_killed (the client detours it itself). Signatures in 8.5.

### 8.2 halo3.dll: functions we call

| RVA | Function | Source | Found today | Proposed signature (entry, site +0) |
|---|---|---|---|---|
| 0x259410 | object teleport (object, pos*, fwd*, up*, 1) | hitbox.c `hb_engine_jobs` / `hb_engine_jobs_locked` | fixed RVA; the first 16 bytes are compared before the first call | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 44 8B 15 ?? ?? ?? ??` |
| 0x3a3c4c | unit_exit_vehicle (unit, 0) | hitbox.c `hb_engine_jobs_locked` | fixed, no check | `48 89 5C 24 08 55 48 8B EC 48 83 EC 60 41 83 CB FF` |
| 0x9bc78 | scenario_location_from_point (u16* out, float* point) | hitbox.c `hb_engine_jobs_locked` | fixed, no check | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 44 8B 05 ?? ?? ?? ?? 83 CD FF 66 89 29` |
| 0x12c948 | game-state lock (returns a token) | hitbox.c `hb_engine_jobs` | fixed, no check | `48 83 EC 28 4C 8B 05 ?? ?? ?? ?? 33 D2 41 83 78 50 FF` |
| 0x12c9a0 | game-state unlock (&token) | hitbox.c `hb_engine_jobs` | fixed, no check | `48 89 5C 24 08 57 48 83 EC 20 83 39 01 48 8B F9` (16 generic bytes: unique today, the first to go ambiguous; it is the function the lock's callers call next with `&token`) |

### 8.3 halo3.dll: return addresses we compare

stats.c `st_on_chud` labels each HUD line by the return address of the call into chud_messaging_post. Each signature
covers the `call` (its rel32 is `??`); after a match, check that the call's target is the chud_messaging_post found
in 8.1.

| Return address | Caller | Proposed signature | Call at | Return = |
|---|---|---|---|---|
| 0x11b860 | game-engine event display (src 0; FUNC 0x11b6e8) | `48 8D 95 F0 04 00 00 8B CF E8 ?? ?? ?? ?? 8B 53 14` | +9 | hit + 0xe |
| 0x2d3f1b | post by string id (src 1; FUNC 0x2d3ee4) | `44 8B C7 48 8D 54 24 20 8B CB E8 ?? ?? ?? ?? 48 8B 9C 24 F0 02 00 00` | +0xa | hit + 0xf |
| 0x2ddd18 | one-time hint (src 2; FUNC 0x2ddc60) | `48 8D 54 24 30 8B CE E8 ?? ?? ?? ?? C6 84 1F 7C 1E 00 00 01` | +7 | hit + 0xc |

Without the leading `44 8B C7` the 0x2d3f1b signature also matches 0x2d5026, a twin helper that calls another function.

### 8.4 halo3.dll: data we read or write

| RVA | What | How | Source | Found today | Anchor signature | Address = |
|---|---|---|---|---|---|---|
| 0xa39f9c | static TLS index (u32); the TLS block is `gs:[0x58][index]` | read | hitbox.c (TLS block lookups, `hb_engine_jobs`, `hb_on_delete`, the snapshot), stats.c (players, betrayal), the chud replay tail | fixed | the chud_messaging_post signature (8.1) | hit + 0x10 + disp32@+0xc (6413 references in .text) |
| 0x1e250d0 | network session pointer (u64) | read | stats.c `st_session` | fixed | `48 83 EC 28 45 33 C9 44 38 0D ?? ?? ?? ?? 74 ?? 4C 8B 15 ?? ?? ?? ??` (FUNC 0x126cc) | hit + 0x17 + disp32@+0x13 |
| 0x8f4028 | engine-ready pointer (u64, non-null once the game state exists) | read | hitbox.c `hb_engine_jobs` | fixed | the game-state lock signature (8.2) | hit + 0xb + disp32@+7 |
| 0x46bb780 .. 0x46bb7f0 | WavesFX_* function-pointer table (15 slots; WavesFX_Initialize = 0x46bb7c8) | write: no-op stubs, Initialize returns a dummy context | peload.c `waves_prefill` | fixed | `48 81 EC 18 01 00 00 48 8D 05 ?? ?? ?? ?? 48 89 44 24 30 48 8D 05 ?? ?? ?? ?? 48 89 44 24 38` (FUNC 0x25a720, which builds {slot, name} pairs for GetProcAddress) | Initialize = hit + 0xe + disp32@+0xa; its name `lea` at +0x13 points at "WavesFX_Initialize". Each further `lea rax,[rip+slot]` / `lea rax,[rip+name]` pair gives one more slot |
| 0xa39e10 / 18 / 20 / 28 | XAudio2 import slots | not touched by the loader; read and replaced by the client's headless-audio code (whose check we NOP) | exe | the exe embeds these four RVAs as constants | `FF 15 ?? ?? ?? ?? F6 47 10 04 F3 42 0F 10 94 3E B8 01 00 00` (a `call [slot1]`) | slot 1 = hit + 6 + disp32@+2; slots 2-4 = +8 / +0x10 / +0x18 |
| 0x21c2398 | the `_Mtx` from the thread-id note (info) | none | peload.c comment | -- | `48 83 EC 38 48 C7 44 24 20 FE FF FF FF BA 02 01 00 00 48 8D 0D ?? ?? ?? ??` (FUNC 0x5170) | hit + 0x19 + disp32@+0x15 |
| HaloScript globals | `PELOAD_H3=name=value` | write | peload.c `h3set_install` | by name at run time ("\0name\0" -> the 24-byte table entry -> storage) | -- | nothing to update |

### 8.5 halo3.dll: referenced only in comments and docs (info)

| RVA | What | Function | Signature | Site |
|---|---|---|---|---|
| 0x13003c | data_array_new | FUNC 0x13003c | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 4C 8B 74 24 70` | +0 |
| 0x130244 | data array header user (hitbox.c note) | FUNC 0x130244 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B DA 41 8B F8` | +0 |
| 0x33f5bf | "object" array creation (call site) | FUNC 0x33f2dc | `48 8D 0D ?? ?? ?? ?? 89 15 ?? ?? ?? ?? 45 8D 41 18` | +0 |
| 0x3457d8 | object_set_permutation | FUNC 0x3457d8 | `48 89 5C 24 18 89 54 24 10 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50` | +0 |
| 0x345d38 | object_set_permutation (2nd) | FUNC 0x345d38 | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 44 8B 15 ?? ?? ?? ?? 49 8B F0` | +0 |
| 0x2aacf0 | armor skin RB-tree | FUNC 0x2aacf0 | `48 89 5C 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 50` | +0 |
| 0x2af4ef | D3DReflect call site | FUNC 0x2af4b8 | `FF 15 ?? ?? ?? ?? 85 C0 0F 88 ?? ?? ?? ?? 48 8B 4D 38` | +0 |
| 0x13f23a | main-thread memcpy call site | FUNC 0x13f168 | `E8 ?? ?? ?? ?? 49 8B 84 FC A0 4A 00 00 8B EB 4C 8B 70 10` | +0 |
| 0x9d22c | old ms timer (no longer hooked) | FUNC 0x9d22c | `40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ?? BA 80 00 00 00` | +0 |
| 0xfcc20 | betrayal-popup condition | FUNC 0xfcc20 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 44 8B 05 ?? ?? ?? ?? 33 DB 65 48 8B 04 25 58 00 00 00 48 8B F2 0F B7 C9 48 69 F9 F8 02 00 00` | +0 |
| 0xff5fc | betrayal popup post | FUNC 0xff5fc | `48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40 0F 29 74 24 30` | +0 |
| 0x118a1c | game_engine_player_killed (**the client detours it: do not hook**) | FUNC 0x118a1c | `48 8B C4 48 89 58 08 44 88 48 20 44 89 40 18 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 B9` | +0 |
| 0x1b8e00 | boundary function the client checks (**do not patch**) | FUNC 0x1b8e00 | `48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 45 32 FF` | +0 |
| 0x172ad0 | refcounted-resource sweep (vtguard site 1) | FUNC 0x172ad0 | `48 8B C4 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC` | +0 |
| 0x172e88 | shared_ptr release (vtguard site 2) | FUNC 0x172e88 | `48 8B C4 4C 89 40 18 57 48 83 EC 30 48 C7 40 E8 FE FF FF FF 48 89 58 08 48 89 70 10 49 8B F8` | +0 |

Debug-only fixed addresses, not maintained: the `hook_log` trace modes (halo3+0x76a0b0 callback table, id 0xd15) and
the decompressor fault dump (halo3+0x5fc000..0x601000, 0x5fec1a); v0.9.13 exe globals 0x2263c60/64 and
0x2264458..0x2264950 (trace ids 0x2263c / 0xca70); and the user-supplied RVAs of `PELOAD_PATCH`, `PELOAD_HOOK`,
`PELOAD_BP`, `PELOAD_POKE` and `PELOAD_PEEK`.

### 8.6 Reclaimer exe (client)

Coverage = releases (of 34) where the signature matches once **and** at the place the anchor search (sections 3-5)
found; no recommended signature matches a wrong place in any release.

| Item | v0.9.18 RVA | What / how | Source | Found today | Recommended signature | Site | Original -> new (v0.9.18) | Coverage |
|---|---|---|---|---|---|---|---|---|
| ui-reentrancy | 0x65703d, 0x657051 | patch, on by default | peload.c `g_sigpatches` / `reentrancy_fix` | signature (same) | `C6 43 68 01 48 83 3B 00 0F 85 ?? ?? ?? ?? 48 C7 03 FF FF FF FF 81 7B 10 00 CA 9A 3B 75 ?? 31 C0 EB ??` | +8, +0x1c | `0F 85 rel32` -> 6x `90`; `75 xx` -> `90 90` | 16/34 = every build that has the code (v0.9.2+) |
| xaudio #1 / #2 / #3 | 0x53d134 / 0x53d16b / 0x53d1a2 | patch (`PELOAD_RECLAIMER_PATCHES`) | peload.c `g_sigpatches` "xaudio" | signature, 16/34 | `16 C4 60 00 48 39 44 24 ?? 0F 85` / `22 C4 60 00 ...` / `0A C4 60 00 ...` | +9 | `0F 85 rel32` -> 6x `90` | 34/34 |
| xaudio #4 | 0x53d1d5 | patch | same | signature, 16/34 | four forms, section 6 | +9 or +7 | `75 75` -> `90 90` | 34/34 |
| watchdog | 0x7963f6 | patch je -> jmp (`RECLAIMER_DIRECT=1` only) | peload.c `g_sigpatches` "watchdog" | signature, 28/34 | `00 CA 9A 3B 74 ?? E8 ?? ?? ?? ?? 90 48 8B 8D ?? ?? ?? ?? 48 39 C8 75` | +4 | `74 28` -> `EB 28` | 34/34 |
| set_muted | 0x66bea0 | called (`rcx` id, `dl` muted) from the frame hook | stats.c `st_voice_install`, `st_voice_jobs` | signature + is_muted pairing (since 2026-10-17) | current (v0.9.2+) and old shape `55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 0F 85`, each accepted only if paired | +0 | -- | 34/34 |
| is_muted (pairing only) | 0xb29fd0 | scanned: must lock the same byte as set_muted | stats.c `st_voice_install` | 13-byte scan of the image | `56 48 83 EC 20 B2 01 31 C0 F0 0F B0 15 ?? ?? ?? ??` (v0.9.2+) / `56 57 48 83 EC 28 B2 01 31 C0 F0 0F B0 15 ?? ?? ?? ??` (up to v0.9.1); several matches by design | lock byte = hit + 0x11 + disp32@+0xd (old: + 0x12, @+0xe) | -- | pairs in 34/34 |
| MUTED | 0x27d7848 | read (`Vec` ptr +0x10, len +0x18) | stats.c `st_session` | derived from set_muted | set_muted + 0x24 + disp32@+0x20 (old shape: + 0x26, @+0x22) | -- | -- | 34/34 |
| voice_recv | 0x684080 | entry hook, 19 bytes compared + relocated | stats.c `st_voice_install` -> `st_on_voice` | signature | current | +0 | `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00` | 4/34 (v0.9.15+); other builds by call graph (section 5) |
| push_history | 0xb89930 | entry hook, 19 bytes | stats.c `st_chat_install` -> `st_on_chat` | signature, 7/34 | `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 20 FE FF FF FF 80 B9 ?? ?? 00 00 00 0F 84` | +0 | `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 00 00 00` | 12/34 (v0.9.6+) |
| expanded-session flag | 0x27f7210 | read byte (0 = 16-player, 1 = 128-player layout) | stats.c `st_voice_init`, `st_session` | **fixed RVA**, only when set_muted sits at 0x66bea0 | `31 C0 48 3B 45 ?? 0F 81 ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01` (the single writer, v0.9.18 0x9e4a42) | writer `C6 05` at +0x13 | flag = hit + 0x1a + disp32@+0x15 | 16/34 = every build that has it (v0.9.2+) |
| guardian-refusal | 0x5cc021 | patch jno -> 6x NOP (manual `PELOAD_PATCH`) | CLAUDE.md / RE notes: `exe+4e066d:909090909090` | **fixed RVA**, v0.9.13 only | `31 C0 48 3B 85 ?? ?? ?? ?? 0F 81 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? E8` | +9 | `0F 81 36 0B 00 00` -> 6x `90` | 13/34 (v0.9.5+; older builds: string anchor, section 5) |
| xaudio presets | = the xaudio sites | manual `PELOAD_PATCH=exe+460294,...` | CLAUDE.md | **fixed RVAs**, v0.9.13 only | the xaudio signatures above | | | |

**The workshop-download command line** (not a patch). peload.c's `CreateProcessW` replaces the client's
`<exe> workshop-download ...` child with `shim/tools/workshop-helper.sh`. It takes the item id from the token after
`--subscribe` or `--update` and the MCC path from `--mcc-path`; with no id it starts `/usr/bin/true`, which prints
nothing. The argv the client builds (CONFIRMED by string references in the spawner functions; the id is formatted
inline with a divide-by-10 loop):

| Builds | Spawners and argv | Effect under the loader |
|---|---|---|
| v0.8.1 .. v0.8.11 | none ("workshop-download" is not in the exe) | -- |
| v0.8.12 .. v0.9.1 | two spawners, both `workshop-download --mcc-path <path>` (+ env `SteamAppId`/`SteamGameId`); neither formats an item id | no id: `/usr/bin/true`, so the client sees "Workshop helper has no output" (GUESSED: these builds passed the item some other way, or the helper only reported subscriptions) |
| v0.9.2 .. v0.9.5 | main spawner `workshop-download --mcc-path <path> [--update] <id>`; a second one without an id | the id is found only with `--update`. GUESSED: a first download passes the id without a flag, so the loader misses it |
| v0.9.6 .. v0.9.18 | main spawner (v0.9.18 exe+0x803290) `workshop-download --mcc-path <path> [--update] [--subscribe] <id>`; the item-less one (v0.9.18 exe+0x8096a0) passes only `--mcc-path <path>` | works as designed (workshop-helper.md); the item-less call gets `/usr/bin/true` |

## 9. Re-finding the halo3.dll items after an MCC update

1. Compare the new halo3.dll's SHA-256 with the one in section 8. If it differs, every fixed RVA in 8.1-8.4 is
   suspect. The loader already refuses the prologue-checked hooks (object_delete, main_loop_body, display_event,
   chud post, the teleport call) and checks the boundary bytes. These would write or jump blind: skinguard, capcb1,
   vtguard, the four unchecked calls (unit_exit_vehicle, scenario_location_from_point, lock, unlock), and
   `waves_prefill`, which writes 15 pointers at fixed .data RVAs on every load. The fixed-RVA reads (TLS index,
   session, engine-ready) would return garbage. Gate all of them on a signature match first.
2. Run each signature over .text. Exactly one match = take it; check the original bytes at the site before
   patching. No match or several: anchor first on something semantic, then rebuild the signature around it:
   - the chud_messaging_post callers, through their return-address signatures (8.3);
   - the TLS index, through any `gs:[0x58]` read (`65 48 8B 04 25 58 00 00 00` next to a `mov r32,[rip+disp]`);
   - the WavesFX table, through the "WavesFX_Initialize" string (`lea` in FUNC 0x25a720);
   - the session pointer and the game-state lock, through their callers;
   - the boundary sites, through the kill/safe-zone logic in map-boundaries.md.
3. Recompute what depends on positions, not just the site:
   - chud post's replay tail: the TLS index address (from the disp32 at +0xc) and the resume point (+0x10);
   - vtguard skip targets: the leading `jne`'s target;
   - the 0x37b816 patch: `E9` + (old rel32 + 1) + `90`;
   - the 0x219c1f patch: `EB` + ((int8)sig[7] + 6);
   - data addresses: the disp32 formulas in 8.4.
4. The exe side moves with **MCC** too. The client's XAudio2 check embeds halo3+0xa39e10..28 and the retail values
   halo3+0x60c416/22/0a/385, and its own player_killed detour checks halo3's 15-byte prologue. A Reclaimer build for
   a new MCC changes those constants, so the xaudio signatures here (which contain them) must be rebuilt from the new
   halo3 RVAs (slot RVAs = the `call [rip+disp]` targets in 8.4).
5. Prefer copying compared prologue bytes from the image after a signature match. Don't keep them as constants in
   the source: a constant breaks on any rebuild that moves a rip-relative operand (chud post) and hides the real
   reason in a "prologue differs" log line.
