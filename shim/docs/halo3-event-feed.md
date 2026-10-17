# Event feed: chat, HUD message lines, kill events (static RE)

Static RE only, 2026-10-17. Tools: capstone over every `.pdata` function of both binaries, `re-tools/refs.py`,
`re-tools/hdis.py`, `re-tools/hsfuncs.py`, `re-tools/h3map.py`. No process was run and no live memory was read.
Binaries:
- `$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`
  (base 0x180000000, addresses `halo3+RVA`)
- `client/project-reclaimer-client-v0.9.18.exe` (base 0x140000000, addresses `exe+RVA`)

**CONFIRMED** means it was read from the code, and the evidence is given. **GUESSED** means the name or meaning is
inferred. "PIC" means position-independent: the bytes can be copied into `entry_hook`'s trampoline (peload.c)
unchanged. TLS: `tls = gs:[0x58][*(u32*)(halo3+0xa39f9c)]`, players data array `*(tls+0x10)`, game_globals
`*(tls+0x48)`, game engine globals `*(tls+0x20)`, chud `*(tls+0xf0)`.

## Summary: three hooks catch everything the player sees

| What | Hook | Bytes to copy | Args |
|---|---|---|---|
| Chat, [SERVER] lines and chat notices (Reclaimer's own chat panel) | `exe+0xb89930` TextChat::push_history | 19, PIC | rcx chat, rdx `Line*` |
| Every line in halo3's HUD message feed: game-engine events and Reclaimer's join/leave lines | `halo3+0x2d3f2c` chud_messaging_post | 16, **not PIC**: needs a custom trampoline, see 2 | ecx user, rdx `wchar_t*`, r8d context |
| Structured game-engine events: kills with damage type and modifiers, joins, flag, infection... | `halo3+0x11b0ec` game_engine_display_event | 17, PIC | rcx `event*` (0x2c bytes) |

Do **not** hook `halo3+0x118a1c` (game_engine_player_killed). The client detours that function itself (see 3.4).

## 1. Chat (Reclaimer's own text chat, not halo3)

Chat is drawn by the client's own egui panel (`src/chat_ui.rs`, drawer `exe+0x2da7e0`), not by halo3's HUD. Every
line that panel shows (chat lines, `[SERVER]` lines and local notices) is appended to its history by **one function**:

**`exe+0xb89930` TextChat::push_history(chat, line, mine, t_secs, t_nanos)** (`src/text_chat.rs`; the name is GUESSED)

| Arg | Win64 | Meaning |
|---|---|---|
| `chat` | rcx | the client's TextChat state. It is the `Mutex` data inside the remote-host connection `Arc` (`Arc`+0x2c0, as read in `exe+0x692240`; `exe+0x68c790` returns the `Arc`). `*(u8*)(chat+0xf8) == 0` means the history is disabled and the line is dropped (checked at `b89953`) |
| `line` | rdx | `Line*`, 0x40 bytes, **moved** (the function takes ownership; copy what you need, do not free it) |
| `mine` | r8b | 1 = a chat line the local player sent (set by the caller when `line.sender == own id` and the variant is Chat) |
| `t_secs`, `t_nanos` | r9, `[rsp+0x28]` at entry | a timestamp (u64 and u32; nanos 1000000000 = none). Stored at entry +0x40 / +0x48. The panel fades lines with it. Meaning GUESSED |

The history is a `VecDeque` of 0x58-byte entries at `chat+0x38` (cap +0x38, buf +0x40, head +0x48, len +0x50; at
most 100 lines, the oldest is popped). An entry holds the `Line` (+0x00..0x40), the timestamp (+0x40/+0x48) and
`mine` (+0x50). CONFIRMED (`b8996b..b89baf`).

Prologue, 27 bytes, all PIC. Copy 19 bytes (the eight pushes and `sub rsp`) for `entry_hook`:
```
b89930  55                      push rbp
b89931  41 57                   push r15
b89933  41 56                   push r14
b89935  41 55                   push r13
b89937  41 54                   push r12
b89939  56                      push rsi
b8993a  57                      push rdi
b8993b  53                      push rbx
b8993c  48 81 ec a8 00 00 00    sub rsp, 0xa8          <- 19 bytes end here
b89943  48 8d ac 24 80 00 00 00 lea rbp, [rsp+0x80]
```
`entry_hook(exe_base+0xb89930, pro, 19, fn)` calls `fn(rcx, rdx, r8, r9)` with the arguments untouched. The hook
must only read `line`. The exe is not verified by the client the way it verifies halo3 code. GUESSED; nothing found
that checks it.

**`Line` (0x40 bytes, Rust enum, niche in the first qword).** CONFIRMED from the chat panel drawer `exe+0x2da7e0`
("[TEAM] " at `2db082`, "[NEARBY] " at `2dad72`, "[SERVER] " at `2dae9f`; name from +0x08/+0x10 at `2db08f`, text
from +0x20/+0x28 at `2db4fb`) and from `push_history`'s drop code. `q0 = *(i64*)line`:

| `q0` | Variant (serde names `Chat`, `Notice`, `Server`, VARIANTS table at `exe+0x205eaf8`) | Layout |
|---|---|---|
| >= 0 | **Chat** (5 fields, "struct variant Line::Chat with 5 elements") | +0x00 String **sender name** {cap, ptr +0x08, len +0x10}. +0x18 String **text** {cap, ptr +0x20, len +0x28}. +0x30 u64 **sender id** (the session peer id that the roster rows hold at +0x88/+0x90, the same id the voice mute list uses, see `reclaimer-voice-mute.md`). +0x38 u8 has_color, +0x39..0x3b name colour RGB. +0x3c u8 **channel: 0 all, 1 team ("[TEAM] "), 2 nearby ("[NEARBY] ")** |
| 0x8000000000000000 | **Notice** (local status line) | +0x08 u8 kind: 0 Throttled, 1 TeamUnavailable, 2 ProximityUnavailable, 3 Disabled, 4 Undelivered (name table at `exe+0x205eb60`; texts like "You are sending messages too quickly.") |
| 0x8000000000000001 | **Server** (shown as "[SERVER] " + text) | +0x08 String {cap, ptr +0x10, len +0x18} |

The strings are **UTF-8 Rust Strings, not wide chars and not NUL-terminated**: use (ptr, len).

**Callers.** All five are CONFIRMED by an `e8` call scan:

| Call site | Function | What it pushes |
|---|---|---|
| `exe+0x683951` | `exe+0x682f60`, the client's handler for messages from the host (`src/text_chat.rs` line 763; jump table on the decoded message variant at `6838c7`) | **every received chat or server line**. `mine` = (variant is Chat) && (`line+0x30` == own id) |
| `exe+0xb89731` | `exe+0xb896c0` (host side: deliver a line to one peer; the peer table is 0xa0-byte rows with the peer id at +0x48) | lines the **host** delivers to itself, because a listen host has no network round trip |
| `exe+0x69175a` | `exe+0x6914c0` | Notice kinds 1 and 3 (send failures) |
| `exe+0x68a144` | `exe+0x6895e0` | Notice kind 4, Undelivered (no ack within about 5 s) |
| `exe+0x691db3` | `exe+0x691c80` | Server lines produced locally (`exe+0xb89c10`; GUESSED: the replies to chat commands such as /help) |

On a dedicated server the lines come from the server, through `exe+0x682f60`. The text chat runs in the process
that loads halo3.dll. That process owns the remote-host connection `exe+0x68c790`, the same one the teleport code
uses (`teleport.md`). GUESSED, so check it at runtime.

**Join/leave lines are different:** they go into halo3's HUD feed, see 2. The client's chat panel never sees them, and
`push_history` does not catch them.

## 2. halo3's HUD message feed (lower left)

### 2.1 chud_messaging_post: `halo3+0x2d3f2c (int user_index, const wchar_t* text, int context)`

CONFIRMED (`2d3f2c..2d3f6f`, the whole function):
```
if (user_index < 4)
    halo3+0x2d4c34(*(tls+0xf0) + 0x1f8c + user_index*0x28c8, text, context);
```
- `ecx` = **output user index** (0..3, the local split-screen user; not a player). The event path passes the users
  that view the event's player (iterator `halo3+0xee660`). Reclaimer always passes 0.
- `rdx` = NUL-terminated **UTF-16** text. It is copied into one of 5 message slots with at most 0x60 characters
  (`2d4cec`, `halo3+0xb60fc`). It is already formatted: player names are substituted.
- `r8d` = context: the event definition's s16 at def+0xC. In the map data: 0 for "you killed / you died", 1 for
  "your team killed", 2 for "your teammate died", 3 for everything general (round over, joins). Reclaimer passes 3.
  GUESSED: it selects the line's colour.

`halo3+0x2d4c34` (state, text, context) picks a free slot, or the oldest one. Slots are at state+0x90 + i*0xe8
(i < 5): +0x00 `wchar_t text[0x60]`, +0xc0 u16 0x100 (the byte at +0xc1 = in use), +0xc4 game time posted
(`*(tls+0xc8)+0xc`), +0xcc u64 0, +0xe4 context. CONFIRMED (`2d4c69..2d4d0a`).

**Callers.** CONFIRMED, all of them:

| Call site | What |
|---|---|
| `halo3+0x11b85b` | game-engine event display (3.3): kill lines, joins, flag, infection... |
| `halo3+0x2d3f16` in `halo3+0x2d3ee4 (user, string_id, context)` | post a localized string by string id (`2d3f07` resolves it). Called from `2d4169`, `2d41d5`, `2d4241`, `2d42c0` |
| `halo3+0x2ddd13` in `halo3+0x2ddc60` | a one-time hint whose text comes from the MCC host interface (`*(halo3+0xa3cab8)` vtable +0x1d0), context 3 |
| **`exe+0x77b393`** (Reclaimer, in `exe+0x76cfa0`) | **Reclaimer's own lines**: `halo3+0x2d3f2c(0, utf16, 3)`. The strings come from `exe+0x75e720` (`src/engine_teams.rs`), which diffs the roster and formats "<name> joined the server" / "<name> left the server". Before posting, the text goes through `exe+0x1911640`, a filter switched by `exe+0x27d9ba0`; then `exe+0x8fc00` converts it to UTF-16. Afterwards the client re-reads the 5 chud slots with a bounded read (`exe+0x75dd20`, `exe+0x75e410`, loop `77b3df..77b4e3`). If none of them holds the line, it logs "the native HUD did not keep a roster notice" (`77b4f7`). This is the only `0x2d3f2c` reference in the exe (imm32 scan: 1 hit) |

So a hook on `0x2d3f2c` sees every line the HUD feed shows. Text only: no weapon, no player handles.

**Hooking it.** The prologue is **not PIC**:
```
2d3f2c  48 83 ec 28              sub rsp, 0x28
2d3f30  83 f9 04                 cmp ecx, 4
2d3f33  7d 36                    jge 0x2d3f6b          ; rel8 -> "add rsp,0x28; ret"
2d3f35  44 8b 0d 60 60 76 00     mov r9d, [rip+..]     ; = *(u32*)(halo3+0xa39f9c), rip-relative
2d3f3c  65 48 8b 04 25 58 00 00 00  mov rax, gs:[0x58] ; PIC
2d3f45  41 bb f0 00 00 00        mov r11d, 0xf0        ; PIC
```
The first 14 bytes end inside the `mov r9d` (2d3f35..2d3f3b), so patch **16 bytes** (2d3f2c..2d3f3b). The
trampoline cannot copy them unchanged. After `fn(rcx, rdx, r8, r9)` it should run this instead:
```
48 83 EC 28                      sub rsp, 0x28
83 F9 04                         cmp ecx, 4
7D <d8>                          jge Lret              ; short jump inside the trampoline
49 B9 <imm64 halo3+0xa39f9c>     mov r9, &tls_index
45 8B 09                         mov r9d, [r9]
49 BB <imm64 halo3+0x2d3f3c>     mov r11, resume
41 FF E3                         jmp r11
Lret: 48 83 C4 28 C3             add rsp, 0x28 ; ret
```
r9 and r11 are overwritten by the original code anyway. A mid-function hook at `2d3f3c` (15 PIC bytes) would also
work, but there `rsp` is 16-byte aligned, so `entry_hook`'s frame would call `fn` misaligned. That needs an extra
8-byte adjust. The client does not check this prologue: no copy of its bytes is in the exe.

## 3. Game-engine events (kills etc.)

### 3.1 The release build's event_test scripts are stubs

`game_engine_event_test_slayer/_general/_flavor/_infection/_ctf/...` all share evaluate `halo3+0x1065f4` (`mov
ecx,edx; xor edx,edx; jmp 0x105858` = return void) and parse `0x1e1bac`. They do nothing in this build, so the path
was found from the event-definition lookup instead.

### 3.2 Event struct `s_game_engine_event_data`, 0x2c bytes. CONFIRMED

Sources: the builder `halo3+0x118a1c` (`119c20..119cef`), the default constants `halo3+0x848fa0` / `0x817a50` /
`0x849010`, the network encoder `halo3+0x93d9c`, the definition lookup `halo3+0x11b480`, the display code
`halo3+0x11b6e8`, the formatter `halo3+0x14a21c`.

| Off | Type | Field | Evidence / notes |
|---|---|---|---|
| +0x00 | s32 | **event type**: 0 general, 1 flavor (medals), 2 slayer, 3 ctf, 4 oddball, 6 king, 7 vip, 8 juggernaut, 9 territories, 10 assault, 11 infection, 13 gun game (5 and 12 have no definition block) | `11b4e3..11b581` picks mulg runtime block 0x88..0x10c by type. Type names from the blocks' contents (3.6). 4 bits on the wire |
| +0x04 | s32 | **event index** within the type (general: 3.6) | compared with def+0x4 (`11b5c9`). 8 bits on the wire |
| +0x08 | s32 | sequence id 0..63. The host sets it from a counter at `*(tls+0x20)+0x14cb4` (`14aa44`) and uses it as the queue slot | 6 bits on the wire |
| +0x0c | s32 | **audience player** (datum handle), -1 = every player | `11b0f9`: -1 means loop over all players |
| +0x10 | s32 | **cause player** (datum handle, -1 none): the killer, the flag grabber... | formatter `#cause_player` (`14a26d`) |
| +0x14 | s32 | **cause team** (team index, -1 none) | `#cause_team` (`14a382`); set from player+0xf0 |
| +0x18 | s32 | **effect player** (handle, -1 none): the victim | `#effect_player` (`14a2ed`) |
| +0x1c | s32 | **effect team** | `#effect_team` (`14a41c`) |
| +0x20 | s32 | quantity (0 for kills). Name GUESSED | encoded specially for 0 and 1, otherwise 32 bits (`940c1..9413e`) |
| +0x24 | u8 | **damage byte** = damage_reporting_type (low 6 bits) + special (high 2 bits). Only valid when +0x25 bit 0 is set | builder stores its 5th arg here (`119c9e`); display masks `& 0x3f` before the MCC report (`d3e27`); sent only when +0x25 bit 0 is set (`941e5`) |
| +0x25 | u8 | bit 0 = **has kill info**; bits 1..7 = modifier bits 0..6 | `= 1 | (m & 0x7f) << 1` (`119ca5..119cef`); other events clear bit 0 (`118727`) |
| +0x26 | u8 | bit 0 = modifier bit 7 | `119c91..119ca7` |
| +0x27 | u8 | pad | |
| +0x28 | s16 | -1 in kills and vehicle-boarding events. Meaning unknown | `119c3a`, `118753`, encoded at `94177` |

**Player fields are datum handles** (salt << 16 | index) in the event the hook sees. CONFIRMED in three places.
The display loop passes the iterator's datum index (`11b12c`); `11b3b4` compares the player handle with
`event+0x10` as a full 32-bit value and indexes the players array with its low 16 bits (`11b3e2`); the formatter
fetches names through `datum_try_and_get` `halo3+0x130854`, which checks the salt (`13087a`). **On the wire they are
absolute indices** (4 bits each). The host converts them in `halo3+0x1ee1c` (`1ee96..1eeb3`, `movzx` of the low
word); the receiver converts them back with the players array's salt in `halo3+0x9460c` (`94698..9472a`). So:
players table entry = `*(tls+0x10)` data array, index = `handle & 0xffff`, check `*(u16*)entry == handle >> 16`.
-1 = none.

**Names.** The engine's formatter (`#cause_player` / `#effect_player`, `halo3+0x14a954`) copies the UTF-16 name from
player datum **+0xd0**. `halo3-objects.md` and stats.c use +0x58 (GUESSED: the client-requested name in the player
configuration; +0xd0 = the host-assigned one; normally they are the same string). Team = player datum **+0xf0**
(`11b423`, `119c61`).

### 3.3 Event flow. CONFIRMED

```
host:   builder (e.g. 0x118a1c player_killed) -> 0x14aa10 send_networked:
          sets +0x08 = sequence id;
          0x1ee1c: if host (gg+0x11 == 5): handles -> absolute indices,
                   0x11b2d8 = players that have a definition for this event,
                   0x53718 = send only to their machines;
          0x11b184(ev, queue=1)
client: simulation event apply 0x9460c (vtable at 0x768978) -> absolute -> handles -> 0x11b184(ev, 1)
0x11b184 send_event(ev, queue): requires 0x101064(); queue=0 -> 0x11b0ec now;
          queue=1 -> slot *(tls+0x20)+0x14cb8 + id*0x34 = {u32 time, u8 flags (1 pending, 2 shown, 4 slow), event}
          (flag 4 for general 0xc joined, 0x19 rejoined, 0x3a pre game over)
0xfd418 (game engine update, from 0xef26e) collects pending slots older than 0.15 s (flag 4: 2 s), sorts them
          (comparator 0x11b268), calls 0x11b0ec for each and marks it shown
0x11b0ec display_event(ev): audience -1 ? every player : the audience player;
          per player: def = 0x11b3b4(player, ev) (tries audience cause player 0, effect player 2,
          cause team 1, effect team 3, all 4), then 0x11b6e8(player, ev, def)
0x11b6e8: formats def+0x10 (display string) with the "#token" formatter 0x14a090 and posts it with
          0x2d3f2c(user, text, def+0xC); def+0x20/+0x24 (primary string and duration) go to 0x2d4df8 (centre
          text); sound from def+0x34 + language*16; MCC reporting; for kill-type indices it calls the MCC kill
          report 0xd3d44(cause, effect, damage byte, modifier flags, kind)
```

**`halo3+0x11b0ec` is the single point every displayed event passes through** on the host and on clients: local
events, received events, and the direct queue=0 path (`ec8f4`). It is called once per event (the queue marks it
shown). It is also called on a machine with no local players; the per-player work then finds nothing. GUESSED that
this holds on the dedicated server.

**Hook:** `halo3+0x11b0ec`, rcx = `event*`. Copy 0x2c bytes.
```
11b0ec  40 53                    push rbx
11b0ee  48 83 ec 40              sub rsp, 0x40
11b0f2  41 83 c8 ff              or r8d, -1
11b0f6  48 8b d9                 mov rbx, rcx
11b0f9  44 39 41 0c              cmp [rcx+0xc], r8d     <- 17 bytes, all PIC
11b0fd  75 61                    jne 0x11b160           (rel8: do not copy)
```
`entry_hook(base+0x11b0ec, {40 53 48 83 ec 40 41 83 c8 ff 48 8b d9 44 39 41 0c}, 17, fn)`. The trampoline's final
`mov r11,imm64; jmp r11` leaves the flags of the copied `cmp` intact for the `jne` at 11b0fd. The client has no
reference to this function and no copy of its bytes. It runs on the engine main thread inside the tick, so the hook
should only copy the event into a ring. Names and teams can be resolved later from the players table, unless the
victim has left the game by then.

**What a client does not see:** the host sends an event only to machines whose players have a definition for it
(`1eec5`/`1eeec`). General kill definitions exist only for the cause/effect player and cause/effect team
(3.6), so a client receives kills that involve itself or its team. In free-for-all that means only its own kills
and deaths; a team game covers nearly all. The host, and the dedicated server, see every event.

Alternative: `halo3+0x11b184` send_event (rcx event, dl queue), 16 PIC bytes
`48 89 5c 24 08 57 48 83 ec 20 40 8a fa 48 8b d9` (the next instruction is a `call rel32`). It fires when an
event is created or received, without the 0.15 s delay, but also for duplicates that are then dropped.

### 3.4 Kills: game_engine_player_killed `halo3+0x118a1c`. CONFIRMED, host only

Args: `ecx` killer player handle (-1 none), `edx` unknown (passed on to the mode's vtable +0x188), `r8d` victim
player handle, `r9b` betrayal flag, `[rsp+0x28]` (5th, u8) damage byte. On a distributed client (gg+0x11 == 4) it
builds no event and only calls the MCC report (`119c0e`); the client learns about kills from the host's event.

**The Reclaimer client detours this function itself.** Its custom-mode runtime checks the 15-byte prologue
(signature at `exe+0x1fec270`) and installs `exe+0x601a90` with its hook installer `exe+0x6ddb00` (`exe+0x5aa05b`,
`5aa2e9`; "Lost .. native death events", "Custom mode native contract mismatch"). Do not hook it: `entry_hook`'s
prologue check would fail, or the client would refuse to start.

General event index chosen (`118c6f..118f7b`; `T` = damage byte & 0x3f, `S` = damage byte >> 6, `C` = category
table `halo3+0x791fa0[T]`):

| Condition | Index | Definition text id |
|---|---|---|
| victim player has flag +4 bit 1 | 0x0b | gen_player_quit_all |
| no killer, T == 1 | 0x23 | gen_killed_by_falling_effect_player |
| no killer, T == 33 (bomb explosion) | none (no event) | |
| no killer, otherwise | 0x0d | gen_killed_by_unknown_effect_player |
| killer == victim | 0x23 if T == 1, else 0x01 | suicide |
| betrayal flag | 0x02 | gen_kill_teammate_* |
| C == 3 (melee) and S == 2 | 0x2c | gen_kill_sneak_* (assassination); m = 3 |
| C == 3 (melee) | 0x25 | gen_kill_melee_*; m = 1 |
| T == 2 or S == 3 | 0x24 | gen_kill_col_* (splatter); m = 4 |
| C == 4 (grenade) and S == 2 | 0x2a | gen_kill_sticky_*; m = 8 |
| S == 1 and T in {14, 15} | 0x2b | gen_kill_sniper_* (sniper headshot) |
| T in {23, 30} | 0x37 | gen_kill_flame_* |
| T == 21 | 0x36 | gen_kill_spartanlaser_* |
| otherwise | 0x00 | gen_kill_* |
| later in the function (`1195b5`, `1196bf`) | 0x32 / 0x33 | bulltrue / kill from the grave |

T == 56 (teleporter) builds no kill event (`118c75`; telefrag is general event 0x2f).

The game mode's vtable +0x198(killer, victim, betrayal) can replace the index (`118dfe`): for example ctf 0x28 /
assault 0x29 "killed the carrier". The event type stays 0, and +0x25 bit 0 stays set.

Special `S`: 0 normal, 1 headshot, 2 assassination (melee) or stick (grenade), 3 collision. The meanings follow the
event each value selects.

Modifier bits `m` (event +0x25 bits 1..7 and +0x26 bit 0):

| Bit | Meaning | Status |
|---|---|---|
| 0x01 | melee | CONFIRMED |
| 0x02 | assassination; always set together with 0x01 | CONFIRMED |
| 0x04 | splatter | CONFIRMED |
| 0x08 | sticky | CONFIRMED |
| 0x10 | headshot (S == 1) | CONFIRMED |
| 0x20 | set when `0xfbd24()` is false and `0xfbc94(victim)` == 0 | meaning unknown |
| 0x40 | team game and `0xd3128(victim team)` == 1 | GUESSED: the victim's team is down to one player |
| 0x80 | non-team game and the iterator `0xe7300` counts 2 | GUESSED: the last two players |

**Identify a kill in a hooked event:** `type == 0 && (+0x25 & 1)`. Killer = +0x10, victim = +0x18, weapon =
`+0x24 & 0x3f`, S = `+0x24 >> 6`, modifiers = `((+0x25 >> 1) & 0x7f) | ((+0x26 & 1) << 7)`. Index 0x01 is a
suicide and index 0x02 a betrayal.

### 3.5 damage_reporting_type (MCC halo3.dll, values 0..62)

There are no enum strings in halo3.dll. The names come from three sources:
- **Projectile tags.** The map files' `proj` tags have the reporting type at byte **+0x1a8**. It matched all 33
  weapon projectiles in guardian.map, then every .map in `maps/` was scanned. CONFIRMED values in the proj column.
- **The client's own table**, `exe+0x22f6860`: 63 entries of 0x28 bytes {key &str, display &str, u64 group},
  indexed by the raw value (`exe+0x1a9d620`, `exe+0xcc1f07`).
- **halo3's category table** `halo3+0x791fa0` (u32[63]): 0 other, 2 weapon, 3 melee, 4 grenade, 5 vehicle.

From value 44 on, **the client's names are two places off.** Its list has spectre and spectre_gunner (44 and 45),
which this build does not have. Four things show it:
- The warthog chaingun projectile is 45 and the gauss is 46.
- halo3's category table makes 39..53 vehicles and 56/57 "other", which fits only the shifted list.
- The kill builder skips T == 56; that is the teleporter in the shifted list.
- halo3's MCC damage mapper `halo3+0xd6908` groups 44, 45, 46 together (47, 48, 49) and 47, 48 together (37, 38).

Any client stats that index this table by the raw value would mislabel vehicle kills (GUESSED).

| Value | Name (best) | Category | proj tag evidence | Client table name |
|---|---|---|---|---|
| 0 | Guardians | other | | guardians |
| 1 | Falling | other | | falling |
| 2 | Collision | other | | collision |
| 3 | Melee | melee | | melee |
| 4 | Explosion | other | | explosion |
| 5 | Magnum | weapon | magnum_bullet | magnum |
| 6 | Plasma Pistol | weapon | plasma_pistol_bolt | plasma_pistol |
| 7 | Needler | weapon | needler_shard | needler |
| 8 | Mauler | weapon | excavator_shard | mauler |
| 9 | SMG | weapon | smg_bullet | smg |
| 10 | Plasma Rifle | weapon | plasma_rifle_bolt | plasma_rifle |
| 11 | Battle Rifle | weapon | battle_rifle_bullet | battle_rifle |
| 12 | Carbine | weapon | carbine_slug | carbine |
| 13 | Shotgun | weapon | shotgun_bullet | shotgun |
| 14 | Sniper Rifle | weapon | sniper_bullet | sniper_rifle |
| 15 | Beam Rifle | weapon | beam_rifle_beam | beam_rifle |
| 16 | Assault Rifle | weapon | assault_rifle_bullet | assault_rifle |
| 17 | Spiker | weapon | spike_shard | spiker |
| 18 | Fuel Rod Gun | weapon | flak_bolt | fuel_rod_gun |
| 19 | Missile Pod | weapon | missile_pod_missile | missile_pod |
| 20 | Rocket Launcher | weapon | rocket | rocket_launcher |
| 21 | Spartan Laser | weapon | spartan_laser_beam | spartan_laser |
| 22 | Brute Shot | weapon | brute shot grenade | brute_shot |
| 23 | Flamethrower | weapon | flamethrower_stream | flamethrower |
| 24 | Sentinel Beam | weapon | sentinel_gun_beam | sentinel_beam |
| 25 | Energy Sword | melee | | energy_sword |
| 26 | Gravity Hammer | melee | | gravity_hammer |
| 27 | Frag Grenade | grenade | frag_grenade | frag_grenade |
| 28 | Plasma Grenade | grenade | plasma_grenade | plasma_grenade |
| 29 | Spike Grenade | grenade | claymore_grenade | spike_grenade |
| 30 | Firebomb | grenade | firebomb_grenade | firebomb |
| 31 | Flag | melee | | flag |
| 32 | Bomb | melee | | bomb |
| 33 | Bomb Explosion | other | | bomb_explosion |
| 34 | Oddball | melee | | ball |
| 35 | Machine Gun Turret | weapon | machinegun_turret_bullet | machinegun_turret |
| 36 | Plasma Cannon | weapon | plasma_cannon_bolt | plasma_cannon |
| 37 | Plasma Mortar | weapon | | plasma_mortar |
| 38 | Plasma Turret | weapon | | plasma_turret |
| 39 | Banshee | vehicle | banshee_bolt, banshee_bomb | banshee |
| 40 | Ghost | vehicle | ghost_bolt | ghost |
| 41 | Mongoose | vehicle | | mongoose |
| 42 | Scorpion | vehicle | | scorpion |
| 43 | Scorpion Gunner | vehicle | scorpion anti_infantry bullet | scorpion_gunner |
| 44 | Warthog (driver) | vehicle | | spectre |
| 45 | Warthog Gunner | vehicle | warthog chaingun bullet | spectre_gunner |
| 46 | Gauss Warthog | vehicle | warthog gauss_bullet | warthog |
| 47 | Wraith | vehicle | wraith mortar_turret_charged | warthog_gunner |
| 48 | Wraith Gunner | vehicle | wraith anti_infantry wraith_bolt | warthog_gauss |
| 49 | Tank (Scorpion cannon) | vehicle | scorpion tank_shell | wraith |
| 50 | Chopper | vehicle | chopper_gun_bullet | wraith_gunner |
| 51 | Hornet | vehicle | hornet_bullet, hornet_missile | tank |
| 52 | Mantis (unused) | vehicle | | chopper |
| 53 | Prowler | vehicle | mauler_bolt (prowler gun) | hornet |
| 54 | Sentinel Beam Turret | weapon | | mantis |
| 55 | Sentinel RPG | weapon | | prowler |
| 56 | Teleporter | other | | sentinel_beam_turret |
| 57 | Trip Mine | other | | sentinel_rpg |
| 58 | Elephant Turret | weapon | shrine behemoth_chaingun_bullet | teleporter |
| 59 | Shade Turret | weapon | shade_turret | tripmine |
| 60 | Silenced SMG | weapon | smg_silenced_bullet | elephant_turret |
| 61 | Automag | weapon | automag_bullet | damage_61 |
| 62 | Brute Plasma Rifle | weapon | plasma_rifle_red | damage_62 |

Names 0..43 come from the client and agree with every projectile. From 44 on, the projectile evidence and the
shifted client names agree. Names without a projectile row (44, 52, 54..57) are GUESSED from the shifted client
list.

### 3.6 Event definitions (multiplayer_globals) and general event indices

The `mulg` tag (guardian.map tag 1136) has a runtime block, element 0. At +0x60 is a tagref to the in-game strings
`multiplayer\in_game_multiplayer_messages` (unic). Event blocks start at +0x88, 12 bytes apart, one per type:
- +0x88 general (94 entries)
- +0x94 flavor (112)
- +0xa0 slayer (3)
- +0xac ctf (19)
- +0xb8 oddball (12)
- +0xc4 king (10)
- +0xd0 vip (13)
- +0xdc juggernaut (6)
- +0xe8 territories (7)
- +0xf4 assault (19)
- +0x100 infection (10)
- +0x10c gun game (10)

Each element is **0x104 bytes**. Fields used by the code:

| Offset | Field |
|---|---|
| +0x0 | u16 flags (bit 0 = no HUD line) |
| +0x2 | u16 type |
| +0x4 | u16 event index |
| +0x6 | u16 audience: 0 cause player, 1 cause team, 2 effect player, 3 effect team, 4 all |
| +0xC | s16 context |
| +0x10 | string_id display string |
| +0x18 | s16 required field (1..4 = +0x10/+0x14/+0x18/+0x1c must be set) |
| +0x1a | s16 excluded audience |
| +0x20 | string_id primary string |
| +0x24 | duration |
| +0x2c | float sound delay |
| +0x30 | sound flags |
| +0x34 | snd! tagref per language, 12 x 16 bytes |
| +0xf8 | random sounds block |

`halo3+0x11b480` reads them: `11b4a5..11b6b0` and `11b6e8..11bb0b`. If no general definition matches and the index
is not 0x39, it returns the default `halo3+0x8f3ef0`.

General indices with a definition in guardian.map: 0 kill, 1 suicide, 2 kill teammate, 3 victory, 4 team victory,
0xb player quit, 0xc player joined, 0xd killed by unknown, 0xe/0xf/0x10/0x11 30/15/5/1 minutes left, 0x13 game
over, 0x17 teleporter blocked, 0x18 player changed team, 0x19 player rejoined, 0x1a-0x1f lead gained, lost or
tied, 0x20 round over, 0x21/0x22 30/10 seconds left, 0x23 killed by falling, 0x24 splatter, 0x25 melee, 0x26
sudden death, 0x27 player booted player, 0x28 ctf flag-carrier kill, 0x29 assault bomb-carrier kill, 0x2a sticky,
0x2b sniper headshot, 0x2c assassination, 0x2d boarded vehicle, 0x2e team notice, 0x2f telefrag, 0x32 bulltrue,
0x33 kill from the grave, 0x34 hijack, 0x35 skyjack, 0x36 spartan laser, 0x37 flame, 0x38 driver assist, 0x3a pre
game over.

Other types (index: definition id). Flavor (type 1) holds the medals: 0 extermination, 1 perfection, 2..10
multikills, 11..16 sprees, and so on.
- ctf (3): 0 start, 1/2 flag grabbed, 3 dropped, 4 recovered, 5 reset, 6 captured, 7 new defensive team, 0xe failed capture
- oddball (4): 1 spawned, 2 picked up, 3 dropped, 4 reset
- king (6): 1 hill controlled, 2 contested, 5/6 team controlled/contested
- vip (7): 1 new vip, 2 vip killed, 3 vip death, 5 destination arrived
- juggernaut (8): 1 new juggernaut, 3 juggernaut kill
- territories (9): 1 captured
- assault (10): 1 bomb pickup, 2 dropped, 3 returned, 4 reset, 5 detonated, 0xc disarmed, 0xd armed
- infection (11): 1 **new infection** ("X infected Y"), 2 **new zombie** ("X has become a zombie"), 3 new alpha, 4 last man
- gun game (13): 1 tier up, 2 tier down, 3 final tier

`re-tools/h3map.py` cannot resolve the display string ids; `h3bsp.sid_name` can. The display texts themselves
(with `#cause_player`-style tokens) are in the unic tag's locale strings, which were not decoded here. Formatter
tokens (UTF-16 literals at `halo3+0x794928..0x794b48`): `#cause_player #effect_player #cause_team #effect_team
#winning_team #winning_player #cause_team_score #cause_team_opponent_score #current_round #total_rounds
#score_to_win #local_spawn_time #local_player_score #local_team_score #local_lives_remaining #switch_side_time`.

## 4. Other client touches near these paths (for compatibility)

- `exe+0x5a0b20` ("Standalone player preferences") checks, and presumably patches, conditional jumps that guard
  the MCC host-interface calls: `halo3+0x11bb21` (`0f 84` in display_event_for_player, the MCC medal/stat block),
  `halo3+0xd3d89` (in the MCC kill report), `0xfe7c6`, `0xfe67a`, `0xfe420`. None of them is a prologue we use.
- `exe+0x93aa15` checks bytes at `halo3+0x2d4c99`, inside chud_messaging_state_post's slot-selection loop.
  `halo3+0x2d4c34`'s prologue is left alone.
- `exe+0x9d7c27` calls `halo3+0x14aa10` send_networked, so custom modes send their own events. The `0x11b0ec` hook
  sees those too.

## 5. Evidence index (all static)

| Address | What |
|---|---|
| `halo3+0x11b480` | event definition lookup (player, event, audience); block switch by type, 0x104-byte stride (`11b699`) |
| `halo3+0x11b3b4` | definition for a player (tries audiences 0, 2, 1, 3, 4) |
| `halo3+0x11b0ec` | display event (all players or the audience player) |
| `halo3+0x11b6e8` | display for one player: string, formatter, chud post at `11b85b`, MCC report |
| `halo3+0x11b184` / `0x14aa10` / `0x1ee1c` | send (queue or now) / send networked / host-to-client encode-and-send |
| `halo3+0x9460c`, `0x93d9c` | client apply (absolute index -> handle) / bit encoder |
| `halo3+0xfd418` | queued event display after 0.15 s (`fd8c9..fd9c2`) |
| `halo3+0x118a1c` | player_killed: index, damage byte, modifiers, event build (`119c20..119cf6`) |
| `halo3+0x14a090`, `0x14a21c`, `0x14a954` | "#token" formatter, token switch, player name (datum +0xd0) |
| `halo3+0x2d3f2c`, `0x2d4c34`, `0x2d3ee4` | chud post, slot writer, post by string id |
| `exe+0xb89930` | client chat history push (all chat, server and notice lines) |
| `exe+0x682f60` | client: messages received from the host -> push_history |
| `exe+0x76cfa0` @ `77b247..77b393` | client: roster lines -> `halo3+0x2d3f2c(0, text, 3)` |
| `exe+0x22f6860` | client damage-type name table (off by two from 44 on) |
