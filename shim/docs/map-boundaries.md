# Map boundaries (Halo 3 MCC): what keeps players in, and how to switch it off

Static RE only, 2026-10-17. Sources: the .map files, using the Assembly Halo3MCC plugins for layouts, and
`halo3.dll` (base 0x180000000; addresses are `halo3+RVA`), read with capstone through `re-tools/hdis.py`.
Nothing was read from a running process.

- **CONFIRMED** = read from the code, with the evidence given.
- **GUESSED** = the name or meaning is inferred.
- TLS block `tls = gs:[0x58][*(u32*)(halo3+0xa39f9c)]`. Objects are at `tls+0x38` and players at `tls+0x10`, as in teleport.md.
- The scenario tag pointer is `*(halo3+0xa46a08)`. The globals (matg) pointer is `*(halo3+0xa46968)`.
- Tag addresses are 32-bit and expand as `*(halo3+0x1fcf4c8) + v*4`.

## 1. What a Halo 3 map uses to keep players in

| Mechanism | Where it lives | Effect on a player | Valhalla (riverworld) |
|---|---|---|---|
| **Kill trigger volume** | scnr +0x258 trigger volumes, listed in scnr +0x484 "Scenario Kill Triggers" (s16 each; a volume's +0x40 holds its index in that list) | Instant death: the matg falling-damage "Distance Damage" effect is applied (section 2a) | `kill_happy_place`: x -22.0..123.7, y -52.0..42.9, z -10.4..-4.8. This is the lake/river below the waterfall, north of the base |
| **Safe zone** | Trigger volumes listed in scnr +0x490 "Scenario Safe Zone Triggers" | **Being outside every safe zone kills you**, through the same code path as a kill volume | `safe_zone`: x 38.1..106.3, y -167.8..-31.9, z -1.9..26.2. **This box is Valhalla's real boundary**: it wraps the valley up to z 26 |
| **Soft ceiling, type 1 "soft kill"** | Scenario-wide: scnr +0x234 (flags, name, type). Geometry: structure_design (sddt, scnr BSP entry +0x10) +0x10, triangles | Touching it for 3 s kills you (`unit_kill`). The timer restarts if contact lapses for more than 6 s (2b) | `th`: 2645 triangles draped over all the out-of-bounds terrain, x -38..206, y -285..90, z -9..93. It ignores vehicles (flag 2), but a player in a vehicle is still tested through the vehicle |
| **Soft ceiling, type 0 "acceleration"** | Same | Pushes you back by changing the velocity of the biped, or of its vehicle | `loki`: 506 triangles, a lid at about z 13 over the valley (x 39.5..110.7, y -165..-38, z -4.2..20.5) |
| **Soft ceiling, type 2 "slip surface"** | Same | Slippery: sets biped +0x547 bit 1, so you can't stand there | `slidey`: 2117 triangles on cliff tops and boulders |
| **Invisible collision ("player clip")** | Collision BSP surface flag 2, both in the structure BSP and in instanced-geometry definitions | Solid for bipeds and vehicles; bullets, line-of-sight and other vector tests pass through it (2c) | 1058 surfaces (206 of them near-vertical walls): 484 in the structure BSP (flags 3 and 67) plus `base_int_thing00..29` and `waterfall_collision`. The walls run along the west base side and the east cliff |
| Structure BSP world bounds | sbsp +0x40 (x, y, z ranges) | Nothing directly. Outside the BSP clusters an object is only disconnected from the map (2d) | x -40.9..206.5, y -285.4..92.5, z -15.5..1892.3 |
| Engine coordinate limit | `object_set_position` | A teleport or spawn past \|c\| > 32768 fails, and the host deletes the object (2d) | Far away (the map spans about ±300) |

Trigger volume layout (0x44 bytes):

| Offset | Field |
|---|---|
| +0 | name sid |
| +0xC | forward |
| +0x18 | up |
| +0x24 | position, which is a **corner** of the box |
| +0x30 | extents |
| +0x3C | half the diagonal (checked) |
| +0x40 | s16 kill-trigger index |

The box is `pos + a*fwd + b*left + c*up` with a, b, c in [0, ext], where `left = up x fwd`.

### Tool: `re-tools/h3bounds.py`

```bash
python3 re-tools/h3bsp.py   "$MAPS/riverworld.map" out/riverworld.png                 # base picture + grid json
python3 re-tools/h3bounds.py "$MAPS/riverworld.map" out/rw_bounds.png --grid out/riverworld.json \
        [--composite out/rw_comp.png] [--no-triangles]
# without --grid it runs h3bsp.py itself first (writes <out-stem>_base.png/.json) and uses that grid
```

- **out.png** is a transparent RGBA overlay on exactly the h3bsp pixel grid (`px = (x-x0)*ppu`, `py = (y1-y)*ppu`), so the stats page can stack it on the map picture. Colours:

  | Colour | Shows |
  |---|---|
  | Red fill / edges | Soft kill |
  | Orange | Acceleration |
  | Yellow | Slip |
  | Cyan | Invisible collision: a fill for sloped/flat surfaces, 2 px lines for walls |
  | Red box + hatch | Kill volumes |
  | Green dashed | Safe zones |
  | Grey | Other trigger volumes |
  | White | BSP world bounds |

  A legend is included.
- **out.json** has these keys:
  - `grid`, `engine_limit`, `world_bounds`
  - `kill_volumes` and `safe_zones`: aabb, center, z range
  - `trigger_volumes`: all of them, with axes, extents, footprint polygon and 8 corners
  - `soft_ceilings`: type, flags, ignores, bbox, triangles
  - `invisible_surfaces`: world-space polygons with source, flags, material and a vertical flag
- It takes about 0.5 s per map.
- Checked by eye on Valhalla: the safe zone wraps the valley, the soft-kill drape covers everything around it, the acceleration lid sits over the valley, and the invisible walls line the base side and the east cliff.

Survey of all 27 MP maps:

| Finding | Maps |
|---|---|
| **No kill volume** | chillout, fortress, ghosttown, isolation, midship, s3d_turf, shrine (Sandtrap), warehouse (Foundry) |
| Exactly one safe zone | Every map except warehouse and zanzibar, which have two |
| Soft kill ignores vehicles (flag 0x2) | Every map with a soft kill; most are named `th` |
| Kill volumes above ground | Sandbox `kill` (z 35..37), Chill `kill_zone1/2` (z 42..56) |
| `playerkill`-named volumes | None anywhere |

More map details:
- **Guardian**:
  - `kill_drop` is under the whole map (z -0.9..13.6).
  - Acceleration ceiling `noouts`.
  - 792 invisible surfaces.
- **Sandtrap**:
  - Acceleration `behemoth` ignores everything except huge vehicles (the Elephant fence).
  - Soft kill `th`.
- **Construct**:
  - `kill_bottom`, plus tilted 1-unit boxes `kill_strut0/1`.

## 2. halo3.dll: where each one is enforced, and how to switch it off

All of this runs in **`biped_update` = `halo3+0x37b6c8`**. That is the slot at the biped object-type definition
`halo3+0x8b2650` +0x88, so it is a pointer at `halo3+0x8b26d8`; the type table is `halo3+0x815160`. CONFIRMED.
`creature_update` `halo3+0x3a4fb0` is the same slot of the creature type `0x8b0fd0`.

These run on the simulation (main) thread, inside the per-object update of each game tick, under the game-state lock.
The kill decisions are made by the **host only**:
- Kill volumes: the damage branch returns when `*(u8*)(gg+0x11) == 4` (distributed client).
- Soft kill: it only sets the unit's kill request, which is host-authoritative.

So the kill/safe-zone patches are needed **in the host process** (listen host or dedicated worker). For the
acceleration and slip soft ceilings, clients also simulate them on their own predicted biped. Patch the joining
clients too, or a remote player gets pulled back by its own prediction until the host's position wins. That last
point is GUESSED from how prediction works; see teleport.md section 3.

### 2a. Kill volumes and safe zones: CONFIRMED

| Address | What | Evidence |
|---|---|---|
| `0x1b8e00 (u32 object, u8* out_in_safe_zone)` | Test one object | One **kill** volume per call, round-robin: `k = (u16(object) + *(u32*)(*(tls+0xc8)+0xc)) % kill_count`, where `+0xc` is the game tick (GUESSED name). Skipped if bit k of the disabled bitvector `*(tls+0x418)` is set; otherwise `0x16f644(tv, object)`. Then, if `out != NULL`: `*out = 1`. On ticks where `(u16(object) + tick) % safe_count == 0`, `*out = 0` and every safe zone is tested; inside any one gives `*out = 1`. Returns 1 if inside the kill volume |
| `0x16f644 (s16 trigger_volume, u32 object)` | trigger_volume_test_object | Tests the object's bounding-sphere centre (datum +0x1c) with `0x16f55c`. The HS functions `volume_test_object(s)` use it too |
| `0x37b80a..0x37b838` (biped_update) | Caller | It skips the whole test when the object is at rest (datum +0xa8 bit 9). Otherwise it calls `0x1b8e00(unit, &safe)` and goes to the **kill path when `in_kill || safe == 0`** |
| `0x37b83e..0x37b95f` | Kill path | Sets `biped +0x460 \|= 0x10`; returns on clients (gg+0x11 == 4). Then `0x36eba0(&dmg, matg->falling_damage[0] +0x5C "Distance Damage")`, `dmg.flags \|= 4`, `0x36fe00(&dmg, unit, -1, -1, -1, 0)` (object_cause_damage, GUESSED name) |
| `0x3a50c3..0x3a50d0` (creature_update) | Creatures | `0x1b8e00(creature, NULL)`; when inside it sets `creature +0x110 \|= 0x40` (the unit_kill request) |
| `0x1e2390` / `0x1e22fc` | HS `kill_volume_disable` / `kill_volume_enable` | Set or clear bit `tv[+0x40]` in `*(tls+0x418)`. The bitvector is 0x80 bytes (1024 bits); `0x1b8d9c` zeroes it at map load |

Vehicles are not tested themselves (no caller passes a vehicle). A seated player still is, through biped_update.
The test uses the biped's position, so a driver in a kill box dies. GUESSED: that this applies while seated (no
early-out seen except "at rest").

