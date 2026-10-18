# Teleporting a player safely (halo3.dll + Reclaimer client v0.9.18)

Static RE, 2026-10-17 (capstone, `re-tools/hdis.py`, `refs.py`, `hsfuncs.py`; sections 1-5 read nothing from a live process;
the live results are in the Status paragraph below).
Binaries: `$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`
(base 0x180000000, addresses `halo3+RVA`) and `client/project-reclaimer-client-v0.9.18.exe` (base 0x140000000, `exe+RVA`).
**CONFIRMED** = read from code (evidence given). **GUESSED** = name or meaning inferred.
TLS: `tls = gs:[0x58][*(u32*)(halo3+0xa39f9c)]`, objects `tls+0x38`, players `tls+0x10`, game_globals `gg = *(tls+0x48)`.

**Status (implemented 2026-10-17, as in section 5):** `hb_frame_hook` is an entry hook on `main_loop_body`
`halo3+0xb2250` (peload.c hbdel_install; the voice-mute jobs run there too); `hb_engine_jobs` (hitbox.c) takes the
game-state lock, runs on the host only (`gg+0x11` 5 or 1; `PELOAD_TELEPORT_CLIENT_TEST=1` = the client-side
experiment), exits the vehicle first (up to 3 frames) and checks the destination with `0x9bc78` at +0.3 / +1.2 and
|coord| < 30000. Needs `PELOAD_ALLOW_TELEPORT=1`. Stats page controls (POST `/control?do=...`; the F7 key is retired,
`PELOAD_HB_FKEYS=1` brings it back): `tpnext&slot=N` ("Teleport next to them"), `teleport` (old F7: the point
reflection through the map centre), host-only `tpto&x=&y=&z=[&slot=]` and `explore&spot=` (map-boundaries.md),
`tpby&dx=&dy=&dz=` (host, or a client with `PELOAD_TELEPORT_CLIENT_TEST`). A client forwards `tpnext` to the host's
loader (`PELOAD_TELEPORT_HOST=<host stats addr>`; the host moves the requester, both views agree within 0.5 s). Tested live: a client-side teleport shows for < 1 s (listen host)
/ ~1.5 s (our dedicated server) and snaps back; the host never moves (net-authority.md).

## Summary

1. halo3's teleport is `halo3+0x259410 (u32 object, const float pos[3], const float fwd[3], const float up[3], bool move)`.
   The shim's argument order is right. `move = false` only turns the object (the position is not passed on). CONFIRMED.
2. Reclaimer calls this function itself in four places. It always calls it on the engine main thread, from its own
   once-per-frame host callback, which runs at the top of halo3's main loop before that frame's ticks. Every call
   holds halo3's game-state lock (`halo3+0x12c948` / `+0x12c9a0`), the lock halo3 holds around every tick. The calls
   run in the host's process only: a client connected to a remote host sends a request (`'MCE1'`, subtype 2) and the
   host moves the object. CONFIRMED.
3. On the host (simulation type 5) the teleport marks the object's network entity dirty: flag 2 = position,
   4 = orientation, 0x10000 = unit facing. The change is then replicated to every client. On a distributed client
   (type 4) that marking does nothing, so the host never learns about a client-side move. CONFIRMED. The client's own
   unit is locally predicted, and an update from the host that differs by more than 0.2-1.5 world units snaps it back.
   CONFIRMED that those threshold comparisons exist; their exact meaning is GUESSED.
4. **Why the shim crashed** (before 2026-10-17). `hb_frame_hook` hooked `halo3+0x9d22c`. That is not a top-of-loop function. It is
   `system_milliseconds` (a QueryPerformanceCounter wrapper) with **421 call sites in 236 functions**, many of them
   inside the tick (simulation, network, AI and object code). The job runs at the first such call on the main thread
   after F7. That can be in the middle of a tick, for example during the physics step or an object-update walk. It also
   ran without the game-state lock, and on a networked *client*. Each of these is a structural reason for the crash.
   CONFIRMED that they exist; which one caused this crash is unproven.
