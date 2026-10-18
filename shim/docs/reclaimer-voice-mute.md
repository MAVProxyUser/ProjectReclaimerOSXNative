# Reclaimer client: per-player voice mute and volume (static RE)

Client `client/project-reclaimer-client-v0.9.18.exe` (ImageBase 0x140000000; addresses are exe RVAs unless marked
`halo3+`). Found with `re-tools/refs.py`, `re-tools/hdis.py` and rip-relative/call xref scans only; nothing was run.
CONFIRMED = read in the disassembly. GUESSED = inferred, check it at runtime before relying on it.

**In use since 2026-10-17** (stats.c, stats page "Connected" list): per-player Mute / Unmute and Mute all through
`set_muted` (section 6), the talking highlight from the `voice_recv` hook, the host badge (section "Host peer").
Checked live: in an 89-player lobby a page mute was read back from the client's own MUTED list and unmuted again;
against our local dedicated server the client reported host peer 0, its own peer 1, "dedicated server". Every RVA
here is v0.9.18 only; other client builds: patch-addresses.md.

## Summary

Mute is a process-global `Mutex<Vec<PeerId>>`, changed through one free function, `set_muted(id, bool)` at
**exe+0x66bea0**. It takes no `self`, no Arc and no UI state. Received voice packets check the list once per packet
(`is_muted`), so a change takes effect on the next packet and the client's own buttons show it.

`PeerId` is the 8-byte **Halo network-session peer (machine) id**. Halo 3's session membership in halo3.dll links
it to the player's session record, which holds the name and peer index. The loader can therefore build the
name/slot -> voice id map from halo3 memory, then call `set_muted` on a guest thread.

## 1. UI -> state (CONFIRMED)

| What | Where |
|---|---|
| Player details "MUTE VOICE"/"UNMUTE VOICE" (`details-mute`) | FUNC 0x171180. `r14 = record+0x88` (the voice id), `is_muted(r14)` @0x174e22 picks the label, `get_volume(r14)` @0x174e54, click -> `set_muted(r14, !muted)` @0x175051. The button is disabled when the record is the local player (byte at closure+0x28 -> "You cannot mute yourself") or when `record+0x88 == 0` ("Voice is not connected"). |
| Scoreboard row speaker (`scoreboard-mute`) | row drawer FUNC 0x2562b0, rows are 0xb8 bytes: `is_muted(row+0x88)` @0x258200, `set_muted` @0x258649 |
| Scoreboard side panel (`scoreboard-side-mute` / `-volume`) | FUNC 0x25fea0 (is_muted 0x26436e, set_muted 0x264568, get_volume 0x263bac, set_volume 0x263f4b) and FUNC 0x267820 (0x26bf8c / 0x26c186 / 0x26b7c6 / 0x26bb65); details volume slider set_volume @0x1753a3 |
| Other readers | is_muted @0x39d3b0 (FUNC 0x39c6c0, a player card speaker icon; there `card+0x88 == -1` means no voice) |

**MUTED** = `static Mutex<Vec<PeerId>>` at **exe+0x27d7848** (in .data):
`+0x00` futex byte (0 free, 1 locked, 2 locked with waiters), `+0x01` poison flag, `+0x08` cap, `+0x10` ptr,
`+0x18` len. Elements are 8 bytes and the initial dangling pointer is 1, so the type has align 1 (an `[u8; 8]`-like
id; read it as a little-endian u64). A data-xref scan finds only `set_muted`, its cold unlock path 0x66c080 and
`is_muted` touching it.

- `set_muted` **exe+0x66bea0**, Win64: `rcx` = id (u64 by value), `dl` = muted (bool). It locks, removes every equal
  entry (retain), pushes the id if muted is true, then unlocks (WakeByAddressSingle when contended). It never touches
  TLS unless the global panic count (exe+0x27f85b0) is nonzero. It may grow the Vec (the Rust allocator ->
  HeapAlloc/HeapReAlloc).
- `is_muted` **exe+0xb29fd0**: `rcx` = id -> `al`. Linear scan under the lock; returns false if the lock is poisoned.

## 2. Per-player volume (CONFIRMED, separate state)

**VOLUMES** = `static Mutex<Vec<(PeerId, f32)>>` at **exe+0x27d7870** (same header layout: cap +8, ptr +0x10,
len +0x18). Elements are 12 bytes: id at +0, f32 at +8, align 4.
- `get_volume` exe+0xb25520: `rcx` = id -> `xmm0`. Returns the float at .rdata 0x1f3d0f8 when the id is absent
  (GUESSED 1.0).
- `set_volume` exe+0x67af40: `rcx` = id, `xmm1` = volume. It clamps to [0, float at 0x1f3db04] (GUESSED 2.0 =
  "200 %"), maps NaN/inf to 0, then updates the entry or pushes a new one.