**Patches** (PELOAD_PATCH is re-applied when halo3.dll is mapped, peload.c:9230). Check the original bytes first.

| Goal | Patch | Original bytes | Notes |
|---|---|---|---|
| **No kill-volume or safe-zone deaths, for any object** (recommended) | `halo3+1b8e00:31C04885D27403C60201C3` | `48 8B C4 48 89 58 08 48 89 68 10` | The entry becomes `xor eax,eax; test rdx,rdx; je ret; mov byte [rdx],1; ret`, meaning "in no kill volume, inside a safe zone". Only biped_update and creature_update call it. HS `volume_test_*` scripts are unaffected |
| Same, bipeds only | `halo3+37b816:E95501000090` | `0F 85 54 01 00 00` | `jne 0x37b970` (the at-rest skip) becomes `jmp`, so bipeds never run the test, exactly like an at-rest biped |
| Kill volumes only (keep safe zones) | `halo3+37b82f:9090` | `75 0D` | Drops `jne kill` after the kill-volume result |
| Safe zones only (keep kill volumes) | `halo3+37b838:E92701000090` | `0F 85 26 01 00 00` | Always skip when not in a kill volume |
| Data-only alternative (kill volumes only) | Write 0xFF to the 0x80 bytes at `*(tls+0x418)` on the sim thread | | This is what `kill_volume_disable` does for every volume. Redo it after every map load. It does not cover safe zones |

