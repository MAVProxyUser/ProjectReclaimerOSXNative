# Who owns a player's position on the network (halo3.dll, distributed MP)

Static RE only, 2026-10-17. Tools: capstone, `re-tools/hdis.py`, `re-tools/refs.py`, plus a scratch call/xref index.
Nothing was read from a live process.
Binary: `$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`
(base 0x180000000; all addresses below are RVAs).
**CONFIRMED** means read from code, with evidence given. **GUESSED** means a name or meaning was inferred.
TLS is as in `teleport.md`: objects `tls+0x38`, players `tls+0x10` (0x2f8-byte records), `gg = *(tls+0x48)`, and
`gg+0x11` (4 = client, 5 = host).

## TL;DR

1. **A client sends more than input. It also sends its own predicted body state.** Every control packet carries two
   per-player records. **A** is the controls: yaw, pitch, throttle, buttons and weapon/grenade/zoom indices.
   **B** is a *prediction*: object index, a 4-bit epoch, **origin, forward, up, linear velocity and angular velocity**
   of the client's unit, or of the vehicle it is **driving**. CONFIRMED.
2. **The host accepts the client's position, but only within a tolerance.** Let `d` be the distance from the
   client-predicted origin to the host's current origin. For a biped:
   - `d <= 0.2`: the host keeps its own position and only nudges the velocity toward the client's.
   - `0.2 < d <= 2.5`: the host **warps its object to the client's position, orientation and velocities**. The
     exception is when a capsule test finds the biped would overlap something at that position.
   - `d > 2.5`: the prediction is **ignored**.

   For a vehicle the limits are 0.4 and 7.5. CONFIRMED.
3. **The client ignores the host's position for its own unit while they are within 3.0 units** (9.0 in a
   vehicle). Beyond that it **hard-snaps** to the host's position and velocity and keeps its own facing. There is no
   blend for your own body: it either ignores the correction or snaps. CONFIRMED.
4. **Why a client-side `object_teleport` never sticks.** The jump puts `d` above 2.5, so every prediction the client
   sends afterwards is dropped (mode 0). The host body stays where it was and keeps being driven by the client's
   controls. The client keeps simulating locally, and the body falls, until a host update arrives with a position
   more than 3.0 away. That update snaps the client back. CONFIRMED.

   The ~1-2 s delay is the time until the host next sends a *position-bearing* update for that unit. The host sends
   position only when its own copy of the object moves (dirty flag 2), and the replication scheduler then rate-limits
   the update. GUESSED for the exact timing.
5. **Trust issue to report.** The host adopts client-supplied positions up to 2.5 units per accepted update (7.5 in a
   vehicle, ×2.5 for one second after the event stamped at unit `+0x408`). The checks are: the sender owns the unit
   (or drives the vehicle), the 4-bit epoch matches, the values are finite and the axes are orthonormal, and for
   bipeds the destination capsule is clear. **No check against the unit's possible speed was found.** A modified
   client could therefore move in steps of just under 2.5 units per update: a speed hack, or a "teleport" made of many
   steps. This is stock Halo 3 design (client-predicted movement), not a shim bug. CONFIRMED for the code path.
   GUESSED that nothing else limits it, because no further rate limit was searched for exhaustively.

## 1. Client to host: the per-player control channel

The distributed simulation view is a 0xc810-byte object. Its pool is at `halo3+0xa49050` and it is constructed at
`0x36a04`. It holds three replication sub-objects. CONFIRMED:

| Offset | vtable | What (name GUESSED) |
|---|---|---|
| `+0x48` | `0x764458` | entity replication (0x400 entity slots at `+0x84`) |
| `+0x4120` | `0x7644a0` | event replication |
| **`+0x4160`** | **`0x764410`** | **per-player control**: 32 slots. A at `+0x28 + i*0x48`, B at `+0x9b0 + i*0x48`. Outgoing masks are `+0x20` (A) and `+0x9a8` (B). Received masks are `+0x24` (A) and `+0x9ac` (B) |

Control vtable `0x764410`:

- `[0] 0x675fc`: has data (`+0x20 | +0x9a8`).
- `[1] 0x67610`: priority.
- `[3] 0x677b4`: write player `i`. It writes a 5-bit index, then bit + A (`0x7d854`), then bit + B (`0x7e2f4`).
- `[5] 0x67a54`: read. It decodes A with `0x7dc70` and B with `0x7e470`.
- `[6] 0x67cb0`: store the received data and set `+0x24` / `+0x9ac`.