5. **Fix (done 2026-10-17):** do it the way Reclaimer does. Run on the main thread at the entry of `main_loop_body` (`halo3+0xb2250`),
   inside `halo3+0x12c948` / `0x12c9a0`, **on the host process only** (dedicated worker or listen host, `gg+0x11` is 5,
   or 1 for local). Validate the target point with `scenario_location_from_point` (`halo3+0x9bc78`), and either get the
   unit out of its vehicle first or refuse.

## 1. How the Reclaimer client teleports

| What | Where | Evidence / details |
|---|---|---|
| Forge monitor TELEPORT, local or host | `exe+0x9ae130 (halo3_base, tls, object, pos*)` | Checks that `*(halo3+0x8f4028)` (the "Native resource context") is non-null and that `*(i32*)(tls+0x18) <= 0x1f`. Then `tok = halo3+0x12c948()`, then `halo3+0x345318(obj, &fwd, &up)` (keeps the current orientation; defaults are (1,0,0) and (0,0,1)), then **`halo3+0x259410(obj, pos, &fwd, &up, 1)`**, then logs "Forge monitor moved to x, y, z", then `halo3+0x12c9a0(&tok)`. CONFIRMED |
| Request from the UI | `exe+0x57bf6d..0x57c086` (inside `exe+0x567bd0`) | The pending flag and position sit in globals `exe+0x27de6a0..0x27de6b0` (byte lock, u32 pending, 3 floats). If this process has no remote-host connection (`exe+0x68c790` state is None), it calls `exe+0x9ae130` directly. CONFIRMED |
| Client to host | `exe+0x57c0b8..0x57c1e3` | With a remote host it **does not teleport locally**. It builds a 21-byte payload `[u8 2][u64 id][f32 x][f32 y][f32 z]` and sends it with `exe+0x66b1f0` and tag `0x3145434d` ('MCE1'). On failure it shows "The host could not be asked to move your monitor". CONFIRMED |
| Host receive | `exe+0x9ada10` (called from the message dispatcher `exe+0x68cb00`) | Accepts type 2 with length >= 0x15. x, y and z must be finite with \|c\| <= 5000. The sender id is checked, or a permission check runs (`exe+0x781710`). The request `{u64 id, pos}` is pushed onto a Vec at `exe+0x27d5540` (lock byte `exe+0x27d5538`). CONFIRMED |
| Host drain | `exe+0x57dcd0..0x57de36` (inside `exe+0x567bd0`) | Swaps the queue out. `exe+0x9ad750(tls)` builds a map from id to monitor-biped handle. `exe+0x9aca80` checks the handle: `halo3+0x34674c(h, 1)` (biped) and biped definition float `+0x3d0 > 0`. Then `exe+0x9ae130`. CONFIRMED |
| When `exe+0x567bd0` runs | Reclaimer host-interface vtable slot `+0xb8` = `exe+0x5cbaa0` (installed at `exe+0x5ca170`). Event 4 calls `exe+0x567bd0` | The prologue returns unless `GetCurrentThreadId() == *(exe+0x27dac68)`, the engine thread recorded at event 1 (`exe+0x5cbd7f`). halo3 fires event 4 from `halo3+0xadfc` (shell state 7, in game), via `halo3+0xb0d0`, `halo3+0x12e15c`, called at **`main_loop_body` `halo3+0xb2250` +0x21**, which is the first thing each main-loop iteration does, **before** the tick loop `halo3+0xb1778` (called at `+0x38`). CONFIRMED |
| Campaign scripts (`src/engine_campaign_play`) | `exe+0x838d50` | Checks halo3's `hs_function_table` (`halo3+0x7f6ad0`; index 0x30 `volume_test_object`, 0x7f `object_teleport`, 0xfe `unit_exit_vehicle`, 0x104 `vehicle_load_magic`, ...). It then swaps evaluate pointers with `exe+0x5a4df0(slot, expected, new)`: `object_teleport` `halo3+0x7ed6a0` (0x1e3ba0 to `exe+0xa72160`), `volume_test_object` `halo3+0x7eef00` (0x1e24f4 to `exe+0xa75df0`), `unit_exit_vehicle` `halo3+0x7eb000` (0x1e5c00 to `exe+0xa729b0`), `vehicle_load_magic` `halo3+0x7eae70` (0x1e5d94 to `exe+0xa73670`), and `halo3+0x7ea7f0` (0x1e65c8 to `exe+0xa73450`, name not resolved). CONFIRMED |
| Co-op `object_teleport` | `exe+0xa72160`, `exe+0xa72330` | Runs the original `halo3+0x259618(object, flag)`. Then it places every other co-op player's unit k at `flag_pos - (0.35*k)*fwd_xy` with the same z and `up = halo3+0x9fcb8(fwd)`, and calls `halo3+0x259410(unit, pos, fwd, up, 1)`. It does not exit the vehicle first (`move=1` detaches). This runs inside script evaluation, during a tick, which is the engine's own context for it. CONFIRMED |
| Campaign follower | `exe+0xa7ce30`, log "Campaign follower .. joined .. where vehicle .. let its leader out" | Keeps a **trail of the leader's origins** (a ring of 128 points, a new point every >= 0.5 units). It walks back `4 + 0.8*k` units along the trail, faces the leader (horizontal and normalised; (1,0,0) if closer than 0.001), and requires the follower's parent `+0x10 == -1`. Then `halo3+0x259410(follower, pos, fwd, up, 1)`. The trail guarantees the point is open floor, because the leader stood there. CONFIRMED |
| Forge / custom-mode command executor (host) | `exe+0x606ad0` | Plain moves use `halo3+0x340f38(obj, pos, NULL, NULL, NULL, 0)`. Re-parenting uses detach `halo3+0x3448e0`, then `halo3+0x1ec3c(obj, 0x800)`. A position+orientation move is followed by `halo3+0x1ec3c(obj, 0x800)`. CONFIRMED |
| Game-script `Teleport` action | `src/custom_modes/logic.rs`, check in `exe+0x1674af0` ("Teleport requires a player") | Only the validator was located. The runtime handler was not traced. GUESSED: it runs on the host like the command executor |