### 2b. Soft ceilings: soft kill, acceleration, slip. CONFIRMED (the type to flag-bit mapping is GUESSED)

| Address | What |
|---|---|
| `0x37b72a..0x37b76b` (biped_update) | Only if `unit+0x178` (owning player) != -1 and `!0x36d520(...)`: `0x1bac00(unit, &player[+0x4c])`. The result goes to the frame byte `[rbp+0xc0]` ("pushed back") |
| `0x1bac00 (unit, pos*)` | Player soft-ceiling response. It works on the unit's ultimate parent (the vehicle when seated). Skipped if `unit+0x110` bit 2 is set. It queries `0x1bb1d0`, then: |
| `0x1baf0f` | Acceleration: if there is a hit, `0x1ba2e8` gives the penetration and the reflected velocity, `0x3411a4` sets it (object_set_velocities), and `parent+0xa8` bit 19 is set |
| `0x1bb0e0..0x1bb15f` | **Soft kill** (result flag 2). `unit+0x404` = contact start tick. If -1 or more than 6 s ago (`0x847254` = 6.0), start = now. Else, once at least 3 s (`0x847180` = 3.0) have passed, call **`unit_kill` `0x34d520`** (sets `+0x110 \|= 0x40`; the same function as HS `unit_kill`) |
| `0x1bb166..0x1bb185` | Slip (result flag 4): when biped `+0x4de == 1`, set `+0x547 \|= 2` |
| `0x1bb1d0` | Query: MOPP query (`0x449590`) over the active BSP's structure_design soft-ceiling triangles. Enabled ceilings come from `0x1bb844(name)`. Ignore flags: biped bit 0, vehicle bit 1, huge-vehicle bit 3. Soft kill applies to object types in mask 0x1003 (biped, vehicle, giant) |
| `0x1bb784` / `0x1bb844` | `soft_ceiling_enable` (HS eval `0x1e2ac4`) / is-enabled. Bit i of `*(tls+0x518)` = scnr soft ceiling i enabled |
| `soft_ceilings_disable` | The HS global exists, but its storage pointer is NULL in this release build (entry `halo3+0x8a7770`), so it does nothing |