All CONFIRMED.

**Producer `0x50c04(sim_player, player_action)`** (called from `0xf7494`/`0xf7354`; GUESSED: once per tick per local
player):
```
50c6a call 0x50fb8            ; A <- player_action  (returns ok)
50c7a call 0x53f9c            ; B <- prediction of this player's body
50ca5 movups [rbx+rcx*8+0x28],xmm0 ... 50ccf or [rbx+0x20],edx      ; queue A
50cd2 cmp dword [rdi+8],1 ; jne   ; B only for sim-player type 1 (local client player; name GUESSED)
50ce9 movups [rbx+rax*8+0x9b0],xmm0 ... 50d1f or [rbx+0x9a8],edx    ; queue B
```

**A, the controls (0x48 bytes).** Encoder `0x7d854`, builder `0x50fb8`. CONFIRMED fields; meanings GUESSED:

| Offset | Content | Encoding |
|---|---|---|
| +0x00 | flag, then a 4-bit value | 1 + 4 bits |
| +0x04 | yaw | quantised [0, 2π], 13 bits (const `0x847268` = 6.2832) |
| +0x08 | pitch | [-π, π], 12 bits |
| +0x0c / +0x10 | throttle x / y | [-1, 1], 5 bits each |
| +0x14 | button bits | from `action+8` bits 0, 1, 2, 11, 14 and `action+0x2c` (7 bits) |
| +0x16..+0x19 | trigger / weapon-state bytes (`0x5452c`) | 1 bit each |
| +0x1a | weapon index (from unit `+0x260`) | 5 bits |
| +0x1c / +0x1d | 3-bit indices | 3 bits each |
| +0x1e | grenade index (unit `+0x298`) | 2 bits |
| +0x20 / +0x24 | aim-assist block (`0x7dfb4`: 2 quantised reals plus a target object) | |
| +0x44 | `0x32dfac(unit)` | 1 bit |

**A contains no position or velocity.**

**B, the prediction (0x48 bytes).** Builder `0x53f9c(player, out)`, encoder `0x7e2f4`. CONFIRMED:
```
53fef mov r8d,[rdi+0x28]              ; player->unit
54017 mov ebx,[rax+0x10]              ; unit->parent
5404a cmp r8d,[rdx+0x2a0] ; jne skip  ; in a vehicle: only if we are its driver (parent+0x2a0 == unit) -> predict the vehicle
5401a mov r15b,[rax+0x20c]            ; epoch byte
5409b call 0x54a14                    ; object -> network index      -> B+0x00
540c3 mov [r14+8],r15b                ; epoch                        -> B+0x08
540c7 call 0x3451b4                   ; object_get_origin            -> B+0x0c
540d6 call 0x345318                   ; forward / up                 -> B+0x18 / B+0x24
540e5 call 0x345480                   ; linear / angular velocity    -> B+0x30 / B+0x3c
```

On the wire:

- The index is 10 + 4 bits (`0x5481c`).
- The epoch is 4 bits.
- The origin uses `0x4e758` (argument 16).
- The axes use `0x1102e8`.
- The linear velocity is clamped to 350 and sent in 10 bits. The angular velocity is clamped to 30 and sent in 8 bits.

Passengers and gunners send no B. A Forge editor uses its own branch (`0x111e44`, sandbox engine only).

## 2. Host: what it does with A and B

**Building the tick update.** `0x38300` is called from `0x387ac`. It runs once for each remote sim player whose
sim-player type is 4 (type name GUESSED). CONFIRMED:
```
383ed test [rdx+0x9ac],r8d            ; received B for this player?
38400 movups xmm0,[rdx+rcx*8+0x9b0] -> update+0x11a4[i]   ; copy, clear bit
38447 call 0x7e568                    ; validate: index valid, epoch < 16, pos/vel finite (exp != 0xff), axes ok (0x57780)
38452 call 0x52594 ; mov [rbx+4],eax  ; object -> entity
38464 bts [rdi+0x11a0],r14d           ; prediction mask
384e1 call 0x675a8 / 384f7 call 0x35100c   ; A -> unit control: yaw/pitch -> facing vectors via cos/sin (0x38507..), throttle, buttons
```