## 2. halo3's teleport chain

| Address | Function | Details (all CONFIRMED from disassembly unless marked) |
|---|---|---|
| `0x1e3ba0` | `object_teleport` evaluate (object, cutscene_flag) | Calls `0x259618(obj, flag)`. The flag comes from scenario `*(halo3+0xa46a08)` `+0x42c` block, 0x1c-byte entries: +4 position, +0x10 yaw, +0x14 pitch. `fwd = (cos y cos p, sin y cos p, sin p)`, `up = 0x9fcb8(fwd)` |
| `0x1e3bcc` | `object_teleport_to_ai_point` | `0x572940(point)` returns pos, fwd and up; then `0x259410(..., 1)` |
| **`0x259410`** | teleport `(obj, pos, fwd, up, move)` | 1. If `move`: when parent `+0x10 != -1`, `0x3448e0` detaches (the type handler `+0xd0` from the type table `halo3+0x815160[type +0x96]` list `+0x148`, then `0x344980` object_detach). Otherwise, if hidden (`+4` bit 7), `0x3474c0(obj, 0)` un-hides. 2. `0x340cb4` resets physics (below). 3. If `+4` bits 7 and 27 are set: `0x3474c0(obj, 0)`, then clear bit 27. 4. Units (`0x34674c(obj, 0x1003)`): `actor = 0xe3948(obj)`. Facing (made relative to the parent's node when seated) is written to unit **+0x194, +0x1a0, +0x1c4** (desired facing, aiming and looking; names GUESSED), then `0x1ec3c(obj, 0x10000)`. AI units (actor != -1) get `0xe6880(actor, -1, 0, pos)` and the position is then not passed below, and `0xe396c(actor, fwd)`. 5. `0x340f38(obj, move ? pos : NULL, fwd, up, NULL, 0)` |
| `0x340cb4` | physics reset (name GUESSED) | `0x3411a4(obj, &zero, &zero)` (object_set_velocities `0x341110` plus havok `0x14522c`), `0x344b7c(obj, 0)` (set_at_rest false: havok body wake `0x160be8`, clears `+0xa8` bit 9, object_wake, net flag 0x4000 for items), `0x143430`, the type handler `+0x130`, `0x14338c` |
| `0x340f38` | `object_set_position(obj, pos, fwd, up, location, at_rest)` | Calls type pre-handlers `+0x100` (`0x37abb8`), then `0x340d7c`. **If that fails** (only when there is no parent and \|coord\| > 32768): unless `0x1e314` is true (distributed client and the object is replicated), it calls **`object_delete` `0x342ed8`**, so the host deletes the unit. Then `0x340c58` object_wake (sets object-table entry flag 0x02 on the object and its parents) |
| `0x340d7c` | set_position_internal | If connected to the map (`+4` bit 8), `0x34339c` disconnects it. Bounds check ±32768. Writes **`+0x50` origin** and `0x1ec3c(obj, 2)`. Writes **`+0x5c` fwd and `+0x68` up** and `0x1ec3c(obj, 4)`. `0x145160` moves the havok bodies: component `+0x9c` indexes the data array `*(halo3+0x46b7710)` (0xC0-byte elements), then `0x1643fc`. `0x348e44` recomputes node matrices and recurses into children (`+0xc` first child, `+0x8` next sibling, so held weapons follow). `0x3430d4` reconnects to the map and recomputes the cluster. If `+4` bit 9 is set, `0x39d07c` |
| `0x1ec3c` | `simulation_action_object_update(obj, flags)` (name GUESSED) | Acts only if `*(u8*)(gg+0x11) == 5` and `0xf01a0` is false. The entity comes from `obj+0xcc` through `tls+0x240` (`+0x48`, `[i*8+4]`), then `0x522e8(entity, idx, flags)` |
| `0x9bc78` | `scenario_location_from_point(u16* out, const float* p)` | Read-only. `*out = 0xFFFF` when the point is in no cluster of the active BSPs (mask `halo3+0x46b70a8`). `0x34b520` uses it with "0xFF in the first byte = outside" |
| `0x3a3c4c` | `unit_exit_vehicle(unit, type)` | What the `unit_exit_vehicle` script calls: eval `0x1e5c00` passes type 0, `0x1e5c2c` passes its second argument |
| `0x345318` / `0x3451b4` | object_get_orientation(obj, fwd*, up*) / object_get_origin(obj, out*) | Used by Reclaimer before teleporting |
| `0x12c948` / `0x12c9a0` | game-state lock acquire / release(&tok) | ctx is `*(halo3+0x8f4028)`; the lock is at `ctx+0x2b900`, with a per-thread recursion table `ctx+0x2b9a8[*(i32*)(tls+0x18)]` (32 slots). Acquire returns 1 if it took the lock and 0 if this thread already holds it; release unlocks only when the token is 1. The tick loop `0xb1778` wraps each tick `0xb1930` in it (`0xb1875..0xb188b`) |
| `0x9d22c` | `system_milliseconds` (QueryPerformanceCounter based) | 421 call sites. **Not a safe point** |

Simulation type, `u8 gg+0x11`: 4 means distributed client and 5 means distributed server (host). The values are
CONFIRMED by the code above (`0x1ec3c` acts only on 5; `0x1e314` and `0x1e364` only on 4; `0x1e3b4` on 4 and 5). The
names, and 1 = local, are GUESSED from the Halo engine's `e_game_simulation_type`.

## 3. Network authority

- **The host is authoritative.** A teleport on the host sets entity update flags 2 and 4 (and 0x10000 for units),
  so the new position goes out to every client in the regular object updates. CONFIRMED.
- **Receivers.** The per-type apply functions (`halo3+0x877a4`, `0x8be94`, `0x8ca48`, `0x8d65c`, `0x8e330`, `0x92928`)
  call `0x55b38(obj, mask, state)`. There, mask bit 2 means a position at `state+0` and bit 4 an orientation at
  `+0xc`. It goes through `0x57a24` and `0x57ea0`, then `0x58890`. Small changes, under `*(f32*)halo3+0x846ec0`, are
  blended (`0x20cd8` / `0x20ecc`, datum `+0xd2` bit 0). Big jumps, and locally predicted objects, are set with
  `0x340f38` plus `0x3411a4`. CONFIRMED.
- **The client's own unit.** `0x1e364` uses `0x1e3b4` (owning player: unit `+0x178`, or `+0x2a0` / `+0x2a4` through
  the parent) and then `0xed1cc` (player `+0x14` / `+0x18` against the local machine id). The result means "predicted
  on this machine". `0x57a24` compares the host position with the local origin, using thresholds 0.2 / 0.4 (0.8 / 1.5
  in some states) and the tunables at `halo3+0xa45c9c..0xa45ca8`. A teleport-sized error exceeds all of them, so the
  client takes the host's position. CONFIRMED that the comparisons exist; the mode meanings (ignore, blend, snap) are
  GUESSED. Reclaimer depends on this when its host moves a remote client's monitor, and that works.