Other users of the soft-ceiling query, not traced:
- `0x1b9474`, called from `0x213718` / `0x213904` (GUESSED: havok vehicles / huge vehicles).
- `0x1b9af4`, called from `0x1fe800` (GUESSED: camera).
- `0x1b9d6c`, called from `0x117a24`.

The player path above covers walking players and players driving vehicles.

| Goal | Patch | Original bytes |
|---|---|---|
| **No soft ceilings at all for players** (acceleration lid, soft kill, slip) | `halo3+37b734:EB` | `74` (`je 0x37b777` becomes `jmp`; this is the engine's own non-player path) |
| Soft kill only (keep the acceleration lid and slip) | `halo3+1bb0ee:EB` | `74` (`je 0x1bb161` becomes `jmp`; flag-2 handling skipped) |
| Acceleration push-back only | `halo3+1baf0f:E9AB01000090` | `0F 8E AA 01 00 00` |
| Data-only alternative | Clear the bits in `*(tls+0x518)` (one per scnr soft ceiling) after map load, which is what `soft_ceiling_enable <name> false` does | |

**Explore-everything recipe (host):**

```
PELOAD_PATCH="halo3+1b8e00:31C04885D27403C60201C3,halo3+37b734:EB"
```

To also cover joining clients' prediction, run their clients with the soft-ceiling patch (`halo3+37b734:EB`).
Still to check live: whether the Reclaimer client or its anti-cheat hashes halo3 code. The XAudio2 check
(CLAUDE.md) only verifies 4 callback slots; a server with anti-cheat on refuses some clients, and the reason was not traced.

### 2c. Invisible collision (player clip)

**CONFIRMED.** The surface "invisible" bit (flag 2) is read in only two places in halo3.dll. These are the leaf
surface checks of `collision_bsp_test_vector` `0x2559ac`:
- small BSP: `0x256424`; the check is at `0x2566b7`: `if (surf.flags & 2) && (test_flags & 8) skip`;
  `& 8 / & 0x10` skips breakable surfaces.
- large BSP: `0x2567e0`, flags at surface +0xE, check at `0x256a54`.

So bit 3 of the collision-test flags dword = "ignore invisible surfaces" and bit 4 = "ignore breakable surfaces"
(CONFIRMED there). This is how bullets, line of sight and other line tests pass through player clip.

The scan covered `test byte [..+0xa/0xe], 2` and byte loads followed by `test reg, 2` across the whole .text. **No
other code looks at the bit.** The sphere/feature tests and the Havok side (bipeds use Havok shape phantoms, from the
strings `character_physics` and `hkSimpleShapePhantom`; GUESSED) never filter it. That is why bipeds and vehicles
collide with player clip.

**Consequence: there is no one-branch patch that lets bipeds through player clip.** Clearing or setting the flag
changes nothing for movement. Options, all NOT done or verified:
1. **Filter on the Havok side.** Find the BSP shape-collection callback that turns a MOPP shape key into a surface
   triangle, and make it return "no shape" for invisible surfaces when the querying body is a biped (or always:
   player clip is only meant for players and vehicles). It was not located. Edge-walk scans of the
   physics region (`0x14xxxx..0x16xxxx`) found only the BSP3D point search `0x16a4c4`.
   Next step: find the hkpMoppBvTreeShape setup from the sbsp "MOPP Codes" block (sbsp +0x298) or the clusters'
   collision MOPP codes (cluster +0xC8), and its child-shape vtable.
2. **Data:** serve a modified collision resource from the .map, as the shim already sees the map open (hb_note_map).
   The sbsp resource is one raw-deflate page (Valhalla page 1420). Invisible surfaces share vertices and edges with
   real geometry, so they would have to be cut out of the BSP and MOPP, not just moved. This is heavy, and it gives
   collision that differs from what clients have.
3. **Live with it:** on Valhalla the player-clip walls are only along the base side and the east cliff, and the
   map's actual boundary is the safe zone plus the soft kill. Section 2a and 2b remove those.

### 2d. World limits an explorer meets next

| What | Where | Status |
|---|---|---|
| Position limit \|x\|,\|y\|,\|z\| <= 32768 | `0x340d7c`, compares `0x340dfc..0x340e36` against `0x8477c4` = -32768.0 (and +32768) | CONFIRMED. Only for explicit set_position (teleport/spawn), where a failure means `object_delete` on the host (teleport.md). Normal movement does not go through it |
| Outside every BSP cluster | `0x34b520` (object location from the centre, then the origin) reports "outside" (`location[0] == 0xFF`); `0x3430d4` then leaves the object disconnected from the map | CONFIRMED that there is no kill on this path. Not traced: what a disconnected biped loses (rendering/PVS, lighting, maybe AI/sound) |
| Falling damage | matg +0x458 falling damage, element 0: "Maximum Falling Distance" +0x58 and "Distance Damage" +0x5C, the same effect the kill volume applies | GUESSED: long falls off cliffs still kill. Not traced |
| Havok world extent | | Not traced. GUESSED: bodies leaving the Havok broadphase get removed; Valhalla's geometry box (about 250 x 380) is far inside any sane broadphase |
| Map edges | The BSP itself: the terrain ends in skirts (world bounds above) | Data |

## 3. Confidence

- **Map data, high.** The layouts come from the Assembly plugins and are checked against the data:
  - The trigger-volume corner convention is checked by the half-diagonal field and by the safe zone wrapping the play area.
  - The soft-ceiling names and types match between the scnr and the sddt.
  - The invisible counts match `h3bsp --invisible`.
- **Kill volumes and safe zones, high.** Every branch was read, and the patch sites are plain jcc/entry overwrites with the bytes checked against the file. Not run live.
- **Soft ceilings, high for the control flow; medium for the result-flag meanings** (2 = soft kill, 4 = slip).
- **Player clip, high for the negative result** (only the vector tests filter the bit). The Havok filter location is unknown.


## Tested live 2026-10-17 (Valhalla, our own hosted game)

Host = `shim/target.sh` (it hosts a hidden Valhalla game), moved with the new host-only teleport-to-a-point control
`POST /control?do=tpto&x=..&y=..&z=..[&slot=N]` (PELOAD_ALLOW_TELEPORT=1; the point must be inside the map structure,
|coord| < 30000; slot = a players-table slot, default = the host's own player). Ground heights off the map's collision
BSP: `re-tools/h3bsp.py` `top_z_at` (the east cliff tops, x 110-122 / y -60..-140, are z 16-29).

| host patches | teleport to (114, -100, 22.6): east cliff top, outside the safe zone (x > 106.3) |
|---|---|
| none | dead within 1 s (safe-zone kill) |
| `PELOAD_PATCH=halo3+1b8e00:31C04885D27403C60201C3,...` | **refused by the Reclaimer client**: "Native function contract mismatch at 0x1801b8e00" -- it checks that function's entry bytes; the worker exits |
| `PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB` (call-site variants inside biped_update) | **alive** 15+ s, standing on the cliff (slides slowly down its slope) |

So for exploring, host the game with
```
PELOAD_ALLOW_TELEPORT=1 PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB
```
(bipeds skip the kill-volume / safe-zone test and the player soft-ceiling response), then teleport with `tpto`.
Joining clients' own prediction still runs the soft ceilings (push-back / slip) unless they run the `37b734` patch too.
Invisible walls are unchanged (section 3).

### Exploring from the stats page (2026-10-17)

`shim/tools/maptop.sh` (FORCE=1 to redo) now also writes `<map>.z.bin`: the top surface height every world unit
(roofs and cliff tops included, no z cut; format in the map's .json `zgrid`). On the live stats page of a game you
host with `PELOAD_ALLOW_TELEPORT=1`, tick **click to teleport** under the map and click a spot: the page looks up the
ground there and sends `tpto` to 0.6 above it. With the boundary patches above you stay alive outside the play area
(tested: Valhalla east cliff, (110, -80) -> standing at z 19.1).