**Applying it.** `0x366fc` is called from `0x35f7c` and `0xef120`:

- For each unit in `update+0x8e8`, it calls `0x350ed4(unit, control)`. That writes the unit control fields
  (`+0x1f4..0x1fc`, `+0x20d`, `+0x210`, `+0x214`, `+0x264`, ...). **The client's inputs drive the host's own unit
  simulation.** CONFIRMED.
- For each bit in `update+0x11a0`, it calls **`0x5410c(player, &B)`**.

**`0x5410c`, the host acceptance gate.** CONFIRMED:
```
54134 call 0x52030                    ; entity -> object, else drop
5417c call 0x111e44                   ; Forge editor (sandbox): accept
541a8 cmp [rdi+0x178],r14d            ; unit's owning player == sender ...
541b1 test ([rdi+0x110]>>2)&1         ; ... and unit +0x110 bit 2 clear (GUESSED: alive / not dying)
541bf mov eax,[rdi+0x2a0] ...         ; or vehicle whose driver's +0x178 == sender
54202 call 0x379cfc(obj,0x3c) ; byte [obj+0x40+*(i16*)(obj+0x166)] != 0   ; extra unit gate (GUESSED meaning)
54256 cmp [unit+0x20c],al(B+8)        ; EPOCH must match (else drop, unless Forge-held)
54283 cmp dword [rdi+0x10],-1         ; not attached
54296 call 0x57a24(obj, 2, &B.pos)    ; -> mode
542ca call 0x57ea0(obj, 2, mode, pos, fwd, up, vel, angvel)
```
The sender's identity comes from the channel the data arrived on: `0x38300` reads it from that machine's view. A client
can only claim positions for units its own players own or drive.

### `0x57a24(obj, source, pos)`: choosing a mode

The source is 0 for a normal replicated object, 1 for the client's own predicted object, and 2 for a client
prediction arriving at the host. CONFIRMED values:

| Threshold | Biped | Vehicle | Where it comes from |
|---|---|---|---|
| small `s` | 0.2 | 0.4 | `0x846bc8` / `0x846c94`. When `0x58804(obj)` is true (a biped special state, unit `+0x20e`): 0.8 or 1.5 (`0x846e00` / `0x846fa8`) |
| large `T`, source 1 (client, own unit) | **3.0** | **9.0** | tunables `0xa45c9c` / `0xa45ca4`, set at `0x241ec` |
| large `T`, source 2 (host, client prediction) | **2.5** | **7.5** | `0xa45ca0` / `0xa45ca8` (`movaps` at `0x241f4` from `0x848b30` = {2.5, 9.0, 7.5, 5.0}) |
| ×2.5 on `T` | if unit `+0x408` was stamped less than 1 s ago (`57c06..57c35`; what the stamp marks is GUESSED) | | |
| Forge-held object (`0x111d90`) | host: `T` = 1000, `s` = 0.5. Client: `T` = 1.0, `s` = 0.5 | | `57b90..57bb8`, `57bc9..57be2` |

```
57c79 comiss d2, T*T ; seta r15b      ; beyond large threshold
57c87 comiss d2, s*s ; seta r14b      ; beyond small threshold
; source 2:  r15 -> mode 0 ; else r14 -> mode 1 ; else mode 3
; source 1:  r15 -> mode 2 (biped) / 1 (vehicle) ; else 0x13fbb4(local,host) ? same : mode 0
; source 0:  r14 -> mode 1 ; else mode 3
57cc2..57e55  host && biped && mode 1/2: capsule overlap test at pos (0x1413e8 shape type 7, 0x1457b8 on havok
              component +0x9c) -> mode 3 if blocked
57e5d..57e77  object Forge-held by a player on this machine -> mode 0
```

`0x13fbb4` is a teleporter-pair test. The local position must be within 1.0 of one node and the host position within
1.0 of another node with the same channel byte; the nodes come from the `tls+0x20 +0x13ea4` list. A real teleporter
jump therefore snaps immediately.

### `0x57ea0`: applying the mode

All CONFIRMED:

| Mode | Effect |
|---|---|
| 0 | Nothing. The host (or client) keeps its own state |
| 1 | `0x58890(obj, pos, fwd, up, vel, angvel)`: hard warp |
| 2 | `0x58890(obj, pos, NULL, NULL, vel, angvel)`: warp the position and keep the orientation (facing and aim are untouched) |
| 3 | Keep the position. `0x58b50` adds a correction to the velocity: linear `0.9*Δpos` clamped to 0.05, angular `5*Δangle` clamped to 1.0. The incoming velocity is adopted only if it differs by more than `|Δv|² 0.06` (`0x846ad0`) or `|Δω|² 0.01` (`0x8469dc`). Then `0x3411a4` sets the velocities |

`0x58890` works as follows:

- If `|Δ| <= 1.0`, the object is a biped, it is not attached, and it is **not** controlled by a local player, it sets
  up render-only smoothing: `0x20cd8` / `0x20ecc`, with datum `+0xd2` bit 0.
- Otherwise it calls `0x20b98`.
- It then always calls `object_set_position` `0x340f38` and `0x3411a4`.
- For unit types it also posts `0x382d88(obj, {1, 6})`. GUESSED: message 6 is "biped_handle_simulation_warp"
  (handler `0x3d0c00`, table entry `0x8b4000`).

On a client, every update that carries a position also records `obj+0xd4` (the time) and `obj+0xd8` (that position)
at `58237..5824e`.

## 3. Client: receiving the host's state for its own unit

The per-type update appliers (`0x877a4`, `0x8be94`, `0x8ca48`, `0x8d65c`, `0x8e330`, `0x92928`) call
`0x55b38(obj, mask, state)`. The state layout is: position `+0x00` (mask 2), forward `+0x0c` / up `+0x18` (mask 4),
velocity `+0x28` (0x10) and angular velocity `+0x34` (0x20). CONFIRMED.
```
55bd3 call 0x1e364(obj,0)    ; 1 = gg+0x11==4 && owner player (unit+0x178, or vehicle driver +0x2a0) is local (0xed1cc)
55be3 call 0x57a24(obj, src, pos) ; 55c47 call 0x57ea0(...)
```

For your own body (source 1, biped):

- `d <= 3.0`: ignored. The client keeps its own prediction.
- `d > 3.0`: hard snap of the position and velocity (mode 2). There is no smoothing, because local control and
  `d > 1.0` both disable it.

The same rule applies in a vehicle with 9.0. A one-second ×2.5 window follows the `+0x408` event. CONFIRMED.

**The epoch.** `0xe6f10(player)` copies `player+0x44` into `unit+0x20c`, then marks the unit dirty with
`0x1ec3c(unit, 0x80000000)`, then increments `player+0x44` modulo 16. The client receives it at `0x87c91` and echoes it
in B. The host drops any B whose epoch differs from the unit's (`54256`).

Callers that bump the epoch:

- halo3's teleporter `0x1408d0`, at `0x140a81`, right after `0x340f38` / `0x3411a4` / `0xe396c`
- `0xe73cc`
- `0x118164`

**`object_teleport` `0x259410` does not bump it.** Its callees are `0x3448e0`, `0x340cb4`, `0x1ec3c`, `0xe6880`,
`0xe396c` and `0x340f38`. CONFIRMED.

**A fallback on the client.** `0x20974(obj)` is used on a client for replicated objects and is called from the
per-player path `0xe97b4` / `0x3620f0`. If the last host position is at least 5.0 s old (`0xa45cac`) **and** the
object is more than 0.25 (`0xa45cb0`) from it, the function sends an event to the host (`0x5339c`, type 0x21) and
re-stamps `+0xd4`. CONFIRMED. GUESSED: the event is an "object refresh" request, and it runs only in that
special-case path.

## 4. Why the client-side teleport shows for about 1-2 s and then snaps back

1. The client runs `0x259410` on its own unit. `0x1ec3c` does nothing on a client (type 4), so nothing is sent as
   state. CONFIRMED (`teleport.md`).
2. The next B packets carry the new origin with an unchanged, matching epoch. On the host `d` is far above 2.5, so
   the mode is 0 and the prediction is dropped every time. The host body stays put and keeps receiving the client's
   A (throttle and facing). CONFIRMED.
3. The client predicts locally from the new spot: gravity applies, so the body falls. Host updates within 3.0 would
   be ignored anyway. CONFIRMED.