- **A client-side teleport is never authoritative.** `0x1ec3c` does nothing on type 4, so the host keeps the old
  position and the next update pulls the unit back. In the meantime the client's physics, prediction and the host's
  view disagree. Reclaimer never teleports locally while connected to a remote host (section 1). Whether halo3
  clients also send a predicted position to the host was not traced; even if they do, the host's simulation decides.
  **Conclusion: teleports must run in the host or server process.**

## 4. Where a player's position lives (what a teleport must keep consistent)

| State | Updated by `0x259410`? | Notes |
|---|---|---|
| Object `+0x50` origin, `+0x5c` / `+0x68` forward / up | yes (`0x340d7c`) | |
| Object `+0x1c` sphere centre (`+0x28` radius) | recomputed (GUESSED: node and bounds update) | |
| Object `+0x74` / `+0x80` velocity and angular velocity | zeroed (`0x340cb4`) | |
| Node matrices (`+0x136` block) and children | yes (`0x348e44`, recursive) | |
| Havok bodies (component `+0x9c`) | yes (`0x145160`, `0x344b7c`, `0x14338c`) | |
| BSP location / cluster | disconnect `0x34339c`, reconnect `0x3430d4` | No validity check: a point in solid geometry is accepted |
| Parent / seat (`+0x10`, unit `+0x24E`) | detached when `move=1` (`0x3448e0`) | |
| Hidden / connected flags (`+4` bits 7, 8, 27), at rest (`+0xa8` bit 9), awake (entry flag 0x02) | yes | |
| Unit facing / aim / look `+0x194`, `+0x1a0`, `+0x1c4` | yes, set to `fwd` | `fwd` must be a unit vector |
| AI actor | `0xe6880` / `0xe396c` (AI units only) | Players have no actor |
| Network entity | flags 2, 4, 0x10000 (`0x1ec3c`) | Host only |
| Player record `+0x4C` position | no | Observed equal to the body position. GUESSED: refreshed from the unit every tick |
| Spawn / respawn state, camera / director | not touched | Not traced. GUESSED: the camera follows the unit |