Mute does not use this list: a muted speaker keeps its volume, and volume 0 silences without counting as muted.

## 3. Playback: where it is applied (CONFIRMED)

The voice UDP loop (FUNC 0x68cb00) parses each datagram with 0x6829a0, which checks the 4-byte `MC?1`/`LC?1` magic
and a 16-byte session token. The 8 bytes at datagram+0x14 are the **sender id**; the 8 bytes at +0x1c are the
recipient, which the loop compares with `[state+0x290]` (GUESSED to be the same own-id field as VoiceClient+0x290).
The payload starts at +0x24. The loop then calls:

**`voice_recv` exe+0x684080**: `rcx` = voice net state, `rdx` = sender PeerId (u64), `r8` = payload, `r9` = len.
The payload is: [0] channel (1 global, 2 team, 3 proximity), [1..9] sequence number, [9..] Opus.
- Drops its own echo (sender == shared+0x9c0) and senders in a second global `Mutex<Vec<u64>>` at exe+0x27cfa20.
  That list is only checked in one connection mode and is replaced wholesale by 0x8671a0 / 0x682f60 / 0x87fd80
  (GUESSED: a server-side block list; the UI never touches it).
- Checks channel membership (0xb25990 / 0xb25d30) and computes the proximity gain (0xb256a0, from a peer-id ->
  position map).
- @0x6847c7 `play = audible && !is_muted(sender)`. When `play` is false, the Opus decode (0xb2cb50 @0x684884) is
  skipped and only the stream bookkeeping (0x684ad8) runs, so the packet is dropped before decode.
- @0x684920 `get_volume(sender)` scales the decoded stream.

## 4. Voice id -> player (layout CONFIRMED, meaning partly GUESSED)

The roster builder exe+0x76c0a0 (called @0x775edf) reads Halo 3's network session through halo3.dll:
`sess = *(u64*)(halo3+0x1e250d0)` (halo3 .data). The caller first requires `*(u32*)(sess+0x67b70)` to be 4 or 6.
0x59ca80, a validity check on the same pointer, tests `sess+0x4144 == 3`.

Layout. `X` = the byte **exe+0x27f7210**: Reclaimer's expanded-session flag, set @0x9e4a55 after it patches
halo3 for 128 slots.

| | X == 0 | X == 1 |
|---|---|---|
| player valid mask | `sess+0x126c`, 16 bits | `sess+0x126c`, 128 bits |
| player i record (stride 0xc8) | `sess+0x1270+i*0xc8` | `sess+0x1493c0+i*0xc8` |
| peer valid mask | `sess+0x54`, 17 bits | `sess+0x54`, 128 bits |
| peer p record (stride 0x110) | `sess+0x58+p*0x110` | `sess+0x1381c0+p*0x110` |

- Player record: `+0x04` u64 player identifier (records with 0 are skipped: "a native player record has no
  identifier of its own"), `+0x0c` i32 **peer index** (must have its bit set in the peer mask), `+0x98` UTF-16 name
  (wchar[16]; "Player" when unreadable), `+0xb8` u32 (<0x40, stored next to the entry).
- Peer record: `+0x00` 8-byte **peer id = the voice id** (copied to roster entry +0x90), `+0x08` u32 state (the
  builder requires 8).
- Roster entries are 0xa0 bytes: name String at +0, player identifier at +0x88, peer id at +0x90. The scoreboard row
  drawer finds a row's roster entry by `entry+0x90 == row+0x88` (loop @0x2567b0), falling back to the name. That is
  why row+0x88 (what the mute button passes) is the peer id. That this 0xa0 list is the roster built by 0x76c0a0 is
  GUESSED (same size and field layout).
- GUESSED consequences: split-screen players on one PC share one peer id, so one mute silences all of them. A
  reconnect may get a new peer id, and the old mute then no longer applies.
- GUESSED: session player index `i` == the Halo players-table slot that stats.c reports. stats.c therefore pairs a
  session record with a players-table row only when the index equals the slot **and** the names match; the
  "Connected" list itself comes from the session alone (it works in the lobby, where the players table is empty).
- Own id: VoiceClient+0x290, where VoiceClient = the Arc in `static Mutex<Option<Arc<..>>>` at exe+0x27dd660 (Arc
  pointer at exe+0x27dd668; accessor 0x68c790 clones it). Do not mute it.

## 5. Persistence (CONFIRMED by absence)

Neither list is saved. `struct AudioSettings` only has master/game/menu/voice channel volumes, mic, talk mode and
similar settings, with no per-player keys. MUTED and VOLUMES start empty in .data and are only written by
set_muted / set_volume, so mutes last for the process lifetime.

## 6. Recommended loader hook

1. **Locate** (byte signatures, unique in .text for 0.9.18):
   - `set_muted`: `55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 0F 85`.
     The MUTED address is `set_muted + 0x24 + disp32 at set_muted+0x20`.
   - `set_volume`: `55 56 57 53 48 83 EC 48 48 8D 6C 24 40 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 48 89 CE 66 0F 7E C8 25 FF FF FF 7F`
   - `voice_recv`: `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 00 00 00 48 C7 45 78 FE FF FF FF 4D 89 CE 4C 89 C7 49 89 D7 48 89 CB`
   - `is_muted`'s prologue is a generic Mutex-lock prologue (3 matches); read MUTED directly instead.
2. **Map** (stats.c, in its 100 ms snapshot; ordinary reads of known addresses, no sweeping): read `sess`, the
   flag `X`, the player mask and records, and the peer ids. Publish per player `"voice":"<16 hex>"` and `"muted"`.
   For `"muted"`, either scan MUTED's ptr/len (racy but bounded; ptr can be freed by a concurrent push, so prefer
   copying under the futex byte: cmpxchg 0->1, copy, xchg 0, call the shim's WakeByAddressSingle if the old value
   was 2), or call `is_muted` on the guest thread below.
