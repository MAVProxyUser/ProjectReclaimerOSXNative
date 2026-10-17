# halo3.dll (MCC) game variant and player traits

Found 2026-10-17 by static RE only (capstone/angr on
`$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`,
ImageBase 0x180000000; all addresses below are RVAs, `halo3+0x...`). Nothing was read from a live process.
**CONFIRMED** = the offset/encoding is read or written that way in code (evidence given). **GUESSED** = name or
meaning inferred (H3/ElDewrito naming, validator order in `game-options.md`, MCC setting-id order), offset itself
may still be confirmed.

## 1. Where the active variant lives

Same thread-local block as players (+0x10) and objects (+0x38): `tls = gs:[0x58][*(u32*)(halo3+0xa39f9c)]`.

| What | Address | Status | Evidence |
|---|---|---|---|
| game_globals | `gg = *(u64*)(tls + 0x48)` | CONFIRMED | `game_difficulty_get` evaluate (halo3+0x1f0fcc) -> halo3+0xeff14 reads `[tls+0x48]`, `gg+0x10`, `gg+0x16c` |
| game mode | `u8 gg+0x10` (1 campaign, 2 multiplayer) | CONFIRMED (values GUESSED) | halo3+0x14b086, halo3+0x149911 (`cmp byte [gg+0x10], 2` before any MP trait work) |
| **c_game_variant** | **`v = gg + 0x1608`**, 0x4C4 bytes | CONFIRMED | `current_game_variant()` = halo3+0x14b068: `lea rax,[gg+0x1608]`, returns it only if the engine index fits the game mode. Size: halo3+0xdff5e memsets a stack variant of 0x4c4 before decoding an `mpvr` chunk into it |
| engine index | `i32 v+0x00` | CONFIRMED | halo3+0x14b09c; halo3+0xbf130 switch |
| current engine object | `game_engines[ *(i32*)(*(tls+0x20) + 0x13e1c) ]`, table halo3+0x8f3c60 | CONFIRMED | `current_game_engine()` = halo3+0x14b02c |
| per-mode variant pointers | `*(u64*)halo3+0x46b7760` CTF, `+7768` Slayer, `+7770` Oddball, `+7778` KOTH, `+7780` VIP, `+7788` Juggernaut, `+7790` Territories, `+7798` Assault, `+77a0` Infection; `+7648` Sandbox | CONFIRMED | each engine's init calls variant vtable slot 0x48 (`get_<mode>_variant(type, out)`, e.g. halo3+0xbe72c) with `rcx = v+4`; so every pointer == **v+4** (the base variant). Null/stale when that mode isn't running -- prefer `v` |