## 5. Recipe for the loader

**Where:** the host or server process. That is the dedicated worker under `dedicated.sh`, or a client that hosts the
game. Check `*(u8*)(gg+0x11)`: 5 (or 1 for local) means go ahead. **4 (client) means do not call it.** Forward the
request to the host instead. There is no generic channel for that: Reclaimer's 'MCE1' type 2 only moves the sender's
own *Forge monitor*. The loader's own channel: the client's stats server forwards the request to the host's
(`PELOAD_TELEPORT_HOST`, POST `tpnext&slot=<target>&who=<requester>`); the worker loads halo3, so it can serve the page.

**When and on which thread:** the main game thread, between ticks. Use an entry hook on `main_loop_body` `halo3+0xb2250`:

- Prologue (20 bytes, nothing rip-relative): `48 8B C4 57 48 81 EC 60 02 00 00 48 C7 44 24 30 FE FF FF FF`
- Call: `entry_hook(base+0xb2250, pro, 20, fn)`

Both the teleport and the voice jobs run there now (they used to run off `0x9d22c`). Inside the hook, `hb_engine_jobs`
checks that `tls` has the object table.

**Steps:**

1. Check that `*(u64*)(halo3+0x8f4028) != 0` and `*(i32*)(tls+0x18) < 32`, then `tok = ((int(*)(void))(h3+0x12c948))()` (ms_abi).
2. Re-check that the unit handle is still valid: salt matches, type 0, `+0x110` bit 2 clear (alive). The player
   record `+0x28` gives the unit.
