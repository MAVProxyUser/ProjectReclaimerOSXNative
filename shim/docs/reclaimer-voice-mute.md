# Reclaimer client: per-player voice mute and volume (static RE)

Client `client/project-reclaimer-client-v0.9.18.exe` (ImageBase 0x140000000; addresses are exe RVAs unless marked
`halo3+`). Found with `re-tools/refs.py`, `re-tools/hdis.py` and rip-relative/call xref scans only; nothing was run.
CONFIRMED = read in the disassembly. GUESSED = inferred, check it at runtime before relying on it.

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
- GUESSED: session player index `i` == the Halo players-table slot that stats.c reports. Check by comparing the
  name at record+0x98 with players-table +0x58, and fall back to matching by name.
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
3. **Apply**: queue `(id, muted)` from the HTTP handler and run it on a guest thread. For example, the existing
   `hb_frame_hook` (halo3+0x9d22c entry hook, halo3 main loop) like the teleport job:
   `((void(__attribute__((ms_abi))*)(uint64_t,uint8_t))(exe+0x66bea0))(id, 1 or 0);`
   Any thread works for the state itself; the UI calls it from the egui thread, and it only touches the global
   Mutex plus allocator imports. A guest thread just keeps it like every other guest call. Per-player volume works
   the same way: `((void(ms_abi*)(uint64_t,float))(exe+0x67af40))(id, v)` (the float lands in xmm1).
4. **Optional "talking" indicator**: `entry_hook(exe+0x684080, <first 19 bytes>, 19, fn)`. The first 19 bytes (pushes
   + `sub rsp,0x118`) are position-independent, and `rdx` is the speaking peer id for every voice packet.
5. **Do not** detour `is_muted`. Its rip-relative `lock cmpxchg` starts at +9, so `entry_hook` cannot relocate 14
   bytes, and a private mute list would desync from the client's own Mute/Unmute buttons.

Runtime check before shipping (one run, no sweeps): log the session peer ids, click "Mute voice" in the client's
scoreboard, and check that the id appended to MUTED (exe+0x27d7858/0x27d7860) matches the peer id this map gives
for that player.