3. **Apply**: queue `(id, muted)` from the HTTP handler and run it on a guest thread. stats.c does it in
   `st_voice_jobs`, called from `hb_frame_hook` (peload.c), the entry hook on main_loop_body (halo3+0xb2250, the
   engine main thread once per frame; the old halo3+0x9d22c timer hook is gone), like the teleport job:
   `((void(__attribute__((ms_abi))*)(uint64_t,uint8_t))(exe+0x66bea0))(id, 1 or 0);`
   Any thread works for the state itself; the UI calls it from the egui thread, and it only touches the global
   Mutex plus allocator imports. A guest thread just keeps it like every other guest call. Per-player volume works
   the same way: `((void(ms_abi*)(uint64_t,float))(exe+0x67af40))(id, v)` (the float lands in xmm1).
4. **Optional "talking" indicator**: `entry_hook(exe+0x684080, <first 19 bytes>, 19, fn)`. The first 19 bytes (pushes
   + `sub rsp,0x118`) are position-independent, and `rdx` is the speaking peer id for every voice packet.
5. **Do not** detour `is_muted`. Its rip-relative `lock cmpxchg` starts at +9, so `entry_hook` cannot relocate 14
   bytes, and a private mute list would desync from the client's own Mute/Unmute buttons.

What stats.c does now: `st_voice_install` (at exe load, stats page on) finds set_muted by the signature above and
keeps it only if an is_muted-shaped function (`56 48 83 EC 20 B2 01 31 C0 F0 0F B0 15 <disp32>`) locks the same byte
(in clients 0.8.16 .. 0.9.1 the signature hits a Forge setter instead; patch-addresses.md section 5); it hooks
voice_recv (`st_on_voice`) and push_history (the chat feed, halo3-event-feed.md). `st_session` reads MUTED's ptr/len
directly (no lock) for the page's "muted" flags; flag `X` is read at its fixed v0.9.18 RVA only when set_muted sits at
exe+0x66bea0, other builds pick the layout whose records are valid. Per-player volume is not used.

Runtime check: done 2026-10-17 (89-player lobby): a page mute was read back from the client's MUTED list, and
unmute removed it.

## 7. Microphone under the loader (wasapi.c)

The Mac microphone is the client's WASAPI capture endpoint (AudioQueue input), so its voice chat starts once instead
of rebuilding a playback stream every ~5 s (the old caulk crash trigger). `PELOAD_MIC` unset = the real microphone
(a microphone that cannot be opened falls back to silence); `PELOAD_MIC=0` = a silent microphone (an endpoint that
delivers silence: the voice chat still starts once); `=none` = no capture device (the old, crash-prone behaviour);
`=tone` = a quiet test tone. `PELOAD_MIC_MUTED=1` starts muted; stats page `do=mic&on=0|1` (real microphone or tone
only). The client sends voice only while its talk key is held (Hold) or toggled on (Toggle). quiet-run.sh uses
`PELOAD_MIC=0` (`QR_MIC=tone` for the tone); target.sh runs without WASAPI (`PELOAD_NO_WASAPI=1`).

## Host peer

Static RE (halo3.dll + client 0.9.18); checked at runtime 2026-10-17 against our local dedicated server (the joined
client read host peer 0, local peer 1, no player on peer 0 = "dedicated server"; stats.c `st_session`, the page's
★ HOST badge). The membership object is at `sess+0x30` (host
create halo3+0xdbac: `lea rbp,[rcx+0x30]` is passed as `this` to the membership calls), and its shared part starts at
`sess+0x38` in Halo 3's usual order: update number, leader, host, slot counts, peer count, peer mask (+0x54), peers (+0x58).