Engine indices (CONFIRMED from each engine's trait code below): 1 CTF, 2 Slayer, 3 Oddball, 4 King of the Hill,
5 Sandbox/Forge, 6 VIP, 7 Juggernaut, 8 Territories, 9 Assault, 10 Infection. 11 and 13 are campaign-mode engines
(halo3+0x14b0a0; 11 has no engine object), 12 is a multiplayer engine with streak/tier traits = Gun Game (GUESSED
from the `gun_game_*` setting strings).

`current_game_variant()` returns NULL unless (engine 1..10 or 12, and game mode 2). A reader should check
`*(u8*)(gg+0x10) == 2` and `1 <= engine <= 12` itself.

The base variant (`c_game_engine_base_variant`) starts at **v+4** with an 8-byte vtable pointer (the struct is
4-byte packed). Below, offsets are given from `v` (add 0x1608 for gg-relative); "base+" = v+4+.

## 2. Base variant layout (from v)

Evidence: base copy/validate halo3+0x1915d8 (field-by-field clamp of every field), metadata copy halo3+0x191b98,
mpvr fix-up halo3+0xbf4e8, plus the readers listed.

| v+ | Type | Field | Range / encoding | Status |
|---|---|---|---|---|
| 0x00 | i32 | engine index | see above | CONFIRMED |
| 0x04 | ptr | vtable (runtime) | | CONFIRMED |
| 0x0C | 0xF8 B | metadata (s_saved_game_item_metadata) | | CONFIRMED (size) |
| 0x0C | u64 | unique id | | GUESSED |
| **0x14** | wchar[16] | **variant name (UTF-16)** | terminator forced at v+0x32 | CONFIRMED (wcsncpy 0x10 at halo3+0x191c5f) |
| 0x34 | char[128] | description | terminator v+0xB3 | CONFIRMED |
| 0xB4 | char[16] | author | terminator v+0xC3 | CONFIRMED |
| 0x104 | u8 | misc flags: bit0 teams, bit1 reset players each round, bit2 reset map each round | 3 bits copied | bits CONFIRMED, names GUESSED (bit0 -> engine "teams" flag at halo3+0xfa820) |
| 0x105 | u8 | round time limit (minutes, 0 = none) | <= 60, default 8 | CONFIRMED |
| 0x106 | u8 | round limit | <= 10, default 1 | CONFIRMED |
| 0x107 | u8 | early victory win count | <= 5, default 2 | CONFIRMED |
| 0x108 | u8 | respawn flags: bit0 inherit respawn time, bit1 respawn with team (synchronize), bit2 respawn at location, bit3 respawn on kills | 4 bits | bits CONFIRMED, names GUESSED (MCC ids 5..8: `respawn_modifiers_inheritance` .. `respawn_modifiers_on_kill`) |
| 0x109 | u8 | lives per round (0 = unlimited) | <= 50 | CONFIRMED |
| 0x10A | u8 | team lives per round | <= 100 | CONFIRMED (memset into the engine's per-team lives, halo3+0xfa80e) |
| 0x10B | u8 | respawn time (s) | <= 240, default 5 | CONFIRMED |
| 0x10C | u8 | suicide penalty (s) | <= 240 | CONFIRMED |
| 0x10D | u8 | betrayal penalty (s) | <= 240 | CONFIRMED |
| 0x10E | u8 | respawn growth (s) | <= 15 | CONFIRMED |
| 0x10F | u8 | respawn traits duration (s) | <= 60, default 5 | CONFIRMED (halo3+0x1499e4) |
| **0x110** | traits | **respawn traits** | | CONFIRMED (halo3+0x149a0e) |
| 0x12C | u16 | social flags: **bit0 friendly fire**, **bit1 betrayal booting**, bits 2-4 (enemy voice, open channel voice, dead player voice) | 5 bits | bit0 CONFIRMED (halo3+0xfe988: teammate damage is 0 unless bit0), bit1 CONFIRMED (halo3+0x1b8a00, called by the boot-betrayer poster halo3+0xff5fc), bits 2-4 GUESSED |
| 0x12E | u16 | team changing: 0 disabled, 1 enabled, 2 balancing only | <= 2 | CONFIRMED (copy halo3+0x190164), names from validator |
| 0x130 | u32 | map override flags: bit0 grenades on map, bit1 indestructible vehicles | 2 bits | bits CONFIRMED, names GUESSED |
| **0x134** | traits | **base player traits** (everyone) | | CONFIRMED (engine vfunc 0xd8 = halo3+0x202904) |
| 0x150 | i16 | weapon set: -2/-1 special (random / map default, GUESSED), >= 0 index into the map's MP weapon sets | | CONFIRMED (validator halo3+0x190600) |
| 0x152 | i16 | vehicle set: -1 map default, >= 0 index | | CONFIRMED (halo3+0x1906f0) |
| **0x154** | traits | **red powerup traits** | | CONFIRMED (halo3+0x149bc3) |
| **0x170** | traits | **blue powerup traits** | | CONFIRMED (halo3+0x149bba) |
| **0x18C** | traits | **yellow powerup traits** | | CONFIRMED (halo3+0x149bb1) |
| 0x1A8 / 0x1A9 / 0x1AA | u8 x3 | red / blue / yellow powerup duration (s) | <= 120 (defaults 5 / 30 / 30) | CONFIRMED (halo3+0x149b10..0x149afe) |
| 0x1AC | u16 | base variant flags (bit0 kept on copy) | | CONFIRMED offset |
| 0x1AE | i16 | team scoring method | <= 3 | CONFIRMED offset, meaning GUESSED |
| 0x1B0 | u32 | zeroed by the base copy (pad) | | CONFIRMED |
| 0x1B4.. | | per-mode data (section 4) | | |

## 3. c_player_traits (0x1C bytes)

Field layout CONFIRMED by four independent code paths: the default trait set built in the per-tick trait update
(halo3+0x149944), the "all unchanged" template (halo3+0x149b66 / halo3+0x149516), the merge function
`c_player_traits::apply` halo3+0x1494bc (sub-structs at +0, +8, +0x10, +0x14, +0x18), and the per-field range checks
in the player store (halo3+0x149c00). **0 = Unchanged** in every field (the merge skips 0); a value outside the
range is rejected. Enum order = the validator order in `game-options.md` (index 0 = "Unchanged"), cross-checked
against halo3's own MCC settings descriptor tables (.data, e.g. shields at halo3+0x909520: option entries
`{u32 default_flag, u32 value, u64 0, name, desc}`).

| +off | Type | Trait | Values (code range) | Default (halo3+0x149944) |
|---|---|---|---|---|
| 0x00 | u8 | damage resistance | 1 10%, 2 50%, 3 90%, 4 100%, 5 110%, 6 150%, 7 200%, 8 300%, 9 500%, 10 1000%, 11 2000%, 12 invulnerable, 13 invulnerable incl. assassinations (MCC) | 4 |
| 0x01 | u8 | shield recharge rate | 1 -25%, 2 -10%, 3 -5%, 4 0%, 5 50%, 6 90%, 7 100%, 8 110%, 9 200% | 7 |
| 0x02 | u8 | vampirism | 1 off, 2 10%, 3 25%, 4 50%, 5 100% | 1 |
| 0x03 | u8 | headshot immunity | 1 immune, 2 vulnerable | 2 |
| 0x04 | u8 | shield multiplier | 1 none, 2 1x, 3 2x, 4 3x, 5 4x | 2 |
| 0x05-0x07 | | pad (always 0) | | 0 |
| 0x08 | u16 | initial grenades | 1 map default, 2 none | 1 |
| 0x0A | i8 | primary weapon | **-2 unchanged** (not 0!), -1 map default, -3 random, >= 0 index into the MP globals weapon list | -1 |
| 0x0B | i8 | secondary weapon | same | -1 |
| 0x0C | u8 | damage modifier | 1 0%, 2 25%, 3 50%, 4 75%, 5 90%, 6 100%, 7 110%, 8 125%, 9 150%, 10 200%, 11 300%, 12 instant kill | 6 |
| 0x0D | u8 | grenade regeneration | 1 on, 2 off, 3 on incl. firebombs | 2 |
| 0x0E | u8 | infinite ammo | 1 normal, 2 infinite, 3 bottomless clip | 1 |
| 0x0F | u8 | weapon pickup | 1 allowed, 2 disabled | 1 |
| 0x10 | u8 | movement speed | 1 0.25, 2 0.5, 3 0.75, 4 0.9, 5 1.0, 6 1.1, 7 1.25, 8 1.5, 9 2.0, 10 3.0 (floats read from halo3+0x148f1c) | 5 |
| 0x11 | u8 | gravity | 1 0.5, 2 0.75, 3 1.0, 4 1.5, 5 2.0 (halo3+0x148fc4) | 3 |
| 0x12 | u8 | vehicle use | 1 none, 2 passenger only, 3 full | 3 |
| 0x13 | | pad | | 0 |
| 0x14 | u8 | active camo | 1 off; 2/3/4 = camo amount 0.333 / 0.667 / 1.0 (halo3+0x149fa6) -> validator labels Poor / Good / Invisible (GUESSED; MCC's own UI shows only 1 off, 3 "poor", 4 "good") | 1 |
| 0x15 | u8 | waypoint | 1 off, 2 allies, 3 everyone | 2 |
| 0x16 | u8 | aura | 1 off, 2 team color, 3 black, 4 white (labels GUESSED, range 1..4 CONFIRMED) | 1 |
| 0x17 | u8 | forced change color | 1 off, 2..13 = color0..color11 | 1 |
| 0x18 | u16 | motion tracker mode | 1 off, 2 allies only, 3 normal, 4 enhanced | 3 |
| 0x1A | u16 | motion tracker range | 1..7 (MCC labels 1 10m, 3 25m, 5 75m, 7 150m; 2/4/6 GUESSED 15/50/100m) | 3 |

Weapon special values: -3 CONFIRMED random (halo3+0x14ace7 rolls the RNG at tls+0x550), -2 CONFIRMED unchanged
(skipped by the store, used by the "unchanged" template), -1 default. ">= 0 follows the validator's weapon order"
(Battle rifle = 0 ...) is GUESSED.

## 4. Per-player ACTIVE traits (players array element, 0x2F8 bytes)

**The player datum holds its resolved, currently active traits: `c_player_traits` at player +0x1FC (to +0x217).**
CONFIRMED: halo3+0x149c00 (`player_set_traits(player_index, traits*)`) range-checks each field of a trait set and
writes it to `player + 0x1FC + field`, raising network-update bits per sub-struct (2 shield/vitality, 4 weapons,
8 movement, 0x10 appearance, 0x20 sensors). Never contains 0 ("unchanged") once set; the attacker's damage
multiplier is read from it at halo3+0xfea2b (`player+0x204` sub-struct), and player spawn initialises it with the
same defaults (halo3+0xe2af4).

It is recomputed every tick for every player (per-player tick halo3+0xfccf8 -> halo3+0x1498d8), only in
multiplayer (`gg+0x10 == 2`) and not when `gg+0x11 == 4`:
1. start from the defaults (table above);
2. engine vfunc 0xd8: apply **base player traits** (v+0x134);
3. engine vfunc 0xe0: apply the **mode role traits** if the player has the role (table below);
4. halo3+0x149a48: apply the **latest powerup's traits** while it lasts;
5. apply **respawn traits** (v+0x110) while `player+0x1D4 (i16) > 0` and `player+0x1D6 == 0` and !(`player+4` & 8);
6. store at player +0x1FC.
Later steps override earlier ones field by field (non-zero fields only).

Supporting player fields (CONFIRMED offsets):
| Player + | Type | Meaning |
|---|---|---|
| 0x1D4 | i16 | respawn-traits ticks remaining (set at spawn halo3+0xe7aec, decremented every tick halo3+0xfcda3) |
| 0x1D6 | u8 | respawn-traits suppressed when non-zero |
| 0x1D8 / 0x1DC | i32 / f32 | an extra timed powerup: pickup tick / duration in seconds; when newer than the three below it applies the BLUE set (halo3+0x149b1e; meaning GUESSED) |
| 0x218, 0x21C, 0x220 | i32 x3 | game tick when the red / blue / yellow powerup was picked up (0 = none; cleared at halo3+0xe61c7). The highest one is active while `(now - tick) * seconds_per_tick <= duration(v+0x1A8+i)` |

Game time for that test: `t = *(u64*)(tls + 0xc8)`; `*(i32*)(t+0xC)` = current tick, `*(f32*)(t+0x8)` = seconds per
tick (halo3+0x149b47).

## 5. Per-mode data and role trait sets

Offsets from v (= base + 4). "Role" = condition the engine's vfunc 0xe0 checks (CONFIRMED in the listed function);
role names follow the H3 / validator naming (GUESSED where the condition is ambiguous).

| Engine | Score to win (vtable slot 0x40) | Trait sets at v+ | Role condition (function) |
|---|---|---|---|
| 1 CTF | i16 v+0x1BC | **0x1C0 flag carrier** | holds a weapon whose MP object type is 1 (flag) (halo3+0x19db18 -> halo3+0x14b0c4) |
| 2 Slayer | i16 v+0x1B4 | **0x1C4 leader** | halo3+0x14b1d0 says the player (or its team) is in first place (halo3+0x19f148) |
| 3 Oddball | i16 v+0x1B8 | **0x1C4 ball carrier** | holds MP object type 2 (ball) (halo3+0x19fcc8) |
| 4 KOTH | i16 v+0x1B8 | **0x1C0 in-hill** | `player+0x23C (u16) != 0` (halo3+0x1a1b60) |
| 5 Sandbox | -- | **0x1B8 editor** | player is editing (halo3+0x116390 -> halo3+0x1115fc) |
| 6 VIP | i16 v+0x1B4 | **0x1C6 VIP team**, **0x1E2 VIP proximity (influence)**, **0x1FE VIP** | VIP: halo3+0x1a420c / 0xfb72c; proximity: inside the team VIP's influence radius (halo3+0x39959c); team: flags bit0 at v+0x1B6 (halo3+0x1a33f8) |
| 7 Juggernaut | i16 v+0x1B4 | **0x1C4 juggernaut** | halo3+0x1a5440 (halo3+0x1a4c8c) |
| 8 Territories | -- | **0x1BC defender**, **0x1D8 attacker** | inside a territory owned by own team / another team (halo3+0x1a6bdc) |
| 9 Assault | i16 v+0x1BC | **0x1C8 bomb carrier**, **0x1E4 arming** | holds MP object type 1; arming = team arm timer > 0 (halo3+0x1a9f30) |
| 10 Infection | -- | **0x1C0 zombie**, **0x1DC alpha zombie**, **0x1F8 safe haven**, **0x214 last human** | zombie: halo3+0x1ad084; alpha: bit in zombie mask (engine globals +0xe19e); last human: == halo3+0x1ad4d8(); haven: in radius (halo3+0x1ac254) |
| 12 Gun Game | i16 v+0x1BE | **0x2D0 final tier**, **0x2EC leader**, **0x308 + k*0x1C** k = 0..4: 1..5 kills in a row | halo3+0x1add8c (leader = halo3+0x14b1d0, final tier = tier == max; streak >= k+1) |

Slayer (CONFIRMED by its copy/validate halo3+0x19256c and getters halo3+0xbe740..): v+0x1B4 i16 score to win
(-1..500, default 25), v+0x1B6 i16 kill points (-10..10, default 1), then s8 points (-10..10) v+0x1B8 assist,
+0x1B9 death, +0x1BA suicide (default -1), +0x1BB betrayal (default -1), +0x1BC leader killed, +0x1BD elimination,
+0x1BE assassination, +0x1BF headshot, +0x1C0 beatdown, +0x1C1 sticky, +0x1C2 splatter, +0x1C3 killing spree
(order = validator order = MCC setting ids 136..149), **v+0x1C4 leader traits**.

The mode trait offsets in the table were read as `[mode variant pointer] + X` (pointer = v+4) and converted to v+;
the copy functions list the same sets as base+: CTF 0x1BC, Slayer 0x1C0, Oddball 0x1C0, KOTH 0x1BC, Sandbox 0x1B4,
VIP 0x1C2/0x1DE/0x1FA, Juggernaut 0x1C0, Territories 0x1B8/0x1D4, Assault 0x1C4/0x1E0, Infection
0x1BC/0x1D8/0x1F4/0x210, Gun Game 0x2CC/0x2E8 (+ the streak array). Gun Game's trait code uses `v` directly.

Oddball (copy halo3+0x1931f4; names GUESSED by MCC id order 171..182): v+0x1B4 u32 flags (auto pickup, ball
effect), v+0x1B8 score to win, v+0x1BA carrying points, v+0x1BC kill, v+0x1BD ball kill, v+0x1BE carrier kill
points, v+0x1BF ball count, v+0x1C0 initial ball delay (s, <= 120), v+0x1C2 ball respawn delay, v+0x1C4 carrier
traits. The other modes' non-trait fields are clamped in their copy functions (CTF halo3+0x191eb0, KOTH
+0x1939c4, Sandbox +0x19738c, VIP +0x1959dc, Juggernaut +0x196764, Territories +0x194b40, Assault +0x195100,
Infection +0x1941a0, Gun Game +0x197878) -- not named yet.

Each per-mode copy function ends with a memset to v+0x4C4 (Slayer: `memset(base+0x1dc, 0, 0x2e4)`), confirming
the 0x4C4 variant size.

## Reading it from the loader (sketch)

```c
uint8_t *tls = /* halo3 TLS block of the simulation thread, as stats.c already does */;
uint8_t *gg  = *(uint8_t **)(tls + 0x48);
if (gg && gg[0x10] == 2) {
    uint8_t *v = gg + 0x1608;
    int32_t engine = *(int32_t *)v;                 /* 1..10, 12 */
    const uint16_t *name = (const uint16_t *)(v + 0x14);   /* 16 UTF-16 chars */
    const uint8_t *base_traits = v + 0x134, *respawn = v + 0x110;
    const uint8_t *red = v + 0x154, *blue = v + 0x170, *yellow = v + 0x18c;
    /* per player: active traits */
    const uint8_t *active = player + 0x1fc;         /* 0x1C bytes, layout of section 3 */
}
```

## 6. Local player

Found 2026-10-17, static only (same halo3.dll). The engine keeps an explicit user -> player mapping, so the local
player never has to be guessed from the camera. Function names are ours (H3 naming, GUESSED); the offsets and
behaviour are CONFIRMED by the instructions cited.

### 6.1 player mapping globals: `pm = *(u64*)(tls + 0x110)`, 0xE8 bytes (the answer)

halo3+0xe1b28 (players init) allocates three game-state blocks in the same TLS block as players/objects: the players
data array (16 x 0x2F8) -> tls+0x10, "players globals" (0x18C) -> tls+0x40, **"player mapping globals" (0xE8) ->
tls+0x110** (`mov ecx,0x110; mov [rcx+rbx],rax` at halo3+0xe1c4c). Reset function halo3+0xedec4 (-1 everywhere,
counts and +0xD8 zeroed).

| pm+ | Type | Meaning | Evidence |
|---|---|---|---|
| 0x00 | i16 | active input users | inc/dec in halo3+0xedf70 |
| 0x02 | i16 | active controllers | halo3+0xee040 |
| 0x04 | i32[4] | input user -> player handle | set halo3+0xedf70 (`[pm+4+user*4] = player`), get halo3+0xee0d4 |
| 0x14 | i32[4] | input user -> unit handle | halo3+0xee264 |
| 0x24 | i32[4] | controller -> player handle | halo3+0xee040 |
| 0x34 | i32[16] | player index -> controller | halo3+0xee040 |
| 0x74 | i32[16] | player index -> input user | halo3+0xedf70, get halo3+0xee100 |
| 0xB4 | i16 | active output users | halo3+0xee2ec |
| **0xB8** | **i32[4]** | **output user -> player handle** (the player that view shows) | set halo3+0xee2ec, get `player_mapping_get_player_by_output_user` halo3+0xee45c (`mov eax,[pm+rcx*4+0xb8]`, ~70 callers: HUD, director, cameras, sound) |
| 0xC8 | i32[4] | output user -> unit handle (-1 while dead) | set halo3+0xee684, get halo3+0xee48c; unit -> output user search halo3+0xee4bc |
| 0xD8 | u8[16] | per player index: bitmask of output users showing that player | bts/btr in halo3+0xee2ec, iterator halo3+0xee504 |

Values are datum handles (salt << 16 | index), -1 = none; callers index the players array with the low 16 bits
(`movzx ecx,ax; imul rdx,rcx,0x2f8`, halo3+0x131b22). "Output user" = a view (split-screen viewport), "input user" =
a controlling local user; for a local player both are the player's machine user index (player +0x1C, below).
halo3+0xee574 = output user active (`pm+0xB8[u] != -1`), halo3+0xee5a8 = next active output user (scans u = 0..3).

When it changes (CONFIRMED):
- `player_new` (halo3+0xe2928): if the new player's machine index (+0x1A) == the local machine index (pg+0x7C),
  maps input user, controller and **output user = player+0x1C** to it (halo3+0xe2c8b..0xe2ca9).
- `players_set_local_machine` (halo3+0xe3574): when the local machine changes, unmaps everyone and remaps every
  player of the new local machine the same way (halo3+0xe36e2..0xe370d).
- `player_set_unit_index(player, unit)` (halo3+0xe3748; with player_new the only writer of player+0x28) calls
  halo3+0xee1f4(player, unit), which rewrites pm+0x14 and pm+0xC8 for every user showing that player. So the unit
  columns follow death (-1) and respawn (new biped); **the player columns do not change on death/respawn**.
- Only when halo3+0xf01a0 is true (`game_globals+0x160 != 0`; GUESSED = saved-film playback; halo3+0xe2170 also
  tests the byte at halo3+0x2137151, next to the film playback speed halo3+0x2137154 that
  `saved_film_set_playback_game_speed` writes), halo3+0xe2170 rebuilds the output users (falling back to any player
  when the local machine has none) and the director re-points them at the watched player (halo3+0x132694 ->
  halo3+0xee2ec). Outside that, pm+0xB8 only ever holds players of the local machine.

### 6.2 players globals: `pg = *(u64*)(tls + 0x40)`, 0x18C bytes

| pg+ | Type | Meaning | Evidence |
|---|---|---|---|
| 0x04 | u8 | input disabled | `player_enable_input` evaluate halo3+0x1f11ac writes `!arg` |
| 0x08 | u32 | valid machine mask (17 machines) | halo3+0xe35ca (`bt` over 0x11 machines) |
| 0x0C | 17 x 6 B | machine identifiers | halo3+0xe344a (block copy), compared per machine at halo3+0xe35d8 |
| 0x72 | u8 | local machine exists | halo3+0xe369f |
| 0x76 | 6 B | local machine identifier | halo3+0xe36ad |
| **0x7C** | **i32** | **local machine index (-1 none)** | written halo3+0xe36a3; compared with player+0x1A at halo3+0xe2259, +0xe2c36, +0xe36ea |

### 6.3 Player element fields (players array, 0x2F8 bytes; written by player_new halo3+0xe2928)

| Player+ | Type | Meaning | Status |
|---|---|---|---|
| 0x04 | u32 | flags; bit 1 (0x2) = left / inactive (player_new sets it from the join info; the iterator halo3+0xe72b0 skips such players) | CONFIRMED offset, meaning GUESSED |
| 0x08 | u64 | player identifier | CONFIRMED (key compared in halo3+0xeca8c) |
| 0x14 | 6 B | machine identifier | CONFIRMED |
| **0x1A** | **i16** | **machine index; the player is local iff `== pg+0x7C`** (and pg+0x7C != -1) | CONFIRMED |
| **0x1C** | **i16** | machine user index 0..3 = input user = output user of a local player | CONFIRMED |
| 0x20 | i32 | controller index | CONFIRMED (passed to halo3+0xee040) |
| 0x28 | i32 | unit handle (-1 while dead) | CONFIRMED |
| 0x2C | i32 | previous / dead unit: set to -1 by player_set_unit_index (which hands the old value to halo3+0x347344 first, halo3+0xe3869); cameras use it when +0x28 is -1 (halo3+0x131b30, halo3+0x131ea2) | offset CONFIRMED, "corpse" GUESSED |

Reverse link: **unit datum +0x178 = owning player handle** (player_set_unit_index writes it on the new unit at
halo3+0xe3839 and -1 on the old one at halo3+0xe37cd; read by halo3+0xee12c and halo3+0xee624), +0x17C = last owning
player (GUESSED). Gives biped -> player without scanning the players array.

### 6.4 Camera / director (bonus)

- **director globals** `dg = *(u64*)(tls + 0x188)` (0x640 bytes, allocated at halo3+0x1309f4): one director per
  output user, `dir = dg + u*0x180` (halo3+0x131310: `lea rbx,[rax+rax*2]; shl rbx,7`); director type i32 at
  `dg + 0x600 + u*0xC` (halo3+0x13162d). CONFIRMED.
- `dir+0x158` i32 output user, `dir+0x15C` watched player handle (set by halo3+0x132694; the base update
  halo3+0x131704 sets it to get_player_by_output_user when it is -1). CONFIRMED.
- `dir+0x08` = the camera object; its type = vtable slot 0, so the vtable identifies it (CONFIRMED values):
  0 halo3+0x7fb748, 1 +0x7fb7b0, 2 +0x7fb818, 3 +0x7961d0, 4 +0x7fb888, 5 +0x7fb8f0, 6 +0x7fb548, 7 +0x7fb958,
  8 +0x7fb228. Names following / orbiting / flying / first person / dead / static / scripted / authored are GUESSED
  from H3's camera-mode order (type 4 is the one built from the player and its dead unit, halo3+0x131e34).
- `dir+0x10` (camera +8) i32 = camera target object: set by halo3+0x131b40 for types 0, 1, 3 from halo3+0x131ae4
  (= output user's unit, else player+0x2C); the cameras' updates compare it with get_unit_by_output_user (type 0
  halo3+0x213d54, type 3 halo3+0x17d7cd). CONFIRMED.
- observer globals `og = *(u64*)(tls + 0x578)` (0xF50 bytes, halo3+0x17e274): one observer per output user at
  `og + 8 + u*0x3D0`. halo3+0x17eb20 (observer_try_and_get_camera(user), GUESSED name) returns `og + 0x11C + u*0x3D0`
  when the user is active and the byte at +0xC of it != 0xFF. That is the final render camera; position at +0
  (3 floats) is GUESSED from H3's s_observer_result, not verified live.

### 6.5 Reading it from the loader (sketch)

```c
/* tls = the engine thread's halo3 TLS block (stats.c's g_st_tls); read through hb_read in real code.
   Returns the players-array index of the player output user 0 shows, or -1. The same in first/third person, in a
   vehicle (unit stays the seated biped; its object +0x10 is the vehicle) and while dead/respawning. */
static int local_player_index(const uint8_t *tls, uint32_t *unit_out) {
    const uint8_t *pm = *(const uint8_t **)(tls + 0x110);          /* player mapping globals */
    if (pm)
        for (int u = 0; u < 4; u++) {                                 /* first active output user (halo3+0xee5a8) */
            int32_t h = *(const int32_t *)(pm + 0xB8 + u * 4);     /* output user -> player handle */
            if (h == -1) continue;
            if (unit_out) *unit_out = *(const uint32_t *)(pm + 0xC8 + u * 4);   /* -1 while dead */
            return h & 0xFFFF;
        }
    /* fallback: first player of the local machine */
    const uint8_t *pg = *(const uint8_t **)(tls + 0x40), *pa = *(const uint8_t **)(tls + 0x10);
    if (!pg || !pa) return -1;
    int32_t local = *(const int32_t *)(pg + 0x7C), n = *(const int32_t *)(pa + 0x3C);
    const uint8_t *data = *(const uint8_t **)(pa + 0x48);
    for (int i = 0; local != -1 && i < n && i < 16; i++) {
        const uint8_t *p = data + i * 0x2F8;
        if (*(const uint16_t *)p && *(const int16_t *)(p + 0x1A) == local) {   /* salt != 0 = slot in use */
            if (unit_out) *unit_out = *(const uint32_t *)(p + 0x28);
            return i;
        }
    }
    return -1;
}
```