3. **Vehicle:** if `+0x10 != -1` or `+0x24E != -1`, call `h3+0x3a3c4c(unit, 0)` (unit_exit_vehicle) and retry the
   teleport on a later frame, or refuse the request. (`move=1` would detach on its own and Reclaimer's co-op path
   relies on that. Exiting first is the conservative choice.)
4. **Target point** "next to player B": start from B's origin `+0x50` (feet), B's forward `+0x5c` flattened and
   normalised, and right = (fy, -fx).
   - Candidates: `pos = B.origin + 1.0*right + (0, 0, 0.1)`, then left, then behind (`-1.0*fwd`), then `-0.35*fwd`
     (Reclaimer's co-op spacing).
   - Accept the first candidate where `h3+0x9bc78(&loc, p) != 0xFFFF` for `p = pos + (0, 0, 0.3)` **and**
     `pos + (0, 0, 1.2)`, and `|coord| < 30000`. The last check matters: past ±32768 the host **deletes** the unit.
   - Best fallback is Reclaimer's own: keep a trail of B's origins (one point per 0.5 units moved; the stats thread
     already samples players every 100 ms) and use a point 2-4 units back along it. B stood there, so it is open floor.
     Not implemented: the loader tries the four candidates and refuses when none is open.
   - Not covered: objects such as crates or vehicles. The BSP check only rejects solid structure.
5. `fwd` = horizontal unit vector from `pos` towards B, `up = (0, 0, 1)`.
6. `((void(ms_abi*)(u32, const float*, const float*, const float*, u8))(h3+0x259410))(unit, pos, fwd, up, 1)`, then
   `((void(ms_abi*)(int*))(h3+0x12c9a0))(&tok)`.
7. Never call it from another thread, from inside a tick, or on a client. Do not call it twice for the same unit in
   one frame.

Expected result: the host moves the unit and the client snaps to the host's position on the next update. Reclaimer's
remote-monitor teleport works this way.

## Confidence

High for the chain, its arguments, the Reclaimer call sites, threading, lock, host-only design and the host-delete
pitfall. Medium for the client-side prediction behaviour (the thresholds are read; the modes are inferred) and for the
root cause of the shim crash (mid-tick execution on a client is structurally possible and the most likely cause, but
the actual crash frame was not seen). Not traced: whether clients send a predicted position, the runtime handler of
the custom-mode `Teleport` action, and camera/director side effects.