| field | offset (both layouts) | status |
|---|---|---|
| update number (-1 = no membership yet) | `sess+0x38` | CONFIRMED |
| leader peer index | `sess+0x3c` | CONFIRMED |
| **host peer index** | `sess+0x40` | CONFIRMED |
| **local peer index** | `sess+0x3dc4` | CONFIRMED |
| incremental update number | `sess+0x3dc8` | CONFIRMED |
| local state | `sess+0x67b70` (4 = established peer, 6..9 = host states) | CONFIRMED that 6..9 gates host-only code; names GUESSED |

Evidence:
- halo3+0x33a454 is a UI text token that resolves to the host player's name. It loads `eax=[sess+0x40]`, walks the
  player mask `sess+0x126c`, and matches `[sess+0x127c+i*0xc8]` (player +0x0c peer index) against it. It then calls
  0x2b9154(i), which returns `&player[i]+0x20`, and copies `+0x78` (= record +0x98, the name).
- halo3+0xdbac creates the host session. With no membership to restore (0xdcf2 -> 0xde21), it adds peer 0 in state 4
  (0x1a000 with edx=0), then `mov qword [sess+0x3c], r13` (r13=0: leader = host = 0), `mov [sess+0x3dc4], r13d`
  (local = 0), and `inc [sess+0x38]`. On the restore path (host migration) it sets each valid peer's state to
  `peer == [sess+0x40] ? 4 : 2` (0xdd2a).
- halo3+0x10338 runs only in host states 6..9. It sets the leader to local (`[0x3c]=[0x3dc4]`, bumps 0x38/0x3dc8),
  then commits (call 0x9cf8) only if `[0x38] != -1 && [0x40] != -1 && [0x3dc4] == [0x40]`. Only the host broadcasts
  membership changes.
- Every is-leader check uses `[sess+0x38] != -1 && [sess+0x3dc4] == [sess+0x3c]` (e.g. 0x17cd1, 0x16d8d, 0x126f6).
- The 128-slot layout keeps these offsets. Reclaimer's 128-slot patcher (exe+0x9e41b0) copies only the arrays: old
  +0x58 / +0x1f18 / +0x1270 / +0x3130 / +0x3dcc go to +0x1381c0 / +0x140ac0 / +0x1493c0 / +0x14f8c0 / +0x155dc0.
  Its own team-balance commit, exe+0x9b8f1f..0x9b8f62, is the same idiom on the same session (players at
  +0x1270 / +0x1493c0 chosen by flag X): `[sess+0x38]++`, `ecx=[sess+0x40]`, `[sess+0x3dc8]++`,
  `cmp [sess+0x3dc4],ecx`, `jmp halo3+0x9cf8`. exe+0x9bd83e (128-peer loop) reads `[sess+0x3dc4]` to skip its own
  peer.
- GUESSED by absence: the client exe never reads `sess+0x40`. The callers of its guarded read exe+0x75d320 read only
  +0x67b70, +0x3dc4, +0x24 and +0x4144 from the session, so its roster and scoreboard have no host marker.

Dedicated server: a freshly created host session puts the hosting machine at **peer 0**, so leader = host = local = 0
there (CONFIRMED static path). A Reclaimer dedicated server has no local player, so on the server and on every
client `host == 0` (the indices are shared through the membership updates) and no player record has peer index 0.
On a session hosted by a player, that player (and any split-screen players on the same machine) have peer == host.
Read `sess+0x40` on every snapshot; do not hard-code 0. The restore path (host migration) keeps the old peer
indices, so a migrated host is not peer 0, and the field can be -1.

stats.c implements it (`st_session`: `g_st_hostpeer` / `g_st_localpeer`, valid only while the update number is not -1
and the state is 4..9). The sketch it was written from:
```c
// stats.c snapshot, after sess and lay (flag X) are read as in the voice map above
int32_t st=0, upd=-1, host=-1, local=-1;
hb_read(sess+0x67b70,&st,4); hb_read(sess+0x38,&upd,4);
if(st>=4 && st<=9 && upd!=-1){ hb_read(sess+0x40,&host,4); hb_read(sess+0x3dc4,&local,4); }
int nq = lay?128:17;
if(host<0 || host>=nq || !peer_bit(sess+0x54,host)) host=-1;   // peer mask: 17 / 128 bits at sess+0x54
int self_is_host = (st>=6 && st<=9);                           // equivalently local==host
int host_has_player = 0;
for(each valid player i){                                      // rec = sess+(lay?0x1493c0:0x1270)+i*0xc8
    int32_t peer = *(int32_t*)(rec+0x0c);
    e->host  = (host>=0 && peer==host);  host_has_player |= e->host;
    e->local = (local>=0 && peer==local);
}
// host>=0 && !host_has_player  ->  "dedicated server (peer <host>)"; host<0 -> no session / unknown
```