4. The snap happens when the client receives a host update for that unit **with mask bit 2 (position)**, where
   `d > 3.0` gives mode 2. The host sets bit 2 only when its own copy moves (`0x340d7c` → `0x1ec3c(obj, 2)`, for
   example from host-side biped movement or a mode-1 acceptance). The entity is then sent by the replication
   scheduler. The scheduler's update period is interpolated by relevance between per-type minimum and maximum
   milliseconds (`0x6358c`; tables at `0xa455d8 + type*0x4c`, filled at run time by `0x23ed8`). The mechanism is
   CONFIRMED.
   **GUESSED:** the 1-2 s is that wait. While the host body is idle it has nothing dirty to send. Once anything moves
   it on the host (input, settling, damage), or the period lets it go out, the next position update snaps the client
   back.

## 5. Consequences for the loader

- Any teleport must run on the host (`teleport.md` §5). A client-side move is not authoritative. Even a short one,
  under 2.5 units, would only be *adopted* because the host trusts small prediction errors; it would still not
  replicate as a teleport.
- **After a host teleport, call `0xe6f10(player_index)`, as halo3's own teleporter does.** Without the bump, a short
  host move (under 2.5 units for a biped) can be undone: the client's next prediction is still near the old spot,
  so `d < 2.5`, the host accepts it (mode 1) and moves the body back. A host move shorter than 3.0 units is also
  *ignored* by the client (source-1 mode 0). Keep the target at least about 3 units from the current origin (at
  least 9 in a vehicle), or accept that the client keeps its own position. GUESSED interaction, derived from the
  thresholds above.
- "Teleport next to player B" normally moves the body far more than 3 units, so the client snaps on the first host
  position update. The `0x382d88` warp message and the absence of smoothing make the snap instant.

## Confidence

**High** for these points:

- The control channel layout and what A and B contain.
- The client sends origin, orientation and velocities.
- The host acceptance gate, the thresholds and the meaning of each mode.
- The client's ignore-within-3-units / snap-beyond rule.
- `object_teleport` does not bump the epoch.

**Medium** for these points:

- Field meanings inside A (which bit is which button).
- What unit `+0x408` stamps, and what `0x379cfc` gates.
- The identity of the 5-second "refresh" event.

**Low / not traced:**

- The exact update period that produces the observed 1-2 s.
- Whether any other path (anti-cheat, or Reclaimer's own Rust side) limits client movement speed. No such limit
  was searched for in Reclaimer.

## Test 2026-10-17: small steps ARE accepted (our own local dedicated server)

Setup: `DED_ENV=PELOAD_STATS_HTTP=8767 bash shim/dedicated.sh` (Valhalla Slayer, anti-cheat at its default), one client
joined with `PELOAD_STATS_HTTP=8765 PELOAD_ALLOW_TELEPORT=1 PELOAD_TELEPORT_CLIENT_TEST=1`; each step = POST
`/control?do=tpby&dx=..` to the CLIENT (a client-side `object_teleport` of its own unit, run on its simulation thread),
positions read from both stats servers (client 8765, server 8767).

| client steps | client after | server after | 3 s later | verdict |
|---|---|---|---|---|
| 10 x +2.0 x, 0.5 s apart | x 81.6 -> 101.3 | 101.3 | both 101.3 | **accepted** (+19.7 units) |
| 5 x -3.0 x, 0.5 s apart | 89.3 | 101.3 | client snapped back to 101.3 | rejected (> 2.5) |
| 20 x -2.4 x, 0.1 s apart | 71.1 | 95.1 | client snapped back to 95.1 | partly: once the client ran > 2.5 ahead of the server, the rest was dropped |
| 3 x +2.0 z, 0.3 s apart | z 0.8 -> 4.9 | 4.9 | -- | **accepted** (into the air) |

So the host adopts any client-predicted position within 2.5 units of its own copy, with no speed or vertical check:
a modified client can move horizontally or vertically at roughly 2.5 units per accepted update, limited only by the host's
update rate (0.5 s steps all landed; 0.1 s steps outran it) and by collision (a capsule test; steps into a wall stop).
This is stock Halo 3 client-predicted movement; whether Reclaimer's own anti-cheat watches for it was not tested beyond
this (anti-cheat at the server's default, no kick or warning in 4 runs). Worth reporting to the Reclaimer developers.
